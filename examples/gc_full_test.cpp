// gc_full_test.cpp - full GC integration: by-reference capture + new/delete.
//
// The focused end-to-end proof for the GC full-integration task. Exercises the
// three task requirements through the FULL engine pipeline (resolve_imports ->
// tokenize -> parse -> sema -> compile_func -> finalize -> call) with the GC
// extension registered + use_gc_env=true:
//
//   (a) By-reference capture: a lambda captures a variable by ref (`fn[&x]`),
//       the variable is modified AFTER capture, the lambda sees the new value.
//       Also: write-through (the lambda mutates the original through the ref)
//       + mixed by-ref/by-value capture + nested by-ref (transitive).
//   (b) new/delete: allocate many objects via gc_new, release via gc_delete,
//       gc_collect reaps the unreachable ones -> heap stays bounded (gc_live
//       stays low), NOT 5000.
//   (c) No leaks: reachable (pinned) objects survive collection (gc_live counts
//       the survivors, not the freed).
//
// This is a thin, scenario-focused companion to gc_integration_test (which has
// the broader host-API + codegen coverage). Same harness shape (compile+run
// one source, return i64 main()).
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

extern "C" void gft_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) EMBER_LONGJMP(ctx->checkpoint, 1);
    }
    std::abort();
}

struct GftModule {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string,int> slots;
    void* main_entry = nullptr;
    std::unordered_map<std::string, NativeSig> natives;
    std::vector<uint8_t> allowlist;
    int slot_count = 0;
    ember::context_t ctx{};
    // Precise GC: typed globals block storage + descriptor (outlive the run).
    std::vector<uint8_t> gb_store;
    ember::GlobalsBlock gb;
    ember::gc::GcGlobalRoots global_roots;
};

// Precise GC global-root support: typed globals layout + GcGlobalRoots. Mirrors
// gc_integration_test's gcit_host_value_bytes.
static uint32_t gft_host_value_bytes(const ember::Type* t) {
    if (!t) return 8;
    if (t->is_slice) return 16;
    if (t->is_lambda) return 16;   // {fn_slot, env_ptr}
    if (t->array_len > 0)
        return uint32_t(t->array_len) * gft_host_value_bytes(t->elem.get());
    if (!t->struct_name.empty()) return 8;
    switch (t->prim) {
    case ember::Prim::Bool: case ember::Prim::I8: case ember::Prim::U8: return 1;
    case ember::Prim::I16: case ember::Prim::U16: return 2;
    case ember::Prim::I32: case ember::Prim::U32: case ember::Prim::F32: return 4;
    default: return 8;
    }
}

static bool compile_gft(const std::string& src, GftModule& m, bool ir = false, bool ctx_reg = false) {
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
    ext_map::register_natives(m.natives);
    ext_gc::register_natives(m.natives);  // __ember_gc_* + gc_new/gc_delete/gc_collect/gc_live
    ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);
    ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
    auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    auto sr=sema(pr.program,m.natives,m.slots,0,&ov,&layouts);
    if(!sr.ok){ std::printf("  sema (%zu errors):\n",sr.errors.size());
        for(auto&e:sr.errors) std::printf("  line %u: %s\n",e.line,e.msg.c_str());
        return false;
    }
    ember::GlobalsBlock gb; std::vector<uint8_t> gbs(pr.program.globals.size()*8, 0);
    // Precise GC: build a TYPED globals block (lambda globals = 16 bytes) +
    // the GcGlobalRoots descriptor (env_ptr half at offset+8 of lambda globals).
    uint32_t gcur = 0;
    auto align8 = [](uint32_t v) -> uint32_t { return (v + 7u) & ~7u; };
    for (auto& g : pr.program.globals) {
        uint32_t sz = gft_host_value_bytes(g.ty.get());
        gcur = align8(gcur);
        m.gb.offsets[g.name] = gcur;
        m.gb.sizes[g.name] = sz;
        if (g.ty && g.ty->is_lambda) m.global_roots.offs.push_back(int32_t(gcur + 8));
        gcur += sz;
    }
    m.gb_store.assign(size_t(gcur), 0);
    m.gb.base = int64_t(m.gb_store.data());
    { uint32_t gi=0; for (auto& g : pr.program.globals) { m.gb.index[g.name]=gi++; m.gb.types[g.name]=g.ty.get(); } }
    m.global_roots.base = uint64_t(m.gb.base);
    GlobalInitCtx gic{m.gb_store, m.gb.index, m.gb.types};
    gic.offsets = &m.gb.offsets; gic.sizes = &m.gb.sizes;
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
    ctx.trap_stub=(void*)&gft_trap; ctx.trap_ctx=&m.ctx;
    ctx.budget_ptr=&m.ctx.budget_remaining; ctx.emit_budget_checks=true;
    ctx.depth_ptr=&m.ctx.call_depth; ctx.max_call_depth=256; ctx.emit_depth_checks=true;
    ctx.use_gc_env = true;  // GC heap env backend (lambdas outlive the frame)
    // Precise GC: address context_t::gc_frame_head via the baked pointer (the
    // harness calls main_entry directly without installing r14 -> baked mode).
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
        ctx.budget_ptr = nullptr;
        ctx.depth_ptr = nullptr;
    }
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); m.table.set(fn.slot,cf.entry); m.fns.push_back(std::move(cf));}
    auto sit=m.slots.find("main");
    if(sit==m.slots.end()) return false;
    m.main_entry = m.table.get(sit->second);
    return m.main_entry != nullptr;
}

static int64_t run_main(GftModule& m, bool* trapped) {
    *trapped = false;
    m.ctx.call_depth = 0; m.ctx.gc_frame_head = nullptr; m.ctx.has_checkpoint=true;
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

static void cleanup(GftModule& m) {
    for(auto&fn:m.fns)if(fn.exec)free_executable(fn.exec);
}

static int64_t run_one(const std::string& src, bool* ok) {
    *ok = true;
    ext_gc::gc_init();
    ext_gc::gc_reset();
    GftModule m;
    if (!compile_gft(src, m)) { *ok = false; cleanup(m); ext_gc::gc_reset(); return 0; }
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

// Compile+run one source through the OPTIMIZED IR backend (enable_ir_backend +
// enable_regalloc) with use_gc_env, returning the i64 main() result. Exercises
// the precise-GC frame-record maintenance + frame-map emit in the Thin IR path
// (thin_lower + thin_emit), which the default tree-walker run_one does NOT
// reach. A lambda-creating function falls back to the tree-walker (the IR
// backend does not yet lower LambdaExpr), so this is used with GC scripts that
// do NOT create lambdas (gc_new/gc_delete loops + nested calls + in-JIT
// collects) to cover the optimized path's GC correctness.
static int64_t run_one_ir(const std::string& src, bool* ok) {
    *ok = true;
    ext_gc::gc_init();
    ext_gc::gc_reset();
    GftModule m;
    if (!compile_gft(src, m, /*ir=*/true)) { *ok = false; cleanup(m); ext_gc::gc_reset(); return 0; }
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

// Compile+run one source with use_context_reg (r14) + ember_call_void, for
// scenarios that need the context register (try/catch + GC). The frame-record
// maintenance + try/catch emit read context_t via r14 (installed by
// ember_call_void). Returns the i64 main() result.
static int64_t run_one_ctx(const std::string& src, bool* ok) {
    *ok = true;
    ext_gc::gc_init();
    ext_gc::gc_reset();
    GftModule m;
    if (!compile_gft(src, m, /*ir=*/false, /*ctx_reg=*/true)) { *ok = false; cleanup(m); ext_gc::gc_reset(); return 0; }
    bool trapped = false;
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
        cleanup(m); ext_gc::gc_reset();
        return r;
    }
    *ok = !trapped;
    cleanup(m); ext_gc::gc_reset();
    return trapped ? int64_t(m.ctx.last_trap) : 0;
}

// ---- PART X: cross-layer GC integration (one collection sequence) ----
// Combines ALL required GC behavior in ONE coherent scenario on a single
// shared thread-local heap with one attached context, so the cross-layer
// interactions are exercised together. Each behavior is asserted
// INDEPENDENTLY with an explicit gc_is_live / freed-count check at its own
// collection checkpoint (a behavioral checksum alone cannot prove reclamation
// or sweeping — a leaked object is still on the heap). The five required
// behaviors:
//
//   (X1) an UNPINNED object reachable from a NESTED JIT frame survives a
//        collect issued from WITHIN that nested frame (the nested frame's
//        record is on the shadow stack; the inner env is rooted via the frame
//        map). Proven by the nested call returning the correct value AFTER
//        its in-JIT gc_collect (a swept env would crash / return garbage).
//   (X2) an object reachable ONLY from a TYPED GLOBAL survives a collect
//        (the global-root descriptor roots the env_ptr) AND is reclaimed after
//        the global is replaced. Proven by retaining env A's identity (read
//        from the typed global's env_ptr word) + gc_is_live checks: live
//        before replacement, NOT live after replacement + collect.
//   (X3) an object reachable ONLY through ARRAY / MAP extension storage
//        survives a collect (the extension trace callbacks report the slot /
//        entry) AND is reclaimed after the entry is removed / the extension is
//        reset. Proven by gc_is_live on the host-allocated objects.
//   (X4) a GENUINELY UNREACHABLE object (no frame root, no global root, no
//        extension root, no pin) is SWEPT by the collect. Proven by retaining
//        the handle in an i64 global (NOT a GC root) + gc_is_live: live before
//        the collect, NOT live after.
//   (X5) a RELEVANT POINTER STORE triggers the write-barrier observer through
//        the PRODUCTION barrier path: JIT-generated code captures a gc_new'd
//        (pinned) object by value into a lambda env — that capture store calls
//        __ember_gc_write_barrier (the same native generated code emits at
//        every GC-child-into-GC-object store), and the observer fires because
//        the env (owner) + the captured object (child) are both live GC
//        objects. This is NOT a bare facade call — it is a real owner->child
//        store through the codegen-emitted barrier call site.
//
// The tree-walker variant uses host-driven checkpoints (call individual script
// fns, check gc_is_live between calls) so X2/X4 reclamation + sweeping are
// independently enforced. The IR-backend variant (run_part_x_ir) exercises the
// same collector through the optimized Thin IR path (the IR backend does not
// yet lower LambdaExpr, so it uses the gc_new/gc_delete + nested-call surface).

// Host-driven script-call helpers: call a JIT'd fn (i64(i64) or i64()) with
// trap recovery while the context is already attached. Each call starts from a
// clean frame chain (gc_frame_head = nullptr) + call_depth 0, so sequential
// host-to-script calls compose correctly (each fn links/unlinks its own frame
// record). Returns the i64 result; sets *trapped on a runtime trap.
static int64_t call_fn_i64(GftModule& m, void* entry, int64_t arg, bool* trapped) {
    *trapped = false;
    m.ctx.call_depth = 0; m.ctx.gc_frame_head = nullptr; m.ctx.has_checkpoint = true;
    if (EMBER_SETJMP(m.ctx.checkpoint)) {
        *trapped = true; m.ctx.has_checkpoint = false; m.ctx.reset_for_call();
        return int64_t(m.ctx.last_trap);
    }
    using F = int64_t(*)(int64_t);
    int64_t r = reinterpret_cast<F>(entry)(arg);
    m.ctx.has_checkpoint = false;
    return r;
}
// Read an 8-byte word from a typed global in the globals block (the env_ptr
// half of a lambda global, or an i64 global's value). Used to retain GC object
// identities the host can check with gc_is_live across host-driven checkpoints.
static int64_t read_global_word(GftModule& m, const char* name, uint32_t word_off = 0) {
    auto it = m.gb.offsets.find(name);
    if (it == m.gb.offsets.end()) return 0;
    return *reinterpret_cast<int64_t*>(m.gb_store.data() + it->second + word_off);
}
static int run_part_x_tree() {
    std::printf("=== PART X (tree-walker): cross-layer one-collection-sequence ===\n");
    ext_gc::gc_init();
    ext_gc::gc_reset();
    ext_array::reset();
    ext_map::reset();

    // (X5) Register a barrier observer so a production-path GC-to-GC store is
    // observable. The observer records the (owner, child) pair + a count so the
    // test can assert the PRODUCTION barrier path fired (not just the facade).
    struct BarrierRec { void* owner; void* child; int count; };
    BarrierRec brec{ nullptr, nullptr, 0 };
    ember::gc::GcBarrierToken botok =
        ember::ext_gc::gc_register_barrier_observer(&brec,
            [](void* ud, void* o, void* c) {
                auto* r = static_cast<BarrierRec*>(ud);
                r->owner = o; r->child = c; r->count++;
            });
    check(botok != 0, "X5a: barrier observer registered (token non-zero)");

    // (X3) Set up an array + a map holding UNPINNED GC objects (extension
    // storage roots). These share the same thread-local heap as the JIT code.
    std::unordered_map<std::string, ember::NativeSig> natives;
    ext_array::register_natives(natives);
    ext_map::register_natives(natives);
    auto grab = [&](const char* name) -> void* {
        auto it = natives.find(name);
        if (it == natives.end()) { check(false, "X3: native missing"); return nullptr; }
        return it->second.fn_ptr;
    };
    using F_arr_new = int64_t(*)(int64_t, int64_t);
    using F_arr_set = void(*)(int64_t, int64_t, int64_t);
    using F_arr_get = int64_t(*)(int64_t, int64_t);
    using F_arr_clr = void(*)(int64_t);
    using F_map_new = int64_t(*)();
    using F_map_set = void(*)(int64_t, int64_t, int64_t);
    using F_map_get = int64_t(*)(int64_t, int64_t);
    using F_map_rem = void(*)(int64_t, int64_t);
    auto p_arr_new = reinterpret_cast<F_arr_new>(grab("array_new"));
    auto p_arr_set = reinterpret_cast<F_arr_set>(grab("array_set_i64"));
    auto p_arr_get = reinterpret_cast<F_arr_get>(grab("array_get_i64"));
    auto p_arr_clr = reinterpret_cast<F_arr_clr>(grab("array_clear"));
    auto p_map_new = reinterpret_cast<F_map_new>(grab("map_new"));
    auto p_map_set = reinterpret_cast<F_map_set>(grab("map_set"));
    auto p_map_get = reinterpret_cast<F_map_get>(grab("map_get"));
    auto p_map_rem = reinterpret_cast<F_map_rem>(grab("map_remove"));
    if (g_fail) { ext_array::reset(); ext_map::reset(); ext_gc::gc_reset(); return g_fail; }

    int64_t arr_h = p_arr_new(8, 4);  // array<i64>, 4 slots
    int64_t map_h = p_map_new();
    check(arr_h >= 1 && map_h >= 1, "X3a: array + map handles allocated");
    int64_t arr_obj = ember::ext_gc::gc_alloc_env(16);  // unpinned; rooted via array
    int64_t map_obj = ember::ext_gc::gc_alloc_env(16);  // unpinned; rooted via map
    check(arr_obj != 0 && map_obj != 0, "X3b: array/map GC objects allocated (unpinned)");
    p_arr_set(arr_h, 0, arr_obj);   // stores + registers array trace cb + barrier
    p_map_set(map_h, 7, map_obj);   // stores + registers map trace cb + barrier
    check(p_arr_get(arr_h, 0) == arr_obj, "X3c: array reads back the stored ptr");
    check(p_map_get(map_h, 7) == map_obj, "X3d: map reads back the stored value ptr");

    // (X1)+(X2)+(X4)+(X5) The JIT side. The script exposes individual fns the
    // host calls at its own checkpoints so X2 reclamation + X4 sweeping are
    // INDEPENDENTLY enforced with gc_is_live (a checksum alone cannot prove a
    // freed object was swept — it would still pass if the object leaked).
    //
    //   g_store : a typed global holding a capturing lambda (X2). Its env_ptr
    //             is rooted via the global-root descriptor (offset+8).
    //   g_drop  : an i64 global holding a gc_new'd handle (X4). It is NOT a
    //             GC root (i64 globals are not in the global-root descriptor),
    //             so it does NOT keep the object alive — the host reads it to
    //             check gc_is_live after the collect.
    //   mk / callg / worker / inner : the X1 (nested frame) + X2 (global) fns.
    //   make_drop : allocs + unpins the X4 target, saving its handle in g_drop.
    //   make_edge : X5 — captures a gc_new'd (pinned) object by value into a
    //               lambda env. The codegen-emitted capture store calls
    //               __ember_gc_write_barrier(env, child) — the PRODUCTION
    //               barrier path — and the observer fires (both live).
    const char* src =
        "global g_store : fn(i64) -> i64 = fn(x: i64) -> i64 { return x; };\n"
        "global g_drop : i64 = 0;\n"
        "fn mk(c: i64) -> i64 { let f = fn(x: i64) -> i64 { return x + c; }; g_store = f; return 0; }\n"
        "fn callg(v: i64) -> i64 { let h = g_store; return h(v); }\n"
        "fn inner(c: i64) -> i64 { let f = fn(x: i64) -> i64 { return x + c; }; gc_collect(); return f(c); }\n"
        "fn worker(c: i64) -> i64 { return inner(c); }\n"
        "fn make_drop() -> i64 { g_drop = gc_new(16); gc_delete(g_drop); return 0; }\n"
        "fn make_edge() -> i64 { let p = gc_new(16); let f = fn[p]() -> i64 { return p; }; return gc_live(); }\n"
        "fn main() -> i64 { return 0; }\n";
    GftModule m;
    bool compiled = compile_gft(src, m);
    check(compiled, "X1/X2/X4/X5: cross-layer script compiled");
    if (!compiled) {
        check(ember::ext_gc::gc_unregister_barrier_observer(botok),
              "X5: barrier observer unregistered (compile-fail cleanup)");
        ext_array::reset(); ext_map::reset(); ext_gc::gc_reset();
        return g_fail;
    }
    void* entry_mk        = m.table.get(m.slots["mk"]);
    void* entry_callg     = m.table.get(m.slots["callg"]);
    void* entry_worker    = m.table.get(m.slots["worker"]);
    void* entry_make_drop = m.table.get(m.slots["make_drop"]);
    void* entry_make_edge = m.table.get(m.slots["make_edge"]);

    // Attach the context (with the typed-global root descriptor) ONCE for the
    // whole checkpoint sequence. g_store's env_ptr word is a root; g_drop is
    // i64 so it is NOT a root (the handle does not keep the object alive).
    m.ctx.call_depth = 0; m.ctx.gc_frame_head = nullptr; m.ctx.has_checkpoint = false;
    ext_gc::gc_attach_context(&m.ctx, m.global_roots.empty() ? nullptr : &m.global_roots);

    bool trapped = false;
    int64_t r = 0;

    // --- (X2) env A installed in the typed global; retain its identity. ---
    r = call_fn_i64(m, entry_mk, 100, &trapped);
    check(!trapped, "X2a: mk(100) ran (no trap)");
    int64_t envA = read_global_word(m, "g_store", 8);  // env_ptr half of the lambda
    check(envA != 0, "X2b: g_store holds a non-null env_ptr after mk(100)");
    check(ember::ext_gc::gc_is_live(envA),
          "X2c: env A live after install (rooted via the typed-global descriptor)");

    // A host-side collect must NOT sweep env A (it is rooted via g_store).
    ember::ext_gc::gc_collect();
    check(ember::ext_gc::gc_is_live(envA),
          "X2d: env A survives a host-side collect (global root holds it)");

    // --- (X1) an unpinned env reachable from a NESTED JIT frame survives an
    //         in-JIT collect issued from within that nested frame. ---
    // worker -> inner: inner creates a lambda env (unpinned, rooted only via
    // inner's frame record), calls gc_collect() WHILE inner's frame is live,
    // then calls f(c). If the env were swept by that in-JIT collect, f(c)
    // would dereference freed memory (crash/trap/garbage). Returning 10 (the
    // correct value, no trap) proves the nested-frame env survived.
    r = call_fn_i64(m, entry_worker, 5, &trapped);
    check(!trapped, "X1a: worker(5) ran (no trap)");
    check(r == 10, "X1b: nested-frame env survived the in-JIT collect (f(5)==10)");

    // --- (X4) a genuinely unreachable object is swept. ---
    // make_drop allocs + unpins (gc_delete) a gc_new'd object, saving its
    // handle in the i64 global g_drop (NOT a GC root). Before the collect the
    // object is still on the heap (unpinned but not yet collected); after the
    // collect it MUST be gone — proving an object with NO root is swept.
    r = call_fn_i64(m, entry_make_drop, 0, &trapped);
    check(!trapped, "X4a: make_drop ran (no trap)");
    int64_t drop = read_global_word(m, "g_drop");
    check(drop != 0, "X4b: g_drop holds the unreachable object's handle");
    check(ember::ext_gc::gc_is_live(drop),
          "X4c: drop object still on heap before collect (unpinned, not yet swept)");
    int64_t freed_before = ember::ext_gc::gc_freed_count();
    ember::ext_gc::gc_collect();   // THE X4 collection checkpoint
    int64_t freed_delta = ember::ext_gc::gc_freed_count() - freed_before;
    check(freed_delta >= 1, "X4d: collect freed >= 1 object (the unreachable drop)");
    check(!ember::ext_gc::gc_is_live(drop),
          "X4e: drop object SWEPT by collect (genuinely unreachable -> gone)");
    // env A must STILL be live (the X4 collect must not sweep a global root).
    check(ember::ext_gc::gc_is_live(envA),
          "X4f: env A still live after the X4 collect (global root unaffected)");

    // --- (X2 reclamation) replace the global -> env A's only root removed. ---
    r = call_fn_i64(m, entry_mk, 200, &trapped);
    check(!trapped, "X2g: mk(200) ran (replaces g_store, no trap)");
    int64_t envB = read_global_word(m, "g_store", 8);
    check(envB != 0 && envB != envA,
          "X2h: g_store now holds a different env_ptr (env B) after mk(200)");
    check(ember::ext_gc::gc_is_live(envB), "X2i: env B live after install");
    // env A may still be on the heap right after the replacement (no collect
    // yet) — it is unreachable but not yet swept. The collect MUST sweep it.
    freed_before = ember::ext_gc::gc_freed_count();
    ember::ext_gc::gc_collect();   // THE X2-reclamation collection checkpoint
    freed_delta = ember::ext_gc::gc_freed_count() - freed_before;
    check(freed_delta >= 1, "X2j: collect freed >= 1 object (env A reclaimed)");
    check(!ember::ext_gc::gc_is_live(envA),
          "X2k: env A SWEPT after global replacement (only root removed -> gone)");
    check(ember::ext_gc::gc_is_live(envB),
          "X2l: env B survives the reclamation collect (current global root)");
    // callg still works via env B.
    r = call_fn_i64(m, entry_callg, 1, &trapped);
    check(!trapped, "X2m: callg(1) ran via env B (no trap)");
    check(r == 201, "X2n: callg(1)==201 via env B (replaced global is usable)");

    // --- (X5) a production-path owner->child store triggers the barrier. ---
    // make_edge allocates a pinned GC object p (gc_new), then captures p BY
    // VALUE into a lambda env. The codegen-emitted capture store writes p into
    // the env + calls __ember_gc_write_barrier(env, p) — the SAME native
    // generated code emits at every GC-child-into-GC-object store. Because the
    // env (owner) + p (child) are both live GC objects, the barrier fires the
    // observer. This is a real store through the production barrier call site,
    // not a bare facade call.
    int64_t edge_barrier_before = ember::ext_gc::gc_barrier_count();
    int obs_before = brec.count;
    r = call_fn_i64(m, entry_make_edge, 0, &trapped);
    check(!trapped, "X5b: make_edge ran (captured a gc_new'd object, no trap)");
    check(r >= 1, "X5c: make_edge returned gc_live()>=1 (the captured object is live)");
    check(brec.count > obs_before,
          "X5d: barrier observer FIRED during the JIT capture store (production path)");
    check(brec.owner != nullptr && brec.child != nullptr,
          "X5e: observer saw a non-null (owner, child) pair");
    // The owner (the env) + child (the captured object) were both live GC
    // objects at the barrier call — the production path's precondition.
    check(ember::ext_gc::gc_is_live(reinterpret_cast<int64_t>(brec.child)),
          "X5f: the captured child was a live GC object (barrier not a no-op)");
    check(ember::ext_gc::gc_barrier_count() > edge_barrier_before,
          "X5g: barrier_calls incremented by the production-path capture store");

    // Detach the context (unregister the frame-chain + global-roots callback).
    ext_gc::gc_detach_context(&m.ctx);

    // (X3) After the checkpoint sequence: the array/map objects SURVIVED every
    // collect (rooted via the extension trace callbacks).
    check(ember::ext_gc::gc_is_live(arr_obj),
          "X3e: array-rooted obj survived the collects (extension trace cb)");
    check(ember::ext_gc::gc_is_live(map_obj),
          "X3f: map-rooted obj survived the collects (extension trace cb)");

    // (X3 reclamation) Remove the array entry + the map entry -> collect reclaims.
    p_arr_clr(arr_h);               // array no longer reports arr_obj
    p_map_rem(map_h, 7);            // map no longer reports map_obj
    ember::ext_gc::gc_collect();    // the reclamation collect
    check(!ember::ext_gc::gc_is_live(arr_obj),
          "X3g: array-rooted obj reclaimed after array_clear + collect");
    check(!ember::ext_gc::gc_is_live(map_obj),
          "X3h: map-rooted obj reclaimed after map_remove + collect");

    // (X3 reclamation via reset) A fresh array obj is reclaimed when the
    // extension is reset (the trace cb is unregistered -> no root).
    int64_t arr_obj2 = ember::ext_gc::gc_alloc_env(16);
    p_arr_set(arr_h, 0, arr_obj2);
    check(ember::ext_gc::gc_is_live(arr_obj2), "X3i: fresh array obj rooted before reset");
    ext_array::reset();             // unregisters the array trace cb + clears the store
    ember::ext_gc::gc_collect();
    check(!ember::ext_gc::gc_is_live(arr_obj2),
          "X3j: array-rooted obj reclaimed after ext_array::reset + collect");

    // Free the JIT'd executable memory (the module's compiled fns).
    cleanup(m);

    // Cleanup: unregister the barrier observer + reset the stores.
    check(ember::ext_gc::gc_unregister_barrier_observer(botok),
          "X5h: barrier observer unregistered");
    ext_map::reset();
    ext_array::reset();
    ext_gc::gc_reset();
    return g_fail;
}

// (X, IR backend) The optimized-path variant: the IR backend does not yet
// lower LambdaExpr, so this exercises the gc_new/gc_delete + nested-call +
// in-JIT-collect surface through the Thin IR path (frame-record maintenance +
// frame-map emit). The array/map extension roots + the barrier edge are set up
// on the same heap BEFORE the run so the script's in-JIT gc_collect() is the
// one collection sequence that witnesses the extension-rooted objects survive
// + the unreachable object is swept, on the optimized backend.
static int run_part_x_ir() {
    std::printf("=== PART X (IR backend): cross-layer one-collection-sequence ===\n");
    ext_gc::gc_init();
    ext_gc::gc_reset();
    ext_array::reset();
    ext_map::reset();

    // (X3) array/map extension roots on the shared heap.
    std::unordered_map<std::string, ember::NativeSig> natives;
    ext_array::register_natives(natives);
    ext_map::register_natives(natives);
    auto grab = [&](const char* name) -> void* {
        auto it = natives.find(name);
        if (it == natives.end()) { check(false, "X-IR: native missing"); return nullptr; }
        return it->second.fn_ptr;
    };
    using F_arr_new = int64_t(*)(int64_t, int64_t);
    using F_arr_set = void(*)(int64_t, int64_t, int64_t);
    using F_arr_clr = void(*)(int64_t);
    using F_map_new = int64_t(*)();
    using F_map_set = void(*)(int64_t, int64_t, int64_t);
    using F_map_rem = void(*)(int64_t, int64_t);
    auto p_arr_new = reinterpret_cast<F_arr_new>(grab("array_new"));
    auto p_arr_set = reinterpret_cast<F_arr_set>(grab("array_set_i64"));
    auto p_arr_clr = reinterpret_cast<F_arr_clr>(grab("array_clear"));
    auto p_map_new = reinterpret_cast<F_map_new>(grab("map_new"));
    auto p_map_set = reinterpret_cast<F_map_set>(grab("map_set"));
    auto p_map_rem = reinterpret_cast<F_map_rem>(grab("map_remove"));
    if (g_fail) { ext_array::reset(); ext_map::reset(); ext_gc::gc_reset(); return g_fail; }

    int64_t arr_h = p_arr_new(8, 4);
    int64_t map_h = p_map_new();
    int64_t arr_obj = ember::ext_gc::gc_alloc_env(16);
    int64_t map_obj = ember::ext_gc::gc_alloc_env(16);
    p_arr_set(arr_h, 1, arr_obj);
    p_map_set(map_h, 3, map_obj);
    check(arr_obj != 0 && map_obj != 0, "X-IR: array/map GC objects allocated (unpinned)");

    // (X1-IR/X4-IR) Nested IR-backend calls + an in-JIT collect + an
    // unreachable object, through the optimized path. worker allocs+deletes
    // (unreachable), collects inside JIT, returns gc_live(); main calls two
    // workers + collects -> the array/map objects survive (extension roots),
    // the unreachable per-iteration objects are swept.
    //
    // Compile + run DIRECTLY (not via run_one_ir, which calls gc_reset() at
    // the end + would clear the shared heap, freeing the host-allocated
    // array/map objects before we can verify they survived).
    //
    // The in-JIT gc_collect() inside worker does NOT sweep arr_obj/map_obj
    // (they are rooted via the array/map extension trace callbacks on the
    // shared heap), so worker's gc_live() == 2 (the two extension-rooted
    // objects), NOT 0. This is the cross-layer proof: the IR backend's
    // in-JIT collect sees the host extension roots. main's two workers each
    // return 2 -> a + b == 4.
    const char* src =
        "fn worker(n: i64) -> i64 {\n"
        "    let mut i: i64 = 0;\n"
        "    while (i < n) { let p = gc_new(16); gc_delete(p); i = i + 1; }\n"
        "    gc_collect();\n"          // in-JIT collect on the IR backend
        "    return gc_live();\n"       // == 2 (arr_obj + map_obj survive via extension roots)
        "}\n"
        "fn main() -> i64 {\n"
        "    let a = worker(50);\n"     // X1-IR: nested IR frame + in-JIT collect -> 2
        "    let b = worker(60);\n"     // -> 2
        "    gc_collect();\n"           // X4-IR: unreachable per-iter objects swept
        "    return a + b;\n"           // 2 + 2 = 4
        "}\n";
    GftModule m;
    bool compiled = compile_gft(src, m, /*ir=*/true);
    check(compiled, "X-IR1a: IR-backend cross-layer script compiled");
    int64_t r = 0;
    if (compiled) {
        bool trapped = false;
        r = run_main(m, &trapped);
        check(!trapped, "X-IR1b: IR-backend nested calls + in-JIT collect ran (no trap)");
        if (trapped) { std::printf("  FAIL: runtime trap (IR): %s\n", m.ctx.last_error.c_str()); }
    }
    // gc_live()==2 per worker (the extension-rooted arr_obj + map_obj survive
    // the in-JIT collect; the unreachable per-iteration gc_new/gc_delete
    // objects are swept). a + b == 4.
    check(compiled && r==4,
          "X-IR2: workers + collect -> 4 (2 extension roots survive per worker; IR frame-record safe)");

    // (X3) The extension-rooted objects survived the IR-backend's in-JIT collects.
    check(ember::ext_gc::gc_is_live(arr_obj),
          "X-IR3: array-rooted obj survived IR-backend in-JIT collects");
    check(ember::ext_gc::gc_is_live(map_obj),
          "X-IR4: map-rooted obj survived IR-backend in-JIT collects");

    // (X3 reclamation) remove + collect -> reclaimed.
    p_arr_clr(arr_h);
    p_map_rem(map_h, 3);
    ember::ext_gc::gc_collect();
    check(!ember::ext_gc::gc_is_live(arr_obj), "X-IR5: array obj reclaimed after clear + collect");
    check(!ember::ext_gc::gc_is_live(map_obj), "X-IR6: map obj reclaimed after remove + collect");

    cleanup(m);
    ext_map::reset();
    ext_array::reset();
    ext_gc::gc_reset();
    return g_fail;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // === (a) By-reference capture ===
    std::printf("=== (a) by-reference capture ===\n");
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
        check(ok,   "(a1) by-ref read: compiled + ran (GC env)");
        check(r==99,"(a2) by-ref read: lambda sees post-capture mutation (99)");
    }
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
        check(ok,   "(a3) by-ref write-through + mixed capture: compiled + ran");
        check(r==50,"(a4) by-ref write-through mutates original (50)");
    }
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
        check(ok,   "(a5) nested by-ref (transitive): compiled + ran");
        check(r==42,"(a6) nested by-ref sees post-capture mutation (42)");
    }

    // === (b) new/delete: GC collects unreachable, heap stays bounded ===
    std::printf("=== (b) new/delete: GC collects unreachable ===\n");
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
        check(ok,  "(b1) 5000 new/delete: compiled + ran (auto-collect mid-run)");
        check(r==0,"(b2) heap bounded; after collect gc_live()==0 (all unreachable reaped)");
    }

    // === (c) No leaks: reachable (pinned) objects survive collection ===
    std::printf("=== (c) no leaks: reachable survive collection ===\n");
    {
        bool ok;
        const char* src =
            "fn main() -> i64 {\n"
            "    let mut kept: i64 = 0;\n"
            "    let mut i: i64 = 0;\n"
            "    while (i < 1000) {\n"
            "        let p = gc_new(16);\n"
            "        if (i < 7) { kept = kept + 1; }\n"
            "        else { gc_delete(p); }\n"
            "        i = i + 1;\n"
            "    }\n"
            "    gc_collect();\n"
            "    return gc_live();\n"
            "}\n";
        int64_t r = run_one(src, &ok);
        check(ok,  "(c1) keep-7-delete-rest: compiled + ran");
        check(r==7,"(c2) reachable (pinned) survive collect; gc_live()==7 (no leaks)");
    }

    // === (X) cross-layer one-collection-sequence (tree-walker + IR backend) ===
    int x_tree = run_part_x_tree();
    int x_ir   = run_part_x_ir();

    std::printf("\ngc_full_test: %s\n", (g_fail || x_tree || x_ir) ? "FAIL" : "PASS");
    return (g_fail || x_tree || x_ir) ? 1 : 0;
}
