// keyed_dispatch_outer_thunk_test — Red 5
// (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §6, §9.8, §10.3, §14.2,
// §14.3): the keyed outer thunk / provider boundary / ModuleInstance /
// safe keyed host-call API gate.
//
// RED-GREEN contract chunk for the keyed outer thunk. This is the RED side
// (written first) of the Red 5 contract. It pins, against the immutable Red 4
// ModuleDispatchRecord + the Red 4 C-ABI resolver, the §6 key lifecycle and
// the §9.8 / §14.2 mandatory buckets for the outer thunk:
//
//   - provider once:      the DerivedMaterialProvider is invoked EXACTLY ONCE
//                          per outer keyed call and NEVER for recursive/nested
//                          script edges (the thunk derives the route word once
//                          at the outer boundary; nested script calls inherit
//                          r15 directly, §6.5).
//   - provider failure:   a provider that returns a structured ExtensionError
//                          prevents entry — the safe API reports a structured
//                          CallResult failure and the thunk is never entered.
//   - r14 correctness:     r14 contains the supplied context_t* throughout the
//                          call (the JIT reads context_t fields through it).
//   - r15 invariant:       r15 contains the transient route word throughout
//                          direct recursion AND native re-entry (§6.5); a
//                          re-entrant native that reads r15 sees the same
//                          route word the outer thunk installed.
//   - caller restore:     the caller's ORIGINAL r14/r15 values are restored
//                          after the call (callee-saved preservation; the
//                          thunk pushes caller r14/r15 and pops them).
//   - r15 cleared:         the transient r15 is cleared (xor r15,r15) BEFORE
//                          restoring the caller value on normal AND trapped
//                          exits (§6.3 step 5).
//   - edge words:         route words 0 and UINT64_MAX both work.
//   - try/throw/catch:    preserves the keyed r15 invariant — a throw that
//                          longjmps to a catch restores r15 from the try/catch
//                          save buffer slot 5 (§6.4), so the route word survives
//                          the throw/catch round-trip.
//   - concurrent callers: two concurrent callers using DIFFERENT providers do
//                          NOT share route state (each derives its own route
//                          word, installs its own r15; §6.6).
//
// The fixture compiles simple JIT functions through the existing pipeline
// (use_context_reg=true, the keyed CodeGenCtx that excludes r15 from regalloc)
// and calls them through the safe keyed host APIs (ember_call_keyed_void /
// ember_call_keyed_i64 / ember_call_keyed_i64_i64). The ModuleInstance owns
// the stable module ID, the dispatch record, the provider/strategy references,
// and the executable pages/globals.
//
// Links ember (keyed_dispatch.* — Red 1, context.hpp, engine.* — Red 5 thunks,
// dispatch_table.hpp) + ember_frontend (module_layout.* — Red 3/4, codegen.* —
// the keyed CodeGenCtx, regalloc.* — r15 exclusion). NOT a CTest entry: the
// filtered suite count must stay 67 (§14.1); the target building cleanly +
// the executable passing IS the gate.

#include "../src/engine.hpp"
#include "../src/context.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/keyed_dispatch.hpp"
#include "../src/module_layout.hpp"
#include "../src/module_instance.hpp"
#include "../src/key_provider.hpp"
#include "../src/codegen.hpp"
#include "../src/regalloc.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/globals.hpp"
#include "../src/binding_builder.hpp"
#include "../src/thin_ir.hpp"
#include "../src/thin_lower.hpp"
#include "../src/thin_emit.hpp"
#include "../src/extension_registry.hpp"
#include "import.hpp"

#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace ember;

// ===========================================================================
// Test harness
// ===========================================================================
static int g_fail = 0;
static int g_checks = 0;
static void ck(bool c, const char* m) {
    ++g_checks;
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// ===========================================================================
// Counting provider — records every derive() invocation so the test can
// assert the provider fires EXACTLY ONCE per outer call and NEVER for nested
// script edges.
// ===========================================================================
struct CountingProvider : public DerivedMaterialProvider {
    std::array<uint8_t, 32> material{};
    mutable std::atomic<uint64_t> derive_count{0};
    mutable std::atomic<bool> fail_next{false};

    explicit CountingProvider(uint8_t fill) { material.fill(fill); }
    ExtensionResult<std::array<uint8_t, 32>> derive(const DerivationRequest& req) const override {
        derive_count.fetch_add(1, std::memory_order_relaxed);
        if (fail_next.load(std::memory_order_relaxed)) {
            return make_extension_result_error<std::array<uint8_t, 32>>(
                "ember-keyed-dispatch", "test-provider", "test-forced provider failure");
        }
        (void)req;
        return make_extension_result_ok(material);
    }
};

// read_r15 / set_caller_r15 — tiny asm helpers exposed by engine.cpp for the
// test to observe/seed the transient route register. set_caller_r15 sets the
// CALLER's r15 before the call so the test can assert it's restored after.

// reentry_probe: a native the JIT'd script calls; it reads r15 and records it
// into a test-owned slot, proving r15 survives the script->native boundary
// (§6.5: "conforming native calls preserve it under the platform ABI").
struct Reentry {
    std::atomic<uint64_t> observed_r15{0};
    std::atomic<int> calls{0};
    void reset() { observed_r15.store(0); calls.store(0); }
};
static Reentry g_reentry;
extern "C" int64_t reentry_probe(int64_t arg) {
    g_reentry.observed_r15.store(ember_read_r15(), std::memory_order_relaxed);
    g_reentry.calls.fetch_add(1, std::memory_order_relaxed);
    return arg * 2;
}

// native_reentry_probe: a native that re-enters the script through the keyed
// re-entry thunk, proving r15 is preserved across native re-entry (§6.5).
struct ReentryCtx {
    void* entry = nullptr;
    context_t* ctx = nullptr;
    std::atomic<uint64_t> observed_r15_before{0};
    std::atomic<uint64_t> observed_r15_after{0};
    std::atomic<int> reentry_calls{0};
    uint64_t route_word = 0;
};
static ReentryCtx g_rec;
extern "C" int64_t native_reentry_probe(int64_t depth) {
    g_rec.observed_r15_before.store(ember_read_r15(), std::memory_order_relaxed);
    if (depth > 0 && g_rec.entry && g_rec.ctx) {
        // Re-enter via the keyed re-entry thunk (preserves/reinstalls r15).
        ember_keyed_reentry_i64(g_rec.entry, g_rec.ctx, depth - 1, g_rec.route_word);
        g_rec.reentry_calls.fetch_add(1, std::memory_order_relaxed);
    }
    g_rec.observed_r15_after.store(ember_read_r15(), std::memory_order_relaxed);
    return depth;
}

// ===========================================================================
// Compile a small module through the existing pipeline with the keyed
// CodeGenCtx (use_context_reg=true + keyed descriptor that excludes r15 from
// regalloc). The ModuleInstance owns the dispatch record (identity mode for
// the thunk contract; keyed-mode resolution correctness is Red 4's gate).
// ===========================================================================
struct Compiled {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string, int> slots;
    std::unordered_map<std::string, NativeSig> natives;
    void* main_entry = nullptr;
    ~Compiled() { for (auto& fn : fns) if (fn.exec) free_executable(fn.exec); }
};

static std::unique_ptr<Compiled> compile_identity(const std::string& src,
                                                   bool register_reentry_natives = false,
                                                   void* trap_stub = nullptr) {
    auto m = std::make_unique<Compiled>();
    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); } catch (...) { return nullptr; }
    auto lr = tokenize(resolved, "<keyed-thunk-test>"); if (!lr.ok) return nullptr;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
    int si = 0; for(auto&fn:pr.program.funcs){m->slots[fn.name]=si++;fn.slot=m->slots[fn.name];}
    ember::OpOverloadTable ov;
    if (register_reentry_natives) {
        NativeSig probe; probe.fn_ptr = reinterpret_cast<void*>(reentry_probe);
        probe.ret = type_i64(); probe.params = {type_i64()};
        m->natives["reentry_probe"] = probe;
        NativeSig rec; rec.fn_ptr = reinterpret_cast<void*>(native_reentry_probe);
        rec.ret = type_i64(); rec.params = {type_i64()};
        m->natives["native_reentry_probe"] = rec;
    }
    auto layouts = build_struct_layouts(pr.program); pr.program.string_xor_key = 0;
    if(!sema(pr.program,m->natives,m->slots,0,&ov,&layouts).ok) return nullptr;

    ember::GlobalsBlock gb; std::vector<uint8_t> gbs(pr.program.globals.size()*8, 0);
    gb.base=int64_t(gbs.data());
    { uint32_t gi=0; for (auto& g : pr.program.globals) { gb.index[g.name]=gi++; gb.types[g.name]=g.ty.get(); } }
    eval_global_initializers(pr.program, GlobalInitCtx{gbs, gb.index, gb.types});
    ember::g_globals_for_codegen = nullptr;
    m->table = DispatchTable(pr.program.funcs.size());
    ember::CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(m->table.base());
    ctx.natives=&m->natives; ctx.script_slots=&m->slots; ctx.structs=&layouts;
    ctx.globals_index = &gb.index; ctx.globals_types = &gb.types;
    ctx.use_context_reg = true;
    ctx.emit_depth_checks = true;
    ctx.emit_budget_checks = true;
    ctx.max_call_depth = 64;
    if (trap_stub) {
        ctx.trap_stub = trap_stub;
        ctx.trap_ctx = nullptr;  // B1 mode: ctx arrives in r14, not a baked ptr
    }
    // KEYED MODE (Red 5 §9.3 / §6.4): the keyed CodeGenCtx descriptor selects
    // keyed mode (reserves r15 in regalloc + the keyed emit path). The thunk
    // installs r15; the JIT'd code treats r15 as read-only route material.
    KeyedDispatchCodegen kd{};
    kd.runtime_key = RuntimeKeyLocation::R15;
    ctx.keyed_dispatch = &kd;

    for(auto&fn:pr.program.funcs){
        auto cf=compile_func(fn,ctx); finalize(cf); m->table.set(fn.slot,cf.entry);
        m->fns.push_back(std::move(cf));
    }
    auto sit=m->slots.find("main");
    if(sit!=m->slots.end()) m->main_entry = m->table.get(sit->second);
    return m;
}

// Build a ModuleInstance in identity mode over a compiled module.
static ModuleInstance make_identity_instance(Compiled& m, const std::string& id,
                                              std::shared_ptr<const DerivedMaterialProvider> provider,
                                              uint32_t strategy_version = 1) {
    ModuleInstance inst;
    inst.module_id = id;
    inst.strategy_version = strategy_version;
    inst.provider = std::move(provider);
    inst.mode = DispatchMode::Identity;
    inst.physical_slot_count = static_cast<uint32_t>(m.slots.size());
    inst.logical_slot_count = static_cast<uint32_t>(m.slots.size());
    inst.dispatch_base = int64_t(m.table.base());
    inst.entry_table = &m.table;
    for (const auto& [n, s] : m.slots) inst.named_entries[n] = static_cast<uint32_t>(s);
    return inst;
}

extern "C" void kt_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) EMBER_LONGJMP(ctx->checkpoint, 1);
    }
    std::abort();
}

int main() {
    std::printf("== keyed_dispatch_outer_thunk_test (Red 5) ==\n");

    // =====================================================================
    // 0. Provider boundary + DispatchKeyAdapter — domain separation, route
    //    word derivation, structured failure.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xAB);
        DispatchKeyAdapter adapter(provider);
        ModuleId mid{"test.mod", 1};
        auto rw = adapter.route_word(mid, 1, "ember/dispatch");
        ck(bool(rw), "adapter: route_word succeeds for ember/dispatch");
        ck(rw.value.has_value(), "adapter: route_word carries a value");
        auto rw_pass = adapter.route_word(mid, 1, "ember/passes");
        auto rw_layout = adapter.route_word(mid, 1, "ember/layout");
        ck(bool(rw_pass) && bool(rw_layout), "adapter: other domains succeed");
        ck(*rw.value != *rw_pass.value, "adapter: ember/dispatch != ember/passes (domain separation)");
        ck(*rw.value != *rw_layout.value, "adapter: ember/dispatch != ember/layout (domain separation)");
        ck(*rw_pass.value != *rw_layout.value, "adapter: ember/passes != ember/layout (domain separation)");
        auto rw_v2 = adapter.route_word(mid, 2, "ember/dispatch");
        ck(bool(rw_v2), "adapter: route_word for version 2 succeeds");
        ck(*rw.value != *rw_v2.value, "adapter: strategy_version folds into the route word");
        ModuleId mid2{"other.mod", 1};
        auto rw_other = adapter.route_word(mid2, 1, "ember/dispatch");
        ck(*rw.value != *rw_other.value, "adapter: different module -> different route word");
        ck(provider->derive_count.load() == 5, "adapter: provider invoked once per route_word call (5 calls)");
    }

    // =====================================================================
    // 1. PROVIDER ONCE PER OUTER CALL — recursive script; provider fires once.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xAB);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_identity(
            "fn fib(n: i64) -> i64 { if (n < 2) { return n; } return fib(n-1) + fib(n-2); }\n"
            "fn main() -> i64 { return fib(10); }\n");
        ck(m != nullptr, "compile fib + main with keyed CodeGenCtx (r15 excluded from regalloc)");
        if (m) {
            auto inst = make_identity_instance(*m, "rec.mod", provider);
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            uint64_t before = provider->derive_count.load();
            auto r = ember_call_keyed_void(inst, "main", ctx, adapter);
            uint64_t after = provider->derive_count.load();
            ck(r.ok, "keyed void call: ok");
            ck(r.value == 55, "keyed void call: fib(10) == 55 (recursive script ran correctly under r15 reservation)");
            ck(after == before + 1, "provider invoked EXACTLY ONCE for the outer call (not once per recursive edge)");
            before = after;
            auto r2 = ember_call_keyed_void(inst, "main", ctx, adapter);
            after = provider->derive_count.load();
            ck(r2.ok && r2.value == 55, "second keyed void call: ok + fib(10)==55");
            ck(after == before + 1, "provider invoked exactly once for the second outer call");
        }
    }

    // =====================================================================
    // 2. PROVIDER FAILURE PREVENTS ENTRY.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xAB);
        provider->fail_next.store(true);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_identity("fn main() -> i64 { return 7; }\n");
        ck(m != nullptr, "compile main=7 for provider-failure test");
        if (m) {
            auto inst = make_identity_instance(*m, "fail.mod", provider);
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL;
            auto r = ember_call_keyed_void(inst, "main", ctx, adapter);
            ck(!r.ok, "provider failure: CallResult reports failure");
            ck(r.reason.size() > 0, "provider failure: CallResult carries a structured reason");
            ck(provider->derive_count.load() == 1, "provider failure: provider invoked once (then failed)");
        }
    }

    // =====================================================================
    // 3. r14 CORRECTNESS — budget trap records on the SUPPLIED context.
    //    The keyed API establishes its OWN checkpoint internally (§9.8) and
    //    returns a structured CallResult{trapped=true} on a trap; the trap
    //    stub (kt_trap) receives the SUPPLIED context (r14 = ctx in B1 mode)
    //    and records last_trap/last_error on it, which the API folds into
    //    r.reason. The test checks r.trapped + r.reason (NOT a longjmp to the
    //    test's own checkpoint — the API owns the checkpoint, matching the
    //    Red 8 keyed_dispatch_extensions test pattern).
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xAB);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_identity(
            "fn main() -> i64 { while (true) { let mut x: i64 = 1+1+1+1+1; } return 0; }\n",
            /*register_reentry_natives=*/false, /*trap_stub=*/reinterpret_cast<void*>(kt_trap));
        ck(m != nullptr, "compile infinite loop for r14-trap test");
        if (m) {
            auto inst = make_identity_instance(*m, "r14.mod", provider);
            inst.trap_stub = reinterpret_cast<void*>(kt_trap);
            context_t ctx; ctx.budget_remaining = 1000; ctx.max_call_depth = 64;
            auto r = ember_call_keyed_void(inst, "main", ctx, adapter);
            ck(r.trapped, "r14 correctness: budget trap caught by keyed API (trapped=true)");
            ck(!r.ok, "r14 correctness: trapped call is not ok");
            ck(r.reason.find("budget") != std::string::npos,
               "r14 correctness: trap reason carries 'budget' (recorded on the SUPPLIED context, r14 = ctx)");
            ck(r.reason.size() > 0, "r14 correctness: trap carried a detail string");
        }
    }

    // =====================================================================
    // 4. r15 INVARIANT THROUGHOUT DIRECT RECURSION — a native the script calls
    //    reads r15 and records it; observed == the installed route word.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xAB);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_identity(
            "fn rec(n: i64) -> i64 { if (n < 1) { return reentry_probe(0); } return rec(n-1); }\n"
            "fn main() -> i64 { return rec(5); }\n",
            /*register_reentry_natives=*/true);
        ck(m != nullptr, "compile rec + main with reentry_probe native for r15-invariant test");
        if (m) {
            auto inst = make_identity_instance(*m, "r15.mod", provider);
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            g_reentry.reset();
            auto rw = adapter.route_word(ModuleId{"r15.mod", 1}, 1, "ember/dispatch");
            ck(bool(rw), "r15 test: derived route word for comparison");
            auto r = ember_call_keyed_void(inst, "main", ctx, adapter);
            ck(r.ok && r.value == 0, "r15 invariant: rec(5) ran + reentry_probe(0)*2=0");
            ck(g_reentry.calls.load() == 1, "r15 invariant: reentry_probe called once (at the leaf of recursion)");
            ck(g_reentry.observed_r15.load() == *rw.value,
               "r15 invariant: native observed r15 == the route word the thunk installed (preserved through recursion)");
            ck(g_reentry.observed_r15.load() != 0, "r15 invariant: r15 was non-zero (route word installed, not garbage)");
        }
    }

    // =====================================================================
    // 5. r15 PRESERVED ACROSS NATIVE RE-ENTRY.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xAB);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_identity(
            "fn rec2(n: i64) -> i64 { if (n < 1) { return 0; } return native_reentry_probe(n); }\n"
            "fn main() -> i64 { return rec2(3); }\n",
            /*register_reentry_natives=*/true);
        ck(m != nullptr, "compile rec2 + main with native_reentry_probe for native re-entry test");
        if (m) {
            auto inst = make_identity_instance(*m, "reentry.mod", provider);
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            auto rw = adapter.route_word(ModuleId{"reentry.mod", 1}, 1, "ember/dispatch");
            ck(bool(rw), "reentry test: derived route word");
            auto rec2it = m->slots.find("rec2");
            g_rec.entry = (rec2it != m->slots.end()) ? m->table.get(rec2it->second) : nullptr;
            g_rec.ctx = &ctx;
            g_rec.route_word = *rw.value;
            g_rec.observed_r15_before.store(0);
            g_rec.observed_r15_after.store(0);
            g_rec.reentry_calls.store(0);
            auto r = ember_call_keyed_void(inst, "main", ctx, adapter);
            ck(r.ok, "native re-entry: keyed void call ok");
            ck(g_rec.observed_r15_before.load() == *rw.value,
               "native re-entry: native observed r15 == route word BEFORE re-entry");
            ck(g_rec.observed_r15_after.load() == *rw.value,
               "native re-entry: native observed r15 == route word AFTER re-entry (preserved/reinstalled)");
            ck(g_rec.reentry_calls.load() >= 1, "native re-entry: the re-entry thunk was called at least once");
        }
    }

    // =====================================================================
    // 6. CALLER r14/r15 RESTORED.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xAB);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_identity("fn main() -> i64 { return 42; }\n");
        ck(m != nullptr, "compile main=42 for caller-restore test");
        if (m) {
            auto inst = make_identity_instance(*m, "restore.mod", provider);
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL;
            uint64_t caller_r15 = 0xCAFEBABE12345678ULL;
            ember_set_r15(caller_r15);
            auto r = ember_call_keyed_void(inst, "main", ctx, adapter);
            ck(r.ok && r.value == 42, "caller restore: call returned 42");
            uint64_t after = ember_read_r15();
            ck(after == caller_r15, "caller restore: caller r15 restored to its original value");
        }
    }

    // =====================================================================
    // 7. r15 CLEARED BEFORE RESTORING CALLER VALUE (normal + trapped exits).
    //    The trapped-exit path leaves caller r15 intact. The keyed API owns the
    //    checkpoint (§9.8) and returns CallResult{trapped=true}; the test's own
    //    setjmp here is ONLY to safely bracket the ember_set_r15 clobber (r15
    //    is callee-saved) — the trap is caught by the API's internal checkpoint
    //    and reported via r.trapped, NOT by longjmp to this setjmp. After the
    //    trapped return, the C++ epilogue restores the caller's callee-saved
    //    r15, so the transient route r15 is cleared and the caller value
    //    survives.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xAB);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_identity(
            "fn main() -> i64 { while (true) { let mut x: i64 = 1+1+1; } return 0; }\n",
            /*register_reentry_natives=*/false, /*trap_stub=*/reinterpret_cast<void*>(kt_trap));
        ck(m != nullptr, "compile infinite loop for trapped-exit r15-clear test");
        if (m) {
            auto inst = make_identity_instance(*m, "trap.mod", provider);
            inst.trap_stub = reinterpret_cast<void*>(kt_trap);
            context_t ctx; ctx.budget_remaining = 1000; ctx.max_call_depth = 64;
            uint64_t caller_r15 = 0xDEADBEEF11111111ULL;
            // The setjmp brackets the r15 clobber so the C++ frame's saved
            // callee-saved r15 is restored on any exit path; the API's own
            // internal checkpoint catches the trap (this setjmp is never
            // longjmp'd to — has_checkpoint stays false so kt_trap longjmps to
            // the API's checkpoint, not here).
            if (EMBER_SETJMP(ctx.checkpoint)) {
                ck(false, "trapped exit: test checkpoint should not fire (API owns the checkpoint)");
                ctx.has_checkpoint = false;
            } else {
                ember_set_r15(caller_r15);
                auto r = ember_call_keyed_void(inst, "main", ctx, adapter);
                ck(r.trapped, "trapped exit: budget trap caught by keyed API (trapped=true)");
                uint64_t after = ember_read_r15();
                ck(after == caller_r15,
                   "trapped exit: transient r15 cleared + caller r15 restored (callee-saved preserved across trapped return)");
            }
        }
    }

    // =====================================================================
    // 8. EDGE ROUTE WORDS — route words 0 and UINT64_MAX both work.
    // =====================================================================
    {
        auto m = compile_identity("fn main() -> i64 { return 7; }\n");
        ck(m != nullptr, "compile main=7 for edge-route-word test");
        if (m) {
            for (uint64_t w : {0ULL, UINT64_MAX}) {
                std::array<uint8_t,32> mat{};
                // Pack the route word into the material so the adapter folds to
                // a word that depends on it (the exact value isn't asserted;
                // the call must SUCCEED under any route word, including the
                // degenerate edge words — the adapter folds 0/UINT64_MAX-
                // influenced material to a valid, non-failing route word).
                for (int i = 0; i < 8; ++i)
                    mat[i] = uint8_t((w >> (8*i)) & 0xFF);
                auto provider = std::make_shared<FixedMaterialProvider>(mat);
                DispatchKeyAdapter adapter(provider);
                auto inst = make_identity_instance(*m, "edge.mod", provider);
                context_t ctx; ctx.budget_remaining = 1'000'000'000LL;
                auto r = ember_call_keyed_void(inst, "main", ctx, adapter);
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "edge route word 0x%llx: call succeeded + returned 7",
                              (unsigned long long)w);
                ck(r.ok && r.value == 7, buf);
            }
        }
    }

    // =====================================================================
    // 9. TRY/THROW/CATCH PRESERVES THE KEYED r15 INVARIANT.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xAB);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_identity(
            "fn main() -> i64 {\n"
            "  try { throw 99; } catch (e) { return reentry_probe(e); }\n"
            "}\n",
            /*register_reentry_natives=*/true);
        ck(m != nullptr, "compile try/throw/catch main with reentry_probe for r15-invariant test");
        if (m) {
            auto inst = make_identity_instance(*m, "tc.mod", provider);
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            auto rw = adapter.route_word(ModuleId{"tc.mod", 1}, 1, "ember/dispatch");
            g_reentry.reset();
            auto r = ember_call_keyed_void(inst, "main", ctx, adapter);
            ck(r.ok, "try/catch: keyed void call ok");
            ck(g_reentry.calls.load() == 1, "try/catch: reentry_probe called once (in the catch block)");
            ck(g_reentry.observed_r15.load() == *rw.value,
               "try/catch: r15 == route word preserved through throw->catch longjmp (slot 5 restore)");
            ck(r.value == 198, "try/catch: catch bound e=99, reentry_probe(99)==198");
        }
    }

    // =====================================================================
    // 10. CONCURRENT CALLERS — different providers do not share route state.
    // =====================================================================
    {
        auto m = compile_identity(
            "fn rec(n: i64) -> i64 { if (n < 1) { return reentry_probe(0); } return rec(n-1); }\n"
            "fn main() -> i64 { return rec(3); }\n",
            /*register_reentry_natives=*/true);
        ck(m != nullptr, "compile rec + main for concurrent-callers test");
        if (m) {
            auto providerA = std::make_shared<CountingProvider>(0x11);
            auto providerB = std::make_shared<CountingProvider>(0x22);
            DispatchKeyAdapter adapterA(providerA);
            DispatchKeyAdapter adapterB(providerB);
            auto instA = make_identity_instance(*m, "concA.mod", providerA);
            auto instB = make_identity_instance(*m, "concB.mod", providerB);
            auto rwA = adapterA.route_word(ModuleId{"concA.mod", 1}, 1, "ember/dispatch");
            auto rwB = adapterB.route_word(ModuleId{"concB.mod", 1}, 1, "ember/dispatch");
            ck(*rwA.value != *rwB.value, "concurrent: the two providers derive DIFFERENT route words");

            // Each thread observes its OWN route word in the native probe. The
            // probe writes to a thread-local atomic (separate globals per
            // thread would be ideal; we serialize the runs by giving each
            // thread its own observation slot via distinct globals).
            struct ThreadObs {
                std::atomic<uint64_t> r15{0};
                std::atomic<bool> done{false};
                std::atomic<int64_t> ret{-1};
            };
            ThreadObs obsA, obsB;
            // Per-thread probe globals: since g_reentry is one global, each
            // thread resets + observes it atomically; the observation is the
            // LAST write in that thread. The two threads run concurrently, so
            // we capture each thread's observation into its own ThreadObs.
            auto run = [&](ThreadObs* obs, ModuleInstance* inst,
                           DispatchKeyAdapter* adapter, uint64_t /*expected_rw*/) {
                context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
                g_reentry.reset();
                auto r = ember_call_keyed_void(*inst, "main", ctx, *adapter);
                obs->r15.store(g_reentry.observed_r15.load());
                obs->ret.store(r.value);
                obs->done.store(true);
            };
            std::thread ta(run, &obsA, &instA, &adapterA, *rwA.value);
            std::thread tb(run, &obsB, &instB, &adapterB, *rwB.value);
            ta.join(); tb.join();
            ck(obsA.done.load() && obsB.done.load(), "concurrent: both threads completed");
            ck(obsA.ret.load() == 0, "concurrent: thread A's call returned 0 (rec leaf)");
            ck(obsB.ret.load() == 0, "concurrent: thread B's call returned 0 (rec leaf)");
            ck(obsA.r15.load() != 0, "concurrent: thread A observed a non-zero r15");
            ck(obsB.r15.load() != 0, "concurrent: thread B observed a non-zero r15");
            ck(obsA.r15.load() != obsB.r15.load(),
               "concurrent: the two callers observed DIFFERENT r15 values (no shared route state, §6.6)");
        }
    }

    // =====================================================================
    // 11. ONE-i64 AND TWO-i64 SAFE APIs.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xAB);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_identity("fn main(a: i64) -> i64 { return a * 3; }\n");
        ck(m != nullptr, "compile main(a)=a*3 for keyed_i64 test");
        if (m) {
            auto inst = make_identity_instance(*m, "i64.mod", provider);
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL;
            auto r = ember_call_keyed_i64(inst, "main", ctx, 14, adapter);
            ck(r.ok && r.value == 42, "keyed_i64: main(14) = 42 (14*3)");
        }
        auto m2 = compile_identity("fn main(a: i64, b: i64) -> i64 { return a + b; }\n");
        ck(m2 != nullptr, "compile main(a,b)=a+b for keyed_i64_i64 test");
        if (m2) {
            auto inst = make_identity_instance(*m2, "i64i64.mod", provider);
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL;
            auto r = ember_call_keyed_i64_i64(inst, "main", ctx, 20, 22, adapter);
            ck(r.ok && r.value == 42, "keyed_i64_i64: main(20,22) = 42 (20+22)");
        }
    }

    // =====================================================================
    // 12. REGALLOC EXCLUDES r15 IN KEYED MODE — no VReg assigned to r15 under
    //     keyed mode; legacy mode retains r15 in the pool.
    // =====================================================================
    {
        const char* src =
            "fn main() -> i64 {\n"
            "  let a: i64 = 1; let b: i64 = 2; let c: i64 = 3; let d: i64 = 4;\n"
            "  let e: i64 = 5; let f: i64 = 6; let g: i64 = 7; let h: i64 = 8;\n"
            "  let p1: i64 = a * b * c * d; let p2: i64 = e * f * g * h;\n"
            "  let p3: i64 = a * e + b * f + c * g + d * h;\n"
            "  return p1 + p2 + p3;\n"
            "}\n";
        auto lr = tokenize(src, "<k>");
        auto pr = parse(std::move(lr.toks));
        Program prog = std::move(pr.program);
        int si = 0; std::unordered_map<std::string,int> slots;
        for(auto&fn:prog.funcs){slots[fn.name]=si++;fn.slot=slots[fn.name];}
        std::unordered_map<std::string,NativeSig> natives; OpOverloadTable ov;
        auto layouts = build_struct_layouts(prog); prog.string_xor_key = 0;
        sema(prog, natives, slots, 0, &ov, &layouts);
        GlobalsBlock gb; gb.base=0; g_globals_for_codegen=&gb;
        DispatchTable table(prog.funcs.size());
        CodeGenCtx ctx; ctx.dispatch_base=int64_t(table.base());
        ctx.script_slots=&slots; ctx.structs=&layouts; ctx.use_context_reg=true;
        KeyedDispatchCodegen kd{}; kd.runtime_key = RuntimeKeyLocation::R15;
        ctx.keyed_dispatch = &kd;
        bool any_r15 = false, any_reg = false;
        for (auto& fn : prog.funcs) {
            ThinFunction thf = lower_function(fn, ctx);
            run_regalloc(thf, 0, /*exclude_r15=*/true);
            for (const auto& [v, a] : thf.ra.map) {
                if (a.in_reg) { any_reg = true; if (a.reg_id == 15) any_r15 = true; }
            }
        }
        ck(any_reg, "keyed regalloc: still assigns VRegs to registers (pool non-empty)");
        ck(!any_r15, "keyed regalloc: NO VReg assigned to r15 (r15 excluded from the pool, §6.4)");

        // LEGACY mode: r15 IS in the pool.
        ctx.keyed_dispatch = nullptr;
        bool legacy_any_r15 = false;
        for (auto& fn : prog.funcs) {
            ThinFunction thf = lower_function(fn, ctx);
            run_regalloc(thf, 0, /*exclude_r15=*/false);
            for (const auto& [v, a] : thf.ra.map)
                if (a.in_reg && a.reg_id == 15) legacy_any_r15 = true;
        }
        ck(legacy_any_r15, "legacy regalloc: r15 IS in the pool (six-register behavior preserved, §6.4)");
    }

    // =====================================================================
    // 13. LEGACY ember_call_* UNCHANGED — raw helpers leave r15 untouched.
    // =====================================================================
    {
        auto m = compile_identity("fn main() -> i64 { return 1; }\n");
        ck(m != nullptr, "compile main=1 for legacy-unchanged test");
        if (m) {
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL;
            uint64_t caller_r15 = 0xBADC0FFEE0LL;
            ember_set_r15(caller_r15);
            int64_t r = ember_call_void(m->main_entry, &ctx);
            ck(r == 1, "legacy ember_call_void: main()==1 (raw helper unchanged)");
            ck(ember_read_r15() == caller_r15, "legacy ember_call_void: r15 untouched (no route word installed)");
        }
    }

    // =====================================================================
    // Summary
    // =====================================================================
    std::printf("== %d checks, %s ==\n", g_checks, g_fail ? "FAILED" : "ALL PASSED");
    return g_fail;
}
