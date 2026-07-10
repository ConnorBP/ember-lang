// thread_safety_test - v1.0 context thread-safety (plan_CONTEXT_THREADSAFETY.md,
// Option D + B1). Proves the per-call context-register (r14) indirection lets
// ONE compiled body serve N per-thread context_t's, with each thread's trap
// longjmp'ing to ITS OWN checkpoint (not a sibling's).
//
// What this pins:
//  (1) B1 mode (use_context_reg=true) runs a correct script and returns its
//      value via ember_call_void (r14 = ctx at entry).
//  (2) A budget-exceeding script traps via the PER-THREAD context_t (the
//      trap reads ctx from r14, not a baked pointer) -> last_trap is set on
//      THAT thread's context, longjmp to THAT thread's checkpoint.
//  (3) Two host threads run the SAME compiled entry concurrently with two
//      separate context_t's (different budgets); each traps independently and
//      its own last_trap is set. This is the core thread-safety invariant:
//      no shared mutable checkpoint/budget/depth in the JIT'd path.
//  (4) The g_globals_for_codegen emit-time fix: compile with globals_index /
//      globals_types threaded through CodeGenCtx and g_globals_for_codegen
//      NULL (the process-wide pointer must NOT be consulted).
//
// Mirrors v0_4_hardening_test's harness (resolve_imports -> tokenize -> parse
// -> slot-assign -> register six extensions -> sema -> compile -> call) but
// with use_context_reg=true and ember_call_void (not a raw fn-ptr call).
#include "import.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "context.hpp"
#include "engine.hpp"        // ember_call_void, ember_call_i64, finalize
#include "globals.hpp"        // eval_global_initializers (v1.0 global seeding)
#include "dispatch_table.hpp"
#include "binding_builder.hpp"
#include "jit_memory.hpp"

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
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

// Per-thread trap stub. Same shape as v0_4's test_trap: record reason on the
// ctx (which arrived in r14) + longjmp to that ctx's checkpoint. The ctx is
// whichever thread's context was passed in r14 -- the whole point of B1.
extern "C" void ts_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1);
    }
    std::abort();
}

// Compile `main()->i64` with use_context_reg=true (B1) + the g_globals fix
// (globals_index threaded, process-wide pointer nulled). Returns the entry
// pointer + keeps the CompiledFns alive in `out_fns` (caller frees). The
// dispatch table + slots are also returned for the caller to call main.
struct B1Module {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string,int> slots;
    void* main_entry = nullptr;
    std::unordered_map<std::string, NativeSig> natives;
};

// Compile once. The SAME entry is called from N threads with N contexts.
static bool compile_b1(const std::string& src, B1Module& m) {
    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); } catch (...) { return false; }
    auto lr = tokenize(resolved, "<t>"); if (!lr.ok) return false;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return false;
    int si=0; for(auto&fn:pr.program.funcs){m.slots[fn.name]=si++;fn.slot=m.slots[fn.name];}
    ember::OpOverloadTable ov;
    ext_vec::register_natives(m.natives);ext_quat::register_natives(m.natives);
    ext_mat::register_natives(m.natives);ext_string::register_natives(m.natives);
    ext_array::register_natives(m.natives);ext_math::register_natives(m.natives);
    ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);
    ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
    auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    if(!sema(pr.program,m.natives,m.slots,0,&ov,&layouts).ok) return false;

    // globals block + the emit-time fix: thread the index/types through ctx and
    // NULL the process-wide pointer (proves codegen no longer reads it).
    ember::GlobalsBlock gb; std::vector<uint8_t> gbs(pr.program.globals.size()*8, 0);
    gb.base=int64_t(gbs.data());
    { uint32_t gi=0; for (auto& g : pr.program.globals) { gb.index[g.name]=gi++; gb.types[g.name]=g.ty.get(); } }
    // v1.0: seed const global initializers into the backing store.
    eval_global_initializers(pr.program, GlobalInitCtx{gbs, gb.index, gb.types});
    ember::g_globals_for_codegen = nullptr;   // MUST stay null through compile
    m.table = DispatchTable(pr.program.funcs.size());
    ember::CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(m.table.base());
    ctx.natives=&m.natives; ctx.script_slots=&m.slots; ctx.structs=&layouts;
    // the emit-time globals fix:
    ctx.globals_index = &gb.index;
    ctx.globals_types = &gb.types;

    // B1: read context_t fields through r14 (the per-call context register).
    // budget_ptr/depth_ptr/trap_ctx are LEFT NULL in B1 mode -- the JIT reads
    // them through r14, not baked pointers. This is the load-bearing setting.
    ctx.use_context_reg = true;
    ctx.trap_stub = (void*)&ts_trap;   // the host fn (constant) -- still baked
    ctx.emit_budget_checks = true;
    ctx.emit_depth_checks = true;
    ctx.max_call_depth = 8;

    for(auto&fn:pr.program.funcs){
        auto cf=compile_func(fn,ctx); finalize(cf); m.table.set(fn.slot,cf.entry);
        m.fns.push_back(std::move(cf));
    }
    auto sit=m.slots.find("main");
    if(sit==m.slots.end()) return false;
    m.main_entry = m.table.get(sit->second);
    return m.main_entry != nullptr;
}

// Run main under a GIVEN context_t (per-thread) via ember_call_void (sets r14).
// Returns the i64 result, or sets *trapped=true if the trap fired (longjmp).
static int64_t run_with_ctx(void* entry, context_t* ectx, bool* trapped) {
    *trapped = false;
    ectx->has_checkpoint = true;
    if (__builtin_setjmp(ectx->checkpoint)) { *trapped = true; ectx->has_checkpoint=false; return 0; }
    int64_t r = ember_call_void(entry, ectx);
    ectx->has_checkpoint = false;
    return r;
}

int main() {
    std::printf("=== v1.0 context thread-safety (Option D + B1) ===\n");

    // ---- (1) B1 mode runs a correct script + returns its value ----
    {
        B1Module m;
        bool ok = compile_b1(
            "fn main() -> i64 { let mut s = 0; let mut i = 0; while (i < 100) { s = s + i; i = i + 1; } return s; }\n", m);
        check(ok, "B1: compile main() with use_context_reg=true + g_globals fix");
        if (ok) {
            context_t ectx; ectx.budget_remaining = 100'000'000; ectx.max_call_depth=8;
            bool trapped=false;
            int64_t r = run_with_ctx(m.main_entry, &ectx, &trapped);
            // sum 0..99 = 4950
            check(!trapped && r == 4950, "B1: correct script returns 4950 via ember_call_void (r14=ctx)");
        }
        for(auto&fn:m.fns) if(fn.exec) free_executable(fn.exec);
    }

    // ---- (2) budget-exceeding script traps via the PER-THREAD context ----
    {
        B1Module m;
        bool ok = compile_b1(
            "fn main() -> i64 { while (true) { let mut x: i64 = 1+1+1+1+1; } return 0; }\n", m);
        if (!ok) { check(false, "B1: compile infinite-loop main"); }
        else {
            // tiny budget -> the loop back-edge sub [r14+off_budget] hits 0 -> trap.
            context_t ectx; ectx.budget_remaining = 1000; ectx.max_call_depth=8;
            bool trapped=false;
            run_with_ctx(m.main_entry, &ectx, &trapped);
            check(trapped, "B1: budget-exceeding script traps (sub via r14, not a baked ptr)");
            check(ectx.last_trap == TrapReason::BudgetExceeded,
                  "B1: trap recorded BudgetExceeded on the PER-THREAD context_t");
        }
        for(auto&fn:m.fns) if(fn.exec) free_executable(fn.exec);
    }

    // ---- (3) TWO threads, SAME entry, TWO contexts, different budgets ----
    // The keystone: thread A (big budget) finishes + returns 4950; thread B
    // (tiny budget) traps. A's trap (none) + B's trap (BudgetExceeded) land on
    // the CORRECT context -- no cross-thread checkpoint corruption. If B1
    // were broken (baked ptr), both threads would share one budget/trap and
    // B's longjmp could land on A's checkpoint (the --tick bug, generalized).
    {
        B1Module m;
        bool ok = compile_b1(
            "fn main() -> i64 { let mut s = 0; let mut i = 0; while (i < 100) { s = s + i; i = i + 1; } return s; }\n", m);
        check(ok, "B1: compile shared main for two-thread test");
        if (ok) {
            std::atomic<bool> a_done{false}, b_done{false};
            int64_t a_result = -1; bool a_trapped = true;
            bool b_trapped = false; TrapReason b_reason = TrapReason::None;

            // Thread A: generous budget, expects completion + 4950.
            std::thread ta([&]{
                context_t ea; ea.budget_remaining = 100'000'000; ea.max_call_depth=8;
                a_trapped = false;
                a_result = run_with_ctx(m.main_entry, &ea, &a_trapped);
                a_done.store(true, std::memory_order_release);
            });
            // Thread B invokes the exact same machine-code entry as A, but its
            // own small context budget traps before the finite loop completes.
            // This distinguishes shared-body/per-context isolation from merely
            // compiling independent modules on independent threads.
            std::thread tb([&]{
                context_t eb; eb.budget_remaining = 20; eb.max_call_depth=8;
                bool tp=false;
                run_with_ctx(m.main_entry, &eb, &tp);
                b_trapped = tp; b_reason = eb.last_trap;
                b_done.store(true, std::memory_order_release);
            });
            ta.join(); tb.join();
            check(!a_trapped && a_result == 4950,
                  "B1: thread A (big budget) completed + returned 4950, no trap");
            check(b_trapped && b_reason == TrapReason::BudgetExceeded,
                  "B1: same compiled body trapped thread B's tiny independent context");
            check(a_done.load() && b_done.load(),
                  "B1: both threads finished (no cross-thread longjmp corrupted either)");
        }
        for(auto&fn:m.fns) if(fn.exec) free_executable(fn.exec);
    }

    // ---- (4) g_globals fix: compile a script that READS a global, with the
    //      process-wide g_globals_for_codegen NULL + globals_index threaded ----
    {
        B1Module m;
        // a global + a main that reads it. sema + globals.hpp register it into
        // gb.index; codegen must resolve via ctx.globals_index (the pointer is null).
        bool ok = compile_b1(
            "global g: i64 = 42;\n"
            "fn main() -> i64 { return g; }\n", m);
        check(ok, "B1: compile global-reading main with g_globals_for_codegen=null");
        if (ok) {
            context_t ectx; ectx.budget_remaining = 100'000'000; ectx.max_call_depth=8;
            bool trapped=false;
            int64_t r = run_with_ctx(m.main_entry, &ectx, &trapped);
            check(!trapped && r == 42,
                  "B1: global read via ctx.globals_index (process-wide ptr stays null)");
        }
        for(auto&fn:m.fns) if(fn.exec) free_executable(fn.exec);
    }

    std::printf("\nv1.0 context thread-safety test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
