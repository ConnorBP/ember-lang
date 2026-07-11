// ext_lifecycle_test - runtime coverage for the dynamic-registration extension
// (docs/planning/plan_FUNCTION_REFS.md §6). Proves the full round-trip: a script passes
// `&fn` to register_routine(fn, data) -> id; the host's host_routines()
// accessor returns the (slot, data) pairs; the host calls a routine via the
// dispatch table (table.get(slot) with arg=data); unregister_routine drops it.
//
// Mirrors ext_sync_test's harness (lex->parse->sema->codegen->JIT->call) +
// the function_refs_test allowlist wiring (register_routine takes a `fn`
// handle, so the fn allowlist must be built so &fn works in the script).
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/context.hpp"
#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/jit_memory.hpp"
#include "../src/binding_builder.hpp"
#include "../extensions/lifecycle/ext_lifecycle.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace ember;

static int g_fail = 0;
static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

extern "C" void lc_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1);
    }
    std::abort();
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    std::printf("=== ember lifecycle extension test (plan_FUNCTION_REFS.md §6) ===\n");

    // Reset the routine table to a clean state.
    ember::ext_lifecycle::reset();

    // A script that registers a routine in @entry. tick is the routine fn.
    // main registers &tick with data=42, returns the routine id.
    std::string src =
        "fn tick(data: i64) -> i64 { return data + 1; }\n"
        "fn main() -> i64 { let h = &tick; return register_routine(h, 42); }\n";

    auto lr = tokenize(src, "<lc>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return 1; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return 1; }

    std::unordered_map<std::string,int> slots; int si=0;
    for(auto&fn:pr.program.funcs){slots[fn.name]=si++;fn.slot=slots[fn.name];}
    std::unordered_map<std::string,ember::NativeSig> natives;
    ember::ext_lifecycle::register_natives(natives);
    auto layouts = build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    auto sr = sema(pr.program, natives, slots, 0, nullptr, &layouts);
    if(!sr.ok){ std::printf("FAIL: sema (%zu):\n",sr.errors.size());
        for(auto&e:sr.errors) std::printf("  line %u: %s\n",e.line,e.msg.c_str());
        return 1;
    }

    ember::GlobalsBlock gb; std::vector<uint8_t> gbs(0); gb.base=int64_t(gbs.data());
    ember::g_globals_for_codegen = &gb;
    ember::DispatchTable table(pr.program.funcs.size()); ember::CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.globals_index=&gb.index; ctx.globals_types=&gb.types;
    ctx.dispatch_base=int64_t(table.base());
    ctx.natives=&natives; ctx.script_slots=&slots; ctx.structs=&layouts;
    // fn allowlist so &tick works (guard validates the handle)
    std::vector<uint8_t> allowlist = build_fn_allowlist(slots, int(slots.size()));
    ctx.fn_allowlist_base = int64_t(allowlist.data());
    ctx.fn_slot_count = int64_t(slots.size());
    ember::context_t ectx; ectx.budget_remaining=2'000'000'000LL; ectx.max_call_depth=64;
    ctx.trap_stub=(void*)&lc_trap; ctx.trap_ctx=&ectx;
    ctx.budget_ptr=&ectx.budget_remaining; ctx.emit_budget_checks=true;
    ctx.depth_ptr=&ectx.call_depth; ctx.max_call_depth=64; ctx.emit_depth_checks=true;
    std::vector<ember::CompiledFn> fns;
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); table.set(fn.slot,cf.entry); fns.push_back(std::move(cf));}

    auto sit=slots.find("main"); void* entry=table.get(sit->second);
    ectx.has_checkpoint=true;
    if (__builtin_setjmp(ectx.checkpoint)) {
        std::printf("FAIL: main trapped: %s\n", ectx.last_error.c_str()); return 1;
    }
    using F0=int64_t(*)(); int64_t routine_id = reinterpret_cast<F0>(entry)();
    ectx.has_checkpoint=false;
    for(auto&fn:fns)if(fn.exec)free_executable(fn.exec);

    // ---- T1: main returned a routine id (>= 1); the host table has 1 routine ----
    check(routine_id >= 1, "T1: register_routine(&tick, 42) returned a routine id (>= 1)");
    int64_t host_count = ember::ext_lifecycle::host_count();
    check(host_count == 1, "T1: host_routines() has exactly 1 routine after registration");

    // ---- T2: the routine's (slot, data) match what the script registered ----
    auto routines = ember::ext_lifecycle::host_routines();
    check(routines.size() == 1 && routines[0].data == 42,
          "T2: the routine's data is 42 (what the script passed)");
    check(routines.size() == 1 && routines[0].slot == slots["tick"],
          "T2: the routine's slot is tick's slot (&tick baked it, the host got it)");

    // ---- T3: the HOST calls the routine via the dispatch table (the §6.2 path) ----
    // The host does table.get(slot) with arg=data — the same call mechanism as
    // the static @on_tick path, just discovered by the script. tick(data) = data+1 = 43.
    {
        // Recompile tick standalone to call it (the table was freed above). In a real
        // host the table lives for the module's lifetime; here we just re-run the compile
        // to get a live entry for tick. (Simpler than keeping fns alive across the block.)
        std::vector<ember::CompiledFn> fns2;
        for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); table.set(fn.slot,cf.entry); fns2.push_back(std::move(cf));}
        void* tick_entry = table.get(slots["tick"]);
        // Call tick(42) via ember_call_i64 (rcx=42). data+1 = 43.
        ectx.call_depth=0;
        int64_t r = ember::ember_call_i64(tick_entry, &ectx, 42);
        check(r == 43, "T3: host calls the routine via dispatch table -> tick(42) = 43");
        for(auto&fn:fns2)if(fn.exec)free_executable(fn.exec);
    }

    // ---- T4: unregister_routine(id) drops it; host_routines() is now empty ----
    {
        // Recompile to call unregister_routine(routine_id) via main? Simpler: call the
        // native directly (it's the same fn_ptr the JIT would call).
        auto it = natives.find("unregister_routine");
        auto unregister = (int64_t(*)(int64_t))it->second.fn_ptr;
        int64_t removed = unregister(routine_id);
        check(removed == 1, "T4: unregister_routine(id) -> 1 (removed)");
        check(ember::ext_lifecycle::host_count() == 0, "T4: host_routines() empty after unregister");
        // unregistering again -> 0 (no such routine / already removed)
        check(unregister(routine_id) == 0, "T4: unregister_routine(id) again -> 0 (already removed)");
    }

    // ---- T5: free-list reuse (register after unregister reuses the id) ----
    {
        auto it = natives.find("register_routine");
        auto reg = (int64_t(*)(int64_t,int64_t))it->second.fn_ptr;
        // tick's slot still valid (we have it); register again
        int64_t id2 = reg(slots["tick"], 99);
        check(id2 == routine_id, "T5: re-register reuses the freed id (free-list)");
        auto rs = ember::ext_lifecycle::host_routines();
        check(rs.size() == 1 && rs[0].data == 99, "T5: the reused routine has the new data (99)");
        ember::ext_lifecycle::reset();
        check(ember::ext_lifecycle::host_count() == 0, "T5: reset() clears all routines");
    }

    std::printf("\nember lifecycle extension test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
