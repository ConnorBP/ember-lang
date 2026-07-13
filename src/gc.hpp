// gc.hpp — tracing mark-sweep garbage collector core.
//
// Foundational infrastructure for lambdas-with-capture (#20) and coroutines
// (#21). Manages a GC heap of objects with known reference layouts (NOT
// classes — OOP is a non-goal). Stop-the-world mark-sweep, single-threaded
// (per-context heaps + mutexes are a follow-up, mirroring how context_t is
// per-thread). No ast.hpp/sema.hpp/codegen.hpp dependency — the `ember` core
// lib must stay frontend-free (one-way link direction per CMakeLists.txt).
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_set>
#include <initializer_list>

namespace ember::gc {

// Which byte-offsets within an object's USER bytes hold GC pointers.
// 8-aligned byte offsets. Empty = leaf object (no outgoing refs).
struct RefMap {
    std::vector<uint32_t> ptr_byte_offsets;
    bool empty() const { return ptr_byte_offsets.empty(); }
};

RefMap refmap_none();
RefMap refmap_words(std::initializer_list<uint32_t> byte_offsets);

struct GcStats {
    size_t live_objects = 0;
    size_t live_bytes   = 0;
    size_t total_allocated = 0;   // cumulative alloc count
    size_t collections  = 0;     // collect() call count
    size_t freed_objects = 0;    // cumulative swept count
};

class GcHeap {
public:
    GcHeap();
    ~GcHeap();

    // Allocate `size` user bytes with reference layout `rm`. Returns pointer to
    // USER bytes (a hidden header precedes it). nullptr on bad_alloc. Object is
    // unmarked; the next collect() frees it unless a root reaches it.
    //
    // SAFETY: every allocation checks process RSS against the global memory
    // limit (safety::check_memory_limit, throttled to every ~64 MiB of live-
    // byte growth). If RSS exceeds the limit, the process instant-fails
    // (abort) — this is the failsafe that stops an unbounded allocation loop
    // from freezing the host machine. nullptr is still possible on malloc
    // failure below the RSS limit.
    void* alloc(size_t size, const RefMap& rm);

    // Precise root registration: `addr` is the address of a slot (void**) that
    // holds a pointer to a GC object's user bytes. The slot must stay alive and
    // its pointed-to pointer must stay valid until remove_root. collect() reads
    // *addr to find the root object, then traces.
    void add_root(void** addr);
    void remove_root(void** addr);
    void clear_roots();

    // Mark from all registered roots, trace each reachable object's RefMap
    // (recursively), then sweep (free) every unmarked object. Clears marks on
    // survivors. Updates + returns stats.
    const GcStats& collect();
    const GcStats& stats() const;

    // True iff `p` points at the user bytes of a currently-live GC object.
    bool is_live(const void* p) const;

    // Free ALL objects + clear roots (test teardown / full reset).
    void clear();

private:
    struct Header;
    std::unordered_set<void*> m_live;  // user-byte pointers
    std::vector<void**> m_roots;
    GcStats m_stats;
    // RSS-check throttle: only call safety::check_memory_limit() when live_bytes
    // has grown by >= 64 MiB since the last check. Keeps the ~1us syscall out of
    // the per-alloc hot path while still catching runaway growth within ~64 MiB.
    size_t m_last_rss_check_bytes = 0;
};

} // namespace ember::gc
