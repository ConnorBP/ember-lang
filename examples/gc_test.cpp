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

    std::printf("\ngc_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
