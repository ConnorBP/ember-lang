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

// --- Trace-callback registration (external root providers) ---

GcTraceToken GcHeap::register_trace_callback(void* user_data, GcTraceFn fn) {
    if (!fn) return 0;  // 0 == invalid token
    GcTraceToken tok = m_next_trace_token++;
    m_trace_cbs.push_back(TraceCb{tok, user_data, fn});
    return tok;
}

bool GcHeap::unregister_trace_callback(GcTraceToken token) {
    if (token == 0) return false;
    for (size_t i = 0; i < m_trace_cbs.size(); ++i) {
        if (m_trace_cbs[i].token == token) {
            // We do NOT touch user_data here: the caller owns it, so removing
            // the registration must not dereference or retain it. Erasing the
            // vector entry drops our only reference to it.
            m_trace_cbs.erase(m_trace_cbs.begin() + i);
            return true;
        }
    }
    return false;  // invalid / already-removed token
}

// GcTraceVisitor::report: validate `candidate` and, if it is a live GC object
// not yet marked, mark it + push onto the worklist. Safe for null / non-GC /
// freed pointers (is_live() returns false for those, so they are ignored).
void GcTraceVisitor::report(void* candidate) {
    if (!candidate || !heap) return;
    if (!heap->is_live(candidate)) return;  // null/non-GC/freed -> ignore
    heap->mark_and_push(candidate, *worklist);
}

// mark_and_push: mark `obj` (a live GC object's user bytes) if not already
// marked, then push it onto the worklist so its RefMap edges get traced.
// Shared by the explicit-root loop and the trace-visitor so both feed the SAME
// worklist (and thus the SAME trace phase) — externally-reported roots and
// explicit roots are indistinguishable to the tracer.
void GcHeap::mark_and_push(void* obj, std::vector<void*>& worklist) {
    uint32_t hb;
    std::memcpy(&hb, static_cast<char*>(obj) - 4, 4);
    Header* hdr = reinterpret_cast<Header*>(static_cast<char*>(obj) - hb);
    if (hdr->mark) return;
    hdr->mark = 1;
    worklist.push_back(obj);
}

// --- Write barrier + barrier observer hook ---

GcBarrierToken GcHeap::register_barrier_observer(void* user_data, GcBarrierObserver obs) {
    if (!obs) return 0;
    GcBarrierToken tok = m_next_barrier_token++;
    m_barrier_obs.push_back(BarrierObs{tok, user_data, obs});
    return tok;
}

bool GcHeap::unregister_barrier_observer(GcBarrierToken token) {
    if (token == 0) return false;
    for (size_t i = 0; i < m_barrier_obs.size(); ++i) {
        if (m_barrier_obs[i].token == token) {
            // Same lifetime contract as trace callbacks: do NOT touch user_data.
            m_barrier_obs.erase(m_barrier_obs.begin() + i);
            return true;
        }
    }
    return false;
}

void GcHeap::write_barrier(void* owner, void* child) {
    // Safely ignore null / non-live values: a barrier is only meaningful when
    // a live owner writes a live child (a real inter-object edge). For the
    // current non-generational collector this is a behavioural no-op beyond
    // the observability surface; the liveness filter keeps the counter + the
    // observers from firing on noise (null writes, non-GC pointers, writes
    // into dead/freed objects).
    if (!owner || !child) return;
    if (!is_live(owner) || !is_live(child)) return;
    m_stats.barrier_calls++;
    for (const BarrierObs& obs : m_barrier_obs) {
        obs.fn(obs.user_data, owner, child);
    }
}

const GcStats& GcHeap::collect() {
    m_stats.collections++;

    // Mark phase: worklist starting from explicit roots AND from every
    // registered trace callback's reported candidates. Both feed the SAME
    // worklist via mark_and_push, so externally-supplied roots are traced
    // identically to explicit root slots.
    std::vector<void*> worklist;

    // (1) Explicit root slots.
    for (void** root : m_roots) {
        if (!root) continue;
        void* obj = *root;
        if (!obj || !is_live(obj)) continue;
        mark_and_push(obj, worklist);
    }

    // (2) Trace-callback-reported roots. Each callback receives a visitor
    // wired to this heap + worklist; it reports candidate pointers via
    // visitor.report(), which validates (live?) + marks + pushes. Invalid /
    // non-live / null candidates are silently filtered by the visitor.
    if (!m_trace_cbs.empty()) {
        GcTraceVisitor vis;
        vis.heap = this;
        vis.worklist = &worklist;
        for (const TraceCb& cb : m_trace_cbs) {
            cb.fn(cb.user_data, vis);
        }
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
            mark_and_push(child, worklist);
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
    // Drop all trace-callback and barrier-observer registrations WITHOUT
    // touching any caller user_data: the heap held raw pointers into caller
    // storage only for the lifetime of each registration; clearing the vectors
    // drops those references. No dangling callback user data remains. Every
    // previously-issued token is now invalid.
    m_trace_cbs.clear();
    m_barrier_obs.clear();
    m_stats = GcStats{};
    // Leave the token counters as-is (they are monotonic per-heap). Tokens
    // issued before clear() remain invalid because their entries are gone;
    // new registrations continue from the next monotonic value. This keeps
    // tokens globally unique across a heap's lifetime (a recycled token could
    // otherwise refer to a stale registration a caller forgot to drop).
}

} // namespace ember::gc
