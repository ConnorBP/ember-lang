// bench_codegen_paths.cpp — ember per-path codegen benchmark harness.
//
// PROTOTYPE of the per-path bench system (docs/spec/BENCHMARK_SYSTEM_DESIGN.md).
// Evidence-gathering infra for the codegen-optimization gate
// (docs/planning/DESIGN.md §9: "no speculative optimization before the bench
// proves it matters"; docs/spec/COMPILER_PIPELINE.md §5 defers SSA-lite IR +
// linear-scan regalloc until "stronger benchmarks show where Ember is slow").
//
// This harness measures WHERE ember's codegen is slow — per codegen PATH,
// in BOTH safety-off and safety-on modes, vs an optimizing native compiler
// (g++ -O2, the "optimizing native-JIT language's speed" the gate names). It
// is a CONSUMER of the existing JIT (no src/ changes), the bench_ember_vs_as /
// v0_4_hardening_test shape. It does NOT measure whole-entry-fn timing only
// (that's `ember bench`'s job); it breaks the matrix down per path × safety ×
// ember-vs-g++-O2 so the optimization design has per-path evidence.
//
// The g++ -O2 baseline (baseline_paths.cpp) is compiled to a DLL at BUILD
// time by a CMake custom command (ctest path) OR at runtime by bench/run_bench.sh
// (ad-hoc path) — stale-probe discipline either way: compile-from-source each
// build/run, never a checked-in binary. Each path's baseline fn is timed with
// the IDENTICAL warmup+iters pipeline so the ember/g++-O2 ratio is apples-to-apples.
//
// PASS = ran + wrote results. NEVER fails on a ratio (this is data, not an
// assertion). Fails only on a compile/run/IO error.
//
// 6 prototype paths (one per category): int_div, call_overhead, loop_overhead,
// slice_bounds, string_decrypt, struct_by_value. Adding a path = one PathBench
// struct + one extern "C" fn in baseline_paths.cpp (see the design doc §5.1).

#include "import.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "context.hpp"
#include "engine.hpp"        // ember_call_void, CompiledFn, finalize, free_executable
#include "dispatch_table.hpp"
#include "binding_builder.hpp"
#include "thin_ir.hpp"        // Stage C: ThinFunction (IR instr count metric)
#include "thin_lower.hpp"     // Stage C: lower_function (IR instr count metric)
#include "ember_pass.hpp"     // Stage C: EmberPassManager
#include "ember_pass_registry.hpp" // Stage C: EmberPassRegistry
#include "ember_pass_pipeline.hpp" // Stage C: build_pipeline_from_string
#include "ext_opt.hpp"         // Stage C: register_passes

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>

using namespace ember;

// ---- machine + compiler provenance (mirrors `ember bench`) ----
#if defined(__GNUC__)
static const char* kCc = "gcc"; static const char* kCcVer = __VERSION__;
#else
static const char* kCc = "unknown"; static const char* kCcVer = "?";
#endif

// ---- trap stub (the v0_4 shape): record reason + longjmp to per-iter checkpoint ----
extern "C" void bench_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1);
    }
    std::abort();
}

// ---- the per-path workload definition ----
// Each PathBench is ONE ember fn `main` doing a fixed amount of work that
// exercises the named codegen path, + the matching extern "C" symbol in the
// g++ -O2 DLL doing the SAME work. `guard_optional` notes whether the path's
// OWN guard (div0, bounds) is optional — the budget/depth guard is always
// optional (controlled by safety mode); a path with a non-optional guard
// records that in `note` so the safety-on overhead is correctly attributed.
// `string_xor` controls string encryption (only the string_decrypt path sets it
// nonzero; the bench measures that path's per-use inline-stack-XOR decrypt).
//
// `inner_n` is the path's inner-loop iteration count. The ember source uses a
// `%N` token which the harness substitutes with inner_n (a source literal —
// ember does not const-fold). The harness passes inner_n (read from volatile)
// to the g++ -O2 baseline fn so -O2 cannot const-fold the baseline loop while
// still optimizing the body. The two do identical work; only the bound's
// representation differs (loop back-edge is the same codegen path).
struct PathBench {
    const char* name;          // "int_div"
    std::string ember_src;     // .ember source (entry fn `main`, with %N tokens)
    const char* baseline_sym;  // extern "C" symbol in the g++ -O2 DLL (takes long long N)
    const char* note;          // "cqo+idiv+div0-guard"
    bool guard_optional;       // is the path's own guard optional? (bounds: no, div0: no)
    int iters;                 // timed iterations (outer: each calls main())
    int warmup;                // untimed warmup iterations
    uint8_t string_xor;        // 0 = no encryption; nonzero = encrypt (string_decrypt path)
    long long inner_n;         // inner-loop count (substituted into ember %N, passed to baseline)
    bool check_correctness;    // is ember==baseline the right invariant? (string_decrypt: no — baseline is a non-decrypt reference, delta is the finding)
    bool ir_safe;              // does this path work with enable_ir_backend? (scalar/control-flow/calls = yes; slices/strings/structs = no, Stage A known gaps)
};

// ---- compile an ember source into a callable module (the v0_4/bench_ember_vs_as shape) ----
struct BenchModule {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string, int> slots;
    GlobalsBlock gb;
    std::vector<uint8_t> gb_store;
    Program prog;
    std::unordered_map<std::string, NativeSig> natives;
    StructLayoutTable layouts;
    // Compile-time metrics (Stage C: for measuring pass overhead + effectiveness).
    double compile_ns = 0;      // total compile time (tokenize→parse→sema→codegen→finalize)
    size_t code_bytes = 0;      // total emitted x86 bytes (all fns)
    size_t ir_instr_count = 0;  // total ThinFunction instrs (if enable_ir_backend; 0 otherwise)
    BenchModule(int n) : table(n) {}
};

static std::unique_ptr<BenchModule> ember_compile(const std::string& src, bool safety_on, uint8_t string_xor, bool ir_safe = true) {
    auto t0 = std::chrono::steady_clock::now();  // compile-time start
    auto m = std::make_unique<BenchModule>(8);
    auto lr = tokenize(src, "<bench>");
    if (!lr.ok) { std::fprintf(stderr, "  ember lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::fprintf(stderr, "  ember parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = m->slots[fn.name]; }
    // register the six core extensions (string for string_length, math unused
    // here but matches the standard addon set the bench paths might call).
    OpOverloadTable ov;
    ext_vec::register_natives(m->natives);    ext_quat::register_natives(m->natives);
    ext_mat::register_natives(m->natives);    ext_string::register_natives(m->natives);
    ext_array::register_natives(m->natives);  ext_math::register_natives(m->natives);
    ext_vec::register_overloads(ov); ext_quat::register_overloads(ov);
    ext_mat::register_overloads(ov); ext_string::register_overloads(ov);
    m->layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = string_xor;   // 0 = plaintext; nonzero = inline-XOR decrypt per use
    auto sr = sema(m->prog, m->natives, m->slots, 0, &ov, &m->layouts);
    if (!sr.ok) {
        std::fprintf(stderr, "  ember sema: %s (line %u)\n",
                     sr.errors.empty() ? "<unknown>" : sr.errors[0].msg.c_str(),
                     sr.errors.empty() ? 0 : sr.errors[0].line);
        return nullptr;
    }

    m->gb_store.assign(m->prog.globals.size() * 8, 0);
    m->gb.base = int64_t(m->gb_store.data());
    g_globals_for_codegen = &m->gb;
    m->table = DispatchTable(m->prog.funcs.size());

    CodeGenCtx ctx;
    ctx.globals_base = m->gb.base;
    ctx.dispatch_base = int64_t(m->table.base());
    ctx.natives = &m->natives;
    ctx.script_slots = &m->slots;
    ctx.structs = &m->layouts;
    ctx.use_context_reg = true;   // B1: r14 = ctx, so ember_call_void works (both modes)
    // Stage 1 codegen optimization (docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4):
    // toggled via the EMBER_STAGE1_OPTS env var so the bench can run twice (off
    // vs on) and compare. Values: "peephole", "regalloc", "both". Default
    // (unset/other) = both off = byte-identical to today (the gate holds).
    if (const char* o = std::getenv("EMBER_STAGE1_OPTS")) {
        std::string s(o);
        if (s == "peephole")      ctx.enable_peephole = true;
        else if (s == "regalloc") ctx.enable_local_regalloc = true;
        else if (s == "both")     { ctx.enable_peephole = true; ctx.enable_local_regalloc = true; }
    }
    // Stage C: enable the IR backend (lower_function → ThinFunction → emit_x64)
    // so we can measure IR instr count + the IR path's compile/run cost.
    // Toggled via EMBER_IR_BACKEND=1. Default off = tree-walker (unchanged).
    // Only enabled for ir_safe paths (scalar/control-flow/calls); the known-
    // gap paths (slices/strings/structs) would crash the IR path (Stage A gaps).
    if (ir_safe) {
        if (const char* o = std::getenv("EMBER_IR_BACKEND")) {
            std::string s(o);
            if (s == "1" || s == "on" || s == "true") ctx.enable_ir_backend = true;
        }
    }
    // Stage C: EMBER_IR_PASS="constprop,cse,dce" — build a pass pipeline from
    // the opt extension registry and run it between lower_function and emit_x64.
    // Only effective when enable_ir_backend is on (set above or by EMBER_IR_BACKEND).
    EmberPassRegistry pass_reg;
    ext_opt::register_passes(pass_reg);
    EmberPassManager pass_pm;
    if (ctx.enable_ir_backend) {
        if (const char* o = std::getenv("EMBER_IR_PASS")) {
            std::string spec(o);
            if (!spec.empty()) {
                std::string pass_err;
                if (build_pipeline_from_string(spec, pass_reg, pass_pm, &pass_err))
                    ctx.pass_manager = &pass_pm;
            }
        }
    }
    if (safety_on) {
        // safety-on: budget + depth + trap. budget set huge (INT64_MAX) so we
        // measure the guard's INSTRUCTION COST (sub+jg per entry/back-edge +
        // inc/cmp per call), not its trap behavior — no false traps.
        ctx.emit_budget_checks = true;
        ctx.emit_depth_checks = true;
        ctx.trap_stub = (void*)&bench_trap;
        // budget/depth read through r14 (use_context_reg); NOT baked. We set
        // them on the per-call context_t (ectx) before each call.
    }
    for (auto& fn : m->prog.funcs) {
        // Stage C: if IR backend is on, lower to ThinFunction first to count
        // instrs (the pass-effectiveness metric: instr count before/after).
        if (ctx.enable_ir_backend) {
            ThinFunction thf = lower_function(fn, ctx);
            for (const auto& blk : thf.blocks)
                m->ir_instr_count += blk.instrs.size();
        }
        CompiledFn cf = compile_func(fn, ctx);
        finalize(cf);
        m->code_bytes += cf.bytes.size();
        m->table.set(fn.slot, cf.entry);
        m->fns.push_back(std::move(cf));
    }
    auto t1 = std::chrono::steady_clock::now();  // compile-time end
    m->compile_ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return m;
}

// ---- one timed cell: warmup + N iters, fresh checkpoint per iter (safety-on), stats ----
struct Stats { double min, median, mean, p99, stddev, cv; bool trapped; int64_t result; };

static Stats time_ember(BenchModule& m, context_t& ectx, int iters, int warmup) {
    Stats s{}; s.trapped = false;
    auto sit = m.slots.find("main");
    if (sit == m.slots.end()) { s.trapped = true; return s; }
    void* entry = m.table.get(sit->second);
    if (!entry) { s.trapped = true; return s; }

    // warmup (untimed), each under a fresh checkpoint
    for (int w = 0; w < warmup; ++w) {
        ectx.reset_for_call();
        ectx.budget_remaining = INT64_MAX;
        ectx.has_checkpoint = true;
        if (__builtin_setjmp(ectx.checkpoint)) { s.trapped = true; return s; }
        (void)ember_call_void(entry, &ectx);
    }
    ectx.has_checkpoint = false;

    // timed iters, each under its OWN fresh checkpoint (a trap in one iter
    // stops the bench for this cell cleanly, not the process)
    std::vector<double> ns; ns.reserve(size_t(iters));
    for (int it = 0; it < iters; ++it) {
        ectx.reset_for_call();
        ectx.budget_remaining = INT64_MAX;
        ectx.has_checkpoint = true;
        if (__builtin_setjmp(ectx.checkpoint)) { s.trapped = true; return s; }
        auto t0 = std::chrono::steady_clock::now();
        s.result = ember_call_void(entry, &ectx);
        auto t1 = std::chrono::steady_clock::now();
        ectx.has_checkpoint = false;
        ns.push_back(double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    std::sort(ns.begin(), ns.end());
    s.min = ns.front();
    s.mean = 0; for (double v : ns) s.mean += v; s.mean /= double(ns.size());
    double sd = 0; for (double v : ns) sd += (v - s.mean) * (v - s.mean);
    s.stddev = ns.size() > 1 ? std::sqrt(sd / double(ns.size() - 1)) : 0.0;
    s.median = ns[ns.size() / 2];
    s.p99 = ns[size_t(double(ns.size() - 1) * 0.99)];
    s.cv = s.mean > 0 ? (s.stddev / s.mean) * 100.0 : 0.0;
    return s;
}

// ---- time the g++ -O2 baseline fn with the IDENTICAL pipeline ----
// The baseline fn takes its inner-loop count N. We pass N read from a
// `volatile` so g++ -O2 cannot const-fold the loop (see baseline_paths.cpp's
// anti-fold discipline). The SAME N is baked into the ember source literal.
using BaselineFn = int64_t (*)(int64_t);

// volatile source of N — the compiler cannot see through this, so the loop
// bound is unknown at compile time and -O2 keeps the loop (optimizing body).
static volatile int64_t g_baseline_N = 0;

static Stats time_baseline(BaselineFn fn, int64_t N, int iters, int warmup) {
    Stats s{}; s.trapped = false;
    // warmup
    for (int w = 0; w < warmup; ++w) { g_baseline_N = N; s.result = fn(g_baseline_N); }
    std::vector<double> ns; ns.reserve(size_t(iters));
    for (int it = 0; it < iters; ++it) {
        auto t0 = std::chrono::steady_clock::now();
        g_baseline_N = N; s.result = fn(g_baseline_N);
        auto t1 = std::chrono::steady_clock::now();
        ns.push_back(double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    std::sort(ns.begin(), ns.end());
    s.min = ns.front();
    s.mean = 0; for (double v : ns) s.mean += v; s.mean /= double(ns.size());
    double sd = 0; for (double v : ns) sd += (v - s.mean) * (v - s.mean);
    s.stddev = ns.size() > 1 ? std::sqrt(sd / double(ns.size() - 1)) : 0.0;
    s.median = ns[ns.size() / 2];
    s.p99 = ns[size_t(double(ns.size() - 1) * 0.99)];
    s.cv = s.mean > 0 ? (s.stddev / s.mean) * 100.0 : 0.0;
    return s;
}

// ---- the 6 prototype paths (one per category; see design doc §6) ----
// Each ember `main` does a fixed amount of work exercising the named path;
// the matching extern "C" fn in baseline_paths.cpp does the SAME work (taking
// inner_n as its N param). The `%N` token in ember_src is substituted with
// inner_n (a source literal) by the harness before compiling.
static std::vector<PathBench> make_paths() {
    return {
        {"int_div",
            "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 1; while (i < %N) { s = s + (i*7)/(i+1) + (i*13)%(i+2); i = i + 1; } return s; }\n",
            "bench_int_div", "cqo+idiv x2 + div0-guard (always on)", false, 2000, 50, 0, 1000, true, true},
        {"call_overhead",
            "fn c(x: i64) -> i64 { return x + 1; }\n"
            "fn b(x: i64) -> i64 { return c(x) + c(x); }\n"
            "fn a(x: i64) -> i64 { return b(x) + b(x); }\n"
            "fn main() -> i64 { let mut sum: i64 = 0; let mut i: i64 = 0; while (i < %N) { sum = sum + a(i); i = i + 1; } return sum; }\n",
            "bench_call_overhead", "dispatch indirect call + prologue/epilogue x4/iter", true, 2000, 50, 0, 100000, true, true},
        {"loop_overhead",
            "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 0; while (i < %N) { s = s + i; i = i + 1; } return s; }\n",
            "bench_loop_overhead", "jmp back-edge + loop-body budget charge (safety-on)", true, 200, 20, 0, 10000000, true, true},
        {"slice_bounds",
            "fn main() -> i64 { let mut a: i64[64]; let mut k: i64 = 0; while (k < 64) { a[k] = k; k = k + 1; } let mut s: i64 = 0; let mut i: i64 = 0; while (i < %N) { s = s + a[i % 64]; i = i + 1; } return s; }\n",
            "bench_slice_bounds", "indexing w/ bounds check (always on); C++ unchecked", false, 200, 20, 0, 1000000, true, false},
        {"string_decrypt",
            "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 0; while (i < %N) { s = s + string_length(\"hello world!\"); i = i + 1; } return s; }\n",
            "bench_string_decrypt", "inline-stack-XOR decrypt per use (string_xor!=0)", true, 2000, 50, 0xA5, 100000, false, false},
        {"struct_by_value",
            "struct P { a: i32; b: i32; c: i32; }\n"
            "fn mkp(a: i32, b: i32, c: i32) -> P { let p: P = P { a: a, b: b, c: c }; return p; }\n"
            "fn sump(p: P) -> i64 { return (p.a as i64) + (p.b as i64) + (p.c as i64); }\n"
            "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 0; while (i < %N) { let r: i64 = sump(mkp(1, 2, 3)); s = s + r; i = i + 1; } return s; }\n",
            // n=20: a larger inner loop (>=~25) triggers a known struct-by-value-
            // in-loop codegen corruption (see findings note in BENCHMARK_SYSTEM_DESIGN.md);
            // both ember and the baseline use the SAME n so the comparison is fair.
            "bench_struct_by_value", "hidden-pointer ABI temp copy; struct-by-value arg + return", true, 2000, 50, 0, 20, true, false},
        // ── IR-pass workloads (Stage C): each exercises one optimization pass's ──
        // ── eliminable pattern. These are the workloads the pass system gates on. ──
        {"cse_redundant",
            // CSE: `x*7 + x*7` — the `x*7` is computed twice; CSE should fold
            // the second into a reuse of the first.
            "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 0; while (i < %N) { let a: i64 = i * 7; s = s + a + a; i = i + 1; } return s; }\n",
            "bench_cse_redundant", "CSE: redundant i*7 computed twice", false, 2000, 50, 0, 1000000, true, true},
        {"dce_dead_store",
            // DCE: `let dead = i * 13;` is computed but never used; DCE should
            // remove it entirely.
            "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 0; while (i < %N) { let dead: i64 = i * 13; s = s + i; i = i + 1; } return s; }\n",
            "bench_dce_dead_store", "DCE: dead store (i*13 unused)", false, 2000, 50, 0, 1000000, true, true},
        {"constprop_fold",
            // Const-prop: `let b = 3; let c = b + 4;` — b and c are constants;
            // const-prop should fold `c` to 7 at compile time.
            "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 0; while (i < %N) { let b: i64 = 3; let c: i64 = b + 4; s = s + c; i = i + 1; } return s; }\n",
            "bench_constprop_fold", "const-prop: b=3, c=b+4 fold to c=7", false, 2000, 50, 0, 1000000, true, true},
        {"licm_invariant",
            // LICM: `let k = 100 * 200;` is loop-invariant (computed every
            // iteration); LICM should hoist it out of the loop.
            "fn main() -> i64 { let mut s: i64 = 0; let mut i: i64 = 0; while (i < %N) { let k: i64 = 100 * 200; s = s + k + i; i = i + 1; } return s; }\n",
            "bench_licm_invariant", "LICM: 100*200 loop-invariant", false, 2000, 50, 0, 1000000, true, true},
    };
}

// ---- verdict band (ratio = ember/g++-O2 median) ----
static const char* verdict(double r) {
    if (r < 1.1) return "ember ~= g++-O2 (YAGNI wins)";
    if (r < 3.0) return "ember slower (peephole candidate)";
    return "ember MUCH slower (SSA-IR warranted)";
}

int main(int argc, char** argv) {
    std::printf("=== ember per-path codegen bench (prototype) ===\n");
    std::printf("    docs/spec/BENCHMARK_SYSTEM_DESIGN.md — gate: DESIGN §9 / COMPILER_PIPELINE §5\n\n");
    std::printf("#   compiler: %s %s\n", kCc, kCcVer);
    std::printf("#   platform: %s (ptr=%zu-bit)\n",
#if defined(__x86_64__) || defined(_M_X64)
                "x86-64", sizeof(void*)*8
#else
                "unknown", sizeof(void*)*8
#endif
    );
    // Stage 1 opts mode (the env var toggles peephole/regalloc; recorded so the
    // results MD is self-describing about which configuration produced it).
    if (const char* o = std::getenv("EMBER_STAGE1_OPTS"))
        std::printf("#   stage1_opts: %s\n", o);
    else
        std::printf("#   stage1_opts: (none — flags off, byte-identical baseline)\n");
    std::printf("#   date:     %s %s\n\n", __DATE__, __TIME__);

    // ---- load the g++ -O2 baseline DLL (compiled at runtime by run_bench.sh) ----
    // The baseline MUST exist (stale-probe discipline: compile-from-source each run).
    const char* dll = argc > 1 ? argv[1] : "bench/baseline_paths.dll";
    HMODULE hBaseline = LoadLibraryA(dll);
    if (!hBaseline) {
        std::fprintf(stderr, "ERROR: could not load g++ -O2 baseline DLL '%s' (err=%lu).\n"
                     "Build it first: g++ -O2 -std=c++17 -shared baseline_paths.cpp -o baseline_paths.dll\n",
                     dll, GetLastError());
        return 2;
    }
    std::printf("#   baseline: %s (g++ -O2)\n\n", dll);

    auto paths = make_paths();

    // ---- results storage (path × safety × engine) ----
    struct Cell { const char* path; const char* safety; const char* engine; Stats st;
                  double compile_ns = 0; size_t code_bytes = 0; size_t ir_instrs = 0; };
    std::vector<Cell> cells;

    bool any_compile_fail = false;

    for (auto& p : paths) {
        std::printf("--- path: %s  [%s]\n", p.name, p.note);

        // correctness: the g++ -O2 baseline result is the reference. ember (both
        // modes) should agree. The struct path uses n=20 (a larger inner loop
        // triggers a known struct-by-value-in-loop codegen corruption — see the
        // findings note in BENCHMARK_SYSTEM_DESIGN.md); correctness is checked
        // at the bench's chosen n.
        BaselineFn bfn = reinterpret_cast<BaselineFn>(
            (void*)GetProcAddress(hBaseline, p.baseline_sym));
        if (!bfn) {
            std::fprintf(stderr, "  ERROR: baseline symbol '%s' not found in DLL\n", p.baseline_sym);
            any_compile_fail = true;
            continue;
        }
        Stats bref = time_baseline(bfn, p.inner_n, 1, 0);   // one untimed call for the reference value
        int64_t ref = bref.result;

        // substitute %N -> inner_n (a source literal) in the ember source
        std::string src = p.ember_src;
        for (size_t pos = src.find("%N"); pos != std::string::npos; pos = src.find("%N", pos + std::to_string(p.inner_n).size())) {
            src.replace(pos, 2, std::to_string(p.inner_n));
        }

        for (bool safety_on : {false, true}) {
            const char* mode = safety_on ? "on" : "off";
            auto m = ember_compile(src, safety_on, p.string_xor, p.ir_safe);
            if (!m || m->fns.empty()) { std::fprintf(stderr, "  ember compile FAILED (%s)\n", mode); any_compile_fail = true; continue; }
            // Stage C: compile-time + code-size + IR-instr-count metrics.
            // Printed once per (path, safety) cell so the pass system can gate
            // on compile overhead + instr-count reduction + code-size delta.
            std::printf("  [compile] safety=%-3s  compile=%8.0f ns  code=%5zu B  ir_instrs=%zu\n",
                        mode, m->compile_ns, m->code_bytes, m->ir_instr_count);
            context_t ectx;
            ectx.max_call_depth = 8192;       // generous: we measure guard cost, not trap behavior
            ectx.budget_remaining = INT64_MAX;
            Stats es = time_ember(*m, ectx, p.iters, p.warmup);
            if (es.trapped) {
                std::fprintf(stderr, "  ember TRAP (safety=%s): %s\n", mode, ectx.last_error.c_str());
                cells.push_back({p.name, mode, "ember", es, m->compile_ns, m->code_bytes, m->ir_instr_count});
                continue;
            }
            // correctness check (ember vs g++ -O2 reference) — only for paths
            // where ember and the baseline do the SAME work. string_decrypt's
            // baseline is a non-decrypt reference (the delta IS the finding), so
            // byte-equality is not the invariant there.
            if (p.check_correctness && es.result != ref) {
                std::fprintf(stderr, "  MISMATCH (safety=%s): ember=%lld g++-O2=%lld\n",
                             mode, (long long)es.result, (long long)ref);
                // record the mismatch but keep timing (the mismatch itself is data)
            }
            cells.push_back({p.name, mode, "ember", es, m->compile_ns, m->code_bytes, m->ir_instr_count});
            Stats bs = time_baseline(bfn, p.inner_n, p.iters, p.warmup);
            cells.push_back({p.name, mode, "gcc_O2", bs});

            std::printf("  safety=%-3s  ember median=%9.1f ns  g++-O2 median=%9.1f ns  ratio=%6.2f  [%s]\n",
                        mode, es.median, bs.median, es.median / bs.median,
                        verdict(es.median / bs.median));
        }
    }
    FreeLibrary(hBaseline);

    if (cells.empty()) { std::fprintf(stderr, "\nbench: FAIL (no cells measured)\n"); return 1; }

    // ---- write CSV (cwd-relative; run_bench.sh cds into bench/) ----
    FILE* f = std::fopen("results_codegen_paths.csv", "w");
    if (!f) std::fprintf(stderr, "WARN: could not open results_codegen_paths.csv for write\n");
    if (f) {
        std::fprintf(f, "path,safety,engine,iters,warmup,min_ns,median_ns,mean_ns,p99_ns,stddev_ns,cv_pct,result,compile_ns,code_bytes,ir_instrs,note\n");
        // notes map per path (one note per path; safety/engine share it)
        std::unordered_map<std::string, const char*> notes;
        for (auto& p : paths) notes[p.name] = p.note;
        for (auto& c : cells) {
            const char* note = notes.count(c.path) ? notes[c.path] : "";
            // iters/warmup: we don't carry them per-cell here; record the path's
            // (the harness used p.iters/p.warmup for both). Find the path.
            int iters = 0, warmup = 0;
            for (auto& p : paths) if (strcmp(p.name, c.path) == 0) { iters = p.iters; warmup = p.warmup; break; }
            std::fprintf(f, "%s,%s,%s,%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%lld,%.0f,%zu,%zu,%s\n",
                c.path, c.safety, c.engine, iters, warmup,
                c.st.min, c.st.median, c.st.mean, c.st.p99, c.st.stddev, c.st.cv,
                (long long)c.st.result, c.compile_ns, c.code_bytes, c.ir_instrs, note);
        }
        std::fclose(f);
        std::printf("\nwrote results_codegen_paths.csv\n");
    }

    // ---- write markdown ----
    f = std::fopen("results_codegen_paths.md", "w");
    if (!f) std::fprintf(stderr, "WARN: could not open results_codegen_paths.md for write\n");
    if (f) {
        std::fprintf(f, "# ember per-path codegen bench (prototype) — results\n\n");
        std::fprintf(f, "Machine: %s %s, %s-bit. Date: %s %s.\n",
            kCc, kCcVer, "64", __DATE__, __TIME__);
        std::fprintf(f, "Baseline: g++ -O2 -std=c++17 (compiled from source this run).\n");
        // Record the Stage-1 opts mode so the results MD is self-describing.
        if (const char* o = std::getenv("EMBER_STAGE1_OPTS"))
            std::fprintf(f, "Stage1 opts: %s (docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4).\n", o);
        else
            std::fprintf(f, "Stage1 opts: none (flags off — byte-identical baseline).\n");
        std::fprintf(f, "Headline = **median ns** (resistant to scheduler outliers). ");
        std::fprintf(f, "ratio = ember/g++-O2 (median). >1 = ember slower than an optimizing native compiler.\n\n");
        std::fprintf(f, "| path | safety | ember med ns | g++-O2 med ns | ember/g++-O2 | verdict |\n");
        std::fprintf(f, "|---|---|---|---|---|---|\n");
        for (size_t i = 0; i + 1 < cells.size(); i += 2) {
            // cells come in pairs: ember then gcc_O2 (same path+safety)
            // (defensive: only pair when path+safety match)
            auto& e = cells[i]; auto& g = cells[i+1];
            if (strcmp(e.path, g.path) != 0 || strcmp(e.safety, g.safety) != 0
                || strcmp(e.engine, "ember") != 0 || strcmp(g.engine, "gcc_O2") != 0) {
                continue;
            }
            double r = e.st.median / g.st.median;
            const char* v = e.st.trapped ? "TRAP" : verdict(r);
            std::fprintf(f, "| %s | %s | %.1f | %.1f | %.2f | %s |\n",
                e.path, e.safety, e.st.median, g.st.median, r, v);
        }
        // safety-on overhead table
        std::fprintf(f, "\n## safety-on overhead (guard cost = safety_on - safety_off)\n\n");
        std::fprintf(f, "| path | safety-off med ns | safety-on med ns | guard overhead (abs / %%) |\n");
        std::fprintf(f, "|---|---|---|---|\n");
        for (auto& p : paths) {
            double off = 0, on = 0; bool have_off = false, have_on = false;
            for (auto& c : cells) {
                if (strcmp(c.path, p.name) != 0 || strcmp(c.engine, "ember") != 0) continue;
                if (strcmp(c.safety, "off") == 0) { off = c.st.median; have_off = true; }
                if (strcmp(c.safety, "on") == 0)  { on  = c.st.median; have_on = true; }
            }
            if (have_off && have_on) {
                double pct = off > 0 ? (on - off) / off * 100.0 : 0;
                std::fprintf(f, "| %s | %.1f | %.1f | %+.1f ns / %+.1f%% |\n",
                    p.name, off, on, on - off, pct);
            }
        }
        std::fclose(f);
        std::printf("wrote results_codegen_paths.md\n");
    }

    // ---- stdout: slowest-3 paths vs g++ -O2 (the headline evidence) ----
    std::printf("\n=== slowest paths vs g++ -O2 (safety-off median ratio) ===\n");
    std::vector<std::pair<double, std::string>> ranks;
    for (size_t i = 0; i + 1 < cells.size(); i += 2) {
        auto& e = cells[i]; auto& g = cells[i+1];
        if (strcmp(e.path, g.path) != 0 || strcmp(e.safety, g.safety) != 0
            || strcmp(e.engine, "ember") != 0 || strcmp(g.engine, "gcc_O2") != 0
            || strcmp(e.safety, "off") != 0 || e.st.trapped) continue;
        double r = e.st.median / g.st.median;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%-18s ratio=%6.2f  ember=%8.1fns  g++-O2=%8.1fns  [%s]",
            e.path, r, e.st.median, g.st.median, verdict(r));
        ranks.push_back({r, std::string(buf)});
    }
    std::sort(ranks.begin(), ranks.end(),
              [](auto& a, auto& b){ return a.first > b.first; });
    for (auto& r : ranks) std::printf("  %s\n", r.second.c_str());

    std::printf("\nbench: %s\n", any_compile_fail ? "FAIL (compile error)" : "PASS (ran + wrote results)");
    return any_compile_fail ? 1 : 0;
}
