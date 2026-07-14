// keyed_dispatch_modules_test — Red 7
// (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §9.7, §10, §14.2, §14.3):
// the module registry + cross-module call gate for keyed dispatch.
//
// RED-GREEN contract chunk for the module registry + cross-module calls. This
// is the RED side (written first) of the Red 7 contract. It pins, against real
// JIT-compiled keyed + identity modules whose dispatch records are published
// through the registry's atomic generation pointer, the §9.7 / §10 / §14.2 /
// §14.3 mandatory buckets:
//
//   - atomic publication:       the registry publishes a complete immutable
//                               ModuleDispatchRecord generation with ONE
//                               release/acquire atomic pointer update; readers
//                               acquire-load one coherent generation (table +
//                               allowlist + counts + mode together, never a
//                               partial combination).
//   - stable module IDs:        registering the same name again (reload) updates
//                               the record/table for the EXISTING id — the id
//                               does not change, so callers that cached
//                               (module_id, logical_slot) pick up the new
//                               generation on the next call.
//   - separate counts:          the registry exposes separate logical and
//                               physical slot counts; a keyed module's
//                               physical_count > logical_count (padding), an
//                               identity module's physical == logical.
//   - keyed→keyed direct:       a keyed caller's `mod::fn()` cross-module call
//                               retains (module_id, logical_slot) identity and
//                               resolves the target's CURRENT record at call
//                               time using the caller's transient r15 route
//                               word; the target's domain salt + the caller's
//                               r15 produce the target-specific physical
//                               resolution. No handle/reloc/import/export
//                               contains a resolved physical index.
//   - keyed→identity direct:    a keyed caller may call a legacy identity
//                               target through the legacy registry-hop sequence
//                               (the keyed caller has r15 but does not use it
//                               for the identity target).
//   - legacy→identity direct:   the legacy caller→identity target path is
//                               byte-for-byte unchanged (the existing kind-2
//                               registry-hop sequence).
//   - legacy→keyed rejected:    a legacy caller (no keyed_dispatch / no r15
//                               route word) linking to a keyed target is
//                               rejected at code-generation time — the emit
//                               produces a trap (the call site never dispatches
//                               into a keyed physical table as if it were
//                               logical).
//   - correct/wrong route words: under the correct route word a keyed→keyed
//                               call resolves to the correct target entry and
//                               returns the correct value; under a wrong route
//                               word the resolved entry is non-null, in-range,
//                               same-ABI-domain (alternate real or padding), no
//                               crash, no AV, call_depth balanced. The padding
//                               trap fires TrapReason::KeyedDispatchPadding.
//   - same-ABI/padding safety:  a wrong route word that lands on the padding
//                               ordinal is a non-null same-ABI trap stub that
//                               fires KeyedDispatchPadding; an alternate-real
//                               route runs a different same-domain callable
//                               (no crash, no StackOverflow for a non-recursive
//                               alternate).
//   - cross-module handles:     a `&mod::fn` cross-module handle retains
//                               (module_id, logical_slot) identity (the handle
//                               is (1<<63)|(module_id<<32)|logical_slot, NEVER
//                               a physical index); calling it from a keyed
//                               caller resolves the target's current record at
//                               call time using the caller's r15.
//   - replacement keeps ID:     replacing a same-name registry entry (reload)
//                               publishes a new record generation without
//                               changing the module ID; concurrent readers
//                               observe only coherent generations (the old or
//                               the new, never a mix).
//   - concurrent readers:       multiple readers acquire-load the record
//                               concurrently while a writer publishes a new
//                               generation; every reader observes a coherent
//                               generation (the record's mode + counts + table
//                               + allowlist are internally consistent).
//   - unsupported mode:         a module whose record declares an unsupported
//                               strategy_version or mode is rejected by
//                               validate_dispatch_record before publication.
//   - logical/physical mismatch: a record whose logical_count > physical_count
//                               (or physical_count < logical_count for keyed)
//                               is rejected by validate_dispatch_record.
//   - legacy identity behavior: when keyed mode is disabled (no keyed_dispatch),
//                               the registry + cross-module path is byte-for-
//                               byte identical to the pre-Red-7 path; the
//                               dispatch_slot_count / handle_records / entries_
//                               arrays are unchanged.
//
// The fixture compiles keyed + identity JIT modules, publishes their dispatch
// records through the registry, wires the exports + keyed CodeGenCtx, and runs
// the caller's main through the keyed re-entry thunk (r14=ctx + r15=route_word).
//
// Links ember (keyed_dispatch.* — Red 1, context.hpp, engine.* — Red 5 thunks,
// dispatch_table.hpp, module_registry.* — Red 7 atomic publication) +
// ember_frontend (module_layout.* — Red 3/4, module_instance.hpp — Red 5,
// codegen.* — the keyed CodeGenCtx + cross-module lowering, thin_lower.*,
// thin_emit.*, dispatch_abi.*, module_linker.hpp). NOT a CTest entry: the
// filtered suite count must stay 67 (§14.1); the target building cleanly + the
// executable passing IS the gate.

#include "../src/engine.hpp"
#include "../src/context.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/keyed_dispatch.hpp"
#include "../src/module_layout.hpp"
#include "../src/module_instance.hpp"
#include "../src/module_registry.hpp"
#include "../src/module_linker.hpp"
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
// Fixed-material provider — always returns the same 32-byte material, so the
// derived route word is deterministic.
// ===========================================================================
struct FixedProvider : public DerivedMaterialProvider {
    std::array<uint8_t, 32> material{};
    explicit FixedProvider(uint64_t route_word_seed) {
        material.fill(0);
        for (int i = 0; i < 8; ++i)
            material[i] = uint8_t((route_word_seed >> (8 * i)) & 0xFF);
        for (int i = 8; i < 32; ++i)
            material[i] = uint8_t(0x42 + i);
    }
    ExtensionResult<std::array<uint8_t, 32>> derive(const DerivationRequest&) const override {
        return make_extension_result_ok(material);
    }
};

// ===========================================================================
// ABI classifier helpers (ported from keyed_dispatch_codegen_test).
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
// A compiled module (keyed or identity). Owns its compiled functions, dispatch
// table, keyed record storage, and the allowlist. For a keyed module, the
// ModuleDispatchRecord is built + validated + published through the registry.
// For an identity module, no record is published (the legacy raw-table path).
// ===========================================================================
struct CompiledModule {
    std::string name;
    std::vector<CompiledFn> fns;
    std::vector<CompileBackend> backends;
    DispatchTable table;
    std::unordered_map<std::string, int> slots;
    std::unordered_map<std::string, NativeSig> natives;
    void* main_entry = nullptr;
    std::vector<uint8_t> allowlist;
    int slot_count = 0;
    uint32_t module_id = UINT32_MAX;
    // Keyed record storage (only for keyed modules).
    ModuleManifest manifest;
    ModuleLayoutPlan plan;
    RecordBuilderStorage st;
    ModuleDispatchRecord rec{};
    uint64_t route_word = 0;
    bool keyed = false;
    ember::context_t ctx{};
    ~CompiledModule() { for (auto& fn : fns) if (fn.exec) free_executable(fn.exec); }
};

// ===========================================================================
// Compile a module from source. `keyed` selects keyed vs identity mode.
// `ir_backend` selects the Thin IR vs tree-walker backend.
// `module_record` is the keyed record (for keyed mode; null for identity).
// `exports` is the ModuleExportTable for cross-module resolution.
// `registry` is the per-process registry (for registry_base + records base).
// `register_with_handles` = publish this module's allowlist for cross-module
// handles. `trap_stub` is the host trap stub.
// ===========================================================================
static std::unique_ptr<CompiledModule> compile_module(
    const std::string& src, const std::string& name,
    bool keyed, bool ir_backend,
    const ModuleDispatchRecord* module_record,
    const ModuleExportTable* exports,
    ModuleRegistry* registry,
    bool register_with_handles,
    void* trap_stub = nullptr) {

    auto m = std::make_unique<CompiledModule>();
    m->name = name;
    m->keyed = keyed;

    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); } catch (...) { return nullptr; }
    auto lr = tokenize(resolved, "<km>"); if (!lr.ok) return nullptr;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
    int si = 0; for(auto&fn:pr.program.funcs){m->slots[fn.name]=si++;fn.slot=m->slots[fn.name];}
    m->slot_count = si;
    ember::OpOverloadTable ov;
    auto layouts = build_struct_layouts(pr.program); pr.program.string_xor_key = 0;
    auto sr = sema(pr.program, m->natives, m->slots, 0, &ov, &layouts, exports);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors) for %s:\n", sr.errors.size(), name.c_str());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return nullptr;
    }

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
    ctx.max_call_depth = 64;
    ctx.fn_allowlist_base = int64_t(m->allowlist.data());
    ctx.fn_slot_count = si;
    if (trap_stub) { ctx.trap_stub = trap_stub; ctx.trap_ctx = &m->ctx; }
    if (registry) {
        ctx.registry_base = int64_t(registry->base());
        ctx.module_handle_records_base = int64_t(registry->handle_records_base());
        ctx.module_handle_records_count = int64_t(registry->handle_records_count());
        ctx.module_dispatch_records_base = int64_t(registry->dispatch_records_base());
    }
    if (keyed) {
        KeyedDispatchCodegen kd{};
        kd.runtime_key = RuntimeKeyLocation::R15;
        kd.module_record = module_record;
        ctx.keyed_dispatch = &kd;
    }
    ctx.enable_ir_backend = ir_backend;

    m->ctx.budget_remaining = 2'000'000'000LL; m->ctx.max_call_depth = 64; m->ctx.call_depth = 0;

    for(auto&fn:pr.program.funcs){
        auto cr = compile_func_checked(fn, ctx); finalize(cr.compiled);
        auto& cf = cr.compiled;
        if (!cf.entry) { std::printf("FAIL: null entry for %s in %s\n", fn.name.c_str(), name.c_str()); return nullptr; }
        m->table.set(fn.slot, cf.entry);
        m->backends.push_back(cr.backend);
        m->fns.push_back(std::move(cf));
    }
    auto sit = m->slots.find("main");
    m->main_entry = (sit != m->slots.end()) ? m->table.get(sit->second) : nullptr;
    return m;
}

// ===========================================================================
// Build a complete keyed module: parse + manifest + compile + plan + record.
// The manifest is built from the source before compilation; the record storage
// is populated with placeholder entries (padding) so the record address is
// stable for compilation; real entries are stored after compilation.
// ===========================================================================
static std::unique_ptr<CompiledModule> build_keyed_module(
    const std::string& src, const std::string& name,
    uint64_t route_word_seed, bool ir_backend,
    const ModuleExportTable* exports,
    ModuleRegistry* registry,
    bool register_with_handles,
    void* trap_stub = nullptr) {

    auto m = std::make_unique<CompiledModule>();
    m->name = name;
    m->keyed = true;

    // 1. Parse to build the manifest (slots + fingerprints).
    {
        std::unordered_set<std::string> seen; std::string resolved;
        try { resolved = resolve_imports(src, "./", seen); } catch (...) { return nullptr; }
        auto lr = tokenize(resolved, "<km>"); if (!lr.ok) return nullptr;
        auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
        std::unordered_map<std::string,int> slots; int si=0;
        for(auto&fn:pr.program.funcs){slots[fn.name]=si++;}
        m->manifest = make_manifest(pr.program, slots, name);
    }

    // 2. Plan the keyed layout to get the record shape.
    uint8_t key_bytes[8];
    for (int i = 0; i < 8; ++i) key_bytes[i] = uint8_t((route_word_seed >> (8*i)) & 0xFF);
    BuildKeyView key{key_bytes, 8};
    ImplicitKeyedLayoutV1 planner;
    auto pr = planner.plan(m->manifest, key);
    if (!pr) { std::printf("FAIL: keyed plan failed for %s\n", name.c_str()); return nullptr; }
    m->plan = std::move(*pr.value);
    m->route_word = derive_route_word(key);

    // 3. Set up record storage with placeholder entries (padding trap).
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

    // 4. Compile with the keyed CodeGenCtx (module_record = &rec).
    auto compiled = compile_module(src, name, /*keyed=*/true, ir_backend,
                                    &m->rec, exports, registry, register_with_handles, trap_stub);
    if (!compiled) { std::printf("FAIL: compile_module returned null for %s\n", name.c_str()); return nullptr; }

    // Move the compiled bits into m (keep the record storage from m).
    m->fns = std::move(compiled->fns);
    m->backends = std::move(compiled->backends);
    m->table = std::move(compiled->table);
    m->slots = std::move(compiled->slots);
    m->natives = std::move(compiled->natives);
    m->main_entry = compiled->main_entry;
    m->allowlist = std::move(compiled->allowlist);
    m->slot_count = compiled->slot_count;
    // context_t is non-copyable (has a mutex); re-initialize m->ctx's
    // budget/depth fields to match the compiled module's defaults.
    m->ctx.budget_remaining = compiled->ctx.budget_remaining;
    m->ctx.max_call_depth = compiled->ctx.max_call_depth;
    m->ctx.call_depth = compiled->ctx.call_depth;

    // 5. Populate real entries.
    for (uint32_t s = 0; s < m->plan.physical_slot_count; ++s) {
        const auto& pe = m->plan.physical_entries[s];
        if (pe.is_padding) continue;
        uint32_t ls = pe.logical_slot;
        void* entry = m->table.get(int(ls));
        m->st.storage[s].store(entry, std::memory_order_release);
    }

    // 6. Validate the record.
    auto vs = validate_dispatch_record(m->rec);
    if (!vs) {
        std::printf("FAIL: validate_dispatch_record failed for %s: %s\n",
                    name.c_str(), vs.error.has_value() ? vs.error->message.c_str() : "(no diag)");
        return nullptr;
    }
    return m;
}

// ===========================================================================
// Build an identity (legacy) module. No keyed record; the raw dispatch table
// is registered directly.
// ===========================================================================
static std::unique_ptr<CompiledModule> build_identity_module(
    const std::string& src, const std::string& name,
    bool ir_backend,
    const ModuleExportTable* exports,
    ModuleRegistry* registry,
    bool register_with_handles,
    void* trap_stub = nullptr) {
    return compile_module(src, name, /*keyed=*/false, ir_backend,
                          /*module_record=*/nullptr, exports, registry,
                          register_with_handles, trap_stub);
}

// ===========================================================================
// Build the ModuleExportTable for a module (one export per non-private fn,
// carrying slot + module_id + signature + dispatch_mode). The dispatch_mode
// is Identity for a legacy module and Keyed for a keyed module.
// ===========================================================================
static void add_module_exports(ModuleExportTable& table, const std::string& src,
                               const std::string& alias, uint32_t module_id,
                               DispatchMode mode) {
    auto lr = tokenize(src, "<x>"); auto pr = parse(std::move(lr.toks));
    int si = 0;
    for (auto& fn : pr.program.funcs) { fn.slot = si++; }
    std::vector<ModuleExport> exps;
    for (const auto& fn : pr.program.funcs) {
        if (!fn.is_exported) continue;
        ModuleExport exp;
        exp.fn_name = fn.name; exp.module_id = module_id; exp.slot = fn.slot;
        exp.ret = fn.ret ? *fn.ret : Type{};
        for (const auto& p : fn.params) exp.params.push_back(p.ty ? *p.ty : Type{});
        exp.dispatch_mode = mode;
        exps.push_back(std::move(exp));
    }
    table[alias] = std::move(exps);
}

// ===========================================================================
// Register a module into the registry. For a keyed module, publish its
// dispatch record + set the logical/physical counts. For an identity module,
// register the raw table + set the single count (legacy path).
// ===========================================================================
static bool register_module(ModuleRegistry& reg, CompiledModule& m,
                            bool register_with_handles, std::string* err) {
    if (register_with_handles) {
        m.module_id = reg.register_module(m.name, m.table.base(), err,
                                          m.allowlist.data(), int64_t(m.slot_count));
    } else {
        m.module_id = reg.register_module(m.name, m.table.base(), err);
    }
    if (m.module_id == UINT32_MAX) return false;
    // Publish the counts (legacy X1 path — sets the single dispatch_slot_count).
    reg.set_dispatch_slot_count(m.module_id, int64_t(m.slot_count));
    // For a keyed module, publish the immutable dispatch record (atomic pointer).
    if (m.keyed) {
        reg.publish_dispatch_record(m.module_id, &m.rec);
    }
    return true;
}

// ===========================================================================
// Trap stub (records the trap + longjmps to the checkpoint).
// ===========================================================================
extern "C" void kdm_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) EMBER_LONGJMP(ctx->checkpoint, 1);
    }
    std::abort();
}

// ===========================================================================
// Run a module's main under a given route word (keyed) or via the legacy
// thunk (identity). Returns the i64 result or a trapped flag.
// ===========================================================================
struct RunResult { bool ok = false; bool trapped = false; int64_t value = 0; int32_t call_depth = 0; TrapReason last_trap = TrapReason::None; std::string reason; };

static RunResult run_keyed_main(CompiledModule& m, uint64_t route_word,
                                bool use_checkpoint = true) {
    RunResult r;
    if (!m.main_entry) { r.reason = "no main entry"; return r; }
    // Resolve the outer entry through the keyed resolver under the route word.
    void* entry = ember_resolve_keyed_dispatch(&m.rec, uint32_t(m.slots["main"]), route_word);
    if (!entry) { r.reason = "resolver returned null"; return r; }
    m.ctx.call_depth = 0;
    m.ctx.budget_remaining = 2'000'000'000LL;
    if (use_checkpoint) {
        m.ctx.has_checkpoint = true;
        if (EMBER_SETJMP(m.ctx.checkpoint)) {
            r.trapped = true; r.ok = true;
            r.last_trap = m.ctx.last_trap;
            r.reason = std::string(trap_reason_str(m.ctx.last_trap)) + ": " + m.ctx.last_error;
            m.ctx.has_checkpoint = false; m.ctx.reset_for_call();
            return r;
        }
    }
    int64_t v = ember_keyed_reentry_i64(entry, &m.ctx, 0, route_word);
    if (use_checkpoint) m.ctx.has_checkpoint = false;
    r.ok = true; r.value = v; r.call_depth = m.ctx.call_depth; r.last_trap = m.ctx.last_trap;
    return r;
}

static RunResult run_identity_main(CompiledModule& m) {
    RunResult r;
    if (!m.main_entry) { r.reason = "no main entry"; return r; }
    m.ctx.call_depth = 0;
    m.ctx.budget_remaining = 2'000'000'000LL;
    m.ctx.has_checkpoint = true;
    if (EMBER_SETJMP(m.ctx.checkpoint)) {
        r.trapped = true; r.ok = true;
        r.last_trap = m.ctx.last_trap;
        r.reason = std::string(trap_reason_str(m.ctx.last_trap)) + ": " + m.ctx.last_error;
        m.ctx.has_checkpoint = false;
        // Do NOT reset_for_call here: the caller may inspect last_trap after
        // the run (the i2k reject test checks last_trap != None).
        return r;
    }
    // ember_call_i64 installs r14=ctx (use_context_reg=true) + calls the entry.
    int64_t v = ember_call_i64(m.main_entry, &m.ctx, 0);
    m.ctx.has_checkpoint = false;
    r.ok = true; r.value = v; r.call_depth = m.ctx.call_depth; r.last_trap = m.ctx.last_trap;
    return r;
}

// ===========================================================================
// Crash guard (same pattern as keyed_dispatch_codegen_test): catches a raw
// ACCESS_VIOLATION / ILLEGAL_INSTRUCTION during a guarded run and converts it
// to a structured CrashedResult.
// ===========================================================================
struct CrashedResult { bool ran = false; bool crashed = false; int64_t value = 0; int32_t call_depth = 0; DWORD code = 0; };

static LONG WINAPI kdm_seh_filter(EXCEPTION_POINTERS* ep);
static thread_local jmp_buf g_seh_jmp;
static thread_local bool g_seh_armed = false;
static thread_local DWORD g_seh_code = 0;

static LONG WINAPI kdm_seh_filter(EXCEPTION_POINTERS* ep) {
    if (g_seh_armed && ep && ep->ExceptionRecord) {
        DWORD c = ep->ExceptionRecord->ExceptionCode;
        if (c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION ||
            c == EXCEPTION_STACK_OVERFLOW || c == EXCEPTION_INT_DIVIDE_BY_ZERO ||
            c == EXCEPTION_PRIV_INSTRUCTION) {
            g_seh_code = c;
            g_seh_armed = false;
            EMBER_LONGJMP(g_seh_jmp, 1);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

template <class F>
static CrashedResult run_guarded(F fn) {
    CrashedResult out;
    PVOID veh = AddVectoredExceptionHandler(1, kdm_seh_filter);
    g_seh_code = 0;
    g_seh_armed = true;
    if (EMBER_SETJMP(g_seh_jmp) == 0) {
        fn(out);
        g_seh_armed = false;
        out.ran = true;
    } else {
        out.crashed = true;
        out.code = g_seh_code;
        g_seh_armed = false;
    }
    if (veh) RemoveVectoredExceptionHandler(veh);
    return out;
}

// ===========================================================================
// Deterministic route-word classification for wrong-key cross-module tests.
// Classifies a target logical slot's route under candidate words into correct,
// alternate-real, or padding.
// ===========================================================================
struct RouteClassification {
    bool found = false;
    uint64_t correct_word = 0;
    uint64_t alternate_word = 0;
    uint32_t alternate_serves = 0xFFFFFFFFu;
    uint64_t padding_word = 0;
    bool has_alternate = false;
    bool has_padding = false;
};

static RouteClassification classify_routes(const CompiledModule& target,
                                           uint32_t callee_logical_slot,
                                           uint32_t caller_logical_slot) {
    RouteClassification rc;
    if (callee_logical_slot >= target.plan.logical_slot_count) return rc;
    const auto& route = target.plan.logical_routes[callee_logical_slot];
    const auto& domain = target.plan.domains[route.domain_index];
    KeyedDispatchDomain kd{domain.domain_salt, domain.strategy_version, domain.physical_count};
    rc.correct_word = target.route_word;
    const uint32_t n = domain.physical_count;
    for (uint64_t w = 1; w < 100000 && !(rc.has_alternate && rc.has_padding); ++w) {
        if (w == target.route_word) continue;
        auto pr = keyed_dispatch_permute_runtime(w, kd, route.ordinal);
        if (!pr || !pr.value) continue;
        uint32_t local = *pr.value;
        if (local >= n) continue;
        uint32_t phys = domain.physical_base + local;
        if (phys >= target.plan.physical_slot_count) continue;
        const auto& pe = target.plan.physical_entries[phys];
        if (pe.is_padding) {
            if (!rc.has_padding) { rc.has_padding = true; rc.padding_word = w; }
        } else {
            uint32_t served = pe.logical_slot;
            if (served == callee_logical_slot) continue;
            if (served == caller_logical_slot) continue;
            if (!rc.has_alternate) {
                rc.has_alternate = true; rc.alternate_word = w; rc.alternate_serves = served;
            }
        }
    }
    rc.found = rc.has_alternate && rc.has_padding;
    return rc;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    std::printf("== keyed_dispatch_modules_test (Red 7) ==\n");

    const uint64_t CORRECT_SEED = 0xCAFEBABE12345678ULL;
    const uint64_t TARGET_SEED  = 0xCAFEBABE12345678ULL;  // same as caller: the route word is machine-derived (§2.2), so all modules on the same machine share it

    // =====================================================================
    // 1. REGISTRY ATOMIC PUBLICATION + SEPARATE COUNTS + STABLE IDs.
    //    Publish a keyed module's record; verify the registry exposes the
    //    record via acquire-load, separate logical/physical counts, the mode,
    //    and a stable module_id. Re-registering the same name (reload) keeps
    //    the id and publishes a new generation.
    // =====================================================================
    {
        ModuleRegistry reg(16);
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn add1(x: i64) -> i64 { return x + 1; }\n";
        auto lib = build_keyed_module(lib_src, "lib", TARGET_SEED, /*ir=*/false,
                                       nullptr, nullptr, /*handles=*/false, reinterpret_cast<void*>(kdm_trap));
        ck(lib != nullptr, "registry: build keyed lib module succeeded");
        if (lib) {
            std::string err;
            ck(register_module(reg, *lib, /*handles=*/false, &err), "registry: register keyed lib");
            ck(lib->module_id != UINT32_MAX, "registry: lib got a module_id");
            // The registry exposes the record via acquire-load.
            const ModuleDispatchRecord* rec = reg.dispatch_record(lib->module_id);
            ck(rec != nullptr, "registry: dispatch_record returns the published record");
            ck(rec == &lib->rec, "registry: dispatch_record returns the SAME record pointer");
            // Separate logical and physical counts.
            ck(reg.logical_slot_count(lib->module_id) == lib->plan.logical_slot_count,
               "registry: logical_slot_count matches the plan");
            ck(reg.physical_slot_count(lib->module_id) == lib->plan.physical_slot_count,
               "registry: physical_slot_count matches the plan");
            ck(reg.physical_slot_count(lib->module_id) > reg.logical_slot_count(lib->module_id),
               "registry: keyed physical > logical (padding present)");
            // Mode.
            ck(reg.dispatch_mode(lib->module_id) == DispatchMode::Keyed,
               "registry: dispatch_mode is Keyed for a keyed module");
            // dispatch_records_base is stable (non-null).
            ck(reg.dispatch_records_base() != nullptr, "registry: dispatch_records_base is non-null");
        }
    }

    // =====================================================================
    // 2. IDENTITY MODULE: no record published, legacy counts, Identity mode.
    // =====================================================================
    {
        ModuleRegistry reg(16);
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn add1(x: i64) -> i64 { return x + 1; }\n";
        auto lib = build_identity_module(lib_src, "lib", /*ir=*/false,
                                         nullptr, &reg, /*handles=*/false, reinterpret_cast<void*>(kdm_trap));
        ck(lib != nullptr, "identity registry: build identity lib succeeded");
        if (lib) {
            std::string err;
            ck(register_module(reg, *lib, /*handles=*/false, &err), "identity registry: register identity lib");
            // No record published.
            ck(reg.dispatch_record(lib->module_id) == nullptr,
               "identity registry: dispatch_record is null (no keyed record)");
            ck(reg.dispatch_mode(lib->module_id) == DispatchMode::Identity,
               "identity registry: dispatch_mode is Identity");
            // logical == physical == slot_count.
            ck(reg.logical_slot_count(lib->module_id) == uint32_t(lib->slot_count),
               "identity registry: logical == slot_count");
            ck(reg.physical_slot_count(lib->module_id) == uint32_t(lib->slot_count),
               "identity registry: physical == slot_count");
            // dispatch_slot_count (legacy) still works.
            ck(reg.dispatch_slot_count(lib->module_id) == int64_t(lib->slot_count),
               "identity registry: legacy dispatch_slot_count preserved");
        }
    }

    // =====================================================================
    // 3. REPLACEMENT KEEPS ID: re-registering the same name publishes a new
    //    record generation without changing the module_id.
    // =====================================================================
    {
        ModuleRegistry reg(16);
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn add1(x: i64) -> i64 { return x + 1; }\n";
        auto lib1 = build_keyed_module(lib_src, "lib", TARGET_SEED, /*ir=*/false,
                                       nullptr, nullptr, false, reinterpret_cast<void*>(kdm_trap));
        ck(lib1 != nullptr, "reload: build lib (generation 1)");
        if (lib1) {
            std::string err;
            ck(register_module(reg, *lib1, false, &err), "reload: register lib (gen 1)");
            uint32_t id1 = lib1->module_id;
            const ModuleDispatchRecord* rec1 = reg.dispatch_record(id1);
            ck(rec1 == &lib1->rec, "reload: gen 1 record published");
            // Build a second module under the SAME name (reload) with a different seed.
            auto lib2 = build_keyed_module(lib_src, "lib", 0xAAAABBBBCCCCDDDDULL, /*ir=*/false,
                                           nullptr, nullptr, false, reinterpret_cast<void*>(kdm_trap));
            ck(lib2 != nullptr, "reload: build lib (generation 2, different seed)");
            if (lib2) {
                ck(register_module(reg, *lib2, false, &err), "reload: register lib (gen 2)");
                ck(lib2->module_id == id1, "reload: module_id unchanged after same-name re-registration");
                const ModuleDispatchRecord* rec2 = reg.dispatch_record(id1);
                ck(rec2 == &lib2->rec, "reload: gen 2 record published (new generation)");
                ck(rec2 != rec1, "reload: new record pointer differs from old generation");
                // The counts reflect the NEW generation.
                ck(reg.logical_slot_count(id1) == lib2->plan.logical_slot_count,
                   "reload: logical count reflects gen 2");
                ck(reg.physical_slot_count(id1) == lib2->plan.physical_slot_count,
                   "reload: physical count reflects gen 2");
            }
        }
    }

    // =====================================================================
    // 4. CONCURRENT READERS OBSERVE COHERENT GENERATIONS.
    //    A writer publishes generations while multiple readers acquire-load
    //    the record; every reader observes a coherent (mode + counts + table)
    //    generation. We verify by checking the record's internal consistency
    //    (logical_count <= physical_count, mode is Keyed, physical_slots non-null)
    //    for every observed generation.
    // =====================================================================
    {
        ModuleRegistry reg(4);
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn add1(x: i64) -> i64 { return x + 1; }\n";
        // Build several generations under the same name.
        std::vector<std::unique_ptr<CompiledModule>> gens;
        uint64_t seeds[] = {TARGET_SEED, 0x1111ULL, 0x2222ULL, 0x3333ULL, 0x4444ULL};
        for (uint64_t s : seeds) {
            auto g = build_keyed_module(lib_src, "lib", s, /*ir=*/false,
                                        nullptr, nullptr, false, reinterpret_cast<void*>(kdm_trap));
            if (g) gens.push_back(std::move(g));
        }
        ck(gens.size() >= 4, "concurrent: built >= 4 generations");
        if (gens.size() >= 2) {
            std::string err;
            // Register the first generation.
            ck(register_module(reg, *gens[0], false, &err), "concurrent: register gen 0");
            uint32_t id = gens[0]->module_id;
            // Writer thread: publishes generations 1..N in sequence, cycling.
            std::atomic<bool> stop{false};
            std::atomic<int> publish_count{0};
            std::thread writer([&] {
                // Cycle through the generations repeatedly so the readers have
                // time to observe many publication events (a single pass may
                // complete before the readers start).
                for (int cycle = 0; cycle < 20 && !stop.load(std::memory_order_relaxed); ++cycle) {
                    for (size_t i = 1; i < gens.size(); ++i) {
                        if (stop.load(std::memory_order_relaxed)) break;
                        reg.register_module("lib", gens[i]->table.base(), &err);
                        reg.set_dispatch_slot_count(id, int64_t(gens[i]->slot_count));
                        reg.publish_dispatch_record(id, &gens[i]->rec);
                        publish_count.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                    }
                }
                stop.store(true, std::memory_order_release);
            });
            // Reader threads: acquire-load the record + verify coherence. Each
            // reader does a fixed number of reads so the test is bounded.
            std::atomic<int> coherent_reads{0};
            std::atomic<int> total_reads{0};
            const int READS_PER_READER = 300;
            auto reader = [&] {
                for (int i = 0; i < READS_PER_READER; ++i) {
                    const ModuleDispatchRecord* r = reg.dispatch_record(id);
                    total_reads.fetch_add(1, std::memory_order_relaxed);
                    if (r) {
                        bool coherent = (r->mode == DispatchMode::Keyed) &&
                                        (r->logical_slot_count <= r->physical_slot_count) &&
                                        (r->physical_slots != nullptr);
                        if (coherent) coherent_reads.fetch_add(1, std::memory_order_relaxed);
                    }
                    std::this_thread::yield();
                }
            };
            std::thread r1(reader); std::thread r2(reader);
            writer.join(); r1.join(); r2.join();
            ck(total_reads.load() > 0, "concurrent: readers performed reads");
            ck(coherent_reads.load() == total_reads.load(),
               "concurrent: every reader observed a coherent generation (mode+counts+table consistent)");
        }
    }

    // =====================================================================
    // 5. KEYED CALLER → KEYED TARGET (direct `mod::fn()`), both backends.
    //    main in the caller module calls lib::double + lib::add1 cross-module.
    //    The keyed caller's r15 resolves the target's current record at call
    //    time. Under the correct route word the call returns the correct value.
    // =====================================================================
    {
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn add1(x: i64) -> i64 { return x + 1; }\n";
        const std::string caller_src =
            "fn main(x: i64) -> i64 { return lib::double(21) + lib::add1(10); }\n";
        for (bool ir : {false, true}) {
            const char* be = ir ? "thin" : "tree";
            ModuleRegistry reg(16);
            ModuleExportTable exports;
            auto lib = build_keyed_module(lib_src, "lib", TARGET_SEED, ir,
                                          nullptr, &reg, /*handles=*/false, reinterpret_cast<void*>(kdm_trap));
            ck(lib != nullptr, (std::string("k2k direct: ") + be + " — build keyed lib").c_str());
            if (lib) {
                std::string err;
                ck(register_module(reg, *lib, false, &err), (std::string("k2k direct: ") + be + " — register lib").c_str());
                add_module_exports(exports, lib_src, "lib", lib->module_id, DispatchMode::Keyed);
                auto caller = build_keyed_module(caller_src, "caller", CORRECT_SEED, ir,
                                                 &exports, &reg, false, reinterpret_cast<void*>(kdm_trap));
                ck(caller != nullptr, (std::string("k2k direct: ") + be + " — build keyed caller").c_str());
                if (caller) {
                    ck(register_module(reg, *caller, false, &err), (std::string("k2k direct: ") + be + " — register caller").c_str());
                    auto r = run_keyed_main(*caller, caller->route_word);
                    ck(r.ok && !r.trapped, (std::string("k2k direct: ") + be + " — run succeeded").c_str());
                    ck(r.value == 53, (std::string("k2k direct: ") + be + " — lib::double(21)+lib::add1(10) == 53 (42+11)").c_str());
                    ck(r.call_depth == 0, (std::string("k2k direct: ") + be + " — call_depth balanced (0)").c_str());
                }
            }
        }
    }

    // =====================================================================
    // 6. KEYED CALLER → IDENTITY TARGET (direct `mod::fn()`), both backends.
    //    The keyed caller has r15 but does not use it for the identity target;
    //    the legacy registry-hop sequence dispatches into the identity table.
    // =====================================================================
    {
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn add1(x: i64) -> i64 { return x + 1; }\n";
        const std::string caller_src =
            "fn main(x: i64) -> i64 { return lib::double(21) + lib::add1(10); }\n";
        for (bool ir : {false, true}) {
            const char* be = ir ? "thin" : "tree";
            ModuleRegistry reg(16);
            ModuleExportTable exports;
            auto lib = build_identity_module(lib_src, "lib", ir,
                                             nullptr, &reg, /*handles=*/false, reinterpret_cast<void*>(kdm_trap));
            ck(lib != nullptr, (std::string("k2i direct: ") + be + " — build identity lib").c_str());
            if (lib) {
                std::string err;
                ck(register_module(reg, *lib, false, &err), (std::string("k2i direct: ") + be + " — register identity lib").c_str());
                add_module_exports(exports, lib_src, "lib", lib->module_id, DispatchMode::Identity);
                auto caller = build_keyed_module(caller_src, "caller", CORRECT_SEED, ir,
                                                 &exports, &reg, false, reinterpret_cast<void*>(kdm_trap));
                ck(caller != nullptr, (std::string("k2i direct: ") + be + " — build keyed caller").c_str());
                if (caller) {
                    ck(register_module(reg, *caller, false, &err), (std::string("k2i direct: ") + be + " — register caller").c_str());
                    auto r = run_keyed_main(*caller, caller->route_word);
                    ck(r.ok && !r.trapped, (std::string("k2i direct: ") + be + " — run succeeded").c_str());
                    ck(r.value == 53, (std::string("k2i direct: ") + be + " — lib::double(21)+lib::add1(10) == 53").c_str());
                    ck(r.call_depth == 0, (std::string("k2i direct: ") + be + " — call_depth balanced (0)").c_str());
                }
            }
        }
    }

    // =====================================================================
    // 7. LEGACY CALLER → IDENTITY TARGET (unchanged legacy path), both backends.
    // =====================================================================
    {
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn add1(x: i64) -> i64 { return x + 1; }\n";
        const std::string caller_src =
            "fn main(x: i64) -> i64 { return lib::double(21) + lib::add1(10); }\n";
        for (bool ir : {false, true}) {
            const char* be = ir ? "thin" : "tree";
            ModuleRegistry reg(16);
            ModuleExportTable exports;
            auto lib = build_identity_module(lib_src, "lib", ir,
                                             nullptr, &reg, false, reinterpret_cast<void*>(kdm_trap));
            ck(lib != nullptr, (std::string("i2i direct: ") + be + " — build identity lib").c_str());
            if (lib) {
                std::string err;
                ck(register_module(reg, *lib, false, &err), (std::string("i2i direct: ") + be + " — register identity lib").c_str());
                add_module_exports(exports, lib_src, "lib", lib->module_id, DispatchMode::Identity);
                auto caller = build_identity_module(caller_src, "caller", ir,
                                                    &exports, &reg, false, reinterpret_cast<void*>(kdm_trap));
                ck(caller != nullptr, (std::string("i2i direct: ") + be + " — build identity caller").c_str());
                if (caller) {
                    ck(register_module(reg, *caller, false, &err), (std::string("i2i direct: ") + be + " — register caller").c_str());
                    auto r = run_identity_main(*caller);
                    ck(r.ok && !r.trapped, (std::string("i2i direct: ") + be + " — run succeeded").c_str());
                    ck(r.value == 53, (std::string("i2i direct: ") + be + " — lib::double(21)+lib::add1(10) == 53").c_str());
                    ck(r.call_depth == 0, (std::string("i2i direct: ") + be + " — call_depth balanced (0)").c_str());
                }
            }
        }
    }

    // =====================================================================
    // 8. LEGACY CALLER → KEYED TARGET REJECTED at codegen time.
    //    A legacy caller (no keyed_dispatch) linking to a keyed target emits
    //    a trap (the call site never dispatches into a keyed physical table as
    //    if it were logical). The run traps with a clear reason (not a crash).
    // =====================================================================
    {
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn add1(x: i64) -> i64 { return x + 1; }\n";
        const std::string caller_src =
            "fn main(x: i64) -> i64 { return lib::double(21); }\n";
        for (bool ir : {false, true}) {
            const char* be = ir ? "thin" : "tree";
            ModuleRegistry reg(16);
            ModuleExportTable exports;
            auto lib = build_keyed_module(lib_src, "lib", TARGET_SEED, ir,
                                          nullptr, &reg, false, reinterpret_cast<void*>(kdm_trap));
            ck(lib != nullptr, (std::string("i2k reject: ") + be + " — build keyed lib").c_str());
            if (lib) {
                std::string err;
                ck(register_module(reg, *lib, false, &err), (std::string("i2k reject: ") + be + " — register keyed lib").c_str());
                add_module_exports(exports, lib_src, "lib", lib->module_id, DispatchMode::Keyed);
                auto caller = build_identity_module(caller_src, "caller", ir,
                                                    &exports, &reg, false, reinterpret_cast<void*>(kdm_trap));
                ck(caller != nullptr, (std::string("i2k reject: ") + be + " — build identity caller (compile succeeded, trap at runtime)").c_str());
                if (caller) {
                    ck(register_module(reg, *caller, false, &err), (std::string("i2k reject: ") + be + " — register caller").c_str());
                    auto g = run_guarded([&](CrashedResult& out) {
                        auto r = run_identity_main(*caller);
                        out.value = r.value; out.call_depth = r.call_depth;
                        // Stash the trap info via a side channel: the caller's ctx.
                        if (r.trapped) out.code = 0; // not a crash — a recoverable trap
                    });
                    ck(!g.crashed, (std::string("i2k reject: ") + be + " — legacy→keyed call did not crash (rejected as a trap, not a segfault)").c_str());
                    // The call must have trapped (legacy caller cannot call keyed target).
                    ck(caller->ctx.last_trap != TrapReason::None,
                       (std::string("i2k reject: ") + be + " — legacy→keyed call trapped (rejected at codegen)").c_str());
                }
            }
        }
    }

    // =====================================================================
    // 9. WRONG ROUTE WORD — keyed→keyed direct cross-module call.
    //    Enter the caller under the correct key (so we enter the intended
    //    caller), install a wrong r15 via the re-entry thunk. The caller's
    //    cross-module call site resolves the target via
    //    ember_resolve_keyed_dispatch(target_record, logical_slot, wrong_r15).
    //    Under a wrong word the resolved entry is non-null, in-range, same-ABI-
    //    domain (alternate real or padding), no crash, depth balanced.
    // =====================================================================
    {
        // Target: double + sub share an ABI domain (both (i64)->i64) + padding.
        // The caller's lib::double(6) call site resolves double's logical slot
        // in the TARGET's domain under the wrong r15.
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn sub(x: i64) -> i64 { return x - 1; }\n";
        const std::string caller_src =
            "fn main(x: i64) -> i64 { return lib::double(6); }\n";
        for (bool ir : {false, true}) {
            const char* be = ir ? "thin" : "tree";
            ModuleRegistry reg(16);
            ModuleExportTable exports;
            auto lib = build_keyed_module(lib_src, "lib", TARGET_SEED, ir,
                                          nullptr, &reg, false, reinterpret_cast<void*>(kdm_trap));
            ck(lib != nullptr, (std::string("wrong-key: ") + be + " — build keyed lib").c_str());
            if (!lib) continue;
            std::string err;
            ck(register_module(reg, *lib, false, &err), (std::string("wrong-key: ") + be + " — register lib").c_str());
            add_module_exports(exports, lib_src, "lib", lib->module_id, DispatchMode::Keyed);
            auto caller = build_keyed_module(caller_src, "caller", CORRECT_SEED, ir,
                                             &exports, &reg, false, reinterpret_cast<void*>(kdm_trap));
            ck(caller != nullptr, (std::string("wrong-key: ") + be + " — build keyed caller").c_str());
            if (!caller) continue;
            ck(register_module(reg, *caller, false, &err), (std::string("wrong-key: ") + be + " — register caller").c_str());
            // Correct key: double(6) = 12.
            auto r_ok = run_keyed_main(*caller, caller->route_word, /*use_checkpoint=*/false);
            ck(r_ok.ok && r_ok.value == 12, (std::string("wrong-key: ") + be + " — correct key lib::double(6)==12").c_str());
            // Classify the target's routes for the double logical slot.
            // double's logical slot in lib: find it.
            uint32_t double_slot = uint32_t(lib->slots["double"]);
            uint32_t sub_slot = uint32_t(lib->slots["sub"]);
            // The caller's own main logical slot (to exclude recursive alternates).
            uint32_t caller_main = uint32_t(caller->slots["main"]);
            (void)caller_main;
            auto rc = classify_routes(*lib, /*callee=*/double_slot, /*caller=*/double_slot);
            // The target domain contains double + sub + padding. The caller's
            // main is in a DIFFERENT module, so no recursive alternate is possible
            // within the target domain. classify_routes excludes served==callee.
            ck(rc.has_padding, (std::string("wrong-key: ") + be + " — target layout yields a padding route word").c_str());
            // Enter the caller under the CORRECT key, install a wrong r15 via the
            // re-entry thunk. The caller's lib::double(6) call site resolves the
            // target under the wrong r15.
            auto run_wrong = [&](uint64_t wrong_word) -> RunResult {
                RunResult r;
                void* entry = ember_resolve_keyed_dispatch(&caller->rec, uint32_t(caller->slots["main"]), caller->route_word);
                if (!entry) { r.reason = "outer resolver null"; return r; }
                caller->ctx.call_depth = 0; caller->ctx.budget_remaining = 2'000'000'000LL;
                caller->ctx.has_checkpoint = true;
                if (EMBER_SETJMP(caller->ctx.checkpoint)) {
                    r.trapped = true; r.ok = true; r.last_trap = caller->ctx.last_trap;
                    r.reason = std::string(trap_reason_str(caller->ctx.last_trap)) + ": " + caller->ctx.last_error;
                    caller->ctx.has_checkpoint = false; caller->ctx.reset_for_call();
                    return r;
                }
                int64_t v = ember_keyed_reentry_i64(entry, &caller->ctx, 0, wrong_word);
                caller->ctx.has_checkpoint = false;
                r.ok = true; r.value = v; r.call_depth = caller->ctx.call_depth; r.last_trap = caller->ctx.last_trap;
                return r;
            };
            if (rc.has_padding) {
                auto r = run_wrong(rc.padding_word);
                ck(r.ok, (std::string("wrong-key: ") + be + " — wrong-r15 padding run succeeded (no crash)").c_str());
                if (r.ok) {
                    ck(r.last_trap == TrapReason::KeyedDispatchPadding,
                       (std::string("wrong-key: ") + be + " — wrong-r15 padding sets KeyedDispatchPadding").c_str());
                    ck(r.call_depth == 0, (std::string("wrong-key: ") + be + " — wrong-r15 padding call_depth balanced (0)").c_str());
                }
            }
            // Host-resolver cross-check: the resolver returns a non-null entry
            // for the wrong word, and the padding entry equals the trap stub.
            if (rc.has_padding) {
                void* e = ember_resolve_keyed_dispatch(&lib->rec, double_slot, rc.padding_word);
                ck(e != nullptr, (std::string("wrong-key: ") + be + " — host resolver non-null for padding word").c_str());
                if (e) ck(ember_is_padding_trap_target(e), (std::string("wrong-key: ") + be + " — padding entry is the trap stub").c_str());
            }
            (void)sub_slot;
        }
    }

    // =====================================================================
    // 10. CROSS-MODULE HANDLE — keyed caller takes &lib::double and calls it.
    //     The handle is (1<<63)|(module_id<<32)|logical_slot (NEVER a physical
    //     index). Calling it from a keyed caller resolves the target's current
    //     record at call time using the caller's r15.
    // =====================================================================
    {
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn add1(x: i64) -> i64 { return x + 1; }\n";
        const std::string caller_src =
            "fn main(x: i64) -> i64 { let h = &lib::double; return h(21); }\n";
        for (bool ir : {false, true}) {
            const char* be = ir ? "thin" : "tree";
            ModuleRegistry reg(16);
            ModuleExportTable exports;
            auto lib = build_keyed_module(lib_src, "lib", TARGET_SEED, ir,
                                          nullptr, &reg, /*handles=*/true, reinterpret_cast<void*>(kdm_trap));
            ck(lib != nullptr, (std::string("xhandle k2k: ") + be + " — build keyed lib (with handles)").c_str());
            if (!lib) continue;
            std::string err;
            ck(register_module(reg, *lib, /*handles=*/true, &err), (std::string("xhandle k2k: ") + be + " — register keyed lib (with handles)").c_str());
            add_module_exports(exports, lib_src, "lib", lib->module_id, DispatchMode::Keyed);
            auto caller = build_keyed_module(caller_src, "caller", CORRECT_SEED, ir,
                                             &exports, &reg, /*handles=*/false, reinterpret_cast<void*>(kdm_trap));
            ck(caller != nullptr, (std::string("xhandle k2k: ") + be + " — build keyed caller").c_str());
            if (!caller) continue;
            ck(register_module(reg, *caller, /*handles=*/false, &err), (std::string("xhandle k2k: ") + be + " — register caller").c_str());
            auto r = run_keyed_main(*caller, caller->route_word);
            ck(r.ok && !r.trapped, (std::string("xhandle k2k: ") + be + " — run succeeded").c_str());
            if (r.ok && !r.trapped) {
                ck(r.value == 42, (std::string("xhandle k2k: ") + be + " — &lib::double(21) == 42").c_str());
                ck(r.call_depth == 0, (std::string("xhandle k2k: ") + be + " — call_depth balanced (0)").c_str());
            }
        }
    }

    // =====================================================================
    // 11. CROSS-MODULE HANDLE — keyed caller to identity target.
    //     The handle dispatches via the identity target's raw table (the keyed
    //     caller has r15 but doesn't use it for the identity target's handle).
    // =====================================================================
    {
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn add1(x: i64) -> i64 { return x + 1; }\n";
        const std::string caller_src =
            "fn main(x: i64) -> i64 { let h = &lib::double; return h(21); }\n";
        for (bool ir : {false, true}) {
            const char* be = ir ? "thin" : "tree";
            ModuleRegistry reg(16);
            ModuleExportTable exports;
            auto lib = build_identity_module(lib_src, "lib", ir,
                                             nullptr, &reg, /*handles=*/true, reinterpret_cast<void*>(kdm_trap));
            ck(lib != nullptr, (std::string("xhandle k2i: ") + be + " — build identity lib (with handles)").c_str());
            if (!lib) continue;
            std::string err;
            ck(register_module(reg, *lib, /*handles=*/true, &err), (std::string("xhandle k2i: ") + be + " — register identity lib (with handles)").c_str());
            add_module_exports(exports, lib_src, "lib", lib->module_id, DispatchMode::Identity);
            auto caller = build_keyed_module(caller_src, "caller", CORRECT_SEED, ir,
                                             &exports, &reg, /*handles=*/false, reinterpret_cast<void*>(kdm_trap));
            ck(caller != nullptr, (std::string("xhandle k2i: ") + be + " — build keyed caller").c_str());
            if (!caller) continue;
            ck(register_module(reg, *caller, /*handles=*/false, &err), (std::string("xhandle k2i: ") + be + " — register caller").c_str());
            auto r = run_keyed_main(*caller, caller->route_word);
            ck(r.ok && !r.trapped, (std::string("xhandle k2i: ") + be + " — run succeeded").c_str());
            if (r.ok && !r.trapped) {
                ck(r.value == 42, (std::string("xhandle k2i: ") + be + " — &lib::double(21) == 42").c_str());
                ck(r.call_depth == 0, (std::string("xhandle k2i: ") + be + " — call_depth balanced (0)").c_str());
            }
        }
    }

    // =====================================================================
    // 12. NO PHYSICAL INDEX IN HANDLE — the &lib::double handle is baked as
    //     (1<<63)|(module_id<<32)|logical_slot. The logical_slot is NOT the
    //     target's physical slot. Scan the caller's bytes for the handle value
    //     and verify it is the LOGICAL slot, not the target's physical placement.
    // =====================================================================
    {
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn add1(x: i64) -> i64 { return x + 1; }\n";
        const std::string caller_src =
            "fn main(x: i64) -> i64 { let h = &lib::double; return h(21); }\n";
        ModuleRegistry reg(16);
        ModuleExportTable exports;
        auto lib = build_keyed_module(lib_src, "lib", TARGET_SEED, /*ir=*/false,
                                      nullptr, &reg, /*handles=*/true, reinterpret_cast<void*>(kdm_trap));
        ck(lib != nullptr, "handle no-phys: build keyed lib");
        if (!lib) goto skip_handle_nophys;
        {
            std::string err;
            ck(register_module(reg, *lib, /*handles=*/true, &err), "handle no-phys: register keyed lib");
            add_module_exports(exports, lib_src, "lib", lib->module_id, DispatchMode::Keyed);
            auto caller = build_keyed_module(caller_src, "caller", CORRECT_SEED, /*ir=*/false,
                                             &exports, &reg, /*handles=*/false, reinterpret_cast<void*>(kdm_trap));
            ck(caller != nullptr, "handle no-phys: build keyed caller");
            if (!caller) goto skip_handle_nophys;
            ck(register_module(reg, *caller, /*handles=*/false, &err), "handle no-phys: register caller");
            // double's logical slot in lib.
            uint32_t double_logical = uint32_t(lib->slots["double"]);
            // double's physical placement in the keyed plan.
            uint32_t double_phys = lib->plan.build_physical_placement[double_logical];
            // The handle value: (1<<63)|(module_id<<32)|logical_slot.
            uint64_t handle_val = (uint64_t(1) << 63) | (uint64_t(lib->module_id) << 32) | uint64_t(double_logical);
            // Scan main's bytes for the handle value (mov rax, imm64 = 48 B8 <8 LE>).
            auto find_handle = [](const std::vector<uint8_t>& bytes, uint64_t want) -> bool {
                for (size_t i = 0; i + 10 <= bytes.size(); ++i) {
                    if (bytes[i] == 0x48 && bytes[i+1] == 0xB8) {
                        uint64_t v = 0; std::memcpy(&v, bytes.data()+i+2, 8);
                        if (v == want) return true;
                    }
                }
                return false;
            };
            auto sit = caller->slots.find("main");
            ck(sit != caller->slots.end(), "handle no-phys: main slot found");
            if (sit != caller->slots.end()) {
                auto& main_bytes = caller->fns[size_t(sit->second)].bytes;
                ck(find_handle(main_bytes, handle_val), "handle no-phys: handle baked as (1<<63)|mod_id<<32|logical_slot");
                // The physical slot must NOT appear in the handle: the handle
                // is logical identity, not the routed physical ordinal.
                if (double_phys != double_logical) {
                    uint64_t phys_handle = (uint64_t(1) << 63) | (uint64_t(lib->module_id) << 32) | uint64_t(double_phys);
                    ck(!find_handle(main_bytes, phys_handle), "handle no-phys: physical slot NOT in the handle (logical identity only)");
                }
                ck(double_logical < lib->plan.logical_slot_count, "handle no-phys: logical slot < logical_count (a logical index)");
            }
        }
        skip_handle_nophys:;
    }

    // =====================================================================
    // 13. UNSUPPORTED STRATEGY/MODE — validate_dispatch_record rejects a
    //     record with an unsupported strategy_version or mode before
    //     publication.
    // =====================================================================
    {
        // Unsupported strategy_version.
        {
            RecordBuilderStorage st;
            st.storage = std::vector<std::atomic<void*>>(2);
            st.storage[0].store(reinterpret_cast<void*>(0x1000), std::memory_order_release);
            st.storage[1].store(const_cast<void*>(ember_keyed_padding_trap_target()), std::memory_order_release);
            LogicalRoute r0; r0.logical_slot = 0; r0.domain_index = 0; r0.ordinal = 0;
            st.routes = {r0};
            DispatchDomain d; d.physical_count = 2; d.logical_count = 1; d.padding_count = 1; d.padding_ordinal = 1;
            d.physical_base = 0; d.logical_slots = {0};
            st.domains = {d};
            PhysicalEntryDescriptor pd0; pd0.physical_slot = 0; pd0.is_padding = false; pd0.logical_slot = 0; pd0.domain_index = 0; pd0.ordinal = 0;
            PhysicalEntryDescriptor pd1; pd1.physical_slot = 1; pd1.is_padding = true; pd1.logical_slot = kPaddingLogicalSlotRuntime; pd1.domain_index = 0; pd1.ordinal = 1;
            st.descriptors = {pd0, pd1};
            st.allowlist = {0x01};
            ModuleDispatchRecord rec;
            rec.mode = DispatchMode::Keyed;
            rec.strategy_version = 99;  // unsupported
            rec.physical_slots = st.storage.data();
            rec.physical_slot_count = 2;
            rec.logical_slot_count = 1;
            rec.logical_routes = st.routes.data();
            rec.domains = st.domains.data();
            rec.domain_count = 1;
            rec.logical_allowlist = st.allowlist.data();
            rec.logical_allowlist_bytes = 1;
            rec.physical_descriptors = st.descriptors.data();
            rec.physical_descriptor_count = 2;
            rec.padding_trap_target = ember_keyed_padding_trap_target();
            auto vs = validate_dispatch_record(rec);
            ck(!vs, "unsupported: validate rejects strategy_version 99");
        }
        // Unsupported mode (invalid enum value).
        {
            ModuleDispatchRecord rec;
            rec.mode = static_cast<DispatchMode>(99);  // invalid
            auto vs = validate_dispatch_record(rec);
            ck(!vs, "unsupported: validate rejects invalid mode");
        }
    }

    // =====================================================================
    // 14. LOGICAL VS PHYSICAL COUNT MISMATCH — validate_dispatch_record rejects
    //     a keyed record where logical_count > physical_count.
    // =====================================================================
    {
        RecordBuilderStorage st;
        st.storage = std::vector<std::atomic<void*>>(1);
        st.storage[0].store(reinterpret_cast<void*>(0x1000), std::memory_order_release);
        LogicalRoute r0; r0.logical_slot = 0; r0.domain_index = 0; r0.ordinal = 0;
        st.routes = {r0};
        DispatchDomain d; d.physical_count = 1; d.logical_count = 1; d.padding_count = 0;
        d.physical_base = 0; d.logical_slots = {0};
        st.domains = {d};
        PhysicalEntryDescriptor pd0; pd0.physical_slot = 0; pd0.is_padding = false; pd0.logical_slot = 0; pd0.domain_index = 0; pd0.ordinal = 0;
        st.descriptors = {pd0};
        st.allowlist = {0x01};
        ModuleDispatchRecord rec;
        rec.mode = DispatchMode::Keyed;
        rec.strategy_version = 1;
        rec.physical_slots = st.storage.data();
        rec.physical_slot_count = 1;   // physical < logical
        rec.logical_slot_count = 2;    // logical > physical (mismatch)
        rec.logical_routes = st.routes.data();
        rec.domains = st.domains.data();
        rec.domain_count = 1;
        rec.logical_allowlist = st.allowlist.data();
        rec.logical_allowlist_bytes = 1;
        rec.physical_descriptors = st.descriptors.data();
        rec.physical_descriptor_count = 1;
        auto vs = validate_dispatch_record(rec);
        ck(!vs, "count mismatch: validate rejects logical_count > physical_count");
    }

    // =====================================================================
    // 15. LEGACY IDENTITY BYTES UNCHANGED — when keyed_dispatch is null, the
    //     cross-module call emit is byte-identical to the pre-Red-7 path.
    //     Compile a legacy caller→identity target cross-module call with
    //     keyed_dispatch=null; verify the bytes do NOT contain the keyed
    //     resolver address or the dispatch_records_base.
    // =====================================================================
    {
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n";
        const std::string caller_src =
            "fn main(x: i64) -> i64 { return lib::double(21); }\n";
        ModuleRegistry reg(16);
        ModuleExportTable exports;
        auto lib = build_identity_module(lib_src, "lib", /*ir=*/false,
                                         nullptr, &reg, false, reinterpret_cast<void*>(kdm_trap));
        ck(lib != nullptr, "legacy bytes: build identity lib");
        if (!lib) goto skip_legacy_bytes;
        {
            std::string err;
            ck(register_module(reg, *lib, false, &err), "legacy bytes: register identity lib");
            add_module_exports(exports, lib_src, "lib", lib->module_id, DispatchMode::Identity);
            auto caller = build_identity_module(caller_src, "caller", /*ir=*/false,
                                                &exports, &reg, false, reinterpret_cast<void*>(kdm_trap));
            ck(caller != nullptr, "legacy bytes: build identity caller");
            if (!caller) goto skip_legacy_bytes;
            ck(register_module(reg, *caller, false, &err), "legacy bytes: register caller");
            auto sit = caller->slots.find("main");
            ck(sit != caller->slots.end(), "legacy bytes: main slot found");
            if (sit != caller->slots.end()) {
                auto& main_bytes = caller->fns[size_t(sit->second)].bytes;
                uint64_t resolver_addr = uint64_t(reinterpret_cast<uintptr_t>(&ember_resolve_keyed_dispatch));
                auto contains = [&](uint64_t want) -> bool {
                    for (size_t i = 0; i + 8 <= main_bytes.size(); ++i) {
                        uint64_t w = 0; std::memcpy(&w, main_bytes.data()+i, 8);
                        if (w == want) return true;
                    }
                    return false;
                };
                ck(!contains(resolver_addr), "legacy bytes: no keyed resolver address in legacy cross-module emit");
                ck(!contains(uint64_t(reg.dispatch_records_base())), "legacy bytes: no dispatch_records_base in legacy cross-module emit");
            }
            // Run it to verify correctness.
            auto r = run_identity_main(*caller);
            ck(r.ok && !r.trapped && r.value == 42, "legacy bytes: lib::double(21)==42 (legacy path correct)");
        }
        skip_legacy_bytes:;
    }

    // =====================================================================
    // 16. SAME-ABI/PADDING SAFETY — a wrong route word that lands on the
    //     padding ordinal is a non-null same-ABI trap stub; an alternate-real
    //     route runs a different same-domain callable. The target domain's
    //     ABI fingerprint is preserved (every selected entry's descriptor
    //     matches the domain's ABI).
    // =====================================================================
    {
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn sub(x: i64) -> i64 { return x - 1; }\n";
        ModuleRegistry reg(16);
        auto lib = build_keyed_module(lib_src, "lib", TARGET_SEED, /*ir=*/false,
                                      nullptr, &reg, false, reinterpret_cast<void*>(kdm_trap));
        ck(lib != nullptr, "abi safety: build keyed lib");
        if (lib) {
            std::string err;
            ck(register_module(reg, *lib, false, &err), "abi safety: register keyed lib");
            uint32_t double_slot = uint32_t(lib->slots["double"]);
            const auto& route = lib->plan.logical_routes[double_slot];
            const auto& domain = lib->plan.domains[route.domain_index];
            KeyedDispatchDomain kd{domain.domain_salt, domain.strategy_version, domain.physical_count};
            // Scan many wrong words; every result is non-null, in-range, same-ABI-domain.
            int checked = 0;
            for (uint64_t w = 0; w < 1000; ++w) {
                if (w == lib->route_word) continue;
                auto pr = keyed_dispatch_permute_runtime(w, kd, route.ordinal);
                if (!pr || !pr.value) continue;
                uint32_t local = *pr.value;
                if (local >= domain.physical_count) continue;
                uint32_t phys = domain.physical_base + local;
                const auto& pe = lib->plan.physical_entries[phys];
                // The selected entry's descriptor ABI matches the domain's ABI.
                ck(pe.abi_fingerprint == domain.abi_fingerprint, "abi safety: every selected entry's ABI matches the domain ABI");
                ck(pe.visibility == domain.visibility, "abi safety: every selected entry's visibility matches the domain");
                ck(pe.calling_mode == domain.calling_mode, "abi safety: every selected entry's calling_mode matches the domain");
                // The host resolver returns a non-null entry for this word.
                void* e = ember_resolve_keyed_dispatch(&lib->rec, double_slot, w);
                ck(e != nullptr, "abi safety: resolver non-null for every wrong word");
                ++checked;
            }
            ck(checked > 100, "abi safety: checked > 100 wrong words (thorough coverage)");
        }
    }

    // =====================================================================
    // 17. EDGE ROUTE WORDS — exact runtime route words 0 and UINT64_MAX for a
    //     keyed→keyed direct cross-module call. The call resolves safely (no
    //     crash, depth balanced).
    // =====================================================================
    {
        const std::string lib_src =
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn sub(x: i64) -> i64 { return x - 1; }\n";
        const std::string caller_src =
            "fn main(x: i64) -> i64 { return lib::double(6); }\n";
        for (bool ir : {false, true}) {
            const char* be = ir ? "thin" : "tree";
            ModuleRegistry reg(16);
            ModuleExportTable exports;
            auto lib = build_keyed_module(lib_src, "lib", TARGET_SEED, ir,
                                          nullptr, &reg, false, reinterpret_cast<void*>(kdm_trap));
            ck(lib != nullptr, (std::string("edge: ") + be + " — build keyed lib").c_str());
            if (!lib) continue;
            std::string err;
            ck(register_module(reg, *lib, false, &err), (std::string("edge: ") + be + " — register lib").c_str());
            add_module_exports(exports, lib_src, "lib", lib->module_id, DispatchMode::Keyed);
            auto caller = build_keyed_module(caller_src, "caller", CORRECT_SEED, ir,
                                             &exports, &reg, false, reinterpret_cast<void*>(kdm_trap));
            ck(caller != nullptr, (std::string("edge: ") + be + " — build keyed caller").c_str());
            if (!caller) continue;
            ck(register_module(reg, *caller, false, &err), (std::string("edge: ") + be + " — register caller").c_str());
            auto run_wrong = [&](uint64_t wrong_word) -> RunResult {
                RunResult r;
                void* entry = ember_resolve_keyed_dispatch(&caller->rec, uint32_t(caller->slots["main"]), caller->route_word);
                if (!entry) { r.reason = "outer resolver null"; return r; }
                caller->ctx.call_depth = 0; caller->ctx.budget_remaining = 2'000'000'000LL;
                caller->ctx.has_checkpoint = true;
                if (EMBER_SETJMP(caller->ctx.checkpoint)) {
                    r.trapped = true; r.ok = true; r.last_trap = caller->ctx.last_trap;
                    caller->ctx.has_checkpoint = false; caller->ctx.reset_for_call();
                    return r;
                }
                int64_t v = ember_keyed_reentry_i64(entry, &caller->ctx, 0, wrong_word);
                caller->ctx.has_checkpoint = false;
                r.ok = true; r.value = v; r.call_depth = caller->ctx.call_depth; r.last_trap = caller->ctx.last_trap;
                return r;
            };
            for (uint64_t edge : {0ULL, UINT64_MAX}) {
                if (edge == caller->route_word) continue;
                auto g = run_guarded([&](CrashedResult& out) {
                    auto r = run_wrong(edge);
                    out.value = r.value; out.call_depth = r.call_depth;
                });
                ck(!g.crashed, (std::string("edge: ") + be + " — exact edge word " + (edge == 0 ? "0" : "UINT64_MAX") + " did not crash").c_str());
            }
        }
    }

    // =====================================================================
    // Summary
    // =====================================================================
    std::printf("== %d checks, %s ==\n", g_checks, g_fail ? "FAILED" : "ALL PASSED");
    return g_fail;
}
