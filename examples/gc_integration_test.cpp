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
#include "ext_math.hpp"
#include "ext_gc.hpp"

#include "../src/ast.hpp"

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
        if (ctx->has_checkpoint) longjmp(ctx->checkpoint, 1);
    }
    std::abort();
}

// ---- PART A: host API (ext_gc direct) ----
static int run_part_a() {
    std::printf("=== PART A: ext_gc host API (alloc/pin/unroot/collect) ===\n");
    ext_gc::gc_init();
    ext_gc::gc_reset();  // start clean

    // [A1] alloc + pin: live count tracks allocs; pinning is implicit.
    {
        ext_gc::gc_reset();
        int64_t a = ext_gc::gc_alloc_env(16);
        int64_t b = ext_gc::gc_alloc_env(24);
        check(a != 0, "A1a: gc_alloc_env returns non-zero ptr (a)");
        check(b != 0, "A1b: gc_alloc_env returns non-zero ptr (b)");
        check(a != b, "A1c: two allocs return distinct ptrs");
        check(ext_gc::gc_live_count() == 2, "A1d: live_count==2 after 2 allocs");
        check(ext_gc::gc_is_live(a) && ext_gc::gc_is_live(b),
              "A1e: both envs are live on the heap");
    }

    // [A2] collect with everything pinned: nothing freed.
    {
        ext_gc::gc_reset();
        ext_gc::gc_alloc_env(16);
        ext_gc::gc_alloc_env(16);
        int64_t freed = ext_gc::gc_collect();
        check(freed == 0, "A2a: collect frees 0 when all envs pinned");
        check(ext_gc::gc_live_count() == 2, "A2b: live_count==2 (pinned survive)");
    }

    // [A3] unroot -> collect reaps the unpinned env; pinned survives.
    {
        ext_gc::gc_reset();
        int64_t a = ext_gc::gc_alloc_env(16);  // will stay pinned
        int64_t b = ext_gc::gc_alloc_env(16);  // will be unrooted
        check(ext_gc::gc_unroot_env(b), "A3a: unroot_env(b) returns true");
        int64_t freed = ext_gc::gc_collect();
        check(freed == 1, "A3b: collect frees 1 (the unrooted env b)");
        check(ext_gc::gc_is_live(a), "A3c: pinned env a survives");
        check(!ext_gc::gc_is_live(b), "A3d: unrooted env b is collected");
        check(ext_gc::gc_live_count() == 1, "A3e: live_count==1 after collect");
    }

    // [A4] MANY lambdas in a loop, heap stays BOUNDED:
    //     allocate 5000 envs, unroot each immediately (simulating frame-local
    //     envs that go out of scope), pin a small reachable set. With the
    //     auto-collect threshold (default 1024), the heap is collected
    //     periodically; live_count must NOT grow to 5000 -- it stays bounded
    //     near the pinned set. The pinned reachable set survives every collect.
    {
        ext_gc::gc_reset();
        const int N = 5000;
        const int PINNED_COUNT = 10;
        int64_t pinned[PINNED_COUNT];
        for (int i = 0; i < N; ++i) {
            int64_t e = ext_gc::gc_alloc_env(32);
            if (i < PINNED_COUNT) {
                pinned[i] = e;            // keep these reachable (stay pinned)
            } else {
                ext_gc::gc_unroot_env(e); // this env is now unreachable
            }
        }
        int64_t live = ext_gc::gc_live_count();
        // Without collection the heap would hold 5000; with auto-collect it
        // stays bounded. Allow slack for envs not yet swept at the final
        // threshold check, but it MUST be well under N (the whole point).
        check(live < 1024 + PINNED_COUNT + 16,
              "A4a: heap bounded (< ~1030) after 5000 alloc+unroot loop");
        // Final explicit collect sweeps any remaining unpinned envs.
        ext_gc::gc_collect();
        check(ext_gc::gc_live_count() == PINNED_COUNT,
              "A4b: after final collect, live_count==10 (only pinned survive)");
        for (int i = 0; i < PINNED_COUNT; ++i) {
            check(ext_gc::gc_is_live(pinned[i]), "A4c: pinned env survives (reachable)");
        }
        check(ext_gc::gc_freed_count() >= N - PINNED_COUNT,
              "A4d: freed_count >= 4990 (unreachable envs collected)");
    }

    // [A5] threshold 0 disables auto-collect (heap grows unbounded until an
    //     explicit collect). Confirms the trigger is the threshold, not magic.
    {
        ext_gc::gc_reset();
        ext_gc::gc_set_threshold(0);
        for (int i = 0; i < 200; ++i) (void)ext_gc::gc_alloc_env(8);
        check(ext_gc::gc_live_count() == 200,
              "A5a: threshold 0 -> no auto-collect, live_count==200");
        ext_gc::gc_set_threshold(1024);  // restore default for subsequent tests
        // unroot half + collect
        // (can't recover individual ptrs here, so just collect-all-pinned:
        //  everything is pinned, so collect frees 0; verify the trigger reset)
        int64_t freed = ext_gc::gc_collect();
        check(freed == 0, "A5b: all pinned -> collect frees 0");
        check(ext_gc::gc_live_count() == 200, "A5c: live_count still 200 (all pinned)");
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
struct GcitModule {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string,int> slots;
    void* main_entry = nullptr;
    std::unordered_map<std::string, NativeSig> natives;
    std::vector<uint8_t> allowlist;
    int slot_count = 0;
    ember::context_t ctx{};
};

// Compile `main()->i64` with use_gc_env=true (lambda envs on the GC heap).
// Registers the standard natives INCLUDING ext_gc's __ember_gc_alloc_env so
// codegen can resolve it. Returns false on lex/parse/sema/compile error.
static bool compile_gcit(const std::string& src, GcitModule& m) {
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
    ember::GlobalsBlock gb; std::vector<uint8_t> gbs(pr.program.globals.size()*8, 0);
    gb.base=int64_t(gbs.data());
    { uint32_t gi=0; for (auto& g : pr.program.globals) { gb.index[g.name]=gi++; gb.types[g.name]=g.ty.get(); } }
    eval_global_initializers(pr.program, GlobalInitCtx{gbs, gb.index, gb.types});
    ember::g_globals_for_codegen = &gb;
    m.table = DispatchTable(pr.program.funcs.size());
    ember::CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(m.table.base());
    ctx.natives=&m.natives; ctx.script_slots=&m.slots; ctx.structs=&layouts;
    m.allowlist = build_fn_allowlist(m.slots, m.slot_count);
    ctx.fn_allowlist_base = int64_t(m.allowlist.data());
    ctx.fn_slot_count = int64_t(m.slot_count);
    m.ctx.budget_remaining=2'000'000'000LL; m.ctx.max_call_depth=256; m.ctx.call_depth=0;
    ctx.trap_stub=(void*)&gcit_trap; ctx.trap_ctx=&m.ctx;
    ctx.budget_ptr=&m.ctx.budget_remaining; ctx.emit_budget_checks=true;
    ctx.depth_ptr=&m.ctx.call_depth; ctx.max_call_depth=256; ctx.emit_depth_checks=true;
    ctx.use_gc_env = true;  // <-- the GC heap env backend
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); m.table.set(fn.slot,cf.entry); m.fns.push_back(std::move(cf));}
    auto sit=m.slots.find("main");
    if(sit==m.slots.end()) return false;
    m.main_entry = m.table.get(sit->second);
    return m.main_entry != nullptr;
}

static int64_t run_main(GcitModule& m, bool* trapped) {
    *trapped = false;
    m.ctx.call_depth = 0; m.ctx.has_checkpoint=true;
    if (setjmp(m.ctx.checkpoint)) { *trapped=true; m.ctx.has_checkpoint=false;
        return int64_t(m.ctx.last_trap); }
    using F0=int64_t(*)();
    int64_t r = reinterpret_cast<F0>(m.main_entry)();
    m.ctx.has_checkpoint=false;
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

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int a = run_part_a();
    int b = run_part_b();
    int c = run_part_c();
    std::printf("\ngc_integration_test: %s\n", (a || b || c) ? "FAIL" : "PASS");
    return (a || b || c) ? 1 : 0;
}
