// keyed_dispatch_arm64_reject_test — ARM64 keyed-dispatch codegen REJECTION
// gate (audit H1). On ARM64 the keyed cross-module call path + the
// legacy->keyed cross-module path are REJECTED at codegen
// (src/thin_emit_arm64.cpp ~2858-2890: "keyed cross-module call not yet
// supported" + "legacy-to-keyed cross-module call rejected at codegen") and
// `@obf_keyed` is blocked by `ir_backend_unavailable_reason` (ARM64 has no
// x86 tree-walker fallback -> a hard compile error). This test pins all three
// rejections so a future regression that silently MISCOMPILES (or crashes)
// instead of rejecting is caught.
//
// What this pins:
//  (1) @obf_keyed fn -> compile_func_checked returns ok()==false with a
//      reason containing "IR backend unavailable" + "obf" (the ARM64 hard
//      error: no x86 tree-walker fallback for an obf function).
//  (2) keyed caller -> keyed cross-module target (mod::fn where the target
//      module's published dispatch_mode is Keyed): the call site is lowered to
//      a TRAP (TrapReason::BadCallTarget) at codegen. The compile produces a
//      trap-stub CompiledFn (no crash); at RUNTIME the trap fires through the
//      host trap stub + longjmps to the checkpoint. Asserts trapped==true +
//      last_trap==BadCallTarget (the keyed cross-module call was rejected,
//      not silently miscompiled).
//  (3) legacy caller (no keyed_dispatch) -> keyed cross-module target:
//      non_serializable_reason = "legacy-to-keyed cross-module call rejected
//      at codegen" (Red 7: a legacy caller has no runtime-key contract).
//
// The keyed cross-module target is constructed by injecting a ModuleExport
// with dispatch_mode=Keyed into sema's ModuleExportTable (the same table the
// linker builds from a registered module's published record). Sema stamps
// cross_module_target_mode=1 on the `mod::fn()` CallExpr; lower_function
// lowers it to a CallCrossModule ThinInstr; emit_arm64's
// emit_cross_module_call hits the rejection. This exercises the REAL
// rejection code path (not a hand-built ThinFunction) while staying
// self-contained (no Windows module registry).
//
// Apple-ARM64 only (the rejection is ARM64-codegen-specific; the x86 backend
// HAS the keyed cross-module thunks). Gated by if(APPLE) in CMakeLists.txt.

#include "../src/thin_emit.hpp"       // emit_arm64
#include "../src/thin_lower.hpp"      // lower_function
#include "../src/thin_ir.hpp"         // ThinFunction
#include "../src/engine.hpp"          // CompiledFn, finalize, free_executable, ember_call_void
#include "../src/dispatch_table.hpp"  // DispatchTable
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"         // CodeGenCtx, compile_func, compile_func_checked
#include "../src/globals.hpp"         // GlobalsBlock
#include "../src/context.hpp"         // context_t, TrapReason, EMBER_SETJMP/LONGJMP
#include "../src/ast.hpp"
#include "../src/module_layout.hpp"   // DispatchMode
#include "../src/jit_memory.hpp"      // free_executable (via engine; included for clarity)

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

static int g_fail = 0;
static void ck(bool c, const char* m) {
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}
static bool contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Compiled module state (owns the finalized CompiledFns + dispatch table).
// ---------------------------------------------------------------------------
struct Compiled {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string, int> slots;
    std::unordered_map<std::string, NativeSig> natives;
    GlobalsBlock gb;
    StructLayoutTable layouts;
    Program prog;
    Compiled() : table(0) {}
    ~Compiled() {
        for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
    }
};

// Trap stub: record the reason on the per-call context_t (arrived in x19) +
// longjmp to that ctx's checkpoint. Mirrors thread_safety_test's ts_trap.
extern "C" void kr_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        if (detail) ctx->last_error = detail;
        if (ctx->has_checkpoint) EMBER_LONGJMP(ctx->checkpoint, 1);
    }
    std::abort();
}

// Build the CodeGenCtx fields common to all probes. enable_ir_backend=true so
// the ARM64 fallback reports the ir_backend_unavailable_reason (matching the
// keyed_dispatch_codegen_test ctx setup). safety ON (use_context_reg + trap
// stub) so a trap-stub rejection can be RUN + observed via the checkpoint.
static void base_ctx(CodeGenCtx& ctx, Compiled& m, bool safety_on) {
    ctx.globals_base = 0;
    ctx.dispatch_base = int64_t(m.table.base());
    ctx.natives = &m.natives;
    ctx.script_slots = &m.slots;
    ctx.structs = &m.layouts;
    ctx.enable_ir_backend = true;
    if (safety_on) {
        ctx.use_context_reg = true;
        ctx.trap_stub = (void*)&kr_trap;
        ctx.trap_ctx = nullptr;  // B1: ctx arrives in x19, not a baked ptr
        ctx.emit_budget_checks = false;  // no budget tick (just the call trap)
        ctx.emit_depth_checks = false;   // depth_leave is a no-op then
    } else {
        ctx.use_context_reg = false;
        ctx.emit_budget_checks = false;
        ctx.emit_depth_checks = false;
    }
}

// Parse + sema a source, injecting a keyed export for `other::foo` so the
// cross-module call resolves with dispatch_mode=Keyed (mode=1).
static bool parse_and_sema_keyed_target(const std::string& src, Compiled& m) {
    auto lr = tokenize(src, "<keyed-reject>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return false; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return false; }
    m.prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m.prog.funcs) { m.slots[fn.name] = si++; fn.slot = si - 1; }
    m.layouts = build_struct_layouts(m.prog);
    m.prog.string_xor_key = 0;
    // Inject a keyed export: `other` module exports `foo` (i64->i64) under
    // Keyed dispatch. Sema stamps cross_module_target_mode=1 on `other::foo()`.
    ModuleExportTable exports;
    ModuleExport exp;
    exp.fn_name = "foo";
    exp.ret = type_i64();
    exp.module_id = 1;
    exp.slot = 0;
    exp.dispatch_mode = DispatchMode::Keyed;  // <-- the target is keyed
    exports["other"].push_back(exp);
    OpOverloadTable ov;
    auto sr = sema(m.prog, m.natives, m.slots, 0, &ov, &m.layouts, &exports);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return false;
    }
    m.gb.base = 0;
    g_globals_for_codegen = &m.gb;
    m.table = DispatchTable(m.prog.funcs.size());
    return true;
}

// Compile every function in m->prog under ctx, finalize + install in the
// dispatch table. Returns false if any compile/finalize failed.
static bool compile_all(Compiled& m, CodeGenCtx& ctx) {
    for (auto& fn : m.prog.funcs) {
        CompiledFn cf = compile_func(fn, ctx);
        if (cf.bytes.empty()) {
            std::printf("FAIL: compile_func gave empty bytes for %s (nsr='%s')\n",
                        fn.name.c_str(), cf.non_serializable_reason.c_str());
            return false;
        }
        if (!finalize(cf)) {
            std::printf("FAIL: finalize (alloc_executable) for %s\n", fn.name.c_str());
            return false;
        }
        m.table.set(fn.slot, cf.entry);
        m.fns.push_back(std::move(cf));
    }
    return true;
}

// Run a no-arg i64 entry under a fresh context_t with a checkpoint. Sets
// *trapped=true if the trap stub fired (longjmp). Returns the i64 result.
static int64_t run_with_ctx(void* entry, context_t* ectx, bool* trapped) {
    *trapped = false;
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
    std::printf("=== keyed_dispatch ARM64 codegen rejection (audit H1) ===\n\n");

    // =====================================================================
    // (1) @obf_keyed -> compile_func_checked HARD FAILS on ARM64 (no x86
    //     tree-walker fallback for an obf function). cr.ok()==false + reason
    //     mentions IR backend unavailable + obf. No executable produced.
    // =====================================================================
    {
        const char* src =
            "@obf_keyed\n"
            "fn secret(x: i64) -> i64 { return x * 31; }\n"
            "fn main() -> i64 { return secret(2); }\n";
        auto m = std::make_unique<Compiled>();
        bool ok_so_far = true;
        auto lr = tokenize(src, "<obf-keyed>");
        if (!lr.ok) { ck(false, "[1] @obf_keyed: lex"); ok_so_far = false; }
        if (ok_so_far) {
            auto pr = parse(std::move(lr.toks));
            if (!pr.ok) { ck(false, "[1] @obf_keyed: parse"); ok_so_far = false; }
            else {
                m->prog = std::move(pr.program);
                int si = 0;
                for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = si - 1; }
                m->layouts = build_struct_layouts(m->prog);
                m->prog.string_xor_key = 0;
                OpOverloadTable ov;
                auto sr = sema(m->prog, m->natives, m->slots, 0, &ov, &m->layouts);
                if (!sr.ok) {
                    std::printf("FAIL [1]: sema (%zu errors):\n", sr.errors.size());
                    for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
                    ck(false, "[1] @obf_keyed: sema"); ok_so_far = false;
                }
                if (ok_so_far) {
                    m->gb.base = 0; g_globals_for_codegen = &m->gb;
                    m->table = DispatchTable(m->prog.funcs.size());
                    CodeGenCtx ctx; base_ctx(ctx, *m, /*safety_on=*/false);
                    for (auto& fn : m->prog.funcs) {
                        bool is_obf = false;
                        for (auto& a : fn.annotations) if (a.name == "obf_keyed") is_obf = true;
                        if (!is_obf) continue;
                        CompileResult cr = compile_func_checked(fn, ctx);
                        ck(!cr.ok(), "[1] @obf_keyed: compile_func_checked ok()==false (ARM64 hard error)");
                        ck(contains(cr.reason, "IR backend unavailable"),
                           "[1] @obf_keyed: reason mentions IR backend unavailable");
                        ck(contains(cr.reason, "obf"),
                           "[1] @obf_keyed: reason mentions obf");
                        ck(!cr.compiled.exec && cr.compiled.bytes.empty(),
                           "[1] @obf_keyed: no executable produced (bytes empty)");
                        std::printf("[INFO] [1] @obf_keyed rejection: %s\n", cr.reason.c_str());
                        break;
                    }
                }
            }
        }
    }

    // =====================================================================
    // (2) keyed caller -> keyed cross-module target: emit_arm64 lowers the
    //     call site to a TrapReason::BadCallTarget trap ("keyed cross-module
    //     call not yet supported"). Compile main with safety ON + a trap stub,
    //     run it via ember_call_void with a checkpoint, and assert the trap
    //     FIRES (trapped==true + last_trap==BadCallTarget) — proving the call
    //     was rejected at codegen, not silently miscompiled or crashed.
    // =====================================================================
    {
        const char* src =
            "fn main() -> i64 { return other::foo(); }\n";
        auto m = std::make_unique<Compiled>();
        if (!parse_and_sema_keyed_target(src, *m)) {
            ck(false, "[2] keyed->keyed: parse/sema");
        } else {
            CodeGenCtx ctx; base_ctx(ctx, *m, /*safety_on=*/true);
            // KEYED CALLER: ctx.keyed_dispatch set (keyed_caller() == true).
            KeyedDispatchCodegen kd{};
            kd.runtime_key = RuntimeKeyLocation::R15;
            kd.module_record = nullptr;  // same-module record not needed for the cross-module path
            ctx.keyed_dispatch = &kd;
            if (!compile_all(*m, ctx)) {
                ck(false, "[2] keyed->keyed: compile/finalize (no crash)");
            } else {
                // main's entry is in the dispatch table. Run it: the
                // other::foo() call site is a trap, so the trap stub fires.
                auto sit = m->slots.find("main");
                void* entry = (sit != m->slots.end()) ? m->table.get(sit->second) : nullptr;
                if (!entry) {
                    ck(false, "[2] keyed->keyed: main entry not found");
                } else {
                    context_t ectx; ectx.budget_remaining = 1'000'000'000LL;
                    ectx.max_call_depth = 64; ectx.last_trap = TrapReason::None;
                    bool trapped = false;
                    (void)run_with_ctx(entry, &ectx, &trapped);
                    ck(true, "[2] keyed->keyed: compile + run did NOT crash (reached rejection)");
                    ck(trapped, "[2] keyed->keyed: trap FIRED at the cross-module call site");
                    ck(ectx.last_trap == TrapReason::BadCallTarget,
                       "[2] keyed->keyed: last_trap == BadCallTarget (keyed cross-module rejected)");
                    std::printf("[INFO] [2] keyed->keyed: trapped=%d last_trap=%d\n",
                                (int)trapped, (int)ectx.last_trap);
                }
            }
        }
    }

    // =====================================================================
    // (3) legacy caller (NO keyed_dispatch) -> keyed cross-module target:
    //     rejected with non_serializable_reason "legacy-to-keyed cross-module
    //     call rejected at codegen" (Red 7: a legacy caller has no runtime-key
    //     contract). Assert the reason is recorded on the CompiledFn (no crash).
    // =====================================================================
    {
        const char* src =
            "fn main() -> i64 { return other::foo(); }\n";
        auto m = std::make_unique<Compiled>();
        if (!parse_and_sema_keyed_target(src, *m)) {
            ck(false, "[3] legacy->keyed: parse/sema");
        } else {
            CodeGenCtx ctx; base_ctx(ctx, *m, /*safety_on=*/false);
            // LEGACY CALLER: ctx.keyed_dispatch == nullptr (keyed_caller() == false).
            ctx.keyed_dispatch = nullptr;
            FuncDecl* main_fn = nullptr;
            for (auto& fn : m->prog.funcs) if (fn.name == "main") main_fn = &fn;
            if (!main_fn) {
                ck(false, "[3] legacy->keyed: main not found");
            } else {
                CompiledFn cf = compile_func(*main_fn, ctx);
                ck(true, "[3] legacy->keyed: compile did NOT crash (reached rejection)");
                ck(contains(cf.non_serializable_reason, "legacy-to-keyed"),
                   "[3] legacy->keyed: non_serializable_reason = 'legacy-to-keyed cross-module call rejected at codegen'");
                std::printf("[INFO] [3] legacy->keyed: nsr='%s' bytes=%zu\n",
                            cf.non_serializable_reason.c_str(), cf.bytes.size());
                if (cf.exec) free_executable(cf.exec);
            }
        }
    }

    std::printf("\nkeyed_dispatch_arm64_reject_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
