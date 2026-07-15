// gc_test.cpp — tracing mark-sweep GC core unit tests.
// Exercises the GcHeap directly: alloc+collect, root scanning, reachable
// survives, unreachable collected, ref-graph tracing, cycles, stats, clear.
#include "../src/gc.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>

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
        const GcStats& s0 = h.stats();
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

    std::printf("\ngc_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
