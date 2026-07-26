// coroutine_darwin_test.cpp — Phase 8 (plan_MACOS_ARM64.md): Darwin ARM64
// cooperative coroutine context-switch end-to-end. Proves the hand-written
// AAPCS64 ember_ctx_switch (src/darwin_arm64_ctx_switch.S) + the macOS code
// path in ext_coroutine.cpp make `yield`/`resume` work natively on Apple
// Silicon — WITHOUT ucontext (deprecated/problematic on Apple; plan §8).
//
// Two test layers:
//  (A) DIRECT driver: compile a generator `gen` (yield 1; yield 2; yield 3;
//      return 0), pull the coroutine_start/next/done native fn_ptrs out of the
//      registered native table, and drive the coroutine from C++ — resuming 4
//      times and asserting the yielded values 1, 2, 3 + the final return 0.
//      This is the task's exact generator contract.
//  (B) INTEGRATION: compile a whole program whose `main` drives a coroutine
//      (coroutine_start + a while(!done){next} loop) and returns the summed
//      consumed values — plus a two-coroutine interleaved case — asserting the
//      JIT'd main returns the expected totals. Mirrors the existing
//      tests/lang/valid_coroutine_*.ember scripts.
//
// Build & run (macOS arm64) — wired into CMake as the `coroutine_darwin` test:
//   cmake -G Ninja .. && cmake --build . --target coroutine_darwin_test
//   ./coroutine_darwin_test
#include "../src/thin_emit.hpp"       // emit_arm64
#include "../src/thin_lower.hpp"      // lower_function
#include "../src/thin_ir.hpp"         // ThinFunction
#include "../src/engine.hpp"          // CompiledFn, finalize, free_executable, ember_call_i64
#include "../src/dispatch_table.hpp"  // DispatchTable
#include "../src/context.hpp"         // context_t
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"         // CodeGenCtx, g_globals_for_codegen
#include "../src/globals.hpp"         // GlobalsBlock
#include "../src/jit_memory.hpp"      // alloc_executable
#include "../src/ast.hpp"
#include "../extensions/coroutine/ext_coroutine.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

// ─── test harness (modeled on emit_arm64_test) ───
struct M {
    std::vector<CompiledFn> fns;
    std::unique_ptr<DispatchTable> table;
    std::unordered_map<std::string, int> slots;
    GlobalsBlock gb;
    StructLayoutTable layouts;
    Program prog;
    std::unordered_map<std::string, NativeSig> natives;
    M() : table(std::make_unique<DispatchTable>(0)) {}
    ~M() {
        for (auto& fn : fns) {
            if (fn.exec) free_executable(fn.exec);
            fn.exec = nullptr; fn.entry = nullptr;
        }
    }
};

static int g_fail = 0;
static void ck(bool c, const char* m) {
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// Compile `src` through lex/parse/sema/lower_function/emit_arm64, finalize +
// install every fn in the dispatch table. Registers the coroutine natives
// (coroutine_start/next/done + __ember_coro_yield) and calls coroutine_init so
// the coroutine store has the context + dispatch table. Safety is OFF
// (use_context_reg=false, no budget/depth checks): the coroutine entry does
// not need the context reg, and a raw AAPCS64 call into `main` works. The
// trampoline still calls ember_call_i64 (the thunk installs x19 = ctx, which
// the entry ignores when use_context_reg is off — harmless; x19 is preserved
// across the ctx_switch as a callee-saved reg).
static std::unique_ptr<M> compile(const std::string& src) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<coroutine_darwin_test>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = si - 1; }

    // Register the coroutine natives the scripts use.
    ext_coroutine::register_natives(m->natives);

    OpOverloadTable overloads;
    m->layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;  // no string encryption
    auto sr = sema(m->prog, m->natives, m->slots, 0, &overloads, &m->layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return nullptr;
    }

    m->gb.base = 0;
    g_globals_for_codegen = &m->gb;
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());

    CodeGenCtx ctx;
    ctx.globals_base = 0;
    ctx.dispatch_base = int64_t(m->table->base());
    ctx.natives = &m->natives;
    ctx.script_slots = &m->slots;
    ctx.structs = &m->layouts;
    // Safety OFF for this gate: the coroutine entry/main do not need the
    // context reg, budget, or depth guards. A raw AAPCS64 C call works.
    ctx.use_context_reg = false;
    ctx.emit_budget_checks = false;
    ctx.emit_depth_checks = false;

    for (auto& fn : m->prog.funcs) {
        ThinFunction thf = lower_function(fn, ctx);
        if (thf.blocks.empty()) {
            std::printf("FAIL: lower_function gave empty blocks for %s\n", fn.name.c_str());
            return nullptr;
        }
        CompiledFn cf = emit_arm64(thf, ctx);
        if (cf.bytes.empty()) {
            std::printf("FAIL: emit_arm64 gave empty bytes for %s\n", fn.name.c_str());
            return nullptr;
        }
        if (!finalize(cf)) {
            std::printf("FAIL: alloc_executable for %s\n", fn.name.c_str());
            return nullptr;
        }
        m->table->set(fn.slot, cf.entry);
        m->fns.push_back(std::move(cf));
    }

    // Wire the coroutine store: the context + dispatch table + slot count the
    // coroutine natives call into. The dispatch table holds the JIT'd entries;
    // coroutine_start's resolve_entry(handle) reads slots[handle] from it.
    static context_t ectx;  // static: must outlive the coroutine's suspended state
    ectx.budget_remaining = 100000000;
    ectx.max_call_depth = 512;
    ectx.has_checkpoint = false;
    ectx.reset_for_call();
    ectx.max_call_depth = 512;
    if (!ext_coroutine::coroutine_init(&ectx, m->table->base(),
                                       int64_t(m->prog.funcs.size()))) {
        std::printf("FAIL: coroutine_init returned false\n");
        return nullptr;
    }
    return m;
}

// Call a no-arg script fn returning i64 (main) via a raw AAPCS64 C call.
static int64_t call0(M& m, const std::string& fn) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1;
    using F = int64_t (*)();
    return reinterpret_cast<F>(m.table->get(it->second))();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== Phase 8: Darwin ARM64 coroutines (yield/resume) ===\n\n");

    // ── (A) DIRECT driver: the task's generator contract ──
    // gen() yields 1, 2, 3 then returns 0. We drive it from C++: start, then
    // resume 4 times and assert the values 1, 2, 3, 0 (the final return).
    {
        const char* src =
            "fn gen() -> i64 {\n"
            "    yield 1;\n"
            "    yield 2;\n"
            "    yield 3;\n"
            "    return 0;\n"
            "}\n"
            "fn main() -> i64 { return 0; }\n";  // main unused here (keeps slots sane)
        auto m = compile(src);
        ck(m.get() != nullptr, "[A] generator gen: compiles + coroutine_init ok");
        if (!m) goto part_b;

        // Pull the coroutine native fn_ptrs out of the registered table.
        auto it_start = m->natives.find("coroutine_start");
        auto it_next  = m->natives.find("coroutine_next");
        auto it_done  = m->natives.find("coroutine_done");
        if (it_start == m->natives.end() || it_next == m->natives.end() ||
            it_done == m->natives.end()) {
            ck(false, "[A] coroutine natives registered");
            goto part_b;
        }
        ck(true, "[A] coroutine natives registered");
        using StartFn = int64_t (*)(int64_t, int64_t);
        using NextFn  = int64_t (*)(int64_t);
        using DoneFn  = int64_t (*)(int64_t);
        auto start_fp = reinterpret_cast<StartFn>(it_start->second.fn_ptr);
        auto next_fp  = reinterpret_cast<NextFn>(it_next->second.fn_ptr);
        auto done_fp  = reinterpret_cast<DoneFn>(it_done->second.fn_ptr);

        int gen_slot = m->slots["gen"];
        int64_t co = start_fp(gen_slot, 0);
        char b[160];
        std::snprintf(b, sizeof b, "[A] coroutine_start(gen) returned nonzero handle (%lld)", (long long)co);
        ck(co > 0, b);

        // done must be false before the fn returns.
        int64_t d0 = done_fp(co);
        ck(d0 == 0, "[A] coroutine_done == 0 before first resume completes");

        // Resume 4 times: 1, 2, 3, then 0 (the final return → done).
        int64_t v1 = next_fp(co);
        int64_t v2 = next_fp(co);
        int64_t v3 = next_fp(co);
        int64_t v4 = next_fp(co);
        std::snprintf(b, sizeof b, "[A] resume #1 == 1 (got %lld)", (long long)v1);
        ck(v1 == 1, b);
        std::snprintf(b, sizeof b, "[A] resume #2 == 2 (got %lld)", (long long)v2);
        ck(v2 == 2, b);
        std::snprintf(b, sizeof b, "[A] resume #3 == 3 (got %lld)", (long long)v3);
        ck(v3 == 3, b);
        std::snprintf(b, sizeof b, "[A] resume #4 (final return) == 0 (got %lld)", (long long)v4);
        ck(v4 == 0, b);

        int64_t d1 = done_fp(co);
        std::snprintf(b, sizeof b, "[A] coroutine_done == 1 after final return (got %lld)", (long long)d1);
        ck(d1 == 1, b);

        // After done, coroutine_next keeps returning the final value (no switch).
        int64_t v5 = next_fp(co);
        std::snprintf(b, sizeof b, "[A] resume after done == 0 (got %lld)", (long long)v5);
        ck(v5 == 0, b);

        ext_coroutine::coroutine_reset();
    }

part_b:
    std::printf("\n=== Phase 8: integration (JIT'd main drives coroutines) ===\n\n");

    // ── (B1) main drives a generator via while(!done){next} (sum = 6) ──
    // Mirrors tests/lang/valid_coroutine_done.ember.
    {
        const char* src =
            "fn gen() -> i64 {\n"
            "    yield 1;\n"
            "    yield 2;\n"
            "    yield 3;\n"
            "    return 0;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let c = coroutine_start(&gen, 0);\n"
            "    let mut sum: i64 = 0;\n"
            "    while (!coroutine_done(c)) {\n"
            "        sum = sum + coroutine_next(c);\n"
            "    }\n"
            "    return sum;\n"  // 1+2+3+0 = 6
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[B1] while(!done){next} program: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[B1] gen sum == 6 (got %lld)", (long long)r);
            ck(r == 6, b);
            ext_coroutine::coroutine_reset();
        }
    }

    // ── (B2) arg + loop yields (squares, sum = 54) — mirrors
    //     tests/lang/valid_coroutine_arg.ember ──
    {
        const char* src =
            "fn squares(n: i64) -> i64 {\n"
            "    let mut i: i64 = 1;\n"
            "    while (i <= n) { yield i * i; i = i + 1; }\n"
            "    return -1;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let c = coroutine_start(&squares, 5);\n"
            "    let mut sum: i64 = 0;\n"
            "    while (!coroutine_done(c)) { sum = sum + coroutine_next(c); }\n"
            "    return sum;\n"  // 1+4+9+16+25 + (-1) = 54
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[B2] squares(n) with arg: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[B2] squares(5) sum == 54 (got %lld)", (long long)r);
            ck(r == 54, b);
            ext_coroutine::coroutine_reset();
        }
    }

    // ── (B3) two coroutines interleaved (sum = 66) — mirrors
    //     tests/lang/valid_coroutine_interleaved.ember. Proves each coroutine
    //     has its own suspended stack/frame (independent coro_ctx + stack). ──
    {
        const char* src =
            "fn genA() -> i64 { yield 1; yield 2; yield 3; return 0; }\n"
            "fn genB() -> i64 { yield 10; yield 20; yield 30; return 0; }\n"
            "fn main() -> i64 {\n"
            "    let a = coroutine_start(&genA, 0);\n"
            "    let b = coroutine_start(&genB, 0);\n"
            "    let mut sum: i64 = 0;\n"
            "    sum = sum + coroutine_next(a);  // 1\n"
            "    sum = sum + coroutine_next(b);  // 10\n"
            "    sum = sum + coroutine_next(a);  // 2\n"
            "    sum = sum + coroutine_next(b);  // 20\n"
            "    sum = sum + coroutine_next(a);  // 3\n"
            "    sum = sum + coroutine_next(b);  // 30\n"
            "    sum = sum + coroutine_next(a);  // 0 (done)\n"
            "    sum = sum + coroutine_next(b);  // 0 (done)\n"
            "    return sum;\n"  // 66
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[B3] two interleaved coroutines: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[B3] interleaved A/B sum == 66 (got %lld)", (long long)r);
            ck(r == 66, b);
            ext_coroutine::coroutine_reset();
        }
    }

    // ── (B4) control flow (if/else) inside a coroutine + arg-selected branch
    //     — mirrors tests/lang/valid_coroutine_control_flow.ember ──
    {
        const char* src =
            "fn classify(n: i64) -> i64 {\n"
            "    if (n > 0) { yield 1; } else { yield -1; }\n"
            "    return 0;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let a = coroutine_start(&classify, 5);\n"
            "    let b = coroutine_start(&classify, 0 - 5);\n"
            "    let mut sum: i64 = 0;\n"
            "    sum = sum + coroutine_next(a);  // 1\n"
            "    sum = sum + coroutine_next(a);  // 0 (done)\n"
            "    sum = sum + coroutine_next(b);  // -1\n"
            "    sum = sum + coroutine_next(b);  // 0 (done)\n"
            "    return sum + 2;\n"  // (1+0-1+0)+2 = 2
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[B4] if/else inside coroutine: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[B4] classify control-flow sum == 2 (got %lld)", (long long)r);
            ck(r == 2, b);
            ext_coroutine::coroutine_reset();
        }
    }

    std::printf("\ncoroutine_darwin_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
