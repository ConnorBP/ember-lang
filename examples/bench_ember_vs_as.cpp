// bench_ember_vs_as - v0.6 benchmark: ember (native JIT) vs AngelScript (bytecode interpreter).
//
// docs/planning/DESIGN.md §9's gate. Runs the SAME workload in both, times the hot path (compile
// once, then N iterations), reports the ratio. The categorical difference: ember
// compiles to native x86-64 (no dispatch-decode loop), AngelScript interprets
// bytecode. The spec's claim: baseline native beats a bytecode interpreter by
// 5-50x on tight loops.
//
// Four representative game-logic workloads, each in both ember and AS script:
//   1. fib(32)            — recursion + dispatch calls
//   2. tight_loop(N=1e8)  — straight-line arithmetic (the no-dispatch-decode-loop advantage)
//   3. nested_calls(M)   — call overhead (a->b->c chain, M times)
//   4. mandelbrot runs   — tight nested loops + float arithmetic (a real game-logic shape)
//
// Honesty: the bench MEASURES, it does not assert. PASS = ran + wrote results.
// The ratio is data for the SSA-lite IR decision (§9): if ember is 5-50x faster,
// the tree-walker is adequate (YAGNI wins); if 1-3x or slower on some pattern,
// that identifies where the stack-spilling hurts.
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "engine.hpp"
#include "dispatch_table.hpp"
#include "binding_builder.hpp"
#include "module_registry.hpp"
#include "module_linker.hpp"
#include "safety.hpp"

#include "ext_vec.hpp"
#include "ext_math.hpp"

#include "angelscript.h"

#include <cstdio>
#include <cstdint>
#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

using namespace ember;
using Clock = std::chrono::steady_clock;
static constexpr uint64_t kWorkloadTimeoutMs = 60000;
static double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// ---- ember: compile a source string, return the entry fn pointer ----
struct EmberModule {
    std::vector<CompiledFn> fns;
    std::unique_ptr<DispatchTable> table;  // heap: DispatchTable has no default ctor
    std::unordered_map<std::string, int> slots;
    GlobalsBlock gb;
    std::vector<uint8_t> gb_store;
    Program prog;
    EmberModule() : table(std::make_unique<DispatchTable>(0)) {}
    ~EmberModule() {
        for (auto& fn : fns) {
            if (fn.exec) free_executable(fn.exec);
            fn.exec = nullptr;
            fn.entry = nullptr;
        }
    }
};

static std::unique_ptr<EmberModule> ember_compile(const std::string& src, const std::string& entry) {
    auto m = std::make_unique<EmberModule>();
    auto lr = tokenize(src, "<bench>");
    if (!lr.ok) { std::fprintf(stderr, "ember lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::fprintf(stderr, "ember parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = m->slots[fn.name]; }
    std::unordered_map<std::string, NativeSig> natives; OpOverloadTable ov;
    ext_vec::register_natives(natives); ext_math::register_natives(natives);
    ext_vec::register_overloads(ov);
    auto layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;
    sema(m->prog, natives, m->slots, 0, &ov, &layouts);
    m->gb_store.assign(m->prog.globals.size() * 8, 0);
    m->gb.base = int64_t(m->gb_store.data());
    g_globals_for_codegen = &m->gb;
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());
    CodeGenCtx ctx;
    ctx.globals_base = m->gb.base;
    ctx.dispatch_base = int64_t(m->table->base());
    ctx.natives = &natives;
    ctx.script_slots = &m->slots;
    ctx.structs = &layouts;
    // safety OFF for the bench (measuring hot-path JIT cost, not sandboxing).
    for (auto& fn : m->prog.funcs) {
        CompiledFn cf = compile_func(fn, ctx);
        finalize(cf);
        m->table->set(fn.slot, cf.entry);
        m->fns.push_back(std::move(cf));
    }
    (void)entry;
    return m;
}
static int64_t ember_call_void(EmberModule& m, const std::string& fn) {
    void* e = m.table->get(m.slots[fn]);
    using F = int64_t(*)();
    return reinterpret_cast<F>(e)();
}

// ---- AngelScript: compile a script string, return the context ----
struct AsModule {
    asIScriptEngine* engine = nullptr;
    asIScriptModule* mod = nullptr;
    asIScriptContext* ctx = nullptr;
    asIScriptFunction* func = nullptr;
    bool ok = false;
    ~AsModule() {
        if (ctx) ctx->Release();
        if (engine) engine->ShutDownAndRelease();
    }
};
static AsModule as_compile(const std::string& script, const std::string& entry_decl) {
    AsModule m;
    m.engine = asCreateScriptEngine();
    if (!m.engine) { std::fprintf(stderr, "AS: create engine failed\n"); return m; }
    m.engine->SetMessageCallback(asFUNCTION(+[](asSMessageInfo* msg, void*) {
        std::fprintf(stderr, "AS: %s (%d,%d) %s\n", msg->section, msg->row, msg->col, msg->message);
    }), nullptr, asCALL_CDECL);
    m.mod = m.engine->GetModule("bench_mod", asGM_ALWAYS_CREATE);
    if (m.mod->AddScriptSection("s", script.c_str(), script.size()) < 0) return m;
    if (m.mod->Build() < 0) { std::fprintf(stderr, "AS: build failed\n"); return m; }
    m.func = m.mod->GetFunctionByDecl(entry_decl.c_str());
    if (!m.func) { std::fprintf(stderr, "AS: entry '%s' not found\n", entry_decl.c_str()); return m; }
    m.ctx = m.engine->CreateContext();
    m.ok = true;
    return m;
}
static int64_t as_exec(AsModule& m) {
    m.ctx->Prepare(m.func);
    m.ctx->Execute();
    asQWORD r = m.ctx->GetReturnQWord();
    return int64_t(r);
}

// ---- the workloads (ember source | AS source | entry decl) ----
struct Workload {
    const char* name;
    std::string ember_src;
    std::string as_src;
    std::string as_decl;       // AS function declaration for GetFunctionByDecl
    int iters;                 // hot-path iterations to time
    // for ember: call void main() (returns i64, ignored) per iter; for fib: call fib(n)
    bool is_fib;
    int fib_arg;
};

int main() {
    std::printf("=== ember v0.6 benchmark: ember (native JIT) vs AngelScript (bytecode interpreter) ===\n\n");

    std::vector<Workload> ws = {
        {"fib(32)",
            "fn fib(n: i64) -> i64 { if (n <= 1) { return n; } return fib(n-1) + fib(n-2); }\n"
            "fn main() -> i64 { return fib(32); }\n",
            "int64 fib(int64 n) { if (n <= 1) return n; return fib(n-1) + fib(n-2); }\n"
            "int64 main() { return fib(32); }\n",
            "int64 main()", 5, true, 32},
        {"tight_loop (1e8)",
            "fn main() -> i64 { let mut sum: i64 = 0; let mut i: i64 = 0; while (i < 100000000) { sum = sum + i*i; i = i + 1; } return sum; }\n",
            "int64 main() { int64 sum = 0; for (int64 i = 0; i < 100000000; i++) sum += i*i; return sum; }\n",
            "int64 main()", 10, false, 0},
        {"nested_calls (1e7)",
            "fn c(x: i64) -> i64 { return x + 1; }\n"
            "fn b(x: i64) -> i64 { return c(x) + c(x); }\n"
            "fn a(x: i64) -> i64 { return b(x) + b(x); }\n"
            "fn main() -> i64 { let mut sum: i64 = 0; let mut i: i64 = 0; while (i < 10000000) { sum = sum + a(i); i = i + 1; } return sum; }\n",
            "int64 c(int64 x) { return x + 1; }\n"
            "int64 b(int64 x) { return c(x) + c(x); }\n"
            "int64 a(int64 x) { return b(x) + b(x); }\n"
            "int64 main() { int64 sum = 0; for (int64 i = 0; i < 10000000; i++) sum += a(i); return sum; }\n",
            "int64 main()", 5, false, 0},
        {"mandelbrot (200x200, iters=50)",
            "fn main() -> i64 { let mut count: i64 = 0; let mut py: f32 = 0.0f; while (py < 200.0f) { let mut px: f32 = 0.0f; while (px < 200.0f) { let cx: f32 = (px - 100.0f) / 100.0f * 2.5f; let cy: f32 = (py - 100.0f) / 100.0f * 2.0f; let mut zx: f32 = 0.0f; let mut zy: f32 = 0.0f; let mut i: i64 = 0; while (i < 50) { let zx2: f32 = zx*zx; let zy2: f32 = zy*zy; if (zx2 + zy2 > 4.0f) { break; } let nz: f32 = zx2 - zy2 + cx; zy = 2.0f*zx*zy + cy; zx = nz; i = i + 1; } if (i == 50) { count = count + 1; } px = px + 1.0f; } py = py + 1.0f; } return count; }\n",
            "int64 main() { int64 count = 0; for (float py = 0; py < 200; py++) for (float px = 0; px < 200; px++) { float cx = (px-100)/100*2.5f; float cy = (py-100)/100*2.0f; float zx=0, zy=0; int64 i=0; for (; i < 50; i++) { float zx2=zx*zx, zy2=zy*zy; if (zx2+zy2 > 4) break; float nz = zx2-zy2+cx; zy = 2*zx*zy+cy; zx = nz; } if (i==50) count++; } return count; }\n",
            "int64 main()", 5, false, 0},
    };

    std::printf("%-32s %12s %12s %10s   %s\n", "workload", "ember ms", "AS ms", "ember/AS", "verdict");
    std::printf(std::string(80, '-') .c_str());
    std::printf("\n");

    bool all_ok = true;
    std::string report = "# ember v0.6 benchmark results\n\n";
    report += "ember (native JIT, tree-walking stack-spilling codegen) vs AngelScript (bytecode interpreter).\n";
    report += "Hot-path: compile once, then N iterations timed. ember/AS ratio < 1 = ember faster.\n\n";
    report += "| workload | ember ms/iter | AS ms/iter | ember/AS | verdict |\n";
    report += "|---|---|---|---|---|\n";

    for (auto& w : ws) {
        const safety::TimePoint workload_start = safety::now();
        bool workload_timed_out = false;
        auto check_workload_safety = [&]() {
            safety::check_memory_limit();
            if (!safety::deadline_expired(workload_start, kWorkloadTimeoutMs)) return true;
            if (!workload_timed_out)
                std::fprintf(stderr, "SAFETY: workload '%s' exceeded 60-second deadline; aborting workload\n", w.name);
            workload_timed_out = true;
            return false;
        };

        // compile both
        if (!check_workload_safety()) { all_ok = false; continue; }
        auto em = ember_compile(w.ember_src, "main");
        if (!em || em->fns.empty()) { std::fprintf(stderr, "ember compile failed for %s\n", w.name); all_ok = false; continue; }
        AsModule as = as_compile(w.as_src, w.as_decl);
        if (!as.ok) { std::fprintf(stderr, "AS compile failed for %s\n", w.name); all_ok = false; continue; }

        // warm + verify correctness (ember and AS should agree on the result)
        if (!check_workload_safety()) { all_ok = false; continue; }
        int64_t e0 = ember_call_void(*em, "main");  // main() calls fib(n) internally, returns result
        if (!check_workload_safety()) { all_ok = false; continue; }
        int64_t a0 = as_exec(as);  // main() calls fib(n) internally
        if (!check_workload_safety()) { all_ok = false; continue; }
        if (e0 != a0) {
            std::printf("%-32s  MISMATCH (ember=%lld AS=%lld)\n", w.name, (long long)e0, (long long)a0);
            report += "| " + std::string(w.name) + " | MISMATCH | | | ember=" + std::to_string(e0) + " AS=" + std::to_string(a0) + " |\n";
            all_ok = false;
            continue;
        }

        // time the hot path: N iterations
        auto t0 = Clock::now();
        int ember_iters = 0;
        for (int i = 0; i < w.iters; ++i) {
            if (!check_workload_safety()) break;
            ember_call_void(*em, "main");
            ++ember_iters;
            if (!check_workload_safety()) break;
        }
        if (workload_timed_out) { all_ok = false; continue; }
        double em_ms = ms_since(t0) / ember_iters;

        t0 = Clock::now();
        int as_iters = 0;
        for (int i = 0; i < w.iters; ++i) {
            if (!check_workload_safety()) break;
            as_exec(as);
            ++as_iters;
            if (!check_workload_safety()) break;
        }
        if (workload_timed_out) { all_ok = false; continue; }
        double as_ms = ms_since(t0) / as_iters;

        double ratio = em_ms / as_ms;
        const char* verdict = (ratio < 0.2) ? "ember >>AS (native wins big)"
                           : (ratio < 1.0) ? "ember faster"
                           : (ratio < 3.0) ? "ember ~AS (tree-walker cost visible)"
                           : "ember SLOWER (SSA-IR refactor warranted)";
        std::printf("%-32s %12.2f %12.2f %10.2f   %s\n", w.name, em_ms, as_ms, ratio, verdict);
        char buf[512];
        std::snprintf(buf, sizeof(buf), "| %s | %.2f | %.2f | %.2f | %s |\n",
                     w.name, em_ms, as_ms, ratio, verdict);
        report += buf;
    }

    std::printf("\n");
    // write the report
    FILE* f = std::fopen("v0.6_BENCHMARK_RESULTS.md", "w");
    if (f) { std::fputs(report.c_str(), f); std::fclose(f); std::printf("wrote v0.6_BENCHMARK_RESULTS.md\n"); }
    std::printf("\nbench: %s\n", all_ok ? "PASS" : "FAIL");
    return all_ok ? 0 : 1;
}
