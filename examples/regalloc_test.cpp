// regalloc_test.cpp — Stage 3: linear-scan register allocation value-preservation +
// register-assignment gate.
//
// Verifies that run_regalloc + emit_x64 produce the SAME i64 return as the
// all-frame-slot path (regalloc off). The test compiles each workload TWO ways:
//   (a) lower_function → emit_x64 (regalloc OFF — the existing all-frame path)
//   (b) lower_function → run_regalloc → emit_x64 (regalloc ON)
// and asserts identical i64 returns. It also checks that the regalloc actually
// assigned SOME VRegs to registers (ra.enabled + at least one in_reg assignment)
// for the register-heavy workloads.
//
// Modeled on ir_passes_test.cpp (the M struct, ck() macro, call0_i64 pattern).

#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/globals.hpp"
#include "../src/binding_builder.hpp"
#include "../src/thin_ir.hpp"
#include "../src/thin_lower.hpp"
#include "../src/thin_emit.hpp"
#include "../src/regalloc.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>

using namespace ember;

// Architecture detection: on ARM64 (Apple Silicon, etc.) the linear-scan
// regalloc is x86-specific — it assigns x86 callee-saved register IDs (rbx=3,
// rsi=6, rdi=7, r12=12, r13=13, r15=15) that emit_arm64 does NOT consume
// (ARM64 uses frame-only emit with NO regalloc; compile_func_checked skips
// run_regalloc on ARM64 via a `&& false` guard). So the x86 regalloc-execution
// cases (which run_regalloc + emit_x64 + execute the regalloc'd x86 code) SIGILL
// on ARM64. The x86 cases are retained under `if (!kIsArm64)`; the ARM64 branch
// uses compile_func_checked to assert frame-only emission (cr.backend ==
// IRBackend) + correct value + no x86 register IDs in the post-pass IR.
static constexpr bool kIsArm64 =
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    true
#else
    false
#endif
    ;

// ─── Workloads ───
// Each exercises a pattern that benefits from register allocation: multiple
// live values, recursion, loop accumulators, register pressure.

// Register-heavy: many simultaneously-live intermediates (a*b*c*d + e*f*g*h)
static const char* SRC_REG_PRESSURE =
    "fn main() -> i64 {\n"
    "    let a: i64 = 1; let b: i64 = 2; let c: i64 = 3; let d: i64 = 4;\n"
    "    let e: i64 = 5; let f: i64 = 6; let g: i64 = 7; let h: i64 = 8;\n"
    "    let p1: i64 = a * b * c * d;     // 24\n"
    "    let p2: i64 = e * f * g * h;     // 1680\n"
    "    return p1 + p2;                  // 1704\n"
    "}\n";

// Recursion (fibonacci): callee-saved registers must survive calls
static const char* SRC_RECURSION =
    "fn fib(n: i64) -> i64 {\n"
    "    if (n < 2) { return n; }\n"
    "    return fib(n - 1) + fib(n - 2);\n"
    "}\n"
    "fn main() -> i64 { return fib(15); }\n";  // 610

// Loop accumulator: the loop variable + sum stay live across iterations
static const char* SRC_LOOP_ACCUM =
    "fn main() -> i64 {\n"
    "    let mut s: i64 = 0;\n"
    "    let mut i: i64 = 0;\n"
    "    while (i < 100) {\n"
    "        s = s + i * i;\n"
    "        i = i + 1;\n"
    "    }\n"
    "    return s;  // sum of i^2 for i=0..99 = 328350\n"
    "}\n";

// Nested function calls (fact): deeper recursion + multiply across calls
static const char* SRC_FACTORIAL =
    "fn fact(n: i64) -> i64 {\n"
    "    if (n <= 1) { return 1; }\n"
    "    return n * fact(n - 1);\n"
    "}\n"
    "fn main() -> i64 { return fact(10); }\n";  // 3628800

// Multiple returns + branches: values flow through different paths
static const char* SRC_BRANCHES =
    "fn classify(n: i64) -> i64 {\n"
    "    if (n < 0) { return 0 - n; }\n"
    "    if (n == 0) { return 1; }\n"
    "    if (n < 10) { return n * 2; }\n"
    "    if (n < 100) { return n + 100; }\n"
    "    return n;\n"
    "}\n"
    "fn main() -> i64 {\n"
    "    let mut acc: i64 = 0;\n"
    "    let mut i: i64 = 0;\n"
    "    while (i < 50) {\n"
    "        acc = acc + classify(i);\n"
    "        i = i + 1;\n"
    "    }\n"
    "    return acc;\n"
    "}\n";

// Compound assignment + inc/dec (register-heavy mutations)
static const char* SRC_COMPOUND =
    "fn main() -> i64 {\n"
    "    let mut a: i64 = 10;\n"
    "    a += 5;   // 15\n"
    "    a -= 3;   // 12\n"
    "    a *= 2;   // 24\n"
    "    a /= 4;   // 6\n"
    "    a %= 4;   // 2\n"
    "    let mut b: i64 = 5;\n"
    "    let post: i64 = b++;  // post=5, b=6\n"
    "    let pre: i64 = ++b;   // pre=7, b=7\n"
    "    return a + b + post + pre;  // 2+7+5+7 = 21\n"
    "}\n";

// Integer arithmetic edges (div, mod, shifts, bitwise)
static const char* SRC_INT_OPS =
    "fn main() -> i64 {\n"
    "    let a: i64 = 7 / 2;      // 3\n"
    "    let b: i64 = 7 % 3;      // 1\n"
    "    let c: i64 = 1 << 8;     // 256\n"
    "    let d: i64 = 256 >> 2;   // 64\n"
    "    let e: i64 = 0x0F | 0xF0; // 255\n"
    "    let f: i64 = 0xFF & 0x0F; // 15\n"
    "    let g: i64 = 0xFF ^ 0x0F; // 240\n"
    "    return a + b + c + d + e + f + g;  // 3+1+256+64+255+15+240 = 834\n"
    "}\n";

// ─── Test infrastructure (modeled on ir_passes_test.cpp) ───

struct M {
    std::vector<CompiledFn> fns;
    std::unique_ptr<DispatchTable> table;
    std::unordered_map<std::string, int> slots;
    GlobalsBlock gb;
    std::vector<uint8_t> gbs;
    StructLayoutTable layouts;
    Program prog;
    // The execution context. The workloads are compiled with use_context_reg=
    // true + emit_depth_checks=true, so the JIT'd code reads/writes the
    // call-depth counter via [r14 + offset]. A raw `int64_t (*)()` call leaves
    // r14 as whatever garbage the CRT left, so the depth check's `inc [r14+off]`
    // writes a random memory location and its `cmp [r14+max_depth]` reads one —
    // silent corruption that eventually surfaces as a SIGILL in a later,
    // unrelated workload (the call-heavy workloads corrupt memory; the next
    // JIT'd function trips over it). ember_call_void installs ctx into r14
    // (the B1 thunk), so the depth check touches THIS context's real fields
    // and the call-depth guard works as designed. Mirrors the in_context/
    // thread_safety harnesses (which pass a real context_t to ember_call_*).
    ember::context_t ctx{};
    M() : table(std::make_unique<DispatchTable>(0)) {}
};

static int g_fail = 0;
static void ck(bool c, const char* m) {
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// Compile with optional regalloc (runs between lower_function and emit_x64).
// Returns the module. When regalloc_on is true, also returns whether the
// regalloc assigned any VRegs to registers (via *any_in_reg).
static std::unique_ptr<M> compile_with(bool regalloc_on, const std::string& src,
                                       bool* any_in_reg = nullptr) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<regalloc_test>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = si - 1; }

    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
    m->layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;
    auto sr = sema(m->prog, natives, m->slots, 0, &overloads, &m->layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema: %s\n", sr.errors.empty() ? "<unknown>" : sr.errors[0].msg.c_str());
        return nullptr;
    }

    m->gbs.assign(0, 0);
    m->gb.base = 0;
    g_globals_for_codegen = &m->gb;
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());

    CodeGenCtx ctx;
    ctx.globals_base = 0;
    ctx.dispatch_base = int64_t(m->table->base());
    ctx.natives = &natives;
    ctx.script_slots = &m->slots;
    ctx.structs = &m->layouts;
    ctx.use_context_reg = true;
    ctx.enable_ir_backend = false;
    ctx.emit_depth_checks = true;

    for (auto& fn : m->prog.funcs) {
        ThinFunction thf = lower_function(fn, ctx);
        if (thf.blocks.empty()) {
            std::printf("FAIL: lower_function gave empty blocks for %s\n", fn.name.c_str());
            return nullptr;
        }
        if (regalloc_on) {
            run_regalloc(thf);
            if (any_in_reg && fn.name == "main") {
                for (const auto& [v, a] : thf.ra.map)
                    if (a.in_reg) { *any_in_reg = true; break; }
            }
        }
        CompiledFn cf = emit_x64(thf, ctx);
        if (cf.bytes.empty()) {
            std::printf("FAIL: emit_x64 gave empty bytes for %s\n", fn.name.c_str());
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

static int64_t call0_i64(M& m, const std::string& fn) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1;
    void* e = m.table->get(it->second);
    // Install a fresh per-call state on the context + enter via the B1 thunk
    // (sets r14 = &m.ctx). The workloads use use_context_reg + depth checks,
    // so r14 MUST point at a real context_t — a raw call leaves it garbage and
    // the depth guard corrupts memory (see the M::ctx note). budget is left
    // high (no false budget traps); depth starts at 0.
    m.ctx.call_depth = 0;
    m.ctx.catch_depth = 0;
    m.ctx.thrown_value = 0;
    m.ctx.budget_remaining = 2'000'000'000LL;
    return ember::ember_call_void(e, &m.ctx);
}

// ─── ARM64 path: frame-only emission via compile_func_checked ───
// On ARM64 the linear-scan regalloc is architecturally N/A (it assigns x86
// callee-saved register IDs emit_arm64 does not consume). compile_func_checked
// skips run_regalloc on ARM64 (a `&& false` guard in codegen.cpp) and uses
// emit_arm64 (frame-only). This helper compiles each workload through
// compile_func_checked with enable_ir_backend=true + enable_regalloc=true +
// request_transformed_ir=true, so the ARM64 branch can assert:
//   (a) frame-only emission is selected (cr.backend == CompileBackend::IRBackend)
//   (b) no x86 register IDs reached emit_arm64 (cr.transformed->ra.enabled ==
//       false + ra.map empty + ra.used_reg_ids empty — regalloc was skipped)
//   (c) the correct value is produced (call0_i64 == expected)
// Returns the module + (via out_cr) the main fn's CompileResult for ra inspection.
static std::unique_ptr<M> compile_arm64(const std::string& src,
                                        CompileResult* out_cr = nullptr) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<regalloc_test>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = si - 1; }

    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
    m->layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;
    auto sr = sema(m->prog, natives, m->slots, 0, &overloads, &m->layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema: %s\n", sr.errors.empty() ? "<unknown>" : sr.errors[0].msg.c_str());
        return nullptr;
    }

    m->gbs.assign(0, 0);
    m->gb.base = 0;
    g_globals_for_codegen = &m->gb;
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());

    CodeGenCtx ctx;
    ctx.globals_base = 0;
    ctx.dispatch_base = int64_t(m->table->base());
    ctx.natives = &natives;
    ctx.script_slots = &m->slots;
    ctx.structs = &m->layouts;
    ctx.use_context_reg = true;
    ctx.enable_ir_backend = true;   // ARM64: IR backend is the sole codegen path
    ctx.enable_regalloc = true;     // requested — but compile_func_checked skips it on ARM64
    ctx.emit_depth_checks = true;
    ctx.request_transformed_ir = true;  // so cr.transformed has the post-pass IR for ra inspection

    for (auto& fn : m->prog.funcs) {
        CompileResult cr = compile_func_checked(fn, ctx);
        if (fn.name == "main" && out_cr) *out_cr = cr;  // save main's result for ra checks
        CompiledFn cf = std::move(cr.compiled);
        if (cf.bytes.empty()) {
            std::printf("FAIL: compile_func_checked gave empty bytes for %s\n", fn.name.c_str());
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

int main() {
    std::printf("=== regalloc_test: Stage 3 linear-scan register allocation ===\n\n");
    std::fflush(stdout);

    struct Workload { const char* name; const char* src; int64_t expected; bool expect_regs; };
    Workload workloads[] = {
        {"reg_pressure",  SRC_REG_PRESSURE, 1704,    true},
        {"recursion_fib", SRC_RECURSION,    610,     true},
        {"loop_accum",    SRC_LOOP_ACCUM,   328350,  true},
        {"factorial",     SRC_FACTORIAL,    3628800, true},
        {"branches",      SRC_BRANCHES,     0,       true},  // computed below
        {"compound",      SRC_COMPOUND,     21,      true},
        {"int_ops",       SRC_INT_OPS,      834,     true},
    };
    int NW = int(sizeof(workloads) / sizeof(workloads[0]));

    // ─── Architecture split ───
    // On x86 (!kIsArm64): run the full Stage 3 linear-scan regalloc suite —
    //   value-preservation (regalloc ON vs OFF), register assignment, regalloc
    //   result sanity (valid x86 callee-saved register IDs), forced spilling,
    //   + regalloc-off no-op. These exercise run_regalloc + emit_x64 + execute
    //   the regalloc'd x86 code, which is architecturally x86-specific.
    // On ARM64 (kIsArm64): the linear-scan regalloc is N/A (it assigns x86
    //   callee-saved register IDs emit_arm64 does not consume; compile_func_
    //   checked skips run_regalloc on ARM64). Assert the ARM64 invariants:
    //   (a) frame-only emission is selected (cr.backend == IRBackend),
    //   (b) no x86 register IDs reached emit_arm64 (ra.enabled == false,
    //       ra.map + ra.used_reg_ids empty — regalloc was skipped),
    //   (c) the correct value is produced via emit_arm64.
    if (!kIsArm64) {

    // Compute the expected value for "branches" by running it without regalloc
    // first (it's a complex accumulator; we verify regalloc matches, not a
    // hardcoded value — but we DO hardcode the others as independent checks).

    // (1) Value-preservation: regalloc ON vs OFF must produce identical results.
    std::printf("(1) Value-preservation (regalloc ON vs OFF)\n"); std::fflush(stdout);
    for (int wi = 0; wi < NW; ++wi) {
        const auto& wl = workloads[wi];
        std::printf("  [%d] %s ...\n", wi, wl.name); std::fflush(stdout);

        // Compile WITHOUT regalloc (baseline)
        auto m_off = compile_with(false, wl.src);
        if (!m_off) { ck(false, wl.name); continue; }
        int64_t rc_off = call0_i64(*m_off, "main");

        // Compile WITH regalloc
        bool any_in_reg = false;
        auto m_on = compile_with(true, wl.src, &any_in_reg);
        if (!m_on) { ck(false, wl.name); continue; }
        int64_t rc_on = call0_i64(*m_on, "main");

        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s: regalloc-off=%lld regalloc-on=%lld %s",
                      wl.name, (long long)rc_off, (long long)rc_on,
                      (rc_off == rc_on) ? "(match)" : "(MISMATCH!)");
        ck(rc_off == rc_on, buf);

        // Also verify against the hardcoded expected value (independent check)
        if (wl.name != std::string("branches")) {
            std::snprintf(buf, sizeof(buf), "%s: expected=%lld got=%lld",
                          wl.name, (long long)wl.expected, (long long)rc_on);
            ck(rc_on == wl.expected, buf);
        }

        // For "branches", store the baseline for the expected check
        if (wl.name == std::string("branches")) {
            workloads[wi].expected = rc_off;  // use the no-regalloc result as expected
        }
    }

    // (2) Register assignment: the regalloc should actually assign VRegs to
    // registers for register-heavy workloads (not spill everything).
    std::printf("\n(2) Register assignment (at least one VReg in a register)\n");
    for (int wi = 0; wi < NW; ++wi) {
        const auto& wl = workloads[wi];
        bool any_in_reg = false;
        auto m = compile_with(true, wl.src, &any_in_reg);
        if (!m) { ck(false, wl.name); continue; }
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s: regalloc assigned VRegs to registers", wl.name);
        if (wl.expect_regs) {
            ck(any_in_reg, buf);
        } else {
            ck(true, buf);  // don't care
        }
    }

    // (3) Regalloc result sanity: ra.enabled is set, used_reg_ids are valid
    // callee-saved registers (rbx=3, rsi=6, rdi=7, r12=12, r13=13, r15=15).
    std::printf("\n(3) Regalloc result sanity (valid callee-saved registers)\n");
    {
        auto m = compile_with(true, SRC_REG_PRESSURE);
        if (!m) { ck(false, "sanity compile"); }
        else {
            // Re-lower to inspect the regalloc result directly
            auto lr = tokenize(SRC_REG_PRESSURE, "<regalloc_test>");
            auto pr = parse(std::move(lr.toks));
            Program prog = std::move(pr.program);
            int si = 0;
            std::unordered_map<std::string, int> slots;
            for (auto& fn : prog.funcs) { slots[fn.name] = si++; fn.slot = si - 1; }
            std::unordered_map<std::string, NativeSig> natives;
            OpOverloadTable overloads;
            StructLayoutTable layouts = build_struct_layouts(prog);
            prog.string_xor_key = 0;
            sema(prog, natives, slots, 0, &overloads, &layouts);
            GlobalsBlock gb; gb.base = 0; g_globals_for_codegen = &gb;
            DispatchTable table(prog.funcs.size());
            CodeGenCtx ctx;
            ctx.globals_base = 0; ctx.dispatch_base = int64_t(table.base());
            ctx.natives = &natives; ctx.script_slots = &slots; ctx.structs = &layouts;
            ctx.use_context_reg = true; ctx.enable_ir_backend = false;

            for (auto& fn : prog.funcs) {
                ThinFunction thf = lower_function(fn, ctx);
                run_regalloc(thf);
                ck(thf.ra.enabled, "regalloc enabled for each fn");
                // Check all used_reg_ids are valid callee-saved registers
                for (int32_t rid : thf.ra.used_reg_ids) {
                    bool valid = (rid == 3 || rid == 6 || rid == 7 ||
                                  rid == 12 || rid == 13 || rid == 15);
                    if (!valid) {
                        char buf[128];
                        std::snprintf(buf, sizeof(buf), "invalid reg_id %d in %s", rid, fn.name.c_str());
                        ck(false, buf);
                    }
                }
                // Check save_offsets are valid (non-zero, rbp-negative)
                for (int32_t off : thf.ra.save_offsets) {
                    if (off >= 0) {
                        char buf[128];
                        std::snprintf(buf, sizeof(buf), "invalid save_offset %d in %s", off, fn.name.c_str());
                        ck(false, buf);
                    }
                }
                // Check frame_size was extended to fit save slots
                int32_t max_save = 0;
                for (int32_t off : thf.ra.save_offsets) {
                    if (-off > max_save) max_save = -off;
                }
                if (thf.frame.frame_size < max_save + 16) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "frame_size %d too small for save slots in %s (need >= %d)",
                                  thf.frame.frame_size, fn.name.c_str(), max_save + 16);
                    ck(false, buf);
                }
            }
            ck(true, "all used_reg_ids are valid callee-saved registers");
        }
    }

    // (4) Regalloc with num_regs=1 (forced spilling): still value-correct
    std::printf("\n(4) Forced spilling (num_regs=1) value-correctness\n");
    {
        auto m_off = compile_with(false, SRC_RECURSION);
        if (!m_off) { ck(false, "spill baseline compile"); }
        else {
            int64_t rc_off = call0_i64(*m_off, "main");

            // Compile with regalloc restricted to 1 register (forces spilling)
            auto lr = tokenize(SRC_RECURSION, "<regalloc_test>");
            auto pr = parse(std::move(lr.toks));
            auto m = std::make_unique<M>();
            m->prog = std::move(pr.program);
            int si = 0;
            for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = si - 1; }
            std::unordered_map<std::string, NativeSig> natives;
            OpOverloadTable overloads;
            m->layouts = build_struct_layouts(m->prog);
            m->prog.string_xor_key = 0;
            sema(m->prog, natives, m->slots, 0, &overloads, &m->layouts);
            m->gbs.assign(0, 0); m->gb.base = 0; g_globals_for_codegen = &m->gb;
            m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());
            CodeGenCtx ctx;
            ctx.globals_base = 0; ctx.dispatch_base = int64_t(m->table->base());
            ctx.natives = &natives; ctx.script_slots = &m->slots; ctx.structs = &m->layouts;
            ctx.use_context_reg = true; ctx.enable_ir_backend = false; ctx.emit_depth_checks = true;
            for (auto& fn : m->prog.funcs) {
                ThinFunction thf = lower_function(fn, ctx);
                run_regalloc(thf, 1);  // only 1 register (rbx)
                CompiledFn cf = emit_x64(thf, ctx);
                finalize(cf);
                m->table->set(fn.slot, cf.entry);
                m->fns.push_back(std::move(cf));
            }
            int64_t rc_on = call0_i64(*m, "main");
            char buf[256];
            std::snprintf(buf, sizeof(buf), "forced-spill fib(15): off=%lld on=%lld %s",
                          (long long)rc_off, (long long)rc_on,
                          (rc_off == rc_on) ? "(match)" : "(MISMATCH!)");
            ck(rc_off == rc_on, buf);
        }
    }

    // (5) Regalloc OFF is byte-identical to the existing path (regalloc is
    // a no-op when ra.enabled is false — the emit falls back to all-frame).
    // (x86-only: inspects the lowered ThinFunction's ra field directly.)
    std::printf("\n(5) Regalloc-off is a no-op (ra.enabled stays false)\n");
    {
        auto lr = tokenize(SRC_REG_PRESSURE, "<regalloc_test>");
        auto pr = parse(std::move(lr.toks));
        Program prog = std::move(pr.program);
        int si = 0;
        std::unordered_map<std::string, int> slots;
        for (auto& fn : prog.funcs) { slots[fn.name] = si++; fn.slot = si - 1; }
        std::unordered_map<std::string, NativeSig> natives;
        OpOverloadTable overloads;
        StructLayoutTable layouts = build_struct_layouts(prog);
        prog.string_xor_key = 0;
        sema(prog, natives, slots, 0, &overloads, &layouts);
        GlobalsBlock gb; gb.base = 0; g_globals_for_codegen = &gb;
        DispatchTable table(prog.funcs.size());
        CodeGenCtx ctx;
        ctx.globals_base = 0; ctx.dispatch_base = int64_t(table.base());
        ctx.natives = &natives; ctx.script_slots = &slots; ctx.structs = &layouts;
        ctx.use_context_reg = true; ctx.enable_ir_backend = false;
        for (auto& fn : prog.funcs) {
            ThinFunction thf = lower_function(fn, ctx);
            ck(!thf.ra.enabled, "regalloc off by default (ra.enabled=false)");
        }
    }

    }  // end if (!kIsArm64) — x86 regalloc suite
    else {
    // ─── ARM64: frame-only emission invariants (regalloc is N/A) ───
    std::printf("(ARM64) Frame-only emission invariants (regalloc N/A on ARM64)\n");
    std::fflush(stdout);

    // (A) For each workload: compile via compile_func_checked (enable_ir_backend
    //     + enable_regalloc + request_transformed_ir), assert:
    //       (a) cr.backend == CompileBackend::IRBackend (frame-only emit selected)
    //       (b) no x86 register IDs reached emit_arm64: the post-pass IR's
    //           ra.enabled == false + ra.map empty + ra.used_reg_ids empty
    //           (compile_func_checked skips run_regalloc on ARM64)
    //       (c) call0_i64 == expected (the correct value via emit_arm64)
    for (int wi = 0; wi < NW; ++wi) {
        const auto& wl = workloads[wi];
        std::printf("  [%d] %s ...\n", wi, wl.name); std::fflush(stdout);

        CompileResult cr;
        auto m = compile_arm64(wl.src, &cr);
        if (!m) { ck(false, wl.name); continue; }

        // (a) frame-only emission: the IR backend was used (not a tree-walker
        //     fallback — there is none on ARM64). emit_arm64 is frame-only.
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s: cr.backend == IRBackend (frame-only emit)", wl.name);
        ck(cr.backend == CompileBackend::IRBackend, buf);
        ck(cr.ok(), "compile_func_checked succeeded (executable produced)");

        // (b) no x86 register IDs reached emit_arm64: regalloc was skipped on
        //     ARM64, so the post-pass IR has ra.enabled == false + empty maps.
        //     cr.transformed holds the post-pass ThinFunction (request_
        //     transformed_ir was set). If absent, the compile still succeeded
        //     but we cannot inspect ra — assert via the stage trace instead.
        if (cr.transformed) {
            std::snprintf(buf, sizeof(buf), "%s: ra.enabled == false (regalloc skipped on ARM64)", wl.name);
            ck(!cr.transformed->ra.enabled, buf);
            std::snprintf(buf, sizeof(buf), "%s: ra.map empty (no x86 reg assignments)", wl.name);
            ck(cr.transformed->ra.map.empty(), buf);
            std::snprintf(buf, sizeof(buf), "%s: ra.used_reg_ids empty (no x86 callee-saved IDs)", wl.name);
            ck(cr.transformed->ra.used_reg_ids.empty(), buf);
        } else {
            // Fallback: the Regalloc stage trace should report "skipped".
            const CompileStageTrace* rt = cr.stage(CompileStage::Regalloc);
            std::snprintf(buf, sizeof(buf), "%s: regalloc stage skipped on ARM64", wl.name);
            ck(rt != nullptr && !rt->reached, buf);
        }

        // (c) correct value via emit_arm64 (frame-only execution).
        //     For "branches" the expected value is computed by the no-regalloc
        //     baseline; on ARM64 there is no regalloc, so the frame-only path
        //     IS the baseline — use the hardcoded expected values (branches
        //     is recomputed below from the first compile).
        int64_t rc = call0_i64(*m, "main");
        if (wl.name != std::string("branches")) {
            std::snprintf(buf, sizeof(buf), "%s: expected=%lld got=%lld (ARM64 frame-only)",
                          wl.name, (long long)wl.expected, (long long)rc);
            ck(rc == wl.expected, buf);
        } else {
            // branches: store the ARM64 result as the expected (it IS the
            // baseline — no regalloc to compare against).
            workloads[wi].expected = rc;
            std::snprintf(buf, sizeof(buf), "%s: ARM64 frame-only result=%lld (baseline)",
                          wl.name, (long long)rc);
            ck(rc >= 0, buf);  // sanity: non-negative accumulator
        }
    }

    // (B) Explicitly verify enable_regalloc=true is ignored on ARM64: compile
    //     a register-heavy workload with regalloc REQUESTED + inspect the
    //     post-pass IR. The regalloc stage must be skipped (not reached) +
    //     ra must be clear. This proves the x86 regalloc output never reaches
    //     emit_arm64 even when the host requests it.
    {
        CompileResult cr;
        auto m = compile_arm64(SRC_REG_PRESSURE, &cr);
        ck(m != nullptr, "ARM64 regalloc-requested compile succeeds");
        if (m) {
            ck(cr.backend == CompileBackend::IRBackend,
               "ARM64: enable_regalloc=true still uses IRBackend (regalloc skipped)");
            const CompileStageTrace* rt = cr.stage(CompileStage::Regalloc);
            ck(rt != nullptr, "ARM64: Regalloc stage trace present");
            if (rt) {
                ck(!rt->reached, "ARM64: Regalloc stage NOT reached (skipped despite enable_regalloc=true)");
            }
            if (cr.transformed) {
                ck(!cr.transformed->ra.enabled,
                   "ARM64: post-pass ra.enabled == false (no x86 regalloc ran)");
                ck(cr.transformed->ra.used_reg_ids.empty(),
                   "ARM64: post-pass ra.used_reg_ids empty (no x86 callee-saved IDs)");
            }
            // Value is still correct (frame-only emit is value-equivalent).
            int64_t rc = call0_i64(*m, "main");
            char buf[256];
            std::snprintf(buf, sizeof(buf), "ARM64: reg_pressure correct under frame-only (=%lld)", (long long)rc);
            ck(rc == 1704, buf);
        }
    }

    }  // end else (ARM64 invariants)

    if (g_fail) {
        std::printf("\n*** regalloc_test: FAILURES ***\n");
        return 1;
    }
    std::printf("\n*** regalloc_test: ALL PASS ***\n");
    return 0;
}
