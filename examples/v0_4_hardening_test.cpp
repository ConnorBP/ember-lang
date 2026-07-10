// v0.4_hardening_test - regression tests pinning the red-team V5 (W^X JIT
// memory) and V6 (per-frame byte budget + int32 struct-sizing overflow)
// mitigations. Each red-team payload that previously crashed (SIGSEGV 139 /
// SIGILL 132 / silent 0-byte frame) must now be REJECTED at sema with a clear
// budget error, and the JIT page must be RX not RWX. A legit in-budget script
// must still sema-check OK (negative control).
//
// Self-contained: the malicious .ember payloads are inlined as string
// literals (no external test assets). The sema pipeline mirrors sema_check
// (resolve_imports -> tokenize -> parse -> slot-assign -> register six
// extensions -> build_struct_layouts -> string_xor_key=0 -> sema). The W^X
// probe allocates a JIT page via alloc_executable and queries its protection
// with VirtualQuery (must be PAGE_EXECUTE_READ, not PAGE_EXECUTE_READWRITE).
#include "import.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"     // compile_func, CodeGenCtx, g_globals_for_codegen (safe-trap runner)
#include "context.hpp"     // context_t, TrapReason
#include "engine.hpp"      // CompiledFn, finalize, free_executable
#include "dispatch_table.hpp"
#include "binding_builder.hpp"
#include "jit_memory.hpp"

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"

#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace ember;

static int g_fail = 0;
static void* g_reenter_entry = nullptr;
static bool g_reenter_enabled = true;
extern "C" int64_t test_native_reenter(int64_t n) {
    if (!g_reenter_enabled) return 7;
    using ScriptFn = int64_t(*)(int64_t);
    return reinterpret_cast<ScriptFn>(g_reenter_entry)(n);
}

static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

// Run parse+sema on an inlined source. Returns true if sema OK, false if
// sema produced errors. Mirrors sema_check's pipeline (extensions registered,
// string_xor_key=0 so no encrypted-rodata path).
static bool sema_ok(const std::string& src) {
    std::unordered_set<std::string> seen;
    std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); }
    catch (const std::exception&) { return false; }
    auto lr = tokenize(resolved, "<v0.4_test>");
    if (!lr.ok) return false;
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) return false;
    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = slots[fn.name]; }
    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
    ext_vec::register_natives(natives);    ext_quat::register_natives(natives);
    ext_mat::register_natives(natives);     ext_string::register_natives(natives);
    ext_array::register_natives(natives);   ext_math::register_natives(natives);
    ext_vec::register_overloads(overloads); ext_quat::register_overloads(overloads);
    ext_mat::register_overloads(overloads); ext_string::register_overloads(overloads);
    auto layouts = build_struct_layouts(pr.program);
    pr.program.string_xor_key = 0;
    auto sr = sema(pr.program, natives, slots, 0, &overloads, &layouts);
    return sr.ok;
}

// Run a script under the v0.4 safe-execution surface (context_t + trap stub +
// budget + depth), calling `main`. Returns the TrapReason that fired, or
// TrapReason::None if the script completed. Mirrors ember_cli's wiring.
extern "C" void test_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1);
    }
    std::abort();
}
static ember::TrapReason run_under_safetrap(const std::string& src, int64_t budget = 100'000'000) {
    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); } catch (...) { return ember::TrapReason::None; }
    auto lr = tokenize(resolved, "<t>"); if (!lr.ok) return ember::TrapReason::None;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return ember::TrapReason::None;
    std::unordered_map<std::string,int> slots; int si=0;
    for(auto&fn:pr.program.funcs){slots[fn.name]=si++;fn.slot=slots[fn.name];}
    std::unordered_map<std::string,ember::NativeSig> natives; ember::OpOverloadTable ov;
    ext_vec::register_natives(natives);ext_quat::register_natives(natives);ext_mat::register_natives(natives);
    ext_string::register_natives(natives);ext_array::register_natives(natives);ext_math::register_natives(natives);
    ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
    auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    if(!sema(pr.program,natives,slots,0,&ov,&layouts).ok) return ember::TrapReason::None;
    ember::GlobalsBlock gb; std::vector<uint8_t> gbs(0); gb.base=int64_t(gbs.data()); ember::g_globals_for_codegen=&gb;
    ember::DispatchTable table(pr.program.funcs.size()); ember::CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(table.base());
    ctx.natives=&natives; ctx.script_slots=&slots; ctx.structs=&layouts;
    ember::context_t ectx; ectx.budget_remaining=budget; ectx.max_call_depth=8;
    ctx.trap_stub=(void*)&test_trap; ctx.trap_ctx=&ectx;
    ctx.budget_ptr=&ectx.budget_remaining; ctx.emit_budget_checks=true;
    ctx.depth_ptr=&ectx.call_depth; ctx.max_call_depth=8; ctx.emit_depth_checks=true;
    std::vector<ember::CompiledFn> fns;
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); table.set(fn.slot,cf.entry); fns.push_back(std::move(cf));}
    auto sit=slots.find("main"); if(sit==slots.end()) return ember::TrapReason::None;
    void* entry=table.get(sit->second); if(!entry) return ember::TrapReason::None;
    ectx.has_checkpoint=true;
    if (__builtin_setjmp(ectx.checkpoint)) { return ectx.last_trap; }  // trap fired
    using F0=int64_t(*)(); reinterpret_cast<F0>(entry)();
    ectx.has_checkpoint=false;
    for(auto&fn:fns) if(fn.exec) free_executable(fn.exec);
    return ember::TrapReason::None;
}

// Prove native depth is simultaneous nesting, not a sequential call count:
// main makes two completed native->script calls, then native -> script bounce
// -> native -> ... remains nested until the deepest native returns. The first
// run traps at a tiny combined-depth limit; after longjmp, reset_for_call
// discards abandoned depth and the same entry completes three sequential native
// calls with re-entry disabled (which would trap if calls accumulated).
static bool native_reentry_depth_test() {
    const std::string src =
        "fn bounce(n: i64) -> i64 { if (n <= 0) { return 0; } return reenter(n - 1); }\n"
        "fn main() -> i64 { let a = reenter(0); let b = reenter(0); return reenter(6); }\n";
    auto lr = tokenize(src, "<native-depth>"); if (!lr.ok) return false;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return false;
    std::unordered_map<std::string,int> slots; int si=0;
    for(auto&fn:pr.program.funcs){slots[fn.name]=si++;fn.slot=slots[fn.name];}
    std::unordered_map<std::string,ember::NativeSig> natives;
    ember::NativeSig reenter;
    reenter.name="reenter"; reenter.fn_ptr=(void*)&test_native_reenter;
    reenter.ret=make_prim(Prim::I64); reenter.params.push_back(make_prim(Prim::I64));
    natives[reenter.name]=reenter;
    ember::OpOverloadTable ov; auto layouts=build_struct_layouts(pr.program);
    pr.program.string_xor_key=0;
    if(!sema(pr.program,natives,slots,0,&ov,&layouts).ok) return false;
    ember::GlobalsBlock gb; std::vector<uint8_t> gbs; gb.base=int64_t(gbs.data());
    ember::g_globals_for_codegen=&gb;
    ember::DispatchTable table(pr.program.funcs.size()); ember::context_t ectx;
    ectx.budget_remaining=1'000'000; ectx.max_call_depth=3;
    ember::CodeGenCtx ctx; ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(table.base());
    ctx.natives=&natives; ctx.script_slots=&slots; ctx.structs=&layouts;
    ctx.trap_stub=(void*)&test_trap; ctx.trap_ctx=&ectx;
    ctx.budget_ptr=&ectx.budget_remaining; ctx.emit_budget_checks=true;
    ctx.depth_ptr=&ectx.call_depth; ctx.max_call_depth=3; ctx.emit_depth_checks=true;
    ctx.use_context_reg=true; // B1: native re-entry inherits callee-saved r14
    std::vector<ember::CompiledFn> fns;
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx);finalize(cf);table.set(fn.slot,cf.entry);fns.push_back(std::move(cf));}
    g_reenter_entry=table.get(slots["bounce"]); void* main_entry=table.get(slots["main"]);
    g_reenter_enabled=true; ectx.has_checkpoint=true;
    bool trapped=false;
    if (__builtin_setjmp(ectx.checkpoint)) {
        trapped=ectx.last_trap==ember::TrapReason::StackOverflow;
    } else {
        (void)ember_call_void(main_entry, &ectx);
    }
    ectx.has_checkpoint=false;
    const bool abandoned_depth_observed = ectx.call_depth > 0;
    ectx.reset_for_call();
    const bool reset = ectx.call_depth == 0 && ectx.last_trap == ember::TrapReason::None;
    g_reenter_enabled=false; ectx.budget_remaining=1'000'000; ectx.has_checkpoint=true;
    int64_t recovered=-1;
    if (__builtin_setjmp(ectx.checkpoint) == 0) {
        recovered=ember_call_void(main_entry, &ectx);
    }
    ectx.has_checkpoint=false; g_reenter_entry=nullptr;
    for(auto&fn:fns)if(fn.exec)free_executable(fn.exec);
    return trapped && abandoned_depth_observed && reset && recovered==7 && ectx.call_depth==0;
}

// Safety instrumentation is an explicit compile profile. Supplying all legacy
// baked pointers while leaving both flags false must produce byte-identical code
// to a context with no safety state at all; enabling the flags must add code.
static bool disabled_safety_checks_are_zero_overhead() {
    const std::string src =
        "fn main() -> i64 { let x = reenter(0); while (false) { let y = 1; } return x; }\n";
    auto lr = tokenize(src, "<safety-flags>"); if (!lr.ok) return false;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok || pr.program.funcs.size()!=1) return false;
    std::unordered_map<std::string,int> slots{{"main",0}}; pr.program.funcs[0].slot=0;
    std::unordered_map<std::string,ember::NativeSig> natives;
    ember::NativeSig reenter; reenter.name="reenter"; reenter.fn_ptr=(void*)&test_native_reenter;
    reenter.ret=make_prim(Prim::I64); reenter.params.push_back(make_prim(Prim::I64));
    natives[reenter.name]=reenter;
    ember::OpOverloadTable ov; auto layouts=build_struct_layouts(pr.program);
    pr.program.string_xor_key=0;
    if(!sema(pr.program,natives,slots,0,&ov,&layouts).ok) return false;
    ember::CodeGenCtx base; base.natives=&natives; base.script_slots=&slots; base.structs=&layouts;
    auto plain=compile_func(pr.program.funcs[0],base);
    ember::context_t ectx;
    ember::CodeGenCtx disabled=base;
    disabled.trap_stub=(void*)&test_trap; disabled.trap_ctx=&ectx;
    disabled.budget_ptr=&ectx.budget_remaining; disabled.depth_ptr=&ectx.call_depth;
    disabled.max_call_depth=3;
    auto flags_off=compile_func(pr.program.funcs[0],disabled);
    disabled.emit_budget_checks=true; disabled.emit_depth_checks=true;
    auto flags_on=compile_func(pr.program.funcs[0],disabled);
    return plain.bytes==flags_off.bytes && flags_on.bytes.size()>flags_off.bytes.size();
}

int main() {
    std::printf("=== v0.4 hardening regression (red-team V5 + V6) ===\n");

    // --- V6-DoS: large fixed-array local must be REJECTED at sema ---
    // Red-team payload: `fn main()->i64 { let a: u8[65536]; return 0; }`
    // Pre-fix: SIGSEGV (exit 139), no per-frame cap. Post-fix: sema budget error.
    check(!sema_ok("fn main() -> i64 { let a: u8[65536]; return 0; }\n"),
          "V6-DoS: u8[65536] local rejected at sema (was SIGSEGV)");

    // --- V6-DoS negative control: a small in-budget array must ACCEPT ---
    check(sema_ok("fn main() -> i64 { let a: u8[100]; return a[0] as i64; }\n"),
          "V6-DoS negative control: u8[100] in-budget sema OK");

    // --- V6-overflow: struct field whose byte_size overflows int32 ---
    // Red-team payload: `struct S { big: i64[1073741824]; }` + `let s: S;`
    // Pre-fix: layout.size silently wraps to 0 -> 0-byte frame slot (latent
    // arbitrary write once field-of-struct array indexing ships). Post-fix:
    // any `let` of the struct is rejected by the per-frame budget.
    check(!sema_ok("struct S { big: i64[1073741824]; }\n"
                   "fn main() -> i64 { let s: S; return 0; }\n"),
          "V6-overflow: let of overflowing struct rejected (was silent 0-byte slot)");

    // --- V6-overflow negative control: a normal struct must ACCEPT ---
    check(sema_ok("struct P { x: i32; y: i32; }\n"
                  "fn main() -> i64 { let p: P; return 0; }\n"),
          "V6-overflow negative control: normal struct sema OK");

    // --- V5: W^X JIT memory - page must be RX, not RWX ---
    // Red-team finding: alloc_executable used PAGE_EXECUTE_READWRITE (RWX).
    // Post-fix: VirtualAlloc RW -> memcpy -> VirtualProtect RX. Verify the
    // published page is PAGE_EXECUTE_READ by querying its protection.
    std::vector<uint8_t> code(16, 0xC3); // rets
    void* page = alloc_executable(code);
    MEMORY_BASIC_INFORMATION mbi{};
    bool queried = page && VirtualQuery(page, &mbi, sizeof(mbi)) != 0;
    check(queried, "V5: alloc_executable + VirtualQuery succeeded");
    if (queried) {
        check(mbi.Protect == PAGE_EXECUTE_READ,
              "V5: JIT page is PAGE_EXECUTE_READ (W^X enforced, was RWX)");
        check(mbi.Protect != PAGE_EXECUTE_READWRITE,
              "V5: JIT page is NOT PAGE_EXECUTE_READWRITE (RWX eliminated)");
    }
    if (page) free_executable(page);

    // --- V7: @obf_keyed forced crash is now a RECOVERABLE trap, not SIGILL ---
    // Red-team V7: @obf_keyed + call -> ud2 -> SIGILL (exit 132), process death.
    // Post-fix: the gate routes through the trap stub -> longjmp to checkpoint
    // -> recoverable error (TrapReason::IllegalInstruction), no crash.
    check(run_under_safetrap("@obf_keyed\nfn b() -> i64 { return 0; }\n"
                            "fn main() -> i64 { return b(); }\n")
          == ember::TrapReason::IllegalInstruction,
          "V7: @obf_keyed traps recoverably (was SIGILL 132 process death)");

    // --- Instruction budget: a runaway loop TRAPS, does not hang ---
    // A tight `while(true){...}` with a tiny budget must exhaust at a back-edge
    // and trap (TrapReason::BudgetExceeded), not loop forever. Pre-fix: hang.
    check(run_under_safetrap("fn main() -> i64 { while (true) { let x: i64 = 1+1+1+1+1; } return 0; }\n",
                            1000)  // tiny budget -> traps within ~200 iters
          == ember::TrapReason::BudgetExceeded,
          "Instruction budget: runaway loop traps (was infinite hang)");

    // M4: no loop and no call are involved. The recursive body estimator is
    // charged at function entry, so this formerly unaccounted straight-line
    // work exhausts a budget smaller than its body cost.
    check(run_under_safetrap("fn main() -> i64 { let a=1; let b=2; let c=3; let d=4; return a+b+c+d; }\n", 3)
          == ember::TrapReason::BudgetExceeded,
          "Instruction budget: straight-line function traps at entry");

    // --- Stack-depth guard: infinite recursion TRAPS, does not SIGSEGV ---
    // `r(){ return r(); }` with max_call_depth=8 must hit the depth guard and
    // trap (TrapReason::StackOverflow), not overflow the native stack (SIGSEGV).
    check(run_under_safetrap("fn r() -> i64 { return r(); }\n"
                            "fn main() -> i64 { return r(); }\n")
          == ember::TrapReason::StackOverflow,
          "Stack-depth guard: infinite recursion traps (was SIGSEGV)");

    check(native_reentry_depth_test(),
          "Combined depth: nested native re-entry traps recoverably and resets");

    check(disabled_safety_checks_are_zero_overhead(),
          "Safety compile flags: disabled budget/depth checks add zero code");

    // --- negative control: a well-behaved script completes with NO trap ---
    check(run_under_safetrap("fn fib(n: i64) -> i64 { if (n <= 1) { return n; } return fib(n-1)+fib(n-2); }\n"
                            "fn main() -> i64 { return fib(6); }\n")
          == ember::TrapReason::None,
          "Safe-execution negative control: fib(6) completes with no trap");

    std::printf("\nv0.4 hardening test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
