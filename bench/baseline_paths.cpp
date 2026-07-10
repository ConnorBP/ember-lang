// baseline_paths.cpp — the g++ -O2 baseline for the per-path codegen bench.
//
// Each `extern "C"` function here is the C++ equivalent of one ember path fn
// in bench_codegen_paths.cpp, doing the SAME work (same iteration count, same
// body) so the ember/g++-O2 ratio is apples-to-apples. Compiled to a DLL at
// runtime by run_bench.sh:
//   g++ -O2 -std=c++17 -shared -fPIC baseline_paths.cpp -o baseline_paths.dll
// then each symbol is dlsym'd and timed identically to the ember fn.
//
// ANTI-FOLD DISCIPLINE: g++ -O2 would const-fold a fixed-count loop to a closed
// form (returning the right value in ~0ns), making the ratio meaningless. To
// measure the CODEGEN PATH (the loop + body), each baseline fn takes its
// iteration count `N` as a PARAMETER, and the harness passes N read from a
// `volatile` source the compiler cannot see through. -O2 still optimizes the
// loop BODY (add/mul/div/call/index) — exactly the codegen path under test —
// but cannot eliminate the loop. ember's count is a source literal (ember does
// not const-fold), so the two do identical work; only the bound's
// representation differs (loop back-edge is the same).
//
// Each fn also writes its accumulator through a volatile sink so the body
// is not DCE'd.

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
// volatile sink — forces the loop body to be retained under -O2.
// Defined here; the harness never reads it.
volatile long long g_sink = 0;
}  // extern "C"

// ---- path 1: int_div (cqo+idiv twice per iter, div0 guard always on) ----
// ember: while(i<N){ s += (i*7)/(i+1) + (i*13)%(i+2); i++; }
extern "C" long long bench_int_div(long long N) {
    long long s = 0;
    long long i = 1;
    while (i < N) {
        s += (i * 7) / (i + 1) + (i * 13) % (i + 2);
        i = i + 1;
    }
    g_sink = s;
    return s;
}

// ---- path 2: call_overhead (3-deep call chain a->b->c, N calls) ----
// noinline: -O2 would otherwise inline c into b into a into the loop, turning
// the call-overhead path into straight-line arithmetic (measuring inlining,
// not the call ABI). The noinline keeps the 4 calls/iter real (the path under
// test in ember, which cannot inline script-to-script calls).
extern "C" __attribute__((noinline)) long long bench_c(long long x) { return x + 1; }
extern "C" __attribute__((noinline)) long long bench_b(long long x) { return bench_c(x) + bench_c(x); }
extern "C" __attribute__((noinline)) long long bench_a(long long x) { return bench_b(x) + bench_b(x); }
extern "C" long long bench_call_overhead(long long N) {
    long long sum = 0;
    long long i = 0;
    while (i < N) { sum += bench_a(i); i = i + 1; }
    g_sink = sum;
    return sum;
}

// ---- path 3: loop_overhead (empty-ish while, N iters) ----
extern "C" long long bench_loop_overhead(long long N) {
    long long s = 0;
    long long i = 0;
    while (i < N) { s += i; i = i + 1; }
    g_sink = s;
    return s;
}

// ---- path 4: slice_bounds (index i64[64] N times, bounds check each in ember) ----
// C++ indexes unchecked (C++ has no bounds check — the bench measures ember's
// bounds-check cost vs the unchecked native; that's the point of this path).
extern "C" long long bench_slice_bounds(long long N) {
    long long a[64];
    for (int k = 0; k < 64; ++k) a[k] = (long long)k;
    long long s = 0;
    long long i = 0;
    while (i < N) { s += a[i % 64]; i = i + 1; }
    g_sink = s;
    return s;
}

// ---- path 5: string_decrypt (string length read N times) ----
// ember: string_xor_key != 0 inlines a byte-XOR decrypt per use; C++ has no
// decrypt — the delta IS the obf-feature cost (the finding). To avoid g++ -O2
// const-folding `strlen(literal)` to a constant, we read the length of a
// heap buffer whose contents the compiler cannot see (a volatile-pointer
// indirection). The body is the length-read path; -fno-tree-vectorize keeps
// it scalar (matching ember's scalar path).
static volatile char* g_str_buf = nullptr;
extern "C" long long bench_string_decrypt(long long N) {
    if (!g_str_buf) {  // one-time init of the volatile-pointed buffer
        char* b = new char[13];
        for (int k = 0; k < 12; ++k) b[k] = "hello world!"[k];
        b[12] = 0;
        g_str_buf = b;
    }
    const char* s = (const char*)g_str_buf;   // load through volatile (compiler can't see contents)
    long long acc = 0;
    long long i = 0;
    while (i < N) {
        long long len = 0; while (s[len]) ++len;   // manual strlen (not foldable)
        acc ^= len + i;   // data-dependent accumulate (not close-formable)
        i = i + 1;
    }
    g_sink = acc;
    return acc;
}

// ---- path 6: struct_by_value (12-byte struct by-value arg + return, N times) ----
// NOTE: ember's struct-by-value path has a known codegen corruption at larger
// N (see BENCHMARK_SYSTEM_DESIGN.md findings), so both ember and the baseline
// use the SAME small N (the harness passes it). The ABI temp copy is the path.
// Workload mirrors ember exactly: each iter builds a fresh BP{1,2,3} (struct
// return by value), passes it by value to sump (struct-by-value arg), sums.
// noinline: -O2 would otherwise inline both fns and elide the ABI temp copy
// (the path under test).
struct BP { int a; int b; int c; };
extern "C" __attribute__((noinline)) BP mkp(int a, int b, int c) { BP p{a,b,c}; return p; }
extern "C" __attribute__((noinline)) long long sump(BP p) { return (long long)p.a + (long long)p.b + (long long)p.c; }
extern "C" long long bench_struct_by_value(long long N) {
    long long s = 0;
    long long i = 0;
    while (i < N) { long long r = sump(mkp(1, 2, 3)); s += r; i = i + 1; }
    g_sink = s;
    return s;
}
