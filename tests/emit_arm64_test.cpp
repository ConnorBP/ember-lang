// emit_arm64_test.cpp — Phase 4 (plan_MACOS_ARM64.md): the ARM64 ThinIR emit
// end-to-end gate. Lowers real ember sources to ThinFunction, emits ARM64 via
// emit_arm64, finalizes (alloc_executable → RX), installs a dispatch table, and
// CALLS the JIT'd ARM64 code via a C function pointer — asserting the correct
// i64 return. This is the first-runnable milestone: integer + control-flow +
// scalar native/script calls, frame-only (no regalloc), safety OFF (no x19
// access) so a raw AAPCS64 C call works.
//
// Build & run (macOS arm64):
//   clang++ -std=c++17 -Wall -Wextra -I src tests/emit_arm64_test.cpp \
//     -L buildm -lember -lember_frontend -lember_ed25519 -o /tmp/emit_arm64_test \
//     && /tmp/emit_arm64_test
#include "../src/thin_emit.hpp"       // emit_arm64
#include "../src/thin_lower.hpp"      // lower_function
#include "../src/thin_ir.hpp"         // ThinFunction
#include "../src/engine.hpp"          // CompiledFn, finalize, free_executable
#include "../src/dispatch_table.hpp"  // DispatchTable
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"         // CodeGenCtx, g_globals_for_codegen
#include "../src/globals.hpp"         // GlobalsBlock
#include "../src/jit_memory.hpp"      // alloc_executable
#include "../src/context.hpp"        // context_t, TrapReason, EMBER_SETJMP/LONGJMP
#include "../src/ast.hpp"
#include "../extensions/array/ext_array.hpp"
#include "../extensions/string/ext_string.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

// ─── test harness (modeled on regalloc_test / thin_ir_test) ───
struct M {
    std::vector<CompiledFn> fns;
    std::unique_ptr<DispatchTable> table;
    std::unordered_map<std::string, int> slots;
    GlobalsBlock gb;
    StructLayoutTable layouts;
    Program prog;
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

// A hand-registered i64(i64) native for the CallNative probe.
static int64_t n_dbl(int64_t x) { return x * 2; }
// i64(i64,i64) native for a two-arg native probe.
static int64_t n_addmul(int64_t a, int64_t b) { return a * 3 + b; }
// f64(f64,f64) native for a float native-call probe (AAPCS64 FP args v0/v1,
// return v0).
static double n_add_d(double a, double b) { return a + b; }
// f32(f32,f32) native for a float native-call probe.
static float n_add_f(float a, float b) { return a + b; }

// Compile `src` through lex/parse/sema/lower_function/emit_arm64. Returns the
// module with all fns finalized + installed in the dispatch table. Safety is
// OFF (use_context_reg=false, no budget/depth checks) so the JIT'd code never
// touches x19 and a raw AAPCS64 C call works.
static std::unique_ptr<M> compile(const std::string& src) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<emit_arm64_test>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = si - 1; }

    // natives: register the hand C fns used by the native-call probes +
    // the standard string/array extension natives (string_from_slice,
    // string_length, string_char_at, array_new, etc.) so slice/string tests
    // sema + JIT-link.
    std::unordered_map<std::string, NativeSig> natives;
    {
        NativeSig s1; s1.name = "dbl"; s1.fn_ptr = (void*)&n_dbl;
        s1.ret = type_i64(); s1.params = {type_i64()};
        natives["dbl"] = s1;
        NativeSig s2; s2.name = "addmul"; s2.fn_ptr = (void*)&n_addmul;
        s2.ret = type_i64(); s2.params = {type_i64(), type_i64()};
        natives["addmul"] = s2;
        NativeSig s3; s3.name = "add_d"; s3.fn_ptr = (void*)&n_add_d;
        s3.ret = type_f64(); s3.params = {type_f64(), type_f64()};
        natives["add_d"] = s3;
        NativeSig s4; s4.name = "add_f"; s4.fn_ptr = (void*)&n_add_f;
        s4.ret = type_f32(); s4.params = {type_f32(), type_f32()};
        natives["add_f"] = s4;
    }
    ext_string::register_natives(natives);
    ext_array::register_natives(natives);
    OpOverloadTable overloads;
    ext_string::register_overloads(overloads);
    m->layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;  // no string encryption (ConstStringRef path)
    auto sr = sema(m->prog, natives, m->slots, 0, &overloads, &m->layouts);
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
    ctx.natives = &natives;
    ctx.script_slots = &m->slots;
    ctx.structs = &m->layouts;
    // Safety OFF for the first milestone: no context reg, no budget/depth.
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
    return m;
}

// Like compile() but with string encryption ON (a non-zero XOR key) so string
// literals lower to StringDecrypt instead of ConstStringRef.
static std::unique_ptr<M> compile_enc(const std::string& src) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<emit_arm64_test>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = si - 1; }
    std::unordered_map<std::string, NativeSig> natives;
    ext_string::register_natives(natives);
    ext_array::register_natives(natives);
    OpOverloadTable overloads;
    ext_string::register_overloads(overloads);
    m->layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0x5A;  // encryption ON
    auto sr = sema(m->prog, natives, m->slots, 0, &overloads, &m->layouts);
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
    ctx.natives = &natives;
    ctx.script_slots = &m->slots;
    ctx.structs = &m->layouts;
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
    return m;
}

// Call a no-arg script fn returning i64 (main).
static int64_t call0(M& m, const std::string& fn) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1;
    using F = int64_t (*)();
    return reinterpret_cast<F>(m.table->get(it->second))();
}
// Call a 1-arg i64->i64 script fn.
static int64_t call1(M& m, const std::string& fn, int64_t a) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1;
    using F = int64_t (*)(int64_t);
    return reinterpret_cast<F>(m.table->get(it->second))(a);
}
// Call a 2-arg i64,i64->i64 script fn.
static int64_t call2(M& m, const std::string& fn, int64_t a, int64_t b) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1;
    using F = int64_t (*)(int64_t, int64_t);
    return reinterpret_cast<F>(m.table->get(it->second))(a, b);
}
// Call a no-arg script fn returning f64 (AAPCS64: float return in v0).
[[maybe_unused]] static double call0_f64(M& m, const std::string& fn) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1.0;
    using F = double (*)();
    return reinterpret_cast<F>(m.table->get(it->second))();
}
// Call a 1-arg f64->f64 script fn.
static double call1_f64(M& m, const std::string& fn, double a) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1.0;
    using F = double (*)(double);
    return reinterpret_cast<F>(m.table->get(it->second))(a);
}
// Call a 2-arg f64,f64->f64 script fn.
static double call2_f64(M& m, const std::string& fn, double a, double b) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1.0;
    using F = double (*)(double, double);
    return reinterpret_cast<F>(m.table->get(it->second))(a, b);
}
// Call a 1-arg f64->i64 script fn (float param, int return).
static int64_t call1_f64_i64(M& m, const std::string& fn, double a) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1;
    using F = int64_t (*)(double);
    return reinterpret_cast<F>(m.table->get(it->second))(a);
}
// Call a 2-arg f32,f32->f32 script fn.
static float call2_f32(M& m, const std::string& fn, float a, float b) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1.0f;
    using F = float (*)(float, float);
    return reinterpret_cast<F>(m.table->get(it->second))(a, b);
}
// Call a 1-arg i64->f64 script fn (int param, float return).
static double call1_i64_f64(M& m, const std::string& fn, int64_t a) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1.0;
    using F = double (*)(int64_t);
    return reinterpret_cast<F>(m.table->get(it->second))(a);
}

// ===========================================================================
// Safety-ON compile + run helpers (audit M5/M6). The probes above use safety
// OFF (no x19 access) so a raw AAPCS64 C call works. These helpers compile with
// use_context_reg=true (B1: context_t* in x19) + a trap stub + budget/depth
// checks, then run via ember_call_void (which installs x19) with a host
// setjmp checkpoint — so a budget/depth/throw TRAP is recoverable + observable
// (mirrors examples/thread_safety_test.cpp's ts_trap / run_with_ctx).
// ===========================================================================
extern "C" void arm64_test_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        if (detail) ctx->last_error = detail;
        if (ctx->has_checkpoint) EMBER_LONGJMP(ctx->checkpoint, 1);
    }
    std::abort();
}

// Compile `src` with safety ON (use_context_reg + trap stub + budget/depth
// checks). max_depth controls the per-call depth budget baked into the JIT'd
// code's depth guard.
static std::unique_ptr<M> compile_safe(const std::string& src, int64_t max_depth = 8) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<emit_arm64_test_safe>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = si - 1; }
    std::unordered_map<std::string, NativeSig> natives;
    ext_string::register_natives(natives);
    ext_array::register_natives(natives);
    OpOverloadTable overloads;
    ext_string::register_overloads(overloads);
    m->layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;
    auto sr = sema(m->prog, natives, m->slots, 0, &overloads, &m->layouts);
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
    ctx.natives = &natives;
    ctx.script_slots = &m->slots;
    ctx.structs = &m->layouts;
    // Safety ON: context reg + trap stub + budget/depth checks.
    ctx.use_context_reg = true;
    ctx.trap_stub = (void*)&arm64_test_trap;
    ctx.trap_ctx = nullptr;       // B1: ctx arrives in x19
    ctx.emit_budget_checks = true;
    ctx.emit_depth_checks = true;
    ctx.max_call_depth = int32_t(max_depth);
    for (auto& fn : m->prog.funcs) {
        ThinFunction thf = lower_function(fn, ctx);
        if (thf.blocks.empty()) {
            std::printf("FAIL: lower_function gave empty blocks for %s (nsr='%s')\n",
                        fn.name.c_str(), thf.non_serializable_reason.c_str());
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
    return m;
}

// Run a no-arg i64 entry under a fresh context_t with a checkpoint. Sets
// *trapped=true if the trap stub fired (longjmp). Returns the i64 result.
static int64_t call0_safe(M& m, const std::string& fn, context_t* ectx, bool* trapped) {
    *trapped = false;
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) { return -1; }
    void* entry = m.table->get(it->second);
    ectx->has_checkpoint = true;
    if (EMBER_SETJMP(ectx->checkpoint)) {
        *trapped = true; ectx->has_checkpoint = false; return 0;
    }
    int64_t r = ember_call_void(entry, ectx);
    ectx->has_checkpoint = false;
    return r;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== Phase 4: emit_arm64 end-to-end (ARM64 ThinIR JIT) ===\n\n");

    // ── 1. arithmetic + params (a*3 + b - 7), 2-arg direct call ──
    {
        const char* src =
            "fn f(a: i64, b: i64) -> i64 { return a * 3 + b - 7; }\n"
            "fn main() -> i64 { return f(10, 20); }\n";  // 30+20-7 = 43
        auto m = compile(src);
        ck(m.get() != nullptr, "[1] arithmetic+params: compiles");
        if (m) {
            int64_t r0 = call0(*m, "main");
            int64_t r1 = call2(*m, "f", 10, 20);
            int64_t r2 = call2(*m, "f", -4, 100);  // -12+100-7 = 81
            char b[128];
            std::snprintf(b, sizeof b, "[1] main() == 43 (got %lld)", (long long)r0);
            ck(r0 == 43, b);
            std::snprintf(b, sizeof b, "[1] f(10,20) == 43 (got %lld)", (long long)r1);
            ck(r1 == 43, b);
            std::snprintf(b, sizeof b, "[1] f(-4,100) == 81 (got %lld)", (long long)r2);
            ck(r2 == 81, b);
        }
    }

    // ── 2. loop (while) — sum 0..n-1 ──
    {
        const char* src =
            "fn sum(n: i64) -> i64 {\n"
            "    let mut s: i64 = 0;\n"
            "    let mut i: i64 = 0;\n"
            "    while (i < n) { s = s + i; i = i + 1; }\n"
            "    return s;\n"
            "}\n"
            "fn main() -> i64 { return sum(100); }\n";  // 4950
        auto m = compile(src);
        ck(m.get() != nullptr, "[2] while-loop: compiles");
        if (m) {
            int64_t r0 = call0(*m, "main");
            int64_t r1 = call1(*m, "sum", 10);   // 45
            int64_t r2 = call1(*m, "sum", 0);    // 0
            char b[128];
            std::snprintf(b, sizeof b, "[2] sum(100) == 4950 (got %lld)", (long long)r0);
            ck(r0 == 4950, b);
            std::snprintf(b, sizeof b, "[2] sum(10) == 45 (got %lld)", (long long)r1);
            ck(r1 == 45, b);
            std::snprintf(b, sizeof b, "[2] sum(0) == 0 (got %lld)", (long long)r2);
            ck(r2 == 0, b);
        }
    }

    // ── 3. for-loop + nested arithmetic (sum of squares) ──
    {
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut s: i64 = 0;\n"
            "    for (let mut i: i64 = 0; i < 100; i = i + 1) { s = s + i * i; }\n"
            "    return s;\n"
            "}\n";  // sum i^2, i=0..99 = 328350
        auto m = compile(src);
        ck(m.get() != nullptr, "[3] for-loop sum-of-squares: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[3] sum i^2 (0..99) == 328350 (got %lld)", (long long)r);
            ck(r == 328350, b);
        }
    }

    // ── 4. recursion (fib) — CallScript through the dispatch table ──
    {
        const char* src =
            "fn fib(n: i64) -> i64 { if (n <= 1) { return n; } return fib(n - 1) + fib(n - 2); }\n"
            "fn main() -> i64 { return fib(20); }\n";  // 6765
        auto m = compile(src);
        ck(m.get() != nullptr, "[4] recursion fib: compiles");
        if (m) {
            int64_t r0 = call0(*m, "main");
            int64_t r1 = call1(*m, "fib", 10);  // 55
            char b[128];
            std::snprintf(b, sizeof b, "[4] fib(20) == 6765 (got %lld)", (long long)r0);
            ck(r0 == 6765, b);
            std::snprintf(b, sizeof b, "[4] fib(10) == 55 (got %lld)", (long long)r1);
            ck(r1 == 55, b);
        }
    }

    // ── 5. native scalar call (dbl: i64(i64)) ──
    {
        const char* src =
            "fn g(x: i64) -> i64 { return dbl(x); }\n"
            "fn main() -> i64 { return g(21); }\n";  // 42
        auto m = compile(src);
        ck(m.get() != nullptr, "[5] native call dbl: compiles");
        if (m) {
            int64_t r0 = call0(*m, "main");
            int64_t r1 = call1(*m, "g", -5);  // -10
            char b[128];
            std::snprintf(b, sizeof b, "[5] g(21) == 42 (got %lld)", (long long)r0);
            ck(r0 == 42, b);
            std::snprintf(b, sizeof b, "[5] g(-5) == -10 (got %lld)", (long long)r1);
            ck(r1 == -10, b);
        }
    }

    // ── 6. native call with 2 args + arithmetic (addmul) ──
    {
        const char* src =
            "fn h(a: i64, b: i64) -> i64 { return addmul(a, b) + 1; }\n"
            "fn main() -> i64 { return h(10, 20); }\n";  // 30+20+1 = 51
        auto m = compile(src);
        ck(m.get() != nullptr, "[6] native call addmul (2 args): compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[6] h(10,20) == 51 (got %lld)", (long long)r);
            ck(r == 51, b);
        }
    }

    // ── 7. comparisons + if/else (max) ──
    {
        const char* src =
            "fn max(a: i64, b: i64) -> i64 { if (a > b) { return a; } return b; }\n"
            "fn main() -> i64 { return max(7, 3) + max(3, 7); }\n";  // 7+7 = 14
        auto m = compile(src);
        ck(m.get() != nullptr, "[7] comparisons + if/else (max): compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[7] max(7,3)+max(3,7) == 14 (got %lld)", (long long)r);
            ck(r == 14, b);
        }
    }

    // ── 8. div/mod + bitwise + shifts (int op coverage) ──
    {
        const char* src =
            "fn main() -> i64 {\n"
            "    let a: i64 = 7 / 2;      // 3\n"
            "    let b: i64 = 7 % 3;      // 1\n"
            "    let c: i64 = 1 << 8;     // 256\n"
            "    let d: i64 = 256 >> 2;   // 64\n"
            "    let e: i64 = 0x0F | 0xF0; // 255\n"
            "    let f: i64 = 0xFF & 0x0F; // 15\n"
            "    let g: i64 = 0xFF ^ 0x0F; // 240\n"
            "    return a + b + c + d + e + f + g;  // 834\n"
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[8] div/mod/bitwise/shifts: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[8] int-op soup == 834 (got %lld)", (long long)r);
            ck(r == 834, b);
        }
    }

    // ── 9. negative / not / bitnot ──
    {
        const char* src =
            "fn main() -> i64 {\n"
            "    let a: i64 = -5;\n"
            "    let b: bool = !true;     // false\n"
            "    let c: i64 = ~0;          // -1\n"
            "    if (b) { return 1; }\n"
            "    return a + c;  // -5 + -1 = -6\n"
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[9] neg/not/bitnot: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[9] (-5) + (~0) + !true-branch == -6 (got %lld)", (long long)r);
            ck(r == -6, b);
        }
    }

    // ── 10. hand-built ThinFunction (no lower_function) — proves emit_arm64
    //     works on a directly-constructed IR too. A minimal fn: ret #42. ──
    {
        ThinFunction thf;
        thf.name = "ret42";
        thf.ret_type = &type_i64();
        thf.frame.frame_size = 16;       // round16; fits rbx_save@-8
        thf.frame.rbx_save_offset = -8;
        // one param (n: i64) at -16 so the AAPCS64 spill has a slot (even though
        // unused) — keeps the prologue/param-spill path exercised.
        ThinFramePlan::ParamSpill p;
        p.name = "n"; p.ty = &type_i64(); p.off = -16; p.word0 = 0; p.nwords = 1;
        thf.frame.params.push_back(p);
        // block 0: ConstInt 42 -> vreg 2 @-24? frame_size must fit. Use 32.
        thf.frame.frame_size = 32;
        ThinBlock b0; b0.id = 0;
        ThinInstr c; c.op = ThinOp::ConstInt; c.dst = 2; c.imm.i = 42;
        c.meta.frame_off = -24; c.meta.type = &type_i64(); c.meta.width = 8;
        b0.instrs.push_back(c);
        b0.term.kind = TermKind::Return; b0.term.ret = 2;
        thf.blocks.push_back(b0);

        CodeGenCtx ctx;  // safety off, no dispatch/globals
        CompiledFn cf = emit_arm64(thf, ctx);
        ck(!cf.bytes.empty(), "[10] hand-built ThinFunction: emit_arm64 non-empty");
        if (!cf.bytes.empty()) {
            ck(finalize(cf), "[10] hand-built: finalize (alloc_executable)");
            if (cf.entry) {
                using F = int64_t (*)(int64_t);
                int64_t r = reinterpret_cast<F>(cf.entry)(99);
                char bb[128];
                std::snprintf(bb, sizeof bb, "[10] hand-built ret42() == 42 (got %lld)", (long long)r);
                ck(r == 42, bb);
                free_executable(cf.exec);
            }
        }
    }

    // ── 11. ConstFloat now works (Phase 6a) — hand-built ThinFunction
    //     returns a float constant via v0. ──
    {
        ThinFunction thf;
        thf.name = "retfloat";
        thf.ret_type = &type_f64();
        thf.frame.frame_size = 32;
        thf.frame.rbx_save_offset = -8;
        ThinBlock b0; b0.id = 0;
        ThinInstr c; c.op = ThinOp::ConstFloat; c.dst = 1; c.imm.f = 1.5;
        c.meta.is_f32 = 0; c.meta.frame_off = -16; c.meta.type = &type_f64(); c.meta.width = 8;
        b0.instrs.push_back(c);
        b0.term.kind = TermKind::Return; b0.term.ret = 1;
        thf.blocks.push_back(b0);
        CodeGenCtx ctx;
        CompiledFn cf = emit_arm64(thf, ctx);
        ck(!cf.bytes.empty(), "[11] ConstFloat: emit_arm64 non-empty");
        if (!cf.bytes.empty()) {
            ck(finalize(cf), "[11] ConstFloat: finalize (alloc_executable)");
            if (cf.entry) {
                using F = double (*)();
                double r = reinterpret_cast<F>(cf.entry)();
                char bb[128];
                std::snprintf(bb, sizeof bb, "[11] ConstFloat retfloat() == 1.5 (got %f)", r);
                ck(r == 1.5, bb);
                free_executable(cf.exec);
            }
        }
    }

    // ── 12. f64 arithmetic (add/sub/mul/div) ──
    {
        const char* src =
            "fn fadd(a: f64, b: f64) -> f64 { return a + b; }\n"
            "fn fsub(a: f64, b: f64) -> f64 { return a - b; }\n"
            "fn fmul(a: f64, b: f64) -> f64 { return a * b; }\n"
            "fn fdiv(a: f64, b: f64) -> f64 { return a / b; }\n"
            "fn main() -> i64 {\n"
            "  if (fadd(1.5, 2.5) != 4.0) { return 0; }\n"
            "  if (fsub(10.0, 3.0) != 7.0) { return 0; }\n"
            "  if (fmul(3.0, 4.0) != 12.0) { return 0; }\n"
            "  if (fdiv(20.0, 8.0) != 2.5) { return 0; }\n"
            "  return 42;\n"
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[12] f64 arithmetic: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[12] f64 arithmetic == 42 (got %lld)", (long long)r);
            ck(r == 42, b);
            // spot-check the individual fns
            double r0 = call2_f64(*m, "fadd", 1.5, 2.5);
            double r1 = call2_f64(*m, "fdiv", 20.0, 8.0);
            char b0[128], b1[128];
            std::snprintf(b0, sizeof b0, "[12] fadd(1.5,2.5)==4.0 (got %f)", r0);
            std::snprintf(b1, sizeof b1, "[12] fdiv(20,8)==2.5 (got %f)", r1);
            ck(r0 == 4.0, b0);
            ck(r1 == 2.5, b1);
        }
    }

    // ── 13. f32 arithmetic (add/mul) — the f32 path (meta.is_f32) ──
    {
        const char* src =
            "fn f32add(a: f32, b: f32) -> f32 { return a + b; }\n"
            "fn f32mul(a: f32, b: f32) -> f32 { return a * b; }\n"
            "fn main() -> i64 {\n"
            "  if (f32add(1.25f, 2.5f) != 3.75f) { return 0; }\n"
            "  if (f32mul(3.0f, 4.0f) != 12.0f) { return 0; }\n"
            "  return 42;\n"
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[13] f32 arithmetic: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[13] f32 arithmetic == 42 (got %lld)", (long long)r);
            ck(r == 42, b);
            float r0 = call2_f32(*m, "f32add", 1.25f, 2.5f);
            char b0[128];
            std::snprintf(b0, sizeof b0, "[13] f32add(1.25,2.5)==3.75 (got %f)", r0);
            ck(r0 == 3.75f, b0);
        }
    }

    // ── 14. float compare — all six predicates, incl. < (mi) and <= (ls),
    //     plus a NaN/unordered case. The < / <= must be FALSE on NaN
    //     (advisor-confirmed mi/ls condition mapping). ──
    {
        const char* src =
            "fn flt(a: f64, b: f64) -> bool { return a < b; }\n"
            "fn fle(a: f64, b: f64) -> bool { return a <= b; }\n"
            "fn fgt(a: f64, b: f64) -> bool { return a > b; }\n"
            "fn fge(a: f64, b: f64) -> bool { return a >= b; }\n"
            "fn feq(a: f64, b: f64) -> bool { return a == b; }\n"
            "fn fne(a: f64, b: f64) -> bool { return a != b; }\n"
            "fn main() -> i64 {\n"
            "  if (!(1.0 < 2.0)) { return 1; }     // lt true\n"
            "  if (2.0 < 1.0) { return 2; }        // lt false\n"
            "  if (!(2.0 <= 2.0)) { return 3; }    // le true (equal)\n"
            "  if (!(2.0 > 1.0)) { return 4; }     // gt true\n"
            "  if (!(2.0 >= 2.0)) { return 5; }    // ge true (equal)\n"
            "  if (!(3.0 == 3.0)) { return 6; }    // eq true\n"
            "  if (3.0 != 3.0) { return 7; }       // ne false\n"
            "  if (!(3.0 != 4.0)) { return 8; }       // ne should be true -> this returns 8 FAIL\n"
            "  return 42;\n"
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[14] float compare: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[14] float compare == 42 (got %lld)", (long long)r);
            ck(r == 42, b);
        }
    }

    // ── 14b. float compare < / <= on NaN (the mi/ls condition mapping).
    //     NaN < x and NaN <= x must both be FALSE (mi/ls are false on unordered).
    //     A hand-built probe: compare a NaN constant against 1.0. ──
    {
        const char* src =
            "fn lt_nan(x: f64) -> bool { return x < 1.0; }\n"
            "fn le_nan(x: f64) -> bool { return x <= 1.0; }\n"
            "fn eq_nan(x: f64) -> bool { return x == 1.0; }\n"
            "fn ne_nan(x: f64) -> bool { return x != 1.0; }\n"
            "fn main() -> i64 {\n"
            "  // produce a NaN via 0.0/0.0 (ember has no NaN literal)\n"
            "  let nan: f64 = 0.0 / 0.0;\n"
            "  if (lt_nan(nan)) { return 1; }   // NaN < 1.0 must be FALSE (mi)\n"
            "  if (le_nan(nan)) { return 2; }   // NaN <= 1.0 must be FALSE (ls)\n"
            "  if (eq_nan(nan)) { return 3; }   // NaN == 1.0 must be FALSE (eq)\n"
            "  if (!(ne_nan(nan))) { return 4; } // NaN != 1.0 must be TRUE (ne)\n"
            "  return 42;\n"
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[14b] float compare NaN/unordered: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[14b] float NaN compare == 42 (got %lld)", (long long)r);
            ck(r == 42, b);
        }
    }

    // ── 15. int<->float + f32<->f64 casts ──
    {
        const char* src =
            "fn i2f(x: i64) -> f64 { return x as f64; }\n"
            "fn f2i(x: f64) -> i64 { return x as i64; }\n"
            "fn f2d(x: f32) -> f64 { return x as f64; }\n"
            "fn d2f(x: f64) -> f32 { return x as f32; }\n"
            "fn main() -> i64 {\n"
            "  if (i2f(42) != 42.0) { return 1; }\n"
            "  if (i2f(-7) != -7.0) { return 2; }\n"
            "  if (f2i(3.9) != 3) { return 3; }    // truncates toward zero\n"
            "  if (f2i(-3.9) != -3) { return 4; }  // truncates toward zero\n"
            "  if (f2d(1.5f) != 1.5) { return 5; } // f32->f64\n"
            "  if (d2f(2.5) != 2.5f) { return 6; } // f64->f32\n"
            "  // round-trip i64 min through f64 (may lose precision, but the\n"
            "  //  exact round-trip holds for INT64_MIN via f64).\n"
            "  let neg: i64 = -9223372036854775807 - 1;\n"
            "  if ((neg as f64) as i64 != neg) { return 7; }\n"
            "  return 42;\n"
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[15] int<->float + f32<->f64 casts: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[15] casts == 42 (got %lld)", (long long)r);
            ck(r == 42, b);
            double ri = call1_i64_f64(*m, "i2f", 42);
            int64_t rf = call1_f64_i64(*m, "f2i", 3.9);
            char bi[128], bf[128];
            std::snprintf(bi, sizeof bi, "[15] i2f(42)==42.0 (got %f)", ri);
            std::snprintf(bf, sizeof bf, "[15] f2i(3.9)==3 (got %lld)", (long long)rf);
            ck(ri == 42.0, bi);
            ck(rf == 3, bf);
        }
    }

    // ── 16. float native call (add_d: f64(f64,f64)) — float args in v0/v1,
    //     float result in v0. ──
    {
        const char* src =
            "fn g(a: f64, b: f64) -> f64 { return add_d(a, b); }\n"
            "fn main() -> i64 {\n"
            "  if (g(1.5, 2.5) != 4.0) { return 0; }\n"
            "  return 42;\n"
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[16] float native call add_d: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[16] g(1.5,2.5)==4.0 via add_d == 42 (got %lld)", (long long)r);
            ck(r == 42, b);
            double r0 = call2_f64(*m, "g", 1.5, 2.5);
            char b0[128];
            std::snprintf(b0, sizeof b0, "[16] g(1.5,2.5)==4.0 (got %f)", r0);
            ck(r0 == 4.0, b0);
        }
    }

    // ── 17. float param + return (a float identity fn + a fn returning a\n    //     float computed from a float param) ──
    {
        const char* src =
            "fn fid(x: f64) -> f64 { return x; }\n"
            "fn fsq(x: f64) -> f64 { return x * x; }\n"
            "fn main() -> i64 {\n"
            "  if (fid(3.14159) != 3.14159) { return 0; }\n"
            "  if (fsq(9.0) != 81.0) { return 0; }\n"
            "  return 42;\n"
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[17] float param + return: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[17] float param+return == 42 (got %lld)", (long long)r);
            ck(r == 42, b);
            double r0 = call1_f64(*m, "fid", 3.14159);
            double r1 = call1_f64(*m, "fsq", 9.0);
            char b0[128], b1[128];
            std::snprintf(b0, sizeof b0, "[17] fid(3.14159)==3.14159 (got %f)", r0);
            std::snprintf(b1, sizeof b1, "[17] fsq(9.0)==81.0 (got %f)", r1);
            ck(r0 == 3.14159, b0);
            ck(r1 == 81.0, b1);
        }
    }

    // ── 18. float loop — sum 0.0..~1.0 step 0.1 (approximate; assert within\n    //     epsilon). The loop exercises float Cmp (<), float add, float\n    //     ConstFloat, float param/local, and float return. ──
    {
        const char* src =
            "fn fsum(n: i64) -> f64 {\n"
            "  let mut s: f64 = 0.0;\n"
            "  let mut i: i64 = 0;\n"
            "  while (i < n) { s = s + 0.1; i = i + 1; }\n"
            "  return s;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "  let s: f64 = fsum(10);  // 10 * 0.1 ~ 1.0 (FP error)\n"
            "  // |s - 1.0| < 1e-9 -> 42; else 0\n"
            "  let mut d: f64 = s - 1.0;\n"
            "  if (d < 0.0) { d = 0.0 - d; }  // abs via negation\n"
            "  if (d < 0.000000001) { return 42; }\n"
            "  return 0;\n"
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[18] float loop: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[18] float loop sum(10*0.1)~1.0 == 42 (got %lld)", (long long)r);
            ck(r == 42, b);
            double s = call1_i64_f64(*m, "fsum", 10);  // fsum takes i64, returns f64
            char bs[128];
            std::snprintf(bs, sizeof bs, "[18] fsum(10) ~ 1.0 (got %.17f)", s);
            ck(s > 0.999999999 && s < 1.000000001, bs);
        }
    }

    std::printf("\n=== Phase 6b/6c: slices / structs / strings / for-each / match ===\n\n");

    // ── 19. slice construction + index + len (fixed array -> slice) ──
    {
        const char* src =
            "fn main() -> i64 {\n"
            "    let a: i64[3];\n"
            "    a[0] = 10; a[1] = 20; a[2] = 30;\n"
            "    let s: i64[] = a[..];\n"  // slice view of the array
            "    return s[0] + s[1] + s[2];\n"  // 60
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[19] slice construction+index: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[19] slice sum == 60 (got %lld)", (long long)r);
            ck(r == 60, b);
        }
    }

    // ── 20. string literal length + index (ConstStringRef + string_from_slice) ──
    {
        const char* src =
            "fn main() -> i64 {\n"
            "    let s: string = \"hello\";\n"  // 5 chars
            "    if (string_length(s) != 5) { return 1; }\n"
            "    if (string_char_at(s, 0) != 104) { return 2; }  // 'h'\n"
            "    if (string_char_at(s, 4) != 111) { return 3; }  // 'o'\n"
            "    return 42;\n"
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[20] string literal (ConstStringRef): compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[20] string literal len+index == 42 (got %lld)", (long long)r);
            ck(r == 42, b);
        }
    }

    // ── 21. string literal with encryption (StringDecrypt) ──
    {
        const char* src =
            "fn main() -> i64 {\n"
            "    let s: string = \"world\";\n"  // 5 chars, encrypted
            "    if (string_length(s) != 5) { return 1; }\n"
            "    if (string_char_at(s, 0) != 119) { return 2; }  // 'w'\n"
            "    if (string_char_at(s, 4) != 100) { return 3; }  // 'd'\n"
            "    return 42;\n"
            "}\n";
        auto m = compile_enc(src);
        ck(m.get() != nullptr, "[21] string literal (StringDecrypt): compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[21] encrypted string len+index == 42 (got %lld)", (long long)r);
            ck(r == 42, b);
        }
    }

    // ── 22. slice returned from a fn (slice return ABI {x0,x1}) — a
    //     hand-built ThinFunction: a fn takes a slice param (ptr,len) and
    //     returns it directly, exercising the slice param spill + slice return
    //     ABI {x0=ptr, x1=len}. ──
    {
        // Build a fn echo(s: i64[]) -> i64[] that returns its slice arg.
        ThinFunction thf; thf.name = "echo"; thf.ret_type = &type_i64();
        // ret_type is a slice<u8> (use a slice type). Build it:
        Type slice_ty = make_slice(std::make_shared<Type>(make_prim(Prim::U8)));
        thf.ret_type = &slice_ty;  // (leaks; test-only)
        thf.frame.frame_size = 48; thf.frame.rbx_save_offset = -8;
        // one slice param s at off=-24 (ptr) / -16 (len)
        ThinFramePlan::ParamSpill p; p.name = "s"; p.ty = &slice_ty;
        p.off = -24; p.word0 = 0; p.nwords = 2;
        thf.frame.params.push_back(p);
        ThinBlock b0; b0.id = 0;
        // Return ret=v1 (the slice ptr vreg; v2 is len)
        b0.term.kind = TermKind::Return; b0.term.ret = 1;
        thf.blocks.push_back(b0);
        // vreg map: v1 = ptr @-24, v2 = len @-16 (set by param spill)
        CodeGenCtx ctx;
        CompiledFn cf = emit_arm64(thf, ctx);
        ck(!cf.bytes.empty(), "[22] slice return (hand-built echo): emit non-empty");
        if (!cf.bytes.empty() && finalize(cf)) {
            // Call echo({ptr, len}). Use a struct-return shim: AAPCS64 returns
            // {x0=ptr, x1=len} for a 16B slice composite.
            struct Slice16 { const uint8_t* ptr; int64_t len; };
            using F = Slice16 (*)(const uint8_t*, int64_t);
            const uint8_t data[4] = {1,2,3,4};
            Slice16 r = reinterpret_cast<F>(cf.entry)(data, 4);
            char b[128];
            std::snprintf(b, sizeof b, "[22] echo slice len == 4 (got %lld)", (long long)r.len);
            ck(r.len == 4, b);
            char b2[128];
            std::snprintf(b2, sizeof b2, "[22] echo slice ptr preserved (got %p)", (void*)r.ptr);
            ck(r.ptr == data, b2);
            free_executable(cf.exec);
        }
    }

    // ── 23. struct-by-value arg + field access (small 16B struct) ──
    {
        const char* src =
            "struct P { x: i64; y: i64; }\n"
            "fn add_xy(p: P) -> i64 { return p.x + p.y; }\n"
            "fn main() -> i64 {\n"
            "    let q: P = P{ x: 30, y: 12 };\n"
            "    return add_xy(q);\n"  // 42
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[23] struct-by-value arg + field: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[23] struct arg p.x+p.y == 42 (got %lld)", (long long)r);
            ck(r == 42, b);
        }
    }

    // ── 24. struct return by ptr (mk returns S, main assigns) ──
    {
        const char* src =
            "struct S { v: i64; }\n"
            "fn mk(x: i64) -> S { return S{ v: x }; }\n"
            "fn main() -> i64 {\n"
            "    let s: S = mk(42);\n"
            "    return s.v;\n"
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[24] struct return by ptr: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[24] mk(42).v == 42 (got %lld)", (long long)r);
            ck(r == 42, b);
        }
    }

    // ── 25. HFA struct (Vec3 of 3 f32) passed + returned ──
    {
        const char* src =
            "struct Vec3 { x: f32; y: f32; z: f32; }\n"
            "fn sum_z(v: Vec3) -> f32 { return v.x + v.y + v.z; }\n"
            "fn main() -> i64 {\n"
            "    let p: Vec3 = Vec3{ x: 1.0f, y: 2.0f, z: 3.0f };\n"
            "    let r: f32 = sum_z(p);\n"  // 6.0
            "    if (r != 6.0f) { return 1; }\n"
            "    return 42;\n"
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[25] HFA Vec3 arg: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[25] HFA Vec3 x+y+z == 6.0 -> 42 (got %lld)", (long long)r);
            ck(r == 42, b);
        }
    }

    // ── 26. >16B struct passed by indirect pointer (5 i64 = 40 bytes) ──
    {
        const char* src =
            "struct Big { a: i64; b: i64; c: i64; d: i64; e: i64; }\n"  // 40 bytes > 16
            "fn sum_big(b: Big) -> i64 { return b.a + b.b + b.c + b.d + b.e; }\n"
            "fn main() -> i64 {\n"
            "    let x: Big = Big{ a: 1, b: 2, c: 3, d: 4, e: 5 };\n"
            "    return sum_big(x);\n"  // 15
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[26] >16B struct by indirect ptr: compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[26] Big sum == 15 (got %lld)", (long long)r);
            ck(r == 15, b);
        }
    }

    // ── 27. struct destructure via field access (FieldAddr) ──
    {
        const char* src =
            "struct P { x: i64; y: i64; }\n"
            "fn main() -> i64 {\n"
            "    let p: P = P{ x: 7, y: 35 };\n"
            "    return p.x + p.y;\n"  // 42
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[27] struct field access (FieldAddr): compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[27] p.x+p.y == 42 (got %lld)", (long long)r);
            ck(r == 42, b);
        }
    }

    // ── 28. match over a typed enum (int compare + branch). Phase 6d
    //     lowered MatchStmt to ThinIR (Cmp+Branch); the ARM64 emit supports
    //     the constituent ops, so this now compiles + runs end-to-end. The
    //     stale [SKIP] fallback (lowerer gap) is removed: a compile failure
    //     here is now a real FAIL, not a silent skip. ──
    {
        const char* src =
            "enum Color : i32 { Red, Green, Blue }\n"
            "fn main() -> i64 {\n"
            "    let c: Color = Color::Green;\n"
            "    let mut r: i64 = 0;\n"
            "    match (c) {\n"
            "        Color::Red => { r = 10; },\n"
            "        Color::Green => { r = 20; },\n"
            "        Color::Blue => { r = 30; },\n"
            "        _ => { r = 99; }\n"
            "    }\n"
            "    return r;\n"  // 20
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[28] match over enum: compiles (Phase 6d lowers match)");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[28] match Green == 20 (got %lld)", (long long)r);
            ck(r == 20, b);
        }
    }

    // ── 29. for-each over a slice (sum elements). Phase 6d lowered ForEachStmt
    //     to ThinIR (MakeSlice+IndexAddr+BoundsCheck+load); the ARM64 emit
    //     supports all the constituent ops, so this now compiles + runs. The
    //     stale [SKIP] fallback (lowerer gap) is removed. ──
    {
        const char* src =
            "fn main() -> i64 {\n"
            "    let a: i64[3];\n"
            "    a[0] = 10; a[1] = 20; a[2] = 30;\n"
            "    let mut sum: i64 = 0;\n"
            "    for (x in a[..]) { sum = sum + x; }\n"  // for-each over a slice view
            "    return sum;\n"  // 60
            "}\n";
        auto m = compile(src);
        ck(m.get() != nullptr, "[29] for-each over slice: compiles (Phase 6d lowers for-each)");
        if (m) {
            int64_t r = call0(*m, "main");
            char b[128];
            std::snprintf(b, sizeof b, "[29] for-each slice sum == 60 (got %lld)", (long long)r);
            ck(r == 60, b);
        }
    }

    // ── Safety-ON probes (audit M5/M6): use_context_reg=true + a trap stub +
    //    budget/depth checks. Run via ember_call_void (installs x19) with a
    //    host setjmp checkpoint so a TRAP is recoverable + observable. These
    //    pin that a budget-exceeded script, a depth-exceeded recursive call,
    //    and an uncaught throw all TRAP through the per-call context_t (not
    //    silently return / not crash the harness). ──
    std::printf("\n=== Phase 5: safety-ON trap probes (use_context_reg + trap stub) ===\n\n");

    // [30] budget-exceeded script TRAPS (BudgetExceeded), not returns.
    {
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut s: i64 = 0;\n"
            "    let mut i: i64 = 0;\n"
            "    while (i < 1000000) { s = s + i; i = i + 1; }\n"  // long loop: burns the budget
            "    return s;\n"
            "}\n";
        auto m = compile_safe(src, /*max_depth=*/64);
        ck(m.get() != nullptr, "[30] budget-exceeded: compiles (safety ON)");
        if (m) {
            context_t ctx; ctx.budget_remaining = 1000; ctx.max_call_depth = 64;
            ctx.last_trap = TrapReason::None;
            bool trapped = false;
            (void)call0_safe(*m, "main", &ctx, &trapped);
            ck(true, "[30] budget-exceeded: run did NOT crash (trap recovered)");
            ck(trapped, "[30] budget-exceeded: TRAPPED (did not return normally)");
            ck(ctx.last_trap == TrapReason::BudgetExceeded,
               "[30] budget-exceeded: last_trap == BudgetExceeded");
            std::printf("[INFO] [30] budget-exceeded: trapped=%d last_trap=%d\n",
                        (int)trapped, (int)ctx.last_trap);
        }
    }

    // [31] depth-exceeded recursive call TRAPS (StackOverflow), not returns.
    //     A recursive fib with a low max_call_depth exceeds the depth guard.
    {
        const char* src =
            "fn rec(n: i64) -> i64 {\n"
            "    if (n <= 0) { return 1; }\n"
            "    return rec(n - 1) + rec(n - 2);\n"  // binary recursion: depth grows fast
            "}\n"
            "fn main() -> i64 { return rec(40); }\n";  // deep recursion
        auto m = compile_safe(src, /*max_depth=*/4);  // tiny depth budget
        ck(m.get() != nullptr, "[31] depth-exceeded: compiles (safety ON)");
        if (m) {
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 4;
            ctx.last_trap = TrapReason::None;
            bool trapped = false;
            (void)call0_safe(*m, "main", &ctx, &trapped);
            ck(true, "[31] depth-exceeded: run did NOT crash (trap recovered)");
            ck(trapped, "[31] depth-exceeded: TRAPPED (did not return normally)");
            ck(ctx.last_trap == TrapReason::StackOverflow,
               "[31] depth-exceeded: last_trap == StackOverflow");
            std::printf("[INFO] [31] depth-exceeded: trapped=%d last_trap=%d\n",
                        (int)trapped, (int)ctx.last_trap);
        }
    }

    // [32] throw-without-catch TRAPS (UnhandledThrow), not returns.
    {
        const char* src =
            "fn throw_uncaught() -> i64 { throw 123; }\n"
            "fn main() -> i64 { return throw_uncaught(); }\n";
        auto m = compile_safe(src, /*max_depth=*/64);
        ck(m.get() != nullptr, "[32] uncaught throw: compiles (safety ON)");
        if (m) {
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            ctx.last_trap = TrapReason::None;
            bool trapped = false;
            (void)call0_safe(*m, "main", &ctx, &trapped);
            ck(true, "[32] uncaught throw: run did NOT crash (trap recovered)");
            ck(trapped, "[32] uncaught throw: TRAPPED (did not return normally)");
            ck(ctx.last_trap == TrapReason::UnhandledThrow,
               "[32] uncaught throw: last_trap == UnhandledThrow");
            std::printf("[INFO] [32] uncaught throw: trapped=%d last_trap=%d\n",
                        (int)trapped, (int)ctx.last_trap);
        }
    }

    // ── [33] Large-frame spill probe (audit L1): ~600 i64 locals → frame
    //    ~4800B > 0xFFF (4095). emit_arm64's prologue splits the `sub sp, sp,
    //    frame_size` into 4096-step decrements when frame_size > 0xFFF (the
    //    ARM64 `sub imm12` field is a 12-bit scaled immediate). This probe
    //    exercises that split-sub path: it declares 600 i64 locals, assigns
    //    each its 1-based index, and sums them. The expected sum is
    //    1+2+...+600 = 600*601/2 = 180300. A bug in the split-sub path
    //    (wrong SP after the split, or a frame slot addressed off the wrong
    //    SP/X29 base) would miscompute the sum or crash. ──
    {
        const int N = 600;
        std::string src = "fn main() -> i64 {\n";
        for (int i = 1; i <= N; ++i)
            src += "    let v" + std::to_string(i) + ": i64 = " + std::to_string(i) + ";\n";
        src += "    let mut sum: i64 = 0;\n";
        for (int i = 1; i <= N; ++i)
            src += "    sum = sum + v" + std::to_string(i) + ";\n";
        src += "    return sum;\n";  // 1+2+...+600 = 180300
        src += "}\n";
        auto m = compile(src);  // safety OFF: a raw AAPCS64 call works
        ck(m.get() != nullptr, "[33] large-frame (~600 i64 locals, frame > 0xFFF): compiles");
        if (m) {
            int64_t r = call0(*m, "main");
            int64_t expect = int64_t(N) * int64_t(N + 1) / 2;  // 180300
            char b[128];
            std::snprintf(b, sizeof b, "[33] large-frame sum == %lld (got %lld)",
                          (long long)expect, (long long)r);
            ck(r == expect, b);
        }
    }

    std::printf("\nemit_arm64_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
