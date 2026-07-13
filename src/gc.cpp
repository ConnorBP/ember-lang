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
        Header* hdr = reinterpret_cast<Header*>(static_cast<char*>(obj) - 16);  // placeholder, fixed below
        // Recover header: the header_bytes field is at user - header_bytes.
        // But we don't know header_bytes yet. Read it from the 8 bytes before
        // the user pointer (header_bytes is the 3rd uint32 in the header, at
        // offset 8 from the header start). The header starts at user - X where
        // X is unknown. BUT: header_bytes is always >= sizeof(Header) = 16, and
        // it's stored at header_start + 8. If we read user - 8, we get the last
        // 2 uint32s of the minimal header (ref_count + pad), NOT header_bytes.
        // SIMPLER: store header_bytes as the FIRST uint32 the user pointer
        // can reliably find. Put it RIGHT BEFORE the user bytes (the last 4
        // bytes of the header region): user - 4 = header_bytes.
        // Actually, let me just put a back-pointer to the header start right
        // before the user bytes.
        // SIMPLEST FIX: read header_bytes from a fixed position. Since
        // sizeof(Header) = 16, and header_bytes is at offset 8, it's at
        // user - header_bytes + 8. But we don't know header_bytes.
        // OK: let me just store the header pointer as a hidden 8 bytes BEFORE
        // the user bytes (wastes 8 bytes per object but is simple + correct).
        // Actually, the cleanest: store header_bytes as a uint32 at
        // (user - 4). That's the LAST 4 bytes before user. So the layout is:
        // [Header (16B)] [ref_offsets] [header_bytes again (4B)] [user bytes]
        // No — that's redundant. Let me just read it from the known position.
        // Since the header is at least 16 bytes, and header_bytes >= 16, I can
        // read user - 4 as a uint32 to get header_bytes (I'll store it there).
        uint32_t hb;
        std::memcpy(&hb, static_cast<char*>(obj) - 4, 4);
        hdr = reinterpret_cast<Header*>(static_cast<char*>(obj) - hb);
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
