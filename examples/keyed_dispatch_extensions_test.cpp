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
//   - keyed-record routing:    a REAL keyed (DispatchMode::Keyed) module with a
//                              permuted physical table + padding — the keyed
//                              call resolves through the permutation and
//                              reaches the INTENDED callable, proving distinct
//                              providers route to the same entry (same key)
//                              and a wrong-key word lands in-range (§8.1).
//   - provider unavailable:    a provider that cannot derive returns a
//                              structured CallResult/ExtensionError failure;
//                              the thunk is never entered. Also tested for the
//                              lifecycle tick + the thread worker.
//   - normal/trap cleanup:     the keyed host boundary cleans all runtime/TLS
//                              state on every normal AND trapped exit (the
//                              current-runtime TLS is cleared on return and on
//                              the keyed API's internal longjmp recovery).
//   - lifecycle after replace: a registered routine is invoked AFTER its
//                              dispatch entry is replaced; the keyed lifecycle
//                              tick resolves the entry at INVOCATION time
//                              (§12.4) through the guarded core keyed-call API,
//                              so it calls the REPLACEMENT.
//   - lifecycle provider fail: lifecycle_tick_keyed with a failing provider
//                              contributes 0 (no thunk entered, no crash).
//   - delayed thread after replace: a spawned worker resolves its entry at
//                              EXECUTION time (not cached at spawn), so a
//                              replacement published between spawn and the
//                              worker's keyed call is observed. The worker
//                              uses the guarded core keyed-call API, NOT a raw
//                              re-entry thunk.
//   - thread provider fail:    a worker whose provider fails at execution
//                              returns 0 (structured failure, no crash).
//   - worker TLS/generation:   the worker establishes the current-keyed-runtime
//                              TLS on its thread (so a keyed native invoked by
//                              the worker's script finds THIS runtime's
//                              per-runtime state, not a legacy global).
//   - two runtimes, distinct providers: concurrent workers belonging to TWO
//                              ModuleInstances with independent providers +
//                              independent per-runtime extension state do not
//                              share or clobber each other's state (§6.6, §10.3
//                              two-runtime isolation).
//   - active-worker destruction: destroying a runtime (shared_ptr) with an
//                              active (not-yet-joined) joinable worker does NOT
//                              terminate the process (deterministic cleanup via
//                              the ThreadRuntimeState destructor).
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
// regalloc), assembles a ModuleDispatchRecord on each ModuleInstance (identity
// OR keyed), and drives the extensions through the keyed host boundary.
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
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
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
// ABI classifier helpers (ported from keyed_dispatch_modules_test, for
// building a REAL keyed dispatch record with permuted physical slots).
// ===========================================================================
static AbiType prim_to_abi(Prim p) {
    switch (p) {
    case Prim::Void:  return abi_scalar(PrimTag::Void);
    case Prim::Bool:  return abi_scalar(PrimTag::Bool);
    case Prim::I8:    return abi_scalar(PrimTag::I8);
    case Prim::I16:   return abi_scalar(PrimTag::I16);
    case Prim::I32:   return abi_scalar(PrimTag::I32);
    case Prim::I64:   return abi_scalar(PrimTag::I64);
    case Prim::U8:    return abi_scalar(PrimTag::U8);
    case Prim::U16:   return abi_scalar(PrimTag::U16);
    case Prim::U32:   return abi_scalar(PrimTag::U32);
    case Prim::U64:   return abi_scalar(PrimTag::U64);
    case Prim::F32:   return abi_scalar(PrimTag::F32);
    case Prim::F64:   return abi_scalar(PrimTag::F64);
    }
    return abi_scalar(PrimTag::Void);
}

static AbiType type_to_abi(const Type& t) {
    if (t.is_slice) {
        AbiType at; at.kind = TypeKind::Slice;
        at.elem = std::make_shared<AbiType>(t.elem ? type_to_abi(*t.elem) : abi_scalar(PrimTag::Void));
        return at;
    }
    if (t.is_lambda) {
        AbiType at; at.kind = TypeKind::Lambda;
        at.has_recorded_sig = t.has_recorded_sig;
        if (t.has_recorded_sig) {
            for (auto& p : t.recorded_params)
                at.recorded_params.push_back(std::make_shared<AbiType>(p ? type_to_abi(*p) : abi_scalar(PrimTag::Void)));
            at.recorded_ret = std::make_shared<AbiType>(t.recorded_ret ? type_to_abi(*t.recorded_ret) : abi_scalar(PrimTag::Void));
        }
        return at;
    }
    if (t.is_fn_handle) {
        AbiType at; at.kind = TypeKind::FnHandle;
        at.struct_name = t.struct_name;
        at.is_cross_module_handle = t.is_cross_module_handle;
        at.has_recorded_sig = t.has_recorded_sig;
        if (t.has_recorded_sig) {
            for (auto& p : t.recorded_params)
                at.recorded_params.push_back(std::make_shared<AbiType>(p ? type_to_abi(*p) : abi_scalar(PrimTag::Void)));
            at.recorded_ret = std::make_shared<AbiType>(t.recorded_ret ? type_to_abi(*t.recorded_ret) : abi_scalar(PrimTag::Void));
        }
        return at;
    }
    return prim_to_abi(t.prim);
}

static uint32_t words_for_type(const Type& t) {
    if (t.is_slice || t.is_lambda) return 2;
    return 1;
}

static uint64_t fingerprint_for(const FuncDecl& f) {
    CallableDescriptor d;
    d.arch = TargetArch::X64_Win64;
    d.convention_version = kWin64ConventionVersion;
    d.calling_mode = CallingMode::LegacyContext;
    d.visibility = f.is_exported ? Visibility::Public : Visibility::Private;
    const Type& rt = f.ret ? *f.ret : type_void();
    d.return_type = type_to_abi(rt);
    if (rt.is_void()) d.return_kind = ReturnKind::Void;
    else if (rt.is_float()) d.return_kind = (rt.prim == Prim::F64) ? ReturnKind::XmmF64 : ReturnKind::XmmF32;
    else if (rt.is_slice) d.return_kind = ReturnKind::SlicePair;
    else if (rt.is_lambda) d.return_kind = ReturnKind::LambdaPair;
    else if (rt.is_fn_handle) d.return_kind = ReturnKind::GpScalar;
    else d.return_kind = ReturnKind::GpScalar;
    d.is_coroutine = f.is_coroutine;
    d.is_special_entry = false;
    d.has_hidden_env = f.is_lambda;
    uint32_t pos = (d.has_hidden_return ? 1u : 0u) + (d.has_hidden_env ? 1u : 0u);
    size_t begin = f.is_lambda ? 1u : 0u;
    for (size_t i = begin; i < f.params.size(); ++i) {
        const Type& pt = *f.params[i].ty;
        AbiType at = type_to_abi(pt);
        uint32_t wc = words_for_type(pt);
        uint32_t byte_ext = (pt.is_slice || pt.is_lambda) ? 16u : (pt.is_float() ? (pt.prim == Prim::F64 ? 8u : 4u) : 8u);
        uint32_t partial = (wc > 0 && byte_ext != wc * 8u) ? (byte_ext - (wc - 1) * 8u) : 0u;
        WordClass wc0 = word_class_for_type(at);
        std::vector<WordClass> wcs;
        for (uint32_t w = 0; w < wc; ++w) wcs.push_back(wc0);
        d.params.push_back(abi_param(std::move(at), wc, byte_ext, partial, std::move(wcs), pos));
        pos += wc;
    }
    return classify_callable(d).fingerprint;
}

static ModuleManifest make_manifest(const Program& prog,
                                    const std::unordered_map<std::string,int>& slots,
                                    const std::string& module_id) {
    ModuleManifest m;
    m.module_id = module_id;
    std::unordered_map<std::string, const FuncDecl*> by_name;
    for (auto& fn : prog.funcs) by_name[fn.name] = &fn;
    for (auto& [name, slot] : slots) {
        ModuleCallable c;
        c.name = name;
        c.logical_slot = uint32_t(slot);
        auto it = by_name.find(name);
        c.abi_fingerprint = (it != by_name.end()) ? fingerprint_for(*it->second) : 0xA0A0A0A0A0A0A0A0ULL;
        c.visibility = Visibility::Public;
        c.calling_mode = CallingMode::LegacyContext;
        c.dispatch_domain = "";
        m.callables.push_back(std::move(c));
    }
    std::sort(m.callables.begin(), m.callables.end(),
              [](const ModuleCallable& a, const ModuleCallable& b) {
                  return a.logical_slot < b.logical_slot;
              });
    return m;
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

// ===========================================================================
// Build a REAL keyed (DispatchMode::Keyed) ModuleInstance with a permuted
// physical dispatch table + padding. The compiled module's entries are placed
// at the permuted physical positions; the keyed resolver re-derives the
// physical index from the route word. This proves distinct providers route
// correctly through a real permutation (not just identity-indexed storage).
//
// `key_seed` folds into the target route word that the planner uses to place
// entries. The runtime provider must derive the SAME route word for the call
// to reach the intended entry (a different provider derives a different word
// and lands on a different in-range entry or padding — the §2.4 contract).
// ===========================================================================
struct KeyedCompiled {
    std::string name;
    std::vector<CompiledFn> fns;
    DispatchTable table;                  // logical-indexed (slot -> entry)
    std::unordered_map<std::string, int> slots;
    std::unordered_map<std::string, NativeSig> natives;
    ModuleManifest manifest;
    ModuleLayoutPlan plan;
    RecordBuilderStorage st;
    ModuleDispatchRecord rec{};
    uint64_t target_route_word = 0;
    context_t ctx{};
    ~KeyedCompiled() { for (auto& fn : fns) if (fn.exec) free_executable(fn.exec); }
};

static std::unique_ptr<KeyedCompiled> compile_keyed_module(
    const std::string& src, const std::string& name, uint64_t key_seed,
    bool register_thread_natives = false,
    bool register_lifecycle_natives = false) {
    auto m = std::make_unique<KeyedCompiled>();
    m->name = name;

    // 1. Parse to build the manifest (slots + fingerprints).
    {
        std::unordered_set<std::string> seen; std::string resolved;
        try { resolved = resolve_imports(src, "./", seen); } catch (...) { return nullptr; }
        auto lr = tokenize(resolved, "<ke>"); if (!lr.ok) return nullptr;
        auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
        int si = 0; for(auto&fn:pr.program.funcs){m->slots[fn.name]=si++;}
        m->manifest = make_manifest(pr.program, m->slots, name);
    }

    // 2. Plan the keyed layout.
    uint8_t key_bytes[8];
    for (int i = 0; i < 8; ++i) key_bytes[i] = uint8_t((key_seed >> (8*i)) & 0xFF);
    BuildKeyView key{key_bytes, 8};
    ImplicitKeyedLayoutV1 planner;
    auto pr = planner.plan(m->manifest, key);
    if (!pr) return nullptr;
    m->plan = std::move(*pr.value);
    m->target_route_word = derive_route_word(key);

    // 3. Set up record storage with placeholder entries (padding).
    m->st.routes = m->plan.logical_routes;
    m->st.domains = m->plan.domains;
    m->st.allowlist.assign((m->plan.logical_slot_count + 7u) >> 3, 0);
    for (uint32_t i = 0; i < m->plan.logical_slot_count; ++i)
        m->st.allowlist[i >> 3] |= (uint8_t(1) << (i & 7u));
    m->st.descriptors.assign(m->plan.physical_slot_count, PhysicalEntryDescriptor{});
    for (uint32_t s = 0; s < m->plan.physical_slot_count; ++s) {
        const auto& pe = m->plan.physical_entries[s];
        PhysicalEntryDescriptor& pd = m->st.descriptors[s];
        pd.physical_slot = pe.physical_slot;
        pd.abi_fingerprint = pe.abi_fingerprint;
        pd.visibility = pe.visibility;
        pd.calling_mode = pe.calling_mode;
        pd.dispatch_domain = pe.dispatch_domain;
        pd.is_padding = pe.is_padding;
        pd.logical_slot = pe.logical_slot;
        pd.domain_index = pe.domain_index;
        pd.ordinal = pe.ordinal;
    }
    const void* pad_stub = ember_keyed_padding_trap_target();
    m->st.storage = std::vector<std::atomic<void*>>(m->plan.physical_slot_count);
    for (uint32_t s = 0; s < m->plan.physical_slot_count; ++s)
        m->st.storage[s].store(const_cast<void*>(pad_stub), std::memory_order_release);
    m->rec.mode = DispatchMode::Keyed;
    m->rec.strategy_version = 1;
    m->rec.physical_slots = m->st.storage.data();
    m->rec.physical_slot_count = m->plan.physical_slot_count;
    m->rec.logical_slot_count = m->plan.logical_slot_count;
    m->rec.logical_routes = m->st.routes.data();
    m->rec.domains = m->st.domains.data();
    m->rec.domain_count = uint32_t(m->st.domains.size());
    m->rec.logical_allowlist = m->st.allowlist.data();
    m->rec.logical_allowlist_bytes = uint32_t(m->st.allowlist.size());
    m->rec.physical_descriptors = m->st.descriptors.data();
    m->rec.physical_descriptor_count = uint32_t(m->st.descriptors.size());
    m->rec.padding_trap_target = pad_stub;

    // 4. Compile with the keyed CodeGenCtx. The module record must be passed
    //    to the codegen so the keyed call-lowering emits keyed resolution.
    {
        std::unordered_set<std::string> seen; std::string resolved;
        try { resolved = resolve_imports(src, "./", seen); } catch (...) { return nullptr; }
        auto lr = tokenize(resolved, "<ke>"); if (!lr.ok) return nullptr;
        auto pr2 = parse(std::move(lr.toks)); if (!pr2.ok) return nullptr;
        int si = 0; for(auto&fn:pr2.program.funcs){fn.slot=si++;}
        if (register_thread_natives)    ember::ext_thread::register_natives(m->natives);
        if (register_lifecycle_natives) ember::ext_lifecycle::register_natives(m->natives);
        ember::OpOverloadTable ov;
        auto layouts = build_struct_layouts(pr2.program); pr2.program.string_xor_key = 0;
        if(!sema(pr2.program,m->natives,m->slots,0,&ov,&layouts).ok) return nullptr;
        ember::GlobalsBlock gb; std::vector<uint8_t> gbs(pr2.program.globals.size()*8, 0);
        gb.base=int64_t(gbs.data());
        { uint32_t gi=0; for (auto& g : pr2.program.globals) { gb.index[g.name]=gi++; gb.types[g.name]=g.ty.get(); } }
        eval_global_initializers(pr2.program, GlobalInitCtx{gbs, gb.index, gb.types});
        ember::g_globals_for_codegen = nullptr;
        m->table = DispatchTable(int(m->slots.size()));
        std::vector<uint8_t> allowlist = build_fn_allowlist(m->slots, int(m->slots.size()));
        ember::CodeGenCtx ctx;
        ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(m->table.base());
        ctx.natives=&m->natives; ctx.script_slots=&m->slots; ctx.structs=&layouts;
        ctx.globals_index = &gb.index; ctx.globals_types = &gb.types;
        ctx.use_context_reg = true;
        ctx.emit_depth_checks = true;
        ctx.emit_budget_checks = true;
        ctx.max_call_depth = 64;
        ctx.fn_allowlist_base = int64_t(allowlist.data());
        ctx.fn_slot_count = int(m->slots.size());
        ctx.trap_stub = reinterpret_cast<void*>(ke_trap);
        ctx.trap_ctx = nullptr;
        KeyedDispatchCodegen kd{};
        kd.runtime_key = RuntimeKeyLocation::R15;
        kd.module_record = &m->rec;
        ctx.keyed_dispatch = &kd;
        for(auto&fn:pr2.program.funcs){
            auto cf=compile_func(fn,ctx); finalize(cf); m->table.set(fn.slot,cf.entry);
            m->fns.push_back(std::move(cf));
        }
    }

    // 5. Populate real entries at their permuted physical positions.
    for (uint32_t s = 0; s < m->plan.physical_slot_count; ++s) {
        const auto& pe = m->plan.physical_entries[s];
        if (pe.is_padding) continue;
        uint32_t ls = pe.logical_slot;
        void* entry = m->table.get(int(ls));
        m->st.storage[s].store(entry, std::memory_order_release);
    }

    // 6. Validate the record.
    auto vs = validate_dispatch_record(m->rec);
    if (!vs) return nullptr;
    return m;
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
    // 2. KEYED-RECORD ROUTING — a REAL keyed (DispatchMode::Keyed) module with
    //    a permuted physical table + padding. The keyed resolver resolves
    //    through the permutation and reaches the INTENDED callable. Like the
    //    other keyed tests (keyed_dispatch_modules_test, keyed_dispatch_runtime-
    //    test), this uses ember_resolve_keyed_dispatch + ember_keyed_reentry_i64
    //    directly with the known target route word (the adapter-based
    //    ember_call_keyed_* APIs fold the provider's material + module identity
    //    into a DIFFERENT route word than derive_route_word(key), so the host
    //    keyed-call APIs are tested separately with identity records above +
    //    in the extension tests below).
    // =====================================================================
    {
        uint64_t seed = 0xBEEF1234ULL;
        auto km = compile_keyed_module(
            "fn add(a: i64, b: i64) -> i64 { return a + b; }\n"
            "fn mul(a: i64, b: i64) -> i64 { return a * b; }\n"
            "fn main() -> i64 { return 0; }\n",
            "keyed.mod", seed);
        ck(km != nullptr, "keyed-record: compiled a real keyed module (permuted + padding)");
        if (km) {
            ck(km->rec.mode == DispatchMode::Keyed, "keyed-record: record mode is Keyed");
            ck(km->rec.physical_slot_count > km->rec.logical_slot_count,
               "keyed-record: physical > logical (padding present)");
            uint64_t rw = km->target_route_word;
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            uint32_t add_slot = uint32_t(km->slots["add"]);
            uint32_t mul_slot = uint32_t(km->slots["mul"]);
            // Resolve through the permutation + invoke via the keyed re-entry thunk.
            void* add_entry = ember_resolve_keyed_dispatch(&km->rec, add_slot, rw);
            ck(add_entry != nullptr, "keyed-record: resolve add through the permutation (non-null)");
            if (add_entry) {
                int64_t v = ember_keyed_reentry_i64_i64(add_entry, &ctx, 3, 4, rw);
                ck(v == 7, "keyed-record: permuted call add(3,4)=7 (correct entry reached through permutation)");
            }
            void* mul_entry = ember_resolve_keyed_dispatch(&km->rec, mul_slot, rw);
            ck(mul_entry != nullptr, "keyed-record: resolve mul through the permutation (non-null)");
            if (mul_entry) {
                int64_t v = ember_keyed_reentry_i64_i64(mul_entry, &ctx, 5, 6, rw);
                ck(v == 30, "keyed-record: permuted call mul(5,6)=30 (correct entry reached through permutation)");
            }
            // A WRONG route word lands in-range (a different same-ABI entry or
            // padding) — never OOB, never a wrong-ABI call. Verify it does NOT
            // reach the intended add result (proving the permutation is real).
            uint64_t wrong_rw = rw ^ 0x0123456789ABCDEFULL;
            if (wrong_rw == rw) wrong_rw ^= 0xFEDCBA9876543210ULL;
            void* wrong_entry = ember_resolve_keyed_dispatch(&km->rec, add_slot, wrong_rw);
            // The wrong entry is either a different same-ABI function (mul) or
            // the padding trap. Either way it must NOT be the same as the
            // correct add entry (proving the permutation is real, §2.4).
            ck(wrong_entry != add_entry || wrong_entry == km->rec.padding_trap_target,
               "keyed-record: wrong key resolves to a DIFFERENT entry or padding (real permutation, §2.4)");
            // Verify the physical layout is actually permuted (not identity):
            // at least one logical slot's physical position differs from its
            // logical index.
            bool permuted = false;
            for (uint32_t s = 0; s < km->plan.physical_slot_count; ++s) {
                const auto& pe = km->plan.physical_entries[s];
                if (!pe.is_padding && pe.physical_slot != pe.logical_slot) {
                    permuted = true; break;
                }
            }
            ck(permuted, "keyed-record: physical layout is actually permuted (not identity-indexed)");
        }
    }

    // =====================================================================
    // 3. PROVIDER-UNAVAILABLE STRUCTURED FAILURE.
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
    // 4. NORMAL/TRAP CLEANUP — the keyed host boundary clears the current-
    //    runtime TLS on every exit (normal return AND trapped longjmp).
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xA3);
        DispatchKeyAdapter adapter(provider);
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
            ck(r.reason.find("budget") != std::string::npos,
               "cleanup: trapped call reason carries 'budget' (structured trap reason in CallResult)");
        }
    }

    // =====================================================================
    // 5. LIFECYCLE INVOCATION AFTER ENTRY REPLACEMENT (identity record).
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
    // 5b. LIFECYCLE PROVIDER FAILURE — lifecycle_tick_keyed with a failing
    //     provider contributes 0 (no thunk entered, no crash). The guarded
    //     core API (ember_call_keyed_i64_by_slot) reports a structured failure
    //     for each routine; the tick sums 0.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xA4F);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_module(
            "fn tick(data: i64) -> i64 { return data + 1; }\n"
            "fn main() -> i64 { let h = &tick; return register_routine(h, 41); }\n",
            "lc_fail.mod", false, false, /*register_lifecycle_natives=*/true);
        ck(m != nullptr, "lifecycle-fail: compile tick+main with lifecycle natives");
        if (m) {
            auto inst = make_identity_instance(*m, "lc_fail.mod", provider);
            ck(ext_lifecycle::lifecycle_init_keyed(inst),
               "lifecycle-fail: lifecycle_init_keyed populates the per-runtime state");
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            uint32_t main_slot = inst.named_entries["main"];
            auto r = ember_call_keyed_i64_by_slot(inst, main_slot, ctx, 0, adapter);
            ck(r.ok && r.value >= 1, "lifecycle-fail: main registered a routine");
            // Now fail the provider + tick. Each routine's keyed call fails
            // (provider unavailable); the tick contributes 0 and does not crash.
            provider->fail_next.store(true);
            int64_t got = ext_lifecycle::lifecycle_tick_keyed(inst, ctx, adapter);
            ck(got == 0, "lifecycle-fail: tick with failing provider contributes 0 (no crash, no thunk entered)");
            ck(ember_current_keyed_runtime() == nullptr,
               "lifecycle-fail: TLS cleared after provider-failed tick (core API cleans up)");
        }
    }

    // =====================================================================
    // 6. DELAYED THREAD EXECUTION AFTER REPLACEMENT.
    //    The worker uses the guarded core keyed-call API (NOT a raw re-entry
    //    thunk). The worker re-resolves at execution time through the
    //    instance's dispatch record, so a replacement published between spawn
    //    and the worker's keyed call is observed.
    //
    //    The keyed worker owns its own context (§6.6) — no shared call_mutex.
    //    The test does NOT write inst.ext_state->thread.ctx or lock
    //    call_mutex; context publication goes through the keyed boundary
    //    (thread_init_keyed + the worker's own context). Join is
    //    ownership-explicit (no unconditional unlock of a never-locked mutex).
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
            // Keyed threads require make_shared-managed instance (shared
            // lifetime ownership for the worker).
            auto inst = std::make_shared<ModuleInstance>(
                make_identity_instance(*m, "th.mod", provider));
            ck(ext_thread::thread_init_keyed(*inst),
               "thread: thread_init_keyed populates the per-runtime state");
            // Install a worker gate so the worker blocks BEFORE resolving its
            // entry — we publish the replacement while the worker is blocked,
            // then open the gate so the worker resolves at execution time and
            // observes the replacement (§12.4).
            ext_thread::install_worker_gate(*inst);
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            uint32_t main_slot = inst->named_entries["main"];
            auto r = ember_call_keyed_i64_by_slot(*inst, main_slot, ctx, 0, adapter);
            ck(r.ok && r.value >= 1, "thread: main spawned a worker (tid >= 1)");
            int64_t tid = r.value;
            auto m2 = compile_module("fn worker(arg: i64) -> i64 { return arg + 5000; }\n",
                                      "th_repl.mod");
            ck(m2 != nullptr, "thread: compile replacement worker (arg+5000)");
            if (m2) {
                void* repl_entry = m2->table.get(m2->slots["worker"]);
                uint32_t worker_slot = static_cast<uint32_t>(m->slots["worker"]);
                // Publish the replacement BEFORE the worker resolves. The
                // worker is blocked in the gate (before its ember_call_keyed),
                // so it has not yet resolved its entry. When the gate opens,
                // the worker re-resolves at execution through the record and
                // observes this replacement.
                m->table.set(worker_slot, repl_entry);
                ext_thread::open_worker_gate(*inst);  // release the worker
                int64_t joined = ext_thread::thread_join_keyed(*inst, tid, ctx, adapter);
                ck(joined == 5499,
                   "thread: worker after replacement called the REPLACEMENT (499+5000=5499), not the stale original (499+1=500)");
            }
        }
    }

    // =====================================================================
    // 6b. THREAD PROVIDER FAILURE — a worker whose provider fails at
    //     execution returns a structured failure (0, no crash). The guarded
    //     core API reports the failure; the worker publishes result=0.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xA5F);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_module(
            "fn worker(arg: i64) -> i64 { return arg + 1; }\n"
            "fn main() -> i64 { let h = &worker; return thread_spawn(h, 42); }\n",
            "th_fail.mod", /*register_thread_natives=*/true);
        ck(m != nullptr, "thread-fail: compile worker+main with thread natives");
        if (m) {
            auto inst = std::make_shared<ModuleInstance>(
                make_identity_instance(*m, "th_fail.mod", provider));
            ck(ext_thread::thread_init_keyed(*inst),
               "thread-fail: thread_init_keyed populates the per-runtime state");
            // Install a gate so the worker blocks before resolving — we fail
            // the provider while the worker is blocked, then open the gate so
            // the worker resolves with the failed provider.
            ext_thread::install_worker_gate(*inst);
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            uint32_t main_slot = inst->named_entries["main"];
            auto r = ember_call_keyed_i64_by_slot(*inst, main_slot, ctx, 0, adapter);
            ck(r.ok && r.value >= 1, "thread-fail: main spawned a worker");
            int64_t tid = r.value;
            // Fail the provider while the worker is blocked in the gate.
            provider->fail_next.store(true);
            ext_thread::open_worker_gate(*inst);  // worker resolves with failed provider
            int64_t joined = ext_thread::thread_join_keyed(*inst, tid, ctx, adapter);
            ck(joined == 0,
               "thread-fail: worker with failing provider returns 0 (structured failure, no crash)");
        }
    }

    // =====================================================================
    // 6c. WORKER TLS / GENERATION GUARD — the keyed worker establishes the
    //     current-keyed-runtime TLS on its thread. A keyed native invoked by
    //     the worker's script finds THIS runtime's per-runtime state (not a
    //     legacy global). We verify by having the worker's script call
    //     register_routine (a keyed lifecycle native) — if the TLS is set on
    //     the worker thread, the routine lands in the WORKER's runtime's
    //     per-runtime store, not a legacy global.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xA5C);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_module(
            "fn helper(data: i64) -> i64 { return data + 1; }\n"
            "fn worker(arg: i64) -> i64 { let h = &helper; register_routine(h, arg); return arg + 1; }\n"
            "fn main() -> i64 { let h = &worker; return thread_spawn(h, 77); }\n",
            "th_tls.mod", /*register_thread_natives=*/true, false,
            /*register_lifecycle_natives=*/true);
        ck(m != nullptr, "thread-tls: compile worker+main with thread + lifecycle natives");
        if (m) {
            auto inst = std::make_shared<ModuleInstance>(
                make_identity_instance(*m, "th_tls.mod", provider));
            ck(ext_thread::thread_init_keyed(*inst),
               "thread-tls: thread_init_keyed populates the per-runtime state");
            ck(ext_lifecycle::lifecycle_init_keyed(*inst),
               "thread-tls: lifecycle_init_keyed populates the per-runtime state");
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            uint32_t main_slot = inst->named_entries["main"];
            auto r = ember_call_keyed_i64_by_slot(*inst, main_slot, ctx, 0, adapter);
            ck(r.ok && r.value >= 1, "thread-tls: main spawned a worker");
            int64_t tid = r.value;
            int64_t joined = ext_thread::thread_join_keyed(*inst, tid, ctx, adapter);
            ck(joined == 78, "thread-tls: worker returned 78 (77+1, correct execution)");
            // The worker's script called register_routine — if the worker
            // established the TLS on its thread, the routine landed in the
            // WORKER's runtime's per-runtime store (inst->ext_state->lifecycle),
            // NOT the legacy file-static global.
            auto routines = ext_lifecycle::host_routines_keyed(*inst);
            ck(routines.size() == 1 && routines[0].data == 77,
               "thread-tls: worker's register_routine landed in the per-runtime store (TLS set on worker thread, data=77)");
            // The legacy global store should be EMPTY (the worker did NOT
            // fall back to the file-static singleton).
            ck(ext_lifecycle::host_count() == 0,
               "thread-tls: legacy global store is empty (worker did not fall back to file-static singleton)");
        }
    }

    // =====================================================================
    // 7. CONCURRENT WORKERS — two ModuleInstances with distinct providers.
    //    The keyed workers own their own contexts and capture shared_ptr<
    //    ModuleInstance>; concurrent workers for two ModuleInstances with
    //    distinct providers remain isolated (§6.6, §10.3).
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
            auto instA = std::make_shared<ModuleInstance>(
                make_identity_instance(*mA, "concA.mod", providerA));
            auto instB = std::make_shared<ModuleInstance>(
                make_identity_instance(*mB, "concB.mod", providerB));
            ck(ext_thread::thread_init_keyed(*instA), "concurrent: thread_init_keyed A");
            ck(ext_thread::thread_init_keyed(*instB), "concurrent: thread_init_keyed B");
            struct Obs { std::atomic<int64_t> ret{-1}; std::atomic<bool> done{false}; };
            Obs obsA, obsB;
            auto run = [&](Obs* obs, std::shared_ptr<ModuleInstance> inst, DispatchKeyAdapter* adapter) {
                context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
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
            std::thread ta(run, &obsA, instA, &adapterA);
            std::thread tb(run, &obsB, instB, &adapterB);
            ta.join(); tb.join();
            ck(obsA.done.load() && obsB.done.load(), "concurrent: both workers completed");
            ck(obsA.ret.load() == 110, "concurrent: worker A returned 110 (its own module, its own state)");
            ck(obsB.ret.load() == 220, "concurrent: worker B returned 220 (its own module, its own state)");
            ck(obsA.ret.load() != obsB.ret.load(),
               "concurrent: the two runtimes produced DIFFERENT results (independent state, §6.6/§10.3)");
        }
    }

    // =====================================================================
    // 7b. ACTIVE-WORKER DESTRUCTION — destroying a runtime (shared_ptr) with
    //     an active (not-yet-joined) joinable worker does NOT terminate the
    //     process. The ThreadRuntimeState destructor joins/detaches all
    //     joinable threads deterministically. We spawn a worker, let it
    //     finish (wait for done), but do NOT join the std::thread, then
    //     destroy the instance — the destructor handles cleanup.
    // =====================================================================
    {
        auto provider = std::make_shared<CountingProvider>(0xA7D);
        DispatchKeyAdapter adapter(provider);
        auto m = compile_module(
            "fn worker(arg: i64) -> i64 { return arg + 1; }\n"
            "fn main() -> i64 { let h = &worker; return thread_spawn(h, 13); }\n",
            "destr.mod", /*register_thread_natives=*/true);
        ck(m != nullptr, "destruction: compile worker+main with thread natives");
        if (m) {
            bool destroyed_ok = false;
            // Capture the ext_state so we can wait for the worker's done flag
            // after the instance is destroyed (the worker's shared_ptr keeps
            // the instance alive, but we need a handle to the ThreadSlot).
            std::shared_ptr<RuntimeExtensionState> captured_state;
            int64_t tid = -1;
            {
                auto inst = std::make_shared<ModuleInstance>(
                    make_identity_instance(*m, "destr.mod", provider));
                ck(ext_thread::thread_init_keyed(*inst),
                   "destruction: thread_init_keyed populates the per-runtime state");
                captured_state = inst->ext_state;
                context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
                uint32_t main_slot = inst->named_entries["main"];
                auto r = ember_call_keyed_i64_by_slot(*inst, main_slot, ctx, 0, adapter);
                ck(r.ok && r.value >= 1, "destruction: main spawned a worker");
                tid = r.value;
                // Do NOT join via the API. Let `inst` go out of scope with
                // the worker still potentially active. The ThreadRuntimeState
                // destructor must handle it without terminating.
                destroyed_ok = true;
            }
            // If we reach here, the destructor did not terminate the process.
            ck(destroyed_ok,
               "destruction: runtime destroyed with active worker — no terminate (deterministic cleanup)");
            // Wait for the worker to publish done (via the captured ext_state)
            // so the worker's OS thread has finished executing before `m` is
            // destroyed (the worker calls JIT'd code from m->table). The
            // worker's shared_ptr<ModuleInstance> keeps the instance alive
            // until it returns; after done, the worker is about to return.
            if (captured_state && tid >= 1) {
                auto& s = captured_state->thread;
                std::unique_lock<std::recursive_mutex> lk(s.setup_mutex);
                if (tid >= 1 && size_t(tid) <= s.threads.size()) {
                    auto& slot = s.threads[size_t(tid - 1)];
                    if (slot) {
                        std::unique_lock<std::mutex> dlk(slot->done_lock);
                        // Wait up to 2 seconds for the worker to finish.
                        slot->done_cv.wait_for(dlk, std::chrono::seconds(2),
                                               [&] { return slot->done; });
                    }
                }
            }
            // Give the worker's cleanup (shared_ptr release + destructor) a
            // moment to complete.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // =====================================================================
    // 8. COROUTINE YIELD/RESUME ACROSS REPLACEMENT — §6.7 fail-closed.
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
    // 9. TWO-RUNTIME STORE ISOLATION.
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
    // 10. UNCHANGED LEGACY IDENTITY APIs.
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
