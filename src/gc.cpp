// gc.cpp — tracing mark-sweep garbage collector core implementation.
//
// CONCURRENCY: see the top-of-file note in gc.hpp. All mutable GcHeap state is
// guarded by the coarse recursive mutex `m_lock`. Public methods acquire the
// lock and delegate to `*_locked` helpers. collect() and write_barrier()
// SNAPSHOT the callback / observer registries before invoking user code so a
// callback / observer that registers or unregisters ITSELF during invocation
// cannot invalidate the active iteration (the same-thread iterator-invalidation
// bug a bare recursive mutex does not prevent). A re-entrant collect() (a
// callback calling collect() from within its own invocation) is short-circuited
// by the `m_collecting` guard so collection can never self-deadlock or
// self-corrupt. stats() / collect() return GcStats BY VALUE (a locked copy) so
// no caller ever holds a reference to mutable counters another thread can
// change concurrently.
#include "gc.hpp"
#include "safety.hpp"

#include <cstring>
#include <cstdlib>
#include <mutex>
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
    std::lock_guard<std::recursive_mutex> g(m_lock);
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
    std::lock_guard<std::recursive_mutex> g(m_lock);
    m_roots.push_back(addr);
}

void GcHeap::remove_root(void** addr) {
    std::lock_guard<std::recursive_mutex> g(m_lock);
    for (size_t i = m_roots.size(); i > 0; --i) {
        if (m_roots[i - 1] == addr) {
            m_roots.erase(m_roots.begin() + (i - 1));
            return;
        }
    }
}

void GcHeap::clear_roots() {
    std::lock_guard<std::recursive_mutex> g(m_lock);
    m_roots.clear();
}

bool GcHeap::is_live(const void* p) const {
    std::lock_guard<std::recursive_mutex> g(m_lock);
    return is_live_locked(p);
}

bool GcHeap::is_live_locked(const void* p) const {
    return m_live.count(const_cast<void*>(p)) > 0;
}

// Deterministic immediate free (the language `delete` substrate). See the
// contract in gc.hpp. Frees a single live object NOW, bypassing the collector;
// leaves stale root/edge values for the liveness filter to ignore.
bool GcHeap::free_object(void* p) {
    if (!p) return false;
    std::lock_guard<std::recursive_mutex> g(m_lock);
    auto it = m_live.find(p);
    if (it == m_live.end()) return false;  // not an exact live GC user pointer
    // Recover the header (same trailer-at-user-4 recovery as collect()) + free
    // the allocation immediately.
    uint32_t hb;
    std::memcpy(&hb, static_cast<char*>(p) - 4, 4);
    Header* hdr = reinterpret_cast<Header*>(static_cast<char*>(p) - hb);
    m_stats.live_bytes -= hdr->size;
    m_stats.live_objects--;
    m_stats.freed_objects++;
    m_live.erase(it);
    std::free(hdr);
    // DELIBERATELY do NOT clear root slots or RefMap edges that may still hold
    // `p`. Those stale values are safely ignored by the existing liveness
    // filter: the root loop checks is_live(*root) before marking; the trace
    // visitor checks is_live(candidate); RefMap edge tracing checks
    // is_live(child). A freed pointer fails is_live() in all three, so no
    // dangling dereference + no false resurrection. Clearing every possible
    // reference would require a full heap scan (the collector's job); leaving
    // stale values for the filter to ignore is the O(1) deterministic-delete
    // contract.
    return true;
}

GcStats GcHeap::stats() const {
    std::lock_guard<std::recursive_mutex> g(m_lock);
    return m_stats;  // by-value snapshot (locked copy)
}

// --- Trace-callback registration (external root providers) ---

GcTraceToken GcHeap::register_trace_callback(void* user_data, GcTraceFn fn) {
    if (!fn) return 0;  // 0 == invalid token
    std::lock_guard<std::recursive_mutex> g(m_lock);
    GcTraceToken tok = m_next_trace_token++;
    m_trace_cbs.push_back(TraceCb{tok, user_data, fn});
    return tok;
}

bool GcHeap::unregister_trace_callback(GcTraceToken token) {
    if (token == 0) return false;
    std::lock_guard<std::recursive_mutex> g(m_lock);
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
// freed pointers (is_live_locked() returns false for those, so they are
// ignored). report() always runs under collect()'s held lock, so it uses the
// *_locked helpers (no re-acquire).
void GcTraceVisitor::report(void* candidate) {
    if (!candidate || !heap) return;
    if (!heap->is_live_locked(candidate)) return;  // null/non-GC/freed -> ignore
    heap->mark_and_push_locked(candidate, *worklist);
}

// mark_and_push_locked: mark `obj` (a live GC object's user bytes) if not
// already marked, then push it onto the worklist so its RefMap edges get traced.
// Shared by the explicit-root loop and the trace-visitor so both feed the SAME
// worklist (and thus the SAME trace phase) — externally-reported roots and
// explicit roots are indistinguishable to the tracer. Assumes m_lock is held.
void GcHeap::mark_and_push_locked(void* obj, std::vector<void*>& worklist) {
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
    std::lock_guard<std::recursive_mutex> g(m_lock);
    GcBarrierToken tok = m_next_barrier_token++;
    m_barrier_obs.push_back(BarrierObs{tok, user_data, obs});
    return tok;
}

bool GcHeap::unregister_barrier_observer(GcBarrierToken token) {
    if (token == 0) return false;
    std::lock_guard<std::recursive_mutex> g(m_lock);
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
    std::lock_guard<std::recursive_mutex> g(m_lock);
    if (!is_live_locked(owner) || !is_live_locked(child)) return;
    m_stats.barrier_calls++;
    // SNAPSHOT the observer registry before invoking ANY observer, so an
    // observer that registers or unregisters ITSELF (or another entry) during
    // invocation mutates the LIVE m_barrier_obs (under the recursive lock — no
    // self-deadlock) but NOT this local snapshot — the active iteration can
    // never be invalidated. Snapshot semantics (documented in gc.hpp): an
    // observer registered during this call is NOT fired in this same call; an
    // observer that unregisters itself during this call may still be fired in
    // this call (it was already in the snapshot) but is not fired thereafter.
    std::vector<BarrierObs> obs_snap = m_barrier_obs;
    for (const BarrierObs& obs : obs_snap) {
        obs.fn(obs.user_data, owner, child);
    }
}

GcStats GcHeap::collect() {
    std::lock_guard<std::recursive_mutex> g(m_lock);
    // Re-entrancy guard: a trace callback that re-enters collect() from within
    // its own invocation would otherwise run a NESTED mark/sweep on the same
    // heap (corrupting marks / double-freeing). Short-circuit it to a stats
    // snapshot so collection can never self-deadlock or self-corrupt. (The
    // recursive mutex already prevents self-deadlock; this prevents the
    // corrupting nested sweep.)
    if (m_collecting) return m_stats;
    m_collecting = true;
    struct CollectGuard {
        bool& flag;
        ~CollectGuard() { flag = false; }
    } guard{ m_collecting };

    m_stats.collections++;

    // Mark phase: worklist starting from explicit roots AND from every
    // registered trace callback's reported candidates. Both feed the SAME
    // worklist via mark_and_push_locked, so externally-supplied roots are
    // traced identically to explicit root slots.
    std::vector<void*> worklist;

    // (1) Explicit root slots.
    for (void** root : m_roots) {
        if (!root) continue;
        void* obj = *root;
        if (!obj || !is_live_locked(obj)) continue;
        mark_and_push_locked(obj, worklist);
    }

    // (2) Trace-callback-reported roots. SNAPSHOT the registry before invoking
    // ANY callback, so a callback that registers or unregisters ITSELF (or
    // another entry) during invocation mutates the LIVE m_trace_cbs (under the
    // recursive lock — no self-deadlock) but NOT this local snapshot — the
    // active iteration can never be invalidated (the same-thread
    // iterator-invalidation bug a bare recursive mutex does not prevent).
    // Snapshot semantics (documented in gc.hpp): a callback registered during
    // this collect() is NOT invoked in this same cycle (it is invoked on the
    // next); a callback that unregisters itself during this cycle may still be
    // invoked in this cycle (it was already in the snapshot) but not thereafter.
    if (!m_trace_cbs.empty()) {
        std::vector<TraceCb> cb_snap = m_trace_cbs;
        GcTraceVisitor vis;
        vis.heap = this;
        vis.worklist = &worklist;
        for (const TraceCb& cb : cb_snap) {
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
            if (!child || !is_live_locked(child)) continue;
            mark_and_push_locked(child, worklist);
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

    return m_stats;  // by-value snapshot (locked copy)
}

void GcHeap::clear() {
    std::lock_guard<std::recursive_mutex> g(m_lock);
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
