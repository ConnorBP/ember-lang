// gc_integration_test.cpp - tracing GC wired into the ember engine.
//
// Two-part integration test for the GC wiring (ext_gc + codegen use_gc_env):
//
//   PART A (host API): exercises ext_gc directly -- alloc many lambda envs in
//     a loop, pin a subset, unroot the rest, collect, and verify:
//       - the heap stays BOUNDED (only pinned envs survive; the rest are
//         swept), so allocating N envs in a loop does NOT grow live_objects
//         past the pinned set,
//       - REACHABLE (pinned) envs survive every collect,
//       - UNREACHABLE (unrooted) envs are collected (freed count grows),
//       - the auto-collect threshold fires + reaps unpinned envs.
//     This is the "creates many lambdas in a loop, GC collects unreachable
//     ones, heap stays bounded, reachable survive" test from the task.
//
//   PART B (codegen integration): compile + run a lambda with
//     CodeGenCtx::use_gc_env=true through the FULL engine pipeline
//     (resolve_imports -> tokenize -> parse -> sema -> compile_func ->
//     finalize -> call), with the __ember_gc_* natives registered, and verify
//     the lambda returns the correct value. This proves the heap-allocated
//     closure env path works end-to-end through the engine (the env is
//     allocated by __ember_gc_alloc_env, captures copied into it, the lambda
//     called, the env_ptr dereferenced at the call site). Covers:
//       - a capturing lambda (env_size > 0, the alloc + capture-copy path),
//       - a no-capture lambda (env_size == 0, the no-env fast path),
//       - a nested lambda (transitive capture through the enclosing heap env),
//       - many lambdas in a loop (the auto-collect threshold fires mid-run +
//         pinned envs survive, so the run still returns the right value).
//
// Mirrors fn_types_test's harness shape (the shared compile+run pipeline).
#include "../src/gc.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/import.hpp"      // resolve_imports
#include "../src/codegen.hpp"
#include "../src/context.hpp"
#include "../src/engine.hpp"
#include "../src/globals.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/binding_builder.hpp"
#include "../src/jit_memory.hpp"

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_map.hpp"
#include "ext_math.hpp"
#include "ext_gc.hpp"

#include "../src/ast.hpp"
#include "../src/gc_roots.hpp"    // ember::gc::GcGlobalRoots (precise global root descriptor)

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
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

extern "C" void gcit_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) EMBER_LONGJMP(ctx->checkpoint, 1);
    }
    std::abort();
}

// ---- PART A: host API (ext_gc direct) ----
// NOTE: gc_alloc_env no longer auto-pins (the precise root-scanning model
// replaced the permanent-pin workaround for lambda envs). The pin layer
// (gc_root_env / gc_unroot_env) is the EXPLICIT root API, still used by the
// script-visible gc_new/gc_delete and available to hosts that root heap
// objects out-of-band. Part A exercises THAT explicit pin layer: alloc then
// gc_root_env to pin, gc_unroot_env to release, gc_collect to reap.
static int run_part_a() {
    std::printf("=== PART A: ext_gc host API (alloc/pin/unroot/collect) ===\n");
    ext_gc::gc_init();
    ext_gc::gc_reset();  // start clean

    // [A1] alloc + root: live count tracks allocs; rooting is explicit.
    {
        ext_gc::gc_reset();
        int64_t a = ext_gc::gc_alloc_env(16);
        int64_t b = ext_gc::gc_alloc_env(24);
        ext_gc::gc_root_env(a);
        ext_gc::gc_root_env(b);
        check(a != 0, "A1a: gc_alloc_env returns non-zero ptr (a)");
        check(b != 0, "A1b: gc_alloc_env returns non-zero ptr (b)");
        check(a != b, "A1c: two allocs return distinct ptrs");
        check(ext_gc::gc_live_count() == 2, "A1d: live_count==2 after 2 allocs");
        check(ext_gc::gc_is_live(a) && ext_gc::gc_is_live(b),
              "A1e: both envs are live on the heap");
    }

    // [A2] collect with everything rooted: nothing freed.
    {
        ext_gc::gc_reset();
        int64_t a = ext_gc::gc_alloc_env(16); ext_gc::gc_root_env(a);
        int64_t b = ext_gc::gc_alloc_env(16); ext_gc::gc_root_env(b);
        int64_t freed = ext_gc::gc_collect();
        check(freed == 0, "A2a: collect frees 0 when all envs rooted");
        check(ext_gc::gc_live_count() == 2, "A2b: live_count==2 (rooted survive)");
    }

    // [A3] unroot -> collect reaps the unrooted env; rooted survives.
    {
        ext_gc::gc_reset();
        int64_t a = ext_gc::gc_alloc_env(16); ext_gc::gc_root_env(a);  // stays rooted
        int64_t b = ext_gc::gc_alloc_env(16); ext_gc::gc_root_env(b);  // will be unrooted
        check(ext_gc::gc_unroot_env(b), "A3a: unroot_env(b) returns true");
        int64_t freed = ext_gc::gc_collect();
        check(freed == 1, "A3b: collect frees 1 (the unrooted env b)");
        check(ext_gc::gc_is_live(a), "A3c: rooted env a survives");
        check(!ext_gc::gc_is_live(b), "A3d: unrooted env b is collected");
        check(ext_gc::gc_live_count() == 1, "A3e: live_count==1 after collect");
    }

    // [A4] MANY envs in a loop, heap stays BOUNDED:
    //     allocate 5000 envs, root a small reachable set, leave the rest
    //     unrooted (collectable). With the auto-collect threshold (default
    //     1024), the heap is collected periodically; live_count must NOT grow
    //     to 5000 -- it stays bounded near the rooted set. The rooted set
    //     survives every collect.
    {
        ext_gc::gc_reset();
        const int N = 5000;
        const int PINNED_COUNT = 10;
        int64_t pinned[PINNED_COUNT];
        for (int i = 0; i < N; ++i) {
            int64_t e = ext_gc::gc_alloc_env(32);
            if (i < PINNED_COUNT) {
                pinned[i] = e;
                ext_gc::gc_root_env(e);  // keep these reachable (rooted)
            }
            // else: unrooted -> collectable (no explicit unroot needed; never rooted)
        }
        int64_t live = ext_gc::gc_live_count();
        // Without collection the heap would hold 5000; with auto-collect it
        // stays bounded. Allow slack for envs not yet swept at the final
        // threshold check, but it MUST be well under N (the whole point).
        check(live < 1024 + PINNED_COUNT + 16,
              "A4a: heap bounded (< ~1030) after 5000 alloc+unroot loop");
        // Final explicit collect sweeps any remaining unrooted envs.
        ext_gc::gc_collect();
        check(ext_gc::gc_live_count() == PINNED_COUNT,
              "A4b: after final collect, live_count==10 (only rooted survive)");
        for (int i = 0; i < PINNED_COUNT; ++i) {
            check(ext_gc::gc_is_live(pinned[i]), "A4c: rooted env survives (reachable)");
        }
        check(ext_gc::gc_freed_count() >= N - PINNED_COUNT,
              "A4d: freed_count >= 4990 (unreachable envs collected)");
    }

    // [A5] threshold 0 disables auto-collect (heap grows unbounded until an
    //     explicit collect). Confirms the trigger is the threshold, not magic.
    {
        ext_gc::gc_reset();
        ext_gc::gc_set_threshold(0);
        for (int i = 0; i < 200; ++i) { int64_t e = ext_gc::gc_alloc_env(8); ext_gc::gc_root_env(e); }
        check(ext_gc::gc_live_count() == 200,
              "A5a: threshold 0 -> no auto-collect, live_count==200");
        ext_gc::gc_set_threshold(1024);  // restore default for subsequent tests
        // everything is rooted, so collect frees 0; verify the trigger reset
        int64_t freed = ext_gc::gc_collect();
        check(freed == 0, "A5b: all rooted -> collect frees 0");
        check(ext_gc::gc_live_count() == 200, "A5c: live_count still 200 (all rooted)");
    }

    // [A6] gc_reset clears everything (between-runs semantics).
    {
        ext_gc::gc_alloc_env(16);
        ext_gc::gc_alloc_env(16);
        ext_gc::gc_reset();
        check(ext_gc::gc_live_count() == 0, "A6a: gc_reset -> live_count==0");
        check(ext_gc::gc_freed_count() == 0, "A6b: gc_reset -> freed_count==0 (stats reset)");
    }

    return g_fail;
}

// ---- PART B: codegen integration (compile + run a lambda with use_gc_env) ----

// Precise GC global-root support: compute a typed globals layout (proper
// 8-aligned offsets + sizes, with lambda globals as 16 bytes) and a
// GcGlobalRoots descriptor listing the byte offsets of GC-pointer words
// (the env_ptr half of every lambda-typed global, at offset+8). Mirrors the
// CLI's compute_typed_globals_layout but local to this harness.
static uint32_t gcit_host_value_bytes(const ember::Type* t) {
    if (!t) return 8;
    if (t->is_slice) return 16;
    if (t->is_lambda) return 16;   // {fn_slot, env_ptr}
    if (t->array_len > 0)
        return uint32_t(t->array_len) * gcit_host_value_bytes(t->elem.get());
    if (!t->struct_name.empty()) return 8;  // harness: struct globals sized conservatively
    switch (t->prim) {
    case ember::Prim::Bool: case ember::Prim::I8: case ember::Prim::U8: return 1;
    case ember::Prim::I16: case ember::Prim::U16: return 2;
    case ember::Prim::I32: case ember::Prim::U32: case ember::Prim::F32: return 4;
    default: return 8;
    }
}

struct GcitModule {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string,int> slots;
    void* main_entry = nullptr;
    std::unordered_map<std::string, NativeSig> natives;
    std::vector<uint8_t> allowlist;
    int slot_count = 0;
    ember::context_t ctx{};
    // Precise GC: the typed globals block storage + descriptor must outlive
    // the run (the JIT'd code reads globals_base; the collector reads the
    // descriptor during collect). Owned here so they live for the module's
    // lifetime, not just compile_gcit's scope.
    std::vector<uint8_t> gb_store;
    ember::GlobalsBlock gb;
    ember::gc::GcGlobalRoots global_roots;
};

// Compile `main()->i64` with use_gc_env=true (lambda envs on the GC heap).
// Registers the standard natives INCLUDING ext_gc's __ember_gc_alloc_env so
// codegen can resolve it. Returns false on lex/parse/sema/compile error.
static bool compile_gcit(const std::string& src, GcitModule& m, bool ir = false, bool ctx_reg = false) {
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
    ext_gc::register_natives(m.natives);  // __ember_gc_alloc_env/collect/live
    ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);
    ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
    auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    auto sr=sema(pr.program,m.natives,m.slots,0,&ov,&layouts);
    if(!sr.ok){ std::printf("  sema (%zu errors):\n",sr.errors.size());
        for(auto&e:sr.errors) std::printf("  line %u: %s\n",e.line,e.msg.c_str());
        return false;
    }
    // Precise GC: build a TYPED globals block (proper 8-aligned offsets +
    // sizes; lambda globals are 16 bytes {slot, env_ptr}) + the GcGlobalRoots
    // descriptor listing the env_ptr half (offset+8) of every lambda-typed
    // global. The storage + descriptor live in the module (m.gb_store /
    // m.global_roots) so they outlive the run. Falls back to flat 8-byte slots
    // when there are no aggregate/lambda globals (the legacy path).
    uint32_t gcur = 0;
    auto align8 = [](uint32_t v) -> uint32_t { return (v + 7u) & ~7u; };
    for (auto& g : pr.program.globals) {
        uint32_t sz = gcit_host_value_bytes(g.ty.get());
        gcur = align8(gcur);
        m.gb.offsets[g.name] = gcur;
        m.gb.sizes[g.name] = sz;
        if (g.ty && g.ty->is_lambda) {
            // The env_ptr is the second word of the 16-byte lambda value.
            m.global_roots.offs.push_back(int32_t(gcur + 8));
        }
        gcur += sz;
    }
    m.gb_store.assign(size_t(gcur), 0);
    m.gb.base = int64_t(m.gb_store.data());
    { uint32_t gi=0; for (auto& g : pr.program.globals) { m.gb.index[g.name]=gi++; m.gb.types[g.name]=g.ty.get(); } }
    m.global_roots.base = uint64_t(m.gb.base);
    GlobalInitCtx gic{m.gb_store, m.gb.index, m.gb.types};
    gic.offsets = &m.gb.offsets;
    gic.sizes = &m.gb.sizes;
    eval_global_initializers(pr.program, gic);
    ember::g_globals_for_codegen = &m.gb;
    m.table = DispatchTable(pr.program.funcs.size());
    ember::CodeGenCtx ctx;
    ctx.globals_base=m.gb.base; ctx.dispatch_base=int64_t(m.table.base());
    ctx.globals_offsets = &m.gb.offsets; ctx.globals_types = &m.gb.types;
    ctx.natives=&m.natives; ctx.script_slots=&m.slots; ctx.structs=&layouts;
    m.allowlist = build_fn_allowlist(m.slots, m.slot_count);
    ctx.fn_allowlist_base = int64_t(m.allowlist.data());
    ctx.fn_slot_count = int64_t(m.slot_count);
    m.ctx.budget_remaining=2'000'000'000LL; m.ctx.max_call_depth=256; m.ctx.call_depth=0;
    ctx.trap_stub=(void*)&gcit_trap; ctx.trap_ctx=&m.ctx;
    ctx.budget_ptr=&m.ctx.budget_remaining; ctx.emit_budget_checks=true;
    ctx.depth_ptr=&m.ctx.call_depth; ctx.max_call_depth=256; ctx.emit_depth_checks=true;
    ctx.use_gc_env = true;  // <-- the GC heap env backend
    // Precise GC: address context_t::gc_frame_head via the baked pointer (the
    // harness calls main_entry directly without installing r14, so it uses
    // baked-ptr mode, not use_context_reg). The prologue/epilogue frame-record
    // maintenance reads/writes [*gc_frame_head_ptr].
    ctx.gc_frame_head_ptr = reinterpret_cast<void**>(&m.ctx.gc_frame_head);
    // Optimized IR backend path: enable_ir_backend + enable_regalloc so the
    // Thin IR lowering + emit (thin_lower + thin_emit) run, exercising the
    // precise-GC frame-record maintenance + frame-map emit in that path.
    if (ir) {
        ctx.enable_ir_backend = true;
        ctx.enable_regalloc = true;
    }
    // Context-register mode (for try/catch + GC): use_context_reg so the
    // try/catch emit + the frame-record maintenance read context_t via r14.
    // The caller MUST invoke via ember_call_void (which installs r14).
    if (ctx_reg) {
        ctx.use_context_reg = true;
        // context-offset budget/depth reads come from r14 too; clear the baked
        // ptrs so codegen uses r14 consistently.
        ctx.budget_ptr = nullptr;
        ctx.depth_ptr = nullptr;
    }
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); m.table.set(fn.slot,cf.entry); m.fns.push_back(std::move(cf));}
    auto sit=m.slots.find("main");
    if(sit==m.slots.end()) return false;
    m.main_entry = m.table.get(sit->second);
    return m.main_entry != nullptr;
}

static int64_t run_main(GcitModule& m, bool* trapped) {
    *trapped = false;
    m.ctx.call_depth = 0; m.ctx.gc_frame_head = nullptr; m.ctx.has_checkpoint=true;
    // Precise GC: attach the context (register the frame-chain + global-roots
    // trace callback) BEFORE the call so any collect (auto or explicit) sees
    // the active frame chain + typed global roots.
    ext_gc::gc_attach_context(&m.ctx, m.global_roots.empty() ? nullptr : &m.global_roots);
    if (EMBER_SETJMP(m.ctx.checkpoint)) { *trapped=true; m.ctx.has_checkpoint=false;
        ext_gc::gc_detach_context(&m.ctx); m.ctx.reset_for_call();
        return int64_t(m.ctx.last_trap); }
    using F0=int64_t(*)();
    int64_t r = reinterpret_cast<F0>(m.main_entry)();
    m.ctx.has_checkpoint=false;
    ext_gc::gc_detach_context(&m.ctx);
    return r;
}

static void cleanup(GcitModule& m) {
    for(auto&fn:m.fns)if(fn.exec)free_executable(fn.exec);
}

// Compile+run one source, returning the i64 main() result. Sets *ok=false on
// any pipeline failure (printed by compile_gcit) or a runtime trap.
static int64_t run_one(const std::string& src, bool* ok) {
    *ok = true;
    ext_gc::gc_init();
    ext_gc::gc_reset();
    GcitModule m;
    if (!compile_gcit(src, m)) { *ok = false; cleanup(m); ext_gc::gc_reset(); return 0; }
    bool trapped = false;
    int64_t r = run_main(m, &trapped);
    if (trapped) {
        std::printf("  FAIL: runtime trap: %s\n", m.ctx.last_error.c_str());
        *ok = false;
    }
    cleanup(m);
    ext_gc::gc_reset();
    return r;
}

// Compile+run one source with use_context_reg (r14) + ember_call_void, for
// scenarios that need the context register (try/catch + GC). The frame-record
// maintenance + try/catch emit read context_t via r14 (installed by
// ember_call_void). Returns the i64 main() result.
static int64_t run_one_ctx(const std::string& src, bool* ok) {
    *ok = true;
    ext_gc::gc_init();
    ext_gc::gc_reset();
    GcitModule m;
    if (!compile_gcit(src, m, /*ir=*/false, /*ctx_reg=*/true)) { *ok = false; cleanup(m); ext_gc::gc_reset(); return 0; }
    bool trapped = false;
    // call via ember_call_void (installs r14 = &ctx). main takes no args.
    m.ctx.call_depth = 0; m.ctx.gc_frame_head = nullptr; m.ctx.has_checkpoint = true;
    ext_gc::gc_attach_context(&m.ctx, m.global_roots.empty() ? nullptr : &m.global_roots);
    if (EMBER_SETJMP(m.ctx.checkpoint)) {
        trapped = true; m.ctx.has_checkpoint = false;
        ext_gc::gc_detach_context(&m.ctx); m.ctx.reset_for_call();
        std::printf("  FAIL: runtime trap (ctx): %s\n", m.ctx.last_error.c_str());
    } else {
        int64_t r = ember::ember_call_void(m.main_entry, &m.ctx);
        m.ctx.has_checkpoint = false;
        ext_gc::gc_detach_context(&m.ctx);
        if (trapped) { *ok = false; }
        else {
            cleanup(m); ext_gc::gc_reset();
            return r;
        }
    }
    *ok = !trapped;
    cleanup(m); ext_gc::gc_reset();
    return trapped ? int64_t(m.ctx.last_trap) : 0;
}

// Compile+run one source through the OPTIMIZED IR backend (enable_ir_backend +
// enable_regalloc) with use_gc_env, returning the i64 main() result. Exercises
// the precise-GC frame-record maintenance + frame-map emit in the Thin IR path
// (thin_lower + thin_emit), which the default tree-walker run_one does NOT
// reach. A lambda-creating function falls back to the tree-walker (the IR
// backend does not yet lower LambdaExpr), so this is used with GC scripts that
// do NOT create lambdas (gc_new/gc_delete loops) to cover the optimized path.
static int64_t run_one_ir(const std::string& src, bool* ok) {
    *ok = true;
    ext_gc::gc_init();
    ext_gc::gc_reset();
    GcitModule m;
    if (!compile_gcit(src, m, /*ir=*/true)) { *ok = false; cleanup(m); ext_gc::gc_reset(); return 0; }
    bool trapped = false;
    int64_t r = run_main(m, &trapped);
    if (trapped) {
        std::printf("  FAIL: runtime trap (IR): %s\n", m.ctx.last_error.c_str());
        *ok = false;
    }
    cleanup(m);
    ext_gc::gc_reset();
    return r;
}

static int run_part_b() {
    std::printf("=== PART B: codegen integration (use_gc_env, full pipeline) ===\n");

    // [B1] capturing lambda: env allocated on the GC heap, capture copied in,
    //      called, env_ptr dereferenced. Mirrors tests/lang/valid_lambda.ember.
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let captured_var: i64 = 40;\n"
            "    let f = fn(x: i64) -> i64 { return x + captured_var; };\n"
            "    return f(2);\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,   "B1a: capturing lambda compiled + ran (no trap)");
        check(r==42,"B1b: capturing lambda returns 42 (heap env capture visible)");
    }

    // [B2] no-capture lambda: env_size == 0, the no-env fast path (no alloc).
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let f = fn(x: i64) -> i64 { return x * 2; };\n"
            "    return f(42);\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,   "B2a: no-capture lambda compiled + ran");
        check(r==84,"B2b: no-capture lambda returns 84");
    }

    // [B3] nested lambda: transitive capture through the enclosing heap env
    //      (the inner lambda re-captures the outer's capture from the outer's
    //      heap env). Mirrors tests/lang/valid_lambda_nested.ember.
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let base: i64 = 10;\n"
            "    let outer = fn(a: i64) -> i64 {\n"
            "        let inner = fn(b: i64) -> i64 { return base + a + b; };\n"
            "        return inner(5);\n"
            "    };\n"
            "    return outer(15);\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,   "B3a: nested lambda compiled + ran");
        check(r==30,"B3b: nested lambda returns 30 (transitive heap capture)");
    }

    // [B4] lambda passed as arg: env travels with the lambda value through a
    //      call boundary. Mirrors tests/lang/valid_lambda_as_arg.ember.
    {
        bool ok;
        const char* src =
            "fn apply(g: fn(i64) -> i64, v: i64) -> i64 {\n"
            "    return g(v);\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let addend: i64 = 15;\n"
            "    let f = fn(x: i64) -> i64 { return x + addend; };\n"
            "    return apply(f, 42);\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,   "B4a: lambda-as-arg compiled + ran");
        check(r==57,"B4b: lambda-as-arg returns 57 (heap env crosses call boundary)");
    }

    // [B5] many lambdas in a loop: the auto-collect threshold fires mid-run;
    //      pinned envs survive every auto-collect, so the run still returns the
    //      correct value. This is the codegen-level "heap stays bounded +
    //      reachable survive" proof: a tight loop creating lambdas must not
    //      crash or corrupt because the GC reaped an in-use env.
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut acc: i64 = 0;\n"
            "    let mut i: i64 = 0;\n"
            "    while (i < 2000) {\n"
            "        let addend: i64 = i;\n"
            "        let f = fn(x: i64) -> i64 { return x + addend; };\n"
            "        acc = acc + f(1);\n"
            "        i = i + 1;\n"
            "    }\n"
            "    return acc;\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        // acc = sum over i=0..1999 of (1 + i) = 2000 + sum(0..1999)
        //     = 2000 + 1999*2000/2 = 2000 + 1999000 = 2001000
        check(ok,        "B5a: 2000-lambda loop compiled + ran (no trap mid-collect)");
        check(r==2001000,"B5b: 2000-lambda loop returns 2001000 (envs survive auto-collect)");
    }

    // [B6] by-reference capture (read): a lambda captures x by ref (`fn[&x]`),
    //      x is modified AFTER the lambda is created, the lambda sees the NEW
    //      value. The env slot holds a POINTER to x's storage (not a copy), so
    //      the post-capture mutation (x = 99) is visible inside the body. This
    //      is the core by-ref semantics + the GC env is on the heap (use_gc_env).
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut x: i64 = 10;\n"
            "    let f = fn[&x]() -> i64 { return x; };\n"
            "    x = 99;\n"
            "    return f();\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,   "B6a: by-ref capture (read) compiled + ran (GC env)");
        check(r==99,"B6b: by-ref capture sees post-capture mutation (returns 99)");
    }

    // [B7] by-reference capture (write-through) + mixed by-value: a lambda
    //      captures x by ref AND mutates it inside the body; the mutation is
    //      visible in the enclosing scope after the call. y is by-value (no &),
    //      an immutable copy. Mixed `[&x, y]` capture list.
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut x: i64 = 10;\n"
            "    let mut y: i64 = 20;\n"
            "    let f = fn[&x, y]() -> i64 { x = x + 5; return x + y; };\n"
            "    let r = f();\n"
            "    return r + x;\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        // x -> 15 (mutated through by-ref), r = 15 + 20 = 35, r + x = 50
        check(ok,   "B7a: by-ref write-through + mixed capture compiled + ran (GC env)");
        check(r==50,"B7b: by-ref write-through mutates original (returns 50)");
    }

    // [B8] nested by-ref capture (transitive): outer captures x by ref, inner
    //      (nested) lambda also captures x by ref; the inner lambda sees the
    //      post-capture mutation. Exercises the transitive by-ref path (the
    //      inner env copies the POINTER from the outer env).
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut x: i64 = 10;\n"
            "    let outer = fn[&x]() -> i64 {\n"
            "        let inner = fn[&x]() -> i64 { return x; };\n"
            "        return inner();\n"
            "    };\n"
            "    x = 42;\n"
            "    return outer();\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,   "B8a: nested by-ref capture compiled + ran (GC env)");
        check(r==42,"B8b: nested by-ref sees post-capture mutation (returns 42)");
    }

    return g_fail;
}

// ---- PART C: script-visible new/delete (gc_new/gc_delete/gc_collect/gc_live) ----
// The full GC integration's new/delete surface: a script allocates raw GC
// heap objects via gc_new (pinned, so they survive auto-collects), releases
// them via gc_delete (unroot -> collectable), and observes the heap via
// gc_collect/gc_live. This is the script-level proof of: (b) GC collects
// unreachable objects (heap stays bounded), and (c) reachable (pinned)
// objects survive collection. Mirrors Part A's host-API proof but through
// the full compile+run pipeline with the script calling the natives.
static int run_part_c() {
    std::printf("=== PART C: script-visible new/delete (gc_new/gc_delete) ===\n");

    // [C1] new/delete + collect: allocate 100 objects, delete each, collect,
    //      verify live drops to 0 (all unreachable ones collected).
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut i: i64 = 0;\n"
            "    while (i < 100) {\n"
            "        let p = gc_new(16);\n"
            "        gc_delete(p);\n"
            "        i = i + 1;\n"
            "    }\n"
            "    gc_collect();\n"
            "    return gc_live();\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,  "C1a: new/delete loop compiled + ran (no trap)");
        check(r==0,"C1b: after delete+collect, gc_live()==0 (all collected)");
    }

    // [C2] reachable (pinned) objects survive collection: keep 5 objects
    //      reachable (do NOT delete them), delete the rest, collect, verify
    //      live == 5. This is the no-leak proof: reachable objects survive.
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut kept: i64 = 0;\n"
            "    let mut i: i64 = 0;\n"
            "    while (i < 200) {\n"
            "        let p = gc_new(16);\n"
            "        if (i < 5) { kept = kept + 1; }\n"
            "        else { gc_delete(p); }\n"
            "        i = i + 1;\n"
            "    }\n"
            "    gc_collect();\n"
            "    return gc_live();\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,  "C2a: keep-5-delete-rest compiled + ran");
        check(r==5,"C2b: reachable (pinned) survive collect; gc_live()==5");
    }

    // [C3] heap stays bounded under many allocs: 5000 allocs + immediate
    //      delete; the auto-collect threshold (1024) fires mid-run; live must
    //      stay bounded (well under 5000). Final collect -> 0.
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut i: i64 = 0;\n"
            "    while (i < 5000) {\n"
            "        let p = gc_new(32);\n"
            "        gc_delete(p);\n"
            "        i = i + 1;\n"
            "    }\n"
            "    gc_collect();\n"
            "    return gc_live();\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,  "C3a: 5000 alloc+delete loop compiled + ran (auto-collect mid-run)");
        check(r==0,"C3b: heap bounded; after final collect gc_live()==0");
    }

    return g_fail;
}

// ---- PART D: precise root scanning (shadow stack + global roots) ----
// The four end-to-end scenarios for the precise GC root scanning introduced by
// this chunk. Each runs an Ember script through the full pipeline (use_gc_env)
// with the context attached (frame-chain + global-root trace callback) and
// asserts the precise root/reclamation behavior. These were RED against the
// permanent-pin workaround (scenarios 2 + 4: reclamation failed because envs
// stayed pinned forever; 1 + 3 guarded the survival invariant).
static int run_part_d() {
    std::printf("=== PART D: precise root scanning (shadow stack + global roots) ===\n");

    // [D1] an UNPINNED GC lambda environment referenced only by a live JIT local
    //      survives gc_collect (the frame map roots it while the frame is live).
    //      RED-against-pin would have passed vacuously (pinned); this guards the
    //      new invariant: with the permanent pin REMOVED, the env survives via
    //      the frame chain, and calling the lambda AFTER the in-frame collect
    //      still works (the env was not reaped).
    {
        bool ok;
        const char* src =
            "fn worker() -> i64 {\n"
            "    let captured: i64 = 100;\n"
            "    let f = fn(x: i64) -> i64 { return x + captured; };\n"
            "    gc_collect();        // env held only by live local f -> must survive\n"
            "    return f(1);         // 101; if env reaped mid-frame this crashes/wrong\n"
            "}\n"
            "fn main() -> i64 { return worker(); }\n";
        int64_t r = run_one(src, &ok);
        check(ok,    "D1a: unpinned env survives in-frame collect (compiled + ran)");
        check(r==101,"D1b: lambda returns 101 after in-frame gc_collect (env rooted by frame map)");
    }

    // [D2] after the owning frame returns and no other root exists, the env is
    //      RECLAIMED. RED against the permanent pin (env stayed pinned forever;
    //      gc_live never dropped to 0). With precise scanning, when worker's
    //      frame exits its record is unlinked, so the env has no roots -> reaped.
    {
        bool ok;
        const char* src =
            "fn maker() -> i64 {\n"
            "    let captured: i64 = 5;\n"
            "    let f = fn(x: i64) -> i64 { return x + captured; };\n"
            "    return 0;            // f is frame-local; NOT returned -> env does not escape\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    maker();             // maker's frame exits -> its env's record unlinked\n"
            "    gc_collect();        // no frame root, no global, no pin -> reaped\n"
            "    return gc_live();    // expect 0\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,  "D2a: env reclaimed after frame exit (compiled + ran)");
        check(r==0,"D2b: after maker() returns + gc_collect, gc_live()==0 (env reclaimed, not pinned)");
    }

    // [D3] a lambda environment stored in a TYPED GLOBAL survives collection
    //      (the global-root descriptor roots the env_ptr at offset+8). Guards
    //      the global-root path: after the creating frame exits, the env is
    //      reachable ONLY via the global, and it survives a collect. (ember
    //      calls a lambda through a local, so the global is read into a local
    //      before the call — the global still roots the env across the collect.)
    {
        bool ok;
        const char* src =
            "global g : fn(i64) -> i64 = fn(x: i64) -> i64 { return x; };\n"
            "fn mk() -> i64 {\n"
            "    let c : i64 = 10;\n"
            "    let f = fn(x: i64) -> i64 { return x + c; };\n"
            "    g = f;              // store the capturing lambda into the global\n"
            "    return 0;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    mk();               // mk's frame exits; g now roots the env\n"
            "    gc_collect();        // env survives via the global-root descriptor\n"
            "    let h = g;          // read the global lambda into a local\n"
            "    return h(5);         // 15; if env reaped this crashes/wrong\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,  "D3a: lambda env in typed global survives collect (compiled + ran)");
        check(r==15,"D3b: h(5) returns 15 (env rooted by global-root descriptor after frame exit)");
    }

    // [D4] replacing/clearing the global allows the old env to be RECLAIMED.
    //      RED against the permanent pin (old env stayed pinned forever). With
    //      precise scanning, overwriting the global removes the only root -> the
    //      old env is reaped on the next collect. The lambda is called via a
    //      helper (callg) so the local holding the env_ptr is in the HELPER's
    //      frame (unrooted when callg returns), not main's — otherwise main's
    //      frame map would keep rooting the env across the clear.
    {
        bool ok;
        const char* src =
            "global g : fn(i64) -> i64 = fn(x: i64) -> i64 { return x; };\n"
            "fn mk(c: i64) -> i64 {\n"
            "    let f = fn(x: i64) -> i64 { return x + c; };\n"
            "    g = f;              // env rooted via global\n"
            "    return 0;\n"
            "}\n"
            "fn callg(v: i64) -> i64 { let h = g; return h(v); }\n"
            "fn main() -> i64 {\n"
            "    mk(10);             // env A now in g\n"
            "    callg(5);           // use env A (h lives only in callg's frame)\n"
            "    mk(20);             // env B overwrites g -> env A has no root\n"
            "    gc_collect();        // env A reaped; env B survives (in g)\n"
            "    callg(5);           // use env B\n"
            "    g = fn(x: i64) -> i64 { return x; };  // clear -> env B no root\n"
            "    gc_collect();        // env B reaped\n"
            "    return gc_live();   // expect 0\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,  "D4a: replacing/clearing global reclaims old envs (compiled + ran)");
        check(r==0,"D4b: after overwrite + clear + collect, gc_live()==0 (both envs reclaimed)");
    }

    return g_fail;
}

// ---- PART E: optimized IR backend path (precise GC frame-record emit) ----
// Exercises the Thin IR path (thin_lower + thin_emit) with use_gc_env so the
// precise-GC frame-record prologue/epilogue maintenance + the GcFrameMap emit
// in that backend are covered (the default tree-walker run_one does not reach
// them). The IR backend does not yet lower LambdaExpr (a lambda-creating fn
// falls back to the tree-walker), so these scenarios use the script-visible
// gc_new/gc_delete surface + nested calls + a collect issued from INSIDE JIT
// code — no lambdas — to cover the optimized path's GC correctness.
static int run_part_e() {
    std::printf("=== PART E: optimized IR backend (precise GC frame-record emit) ===\n");

    // [E1] gc_new/gc_delete loop through the IR backend: the IR path's
    //      frame-record maintenance must not corrupt the heap or the return.
    //      Same shape as C3 (5000 alloc+delete -> gc_live()==0) but via the
    //      optimized IR backend.
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut i: i64 = 0;\n"
            "    while (i < 5000) {\n"
            "        let p = gc_new(32);\n"
            "        gc_delete(p);\n"
            "        i = i + 1;\n"
            "    }\n"
            "    gc_collect();\n"
            "    return gc_live();\n"
            "}\n";
        int64_t r = run_one_ir(src, &ok);
        check(ok,  "E1a: 5000 new/delete via IR backend compiled + ran (auto-collect mid-run)");
        check(r==0,"E1b: heap bounded; after collect gc_live()==0 (IR backend precise GC safe)");
    }

    // [E2] nested calls + collection from inside JIT code (IR backend): a
    //      helper allocs + deletes, the caller collects, verify reclamation.
    //      Covers the frame-chain across nested IR-backend frames + a collect
    //      issued by JIT'd code (gc_collect native) walking the chain.
    {
        bool ok;
        const char* src =
            "fn worker(n: i64) -> i64 {\n"
            "    let mut i: i64 = 0;\n"
            "    let mut freed: i64 = 0;\n"
            "    while (i < n) {\n"
            "        let p = gc_new(16);\n"
            "        gc_delete(p);\n"
            "        i = i + 1;\n"
            "    }\n"
            "    gc_collect();\n"
            "    return gc_live();\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    let a = worker(100);\n"
            "    let b = worker(200);\n"
            "    gc_collect();\n"
            "    return a + b;\n"  // both 0 -> 0
            "}\n";
        int64_t r = run_one_ir(src, &ok);
        check(ok,  "E2a: nested IR-backend calls + in-JIT collect compiled + ran");
        check(r==0,"E2b: nested workers + collect -> gc_live()==0+0 (frame chain across IR frames)");
    }

    return g_fail;
}

// ---- PART F: try/catch safety across the GC shadow stack ----
// A throw across frames longjmps past the abandoned inner frames WITHOUT
// running their epilogues, so their GcFrameRecords stay linked on
// gc_frame_head with reclaimed stack memory. The catch entry must restore the
// head to the catching frame's record (unlinking the stale ones) so a
// subsequent collect does not walk freed frames. This scenario creates a GC
// lambda env in the try, throws across the frame boundary, catches, and
// collects — verifying no corruption + correct reclamation.
static int run_part_f() {
    std::printf("=== PART F: try/catch safety across the GC shadow stack ===\n");

    // [F1] throw out of a frame that created a GC lambda env; the catch (in an
    //      outer frame) collects. The abandoned inner frame's record must be
    //      unlinked by the catch entry; the collect must not crash or corrupt.
    //      The thrown value (an i64) is returned via the catch.
    {
        bool ok;
        const char* src =
            "fn inner() -> i64 {\n"
            "    let c: i64 = 10;\n"
            "    let f = fn(x: i64) -> i64 { return x + c; };\n"
            "    throw 42;            // abandon inner's frame (env in it)\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    try {\n"
            "        inner();\n"
            "    } catch (e) {\n"
            "        gc_collect();    // collect after the cross-frame throw\n"
            "        return e;        // 42\n"
            "    }\n"
            "    return 0;\n"
            "}\n";
        int64_t r = run_one_ctx(src, &ok);
        check(ok,  "F1a: cross-frame throw + GC collect compiled + ran (no crash)");
        check(r==42,"F1b: caught value 42 returned (shadow stack safe across throw)");
    }

    // [F2] nested try/catch + a GC collect in the catch, then a fresh lambda
    //      created + used after recovery — the frame chain must be consistent.
    {
        bool ok;
        const char* src =
            "fn boom() -> i64 { throw 7; }\n"
            "fn main() -> i64 {\n"
            "    let mut caught: i64 = 0;\n"
            "    try {\n"
            "        try {\n"
            "            boom();\n"
            "        } catch (e1) {\n"
            "            throw e1 + 1;   // re-throw across the inner frame\n"
            "        }\n"
            "    } catch (e2) {\n"
            "        caught = e2;        // 8\n"
            "    }\n"
            "    gc_collect();\n"
            "    let c: i64 = 100;\n"
            "    let f = fn(x: i64) -> i64 { return x + c; };\n"
            "    return f(caught);      // 8 + 100 = 108\n"
            "}\n";
        int64_t r = run_one_ctx(src, &ok);
        check(ok,   "F2a: nested try/catch + post-recovery lambda compiled + ran");
        check(r==108,"F2b: nested throw recovery + fresh lambda returns 108 (chain consistent)");
    }

    return g_fail;
}

// ---- PART G: host-store extension trace-callback integration (c1 facade) ----
// The vec / string / array / map host-store extensions register idempotent
// trace callbacks against the current thread-local GC runtime (the c1 facade
// in ext_gc) so an UNPINNED GC object stored in their external-root storage is
// rooted through the extension while the entry is present, and reclaimed once
// the entry is removed / cleared / the extension is reset. Array tracing visits
// only aligned i64 slots whose elem_size == 8; map tracing visits candidate
// pointer-bearing keys AND values; the heap visitor rejects ordinary integers
// + stale / non-live addresses (so a u8 array or a non-pointer i64 never
// creates a false root). vec + string contain NO GC-managed children (float /
// char bytes), so they register no-op LEAF callbacks that report nothing --
// guarding that their no-child layouts do not create false roots. Every
// pointer-capable mutation (array set_i64 / push_i64, map_set) also invokes the
// GC write-barrier facade (ceremonial for the current stop-the-world collector:
// the external-root owner is not a GC object, so the barrier is a no-op, but
// the call site is the surface a future remembered-set collector would use).
//
// These are HOST-side tests (call the registered native fn_ptrs directly + the
// ext_gc facade), mirroring ext_map_test / ext_bounds_test's direct-coverage
// shape. They were RED before the extensions registered trace callbacks (an
// unpinned object stored in array/map was collected immediately because the
// extension never reported it as a root).
static int run_part_g() {
    std::printf("=== PART G: host-store extension trace-callback integration ===\n");
    ext_gc::gc_init();

    // Pull the array + map + vec + string native fn_ptrs from a registered
    // table (the same direct-coverage shape ext_map_test / ext_bounds_test
    // use). vec/string are exercised through their host helpers (alloc /
    // vec3_new) directly.
    std::unordered_map<std::string, NativeSig> natives;
    ext_array::register_natives(natives);
    ext_map::register_natives(natives);
    ext_vec::register_natives(natives);
    ext_string::register_natives(natives);

    auto grab = [&](const char* name) -> void* {
        auto it = natives.find(name);
        if (it == natives.end()) { std::printf("FAIL: native '%s' not registered\n", name); g_fail = 1; return nullptr; }
        return it->second.fn_ptr;
    };
    using F_array_new    = int64_t(*)(int64_t, int64_t);
    using F_array_set_i64 = void(*)(int64_t, int64_t, int64_t);
    using F_array_get_i64 = int64_t(*)(int64_t, int64_t);
    using F_array_push_i64 = void(*)(int64_t, int64_t);
    using F_array_clear  = void(*)(int64_t);
    using F_array_remove = void(*)(int64_t, int64_t);
    using F_array_set_u8 = void(*)(int64_t, int64_t, int64_t);
    using F_map_new      = int64_t(*)();
    using F_map_set      = void(*)(int64_t, int64_t, int64_t);
    using F_map_get      = int64_t(*)(int64_t, int64_t);
    using F_map_remove   = void(*)(int64_t, int64_t);
    using F_map_clear    = void(*)(int64_t);
    auto p_arr_new    = reinterpret_cast<F_array_new>(grab("array_new"));
    auto p_set_i64    = reinterpret_cast<F_array_set_i64>(grab("array_set_i64"));
    auto p_get_i64    = reinterpret_cast<F_array_get_i64>(grab("array_get_i64"));
    auto p_push_i64   = reinterpret_cast<F_array_push_i64>(grab("array_push_i64"));
    auto p_arr_clear  = reinterpret_cast<F_array_clear>(grab("array_clear"));
    auto p_arr_remove = reinterpret_cast<F_array_remove>(grab("array_remove"));
    auto p_set_u8     = reinterpret_cast<F_array_set_u8>(grab("array_set_u8"));
    auto p_map_new    = reinterpret_cast<F_map_new>(grab("map_new"));
    auto p_map_set    = reinterpret_cast<F_map_set>(grab("map_set"));
    auto p_map_get    = reinterpret_cast<F_map_get>(grab("map_get"));
    auto p_map_remove = reinterpret_cast<F_map_remove>(grab("map_remove"));
    auto p_map_clear  = reinterpret_cast<F_map_clear>(grab("map_clear"));
    if (g_fail) return g_fail;

    // Helper: alloc an UNPINNED GC object (gc_alloc_env does not pin). It is
    // reachable ONLY via whatever extension store we put it in.
    auto alloc_unpinned = [](int64_t sz) -> int64_t {
        return ext_gc::gc_alloc_env(sz);
    };

    // [G1] array<i64>: an unpinned GC object stored in an i64 element SURVIVES
    //      gc_collect (the array trace callback reports the slot's value as a
    //      root candidate; the visitor validates it live + marks it). Then it
    //      is RECLAIMED after array_clear removes the entry.
    {
        ext_gc::gc_reset(); ext_array::reset();
        int64_t h = p_arr_new(8, 4);  // elem_size=8, 4 slots
        check(h >= 1, "G1a: array_new(8,4) handle");
        int64_t p = alloc_unpinned(16);
        check(p != 0, "G1b: gc_alloc_env returns non-zero");
        p_set_i64(h, 0, p);           // store the GC ptr; registers trace cb + barrier
        check(p_get_i64(h, 0) == p, "G1c: array_get_i64 reads back the stored ptr");
        ext_gc::gc_collect();         // callback reports slot 0 -> p survives
        check(ext_gc::gc_is_live(p), "G1d: unpinned obj survives collect via array trace cb");
        check(ext_gc::gc_live_count() == 1, "G1e: live_count==1 (rooted by array)");
        p_arr_clear(h);               // remove the entry holding p
        ext_gc::gc_collect();         // p no longer reported -> freed
        check(!ext_gc::gc_is_live(p), "G1f: obj reclaimed after array_clear + collect");
        check(ext_gc::gc_live_count() == 0, "G1g: live_count==0 after reclaim");
    }

    // [G2] array<i64>: reclaimed after array_remove (a single entry removed,
    //      not a full clear). Other slots' objects survive.
    {
        ext_gc::gc_reset(); ext_array::reset();
        int64_t h = p_arr_new(8, 4);
        int64_t a = alloc_unpinned(16);
        int64_t b = alloc_unpinned(16);
        p_set_i64(h, 0, a);
        p_set_i64(h, 1, b);
        ext_gc::gc_collect();
        check(ext_gc::gc_is_live(a) && ext_gc::gc_is_live(b),
              "G2a: both objs survive collect (two array slots)");
        p_arr_remove(h, 0);           // remove slot 0 (a) only
        ext_gc::gc_collect();
        check(!ext_gc::gc_is_live(a), "G2b: removed-slot obj reclaimed after collect");
        check(ext_gc::gc_is_live(b), "G2c: sibling obj still survives (slot 1 present)");
        p_arr_clear(h);
        ext_gc::gc_collect();
        check(!ext_gc::gc_is_live(b), "G2d: sibling reclaimed after clear");
    }

    // [G3] array<i64> push: a pushed GC ptr survives; pop removes it -> reclaim.
    {
        ext_gc::gc_reset(); ext_array::reset();
        int64_t h = p_arr_new(8, 0);  // empty, grow via push
        int64_t p = alloc_unpinned(16);
        p_push_i64(h, p);             // push registers trace cb + barrier
        ext_gc::gc_collect();
        check(ext_gc::gc_is_live(p), "G3a: pushed obj survives collect");
        // Overwrite slot 0 with a non-pointer integer (0). The trace cb now
        // reports 0 -> visitor ignores null -> p has no root -> reclaimed.
        p_set_i64(h, 0, 0);
        ext_gc::gc_collect();
        check(!ext_gc::gc_is_live(p), "G3b: obj reclaimed after slot overwritten with 0");
    }

    // [G4] array<u8> does NOT root a GC object: a u8 array (elem_size=1) cannot
    //      hold an 8-byte pointer in one slot; the trace cb only visits
    //      elem_size==8 slots. Storing a byte of the ptr via set_u8 must not
    //      root the object. Guards the elem_size==8 guard against false roots
    //      from byte-level reinterpretation.
    {
        ext_gc::gc_reset(); ext_array::reset();
        int64_t h = p_arr_new(1, 8);  // elem_size=1 (u8)
        int64_t p = alloc_unpinned(16);
        // Store the low byte of p into a u8 slot -- this MUST NOT root p.
        p_set_u8(h, 0, int64_t(uint8_t(uintptr_t(p) & 0xff)));
        ext_gc::gc_collect();
        check(!ext_gc::gc_is_live(p),
              "G4a: u8 array does not root a GC obj (elem_size guard: no false root)");
        check(ext_gc::gc_live_count() == 0, "G4b: live_count==0 (u8 array is a leaf)");
    }

    // [G5] map value: an unpinned GC object stored as a map VALUE survives
    //      collect; reclaimed after map_remove.
    {
        ext_gc::gc_reset(); ext_map::reset();
        int64_t h = p_map_new();
        check(h >= 1, "G5a: map_new handle");
        int64_t p = alloc_unpinned(16);
        p_map_set(h, 1, p);           // value is p; registers trace cb + barrier
        check(p_map_get(h, 1) == p, "G5b: map_get reads back the value ptr");
        ext_gc::gc_collect();
        check(ext_gc::gc_is_live(p), "G5c: unpinned obj survives collect via map value trace cb");
        p_map_remove(h, 1);
        ext_gc::gc_collect();
        check(!ext_gc::gc_is_live(p), "G5d: obj reclaimed after map_remove + collect");
    }

    // [G6] map key: an unpinned GC object stored as a map KEY survives collect;
    //      reclaimed after map_clear.
    {
        ext_gc::gc_reset(); ext_map::reset();
        int64_t h = p_map_new();
        int64_t p = alloc_unpinned(16);
        p_map_set(h, p, 99);          // key is p; registers trace cb + barrier
        ext_gc::gc_collect();
        check(ext_gc::gc_is_live(p), "G6a: unpinned obj survives collect via map key trace cb");
        p_map_clear(h);
        ext_gc::gc_collect();
        check(!ext_gc::gc_is_live(p), "G6b: obj reclaimed after map_clear + collect");
    }

    // [G7] map replacement: overwriting a value reclaims the OLD value (it is
    //      no longer in the map) while the NEW value survives. Guards that the
    //      trace cb walks the CURRENT entries, not a stale snapshot.
    {
        ext_gc::gc_reset(); ext_map::reset();
        int64_t h = p_map_new();
        int64_t a = alloc_unpinned(16);
        p_map_set(h, 1, a);
        ext_gc::gc_collect();
        check(ext_gc::gc_is_live(a), "G7a: first value survives");
        // Allocate b AFTER the first collect so it is not reaped before it is
        // stored (an unpinned object not yet in any root is collectable).
        int64_t b = alloc_unpinned(16);
        p_map_set(h, 1, b);           // replace: a no longer in map, b in
        ext_gc::gc_collect();
        check(!ext_gc::gc_is_live(a), "G7b: old value reclaimed after replacement");
        check(ext_gc::gc_is_live(b), "G7c: new value survives replacement");
        p_map_clear(h);
        ext_gc::gc_collect();
        check(!ext_gc::gc_is_live(b), "G7d: new value reclaimed after clear");
    }

    // [G8] vec leaf: registering vec's no-op leaf trace callback does NOT create
    //      a false root. A vec holds floats (3/4 bytes), never GC pointers, so
    //      the leaf cb reports nothing; an unrelated unpinned object is still
    //      collected. Guards that vec's no-child layout never false-roots.
    {
        ext_gc::gc_reset(); ext_vec::reset();
        // Creating a vec lazily registers the leaf callback (GC is initialized).
        // vec3_new(float,float,float) -> i64 handle; pull its fn_ptr.
        auto it = natives.find("vec3_new");
        check(it != natives.end(), "G8a: vec3_new registered");
        using F_v3 = int64_t(*)(float, float, float);
        auto v3new = reinterpret_cast<F_v3>(it->second.fn_ptr);
        int64_t vh = v3new(1.0f, 2.0f, 3.0f);  // registers vec leaf trace cb
        check(vh >= 1, "G8b: vec3_new handle");
        int64_t p = alloc_unpinned(16);  // NOT stored in the vec
        ext_gc::gc_collect();
        check(!ext_gc::gc_is_live(p),
              "G8c: vec leaf cb creates no false root (unrelated obj collected)");
        // The vec itself is a host store object, not a GC object, so it is not
        // tracked by gc_live_count; confirm the collect did not keep p.
        check(ext_gc::gc_live_count() == 0, "G8d: live_count==0 (vec leaf reports nothing)");
    }

    // [G9] string leaf: registering string's no-op leaf trace callback does NOT
    //      create a false root. A string holds chars (1 byte), never GC
    //      pointers; the leaf cb reports nothing.
    {
        ext_gc::gc_reset(); ext_string::reset();
        int64_t sh = ember::ext_string::alloc(std::string("hello"));  // registers leaf cb
        check(sh >= 1, "G9a: string alloc handle");
        int64_t p = alloc_unpinned(16);  // NOT stored in the string
        ext_gc::gc_collect();
        check(!ext_gc::gc_is_live(p),
              "G9b: string leaf cb creates no false root (unrelated obj collected)");
        check(ext_gc::gc_live_count() == 0, "G9c: live_count==0 (string leaf reports nothing)");
    }

    // [G10] extension reset reclaims: ext_array::reset() drops the store +
    //       unregisters the trace callback, so a previously-rooted object is
    //       reclaimed on the next collect (the callback cannot outlive reset).
    {
        ext_gc::gc_reset(); ext_array::reset();
        int64_t h = p_arr_new(8, 4);
        int64_t p = alloc_unpinned(16);
        p_set_i64(h, 0, p);
        ext_gc::gc_collect();
        check(ext_gc::gc_is_live(p), "G10a: obj rooted by array before reset");
        ember::ext_array::reset();    // clears store + unregisters trace cb
        ext_gc::gc_collect();         // no callback -> no root -> p reclaimed
        check(!ext_gc::gc_is_live(p), "G10b: obj reclaimed after ext_array::reset + collect");
    }

    // [G11] idempotent registration: repeated set_i64 on the same array does
    //      not double-register the callback (collect still finds exactly one
    //      root per slot; no duplicate marking, no blow-up). Guards the
    //      idempotent-registration contract.
    {
        ext_gc::gc_reset(); ext_array::reset();
        int64_t h = p_arr_new(8, 4);
        int64_t p = alloc_unpinned(16);
        p_set_i64(h, 0, p);
        p_set_i64(h, 0, p);           // idempotent: same ptr, re-register
        p_set_i64(h, 0, p);
        ext_gc::gc_collect();
        check(ext_gc::gc_is_live(p), "G11a: obj survives after repeated idempotent set_i64");
        check(ext_gc::gc_live_count() == 1, "G11b: live_count==1 (no duplicate roots)");
        ext_array::reset();
    }

    // [G12] non-GC-inactive: when the GC runtime is NOT initialized on a thread,
    //      array/map operations work byte-identically (set/get round-trip) with
    //      no GC interference. (gc_integration_test already called gc_init, so
    //      we instead confirm gc_runtime_initialized reflects state + that the
    //      array still round-trips a plain integer while GC is active -- the
    //      ordinary-integer case the visitor must reject.)
    {
        ext_gc::gc_reset(); ext_array::reset();
        check(ext_gc::gc_runtime_initialized(), "G12a: gc_runtime_initialized true after gc_init");
        int64_t h = p_arr_new(8, 4);
        p_set_i64(h, 2, 123456789);   // a plain integer, not a pointer
        check(p_get_i64(h, 2) == 123456789, "G12b: plain i64 round-trips through array");
        // The plain integer is not a live GC object, so the trace cb reporting
        // it is rejected by the visitor -> no false root + no surviving object.
        ext_gc::gc_collect();
        check(ext_gc::gc_live_count() == 0,
              "G12c: plain-integer array slot creates no false root (visitor rejects it)");
        ext_array::reset();
    }

    // Cleanup the extension stores so later parts start clean.
    ext_array::reset(); ext_map::reset(); ext_vec::reset(); ext_string::reset();
    ext_gc::gc_reset();
    return g_fail;
}

// ---- PART H: deterministic new/delete substrate (gc_alloc_object / ----
// ---- gc_delete_object) ----
// The runtime substrate for the future language-level `new`/`delete` operators,
// exposed as the internal native bindings __ember_gc_alloc_object /
// __ember_gc_delete_object. These are HOST-side tests (call the ext_gc host
// API directly) verifying the new surface is distinct from the legacy
// gc_new/gc_delete (pin/unpin) compatibility surface:
//   - gc_alloc_object allocates an UNPINNED object (same thread-local heap +
//     collect-before-allocate threshold safety as gc_alloc_env).
//   - gc_delete_object calls GcHeap::free_object for DETERMINISTIC immediate
//     destruction (frees NOW, not just unpin -> collectable later).
// This guards that the new immediate-free primitive works through the
// extension layer + that the legacy gc_new/gc_delete pin/unpin behavior is
// preserved (no regression).
static int run_part_h() {
    std::printf("=== PART H: deterministic new/delete substrate (gc_alloc_object / gc_delete_object) ===\n");
    ext_gc::gc_init();

    // [H1] gc_alloc_object returns a live UNPINNED object; gc_delete_object
    //      frees it IMMEDIATELY (is_live false right away, no collect needed).
    {
        ext_gc::gc_reset();
        int64_t p = ext_gc::gc_alloc_object(24);
        check(p != 0, "H1a: gc_alloc_object returns non-zero ptr");
        check(ext_gc::gc_is_live(p), "H1b: allocated object is live");
        check(ext_gc::gc_live_count() == 1, "H1c: live_count==1 after alloc");
        check(ext_gc::gc_delete_object(p) == 1, "H1d: gc_delete_object returns 1 (freed)");
        check(!ext_gc::gc_is_live(p), "H1e: object not live after gc_delete_object");
        check(ext_gc::gc_live_count() == 0, "H1f: live_count==0 after immediate free");
        check(ext_gc::gc_freed_count() == 1, "H1g: freed_count==1 (immediate, no collect)");
        // A subsequent collect must not double-free or change the count.
        ext_gc::gc_collect();
        check(ext_gc::gc_freed_count() == 1, "H1h: collect after delete does not re-free");
        check(ext_gc::gc_live_count() == 0, "H1i: live_count still 0 after post-delete collect");
    }

    // [H2] gc_delete_object rejects null / non-live / already-freed pointers
    //      (returns 0), without touching stats. Repeated delete is idempotent.
    {
        ext_gc::gc_reset();
        check(ext_gc::gc_delete_object(0) == 0, "H2a: gc_delete_object(0) returns 0");
        // A non-GC integer (not a live pointer) -> 0.
        check(ext_gc::gc_delete_object(0x1234) == 0, "H2b: gc_delete_object(non-ptr) returns 0");
        int64_t p = ext_gc::gc_alloc_object(16);
        check(ext_gc::gc_delete_object(p) == 1, "H2c: first delete returns 1");
        check(ext_gc::gc_delete_object(p) == 0, "H2d: second delete returns 0 (already freed)");
        check(ext_gc::gc_delete_object(p) == 0, "H2e: third delete returns 0 (still freed)");
        check(ext_gc::gc_freed_count() == 1, "H2f: freed_count==1 (no double-count)");
        check(ext_gc::gc_live_count() == 0, "H2g: live_count==0");
    }

    // [H3] gc_delete_object is DETERMINISTIC destruction, NOT unpinning:
    //      contrast with gc_delete (the legacy unpin native). gc_new allocs +
    //      PINS; gc_delete UNPINS (the object is still on the heap until a
    //      collect); gc_delete_object frees IMMEDIATELY (gone before any
    //      collect). This guards the unpinning-vs-destruction distinction +
    //      that the legacy surface still works (no regression).
    {
        ext_gc::gc_reset();
        // Legacy surface: gc_new (pin) + gc_delete (unpin) -> still live until collect.
        int64_t a = ext_gc::gc_alloc_env(16); ext_gc::gc_root_env(a); // simulate gc_new
        check(ext_gc::gc_is_live(a), "H3a: legacy pinned object live");
        ext_gc::gc_unroot_env(a);                 // simulate gc_delete (unpin)
        check(ext_gc::gc_is_live(a), "H3b: unpinning does NOT free immediately (still live)");
        check(ext_gc::gc_live_count() == 1, "H3c: live_count==1 after unpin (not freed yet)");
        ext_gc::gc_collect();                     // NOW the collector reaps it
        check(!ext_gc::gc_is_live(a), "H3d: unpinning -> collected on next collect");
        // New surface: gc_delete_object frees IMMEDIATELY (no collect needed).
        int64_t b = ext_gc::gc_alloc_object(16);
        check(ext_gc::gc_is_live(b), "H3e: new object live before delete_object");
        ext_gc::gc_delete_object(b);              // DETERMINISTIC destruction
        check(!ext_gc::gc_is_live(b), "H3f: gc_delete_object frees IMMEDIATELY (gone, no collect)");
        check(ext_gc::gc_live_count() == 0, "H3g: live_count==0 after immediate delete");
    }

    // [H4] stale root slot after gc_delete_object: pin an object (gc_root_env),
    //      then gc_delete_object it. The pin slot still holds the freed pointer;
    //      a subsequent collect must ignore the stale slot (is_live -> false),
    //      not crash, not resurrect, not double-count the free.
    {
        ext_gc::gc_reset();
        int64_t p = ext_gc::gc_alloc_object(16);
        ext_gc::gc_root_env(p);                   // pin: a root slot now holds p
        check(ext_gc::gc_delete_object(p) == 1, "H4a: gc_delete_object on a pinned obj returns 1");
        check(!ext_gc::gc_is_live(p), "H4b: pinned obj freed immediately by gc_delete_object");
        size_t freed_before = size_t(ext_gc::gc_freed_count());
        ext_gc::gc_collect();                     // stale pin slot -> ignored
        check(ext_gc::gc_freed_count() == int64_t(freed_before),
              "H4c: stale pin slot not re-freed by collect");
        check(!ext_gc::gc_is_live(p), "H4d: no resurrection via stale pin slot");
        check(ext_gc::gc_live_count() == 0, "H4e: live_count==0 (stale pin ignored)");
    }

    // [H5] collect-before-allocate threshold safety: gc_alloc_object uses the
    //      SAME threshold as gc_alloc_env. Allocating past the threshold
    //      triggers a collect FIRST (so the new object is the newest + the next
    //      collect is at the next alloc). Verify with a low threshold + many
    //      unpinned allocs that the heap stays bounded (the auto-collect reaps
    //      the unrooted objects).
    {
        ext_gc::gc_reset();
        ext_gc::gc_set_threshold(64);
        for (int i = 0; i < 500; ++i) {
            (void)ext_gc::gc_alloc_object(16);   // unrooted -> collectable
        }
        check(ext_gc::gc_live_count() < 64 + 16,
              "H5a: heap bounded (< ~80) after 500 unpinned allocs (auto-collect via threshold)");
        ext_gc::gc_collect();
        check(ext_gc::gc_live_count() == 0, "H5b: after final collect, live_count==0 (all reaped)");
        check(ext_gc::gc_freed_count() >= 500, "H5c: freed_count>=500 (auto + explicit collect)");
        ext_gc::gc_set_threshold(1024);           // restore default
    }

    // [H6] the new internal natives are registered (stable compiler contract):
    //      __ember_gc_alloc_object + __ember_gc_delete_object resolve in a
    //      registered native table. Guards that the compiler can lower the
    //      future language `new`/`delete` to these names.
    {
        std::unordered_map<std::string, ember::NativeSig> natives;
        ext_gc::register_natives(natives);
        auto it_a = natives.find("__ember_gc_alloc_object");
        auto it_d = natives.find("__ember_gc_delete_object");
        check(it_a != natives.end(), "H6a: __ember_gc_alloc_object registered");
        check(it_d != natives.end(), "H6b: __ember_gc_delete_object registered");
        // The legacy surface is still registered (no regression).
        check(natives.count("gc_new") && natives.count("gc_delete"),
              "H6c: legacy gc_new/gc_delete still registered (compatibility preserved)");
        // Call the registered fn_ptrs directly to confirm the binding works.
        if (it_a != natives.end() && it_d != natives.end()) {
            using F_alloc = int64_t(*)(int64_t);
            using F_delete = int64_t(*)(int64_t);
            auto f_alloc = reinterpret_cast<F_alloc>(it_a->second.fn_ptr);
            auto f_delete = reinterpret_cast<F_delete>(it_d->second.fn_ptr);
            int64_t p = f_alloc(16);
            check(p != 0 && ext_gc::gc_is_live(p), "H6d: __ember_gc_alloc_object fn_ptr allocs a live obj");
            check(f_delete(p) == 1, "H6e: __ember_gc_delete_object fn_ptr frees (returns 1)");
            check(!ext_gc::gc_is_live(p), "H6f: object gone after native delete");
        }
        ext_gc::gc_reset();
    }

    ext_gc::gc_reset();
    return g_fail;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int a = run_part_a();
    int b = run_part_b();
    int c = run_part_c();
    int d = run_part_d();
    int e = run_part_e();
    int f = run_part_f();
    int g = run_part_g();
    int h = run_part_h();
    std::printf("\ngc_integration_test: %s\n", (a || b || c || d || e || f || g || h) ? "FAIL" : "PASS");
    return (a || b || c || d || e || f || g || h) ? 1 : 0;
}
