// gc.cpp — tracing mark-sweep garbage collector core implementation.
#include "gc.hpp"
#include "safety.hpp"

#include <cstring>
#include <cstdlib>
#include <unordered_set>
#include <initializer_list>

namespace ember::gc {

RefMap refmap_none() { return {}; }
RefMap refmap_words(std::initializer_list<uint32_t> byte_offsets) {
    RefMap r;
    r.ptr_byte_offsets = byte_offsets;
    return r;
}

// Object header (hidden, immediately before the user bytes).
// user_ptr - sizeof(Header) recovers the header.
// The RefMap is stored as a pointer to a separately-allocated array (NOT
// inline) so the header is trivially destructible + std::free is safe.
struct GcHeap::Header {
    uint32_t size;          // user bytes
    uint32_t mark;          // mark bit (0 = unmarked, 1 = marked)
    uint32_t header_bytes;  // total header size (sizeof(Header) + ref_offset bytes) — for recovery
    uint32_t ref_count;     // number of pointer word offsets
    // ref_offset values follow (ref_count * uint32_t), then user bytes.
    const uint32_t* ref_offsets() const {
        return reinterpret_cast<const uint32_t*>(reinterpret_cast<const char*>(this) + sizeof(Header));
    }
};

GcHeap::GcHeap() {}
GcHeap::~GcHeap() { clear(); }

void* GcHeap::alloc(size_t size, const RefMap& rm) {
    // SAFETY FAILSAFE: throttled RSS check. If an unbounded allocation loop is
    // filling the heap faster than collection reclaims it, this aborts the
    // process before host RAM is exhausted (the incident root cause). Throttled
    // to every ~64 MiB of live-byte growth so it stays out of the hot path.
    if (m_stats.live_bytes >= m_last_rss_check_bytes + (64ull * 1024 * 1024)) {
        safety::check_memory_limit();
        m_last_rss_check_bytes = m_stats.live_bytes;
    }
    size_t ref_bytes = rm.ptr_byte_offsets.size() * sizeof(uint32_t);
    size_t hdr_total = sizeof(Header) + ref_bytes + 4;  // +4 for the header_bytes trailer at user-4
    size_t total = hdr_total + size;
    void* raw = std::malloc(total);
    if (!raw) return nullptr;
    Header* hdr = static_cast<Header*>(raw);
    hdr->size = uint32_t(size);
    hdr->mark = 0;
    hdr->header_bytes = uint32_t(hdr_total);
    hdr->ref_count = uint32_t(rm.ptr_byte_offsets.size());
    if (ref_bytes) {
        std::memcpy(const_cast<uint32_t*>(hdr->ref_offsets()),
                    rm.ptr_byte_offsets.data(), ref_bytes);
    }
    void* user = static_cast<char*>(raw) + hdr_total;
    // Store header_bytes as the last 4 bytes before user (for O(1) header recovery).
    std::memcpy(static_cast<char*>(user) - 4, &hdr_total, 4);
    std::memset(user, 0, size);  // deterministic for tests
    m_live.insert(user);
    m_stats.live_objects++;
    m_stats.live_bytes += size;
    m_stats.total_allocated++;
    return user;
}

void GcHeap::add_root(void** addr) {
    m_roots.push_back(addr);
}

void GcHeap::remove_root(void** addr) {
    for (size_t i = m_roots.size(); i > 0; --i) {
        if (m_roots[i - 1] == addr) {
            m_roots.erase(m_roots.begin() + (i - 1));
            return;
        }
    }
}

void GcHeap::clear_roots() {
    m_roots.clear();
}

bool GcHeap::is_live(const void* p) const {
    return m_live.count(const_cast<void*>(p)) > 0;
}

const GcStats& GcHeap::stats() const { return m_stats; }

const GcStats& GcHeap::collect() {
    m_stats.collections++;

    // Mark phase: worklist starting from roots.
    std::vector<void*> worklist;
    for (void** root : m_roots) {
        if (!root) continue;
        void* obj = *root;
        if (!obj || !is_live(obj)) continue;
        // Recover the header: header_bytes is stored as a uint32 at user - 4.
        uint32_t hb;
        std::memcpy(&hb, static_cast<char*>(obj) - 4, 4);
        Header* hdr = reinterpret_cast<Header*>(static_cast<char*>(obj) - hb);
        if (hdr->mark) continue;
        hdr->mark = 1;
        worklist.push_back(obj);
    }
    // Trace: follow each object's pointer word offsets.
    while (!worklist.empty()) {
        void* obj = worklist.back();
        worklist.pop_back();
        uint32_t hb;
        std::memcpy(&hb, static_cast<char*>(obj) - 4, 4);
        Header* hdr = reinterpret_cast<Header*>(static_cast<char*>(obj) - hb);
        for (uint32_t i = 0; i < hdr->ref_count; ++i) {
            uint32_t off = hdr->ref_offsets()[i];
            if (off + 8 > hdr->size) continue;
            void* child;
            std::memcpy(&child, static_cast<char*>(obj) + off, 8);
            if (!child || !is_live(child)) continue;
            uint32_t chb;
            std::memcpy(&chb, static_cast<char*>(child) - 4, 4);
            Header* chdr = reinterpret_cast<Header*>(static_cast<char*>(child) - chb);
            if (chdr->mark) continue;
            chdr->mark = 1;
            worklist.push_back(child);
        }
    }

    // Sweep phase: free unmarked objects, clear marks on survivors.
    std::vector<void*> to_free;
    for (void* obj : m_live) {
        uint32_t hb;
        std::memcpy(&hb, static_cast<char*>(obj) - 4, 4);
        Header* hdr = reinterpret_cast<Header*>(static_cast<char*>(obj) - hb);
        if (hdr->mark) {
            hdr->mark = 0;  // clear for next cycle
        } else {
            to_free.push_back(obj);
        }
    }
    for (void* obj : to_free) {
        uint32_t hb;
        std::memcpy(&hb, static_cast<char*>(obj) - 4, 4);
        Header* hdr = reinterpret_cast<Header*>(static_cast<char*>(obj) - hb);
        m_stats.live_bytes -= hdr->size;
        m_stats.live_objects--;
        m_stats.freed_objects++;
        m_live.erase(obj);
        std::free(hdr);
    }

    return m_stats;
}

void GcHeap::clear() {
    for (void* obj : m_live) {
        uint32_t hb;
        std::memcpy(&hb, static_cast<char*>(obj) - 4, 4);
        Header* hdr = reinterpret_cast<Header*>(static_cast<char*>(obj) - hb);
        std::free(hdr);
    }
    m_live.clear();
    m_roots.clear();
    m_stats = GcStats{};
}

} // namespace ember::gc
