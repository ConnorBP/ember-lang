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

// H-M4-2 regression R4: 300 native calls in a straight line under budget=3 must
// trap (BudgetExceeded). Pre-fix the per-statement constant counted the whole
// `return z()+z()+...+z()` as ONE statement (cost ~2), so it completed under
// budget=3. Post-fix each CallExpr site charges setup+call+arg-marshal, so 300
// calls far exceed 3. A trivial registered native keeps the trusted-native
// body time out of the unit (only the JIT call-site setup is charged).
extern "C" int64_t n_zero_r4() { return 0; }
static bool budget_300_native_calls_trap() {
    // Build it explicitly: 300 z() calls added left-associatively.
    std::string body = "fn main() -> i64 { return ";
    for (int i = 0; i < 300; ++i) { if (i) body += "+"; body += "z()"; }
    body += "; }\n";
    auto lr = tokenize(body, "<r4>"); if (!lr.ok) return false;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return false;
    std::unordered_map<std::string,int> slots{{"main",0}}; pr.program.funcs[0].slot=0;
    std::unordered_map<std::string,ember::NativeSig> natives;
    ember::NativeSig z; z.name="z"; z.fn_ptr=(void*)&n_zero_r4; z.ret=make_prim(Prim::I64);
    natives[z.name]=z;
    ember::OpOverloadTable ov; auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    if(!sema(pr.program,natives,slots,0,&ov,&layouts).ok) return false;
    ember::GlobalsBlock gb; std::vector<uint8_t> gbs; gb.base=int64_t(gbs.data());
    ember::g_globals_for_codegen=&gb;
    ember::DispatchTable table(pr.program.funcs.size()); ember::context_t ectx;
    ectx.budget_remaining=3; ectx.max_call_depth=64;
    ember::CodeGenCtx ctx; ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(table.base());
    ctx.natives=&natives; ctx.script_slots=&slots; ctx.structs=&layouts;
    ctx.trap_stub=(void*)&test_trap; ctx.trap_ctx=&ectx;
    ctx.budget_ptr=&ectx.budget_remaining; ctx.emit_budget_checks=true;
    ctx.depth_ptr=&ectx.call_depth; ctx.max_call_depth=64; ctx.emit_depth_checks=true;
    std::vector<ember::CompiledFn> fns;
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx);finalize(cf);table.set(fn.slot,cf.entry);fns.push_back(std::move(cf));}
    ectx.has_checkpoint=true;
    bool trapped=false;
    if (__builtin_setjmp(ectx.checkpoint)) trapped = (ectx.last_trap==ember::TrapReason::BudgetExceeded);
    else reinterpret_cast<int64_t(*)()>(table.get(0))();
    ectx.has_checkpoint=false;
    for(auto&fn:fns)if(fn.exec)free_executable(fn.exec);
    return trapped;
}

// H-M4-2 regression R5: eight 8KiB aggregate copies under budget=30 must
// trap (BudgetExceeded). Pre-fix the per-statement constant counted the whole
// `return eat(b)+...+eat(b)` as ONE statement (cost ~2), so it completed under
// budget=30. Post-fix each by-value struct argument charges its byte-copy cost
// (8KiB / 8 = 1024), so 8 copies far exceed 30. `eat` is a SCRIPT function (not
// a native): sema rejects native by-value aggregate args > 8 bytes, so the
// aggregate copy happens at the script-to-script call site, exactly where the
// audit's probe exercises it.
static bool budget_aggregate_copies_trap() {
    std::string s =
        "struct Big { a: u8[8192]; }\n"
        "fn eat(b: Big) -> i64 { return 1; }\n"
        "fn main() -> i64 { let b: Big; return ";
    for (int i = 0; i < 8; ++i) { if (i) s += "+"; s += "eat(b)"; }
    s += "; }\n";
    auto lr = tokenize(s, "<r5>"); if (!lr.ok) return false;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return false;
    std::unordered_map<std::string,int> slots; int si=0;
    for(auto&f:pr.program.funcs){slots[f.name]=si++;f.slot=slots[f.name];}
    std::unordered_map<std::string,ember::NativeSig> natives;  // no natives: eat is a script fn
    ember::OpOverloadTable ov; auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    if(!sema(pr.program,natives,slots,0,&ov,&layouts).ok) return false;
    ember::GlobalsBlock gb; std::vector<uint8_t> gbs; gb.base=int64_t(gbs.data());
    ember::g_globals_for_codegen=&gb;
    ember::DispatchTable table(pr.program.funcs.size()); ember::context_t ectx;
    ectx.budget_remaining=30; ectx.max_call_depth=64;
    ember::CodeGenCtx ctx; ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(table.base());
    ctx.natives=&natives; ctx.script_slots=&slots; ctx.structs=&layouts;
    ctx.trap_stub=(void*)&test_trap; ctx.trap_ctx=&ectx;
    ctx.budget_ptr=&ectx.budget_remaining; ctx.emit_budget_checks=true;
    ctx.depth_ptr=&ectx.call_depth; ctx.max_call_depth=64; ctx.emit_depth_checks=true;
    std::vector<ember::CompiledFn> fns;
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx);finalize(cf);table.set(fn.slot,cf.entry);fns.push_back(std::move(cf));}
    auto main_it=slots.find("main"); if(main_it==slots.end()) return false;
    void* entry=table.get(main_it->second); if(!entry) return false;
    ectx.has_checkpoint=true;
    bool trapped=false;
    if (__builtin_setjmp(ectx.checkpoint)) trapped = (ectx.last_trap==ember::TrapReason::BudgetExceeded);
    else reinterpret_cast<int64_t(*)()>(entry)();
    ectx.has_checkpoint=false;
    for(auto&fn:fns)if(fn.exec)free_executable(fn.exec);
    return trapped;
}

// M-M4-3 regression: two contexts sharing ONE compiled body, with the runtime
// context_t::max_call_depth set differently per context, must observe DIFFERENT
// behavior from the same compiled code. Compile once with use_context_reg=true
// (B1) and a compile-time max_call_depth (64) that does NOT match either
// runtime value; then run the same entry against a context with max=1 (must
// trap on the first recursive edge) and a context with max=100 (must permit
// r(5) to complete). Pre-fix B1 baked the compile-time max as imm32, so both
// contexts used 64 and the runtime value was ignored.
static bool b1_per_context_max_call_depth() {
    const std::string src =
        "fn r(n: i64) -> i64 { if (n <= 0) { return 0; } return r(n - 1); }\n"
        "fn main() -> i64 { return r(5); }\n";
    auto lr = tokenize(src, "<m4-3>"); if (!lr.ok) return false;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return false;
    std::unordered_map<std::string,int> slots; int si=0;
    for(auto&f:pr.program.funcs){slots[f.name]=si++;f.slot=slots[f.name];}
    std::unordered_map<std::string,ember::NativeSig> natives; ember::OpOverloadTable ov;
    auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    if(!sema(pr.program,natives,slots,0,&ov,&layouts).ok) return false;
    ember::GlobalsBlock gb; std::vector<uint8_t> gbs; gb.base=int64_t(gbs.data());
    ember::g_globals_for_codegen=&gb;
    ember::DispatchTable table(pr.program.funcs.size());
    ember::CodeGenCtx ctx; ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(table.base());
    ctx.natives=&natives; ctx.script_slots=&slots; ctx.structs=&layouts;
    // B1: use_context_reg=true. Compile-time max_call_depth=64 must NOT match
    // either runtime value (1 or 100), so the pre-fix imm32 path would ignore both.
    ctx.use_context_reg=true; ctx.emit_depth_checks=true; ctx.max_call_depth=64;
    ctx.trap_stub=(void*)&test_trap;
    std::vector<ember::CompiledFn> fns;
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx);finalize(cf);table.set(fn.slot,cf.entry);fns.push_back(std::move(cf));}
    auto main_it=slots.find("main"); if(main_it==slots.end()) return false;
    void* entry=table.get(main_it->second); if(!entry) return false;

    // Run A: runtime max_call_depth=1 -> the first recursive edge (r(5)->r(4))
    // pushes depth to 1, which is >= max 1, so it must trap (StackOverflow).
    // ember_call_void installs r14 = &context (B1 requires it; a raw cast would
    // leave r14 garbage and fault).
    ember::context_t ectxA; ectxA.budget_remaining=1'000'000; ectxA.max_call_depth=1;
    ctx.trap_ctx=&ectxA; ectxA.has_checkpoint=true;
    bool a_trapped=false;
    if (__builtin_setjmp(ectxA.checkpoint)) a_trapped=(ectxA.last_trap==ember::TrapReason::StackOverflow);
    else ember_call_void(entry, &ectxA);
    ectxA.has_checkpoint=false;

    // Run B: runtime max_call_depth=100 -> r(5) nests 6 deep, well under 100,
    // so it must complete with no trap.
    ember::context_t ectxB; ectxB.budget_remaining=1'000'000; ectxB.max_call_depth=100;
    ctx.trap_ctx=&ectxB; ectxB.has_checkpoint=true;
    bool b_trapped=false; int64_t b_val=-999;
    if (__builtin_setjmp(ectxB.checkpoint)) b_trapped=true;
    else b_val=ember_call_void(entry, &ectxB);
    ectxB.has_checkpoint=false;

    for(auto&fn:fns)if(fn.exec)free_executable(fn.exec);
    // Same compiled body, different runtime max_call_depth -> different behavior.
    return a_trapped && !b_trapped && b_val==0;
}

// H-§10-1: direct host/JIT call asserting the FULL rax for a folded i32.
// Compiles `enum E{Top=1073741824,One=1} fn folded() -> i32 { return E::Top<<E::One; }`
// (enum members lower to typed i32 IntLit at sema.cpp:503-506, so the fold path
// is reachable with a typed-i32 result) and calls it via a raw cast that returns
// the full 64-bit rax (NOT a normalizing call). Pre-fix the fold returned before
// normalize_rax, so rax held 0x0000000080000000 (2147483648); post-fix the fold
// normalizes to i32 first, so rax holds 0xffffffff80000000 (-2147483648). Also
// compiles a runtime (non-folded) variant and asserts the two full-register
// values are EQUAL (both sign-normalized), which fails pre-fix. This is the
// scrutiny's shift_enum_direct_probe shape, integrated into the hardening
// suite: the test fails with the fix reverted (returns 2147483648).
static bool fold_i32_sign_normalize_direct_probe() {
    const char* src =
        "enum E{Top=1073741824,One=1}"
        " fn folded() -> i32 { return E::Top<<E::One; }"
        " fn runtime(x:i32,c:i32) -> i32 { return x<<c; }";
    auto lr = tokenize(src, "<fold-i32-probe>");
    if (!lr.ok) return false;
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) return false;
    std::unordered_map<std::string,int> slots; int si=0;
    for(auto&fn:pr.program.funcs){slots[fn.name]=si++;fn.slot=slots[fn.name];}
    std::unordered_map<std::string,ember::NativeSig> natives; ember::OpOverloadTable ov;
    auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    if(!sema(pr.program,natives,slots,0,&ov,&layouts).ok) return false;
    ember::GlobalsBlock gb; std::vector<uint8_t> gbs(0); gb.base=int64_t(gbs.data());
    ember::g_globals_for_codegen=&gb;
    ember::DispatchTable table(pr.program.funcs.size()); ember::CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(table.base());
    ctx.natives=&natives; ctx.script_slots=&slots; ctx.structs=&layouts;
    // No budget/depth/trap: a trivial fold has no runtime hazards; the raw
    // cast below returns the FULL rax (what a host caller observes at the
    // ABI boundary), which is the whole point of this probe.
    auto cf=compile_func(pr.program.funcs[0],ctx);  // folded()
    auto cr=compile_func(pr.program.funcs[1],ctx);  // runtime(x,c)
    if(!finalize(cf)||!finalize(cr)) return false;
    using F0=int64_t(*)();
    using F2=int64_t(*)(int64_t,int64_t);
    int64_t folded_ret = reinterpret_cast<F0>(cf.entry)();
    int64_t runtime_ret = reinterpret_cast<F2>(cr.entry)(1073741824,1);
    bool full_equal = (folded_ret == runtime_ret);
    bool folded_ok = (folded_ret == -2147483648LL);
    if (cf.exec) free_executable(cf.exec);
    if (cr.exec) free_executable(cr.exec);
    return folded_ok && full_equal;
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

    // === g6 remediation regressions (H-M4-1, H-M4-2, M-M4-3) ===
    // Each would FAIL with the fix reverted (non-circular): pre-fix the
    // while-continue hole let the loop complete, the coarse per-statement
    // charging let large straight-line work complete under tiny budgets, and
    // B1 baked the compile-time max so runtime per-context max was ignored.

    // (R1) H-M4-1: while(true)+continue under budget=20 MUST trap. Pre-fix
    // `continue` jumped to `top` (the condition), skipping the back-edge
    // charge, so a 1M-iteration loop completed. Post-fix a dedicated charged
    // latch is the continue target, so the charge runs before re-checking.
    check(run_under_safetrap("fn main() -> i64 { while (true) { let x: i64 = 1+1+1+1+1; continue; } return 0; }\n",
                            20)
          == ember::TrapReason::BudgetExceeded,
          "H-M4-1: while-true+continue traps under budget=20 (was: completed)");

    // (R2) H-M4-1 negative control: a finite while+continue under a SUFFICIENT
    // budget MUST complete (return the expected value, NOT trap). Proves the
    // latch didn't break normal while+continue.
    check(run_under_safetrap("fn main() -> i64 { let mut i = 0; while (i < 5) { i = i + 1; continue; } return i; }\n",
                            10000)
          == ember::TrapReason::None,
          "H-M4-1 negative control: finite while+continue completes (latch intact)");

    // (R3) H-M4-2: a single 2000-term expression under budget=5 MUST trap.
    // Pre-fix block_cost counted the whole `return x+x+...+x` as ONE statement
    // (~2), so it completed under budget=5. Post-fix expr_cost counts each
    // lowered BinExpr/Ident node, so 2000 terms far exceed 5.
    {
        std::string e = "fn main() -> i64 { let mut x = 1; return ";
        for (int i = 0; i < 2000; ++i) { if (i) e += "+"; e += "x"; }
        e += "; }\n";
        check(run_under_safetrap(e, 5) == ember::TrapReason::BudgetExceeded,
              "H-M4-2: 2000-term expression traps under budget=5 (was: completed)");
    }

    // (R4) H-M4-2: 300 native calls in a straight line under budget=3 MUST
    // trap. Pre-fix the per-statement constant let it complete; post-fix each
    // call site charges setup+call+arg-marshal.
    check(budget_300_native_calls_trap(),
          "H-M4-2: 300 native calls trap under budget=3 (was: completed)");

    // (R5) H-M4-2: eight 8KiB aggregate copies under budget=30 MUST trap.
    // Pre-fix the per-statement constant let it complete; post-fix each
    // by-value struct arg charges its byte-copy cost (8KiB/8 = 1024).
    check(budget_aggregate_copies_trap(),
          "H-M4-2: 8x8KiB aggregate copies trap under budget=30 (was: completed)");

    // M-M4-3: two contexts sharing one compiled body, runtime max_call_depth
    // 1 (traps) vs 100 (permits) must observe DIFFERENT behavior from the same
    // code. Pre-fix B1 baked the compile-time max as imm32 and ignored the
    // per-context field.
    check(b1_per_context_max_call_depth(),
          "M-M4-3: B1 per-context max_call_depth observed (1 traps, 100 permits)");

    // H-§10-1: a folded i32 (here an enum i32 value shifted into the sign
    // bit, the reachable path sema.cpp:503-506 produces by lowering enum
    // members to typed i32 IntLit) MUST reach the host/ABI boundary
    // sign-normalized to the full register, exactly as the runtime integer
    // path does. Pre-fix the fold early return emitted the immediate and
    // returned BEFORE normalize_rax(lt), so the i32 sign bit landed
    // zero-extended (0x0000000080000000 = 2147483648) instead of
    // sign-normalized (0xffffffff80000000 = -2147483648). This is the direct
    // host-call shape (full rax), NOT an in-language i32 comparison (which
    // would observe only the normalized low 32 bits and circularly hide the
    // defect). Fails with the fix reverted.
    check(fold_i32_sign_normalize_direct_probe(),
          "H-§10-1: folded i32 enum shift returns -2147483648 (full rax, sign-normalized)");

    std::printf("\nv0.4 hardening test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
