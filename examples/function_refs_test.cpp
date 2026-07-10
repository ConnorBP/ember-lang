// function_refs_test - v1.0 Tier 2 first-class function references
// (plan_FUNCTION_REFS.md). Proves:
//   A. `&fn` bakes the slot as an i64 literal; the `fn` type tag distinguishes
//      it from a plain i64 (i64<->fn assignment is a sema error).
//   B. `handle(args)` dispatches to the right fn through the runtime i64,
//      including multi-arg (Win64 rcx/rdx) and recursion via handle.
//   C. The REDSHELL guard #6: a forged/out-of-range/in-range-unregistered
//      handle traps via emit_trap(BadCallTarget) (longjmp to the checkpoint),
//      NOT a raw call-rax-of-script-value crash. This is the load-bearing
//      safety invariant the feature opens.
//
// Uses the BAKED-ptr codegen path (use_context_reg=false, the default every
// existing host uses) — the guard + indirect dispatch are path-agnostic.
// Mirrors v0_4_hardening_test's harness (resolve_imports -> tokenize -> parse
// -> slot-assign -> register six extensions -> sema -> compile -> call under
// the safe-trap surface), PLUS building the fn allowlist + wiring
// ctx.fn_allowlist_base / fn_slot_count.
#include "import.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "context.hpp"
#include "engine.hpp"
#include "globals.hpp"
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
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace ember;

static int g_fail = 0;
static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

extern "C" void fr_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1);
    }
    std::abort();
}

struct FRModule {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string,int> slots;
    void* main_entry = nullptr;
    std::unordered_map<std::string, NativeSig> natives;
    std::vector<uint8_t> allowlist;   // owned; keep alive for the module's lifetime
    int slot_count = 0;
    // The baked-ptr trap_ctx is the ADDRESS of this context, baked at compile.
    // The run MUST use this same context (same address) or the longjmp lands
    // wrong. So the module owns it + run reuses it.
    ember::context_t ctx{};
};

// A test-only host native: returns a FORGED fn handle (an i64 with the
// is_fn_handle tag) for an arbitrary slot number, so a script can call
// handle() on a handle that sema DIDN'T produce from a `&fn`. This is how
// the guard's range + bit tests get exercised (plan §8 C1/C2).
// Signature: i64 forge_fn_handle(i64 slot) -> i64 (tagged fn).
static int64_t g_forge_return = 0;  // the last forged value (for test inspection)
extern "C" int64_t n_forge_fn_handle(int64_t slot) {
    // We don't set is_fn_handle on the Type here (natives return plain i64 by
    // signature); the test instead assigns the returned i64 to a `fn`-typed
    // var via a *second* test native `forge_fn_typed(slot)` whose declared
    // return type is `fn`. See below.
    g_forge_return = slot;
    return slot;
}

// Compile `main()->i64`. Returns false on lex/parse/sema/compile error
// (prints the error). Wires the fn allowlist so the guard is live.
static bool compile_fr(const std::string& src, FRModule& m) {
    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); } catch (...) { return false; }
    auto lr = tokenize(resolved, "<t>"); if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return false; }
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return false; }
    int si=0; for(auto&fn:pr.program.funcs){m.slots[fn.name]=si++;fn.slot=m.slots[fn.name];}
    m.slot_count = si;
    ember::OpOverloadTable ov;
    ext_vec::register_natives(m.natives);ext_quat::register_natives(m.natives);
    ext_mat::register_natives(m.natives);ext_string::register_natives(m.natives);
    ext_array::register_natives(m.natives);ext_math::register_natives(m.natives);
    ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);
    ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
    auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    auto sr=sema(pr.program,m.natives,m.slots,0,&ov,&layouts);
    if(!sr.ok){ std::printf("FAIL: sema (%zu errors):\n",sr.errors.size());
        for(auto&e:sr.errors) std::printf("  line %u: %s\n",e.line,e.msg.c_str()); return false; }

    ember::GlobalsBlock gb; std::vector<uint8_t> gbs(pr.program.globals.size()*8, 0);
    gb.base=int64_t(gbs.data());
    { uint32_t gi=0; for (auto& g : pr.program.globals) { gb.index[g.name]=gi++; gb.types[g.name]=g.ty.get(); } }
    eval_global_initializers(pr.program, GlobalInitCtx{gbs, gb.index, gb.types});
    ember::g_globals_for_codegen = &gb;
    m.table = DispatchTable(pr.program.funcs.size());
    ember::CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(m.table.base());
    ctx.natives=&m.natives; ctx.script_slots=&m.slots; ctx.structs=&layouts;
    // build + wire the fn allowlist (plan §5.2). The guard validates handles
    // against this bitset; a slot not in script_slots has its bit clear.
    m.allowlist = build_fn_allowlist(m.slots, m.slot_count);
    ctx.fn_allowlist_base = int64_t(m.allowlist.data());
    ctx.fn_slot_count = int64_t(m.slot_count);
    // safe-trap surface (baked-ptr path, the default). The trap_ctx address is
    // baked into the JIT'd code, so it MUST be m.ctx (stable module lifetime).
    m.ctx.budget_remaining=2'000'000'000LL; m.ctx.max_call_depth=64; m.ctx.call_depth=0;
    ctx.trap_stub=(void*)&fr_trap; ctx.trap_ctx=&m.ctx;
    ctx.budget_ptr=&m.ctx.budget_remaining; ctx.emit_budget_checks=true;
    ctx.depth_ptr=&m.ctx.call_depth; ctx.max_call_depth=64; ctx.emit_depth_checks=true;
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); m.table.set(fn.slot,cf.entry); m.fns.push_back(std::move(cf));}
    auto sit=m.slots.find("main");
    if(sit==m.slots.end()) return false;
    m.main_entry = m.table.get(sit->second);
    return m.main_entry != nullptr;
}

// Run main under the module's own context (the baked trap_ctx); returns the
// i64 result or *trapped=true (with m.ctx.last_trap set).
static int64_t run_main(FRModule& m, bool* trapped) {
    *trapped = false;
    m.ctx.call_depth = 0; m.ctx.has_checkpoint=true;
    if (__builtin_setjmp(m.ctx.checkpoint)) { *trapped=true; m.ctx.has_checkpoint=false;
        return int64_t(m.ctx.last_trap); }
    using F0=int64_t(*)();
    int64_t r = reinterpret_cast<F0>(m.main_entry)();
    m.ctx.has_checkpoint=false;
    return r;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    std::printf("=== v1.0 Tier 2 first-class function references ===\n");

    // ---- A. handle creation: &fn bakes the slot ----
    {
        FRModule m;
        // main is slot 0, id is slot 1 (declared after main? no — funcs are
        // slotted in declaration order; main first -> slot 0, id -> slot 1).
        // `return &id;` returns id's slot index as an i64.
        bool ok = compile_fr(
            "fn id(x: i64) -> i64 { return x; }\n"
            "fn main() -> i64 { let h = &id; return h; }\n", m);
        check(ok, "A1: compile `let h = &id; return h;`");
        if (ok) { bool t=false; int64_t r=run_main(m,&t);
            // id is slot 1 (declared first -> slot 0; wait: id declared first -> slot 0)
            check(!t && r == m.slots["id"], "A1: &id bakes id's slot index as the i64 handle");
        }
        for(auto&fn:m.fns)if(fn.exec)free_executable(fn.exec);
    }

    // ---- A2/A3/A4/A5/A6/A7: sema rejections (the type-level forging guard) ----
    auto sema_bad = [](const char* desc, const std::string& src)->bool{
        FRModule m; bool ok = compile_fr(src, m);
        for(auto&fn:m.fns)if(fn.exec)free_executable(fn.exec);
        bool r = !ok;  // compile_fr prints the sema errors + returns false
        check(r, desc); return r;
    };
    // A2: let h: i64 = &id  (fn -> plain i64) rejected
    sema_bad("A2: `let h: i64 = &id;` rejected (fn not interchangeable with i64)",
        "fn id(x: i64) -> i64 { return x; }\n fn main() -> i64 { let h: i64 = &id; return h; }\n");
    // A3: let h = 5; let f = &id; h = f;  (assign fn to i64 var) rejected
    sema_bad("A3: assigning a fn handle to a plain i64 var rejected",
        "fn id(x: i64) -> i64 { return x; }\n fn main() -> i64 { let h = 5; let f = &id; h = f; return h; }\n");
    // A4: let forged: i64 = 5; let f: fn = forged;  (i64 -> fn) rejected
    sema_bad("A4: `let f: fn = forged_i64;` rejected (no i64-to-fn assignment)",
        "fn id(x: i64) -> i64 { return x; }\n fn main() -> i64 { let forged: i64 = 5; let f: fn = forged; return f(1); }\n");
    // A5: &&id  (& of a non-name) rejected
    sema_bad("A5: `&&id` rejected (& may only be applied to a function name)",
        "fn id(x: i64) -> i64 { return x; }\n fn main() -> i64 { let h = &&id; return h; }\n");
    // A6: &undefined_name  rejected
    sema_bad("A6: `&undefined_name` rejected (not a script function)",
        "fn main() -> i64 { let h = &nope; return h; }\n");

    // ---- B. happy-path first-class calls ----
    {
        FRModule m;
        bool ok = compile_fr(
            "fn id(x: i64) -> i64 { return x; }\n"
            "fn main() -> i64 { let h = &id; return h(42); }\n", m);
        check(ok, "B1: compile `let h = &id; return h(42);`");
        if (ok) { bool t=false; int64_t r=run_main(m,&t); check(!t && r==42, "B1: handle(42) dispatches to id, returns 42"); }
        for(auto&fn:m.fns)if(fn.exec)free_executable(fn.exec);
    }
    {
        FRModule m;
        bool ok = compile_fr(
            "fn add(a: i64, b: i64) -> i64 { return a + b; }\n"
            "fn main() -> i64 { let h = &add; return h(3, 4); }\n", m);
        check(ok, "B2: compile two-arg handle call");
        if (ok) { bool t=false; int64_t r=run_main(m,&t); check(!t && r==7, "B2: h(3,4) dispatches to add, returns 7 (Win64 rcx/rdx)"); }
        for(auto&fn:m.fns)if(fn.exec)free_executable(fn.exec);
    }
    {
        FRModule m;
        bool ok = compile_fr(
            "fn id(x: i64) -> i64 { return x; }\n"
            "fn main() -> i64 { let h: fn = &id; return h(42); }\n", m);
        check(ok, "B3: compile `let h: fn = &id;` (explicit fn type)");
        if (ok) { bool t=false; int64_t r=run_main(m,&t); check(!t && r==42, "B3: explicit-`fn`-typed handle call returns 42"); }
        for(auto&fn:m.fns)if(fn.exec)free_executable(fn.exec);
    }
    {
        // B5: recursion via handle. fib calls itself through a handle.
        FRModule m;
        bool ok = compile_fr(
            "fn fib(n: i64) -> i64 { if (n <= 1) { return n; } let f = &fib; return f(n - 1) + f(n - 2); }\n"
            "fn main() -> i64 { return fib(10); }\n", m);
        check(ok, "B5: compile recursion via handle (fib via &fib)");
        if (ok) { bool t=false; int64_t r=run_main(m,&t); check(!t && r==55, "B5: fib(10) via handle recursion = 55"); }
        for(auto&fn:m.fns)if(fn.exec)free_executable(fn.exec);
    }

    // ---- C. the guard: invalid handles trap (REDSHELL #6) ----
    // The guard validates the runtime handle against the allowlist bitset
    // before indexing the dispatch table. We forge a bad handle via a
    // test-only approach: a fn-typed var assigned a literal slot value that
    // is out-of-range or an unregistered slot. Since sema forbids i64->fn
    // assignment (A4), we use a host native that RETURNS a fn-typed value —
    // the one trusted runtime source (plan §4.5). We register `forge_fn` as
    // returning `fn` so a script can do `let h: fn = forge_fn(99999); h(1);`.
    {
        FRModule m;
        // Register the forge native with a `fn` return type so sema accepts the
        // assignment to a fn-typed var (the trusted-runtime-source path, §4.5).
        // Its C impl returns the raw slot arg (a forged i64 the guard must catch).
        // We register it directly into m.natives before sema (compile_fr re-registers
        // the six extensions; we add this one on top).
        // -- But compile_fr builds its own natives map; we need to inject ours.
        // Simpler: use a SEPARATE compile path for the guard tests. Inline it here.
        std::string src =
            "fn main() -> i64 { let h: fn = forge_fn(99999); return h(1); }\n";
        // Build a custom harness for this one (needs the forge native registered).
        std::unordered_set<std::string> seen; std::string resolved;
        try { resolved = resolve_imports(src, "./", seen); } catch (...) { resolved = src; }
        auto lr = tokenize(resolved, "<g>"); auto pr = parse(std::move(lr.toks));
        int si=0; for(auto&fn:pr.program.funcs){m.slots[fn.name]=si++;fn.slot=m.slots[fn.name];}
        m.slot_count = si;
        ember::OpOverloadTable ov;
        ext_vec::register_natives(m.natives);ext_quat::register_natives(m.natives);
        ext_mat::register_natives(m.natives);ext_string::register_natives(m.natives);
        ext_array::register_natives(m.natives);ext_math::register_natives(m.natives);
        ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);
        ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
        // the test-only forge native: declared return type `fn` (is_fn_handle=true)
        {
            NativeSig ns; ns.fn_ptr = (void*)&n_forge_fn_handle;
            ns.ret = type_i64(); ns.ret.is_fn_handle = true;
            ns.params.push_back(type_i64());
            m.natives["forge_fn"] = std::move(ns);
        }
        auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
        auto sr=sema(pr.program,m.natives,m.slots,0,&ov,&layouts);
        bool sema_ok = sr.ok;
        if (!sema_ok) { for(auto&e:sr.errors) std::printf("  sema line %u: %s\n",e.line,e.msg.c_str()); }
        check(sema_ok, "C1: sema accepts `let h: fn = forge_fn(99999)` (trusted native returns fn)");
        if (sema_ok) {
            ember::GlobalsBlock gb; std::vector<uint8_t> gbs(0); gb.base=int64_t(gbs.data());
            ember::g_globals_for_codegen=&gb;
            m.table = DispatchTable(pr.program.funcs.size());
            ember::CodeGenCtx ctx; ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(m.table.base());
            ctx.natives=&m.natives; ctx.script_slots=&m.slots; ctx.structs=&layouts;
            m.allowlist = build_fn_allowlist(m.slots, m.slot_count);
            ctx.fn_allowlist_base = int64_t(m.allowlist.data());
            ctx.fn_slot_count = int64_t(m.slot_count);
            ember::context_t ectx; ectx.budget_remaining=2'000'000'000LL; ectx.max_call_depth=64;
            ctx.trap_stub=(void*)&fr_trap; ctx.trap_ctx=&ectx;
            ctx.emit_budget_checks=true; ctx.depth_ptr=&ectx.call_depth; ctx.max_call_depth=64; ctx.emit_depth_checks=true;
            for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); m.table.set(fn.slot,cf.entry); m.fns.push_back(std::move(cf));}
            auto sit=m.slots.find("main"); void* entry = m.table.get(sit->second);
            // Run under a fresh context. The guard should fire: 99999 >= slot_count (1) -> trap.
            ember::context_t run_ctx; run_ctx.max_call_depth=64; run_ctx.has_checkpoint=true;
            bool trapped=false;
            // NOTE: the trap_ctx baked at compile was &ectx; the run must use the
            // SAME context address or the longjmp lands wrong. Reuse ectx.
            ectx.has_checkpoint=true;
            if (__builtin_setjmp(ectx.checkpoint)) { trapped=true; ectx.has_checkpoint=false; }
            else { using F0=int64_t(*)(); reinterpret_cast<F0>(entry)(); ectx.has_checkpoint=false; }
            check(trapped, "C1: out-of-range forged handle (99999) traps via the guard, not a raw call");
            check(ectx.last_trap == TrapReason::BadCallTarget,
                  "C1: trap reason is BadCallTarget (recoverable longjmp, not a crash)");
        }
        for(auto&fn:m.fns)if(fn.exec)free_executable(fn.exec);
    }

    // C2: in-range UNREGISTERED slot. main is slot 0 (registered). Forge handle
    // = a slot in [0, slot_count) whose bit is CLEAR — but with only main (slot
    // 0, registered), every slot in range is registered. So use a 2-fn module
    // and forge a slot BETWEEN them that's unregistered... slots are dense
    // (0,1 for 2 fns), so no in-range-unregistered slot exists. The in-range
    // unregistered case needs a slot_count > registered count. We simulate by
    // setting ctx.fn_slot_count HIGHER than the actual registered count (a
    // "reserved but uncompiled" slot). Forge handle = slot_count-1 (in range,
    // bit clear). This is exactly plan §7 case 2.
    {
        FRModule m;
        std::string src =
            "fn id(x: i64) -> i64 { return x; }\n"
            "fn main() -> i64 { let h: fn = forge_fn(2); return h(1); }\n";  // 2 = in-range-but-unregistered (we inflate slot_count to 4)
        std::unordered_set<std::string> seen; std::string resolved;
        try { resolved = resolve_imports(src, "./", seen); } catch (...) { resolved = src; }
        auto lr = tokenize(resolved, "<g>"); auto pr = parse(std::move(lr.toks));
        int si=0; for(auto&fn:pr.program.funcs){m.slots[fn.name]=si++;fn.slot=m.slots[fn.name];}
        m.slot_count = si;
        ember::OpOverloadTable ov;
        ext_vec::register_natives(m.natives);ext_quat::register_natives(m.natives);
        ext_mat::register_natives(m.natives);ext_string::register_natives(m.natives);
        ext_array::register_natives(m.natives);ext_math::register_natives(m.natives);
        ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);
        ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
        { NativeSig ns; ns.fn_ptr=(void*)&n_forge_fn_handle;
          ns.ret=type_i64(); ns.ret.is_fn_handle=true; ns.params.push_back(type_i64());
          m.natives["forge_fn"]=std::move(ns); }
        auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
        auto sr=sema(pr.program,m.natives,m.slots,0,&ov,&layouts);
        check(sr.ok, "C2: sema accepts the in-range-unregistered test setup");
        if (sr.ok) {
            ember::GlobalsBlock gb; std::vector<uint8_t> gbs(0); gb.base=int64_t(gbs.data());
            ember::g_globals_for_codegen=&gb;
            m.table = DispatchTable(pr.program.funcs.size());
            ember::CodeGenCtx ctx; ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(m.table.base());
            ctx.natives=&m.natives; ctx.script_slots=&m.slots; ctx.structs=&layouts;
            // build the allowlist with the REAL registered slots, but LIE about
            // slot_count (inflate to 4) so slot 2 is in-range-but-unregistered.
            m.allowlist = build_fn_allowlist(m.slots, 4);  // 4 slots, bits 0,1 set; 2,3 clear
            ctx.fn_allowlist_base = int64_t(m.allowlist.data());
            ctx.fn_slot_count = 4;   // inflated: slot 2 is in range, bit clear
            ember::context_t ectx; ectx.max_call_depth=64;
            ctx.trap_stub=(void*)&fr_trap; ctx.trap_ctx=&ectx;
            ctx.emit_budget_checks=true; ctx.depth_ptr=&ectx.call_depth; ctx.max_call_depth=64; ctx.emit_depth_checks=true;
            for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); m.table.set(fn.slot,cf.entry); m.fns.push_back(std::move(cf));}
            auto sit=m.slots.find("main"); void* entry = m.table.get(sit->second);
            ectx.has_checkpoint=true; bool trapped=false;
            if (__builtin_setjmp(ectx.checkpoint)) { trapped=true; ectx.has_checkpoint=false; }
            else { using F0=int64_t(*)(); reinterpret_cast<F0>(entry)(); ectx.has_checkpoint=false; }
            check(trapped, "C2: in-range-but-unregistered handle (slot 2, bit clear) traps (bt fires)");
            check(ectx.last_trap == TrapReason::BadCallTarget, "C2: trap reason BadCallTarget");
        }
        for(auto&fn:m.fns)if(fn.exec)free_executable(fn.exec);
    }

    std::printf("\nv1.0 Tier 2 function refs test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
