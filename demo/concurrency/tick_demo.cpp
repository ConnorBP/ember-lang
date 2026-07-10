// demo/concurrency/tick_demo.cpp — shape (B) single-thread tick-sim pipeline harness.
//
// Drives the ember-only tick_sim.ember pipeline: @entry creates the sync
// primitives (spsc, atomics, swapbuf); @on_tick producer/consumer stages run
// each tick. Bounded, deterministic, NO real threads (the single-context
// contract: one context, ticked by the host). Exercises the sync primitives'
// API ergonomics + tick integration (swapbuf double-buffered frame publish,
// atomic counters, spsc producer/consumer with retry-on-full/empty) without
// real concurrency.
//
// Determinism contract: after TICKS ticks, the produced atomic == TICKS*3
// (PER_TICK=3 per tick), consumed atomic <= produced (drain may lag by the
// queue depth), and the swapbuf back frame carries the last published
// generation number. The host reads these back via the _host accessors and
// asserts the invariants.
//
// Compile model (mirrors concurrency_demo / ember_cli's tick loop):
//   - resolve_imports -> tokenize -> parse -> slot-assign -> register sync
//     natives -> sema -> compile with use_context_reg=true (B1).
//   - Run @entry once (creates the primitives, stores handles in globals).
//   - Tick @on_tick fns a fixed TICKS times (producer then consumer each tick),
//     each under a fresh checkpoint. Single context.
//   - Read back atomics + swapbuf via _host accessors; assert determinism.
//
// Link: g++ -std=c++17 -O2 -pthread -Isrc -Iextensions/sync
//       demo/concurrency/tick_demo.cpp buildt/libember_frontend.a buildt/libember.a
//       buildt/libember_ext_sync.a buildt/libember_import.a
//       -o demo/concurrency/tick_demo.exe
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
#include "lifecycle.hpp"     // get_annotated_functions

#include "ext_sync.hpp"

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

extern "C" void tick_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1);
    }
    std::abort();
}

struct TickModule {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string,int> slots;
    std::unordered_map<std::string, NativeSig> natives;
    std::vector<uint8_t> globals_store;
    GlobalsBlock gb;
    Program program;
    // resolved annotated fns
    std::vector<AnnotatedFn> entry_fns;    // @entry
    std::vector<AnnotatedFn> tick_fns;     // @on_tick
    bool ok = false;
};

static bool compile_tick(const std::string& src, TickModule& m) {
    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./demo/concurrency/", seen); }
    catch (const std::exception& e) { std::printf("FAIL: resolve_imports: %s\n", e.what()); return false; }
    auto lr = tokenize(resolved, "<tick>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return false; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return false; }
    int si=0; for(auto&fn:pr.program.funcs){m.slots[fn.name]=si++;fn.slot=m.slots[fn.name];}
    ember::ext_sync::register_natives(m.natives);
    ember::OpOverloadTable ov;
    auto layouts = build_struct_layouts(pr.program); pr.program.string_xor_key = 0;
    auto sr = sema(pr.program, m.natives, m.slots, 0, &ov, &layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return false;
    }
    m.globals_store.assign(pr.program.globals.size() * 8, 0);
    m.gb.base = int64_t(m.globals_store.data());
    { uint32_t gi=0; for (auto& g : pr.program.globals) { m.gb.index[g.name]=gi++; m.gb.types[g.name]=g.ty.get(); } }
    eval_global_initializers(pr.program, GlobalInitCtx{m.globals_store, m.gb.index, m.gb.types});
    ember::g_globals_for_codegen = nullptr;
    m.table = DispatchTable(pr.program.funcs.size());
    ember::CodeGenCtx ctx;
    ctx.globals_base = m.gb.base; ctx.dispatch_base = int64_t(m.table.base());
    ctx.natives = &m.natives; ctx.script_slots = &m.slots; ctx.structs = &layouts;
    ctx.globals_index = &m.gb.index; ctx.globals_types = &m.gb.types;
    ctx.use_context_reg = true;
    ctx.trap_stub = (void*)&tick_trap;
    ctx.emit_budget_checks = true; ctx.emit_depth_checks = true; ctx.max_call_depth = 64;
    for(auto&fn:pr.program.funcs){
        auto cf = compile_func(fn, ctx); finalize(cf); m.table.set(fn.slot, cf.entry);
        m.fns.push_back(std::move(cf));
    }
    m.entry_fns = get_annotated_functions(pr.program, "@entry");
    m.tick_fns  = get_annotated_functions(pr.program, "@on_tick");
    m.program = std::move(pr.program);
    m.ok = true;
    return true;
}

template <typename Sig>
static Sig native_ptr(const std::unordered_map<std::string,NativeSig>& m, const char* name) {
    auto it = m.find(name);
    return it == m.end() ? nullptr : reinterpret_cast<Sig>(it->second.fn_ptr);
}

// Read an i64-typed global from the shared store by name.
static int64_t read_global_i64(TickModule& m, const char* name) {
    auto it = m.gb.index.find(name);
    if (it == m.gb.index.end()) return 0;
    return *(reinterpret_cast<int64_t*>(m.gb.base) + it->second);
}

int main() {
    std::printf("=== ember concurrency demo (shape B: single-thread tick sim) ===\n");
    ember::ext_sync::reset();

    TickModule m;
    {
        FILE* f = std::fopen("demo/concurrency/tick_sim.ember", "rb");
        if (!f) { std::printf("FAIL: cannot open demo/concurrency/tick_sim.ember\n"); return 1; }
        std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        std::string src(sz, '\0'); std::fread(&src[0], 1, sz, f); std::fclose(f);
        if (!compile_tick(src, m)) { check(false, "compile tick_sim.ember"); return g_fail; }
        check(true, "compile tick_sim.ember (B1, use_context_reg=true)");
    }
    check(!m.entry_fns.empty(), "@entry fn discovered");
    check(m.tick_fns.size() == 2, "exactly 2 @on_tick fns discovered (producer + consumer)");

    // ---- run @entry once (creates the primitives, stores handles in globals) ----
    context_t ectx; ectx.budget_remaining = 500'000'000; ectx.max_call_depth = 64;
    bool trapped = false;
    ectx.has_checkpoint = true;
    if (__builtin_setjmp(ectx.checkpoint)) { trapped = true; ectx.has_checkpoint = false; }
    if (!trapped) {
        for (auto& af : m.entry_fns) {
            void* entry = m.table.get(af.slot);
            if (entry) ember_call_void(entry, &ectx);
        }
        ectx.has_checkpoint = false;
    }
    check(!trapped, "@entry (setup) ran without trap");

    // Read back the handles the @entry stored in globals (via the host accessors
    // we can inspect the primitives directly).
    int64_t spsc_h      = read_global_i64(m, "spsc_h");
    int64_t counter_p   = read_global_i64(m, "counter_produced");
    int64_t counter_c   = read_global_i64(m, "counter_consumed");
    int64_t frame_h     = read_global_i64(m, "frame_h");
    int64_t tick_gen_h  = read_global_i64(m, "tick_gen");
    check(spsc_h != 0 && counter_p != 0 && counter_c != 0 && frame_h != 0 && tick_gen_h != 0,
          "@entry created spsc/atomics/swapbuf + stored handles in globals");

    // ---- tick @on_tick producer then consumer, TICKS times ----
    constexpr int64_t TICKS = 200;
    constexpr int64_t PER_TICK = 3;
    context_t tctx; tctx.budget_remaining = 500'000'000; tctx.max_call_depth = 64;
    bool tick_trapped = false;
    int64_t ticks_done = 0;
    for (int64_t t = 0; t < TICKS; ++t) {
        tctx.call_depth = 0;
        tctx.has_checkpoint = true;
        if (__builtin_setjmp(tctx.checkpoint)) { tick_trapped = true; break; }
        // run each @on_tick fn (producer, then consumer, in discovery order)
        for (auto& af : m.tick_fns) {
            void* entry = m.table.get(af.slot);
            if (entry) ember_call_void(entry, &tctx);
        }
        tctx.has_checkpoint = false;
        ++ticks_done;
    }
    check(!tick_trapped, "no tick trapped across 200 ticks");
    check(ticks_done == TICKS, "ran exactly 200 ticks");

    // ---- determinism assertions via the _host accessors ----
    auto atomic_load  = native_ptr<int64_t(*)(int64_t)>(m.natives, "atomic_load");
    auto swapbuf_back = ember::ext_sync::swapbuf_back_ptr;   // host accessor
    auto spsc_size    = native_ptr<int64_t(*)(int64_t)>(m.natives, "spsc_size");

    // produced == TICKS * PER_TICK (the producer pushes PER_TICK per tick, every
    // tick, cap 4 >= PER_TICK so it never blocks).
    int64_t produced = atomic_load(counter_p);
    int64_t consumed = atomic_load(counter_c);
    check(produced == TICKS * PER_TICK,
          "produced atomic == 600 (200 ticks x 3 per tick) -- deterministic frame production");
    // consumed <= produced (the consumer drains next tick; lag <= queue depth 4).
    // Over 200 ticks the consumer catches up: consumed == produced (the queue is
    // drained each tick since cap 4 and PER_TICK 3 and the consumer runs after).
    check(consumed <= produced, "consumed <= produced (consumer never over-drains)");
    check(consumed == produced,
          "consumed == produced (consumer drains the SPSC fully over 200 ticks, no loss)");

    // swapbuf: the back frame carries the last published generation + items.
    // The producer publishes a frame [tick_gen, items_this_tick] each tick and
    // swaps. The back side holds the LAST published frame. generation == TICKS-1
    // (0-indexed: tick 0 -> gen 0, ... tick 199 -> gen 199), items == 3.
    int64_t* back = nullptr; int64_t back_len = 0;
    check(swapbuf_back(frame_h, &back, &back_len), "swapbuf_back_ptr -> back frame readable");
    if (back && back_len >= 2) {
        check(back[0] == TICKS - 1, "swapbuf back frame generation == 199 (last published)");
        check(back[1] == PER_TICK, "swapbuf back frame items_this_tick == 3");
    } else {
        check(false, "swapbuf back frame has >= 2 cells");
    }

    // spsc_size: after 200 ticks with the consumer draining each tick, the
    // queue should be empty (size 0).
    check(spsc_size(spsc_h) == 0, "SPSC empty after 200 ticks (consumer fully drained)");

    std::printf("  produced=%lld consumed=%lld back_gen=%lld back_items=%lld spsc_size=%lld\n",
                (long long)produced, (long long)consumed,
                back && back_len >= 2 ? (long long)back[0] : -1,
                back && back_len >= 2 ? (long long)back[1] : -1,
                (long long)spsc_size(spsc_h));

    std::printf("\nember concurrency demo (shape B): %s\n", g_fail ? "FAIL" : "PASS");
    for(auto&fn:m.fns) if(fn.exec) free_executable(fn.exec);
    ember::ext_sync::reset();
    return g_fail;
}
