// gc_test.cpp — tracing mark-sweep GC core unit tests.
// Exercises the GcHeap directly: alloc+collect, root scanning, reachable
// survives, unreachable collected, ref-graph tracing, cycles, stats, clear.
#include "../src/gc.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <vector>
#include <algorithm>

using namespace ember::gc;

static int g_fail = 0;
static void ck(bool c, const char* msg) {
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", msg);
    if (!c) g_fail = 1;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== gc_test: tracing mark-sweep GC core ===\n");

    // [1] alloc + collect (empty heap)
    {
        GcHeap h;
        GcStats s0 = h.stats();
        h.collect();
        ck(s0.live_objects == 0, "[1a] empty heap collect: live_objects==0");
        ck(h.stats().collections == 1, "[1b] collections==1 after first collect");
        void* obj = h.alloc(8, refmap_none());
        ck(obj != nullptr, "[1c] alloc returns non-null");
        ck(h.stats().live_objects == 1, "[1d] live_objects==1 after alloc");
        ck(h.stats().total_allocated == 1, "[1e] total_allocated==1");
        h.collect();  // no roots -> freed
        ck(!h.is_live(obj), "[1f] unrooted object collected");
        ck(h.stats().live_objects == 0, "[1g] live_objects==0 after collect");
        ck(h.stats().freed_objects == 1, "[1h] freed_objects==1");
    }

    // [2] root scanning (explicit root set)
    {
        GcHeap h;
        void* obj = h.alloc(8, refmap_none());
        void* p = obj;
        void** root = &p;
        h.add_root(root);
        h.collect();
        ck(h.is_live(obj), "[2a] rooted object survives collect");
        ck(h.stats().live_objects == 1, "[2b] live_objects==1 with root");
        h.remove_root(root);
        h.collect();
        ck(!h.is_live(obj), "[2c] object collected after root removed");
        ck(h.stats().freed_objects == 1, "[2d] freed_objects==1 after root removed");
    }

    // [3] reachable survives across multiple collections
    {
        GcHeap h;
        void* obj = h.alloc(8, refmap_none());
        void* p = obj;
        h.add_root(&p);
        for (int i = 0; i < 3; ++i) h.collect();
        ck(h.is_live(obj), "[3a] rooted object survives 3 collections");
        ck(h.stats().live_objects == 1, "[3b] live_objects==1 after 3 collects");
        ck(h.stats().freed_objects == 0, "[3c] freed_objects==0 (marks cleared each cycle)");
    }

    // [4] unreachable collected
    {
        GcHeap h;
        void* obj1 = h.alloc(8, refmap_none());
        void* obj2 = h.alloc(8, refmap_none());
        void* p1 = obj1;
        h.add_root(&p1);
        h.collect();
        ck(h.is_live(obj1), "[4a] rooted obj1 survives");
        ck(!h.is_live(obj2), "[4b] unrooted obj2 collected");
        ck(h.stats().live_objects == 1, "[4c] live_objects==1");
        ck(h.stats().freed_objects == 1, "[4d] freed_objects==1");
        void* obj3 = h.alloc(8, refmap_none());
        (void)obj3;  // unrooted: allocated only to be collected on the next sweep
        h.collect();
        ck(h.stats().live_objects == 1, "[4e] still 1 after second collect");
        ck(h.stats().freed_objects == 2, "[4f] freed_objects==2");
    }

    // [5] reference graph tracing (A->B, A->C, B->C, D isolated)
    {
        GcHeap h;
        void* A = h.alloc(24, refmap_words({0, 8}));
        void* B = h.alloc(16, refmap_words({0}));
        void* C = h.alloc(8, refmap_none());
        void* D = h.alloc(8, refmap_none());
        std::memcpy(A, &B, 8);       // A[0] = &B
        std::memcpy(static_cast<char*>(A) + 8, &C, 8);  // A[8] = &C
        std::memcpy(B, &C, 8);       // B[0] = &C
        (void)D;  // D is isolated
        void* rootA = A;
        h.add_root(&rootA);
        h.collect();
        ck(h.is_live(A), "[5a] A survives (rooted)");
        ck(h.is_live(B), "[5b] B survives (reachable via A)");
        ck(h.is_live(C), "[5c] C survives (reachable via A and B, diamond)");
        ck(!h.is_live(D), "[5d] D collected (isolated)");
        ck(h.stats().live_objects == 3, "[5e] live_objects==3");
        ck(h.stats().freed_objects >= 1, "[5f] freed_objects>=1 (D)");
    }

    // [6] cycle is collected when root removed
    {
        GcHeap h;
        void* X = h.alloc(16, refmap_words({0}));
        void* Y = h.alloc(16, refmap_words({0}));
        std::memcpy(X, &Y, 8);  // X[0] = &Y
        std::memcpy(Y, &X, 8);  // Y[0] = &X (cycle)
        void* rootX = X;
        h.add_root(&rootX);
        h.collect();
        ck(h.is_live(X) && h.is_live(Y), "[6a] cycle survives when rooted");
        ck(h.stats().live_objects == 2, "[6b] live_objects==2 (cycle)");
        h.remove_root(&rootX);
        h.collect();
        ck(!h.is_live(X) && !h.is_live(Y), "[6c] cycle collected when root removed");
        ck(h.stats().live_objects == 0, "[6d] live_objects==0 after cycle collected");
    }

    // [7] stats sanity
    {
        GcHeap h;
        h.alloc(8, refmap_none());   // obj A (unrooted)
        void* b = h.alloc(16, refmap_none());  // obj B (will root)
        h.alloc(8, refmap_none());   // obj C (unrooted)
        void* rb = b;
        h.add_root(&rb);
        h.collect();
        ck(h.stats().total_allocated == 3, "[7a] total_allocated==3");
        ck(h.stats().collections == 1, "[7b] collections==1");
        ck(h.stats().live_objects == 1, "[7c] live_objects==1");
        ck(h.stats().freed_objects == 2, "[7d] freed_objects==2");
        ck(h.stats().live_bytes == 16, "[7e] live_bytes==16 (survivor B)");
    }

    // [8] clear() resets
    {
        GcHeap h;
        h.alloc(8, refmap_none());
        void* p = h.alloc(8, refmap_none());
        h.add_root(&p);
        h.collect();
        h.clear();
        ck(h.stats().live_objects == 0, "[8a] clear: live_objects==0");
        h.collect();  // should not crash
        ck(h.stats().freed_objects == 0, "[8b] clear: no freed after reset");
    }

    // ====================================================================
    // [9]-[13]: trace-callback + write-barrier foundational collector APIs.
    // These exercise the per-GcHeap trace-callback registration mechanism
    // (external root providers) + the write barrier. See gc.hpp for the API.
    // ====================================================================

    // [9] externally supplied roots: a trace callback reports an object as a
    //     root; the object survives collect even though no explicit root slot
    //     was registered. This is the core extension-owned-roots contract.
    {
        GcHeap h;
        void* obj = h.alloc(8, refmap_none());
        // user_data holds the pointer the callback will report.
        void* reported = obj;
        GcTraceToken tok = h.register_trace_callback(&reported,
            [](void* ud, GcTraceVisitor& v) {
                void* p = *static_cast<void**>(ud);
                v.report(p);
            });
        ck(tok != 0, "[9a] register_trace_callback returns non-zero token");
        h.collect();
        ck(h.is_live(obj), "[9b] callback-reported object survives collect");
        ck(h.stats().live_objects == 1, "[9c] live_objects==1 (callback root)");
        // Unregister -> object no longer reported -> collected next cycle.
        ck(h.unregister_trace_callback(tok), "[9d] unregister returns true");
        h.collect();
        ck(!h.is_live(obj), "[9e] object collected after callback removed");
        ck(h.stats().freed_objects == 1, "[9f] freed_objects==1 after removal");
        // unregister again -> false (already removed / invalid token).
        ck(!h.unregister_trace_callback(tok), "[9g] re-unregister returns false");
    }

    // [10] callback-reported child pointers: a callback reports an object A
    //      whose RefMap points to a child B. collect() traces from the
    //      callback-reported A through the RefMap edge to B (the same mark
    //      worklist used by explicit roots), so B survives too. An isolated
    //      object C (not reported, not reachable) is collected.
    {
        GcHeap h;
        void* A = h.alloc(16, refmap_words({0}));   // A[0] -> B
        void* B = h.alloc(8, refmap_none());
        void* C = h.alloc(8, refmap_none());         // isolated
        std::memcpy(A, &B, 8);                       // A[0] = &B
        void* reported = A;
        h.register_trace_callback(&reported,
            [](void* ud, GcTraceVisitor& v) {
                v.report(*static_cast<void**>(ud));
            });
        h.collect();
        ck(h.is_live(A), "[10a] callback-rooted A survives");
        ck(h.is_live(B), "[10b] B survives via RefMap edge from callback-rooted A");
        ck(!h.is_live(C), "[10c] isolated C collected");
        ck(h.stats().live_objects == 2, "[10d] live_objects==2 (A + B)");
    }

    // [11] callback removal + clear() lifetime: removing a callback must not
    //      leave dangling user data in the heap. After unregister, the heap
    //      holds no reference to the caller's user_data. clear() drops every
    //      registration without retaining caller pointers. We verify by (a)
    //      letting the user_data storage go out of scope after unregister and
    //      running another collect (no dangling access), and (b) clear()
    //      dropping a still-registered callback without touching its data.
    {
        GcHeap h;
        // (a) unregister then let user_data go out of scope before next collect.
        GcTraceToken tok;
        {
            void* obj = h.alloc(8, refmap_none());
            void* reported = obj;
            tok = h.register_trace_callback(&reported,
                [](void* ud, GcTraceVisitor& v) {
                    v.report(*static_cast<void**>(ud));
                });
            h.collect();
            ck(h.is_live(obj), "[11a] object survives while callback registered");
            h.unregister_trace_callback(tok);
            // `reported` goes out of scope here; the heap must not retain it.
        }
        // collect again: the heap must not dereference the now-gone user_data.
        h.collect();
        ck(h.stats().collections == 2, "[11b] second collect ran after user_data gone (no dangling)");
        // (b) clear() drops a still-registered callback without touching user_data.
        void* obj2 = h.alloc(8, refmap_none());
        (void)obj2;
        int touched = 0;
        h.register_trace_callback(&touched,
            [](void* ud, GcTraceVisitor& v) {
                *static_cast<int*>(ud) += 1;  // mark that the callback ran
                (void)v;
            });
        h.clear();  // drops the callback; the next collect must NOT call it.
        int touched_before = touched;
        h.collect();
        ck(touched == touched_before, "[11c] clear() dropped callback: not invoked after clear");
        ck(h.stats().live_objects == 0, "[11d] clear() freed all objects");
        // After clear, the token from (a) is invalid (heap reset). unregister is false.
        ck(!h.unregister_trace_callback(tok), "[11e] token invalid after clear()");
    }

    // [12] invalid / non-live reported pointers: a callback may report null,
    //      a non-GC pointer (e.g. a stack address), and a pointer to an object
    //      that was already freed. collect() must safely ignore all of these
    //      and only keep genuinely-live reported objects. No crash, no false
    //      mark of freed memory.
    {
        GcHeap h;
        void* live = h.alloc(8, refmap_none());
        // Root `live` so it survives the first collect, while `victim` (no
        // root) is freed. We then drop that root and let the callback be the
        // SOLE source keeping `live` alive in the second collect — proving the
        // callback can rescue a live object while ignoring a freed pointer.
        void* rlive = live;
        h.add_root(&rlive);
        void* victim = h.alloc(8, refmap_none());
        h.collect();          // victim unrooted -> freed; live rooted -> survives
        ck(!h.is_live(victim), "[12a] victim freed before callback reports it");
        ck(h.is_live(live), "[12a2] live survived first collect (rooted)");
        h.remove_root(&rlive);  // callback is now the only thing referencing live
        int stack_local = 0;
        void* stack_ptr = &stack_local;   // a non-GC pointer
        // Bundle the candidates the callback will report.
        struct Cand { void* live_p; void* freed_p; void* stack_p; };
        Cand c{ live, victim, stack_ptr };
        h.register_trace_callback(&c,
            [](void* ud, GcTraceVisitor& v) {
                Cand* p = static_cast<Cand*>(ud);
                v.report(nullptr);        // null
                v.report(p->freed_p);     // already freed
                v.report(p->stack_p);     // non-GC pointer
                v.report(p->live_p);      // the one genuinely-live candidate
            });
        h.collect();
        ck(h.is_live(live), "[12b] live reported object survives");
        ck(!h.is_live(victim), "[12c] freed reported pointer not resurrected");
        ck(h.stats().live_objects == 1, "[12d] live_objects==1 (only the live candidate)");
        ck(h.stats().freed_objects == 1, "[12e] freed_objects==1 (victim only)");
    }

    // [13] write-barrier invocation: write_barrier(owner, child) is observable
    //      via a registered observer/hook and via stats().barrier_calls. It
    //      must safely ignore null/non-live owner or child (no observer fire,
    //      no counter bump for those). The current non-generational collector
    //      does not need a remembered set, so the barrier is a no-op on actual
    //      collection behavior — but the observability surface must work.
    {
        GcHeap h;
        ck(h.stats().barrier_calls == 0, "[13a] barrier_calls starts at 0");
        void* owner = h.alloc(16, refmap_words({0}));
        void* child = h.alloc(8, refmap_none());
        // Observer records the (owner, child) pairs it sees.
        struct Obs { void* owner; void* child; int count; };
        Obs rec{ nullptr, nullptr, 0 };
        GcBarrierToken otok = h.register_barrier_observer(&rec,
            [](void* ud, void* o, void* c) {
                Obs* r = static_cast<Obs*>(ud);
                r->owner = o; r->child = c; r->count++;
            });
        ck(otok != 0, "[13b] register_barrier_observer returns non-zero token");
        // Valid barrier: both live -> observer fires, counter bumps.
        h.write_barrier(owner, child);
        ck(rec.count == 1, "[13c] observer fired for valid barrier");
        ck(rec.owner == owner && rec.child == child, "[13d] observer saw (owner, child)");
        ck(h.stats().barrier_calls == 1, "[13e] barrier_calls==1 after valid call");
        // Null child -> safely ignored (no observer fire, no bump).
        h.write_barrier(owner, nullptr);
        ck(rec.count == 1, "[13f] null child: observer not fired");
        ck(h.stats().barrier_calls == 1, "[13g] null child: barrier_calls unchanged");
        // Null owner -> safely ignored.
        h.write_barrier(nullptr, child);
        ck(rec.count == 1, "[13h] null owner: observer not fired");
        ck(h.stats().barrier_calls == 1, "[13i] null owner: barrier_calls unchanged");
        // Non-live child (a stack pointer) -> safely ignored.
        int sl = 0;
        h.write_barrier(owner, &sl);
        ck(rec.count == 1, "[13j] non-live child: observer not fired");
        ck(h.stats().barrier_calls == 1, "[13k] non-live child: barrier_calls unchanged");
        // Unregister observer -> subsequent barriers do not fire it.
        ck(h.unregister_barrier_observer(otok), "[13l] unregister observer returns true");
        h.write_barrier(owner, child);
        ck(rec.count == 1, "[13m] observer not fired after unregister");
        ck(h.stats().barrier_calls == 2, "[13n] barrier_calls still counts after observer removed");
        // collect() is unaffected by the barrier (non-generational: no remembered set).
        void* rootO = owner;
        h.add_root(&rootO);
        std::memcpy(owner, &child, 8);  // owner[0] = child (reachable edge)
        h.collect();
        ck(h.is_live(owner) && h.is_live(child), "[13o] barrier does not change reachability");
    }

    // ====================================================================
    // [14]-[19]: deterministic immediate free (free_object).
    // These exercise GcHeap::free_object — the deterministic-destruction
    // primitive that frees a single live object IMMEDIATELY (bypassing the
    // mark-sweep collector). It is the runtime substrate for the future
    // language `delete` operator, separate from unpinning (the extension's
    // gc_unroot_env / gc_delete native only MARK an object collectable;
    // free_object frees it NOW). See gc.hpp for the contract.
    // ====================================================================

    // [14] free a live object immediately: alloc, free_object, the object is
    //      gone right away (no collect needed), stats reflect the free.
    {
        GcHeap h;
        void* obj = h.alloc(24, refmap_none());
        ck(h.stats().live_objects == 1, "[14a] live_objects==1 after alloc");
        ck(h.stats().live_bytes == 24, "[14b] live_bytes==24 after alloc");
        ck(h.free_object(obj), "[14c] free_object returns true for a live object");
        ck(!h.is_live(obj), "[14d] object not live after free_object");
        ck(h.stats().live_objects == 0, "[14e] live_objects==0 after free_object");
        ck(h.stats().live_bytes == 0, "[14f] live_bytes==0 after free_object");
        ck(h.stats().freed_objects == 1, "[14g] freed_objects==1 after free_object");
        ck(h.stats().total_allocated == 1, "[14h] total_allocated unchanged (still 1)");
        // A subsequent collect must not double-free or crash (the object is
        // already gone from the live set).
        h.collect();
        ck(h.stats().freed_objects == 1, "[14i] collect after free does not re-free");
        ck(h.stats().live_objects == 0, "[14j] still 0 live after post-free collect");
    }

    // [15] free a ROOTED object: free_object frees it immediately even though a
    //      root slot still points at it. The stale root slot is SAFELY IGNORED
    //      by the next collect (the root loop checks is_live() -> false -> skip).
    //      No crash, no resurrection. This is the "stale root slot" case.
    {
        GcHeap h;
        void* obj = h.alloc(16, refmap_none());
        void* p = obj;
        void** root = &p;
        h.add_root(root);           // root slot holds obj
        ck(h.is_live(obj), "[15a] rooted object live before free");
        ck(h.free_object(obj), "[15b] free_object returns true for a rooted object");
        ck(!h.is_live(obj), "[15c] rooted object not live after free_object");
        ck(h.stats().live_objects == 0, "[15d] live_objects==0 after freeing rooted obj");
        // The root slot STILL holds the (now-freed) pointer. collect() must
        // read it, check is_live() -> false, and skip it (no dangling deref,
        // no resurrection). The freed count must not grow (already freed).
        size_t freed_before = h.stats().freed_objects;
        h.collect();
        ck(h.stats().freed_objects == freed_before, "[15e] stale root slot not re-freed by collect");
        ck(h.stats().live_objects == 0, "[15f] no resurrection via stale root slot");
        ck(!h.is_live(obj), "[15g] object stays gone after collect with stale root");
        // remove_root the stale slot for clean teardown.
        h.remove_root(root);
    }

    // [16] null / foreign (non-GC) pointers: free_object must reject them
    //      (return false) without touching the heap or stats.
    {
        GcHeap h;
        void* obj = h.alloc(8, refmap_none());
        ck(!h.free_object(nullptr), "[16a] free_object(nullptr) returns false");
        int stack_local = 0;
        void* stack_ptr = &stack_local;   // a foreign (non-GC) pointer
        ck(!h.free_object(stack_ptr), "[16b] free_object(stack ptr) returns false");
        // A pointer into the MIDDLE of a live object is not an exact user
        // pointer -> rejected (free_object validates EXACT live user pointers).
        void* interior = static_cast<char*>(obj) + 4;
        ck(!h.free_object(interior), "[16c] free_object(interior ptr) returns false");
        // The rejections must not have freed obj or changed stats.
        ck(h.is_live(obj), "[16d] live object unaffected by rejected frees");
        ck(h.stats().live_objects == 1, "[16e] live_objects still 1 after rejected frees");
        ck(h.stats().freed_objects == 0, "[16f] freed_objects still 0 after rejected frees");
    }

    // [17] repeated free: free_object on an already-freed pointer returns false
    //      (idempotent rejection — no double-free).
    {
        GcHeap h;
        void* obj = h.alloc(8, refmap_none());
        ck(h.free_object(obj), "[17a] first free_object returns true");
        ck(!h.free_object(obj), "[17b] second free_object returns false (already freed)");
        ck(!h.free_object(obj), "[17c] third free_object returns false (still freed)");
        ck(h.stats().freed_objects == 1, "[17d] freed_objects==1 (no double-count)");
        ck(h.stats().live_objects == 0, "[17e] live_objects==0");
    }

    // [18] stale object EDGES followed by collection: object A has a RefMap
    //      edge to object B; free_object(B) frees B immediately, leaving A's
    //      edge holding a stale (freed) pointer. A subsequent collect() traces
    //      A's edges, reads the stale B pointer, checks is_live() -> false, and
    //      skips it. A survives (rooted), B stays gone. No crash, no
    //      resurrection. This is the "stale object edge" case.
    {
        GcHeap h;
        void* A = h.alloc(16, refmap_words({0}));   // A[0] -> B
        void* B = h.alloc(8, refmap_none());
        std::memcpy(A, &B, 8);                       // A[0] = &B (the edge)
        void* rootA = A;
        h.add_root(&rootA);
        ck(h.is_live(A) && h.is_live(B), "[18a] A + B live before free");
        // Free B immediately. A's edge still holds &B (now freed).
        ck(h.free_object(B), "[18b] free_object(B) returns true");
        ck(!h.is_live(B), "[18c] B not live after free_object");
        ck(h.is_live(A), "[18d] A still live (not freed)");
        // A's edge is now stale (points at freed B). collect() traces A's
        // RefMap edge, reads the stale pointer, is_live() -> false -> skip.
        size_t freed_before = h.stats().freed_objects;
        h.collect();
        ck(h.stats().freed_objects == freed_before, "[18e] stale edge not re-freed by collect");
        ck(h.is_live(A), "[18f] A survives collect (rooted; stale edge ignored)");
        ck(!h.is_live(B), "[18g] B stays gone (stale edge did not resurrect it)");
        ck(h.stats().live_objects == 1, "[18h] live_objects==1 (only A)");
    }

    // [19] correct GcStats updates across a mix of free_object + collect: alloc
    //      several objects, free some via free_object, collect the rest, and
    //      verify every stat (live_objects, live_bytes, total_allocated,
    //      freed_objects, collections) is exactly right.
    {
        GcHeap h;
        void* a = h.alloc(8, refmap_none());    // 8 bytes
        void* b = h.alloc(16, refmap_none());   // 16 bytes
        void* c = h.alloc(24, refmap_none());   // 24 bytes
        void* rb = b;
        h.add_root(&rb);                         // root b (so it survives collect)
        ck(h.stats().live_objects == 3, "[19a] live_objects==3 after 3 allocs");
        ck(h.stats().live_bytes == 48, "[19b] live_bytes==48 (8+16+24)");
        ck(h.stats().total_allocated == 3, "[19c] total_allocated==3");
        // Free a + c via free_object (deterministic, immediate).
        ck(h.free_object(a), "[19d] free_object(a) true");
        ck(h.free_object(c), "[19e] free_object(c) true");
        ck(h.stats().live_objects == 1, "[19f] live_objects==1 (only b) after free_object x2");
        ck(h.stats().live_bytes == 16, "[19g] live_bytes==16 (only b's 16 bytes)");
        ck(h.stats().freed_objects == 2, "[19h] freed_objects==2 (a + c via free_object)");
        ck(h.stats().total_allocated == 3, "[19i] total_allocated still 3");
        // collect: b is rooted -> survives; nothing else to free.
        size_t freed_before = h.stats().freed_objects;
        GcStats s = h.collect();
        ck(s.collections == 1, "[19j] collections==1 after one collect");
        ck(h.is_live(b), "[19k] rooted b survives collect");
        ck(h.stats().live_objects == 1, "[19l] live_objects==1 (b survived)");
        ck(h.stats().live_bytes == 16, "[19m] live_bytes==16 (b)");
        ck(h.stats().freed_objects == freed_before, "[19n] freed_objects unchanged (nothing swept)");
        ck(h.stats().total_allocated == 3, "[19o] total_allocated still 3");
        // remove the root + collect -> b reaped by the collector (not free_object).
        h.remove_root(&rb);
        h.collect();
        ck(!h.is_live(b), "[19p] b collected after root removed");
        ck(h.stats().live_objects == 0, "[19q] live_objects==0 at end");
        ck(h.stats().freed_objects == 3, "[19r] freed_objects==3 (a,c via free_object + b via collect)");
    }

    // ====================================================================
    // [20]-[28]: CONCURRENCY + RE-ENTRY SAFETY (the standalone collector
    // core under multiple OS threads + same-thread registry mutation during
    // callback/observer invocation).
    //
    // These tests pin the thread-safety contract the collector core must hold:
    //   - concurrent allocation / root registration+removal / liveness checks /
    //     write barriers / callback registration / immediate deletion
    //     (free_object) / repeated collection must complete WITHOUT container
    //     corruption, invalid iterator use, double-free, or inconsistent
    //     counters; and
    //   - a trace callback or barrier observer that registers or unregisters
    //     ITSELF (or another entry) DURING invocation must not invalidate the
    //     active iteration over the registry (snapshot semantics).
    //
    // They are RED on the pre-synchronization implementation: the old core
    // has no heap lock (concurrent mutation of m_live / m_roots / m_trace_cbs /
    // m_barrier_obs / m_stats is a data race -> corruption/crash/lost counts)
    // AND range-iterates the callback/observer vectors while invoking user code
    // that can mutate that same vector (same-thread iterator invalidation). The
    // assertions below encode the full GREEN behavior and fail honestly on the
    // old implementation (crash or invariant violation).
    //
    // Invariants every multithreaded test checks after joining all workers:
    //   (I1) the process did not crash (we reached the assertions);
    //   (I2) live_objects + freed_objects == total_allocated (every allocated
    //        object is either still live or has been freed exactly once);
    //   (I3) no counter went negative / inconsistent (stats are sane).
    // ====================================================================

    // [20] Re-entry: a trace callback that UNREGISTERS ITSELF during collect().
    //      Two callbacks A (first) + B (second) each keep an object alive; A
    //      unregisters itself from inside its own invocation. On the old core,
    //      collect() range-iterates m_trace_cbs while A erases from it -> the
    //      active iterator/reference is invalidated (B is skipped or the loop
    //      runs off the end -> UB/crash). The GREEN contract is SNAPSHOT
    //      semantics: collect() snapshots the registry before invoking, so A
    //      unregistering itself mutates the live registry but NOT the snapshot
    //      -> B is still invoked -> BOTH objects survive.
    {
        GcHeap h;
        void* objA = h.alloc(8, refmap_none());
        void* objB = h.alloc(8, refmap_none());
        void* reportedA = objA;
        void* reportedB = objB;
        // GcTraceFn is a plain function pointer, so we pack everything the
        // callback needs into one user_data struct. dummies[] captures the
        // tokens of the throwaway callbacks A registers to FORCE a
        // reallocation of m_trace_cbs during collect() (a bare erase may not
        // always crash on every libc++, but a reallocation invalidates EVERY
        // iterator/reference collect() holds -> deterministic crash on the old
        // core). On the new core the snapshot is independent of the live
        // vector, so the reallocation is harmless.
        struct ACtx {
            void* reported;
            GcHeap* heap;
            GcTraceToken token;
            int* ran_flag;
            GcTraceToken* dummies;
            int* dummy_n;
        };
        int ranA = 0;
        GcTraceToken dummies[16] = {0};
        int dummyN = 0;
        ACtx ac{ reportedA, &h, 0, &ranA, dummies, &dummyN };
        // A (registered FIRST): report objA, unregister ITSELF, AND register a
        // burst of no-op callbacks to force m_trace_cbs reallocation while
        // collect() is iterating it. On the old core this invalidates the
        // active iterator/reference (B skipped or crash). On the new core the
        // snapshot was taken before invocation, so A mutating the live registry
        // (erase + push_backs) is safe + B is still invoked from the snapshot.
        GcTraceToken realA = h.register_trace_callback(&ac,
            [](void* ud, GcTraceVisitor& v) {
                ACtx* c = static_cast<ACtx*>(ud);
                v.report(c->reported);
                *c->ran_flag = 1;
                // Register ~12 no-op callbacks to force a reallocation of
                // m_trace_cbs (capacity 2 -> must grow). This is the reliable
                // iterator-invalidation trigger.
                for (int i = 0; i < 12; ++i) {
                    c->dummies[*c->dummy_n] =
                        c->heap->register_trace_callback(nullptr,
                            [](void*, GcTraceVisitor&) {});
                    (*c->dummy_n)++;
                }
                c->heap->unregister_trace_callback(c->token);
            });
        ac.token = realA;
        // B (registered SECOND): report objB. If A's mutations invalidated the
        // iterator, B is skipped -> objB reaped.
        h.register_trace_callback(&reportedB,
            [](void* ud, GcTraceVisitor& v) {
                v.report(*static_cast<void**>(ud));
            });
        h.collect();
        ck(ranA == 1, "[20a] self-unregistering callback A ran");
        ck(h.is_live(objA), "[20b] A's object survives (A reported it before unregister)");
        ck(h.is_live(objB), "[20c] B's object survives (B not skipped by iterator invalidation)");
        ck(h.stats().live_objects == 2, "[20d] live_objects==2 (snapshot: both callbacks invoked)");
        // A is now unregistered; a second collect keeps B but reaps A (no root).
        h.collect();
        ck(!h.is_live(objA), "[20e] A's object collected after A unregistered");
        ck(h.is_live(objB), "[20f] B's object still survives");
        // Clean up the dummy callbacks A registered.
        for (int i = 0; i < dummyN; ++i) h.unregister_trace_callback(dummies[i]);
    }

    // [21] Re-entry: a trace callback that REGISTERS A NEW CALLBACK during
    //      collect(). On the old core, push_back on m_trace_cbs may reallocate
    //      the vector while collect() holds an iterator/reference into it ->
    //      dangling reference / crash. The GREEN contract (snapshot semantics):
    //      a callback registered DURING a collect() cycle is NOT invoked in
    //      that same cycle (the snapshot was already taken); it IS invoked in
    //      the NEXT cycle. We assert exactly that.
    {
        GcHeap h;
        void* objA = h.alloc(8, refmap_none());
        void* objNew = h.alloc(8, refmap_none());
        void* reportedA = objA;
        // newSlot is the void** the newly-registered callback will report
        // (*newSlot). The during-collect callback registers a new callback
        // backed by &newSlot; we set newSlot BEFORE each collect to choose
        // which object that callback should keep alive.
        void* newSlot = objNew;
        struct Ctx { void* reported; void** slot; GcHeap* heap; GcTraceToken* out_tok; bool* did_reg; };
        GcTraceToken newTok = 0;
        bool didReg = false;
        Ctx c{ reportedA, &newSlot, &h, &newTok, &didReg };
        h.register_trace_callback(&c,
            [](void* ud, GcTraceVisitor& v) {
                Ctx* c = static_cast<Ctx*>(ud);
                v.report(c->reported);
                // Register a NEW callback during invocation, ONCE. On the old
                // core this push_back can reallocate m_trace_cbs while collect()
                // iterates it -> crash. On the new core the snapshot is
                // unaffected; the new callback is deferred to the next cycle.
                if (!*c->did_reg) {
                    *c->out_tok = c->heap->register_trace_callback(c->slot,
                        [](void* ud2, GcTraceVisitor& v2) {
                            v2.report(*static_cast<void**>(ud2));
                        });
                    *c->did_reg = true;
                }
            });
        // First collect: A reported -> objA survives. objNew is NOT reported
        // this cycle (the new callback was registered mid-cycle, AFTER the
        // snapshot was taken) -> objNew collected.
        h.collect();
        ck(h.is_live(objA), "[21a] A survives first collect");
        ck(!h.is_live(objNew), "[21b] new-callback object collected first cycle (snapshot: not yet invoked)");
        ck(newTok != 0, "[21c] new callback registered with a valid token");
        // Second collect: the new callback is now in the registry -> it reports
        // *newSlot (still objNew). But objNew was already freed last cycle; the
        // visitor safely ignores the freed pointer. So objNew stays gone.
        h.collect();
        ck(h.is_live(objA), "[21d] A survives second collect");
        ck(!h.is_live(objNew), "[21e] freed pointer not resurrected by new callback");
        // Point newSlot at a FRESH object + collect: the now-active new
        // callback reports it -> it survives.
        void* objNew2 = h.alloc(8, refmap_none());
        newSlot = objNew2;  // the callback reads *newSlot
        h.collect();
        ck(h.is_live(objNew2), "[21f] new callback active: its object survives next cycle");
        h.unregister_trace_callback(newTok);
        h.collect();
        ck(!h.is_live(objNew2), "[21g] new-callback object reaped after unregister");
    }

    // [22] Re-entry: a barrier observer that UNREGISTERS ITSELF during
    //      write_barrier(). On the old core, write_barrier() range-iterates
    //      m_barrier_obs while the observer erases from it -> iterator
    //      invalidation (the same bug as [20] for the barrier path). GREEN:
    //      write_barrier() snapshots the observers before invocation, so a
    //      self-unregistering observer mutates the live registry but not the
    //      snapshot -> the second observer is still invoked.
    {
        GcHeap h;
        void* owner = h.alloc(16, refmap_words({0}));
        void* child = h.alloc(8, refmap_none());
        // dummies[] captures tokens of throwaway observers A registers to FORCE
        // a reallocation of m_barrier_obs during write_barrier() (a bare erase
        // may not always crash, but a reallocation invalidates every iterator/
        // reference write_barrier() holds -> deterministic crash on the old
        // core). On the new core the snapshot is independent of the live vector.
        struct OCtx { GcHeap* heap; GcBarrierToken token; int* fired; GcBarrierToken* dummies; int* dummy_n; };
        int firedA = 0, firedB = 0;
        GcBarrierToken dummies[16] = {0};
        int dummyN = 0;
        OCtx oa{ &h, 0, &firedA, dummies, &dummyN };
        GcBarrierToken tokA = h.register_barrier_observer(&oa,
            [](void* ud, void* o, void* c) {
                OCtx* x = static_cast<OCtx*>(ud);
                (*x->fired)++;
                // Register ~12 no-op observers to force m_barrier_obs to
                // reallocate (capacity 2 -> must grow) while write_barrier()
                // iterates it. Reliable iterator-invalidation trigger.
                for (int i = 0; i < 12; ++i) {
                    x->dummies[*x->dummy_n] =
                        x->heap->register_barrier_observer(nullptr,
                            [](void*, void*, void*) {});
                    (*x->dummy_n)++;
                }
                x->heap->unregister_barrier_observer(x->token);  // self-unregister
                (void)o; (void)c;
            });
        oa.token = tokA;
        struct BRec { int* fired; } br{ &firedB };
        h.register_barrier_observer(&br,
            [](void* ud, void* o, void* c) {
                (*static_cast<BRec*>(ud)->fired)++;
                (void)o; (void)c;
            });
        h.write_barrier(owner, child);
        ck(firedA == 1, "[22a] self-unregistering observer A fired");
        ck(firedB == 1, "[22b] observer B fired (not skipped by iterator invalidation)");
        ck(h.stats().barrier_calls == 1, "[22c] barrier_calls==1");
        // A is now unregistered; a second barrier fires only B.
        h.write_barrier(owner, child);
        ck(firedA == 1, "[22d] A not fired after self-unregister");
        ck(firedB == 2, "[22e] B fired again");
        ck(h.stats().barrier_calls == 2, "[22f] barrier_calls==2");
        for (int i = 0; i < dummyN; ++i) h.unregister_barrier_observer(dummies[i]);
    }

    // [23] Re-entry: a barrier observer that REGISTERS A NEW OBSERVER during
    //      write_barrier(). On the old core, push_back on m_barrier_obs may
    //      reallocate while write_barrier() iterates it -> crash. GREEN: the
    //      new observer is deferred (not fired in this same call; snapshot
    //      already taken).
    {
        GcHeap h;
        void* owner = h.alloc(16, refmap_words({0}));
        void* child = h.alloc(8, refmap_none());
        struct Ctx { GcHeap* heap; GcBarrierToken* out; int* reg_fired; bool* did_reg; };
        int regFired = 0;
        GcBarrierToken newTok = 0;
        bool didReg = false;
        Ctx c{ &h, &newTok, &regFired, &didReg };
        h.register_barrier_observer(&c,
            [](void* ud, void* o, void* ch) {
                Ctx* c = static_cast<Ctx*>(ud);
                // Register a NEW observer during invocation, ONCE. On the old
                // core this push_back can reallocate m_barrier_obs while
                // write_barrier() iterates it -> crash. On the new core the
                // snapshot is unaffected; the new observer is deferred.
                if (!*c->did_reg) {
                    *c->out = c->heap->register_barrier_observer(c->reg_fired,
                        [](void* ud2, void*, void*) {
                            (*static_cast<int*>(ud2))++;
                        });
                    *c->did_reg = true;
                }
                (void)o; (void)ch;
            });
        h.write_barrier(owner, child);
        ck(newTok != 0, "[23a] new observer registered with valid token");
        ck(regFired == 0, "[23b] new observer NOT fired in the same call (snapshot semantics)");
        ck(h.stats().barrier_calls == 1, "[23c] barrier_calls==1");
        // Next barrier: the new observer IS in the registry now -> fires.
        h.write_barrier(owner, child);
        ck(regFired == 1, "[23d] new observer fires on the next barrier");
        ck(h.stats().barrier_calls == 2, "[23e] barrier_calls==2");
        h.unregister_barrier_observer(newTok);
    }

    // --------------------------------------------------------------------
    // Multithreaded tests. Each spawns N worker threads that hammer a shared
    // GcHeap concurrently, then joins all + checks the (I1)/(I2)/(I3)
    // invariants. On the old (unsynchronized) core these are data races on
    // std::unordered_set / std::vector / size_t counters -> corruption,
    // double-free, or crash. On the synchronized core (coarse heap lock) they
    // complete with consistent state.
    // --------------------------------------------------------------------
    const int HW = std::max(2, int(std::thread::hardware_concurrency()));
    // Cap the worker count so the test stays fast + deterministic-ish under
    // the build-timeout; use at least 4 threads to force real contention even
    // on low-core CI boxes.
    const int NTHREADS = HW < 4 ? 4 : (HW > 8 ? 8 : HW);

    // [24] concurrent allocation + repeated collection (no roots).
    //      N-1 threads allocate K objects each (unrooted); 1 thread repeatedly
    //      collects while they do. Invariant: completes; live+freed==total;
    //      a final collect reaps every unrooted object -> live==0.
    //
    //      Stop protocol: the collect worker loops on `running` until the
    //      alloc workers have ALL finished (we join them first), then the main
    //      thread clears `running` and joins the collect worker. (Joining the
    //      collect worker before signalling stop would deadlock: it would loop
    //      forever waiting for a flag only set after its own join.)
    {
        GcHeap h;
        const int K = 4000;
        std::atomic<bool> running{true};
        std::vector<std::thread> alloc_ths;
        auto alloc_worker = [&]() {
            for (int i = 0; i < K; ++i) {
                void* p = h.alloc(16, refmap_none());
                (void)p;  // unrooted -> collectable
            }
        };
        auto collect_worker = [&]() {
            while (running.load(std::memory_order_relaxed)) {
                h.collect();
            }
        };
        for (int t = 0; t < NTHREADS - 1; ++t) alloc_ths.emplace_back(alloc_worker);
        std::thread collect_th(collect_worker);
        for (auto& th : alloc_ths) th.join();   // alloc workers finish after K
        running.store(false, std::memory_order_relaxed);  // signal collect worker to stop
        collect_th.join();
        GcStats s = h.stats();
        ck(s.total_allocated == size_t((NTHREADS - 1) * K), "[24a] total_allocated == (N-1)*K");
        ck(s.live_objects + s.freed_objects == s.total_allocated, "[24b] invariant: live+freed==total");
        h.collect();  // reap any remaining unrooted
        s = h.stats();
        ck(s.live_objects == 0, "[24c] final collect reaped all unrooted -> live==0");
        ck(s.freed_objects == s.total_allocated, "[24d] freed==total at end");
    }

    // [25] concurrent root registration/removal + liveness checks.
    //      PHASED design (avoids the alloc-to-root sequence-atomicity window):
    //      the core coarse lock makes individual ops (alloc / add_root /
    //      collect / remove_root / is_live) atomic, but it does NOT make a
    //      multi-op sequence (alloc -> add_root) atomic across ops — a
    //      concurrent collect by another thread can reap an object in that
    //      window (that is the higher-level stop-the-world protocol's job,
    //      documented in gc.hpp). So we keep concurrent collect OUT of the
    //      alloc-to-root window:
    //        phase 1: N threads each alloc K objects + root them (no collect).
    //        phase 2: main collects once -> every rooted object survives.
    //        phase 3: N threads each unroot their K objects (no collect).
    //        phase 4: main collects once -> every object reaped.
    //      This exercises concurrent add_root / remove_root / is_live /
    //      alloc (phase 1+3) + liveness after a real collect (phase 2+4)
    //      without relying on sequence atomicity the core lock does not give.
    //      Invariants: no crash; live+freed==total; phase2 live==N*K; phase4
    //      live==0.
    {
        GcHeap h;
        const int K = 1500;
        // Each thread owns K (object, slot) pairs in its own vector so the
        // root slots stay alive + stable across phases.
        struct Slot { void* obj; void* ptr; };
        std::vector<std::vector<Slot>> per_thread(NTHREADS);
        auto root_worker = [&](int tid) {
            auto& mine = per_thread[tid];
            mine.reserve(K);  // no reallocation: &mine[i].ptr stays stable
            for (int i = 0; i < K; ++i) {
                void* obj = h.alloc(16, refmap_none());
                mine.push_back(Slot{ obj, obj });  // stable address now
                Slot& s = mine.back();
                h.add_root(&s.ptr);   // root the STABLE vector slot (concurrent add_root)
                if (!h.is_live(obj)) { g_fail = 1; std::printf("  [FAIL] [25] obj not live right after alloc+root\n"); }
            }
        };
        std::vector<std::thread> ths;
        for (int t = 0; t < NTHREADS; ++t) ths.emplace_back(root_worker, t);
        for (auto& th : ths) th.join();   // phase 1 done: all rooted
        GcStats s = h.stats();
        ck(s.total_allocated == size_t(NTHREADS * K), "[25a] total_allocated == N*K after phase 1");
        h.collect();                      // phase 2: every rooted object must survive
        s = h.stats();
        ck(s.live_objects == size_t(NTHREADS * K), "[25b] phase 2: all rooted objects survived collect");
        ck(s.freed_objects == 0, "[25c] phase 2: nothing freed (all rooted)");
        // verify every object is still live via is_live (concurrent-read safe)
        for (auto& mine : per_thread) for (auto& sl : mine)
            if (!h.is_live(sl.obj)) { g_fail = 1; std::printf("  [FAIL] [25] rooted obj not live after phase 2 collect\n"); }
        ths.clear();
        auto unroot_worker = [&](int tid) {
            for (auto& sl : per_thread[tid]) h.remove_root(&sl.ptr);  // concurrent remove_root
        };
        for (int t = 0; t < NTHREADS; ++t) ths.emplace_back(unroot_worker, t);
        for (auto& th : ths) th.join();   // phase 3 done: all unrooted
        h.collect();                      // phase 4: every object reaped
        s = h.stats();
        ck(s.live_objects == 0, "[25d] phase 4: all objects reaped after unroot+collect");
        ck(s.live_objects + s.freed_objects == s.total_allocated, "[25e] invariant: live+freed==total");
        ck(s.freed_objects == s.total_allocated, "[25f] freed==total at end");
    }

    // [26] concurrent immediate deletion (free_object) + collection.
    //      Each thread allocs K objects, free_object's the even-indexed ones
    //      (each freed EXACTLY once — no re-free), and periodically collects
    //      concurrently with the other threads. The odd-indexed objects are
    //      left unrooted for the collectors to reap. Invariant: no crash; no
    //      double-free / corruption (freed==total at end); live+freed==total.
    //
    //      NOTE: we do NOT re-free the same pointer here (unlike the
    //      single-threaded [17]). Under concurrency the allocator may RECYCLE
    //      a just-freed address into another thread's alloc, so a stale
    //      free_object would free an UNRELATED live object (a use-after-free
    //      that is the caller's contract per gc.hpp, not a GC bug). The
    //      deterministic double-free-rejects contract is pinned single-
    //      threaded in [17]; this test pins the CONCURRENT safety of
    //      free_object (no corruption / inconsistent counters under load).
    {
        GcHeap h;
        const int K = 2000;
        std::atomic<size_t> freed_calls{0};
        std::vector<std::thread> ths;
        auto worker = [&]() {
            std::vector<void*> objs;
            objs.reserve(K);
            for (int i = 0; i < K; ++i) objs.push_back(h.alloc(16, refmap_none()));
            for (int i = 0; i < K; ++i) {
                if (i % 2 == 0) {
                    if (h.free_object(objs[i]))           // each freed exactly once
                        freed_calls.fetch_add(1, std::memory_order_relaxed);
                }
                if (i % 500 == 0) h.collect();  // concurrent collect while others free
            }
            h.collect();
        };
        for (int t = 0; t < NTHREADS; ++t) ths.emplace_back(worker);
        for (auto& th : ths) th.join();
        GcStats s = h.stats();
        ck(s.total_allocated == size_t(NTHREADS * K), "[26a] total_allocated == N*K");
        ck(s.live_objects + s.freed_objects == s.total_allocated, "[26b] invariant: live+freed==total");
        ck(s.live_objects == 0, "[26c] all reaped by end (odd collected, even free_object'd)");
        ck(s.freed_objects == s.total_allocated, "[26d] freed==total (no corruption/double-free)");
        ck(freed_calls.load() <= size_t(NTHREADS * (K / 2)), "[26e] free_object succeeded <= N*(K/2) (rest reaped by concurrent collect)");
    }

    // [27] concurrent write barriers + observer registration/removal.
    //      Each thread allocs an owner+child, registers an observer, hammers
    //      write_barrier(owner,child), then unregisters. barrier_calls must
    //      equal the total number of valid barrier events (each thread counts
    //      its own). No crash; counter consistent.
    {
        GcHeap h;
        const int K = 2000;
        std::atomic<size_t> expected_barriers{0};
        std::atomic<size_t> obs_fires{0};
        std::vector<std::thread> ths;
        struct Obs { std::atomic<size_t>* fires; };
        auto worker = [&]() {
            void* owner = h.alloc(16, refmap_words({0}));
            void* child = h.alloc(8, refmap_none());
            std::memcpy(owner, &child, 8);
            Obs rec{ &obs_fires };
            GcBarrierToken tok = h.register_barrier_observer(&rec,
                [](void* ud, void*, void*) {
                    static_cast<Obs*>(ud)->fires->fetch_add(1, std::memory_order_relaxed);
                });
            size_t mine = 0;
            for (int i = 0; i < K; ++i) {
                if (h.is_live(owner) && h.is_live(child)) {
                    h.write_barrier(owner, child);
                    ++mine;
                }
            }
            expected_barriers.fetch_add(mine, std::memory_order_relaxed);
            h.unregister_barrier_observer(tok);
            // keep owner+child alive so the observer reads stay valid.
        };
        for (int t = 0; t < NTHREADS; ++t) ths.emplace_back(worker);
        for (auto& th : ths) th.join();
        GcStats s = h.stats();
        // Each valid barrier bumps barrier_calls exactly once. With N threads
        // each registering its OWN observer, a single barrier event fires every
        // currently-registered observer (up to N), so obs_fires may be up to
        // N*barrier_calls (NOT <= barrier_calls). barrier_calls itself is the
        // authoritative per-event count.
        ck(s.barrier_calls == expected_barriers.load(), "[27a] barrier_calls == sum of valid barriers per thread");
        ck(obs_fires.load() <= size_t(NTHREADS) * s.barrier_calls, "[27b] observer fires <= N*barrier_calls (each barrier fires all registered observers)");
        ck(obs_fires.load() > 0, "[27c] at least one observer fired");
        ck(s.live_objects + s.freed_objects == s.total_allocated, "[27d] invariant: live+freed==total");
    }

    // [28] concurrent callback registration + collection (phased).
    //      Same phased rationale as [25]: the core lock makes individual ops
    //      atomic but NOT the alloc->register-callback sequence, so a
    //      concurrent collect could reap an object in that window. We keep
    //      concurrent collect out of the window:
    //        phase 1: N threads each alloc K objects + register a trace
    //                 callback reporting each (no collect).
    //        phase 2: main collects once -> every callback-reported object
    //                 survives (concurrent m_trace_cbs registration is now
    //                 stable; the snapshot covers all N*K callbacks).
    //        phase 3: N threads each unregister their K callbacks (no collect).
    //        phase 4: main collects once -> every object reaped.
    //      Exercises concurrent register_trace_callback / unregister_trace_callback
    //      + a real collect over the full registry. No crash; live+freed==total.
    {
        GcHeap h;
        const int K = 800;
        struct Entry { void* obj; void* slot; GcTraceToken tok; };
        std::vector<std::vector<Entry>> per_thread(NTHREADS);
        auto reg_worker = [&](int tid) {
            auto& mine = per_thread[tid];
            mine.reserve(K);  // no reallocation: &mine[i].slot stays stable
            for (int i = 0; i < K; ++i) {
                void* obj = h.alloc(16, refmap_none());
                mine.push_back(Entry{ obj, obj, 0 });   // stable address now
                Entry& e = mine.back();
                // Register the callback against the STABLE vector slot
                // (&e.slot), not a local. The callback reports *slot == obj.
                e.tok = h.register_trace_callback(&e.slot,
                    [](void* ud, GcTraceVisitor& v) {
                        v.report(*static_cast<void**>(ud));
                    });
            }
        };
        std::vector<std::thread> ths;
        for (int t = 0; t < NTHREADS; ++t) ths.emplace_back(reg_worker, t);
        for (auto& th : ths) th.join();   // phase 1 done: all callbacks registered
        GcStats s = h.stats();
        ck(s.total_allocated == size_t(NTHREADS * K), "[28a] total_allocated == N*K after phase 1");
        h.collect();                      // phase 2: every callback-reported object survives
        s = h.stats();
        ck(s.live_objects == size_t(NTHREADS * K), "[28b] phase 2: all callback-reported objects survived collect");
        ck(s.freed_objects == 0, "[28c] phase 2: nothing freed (all callback-rooted)");
        for (auto& mine : per_thread) for (auto& e : mine)
            if (!h.is_live(e.obj)) { g_fail = 1; std::printf("  [FAIL] [28] callback-reported obj not live after phase 2 collect\n"); }
        ths.clear();
        auto unreg_worker = [&](int tid) {
            for (auto& e : per_thread[tid]) h.unregister_trace_callback(e.tok);  // concurrent unregister
        };
        for (int t = 0; t < NTHREADS; ++t) ths.emplace_back(unreg_worker, t);
        for (auto& th : ths) th.join();   // phase 3 done: all unregistered
        h.collect();                      // phase 4: every object reaped
        s = h.stats();
        ck(s.live_objects == 0, "[28d] phase 4: all objects reaped after unregister+collect");
        ck(s.live_objects + s.freed_objects == s.total_allocated, "[28e] invariant: live+freed==total");
        ck(s.freed_objects == s.total_allocated, "[28f] freed==total at end");
    }

    std::printf("\ngc_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
