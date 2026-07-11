// gc.cpp — tracing mark-sweep garbage collector core implementation.
#include "gc.hpp"

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
struct GcHeap::Header {
    uint32_t size;   // user bytes
    uint32_t mark;   // mark bit (0 = unmarked, 1 = marked)
    RefMap  refmap;  // pointer word offsets within user bytes
};

GcHeap::GcHeap() {}
GcHeap::~GcHeap() { clear(); }

void* GcHeap::alloc(size_t size, const RefMap& rm) {
    size_t total = sizeof(Header) + size;
    void* raw = std::malloc(total);
    if (!raw) return nullptr;
    Header* hdr = static_cast<Header*>(raw);
    hdr->size = uint32_t(size);
    hdr->mark = 0;
    hdr->refmap = rm;
    void* user = static_cast<char*>(raw) + sizeof(Header);
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
        Header* hdr = reinterpret_cast<Header*>(static_cast<char*>(obj) - sizeof(Header));
        if (hdr->mark) continue;
        hdr->mark = 1;
        worklist.push_back(obj);
    }
    // Trace: follow each object's RefMap pointer words.
    while (!worklist.empty()) {
        void* obj = worklist.back();
        worklist.pop_back();
        Header* hdr = reinterpret_cast<Header*>(static_cast<char*>(obj) - sizeof(Header));
        for (uint32_t off : hdr->refmap.ptr_byte_offsets) {
            if (off + 8 > hdr->size) continue;
            void* child;
            std::memcpy(&child, static_cast<char*>(obj) + off, 8);
            if (!child || !is_live(child)) continue;
            Header* chdr = reinterpret_cast<Header*>(static_cast<char*>(child) - sizeof(Header));
            if (chdr->mark) continue;
            chdr->mark = 1;
            worklist.push_back(child);
        }
    }

    // Sweep phase: free unmarked objects, clear marks on survivors.
    std::vector<void*> to_free;
    for (void* obj : m_live) {
        Header* hdr = reinterpret_cast<Header*>(static_cast<char*>(obj) - sizeof(Header));
        if (hdr->mark) {
            hdr->mark = 0;  // clear for next cycle
        } else {
            to_free.push_back(obj);
        }
    }
    for (void* obj : to_free) {
        Header* hdr = reinterpret_cast<Header*>(static_cast<char*>(obj) - sizeof(Header));
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
        Header* hdr = reinterpret_cast<Header*>(static_cast<char*>(obj) - sizeof(Header));
        std::free(hdr);
    }
    m_live.clear();
    m_roots.clear();
    m_stats = GcStats{};
}

} // namespace ember::gc
