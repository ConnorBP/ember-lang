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
// Output files are deterministic on `--passes`: a `--passes` run writes
// results_codegen_paths_passes.csv/.md; the no-passes run writes the original
// results_codegen_paths.csv/.md. The two never clobber each other (see
// bench/bench_output_names.hpp + examples/bench_output_names_test.cpp).
//
// 10 paths: 6 original category paths (int_div, call_overhead, loop_overhead,
// slice_bounds, string_decrypt, struct_by_value) + 4 IR-pass workloads
// (cse_redundant, dce_dead_store, constprop_fold, licm_invariant). The 3
// Stage A IR-backend gap paths (slice_bounds, string_decrypt, struct_by_value)
// have ir_safe=false so --passes skips them; the 7 scalar/CF paths have
// ir_safe=true. Adding a path = one PathBench struct + one extern "C" fn in
// baseline_paths.cpp (see the design doc §5.1).

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
#include "safety.hpp"          // RSS ceiling + per-path wall-clock deadline
#include "ext_opt.hpp"         // Stage C: register_passes
#include "bench_output_names.hpp"  // deterministic results-artifact naming (--passes/no-passes)

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
#include <random>

using namespace ember;

static constexpr uint64_t kBenchPathTimeoutMs = 60000;
static constexpr int64_t kBenchInstructionBudget = 10000000000LL;

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
    ~BenchModule() {
        for (auto& fn : fns) {
            if (fn.exec) free_executable(fn.exec);
            fn.exec = nullptr;
            fn.entry = nullptr;
        }
    }
};

static std::unique_ptr<BenchModule> ember_compile(const std::string& src, bool safety_on, uint8_t string_xor, bool ir_safe = true, bool passes_on = false) {
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
    // Stage C: --passes flag (bench harness). Forces the IR backend + the
    // standard 7-pass optimization pipeline + Stage 3 linear-scan regalloc —
    // the exact configuration the CLI's `--passes` enables (examples/ember_cli.cpp:
    // enable_ir_backend + enable_regalloc + pass_manager). Only for ir_safe
    // paths; slices/strings/structs are Stage A IR-backend gaps (thin_lower does
    // not lower them), so the tree-walker stays the correctness path there and
    // the harness reports nopass only for those paths.
    //
    // Pipeline (docs/audit/FINAL_SPEED_AUDIT_2026-07-11.md §5):
    //   constprop,forward,copyprop,instcombine,dce,licm,dse
    // (value-preserving: validation returns 177 with and without.)
    //
    // NOTE: the EMBER_IR_PASS env-var path below does NOT enable regalloc —
    // that gap is exactly what --passes closes (the audit's TODO #1).
    EmberPassRegistry pass_reg;
    ext_opt::register_passes(pass_reg);
    EmberPassManager pass_pm;
    if (passes_on && ir_safe) {
        ctx.enable_ir_backend = true;
        ctx.enable_regalloc   = true;
        static constexpr const char* kPassPipeline =
            "constprop,forward,copyprop,instcombine,dce,licm,dse";
        std::string pass_err;
        if (build_pipeline_from_string(kPassPipeline, pass_reg, pass_pm, &pass_err)) {
            ctx.pass_manager = &pass_pm;
        } else {
            std::fprintf(stderr, "  --passes pipeline: %s\n", pass_err.c_str());
        }
    } else if (ctx.enable_ir_backend) {
        // EMBER_IR_PASS env var (the existing ad-hoc knob; only when the IR
        // backend is already on via EMBER_IR_BACKEND). Unchanged behavior —
        // this path does NOT enable regalloc (the gap --passes closes).
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
        // safety-on: budget + depth + trap. The 10B budget is deliberately
        // large enough to measure guard cost without false traps, but finite
        // so malformed benchmark code cannot execute literally forever.
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
struct Stats {
    double min, median, mean, p99, stddev, cv;
    bool trapped;
    bool timed_out;
    int64_t result;
};

static Stats time_ember(BenchModule& m, context_t& ectx, int iters, int warmup,
                        safety::TimePoint path_start) {
    Stats s{}; s.trapped = false; s.timed_out = false;
    auto sit = m.slots.find("main");
    if (sit == m.slots.end()) { s.trapped = true; return s; }
    void* entry = m.table.get(sit->second);
    if (!entry) { s.trapped = true; return s; }

    // warmup (untimed), each under a fresh checkpoint
    for (int w = 0; w < warmup; ++w) {
        safety::check_memory_limit();
        if (safety::deadline_expired(path_start, kBenchPathTimeoutMs)) {
            s.timed_out = true;
            return s;
        }
        // Reclaim the append-only string host store between independent bench
        // iterations. g_strings (extensions/string/ext_string.cpp) only ever
        // push_back, never evict, so the string_decrypt path's per-iteration
        // string_length("literal") -> string_from_slice -> str_new would
        // accumulate iters*inner_n host strings without bound and trip the
        // 2 GiB RSS failsafe (src/safety.cpp). Each bench iteration is an
        // independent main() call whose strings are dead once it returns, so
        // clearing here is safe (no live handle survives the call) and keeps
        // the measured per-iteration cost identical (each iteration still does
        // the same allocations; only the cumulative growth is bounded). A
        // no-op for paths that never allocate strings (the store stays empty).
        ext_string::reset();
        ectx.reset_for_call();
        ectx.budget_remaining = kBenchInstructionBudget;
        ectx.has_checkpoint = true;
        if (__builtin_setjmp(ectx.checkpoint)) { s.trapped = true; return s; }
        (void)ember_call_void(entry, &ectx);
    }
    ectx.has_checkpoint = false;

    // timed iters, each under its OWN fresh checkpoint (a trap in one iter
    // stops the bench for this cell cleanly, not the process)
    std::vector<double> ns; ns.reserve(size_t(iters));
    for (int it = 0; it < iters; ++it) {
        safety::check_memory_limit();
        if (safety::deadline_expired(path_start, kBenchPathTimeoutMs)) {
            s.timed_out = true;
            return s;
        }
        ext_string::reset();  // see warmup loop: bound the append-only string store
        ectx.reset_for_call();
        ectx.budget_remaining = kBenchInstructionBudget;
        ectx.has_checkpoint = true;
        if (__builtin_setjmp(ectx.checkpoint)) { s.trapped = true; return s; }
        auto t0 = std::chrono::steady_clock::now();
        s.result = ember_call_void(entry, &ectx);
        auto t1 = std::chrono::steady_clock::now();
        ectx.has_checkpoint = false;
        ns.push_back(double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    if (ns.empty()) { s.timed_out = true; return s; }
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

static Stats time_baseline(BaselineFn fn, int64_t N, int iters, int warmup,
                           safety::TimePoint path_start) {
    Stats s{}; s.trapped = false; s.timed_out = false;
    // warmup
    for (int w = 0; w < warmup; ++w) {
        safety::check_memory_limit();
        if (safety::deadline_expired(path_start, kBenchPathTimeoutMs)) {
            s.timed_out = true;
            return s;
        }
        g_baseline_N = N;
        s.result = fn(g_baseline_N);
    }
    std::vector<double> ns; ns.reserve(size_t(iters));
    for (int it = 0; it < iters; ++it) {
        safety::check_memory_limit();
        if (safety::deadline_expired(path_start, kBenchPathTimeoutMs)) {
            s.timed_out = true;
            return s;
        }
        auto t0 = std::chrono::steady_clock::now();
        g_baseline_N = N; s.result = fn(g_baseline_N);
        auto t1 = std::chrono::steady_clock::now();
        ns.push_back(double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    if (ns.empty()) { s.timed_out = true; return s; }
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

// ---- paired/interleaved comparison (kills shared-mode noise) ----
// Alternates ember/baseline per iteration, collects per-pair ratios.
// Returns the median of the paired ratios + a bootstrap 95% CI.
struct PairedStats {
    double median_ratio;     // median of per-pair (ember_ns / baseline_ns)
    double ci_lo, ci_hi;     // bootstrap 95% CI on the median ratio
    double mean_ratio;       // mean of per-pair ratios
    int n_pairs;             // number of valid pairs (baseline_ns > 0)
    bool timed_out;
};

static PairedStats time_paired(BenchModule& m, context_t& ectx, void* entry,
                               BaselineFn bfn, int64_t N,
                               int iters, int warmup, safety::TimePoint path_start) {
    PairedStats ps{}; ps.median_ratio = 0; ps.ci_lo = 0; ps.ci_hi = 0;
    ps.mean_ratio = 0; ps.n_pairs = 0; ps.timed_out = false;
    // warmup both
    for (int w = 0; w < warmup; ++w) {
        safety::check_memory_limit();
        if (safety::deadline_expired(path_start, kBenchPathTimeoutMs)) {
            ps.timed_out = true;
            return ps;
        }
        ext_string::reset();  // bound the append-only string store (see time_ember)
        ectx.reset_for_call(); ectx.budget_remaining = kBenchInstructionBudget;
        ectx.has_checkpoint = true;
        if (__builtin_setjmp(ectx.checkpoint)) return ps;  // trap
        (void)ember_call_void(entry, &ectx);
        ectx.has_checkpoint = false;
        g_baseline_N = N; (void)bfn(g_baseline_N);
    }
    // interleaved timed iters
    std::vector<double> ratios; ratios.reserve(size_t(iters));
    for (int it = 0; it < iters; ++it) {
        safety::check_memory_limit();
        if (safety::deadline_expired(path_start, kBenchPathTimeoutMs)) {
            ps.timed_out = true;
            return ps;
        }
        // ember
        ext_string::reset();  // bound the append-only string store (see time_ember)
        ectx.reset_for_call(); ectx.budget_remaining = kBenchInstructionBudget;
        ectx.has_checkpoint = true;
        if (__builtin_setjmp(ectx.checkpoint)) return ps;
        auto te0 = std::chrono::steady_clock::now();
        (void)ember_call_void(entry, &ectx);
        auto te1 = std::chrono::steady_clock::now();
        ectx.has_checkpoint = false;
        double ens = double(std::chrono::duration_cast<std::chrono::nanoseconds>(te1 - te0).count());
        // baseline (immediately after — shared system state)
        auto tb0 = std::chrono::steady_clock::now();
        g_baseline_N = N; (void)bfn(g_baseline_N);
        auto tb1 = std::chrono::steady_clock::now();
        double bns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(tb1 - tb0).count());
        if (bns > 0) ratios.push_back(ens / bns);
    }
    if (ratios.empty()) return ps;
    ps.n_pairs = int(ratios.size());
    // median + mean of ratios
    std::vector<double> sorted = ratios;
    std::sort(sorted.begin(), sorted.end());
    ps.median_ratio = sorted[sorted.size() / 2];
    ps.mean_ratio = 0; for (double r : ratios) ps.mean_ratio += r;
    ps.mean_ratio /= double(ratios.size());
    // bootstrap 95% CI on the median (B=1000 resamples)
    // Simple bootstrap: resample with replacement, compute median of each
    // resample, take the 2.5th and 97.5th percentiles of the medians.
    std::vector<double> boot_medians;
    boot_medians.reserve(1000);
    std::mt19937 rng(12345);  // fixed seed for reproducibility
    for (int b = 0; b < 1000; ++b) {
        std::vector<double> resample;
        resample.reserve(ratios.size());
        std::uniform_int_distribution<int> dist(0, int(ratios.size()) - 1);
        for (size_t i = 0; i < ratios.size(); ++i)
            resample.push_back(ratios[size_t(dist(rng))]);
        std::sort(resample.begin(), resample.end());
        boot_medians.push_back(resample[resample.size() / 2]);
    }
    std::sort(boot_medians.begin(), boot_medians.end());
    ps.ci_lo = boot_medians[size_t(double(boot_medians.size()) * 0.025)];
    ps.ci_hi = boot_medians[size_t(double(boot_medians.size()) * 0.975)];
    return ps;
}
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

// ---- results storage (path × safety × engine × config) ----
// `config` is "nopass" (tree-walker, the default) or "passes" (IR backend +
// 7-pass pipeline + Stage 3 regalloc, only for ir_safe paths). Baseline (g++-O2)
// cells carry config "baseline". Lookups use find_cell (no index-pairing —
// passes mode emits 2 ember cells + 1 baseline per (path,safety)).
struct Cell {
    const char* path; const char* safety; const char* engine; const char* config;
    Stats st;
    double compile_ns = 0; size_t code_bytes = 0; size_t ir_instrs = 0;
};

static const Cell* find_cell(const std::vector<Cell>& cells,
                             const char* path, const char* safety,
                             const char* engine, const char* config) {
    for (const auto& c : cells)
        if (strcmp(c.path, path) == 0 && strcmp(c.safety, safety) == 0
            && strcmp(c.engine, engine) == 0 && strcmp(c.config, config) == 0)
            return &c;
    return nullptr;
}

int main(int argc, char** argv) {
    // ---- parse args: --passes flag + first positional = baseline DLL path ----
    // --passes: also run each ir_safe path through the IR backend + the 7-pass
    // pipeline (constprop,forward,copyprop,instcombine,dce,licm,dse) + Stage 3
    // regalloc, and report BOTH nopass + passes numbers per path (the audit's
    // TODO #1: docs/audit/FINAL_SPEED_AUDIT_2026-07-11.md §5/§8).
    bool passes_flag = false;
    bool selftest_flag = false;
    const char* dll = nullptr;
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "--passes") { passes_flag = true; continue; }
        if (a == "--selftest") { selftest_flag = true; continue; }
        if (a == "--help" || a == "-h") {
            std::printf("usage: bench_codegen_paths [baseline_dll] [--passes] [--selftest]\n"
                        "  --passes    also run each ir_safe path through the IR backend +\n"
                        "              7-pass pipeline (constprop,forward,copyprop,instcombine,\n"
                        "              dce,licm,dse) + Stage 3 regalloc; report nopass vs passes.\n"
                        "  --selftest  assert make_paths() IR eligibility (ir_safe flags) without\n"
                        "              loading the baseline DLL; exit 0 if the 3 Stage A IR-backend\n"
                        "              gap paths are ir_safe=false and the 7 scalar/CF paths are\n"
                        "              ir_safe=true, else exit 1. (F-irsafe-contradiction pin.)\n");
            return 0;
        }
        if (a.rfind("--", 0) == 0) continue;   // ignore unknown flags
        if (!dll) dll = argv[i];                // first positional = DLL path
    }
    if (!dll) dll = "bench/baseline_paths.dll";

    // ---- --selftest: fast IR-eligibility assertion (no DLL load, no timing) ----
    // Pins the F-irsafe-contradiction contract: the 3 Stage A IR-backend gap
    // paths (slice_bounds, string_decrypt, struct_by_value) must have
    // ir_safe=false so --passes skips them; the 7 scalar/CF paths must have
    // ir_safe=true so --passes runs them. Runs WITHOUT LoadLibraryA so it is a
    // fast focused CTest (sub-second) independent of the g++ -O2 baseline DLL.
    if (selftest_flag) {
        auto sp = make_paths();
        int fail = 0;
        // is this name one of the 3 Stage A IR-backend gap paths?
        auto is_gap = [](const char* n) {
            return strcmp(n, "slice_bounds") == 0
                || strcmp(n, "string_decrypt") == 0
                || strcmp(n, "struct_by_value") == 0;
        };
        for (const auto& p : sp) {
            if (is_gap(p.name) && p.ir_safe) {
                std::fprintf(stderr, "SELFTEST FAIL: '%s' ir_safe=true but is a Stage A IR-backend gap (expected false)\n", p.name);
                ++fail;
            }
            if (!is_gap(p.name) && !p.ir_safe) {
                std::fprintf(stderr, "SELFTEST FAIL: '%s' ir_safe=false but is a scalar/CF path (expected true)\n", p.name);
                ++fail;
            }
        }
        if (sp.size() != 10) {
            std::fprintf(stderr, "SELFTEST FAIL: expected 10 paths, got %zu\n", sp.size());
            ++fail;
        }
        if (fail == 0) {
            std::printf("SELFTEST PASS: 10 paths; 3 IR-backend gaps (slice_bounds, string_decrypt, struct_by_value) ir_safe=false; 7 scalar/CF paths ir_safe=true\n");
            return 0;
        }
        std::fprintf(stderr, "SELFTEST FAIL: %d assertion(s) failed\n", fail);
        return 1;
    }

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
    std::printf("#   passes:    %s\n", passes_flag
        ? "ON — IR backend + constprop,forward,copyprop,instcombine,dce,licm,dse + regalloc"
        : "off (tree-walker baseline; pass --passes to enable)");
    std::printf("#   date:      %s %s\n\n", __DATE__, __TIME__);

    // ---- load the g++ -O2 baseline DLL (compiled at runtime by run_bench.sh) ----
    // The baseline MUST exist (stale-probe discipline: compile-from-source each
    // run). `dll` was resolved by the arg scan above (first positional arg).
    HMODULE hBaseline = LoadLibraryA(dll);
    if (!hBaseline) {
        std::fprintf(stderr, "ERROR: could not load g++ -O2 baseline DLL '%s' (err=%lu).\n"
                     "Build it first: g++ -O2 -std=c++17 -shared baseline_paths.cpp -o baseline_paths.dll\n",
                     dll, GetLastError());
        return 2;
    }
    std::printf("#   baseline: %s (g++ -O2)\n\n", dll);

    auto paths = make_paths();

    // ---- results storage (path × safety × engine × config) ----
    std::vector<Cell> cells;

    bool any_compile_fail = false;

    for (auto& p : paths) {
        std::printf("--- path: %s  [%s]\n", p.name, p.note);
        const safety::TimePoint path_start = safety::now();
        bool path_timed_out = false;
        safety::check_memory_limit();

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
        Stats bref = time_baseline(bfn, p.inner_n, 1, 0, path_start); // reference value
        if (bref.timed_out) {
            std::fprintf(stderr, "  SAFETY: path '%s' exceeded 60-second deadline; skipping\n", p.name);
            continue;
        }
        int64_t ref = bref.result;

        // substitute %N -> inner_n (a source literal) in the ember source
        std::string src = p.ember_src;
        for (size_t pos = src.find("%N"); pos != std::string::npos; pos = src.find("%N", pos + std::to_string(p.inner_n).size())) {
            src.replace(pos, 2, std::to_string(p.inner_n));
        }

        // The configs to run for this path. --passes adds a passes-on config
        // (IR backend + 7-pass pipeline + Stage 3 regalloc) for ir_safe paths.
        // Non-ir_safe paths (slices/strings/structs) are Stage A IR-backend gaps
        // — the tree-walker (nopass) stays the only correctness path there.
        struct RunCfg { const char* label; bool passes_on; };
        std::vector<RunCfg> cfgs = {{"nopass", false}};
        if (passes_flag && p.ir_safe) cfgs.push_back({"passes", true});
        if (passes_flag && !p.ir_safe)
            std::printf("  [passes] skipped — ir_safe=false (Stage A IR-backend gap; tree-walker only)\n");

        for (bool safety_on : {false, true}) {
            const char* mode = safety_on ? "on" : "off";
            // time the g++ -O2 baseline once per (path, safety) — it is the
            // same reference for every ember config, so we don't re-time it per
            // config (keeps the passes/nopass comparison against ONE baseline).
            Stats bs = time_baseline(bfn, p.inner_n, p.iters, p.warmup, path_start);
            if (bs.timed_out) {
                std::fprintf(stderr, "  SAFETY: path '%s' exceeded 60-second deadline; skipping remainder\n", p.name);
                path_timed_out = true;
                break;
            }
            cells.push_back({p.name, mode, "gcc_O2", "baseline", bs, 0, 0, 0});

            for (const auto& cfg : cfgs) {
                auto m = ember_compile(src, safety_on, p.string_xor, p.ir_safe, cfg.passes_on);
                if (!m || m->fns.empty()) {
                    std::fprintf(stderr, "  ember compile FAILED (safety=%s cfg=%s)\n", mode, cfg.label);
                    any_compile_fail = true; continue;
                }
                // Stage C: compile-time + code-size + IR-instr-count metrics.
                // ir_instrs is the PRE-pass ThinFunction instr count (the IR
                // backend builds the IR; passes then reduce it before emit). For
                // nopass (tree-walker) ir_instrs stays 0 — no IR is built.
                std::printf("  [compile] safety=%-3s cfg=%-6s compile=%8.0f ns  code=%5zu B  ir_instrs=%zu\n",
                            mode, cfg.label, m->compile_ns, m->code_bytes, m->ir_instr_count);
                context_t ectx;
                ectx.max_call_depth = 8192;       // generous: we measure guard cost, not trap behavior
                ectx.budget_remaining = kBenchInstructionBudget;
                Stats es = time_ember(*m, ectx, p.iters, p.warmup, path_start);
                if (es.timed_out) {
                    std::fprintf(stderr, "  SAFETY: path '%s' exceeded 60-second deadline; skipping remainder\n", p.name);
                    path_timed_out = true;
                    break;
                }
                if (es.trapped) {
                    std::fprintf(stderr, "  ember TRAP (safety=%s cfg=%s): %s\n",
                                 mode, cfg.label, ectx.last_error.c_str());
                    cells.push_back({p.name, mode, "ember", cfg.label, es,
                                     m->compile_ns, m->code_bytes, m->ir_instr_count});
                    continue;
                }
                // correctness check (ember vs g++ -O2 reference) — only for paths
                // where ember and the baseline do the SAME work. string_decrypt's
                // baseline is a non-decrypt reference (the delta IS the finding),
                // so byte-equality is not the invariant there. Run for BOTH
                // configs: the passes are value-preserving (validation 177), so a
                // passes-on mismatch is a real regression signal (recorded, not
                // fatal — the mismatch itself is data).
                if (p.check_correctness && es.result != ref) {
                    std::fprintf(stderr, "  MISMATCH (safety=%s cfg=%s): ember=%lld g++-O2=%lld\n",
                                 mode, cfg.label, (long long)es.result, (long long)ref);
                }
                cells.push_back({p.name, mode, "ember", cfg.label, es,
                                 m->compile_ns, m->code_bytes, m->ir_instr_count});

                std::printf("  safety=%-3s cfg=%-6s ember median=%9.1f ns  g++-O2 median=%9.1f ns  ratio=%6.2f  [%s]\n",
                            mode, cfg.label, es.median, bs.median, es.median / bs.median,
                            verdict(es.median / bs.median));
                // Paired/interleaved comparison + bootstrap 95% CI (kills shared-
                // mode noise — the separate-loop ratio above can drift if system
                // state changes between the ember and baseline timing windows).
                context_t pctx; pctx.max_call_depth = 8192; pctx.budget_remaining = kBenchInstructionBudget;
                PairedStats ps = time_paired(*m, pctx, m->table.get(m->slots["main"]),
                                             bfn, p.inner_n, std::min(p.iters, 200),
                                             std::min(p.warmup, 10), path_start);
                if (ps.timed_out) {
                    std::fprintf(stderr, "  SAFETY: path '%s' exceeded 60-second deadline; skipping remainder\n", p.name);
                    path_timed_out = true;
                    break;
                }
                if (ps.n_pairs > 0) {
                    std::printf("  [paired] safety=%-3s cfg=%-6s paired median ratio=%5.2f  CI=[%.2f, %.2f]  (n=%d)\n",
                                mode, cfg.label, ps.median_ratio, ps.ci_lo, ps.ci_hi, ps.n_pairs);
                }
            }
            if (path_timed_out) break;
        }
    }
    FreeLibrary(hBaseline);

    if (cells.empty()) { std::fprintf(stderr, "\nbench: FAIL (no cells measured)\n"); return 1; }

    // ---- write CSV (cwd-relative; run_bench.sh cds into bench/) ----
    // Filename is deterministic on passes_flag: a manual --passes run writes
    // results_codegen_paths_passes.csv while the CTest-registered no-passes
    // run writes results_codegen_paths.csv — the two never clobber each other
    // (the artifact-clobber fix; see bench/bench_output_names.hpp).
    const std::string csv_path = ember::bench::results_csv_path(passes_flag);
    FILE* f = std::fopen(csv_path.c_str(), "w");
    if (!f) std::fprintf(stderr, "WARN: could not open %s for write\n", csv_path.c_str());
    if (f) {
        std::fprintf(f, "path,safety,engine,config,iters,warmup,min_ns,median_ns,mean_ns,p99_ns,stddev_ns,cv_pct,result,compile_ns,code_bytes,ir_instrs,note\n");
        // notes map per path (one note per path; safety/engine/config share it)
        std::unordered_map<std::string, const char*> notes;
        for (auto& p : paths) notes[p.name] = p.note;
        for (auto& c : cells) {
            const char* note = notes.count(c.path) ? notes[c.path] : "";
            // iters/warmup: we don't carry them per-cell here; record the path's
            // (the harness used p.iters/p.warmup for both). Find the path.
            int iters = 0, warmup = 0;
            for (auto& p : paths) if (strcmp(p.name, c.path) == 0) { iters = p.iters; warmup = p.warmup; break; }
            std::fprintf(f, "%s,%s,%s,%s,%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%lld,%.0f,%zu,%zu,%s\n",
                c.path, c.safety, c.engine, c.config, iters, warmup,
                c.st.min, c.st.median, c.st.mean, c.st.p99, c.st.stddev, c.st.cv,
                (long long)c.st.result, c.compile_ns, c.code_bytes, c.ir_instrs, note);
        }
        std::fclose(f);
        std::printf("\nwrote %s\n", csv_path.c_str());
    }

    // ---- write markdown ----
    // Same passes_flag-keyed naming as the CSV (see bench_output_names.hpp):
    // --passes -> results_codegen_paths_passes.md; no-passes -> the original .md.
    const std::string md_path = ember::bench::results_md_path(passes_flag);
    f = std::fopen(md_path.c_str(), "w");
    if (!f) std::fprintf(stderr, "WARN: could not open %s for write\n", md_path.c_str());
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
        std::fprintf(f, "Passes: %s", passes_flag
            ? "ON — IR backend + constprop,forward,copyprop,instcombine,dce,licm,dse + Stage 3 regalloc\n"
            : "off (tree-walker baseline; pass --passes to enable)\n");
        std::fprintf(f, "Headline = **median ns** (resistant to scheduler outliers). ");
        std::fprintf(f, "ratio = ember/g++-O2 (median). >1 = ember slower than an optimizing native compiler.\n\n");
        // main per-path table — one row per (path, safety, config). nopass is
        // always present; passes rows appear only when --passes ran them.
        std::fprintf(f, "| path | safety | config | ember med ns | g++-O2 med ns | ember/g++-O2 | verdict |\n");
        std::fprintf(f, "|---|---|---|---|---|---|---|\n");
        for (auto& p : paths) {
            for (bool safety_on : {false, true}) {
                const char* mode = safety_on ? "on" : "off";
                const Cell* b = find_cell(cells, p.name, mode, "gcc_O2", "baseline");
                if (!b) continue;
                for (const char* cfg_label : {"nopass", "passes"}) {
                    const Cell* e = find_cell(cells, p.name, mode, "ember", cfg_label);
                    if (!e) continue;   // passes row absent for non-ir_safe paths / no --passes
                    char ratio_buf[32];
                    const char* v;
                    if (e->st.trapped) { std::snprintf(ratio_buf, sizeof(ratio_buf), "TRAP"); v = "TRAP"; }
                    else { std::snprintf(ratio_buf, sizeof(ratio_buf), "%.2f", e->st.median / b->st.median); v = verdict(e->st.median / b->st.median); }
                    std::fprintf(f, "| %s | %s | %s | %.1f | %.1f | %s | %s |\n",
                        p.name, mode, cfg_label, e->st.median, b->st.median, ratio_buf, v);
                }
            }
        }
        // passes impact section — only when --passes ran. The headline new
        // evidence: how much the 7 passes + regalloc close the gap to g++-O2.
        if (passes_flag) {
            std::fprintf(f, "\n## passes impact (nopass vs passes + Stage 3 regalloc, vs g++-O2)\n\n");
            std::fprintf(f, "Pipeline: constprop,forward,copyprop,instcombine,dce,licm,dse + linear-scan regalloc ");
            std::fprintf(f, "(enable_ir_backend + enable_regalloc — mirrors `ember --passes`). ");
            std::fprintf(f, "Only ir_safe paths (slices/strings/structs are Stage A IR-backend gaps).\n");
            std::fprintf(f, "pass speedup = nopass_med / passes_med (>1 = passes made ember faster). ");
            std::fprintf(f, "passes ratio = passes_med / g++-O2_med.\n\n");
            std::fprintf(f, "| path | safety | nopass med ns | passes med ns | pass speedup | g++-O2 med ns | nopass ratio | passes ratio | verdict (passes) |\n");
            std::fprintf(f, "|---|---|---|---|---|---|---|---|---|\n");
            for (auto& p : paths) {
                for (bool safety_on : {false, true}) {
                    const char* mode = safety_on ? "on" : "off";
                    const Cell* b = find_cell(cells, p.name, mode, "gcc_O2", "baseline");
                    const Cell* n = find_cell(cells, p.name, mode, "ember", "nopass");
                    const Cell* s = find_cell(cells, p.name, mode, "ember", "passes");
                    if (!b || !n) continue;
                    double rn = n->st.median / b->st.median;
                    if (!s) {
                        std::fprintf(f, "| %s | %s | %.1f | N/A | N/A | %.1f | %.2f | N/A | IR-backend gap (Stage A) |\n",
                            p.name, mode, n->st.median, b->st.median, rn);
                        continue;
                    }
                    double speedup = s->st.median > 0 ? n->st.median / s->st.median : 0;
                    double rs = s->st.median / b->st.median;
                    std::fprintf(f, "| %s | %s | %.1f | %.1f | %.2fx | %.1f | %.2f | %.2f | %s |\n",
                        p.name, mode, n->st.median, s->st.median, speedup, b->st.median, rn, rs, verdict(rs));
                }
            }
        }
        // safety-on overhead table — per (path, config). nopass is always
        // present; passes rows appear only when --passes ran them, showing the
        // guard cost on the optimized code too.
        std::fprintf(f, "\n## safety-on overhead (guard cost = safety_on - safety_off)\n\n");
        std::fprintf(f, "| path | config | safety-off med ns | safety-on med ns | guard overhead (abs / %%) |\n");
        std::fprintf(f, "|---|---|---|---|---|\n");
        for (auto& p : paths) {
            for (const char* cfg_label : {"nopass", "passes"}) {
                double off = 0, on = 0; bool have_off = false, have_on = false;
                for (auto& c : cells) {
                    if (strcmp(c.path, p.name) != 0 || strcmp(c.engine, "ember") != 0
                        || strcmp(c.config, cfg_label) != 0) continue;
                    if (strcmp(c.safety, "off") == 0) { off = c.st.median; have_off = true; }
                    if (strcmp(c.safety, "on") == 0)  { on  = c.st.median; have_on = true; }
                }
                if (have_off && have_on) {
                    double pct = off > 0 ? (on - off) / off * 100.0 : 0;
                    std::fprintf(f, "| %s | %s | %.1f | %.1f | %+.1f ns / %+.1f%% |\n",
                        p.name, cfg_label, off, on, on - off, pct);
                }
            }
        }
        std::fclose(f);
        std::printf("wrote %s\n", md_path.c_str());
    }

    // ---- stdout: slowest paths vs g++ -O2 (the headline evidence) ----
    // Ranks by safety-off nopass median ratio (the baseline gap). When --passes
    // ran, a second summary shows the passes impact (nopass -> passes speedup).
    std::printf("\n=== slowest paths vs g++ -O2 (safety-off, nopass median ratio) ===\n");
    std::vector<std::pair<double, std::string>> ranks;
    for (auto& p : paths) {
        const Cell* e = find_cell(cells, p.name, "off", "ember", "nopass");
        const Cell* g = find_cell(cells, p.name, "off", "gcc_O2", "baseline");
        if (!e || !g || e->st.trapped) continue;
        double r = e->st.median / g->st.median;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%-18s ratio=%6.2f  ember=%8.1fns  g++-O2=%8.1fns  [%s]",
            p.name, r, e->st.median, g->st.median, verdict(r));
        ranks.push_back({r, std::string(buf)});
    }
    std::sort(ranks.begin(), ranks.end(),
              [](auto& a, auto& b){ return a.first > b.first; });
    for (auto& r : ranks) std::printf("  %s\n", r.second.c_str());

    if (passes_flag) {
        std::printf("\n=== passes impact (nopass -> passes + regalloc, safety-off) ===\n");
        std::printf("  pipeline: constprop,forward,copyprop,instcombine,dce,licm,dse + Stage 3 regalloc\n");
        std::vector<std::pair<double, std::string>> pass_ranks;
        for (auto& p : paths) {
            const Cell* n = find_cell(cells, p.name, "off", "ember", "nopass");
            const Cell* s = find_cell(cells, p.name, "off", "ember", "passes");
            const Cell* g = find_cell(cells, p.name, "off", "gcc_O2", "baseline");
            if (!n || !g) continue;
            char buf[256];
            if (!s) {
                std::snprintf(buf, sizeof(buf),
                    "%-18s nopass=%8.1fns  passes=N/A (IR-backend gap)  g++-O2=%8.1fns",
                    p.name, n->st.median, g->st.median);
                pass_ranks.push_back({-1.0, std::string(buf)});   // gap entries sort last
                continue;
            }
            double speedup = s->st.median > 0 ? n->st.median / s->st.median : 0;
            double rs = s->st.median / g->st.median;
            std::snprintf(buf, sizeof(buf),
                "%-18s nopass=%8.1fns  passes=%8.1fns  speedup=%5.2fx  passes/g++-O2=%5.2f  [%s]",
                p.name, n->st.median, s->st.median, speedup, rs, verdict(rs));
            pass_ranks.push_back({speedup, std::string(buf)});
        }
        // sort by speedup descending (biggest pass win first); gap entries (-1) sink
        std::sort(pass_ranks.begin(), pass_ranks.end(),
                  [](auto& a, auto& b){ return a.first > b.first; });
        for (auto& r : pass_ranks) std::printf("  %s\n", r.second.c_str());
    }

    std::printf("\nbench: %s\n", any_compile_fail ? "FAIL (compile error)" : "PASS (ran + wrote results)");
    return any_compile_fail ? 1 : 0;
}
