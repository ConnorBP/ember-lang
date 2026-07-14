// keyed_dispatch_extensions_test — Red 8
// (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §6.5–§6.7, §9.8, §10.3,
// §12.4, §14.2, §14.3 Red 8): the host / lifecycle / thread / coroutine
// integration gate for keyed dispatch.
//
// RED-GREEN contract chunk for the extension migration. This is the RED side
// (written first) of the Red 8 contract. It pins, against real JIT-compiled
// modules whose dispatch records are assembled on a per-runtime ModuleInstance
// and whose extension state lives on that same instance (not in file-static
// globals), the §6.5–§6.7 / §9.8 / §10.3 / §12.4 / §14.2 mandatory buckets:
//
//   - safe keyed host calls:   by logical slot AND by export name, through the
//                              immutable ModuleDispatchRecord + the transient
//                              provider-derived route word (NOT raw
//                              entry_table[logical_slot]).
//   - provider unavailable:    a provider that cannot derive returns a
//                              structured CallResult/ExtensionError failure;
//                              the thunk is never entered.
//   - normal/trap cleanup:     the keyed host boundary cleans all runtime/TLS
//                              state on every normal AND trapped exit (the
//                              current-runtime TLS is cleared on return and on
//                              the keyed API's internal longjmp recovery).
//   - lifecycle after replace: a registered routine is invoked AFTER its
//                              dispatch entry is replaced; the keyed lifecycle
//                              tick resolves the entry at INVOCATION time
//                              (§12.4), so it calls the REPLACEMENT.
//   - delayed thread after replace: a spawned worker resolves its entry at
//                              EXECUTION time (not cached at spawn), so a
//                              replacement published between spawn and the
//                              worker's ember_call is observed.
//   - two runtimes, distinct providers: concurrent workers belonging to TWO
//                              ModuleInstances with independent providers +
//                              independent per-runtime extension state do not
//                              share or clobber each other's state (§6.6, §10.3
//                              two-runtime isolation).
//   - coroutine yield/resume:  on Win64 a coroutine that yields + resumes
//                              across an entry replacement must preserve the
//                              keyed register/generation invariant; where that
//                              invariant cannot yet be guaranteed, coroutine
//                              start in keyed mode returns a TYPED
//                              unsupported-mode failure (§6.7 fail-closed).
//   - two-runtime store isolation: the per-runtime extension stores on two
//                              ModuleInstances are independent.
//   - legacy identity unchanged: the raw ember_call_* helpers + the legacy
//                              thread_init/coroutine_init/lifecycle identity
//                              APIs are byte/behavior-preserving when no keyed
//                              runtime is active.
//
// The fixture compiles small modules through the existing pipeline
// (use_context_reg=true + the keyed CodeGenCtx that excludes r15 from
// regalloc), assembles an identity ModuleDispatchRecord on each ModuleInstance,
// and drives the extensions through the keyed host boundary.
//
// Links ember + ember_frontend + ember_import + ember_ext_lifecycle +
// ember_ext_thread + ember_ext_coroutine (+ kernel32 on Windows). NOT a CTest
// entry: the configured/filtered CTest count must stay unchanged (§14.1); the
// target building cleanly + the executable passing IS the gate.

#include "../src/engine.hpp"
#include "../src/context.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/keyed_dispatch.hpp"
#include "../src/module_layout.hpp"
#include "../src/module_instance.hpp"
#include "../src/runtime_extension_state.hpp"
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
#include "../src/dispatch_abi.hpp"
#include "../src/extension_registry.hpp"
#include "import.hpp"

#include "../extensions/lifecycle/ext_lifecycle.hpp"
#include "../extensions/thread/ext_thread.hpp"
#include "../extensions/coroutine/ext_coroutine.hpp"

#include <atomic>
#include <array>
#include <algorithm>
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
// assert the provider fires exactly once per outer keyed call.
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

// A host trap stub: records the reason on the context and longjmps to the
// checkpoint. Mirrors the kt_trap in the outer-thunk test.
extern "C" void ke_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) EMBER_LONGJMP(ctx->checkpoint, 1);
    }
    std::abort();
}

// ===========================================================================
// Compile a small module through the existing pipeline with the keyed
// CodeGenCtx (use_context_reg=true + keyed descriptor that excludes r15 from
// regalloc). The compiled module owns its dispatch table + allowlist + the
// keyed record storage the ModuleInstance's dispatch record borrows.
// ===========================================================================
struct Compiled {
    std::string name;
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string, int> slots;
    std::unordered_map<std::string, NativeSig> natives;
    std::vector<uint8_t> allowlist;
    int slot_count = 0;
    void* main_entry = nullptr;
    context_t ctx{};
    ~Compiled() { for (auto& fn : fns) if (fn.exec) free_executable(fn.exec); }
};

static std::unique_ptr<Compiled> compile_module(
    const std::string& src, const std::string& name,
    bool register_thread_natives = false,
    bool register_coroutine_natives = false,
    bool register_lifecycle_natives = false) {
    auto m = std::make_unique<Compiled>();
    m->name = name;
    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); } catch (...) { return nullptr; }
    auto lr = tokenize(resolved, "<ke>"); if (!lr.ok) return nullptr;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
    int si = 0; for(auto&fn:pr.program.funcs){m->slots[fn.name]=si++;fn.slot=m->slots[fn.name];}
    m->slot_count = si;
    ember::OpOverloadTable ov;
    if (register_thread_natives)    ember::ext_thread::register_natives(m->natives);
    if (register_coroutine_natives) ember::ext_coroutine::register_natives(m->natives);
    if (register_lifecycle_natives) ember::ext_lifecycle::register_natives(m->natives);
    auto layouts = build_struct_layouts(pr.program); pr.program.string_xor_key = 0;
    if(!sema(pr.program,m->natives,m->slots,0,&ov,&layouts).ok) return nullptr;

    ember::GlobalsBlock gb; std::vector<uint8_t> gbs(pr.program.globals.size()*8, 0);
    gb.base=int64_t(gbs.data());
    { uint32_t gi=0; for (auto& g : pr.program.globals) { gb.index[g.name]=gi++; gb.types[g.name]=g.ty.get(); } }
    eval_global_initializers(pr.program, GlobalInitCtx{gbs, gb.index, gb.types});
    ember::g_globals_for_codegen = nullptr;
    m->table = DispatchTable(si);
    m->allowlist = build_fn_allowlist(m->slots, si);

    ember::CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(m->table.base());
    ctx.natives=&m->natives; ctx.script_slots=&m->slots; ctx.structs=&layouts;
    ctx.globals_index = &gb.index; ctx.globals_types = &gb.types;
    ctx.use_context_reg = true;
    ctx.emit_depth_checks = true;
    ctx.emit_budget_checks = true;  // Red 8: enable the budget trap for the trapped-cleanup test
    ctx.max_call_depth = 64;
    ctx.fn_allowlist_base = int64_t(m->allowlist.data());
    ctx.fn_slot_count = si;
    ctx.trap_stub = reinterpret_cast<void*>(ke_trap);
    ctx.trap_ctx = nullptr;  // B1 mode: ctx arrives in r14
    // KEYED MODE: reserves r15 in regalloc + the keyed emit path.
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

// Build a ModuleInstance in identity mode over a compiled module + assemble
// an identity ModuleDispatchRecord on the instance.
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
    inst.trap_stub = reinterpret_cast<void*>(ke_trap);
    inst.ext_state = std::make_shared<RuntimeExtensionState>();
    assemble_identity_dispatch_record(inst);
    return inst;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== keyed_dispatch_extensions_test (Red 8) ==\n");

    // =====================================================================
    // 1. SAFE KEYED HOST CALLS — by logical slot AND by export name.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xA1);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_module("fn main(a: i64) -> i64 { return a + 100; }\n", "slot.mod");
        ck(m != nullptr, "compile main(a)=a+100 for keyed-by-slot test");
        if (m) {
            auto inst = make_identity_instance(*m, "slot.mod", provider);
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            uint32_t main_slot = inst.named_entries["main"];
            auto r_slot = ember_call_keyed_i64_by_slot(inst, main_slot, ctx, 7, adapter);
            ck(r_slot.ok && r_slot.value == 107, "keyed call by logical slot: main(7) = 107");
            auto r_name = ember_call_keyed_i64(inst, "main", ctx, 8, adapter);
            ck(r_name.ok && r_name.value == 108, "keyed call by export name: main(8) = 108");
            auto e_slot = resolve_entry_keyed(inst, LogicalCallableId{main_slot}, adapter);
            ck(bool(e_slot) && *e_slot.value != nullptr,
               "resolve_entry_keyed: returns a non-null entry for the logical slot");
            auto e_name = resolve_entry_by_name_keyed(inst, "main", adapter);
            ck(bool(e_name) && *e_name.value != nullptr,
               "resolve_entry_by_name_keyed: returns a non-null entry for 'main'");
            ck(*e_slot.value == *e_name.value,
               "resolve_entry_keyed + resolve_entry_by_name_keyed agree on the entry");
            ck(*e_slot.value == m->table.get(main_slot),
               "resolve_entry_keyed: entry matches the physical slot storage (identity record)");
        }
    }

    // =====================================================================
    // 2. PROVIDER-UNAVAILABLE STRUCTURED FAILURE.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xA2);
        provider->fail_next.store(true);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_module("fn main() -> i64 { return 1; }\n", "prov.mod");
        ck(m != nullptr, "compile main=1 for provider-unavailable test");
        if (m) {
            auto inst = make_identity_instance(*m, "prov.mod", provider);
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL;
            uint32_t main_slot = inst.named_entries["main"];
            auto r = ember_call_keyed_i64_by_slot(inst, main_slot, ctx, 0, adapter);
            ck(!r.ok, "provider unavailable: CallResult reports failure (by slot)");
            ck(r.reason.size() > 0, "provider unavailable: structured reason carried (by slot)");
            auto e = resolve_entry_keyed(inst, LogicalCallableId{main_slot}, adapter);
            ck(!bool(e), "provider unavailable: resolve_entry_keyed returns a structured error");
        }
    }

    // =====================================================================
    // 3. NORMAL/TRAP CLEANUP — the keyed host boundary clears the current-
    //    runtime TLS on every exit (normal return AND trapped longjmp). The
    //    keyed API establishes its OWN checkpoint internally (§9.8) and
    //    returns a structured CallResult{trapped=true} on a trap — the test
    //    checks r.trapped, NOT a longjmp to the test's own checkpoint (the
    //    keyed API owns the checkpoint, so the test must use the returned
    //    result; this corrects the original invalid test expectation that
    //    caused the stall — the test's setjmp was overwritten by the API's
    //    internal setjmp, and the API's setjmp is the one that catches the
    //    trap).
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xA3);
        DispatchKeyAdapter adapter(provider);
        // Normal-exit cleanup: after a normal call returns, the TLS is cleared.
        auto m2 = compile_module("fn main() -> i64 { return 5; }\n", "cleanup_ok.mod");
        ck(m2 != nullptr, "compile main=5 for normal cleanup test");
        if (m2) {
            auto inst2 = make_identity_instance(*m2, "cleanup_ok.mod", provider);
            context_t ctx2; ctx2.budget_remaining = 1'000'000'000LL;
            uint32_t s2 = inst2.named_entries["main"];
            ck(ember_current_keyed_runtime() == nullptr,
               "cleanup: no keyed runtime active before the call");
            auto r = ember_call_keyed_i64_by_slot(inst2, s2, ctx2, 0, adapter);
            ck(r.ok && r.value == 5, "cleanup: normal call returned 5");
            ck(ember_current_keyed_runtime() == nullptr,
               "cleanup: TLS current-runtime cleared after normal return");
        }
        // Trapped-exit cleanup: a budget trap fires inside the keyed API; the
        // API's internal checkpoint catches it and returns
        // CallResult{trapped=true}. The TLS is cleared on the trapped exit.
        auto m = compile_module(
            "fn main() -> i64 { while (true) { let mut x: i64 = 1+1+1; } return 0; }\n",
            "cleanup.mod");
        ck(m != nullptr, "compile infinite loop for trapped cleanup test");
        if (m) {
            auto inst = make_identity_instance(*m, "cleanup.mod", provider);
            context_t ctx; ctx.budget_remaining = 1000; ctx.max_call_depth = 64;
            uint32_t main_slot = inst.named_entries["main"];
            auto r = ember_call_keyed_i64_by_slot(inst, main_slot, ctx, 0, adapter);
            ck(r.trapped, "cleanup: budget trap caught by keyed API (trapped=true, structured return)");
            ck(!r.ok, "cleanup: trapped call is not ok");
            ck(r.reason.size() > 0, "cleanup: trapped call carries a structured reason");
            ck(ember_current_keyed_runtime() == nullptr,
               "cleanup: TLS current-runtime cleared after trapped exit (API's internal longjmp recovery)");
            // The keyed API's internal longjmp recovery calls ctx.reset_for_call()
            // which clears last_trap; the structured reason is carried in r.reason.
            ck(r.reason.find("budget") != std::string::npos,
               "cleanup: trapped call reason carries 'budget' (structured trap reason in CallResult)");
        }
    }

    // =====================================================================
    // 4. LIFECYCLE INVOCATION AFTER ENTRY REPLACEMENT.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xA4);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_module(
            "fn tick(data: i64) -> i64 { return data + 1; }\n"
            "fn main() -> i64 { let h = &tick; return register_routine(h, 41); }\n",
            "lc.mod", false, false, /*register_lifecycle_natives=*/true);
        ck(m != nullptr, "compile tick+main with lifecycle natives");
        if (m) {
            auto inst = make_identity_instance(*m, "lc.mod", provider);
            ck(ext_lifecycle::lifecycle_init_keyed(inst),
               "lifecycle: lifecycle_init_keyed populates the per-runtime state");
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            uint32_t main_slot = inst.named_entries["main"];
            auto r = ember_call_keyed_i64_by_slot(inst, main_slot, ctx, 0, adapter);
            ck(r.ok && r.value >= 1, "lifecycle: main registered a routine (id >= 1)");
            auto routines = ext_lifecycle::host_routines_keyed(inst);
            ck(routines.size() == 1 && routines[0].data == 41,
               "lifecycle: per-runtime store has 1 routine with data=41");
            uint32_t tick_slot = routines[0].slot;
            auto m2 = compile_module("fn tick(data: i64) -> i64 { return data + 1000; }\n",
                                      "lc_repl.mod");
            ck(m2 != nullptr, "lifecycle: compile replacement tick (data+1000)");
            if (m2) {
                void* repl_entry = m2->table.get(m2->slots["tick"]);
                ck(repl_entry != nullptr, "lifecycle: replacement entry non-null");
                m->table.set(tick_slot, repl_entry);
                int64_t got = ext_lifecycle::lifecycle_tick_keyed(inst, ctx, adapter);
                ck(got == 1041,
                   "lifecycle: tick after replacement called the REPLACEMENT (41+1000=1041), not the stale original (41+1=42)");
            }
        }
    }

    // =====================================================================
    // 5. DELAYED THREAD EXECUTION AFTER REPLACEMENT.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xA5);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_module(
            "fn worker(arg: i64) -> i64 { return arg + 1; }\n"
            "fn main() -> i64 { let h = &worker; return thread_spawn(h, 499); }\n",
            "th.mod", /*register_thread_natives=*/true);
        ck(m != nullptr, "compile worker+main with thread natives");
        if (m) {
            auto inst = make_identity_instance(*m, "th.mod", provider);
            ck(ext_thread::thread_init_keyed(inst),
               "thread: thread_init_keyed populates the per-runtime state");
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            // Publish the context on the per-runtime thread state so the
            // thread_spawn native (called from main under the keyed boundary)
            // can pass it to the worker.
            inst.ext_state->thread.ctx = &ctx;
            uint32_t main_slot = inst.named_entries["main"];
            // Lock call_mutex around the keyed call so the spawned worker
            // blocks until we publish the replacement + join (the in-context-
            // thread serialization model: the worker locks call_mutex before
            // its ember_call, so it waits for us to release it via join).
            ctx.call_mutex.lock();
            auto r = ember_call_keyed_i64_by_slot(inst, main_slot, ctx, 0, adapter);
            // Do NOT unlock here — keep call_mutex held so the worker stays
            // blocked until we publish the replacement + thread_join_keyed
            // releases it.
            ck(r.ok && r.value >= 1, "thread: main spawned a worker (tid >= 1)");
            int64_t tid = r.value;
            auto m2 = compile_module("fn worker(arg: i64) -> i64 { return arg + 5000; }\n",
                                      "th_repl.mod");
            ck(m2 != nullptr, "thread: compile replacement worker (arg+5000)");
            if (m2) {
                void* repl_entry = m2->table.get(m2->slots["worker"]);
                uint32_t worker_slot = static_cast<uint32_t>(m->slots["worker"]);
                m->table.set(worker_slot, repl_entry);
                int64_t joined = ext_thread::thread_join_keyed(inst, tid, ctx, adapter);
                ck(joined == 5499,
                   "thread: worker after replacement called the REPLACEMENT (499+5000=5499), not the stale original (499+1=500)");
            }
        }
    }

    // =====================================================================
    // 6. CONCURRENT WORKERS — two ModuleInstances with distinct providers.
    // =====================================================================
    {
        auto providerA = std::make_shared<CountingProvider>(0x6A);
        auto providerB = std::make_shared<CountingProvider>(0x6B);
        DispatchKeyAdapter adapterA(providerA);
        DispatchKeyAdapter adapterB(providerB);
        auto mA = compile_module(
            "fn worker(arg: i64) -> i64 { return arg + 10; }\n"
            "fn main() -> i64 { let h = &worker; return thread_spawn(h, 100); }\n",
            "concA.mod", /*register_thread_natives=*/true);
        auto mB = compile_module(
            "fn worker(arg: i64) -> i64 { return arg + 20; }\n"
            "fn main() -> i64 { let h = &worker; return thread_spawn(h, 200); }\n",
            "concB.mod", /*register_thread_natives=*/true);
        ck(mA != nullptr && mB != nullptr, "concurrent: compiled two modules with thread natives");
        if (mA && mB) {
            auto instA = make_identity_instance(*mA, "concA.mod", providerA);
            auto instB = make_identity_instance(*mB, "concB.mod", providerB);
            ck(ext_thread::thread_init_keyed(instA), "concurrent: thread_init_keyed A");
            ck(ext_thread::thread_init_keyed(instB), "concurrent: thread_init_keyed B");
            struct Obs { std::atomic<int64_t> ret{-1}; std::atomic<bool> done{false}; };
            Obs obsA, obsB;
            auto run = [&](Obs* obs, ModuleInstance* inst, DispatchKeyAdapter* adapter) {
                context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
                // Publish the context so thread_spawn (called from main) can reach it.
                inst->ext_state->thread.ctx = &ctx;
                uint32_t main_slot = inst->named_entries["main"];
                auto r = ember_call_keyed_i64_by_slot(*inst, main_slot, ctx, 0, *adapter);
                int64_t tid = r.value;
                if (tid >= 1) {
                    int64_t joined = ext_thread::thread_join_keyed(*inst, tid, ctx, *adapter);
                    obs->ret.store(joined);
                } else {
                    obs->ret.store(-2);
                }
                obs->done.store(true);
            };
            std::thread ta(run, &obsA, &instA, &adapterA);
            std::thread tb(run, &obsB, &instB, &adapterB);
            ta.join(); tb.join();
            ck(obsA.done.load() && obsB.done.load(), "concurrent: both workers completed");
            ck(obsA.ret.load() == 110, "concurrent: worker A returned 110 (its own module, its own state)");
            ck(obsB.ret.load() == 220, "concurrent: worker B returned 220 (its own module, its own state)");
            ck(obsA.ret.load() != obsB.ret.load(),
               "concurrent: the two runtimes produced DIFFERENT results (independent state, §6.6/§10.3)");
        }
    }

    // =====================================================================
    // 7. COROUTINE YIELD/RESUME ACROSS REPLACEMENT — §6.7 fail-closed.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xA7);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_module(
            "fn counter(arg: i64) -> i64 { yield arg; yield arg + 1; return arg + 2; }\n"
            "fn main() -> i64 { let h = &counter; return coroutine_start(h, 10); }\n",
            "co.mod", false, /*register_coroutine_natives=*/true);
        ck(m != nullptr, "compile counter+main with coroutine natives");
        if (m) {
            auto inst = make_identity_instance(*m, "co.mod", provider);
            ck(ext_coroutine::coroutine_init_keyed(inst),
               "coroutine: coroutine_init_keyed populates the per-runtime state");
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            uint32_t main_slot = inst.named_entries["main"];
            auto r = ember_call_keyed_i64_by_slot(inst, main_slot, ctx, 0, adapter);
            ck(r.ok, "coroutine: keyed main call completed (coroutine_start returned a value)");
            ck(r.value == 0,
               "coroutine: coroutine_start in keyed mode returned 0 (fail-closed, no coroutine created)");
            auto st = ext_coroutine::coroutine_last_start_status_keyed(inst);
            ck(st.unsupported_mode,
               "coroutine: per-runtime state records a TYPED unsupported-mode failure (§6.7 fail-closed)");
            ck(!st.ok, "coroutine: the typed status is a failure (not a silent success)");
        }
    }

    // =====================================================================
    // 8. TWO-RUNTIME STORE ISOLATION.
    // =====================================================================
    {
        auto providerA = std::make_shared<CountingProvider>(0x8A);
        auto providerB = std::make_shared<CountingProvider>(0x8B);
        DispatchKeyAdapter adapterA(providerA);
        DispatchKeyAdapter adapterB(providerB);
        auto mA = compile_module(
            "fn tick(data: i64) -> i64 { return data + 1; }\n"
            "fn main() -> i64 { let h = &tick; return register_routine(h, 111); }\n",
            "isoA.mod", false, false, /*register_lifecycle_natives=*/true);
        auto mB = compile_module(
            "fn tick(data: i64) -> i64 { return data + 1; }\n"
            "fn main() -> i64 { return 0; }\n",
            "isoB.mod", false, false, /*register_lifecycle_natives=*/true);
        ck(mA != nullptr && mB != nullptr, "isolation: compiled two lifecycle modules");
        if (mA && mB) {
            auto instA = make_identity_instance(*mA, "isoA.mod", providerA);
            auto instB = make_identity_instance(*mB, "isoB.mod", providerB);
            ck(ext_lifecycle::lifecycle_init_keyed(instA), "isolation: lifecycle_init_keyed A");
            ck(ext_lifecycle::lifecycle_init_keyed(instB), "isolation: lifecycle_init_keyed B");
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            uint32_t mainA = instA.named_entries["main"];
            auto rA = ember_call_keyed_i64_by_slot(instA, mainA, ctx, 0, adapterA);
            ck(rA.ok && rA.value >= 1, "isolation: A registered a routine");
            auto routA = ext_lifecycle::host_routines_keyed(instA);
            auto routB = ext_lifecycle::host_routines_keyed(instB);
            ck(routA.size() == 1, "isolation: A's per-runtime store has 1 routine");
            ck(routB.size() == 0, "isolation: B's per-runtime store has 0 routines (no clobber, independent stores)");
        }
    }

    // =====================================================================
    // 9. UNCHANGED LEGACY IDENTITY APIs.
    // =====================================================================
    {
        auto m = compile_module("fn main() -> i64 { return 1; }\n", "leg.mod");
        ck(m != nullptr, "legacy: compile main=1");
        if (m) {
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL;
            uint64_t caller_r15 = 0xFEEDFACE1234ULL;
            ember_set_r15(caller_r15);
            int64_t r = ember_call_void(m->main_entry, &ctx);
            ck(r == 1, "legacy: ember_call_void main()==1 (raw helper unchanged)");
            ck(ember_read_r15() == caller_r15, "legacy: ember_call_void leaves r15 untouched");
            ck(ember_current_keyed_runtime() == nullptr,
               "legacy: no keyed runtime active after raw ember_call_*");
        }
        ext_lifecycle::reset();
        ck(ext_lifecycle::host_count() == 0, "legacy: lifecycle reset -> host_count()==0");
        ck(!ext_thread::thread_init(nullptr, m ? m->table.base() : nullptr, 0),
           "legacy: thread_init(null,...) returns false (signature unchanged)");
        ck(!ext_coroutine::coroutine_init(nullptr, m ? m->table.base() : nullptr, 0),
           "legacy: coroutine_init(null,...) returns false (signature unchanged)");
    }

    // =====================================================================
    // Summary
    // =====================================================================
    std::printf("== %d checks, %s ==\n", g_checks, g_fail ? "FAILED" : "ALL PASSED");
    return g_fail;
}
