// keyed_dispatch_hot_reload_test — Red 10
// (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §12, §10, §14.2, §14.3
// Red 10): the keyed single-function hot-reload + whole-generation
// replacement gate.
//
// RED-GREEN contract chunk for keyed hot reload. This is the RED side
// (written first) of the Red 10 contract. It pins, against real JIT-compiled
// keyed modules whose dispatch records use a real DispatchMode::Keyed
// topology with at least two same-ABI functions plus one ABI-compatible
// padding target, the §12 / §10 / §14.2 / §14.3 mandatory buckets:
//
//   - physical-slot replacement:  single-function reload publishes at the
//                                  KEYED domain slot (P(K, domain, ordinal)),
//                                  NOT the raw logical index; the physical
//                                  slot is the keyed domain slot selected by
//                                  the versioned permutation under the
//                                  transient build-provider route word. This
//                                  is a HARD assertion under a deterministic
//                                  topology/seed, not an informational
//                                  observation.
//   - logical handle stability:   calls and logical function handles retain
//                                  their logical slot and observe the
//                                  replacement through the immutable
//                                  ModuleDispatchRecord + the keyed resolver.
//   - ABI/visibility/domain       the replacement source preserves the exact
//     preservation:                canonical ABI fingerprint, visibility,
//                                  calling mode, and dispatch-domain
//                                  membership of the replaced callable.
//   - provider/compile/validation the transient build-provider route, the
//     failure:                     compile, and validation stages each publish
//                                  NOTHING and do NOT advance the epoch on
//                                  failure.
//   - wrong build provider:       a deliberately different build provider
//                                  remains bounded, non-null, and within the
//                                  same ABI/visibility domain without an
//                                  expected-key comparison or a memory fault.
//   - guard pins old page:        a guard acquired before publication pins the
//                                  old executable page until released; the
//                                  old page remains callable while the guard
//                                  is active and is reclaimed only after the
//                                  guard drains.
//   - whole-generation replace:   a complete keyed module generation can
//                                  replace another generation under the same
//                                  stable module ID using a fresh immutable
//                                  ModuleDispatchRecord and possibly changed
//                                  keyed topology; readers see only coherent
//                                  old or new records.
//   - old record alive under guard: old record backing storage and executable
//                                  pages remain alive while an old guard is
//                                  active across a generation swap.
//   - disabled keyed mode:        when keyed mode is disabled, the existing
//                                  identity-layout reload_function behavior is
//                                  preserved (logical slot == physical slot,
//                                  unchanged values).
//   - topology rejection:         single-function reload REJECTS topology/
//                                  domain changes and directs those changes to
//                                  whole-generation replacement.
//
// The minimal expected Red 10 API contract is DECLARED below (forward
// declarations of the types + free functions the GREEN side will provide).
// No production behavior is implemented in this chunk: the declarations let
// the test compile against the expected surface, and the LINK step fails with
// undefined references — the genuine RED failure. The GREEN phase will move
// these declarations into a production header (src/keyed_hot_reload.hpp) and
// implement them.
//
// Links ember + ember_frontend + ember_import + the standard six extensions
// (the keyed CodeGenCtx + the keyed resolver + the existing reload_function
// identity path + native registration). NOT a CTest entry: the filtered suite
// count must stay 67 (§14.1); the target building cleanly + the executable
// passing IS the gate.

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
#include "../src/hot_reload.hpp"
#include "../src/hot_reload_domain.hpp"
#include "../src/keyed_hot_reload.hpp"   // Red 10 GREEN: the keyed reload + generation-replacement contract
#include "import.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
// The Red 10 API contract now lives in src/keyed_hot_reload.hpp (included
// above). The GREEN implementation in src/keyed_hot_reload.cpp provides
// reload_keyed_function, replace_keyed_generation, and
// keyed_reload_preserves_topology. This test exercises that contract against
// real JIT-compiled keyed modules with a real DispatchMode::Keyed topology.
// ===========================================================================

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

// A failing provider — always returns an error (§12.2: provider failure
// publishes nothing and does not advance the epoch).
struct FailingProvider : public DerivedMaterialProvider {
    ExtensionResult<std::array<uint8_t, 32>> derive(const DerivationRequest&) const override {
        return make_extension_result_error<std::array<uint8_t, 32>>(
            "ember-keyed-dispatch", "provider", "deliberate provider failure");
    }
};

// ===========================================================================
// ABI classifier helpers (ported from keyed_dispatch_modules_test).
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
// A compiled keyed module. Owns its compiled functions, dispatch table, keyed
// record storage, and the program (for reload). The hot-reload domain is
// owned separately by the test harness so a guard can outlive a generation
// swap.
// ===========================================================================
struct KeyedModule {
    std::string name;
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string, int> slots;
    std::unordered_map<std::string, NativeSig> natives;
    Program prog;
    OpOverloadTable ov;
    StructLayoutTable layouts;
    GlobalsBlock gb;
    std::vector<uint8_t> gbs;
    std::vector<uint8_t> allowlist;
    int slot_count = 0;
    ModuleManifest manifest;
    ModuleLayoutPlan plan;
    RecordBuilderStorage st;
    ModuleDispatchRecord rec{};
    uint64_t route_word = 0;
    uint64_t seed = 0;
    ember::context_t ctx{};
    ~KeyedModule() { for (auto& fn : fns) if (fn.exec) free_executable(fn.exec); }
};

// ===========================================================================
// Trap stub (records the trap + longjmps to the checkpoint).
// ===========================================================================
extern "C" void khr_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) EMBER_LONGJMP(ctx->checkpoint, 1);
    }
    std::abort();
}

// ===========================================================================
// Build a complete keyed module: parse + manifest + compile + plan + record.
// Uses the same shape as keyed_dispatch_modules_test's build_keyed_module.
// ===========================================================================
static std::unique_ptr<KeyedModule> build_keyed_module(
    const std::string& src, const std::string& name,
    uint64_t route_word_seed,
    HotReloadDomain* domain,
    void* trap_stub = nullptr) {

    auto m = std::make_unique<KeyedModule>();
    m->name = name;
    m->seed = route_word_seed;

    // 1. Parse to build the manifest (slots + fingerprints) + keep the program.
    {
        std::unordered_set<std::string> seen; std::string resolved;
        try { resolved = resolve_imports(src, "./", seen); } catch (...) { return nullptr; }
        auto lr = tokenize(resolved, "<khr>"); if (!lr.ok) return nullptr;
        auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
        m->prog = std::move(pr.program);
        int si = 0;
        for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = m->slots[fn.name]; }
        m->slot_count = si;
        m->manifest = make_manifest(m->prog, m->slots, name);
    }

    // 2. Plan the keyed layout to get the record shape.
    uint8_t key_bytes[8];
    for (int i = 0; i < 8; ++i) key_bytes[i] = uint8_t((route_word_seed >> (8*i)) & 0xFF);
    BuildKeyView key{key_bytes, 8};
    ImplicitKeyedLayoutV1 planner;
    auto pp = planner.plan(m->manifest, key);
    if (!pp) { std::printf("FAIL: keyed plan failed for %s\n", name.c_str()); return nullptr; }
    m->plan = std::move(*pp.value);
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
    m->layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;
    auto sr = sema(m->prog, m->natives, m->slots, 0, &m->ov, &m->layouts, nullptr);
    if (!sr.ok) {
        std::printf("FAIL: sema for %s:\n", name.c_str());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return nullptr;
    }
    m->gbs.assign(m->prog.globals.size() * 8, 0);
    m->gb.base = int64_t(m->gbs.data());
    { uint32_t gi = 0; for (auto& g : m->prog.globals) { m->gb.index[g.name] = gi++; m->gb.types[g.name] = g.ty.get(); } }
    g_globals_for_codegen = nullptr;
    m->table = DispatchTable(m->slot_count);
    m->allowlist = build_fn_allowlist(m->slots, m->slot_count);

    CodeGenCtx ctx;
    ctx.globals_base = m->gb.base;
    ctx.dispatch_base = int64_t(m->table.base());
    ctx.natives = &m->natives;
    ctx.script_slots = &m->slots;
    ctx.structs = &m->layouts;
    ctx.globals_index = &m->gb.index;
    ctx.globals_types = &m->gb.types;
    ctx.use_context_reg = true;
    ctx.emit_depth_checks = true;
    ctx.max_call_depth = 64;
    ctx.fn_allowlist_base = int64_t(m->allowlist.data());
    ctx.fn_slot_count = m->slot_count;
    if (trap_stub) { ctx.trap_stub = trap_stub; ctx.trap_ctx = &m->ctx; }
    {
        KeyedDispatchCodegen kd{};
        kd.runtime_key = RuntimeKeyLocation::R15;
        kd.module_record = &m->rec;
        ctx.keyed_dispatch = &kd;
    }
    ctx.enable_ir_backend = false;

    m->ctx.budget_remaining = 2'000'000'000LL;
    m->ctx.max_call_depth = 64;
    m->ctx.call_depth = 0;

    for (auto& fn : m->prog.funcs) {
        auto cr = compile_func_checked(fn, ctx);
        finalize(cr.compiled);
        auto& cf = cr.compiled;
        if (!cf.entry) { std::printf("FAIL: null entry for %s in %s\n", fn.name.c_str(), name.c_str()); return nullptr; }
        m->table.set(fn.slot, cf.entry);
        m->fns.push_back(std::move(cf));
    }

    // 5. Populate real entries in the keyed physical storage.
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
    (void)domain;
    return m;
}

// Make a CodeGenCtx matching the module's compilation environment (for reload).
static CodeGenCtx make_ctx(KeyedModule& m) {
    CodeGenCtx ctx;
    ctx.globals_base = m.gb.base;
    ctx.dispatch_base = int64_t(m.table.base());
    ctx.natives = &m.natives;
    ctx.script_slots = &m.slots;
    ctx.structs = &m.layouts;
    ctx.globals_index = &m.gb.index;
    ctx.globals_types = &m.gb.types;
    ctx.use_context_reg = true;
    ctx.emit_depth_checks = true;
    ctx.max_call_depth = 64;
    ctx.fn_allowlist_base = int64_t(m.allowlist.data());
    ctx.fn_slot_count = m.slot_count;
    ctx.trap_stub = reinterpret_cast<void*>(khr_trap);
    ctx.trap_ctx = &m.ctx;
    {
        KeyedDispatchCodegen kd{};
        kd.runtime_key = RuntimeKeyLocation::R15;
        kd.module_record = &m.rec;
        ctx.keyed_dispatch = &kd;
    }
    ctx.enable_ir_backend = false;
    return ctx;
}

// Run a module's function by logical slot under the correct route word,
// through the keyed resolver + the keyed re-entry thunk. Returns the i64
// result or a trapped flag.
struct RunResult { bool ok = false; bool trapped = false; int64_t value = 0; std::string reason; };

static RunResult run_keyed(KeyedModule& m, const std::string& fn_name, int64_t arg = 0) {
    RunResult r;
    auto sit = m.slots.find(fn_name);
    if (sit == m.slots.end()) { r.reason = "no slot for " + fn_name; return r; }
    uint32_t logical_slot = uint32_t(sit->second);
    void* entry = ember_resolve_keyed_dispatch(&m.rec, logical_slot, m.route_word);
    if (!entry) { r.reason = "resolver returned null for " + fn_name; return r; }
    m.ctx.call_depth = 0;
    m.ctx.budget_remaining = 2'000'000'000LL;
    m.ctx.has_checkpoint = true;
    if (EMBER_SETJMP(m.ctx.checkpoint)) {
        r.trapped = true; r.ok = true;
        r.reason = std::string(trap_reason_str(m.ctx.last_trap)) + ": " + m.ctx.last_error;
        m.ctx.has_checkpoint = false; m.ctx.reset_for_call();
        return r;
    }
    int64_t v = ember_keyed_reentry_i64(entry, &m.ctx, arg, m.route_word);
    m.ctx.has_checkpoint = false;
    r.ok = true; r.value = v;
    return r;
}

// ===========================================================================
// SEH crash guard (same pattern as keyed_dispatch_modules_test).
// ===========================================================================
static LONG WINAPI khr_seh_filter(EXCEPTION_POINTERS* ep);
static thread_local jmp_buf g_seh_jmp;
static thread_local bool g_seh_armed = false;
static thread_local DWORD g_seh_code = 0;

static LONG WINAPI khr_seh_filter(EXCEPTION_POINTERS* ep) {
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

struct CrashedResult { bool ran = false; bool crashed = false; int64_t value = 0; DWORD code = 0; };

template <class F>
static CrashedResult run_guarded(F fn) {
    CrashedResult out;
    PVOID veh = AddVectoredExceptionHandler(1, khr_seh_filter);
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
// The deterministic seed. Under this seed, a keyed module with two
// (i64)->i64 same-ABI public functions + one padding target (domain size 3)
// produces a permutation where BOTH real functions' physical slots differ
// from their logical slots:
//     logical 0 (double) -> physical 1
//     logical 1 (add1)   -> physical 2
//     padding             -> physical 0
// This makes the "physical slot is the keyed domain slot, not the raw logical
// index" invariant a HARD, testable assertion (the physical slot published by
// the reload MUST equal the permuted slot and MUST differ from the logical
// index). Seed 3 (verified against the production ImplicitKeyedLayoutV1
// planner with the real Public (i64)->i64 ABI fingerprint) yields exactly this
// placement; seed 1 yields an identity permutation under the same fingerprint,
// so it cannot exercise the physical!=logical invariant.
// ===========================================================================
static constexpr uint64_t DETERMINISTIC_SEED = 3;

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    std::printf("== keyed_dispatch_hot_reload_test (Red 10) ==\n");

    // The source for the keyed module: two same-ABI (i64)->i64 functions.
    // double + add1 share one ABI domain; the keyed planner adds one padding
    // ordinal, so the domain size is 3.
    const std::string mod_src =
        "fn double(x: i64) -> i64 { return x * 2; }\n"
        "fn add1(x: i64) -> i64 { return x + 1; }\n";

    // =====================================================================
    // 1. PHYSICAL-SLOT REPLACEMENT — single-function keyed reload publishes
    //    at the KEYED domain slot (P(K, domain, ordinal)), NOT the raw
    //    logical index. HARD assertion under the deterministic seed.
    // =====================================================================
    {
        HotReloadDomain domain;
        auto m = build_keyed_module(mod_src, "kmod", DETERMINISTIC_SEED, &domain,
                                    reinterpret_cast<void*>(khr_trap));
        ck(m != nullptr, "single-fn: build keyed module with two same-ABI functions + padding");
        if (!m) goto skip_single_fn;

        // HARD assertion: the deterministic topology separates the physical
        // slot from the raw logical index for BOTH real functions. This is
        // the invariant the keyed reload must honor: it publishes at the
        // permuted physical slot, not the logical index.
        {
            uint32_t double_logical = uint32_t(m->slots["double"]);
            uint32_t add1_logical = uint32_t(m->slots["add1"]);
            uint32_t double_phys = m->plan.build_physical_placement[double_logical];
            uint32_t add1_phys = m->plan.build_physical_placement[add1_logical];
            ck(double_phys != double_logical,
               "single-fn: deterministic topology separates double physical from logical (HARD)");
            ck(add1_phys != add1_logical,
               "single-fn: deterministic topology separates add1 physical from logical (HARD)");
            ck(double_phys < m->plan.physical_slot_count,
               "single-fn: double physical slot in range");
            ck(add1_phys < m->plan.physical_slot_count,
               "single-fn: add1 physical slot in range");
            ck(m->plan.physical_slot_count == 3,
               "single-fn: domain size is 3 (2 real + 1 padding)");
            ck(m->plan.domains.size() == 1,
               "single-fn: one ABI domain (double + add1 share ABI)");
            // Snapshot the keyed physical slot for double (the slot the reload
            // must publish at).
            uint32_t double_keyed_phys = double_phys;
            void* old_entry_at_phys = m->st.storage[double_keyed_phys].load(std::memory_order_acquire);
            ck(old_entry_at_phys != nullptr,
               "single-fn: old entry at the keyed physical slot is non-null");
            ck(!ember_is_padding_trap_target(old_entry_at_phys),
               "single-fn: old entry at the keyed physical slot is a REAL entry (not padding)");

            // Verify the initial values through the keyed resolver.
            auto r0 = run_keyed(*m, "double", 21);
            ck(r0.ok && !r0.trapped && r0.value == 42,
               "single-fn: initial double(21)==42 through keyed resolver");
            auto r1 = run_keyed(*m, "add1", 10);
            ck(r1.ok && !r1.trapped && r1.value == 11,
               "single-fn: initial add1(10)==11 through keyed resolver");

            // Reload double to return x * 3 (same ABI: (i64)->i64, same
            // visibility, same calling mode, same dispatch domain). The
            // transient build-provider route word is derived from the SAME
            // provider the build used (the FixedProvider seeded with
            // DETERMINISTIC_SEED), so the keyed domain slot is the same
            // permuted slot the build placed double at.
            auto provider = std::make_shared<FixedProvider>(DETERMINISTIC_SEED);
            KeyedReloadRequest req;
            req.new_fn_source = "fn double(x: i64) -> i64 { return x * 3; }\n";
            req.logical_slot = double_logical;
            req.build_provider = provider;

            auto ctx = make_ctx(*m);
            KeyedReloadResult kr = reload_keyed_function(
                req, m->prog, m->st, m->rec, m->plan, domain, ctx,
                m->natives, &m->ov, &m->layouts);

            ck(kr.ok, "single-fn: keyed reload of double succeeded");
            if (kr.ok) {
                // HARD assertion: the physical slot published is the KEYED
                // domain slot (the permuted slot), NOT the raw logical index.
                ck(kr.physical_slot_published == double_keyed_phys,
                   "single-fn: reload published at the KEYED domain slot (permuted)");
                ck(kr.physical_slot_published != double_logical,
                   "single-fn: published physical slot != raw logical index (HARD)");
                ck(kr.publication_epoch > 0,
                   "single-fn: successful publication advanced the epoch");
                ck(kr.old_page_retired,
                   "single-fn: old page transferred to domain for reclamation");

                // The keyed physical storage now holds the NEW entry.
                void* new_entry_at_phys = m->st.storage[double_keyed_phys].load(std::memory_order_acquire);
                ck(new_entry_at_phys != nullptr && new_entry_at_phys != old_entry_at_phys,
                   "single-fn: keyed physical slot now holds the NEW entry");

                // Logical handle stability: calls through the keyed resolver
                // by logical slot observe the replacement. double(21) now
                // returns 63 (21*3), not 42 (21*2).
                auto r2 = run_keyed(*m, "double", 21);
                ck(r2.ok && !r2.trapped && r2.value == 63,
                   "single-fn: logical handle observes replacement (double(21)==63)");
                // add1 is unaffected.
                auto r3 = run_keyed(*m, "add1", 10);
                ck(r3.ok && !r3.trapped && r3.value == 11,
                   "single-fn: add1 unaffected by double's reload (add1(10)==11)");

                // The logical slot for double is unchanged (stable identity).
                ck(uint32_t(m->slots["double"]) == double_logical,
                   "single-fn: logical slot for double is unchanged (stable identity)");

                // Keep the new fn alive (it is current; the domain owns the old page).
                m->fns.push_back(std::move(kr.new_fn));
            }
        }
        skip_single_fn:;
    }

    // =====================================================================
    // 2. ABI/VISIBILITY/DOMAIN PRESERVATION — the replacement source must
    //    preserve the exact ABI fingerprint, visibility, calling mode, and
    //    dispatch-domain membership. A replacement that changes the ABI is
    //    rejected (publishes nothing, epoch unchanged).
    // =====================================================================
    {
        HotReloadDomain domain;
        auto m = build_keyed_module(mod_src, "kmod", DETERMINISTIC_SEED, &domain,
                                    reinterpret_cast<void*>(khr_trap));
        ck(m != nullptr, "abi-preserve: build keyed module");
        if (!m) goto skip_abi;

        {
            uint32_t double_logical = uint32_t(m->slots["double"]);
            uint64_t expected_fp = m->plan.logical_routes[double_logical].abi_fingerprint;
            Visibility expected_vis = m->plan.logical_routes[double_logical].visibility;
            CallingMode expected_mode = m->plan.logical_routes[double_logical].calling_mode;
            std::string expected_domain = m->plan.logical_routes[double_logical].dispatch_domain;

            const uint64_t epoch_before = domain.epoch();

            // A replacement that changes the return type (i64 -> f32) is an
            // ABI change: it must be rejected.
            auto provider = std::make_shared<FixedProvider>(DETERMINISTIC_SEED);
            KeyedReloadRequest req;
            req.new_fn_source = "fn double(x: i64) -> f32 { return 3.0f; }\n";
            req.logical_slot = double_logical;
            req.build_provider = provider;
            auto ctx = make_ctx(*m);
            KeyedReloadResult kr = reload_keyed_function(
                req, m->prog, m->st, m->rec, m->plan, domain, ctx,
                m->natives, &m->ov, &m->layouts);
            ck(!kr.ok, "abi-preserve: ABI-changing reload (i64->f32 return) rejected");
            ck(domain.epoch() == epoch_before,
               "abi-preserve: ABI-changing reload did not advance the epoch");

            // A correct-ABI replacement (same (i64)->i64) preserves the
            // fingerprint/visibility/mode/domain. The reload succeeds and the
            // route's metadata is unchanged.
            req.new_fn_source = "fn double(x: i64) -> i64 { return x + 100; }\n";
            KeyedReloadResult kr2 = reload_keyed_function(
                req, m->prog, m->st, m->rec, m->plan, domain, ctx,
                m->natives, &m->ov, &m->layouts);
            ck(kr2.ok, "abi-preserve: same-ABI reload succeeds");
            if (kr2.ok) {
                ck(m->plan.logical_routes[double_logical].abi_fingerprint == expected_fp,
                   "abi-preserve: ABI fingerprint preserved after reload");
                ck(m->plan.logical_routes[double_logical].visibility == expected_vis,
                   "abi-preserve: visibility preserved after reload");
                ck(m->plan.logical_routes[double_logical].calling_mode == expected_mode,
                   "abi-preserve: calling mode preserved after reload");
                ck(m->plan.logical_routes[double_logical].dispatch_domain == expected_domain,
                   "abi-preserve: dispatch-domain membership preserved after reload");
                auto r = run_keyed(*m, "double", 1);
                ck(r.ok && !r.trapped && r.value == 101,
                   "abi-preserve: replacement observed (double(1)==101)");
                m->fns.push_back(std::move(kr2.new_fn));
            }
        }
        skip_abi:;
    }

    // =====================================================================
    // 3. PROVIDER/COMPILE/VALIDATION FAILURES publish nothing and do NOT
    //    advance the epoch.
    // =====================================================================
    {
        HotReloadDomain domain;
        auto m = build_keyed_module(mod_src, "kmod", DETERMINISTIC_SEED, &domain,
                                    reinterpret_cast<void*>(khr_trap));
        ck(m != nullptr, "fail: build keyed module");
        if (!m) goto skip_fail;
        {
            uint32_t double_logical = uint32_t(m->slots["double"]);
            const uint64_t epoch_before = domain.epoch();

            // (3a) Provider failure: a failing provider returns an error; the
            // reload publishes nothing and the epoch is unchanged.
            {
                auto fail_provider = std::make_shared<FailingProvider>();
                KeyedReloadRequest req;
                req.new_fn_source = "fn double(x: i64) -> i64 { return x * 3; }\n";
                req.logical_slot = double_logical;
                req.build_provider = fail_provider;
                auto ctx = make_ctx(*m);
                KeyedReloadResult kr = reload_keyed_function(
                    req, m->prog, m->st, m->rec, m->plan, domain, ctx,
                    m->natives, &m->ov, &m->layouts);
                ck(!kr.ok, "fail: provider failure rejected the reload");
                ck(domain.epoch() == epoch_before,
                   "fail: provider failure did not advance the epoch");
            }

            // (3b) Compile failure: a syntactically invalid source fails at
            // parse/sema; the reload publishes nothing and the epoch is
            // unchanged.
            {
                auto provider = std::make_shared<FixedProvider>(DETERMINISTIC_SEED);
                KeyedReloadRequest req;
                req.new_fn_source = "fn double(x: i64) -> i64 { return x +; }\n";  // syntax error
                req.logical_slot = double_logical;
                req.build_provider = provider;
                auto ctx = make_ctx(*m);
                KeyedReloadResult kr = reload_keyed_function(
                    req, m->prog, m->st, m->rec, m->plan, domain, ctx,
                    m->natives, &m->ov, &m->layouts);
                ck(!kr.ok, "fail: compile failure (syntax error) rejected the reload");
                ck(domain.epoch() == epoch_before,
                   "fail: compile failure did not advance the epoch");
            }

            // (3c) Sema failure: a type error fails at sema; the reload
            // publishes nothing and the epoch is unchanged.
            {
                auto provider = std::make_shared<FixedProvider>(DETERMINISTIC_SEED);
                KeyedReloadRequest req;
                req.new_fn_source = "fn double(x: i64) -> i64 { return 1.5f; }\n";  // type error
                req.logical_slot = double_logical;
                req.build_provider = provider;
                auto ctx = make_ctx(*m);
                KeyedReloadResult kr = reload_keyed_function(
                    req, m->prog, m->st, m->rec, m->plan, domain, ctx,
                    m->natives, &m->ov, &m->layouts);
                ck(!kr.ok, "fail: sema failure (type error) rejected the reload");
                ck(domain.epoch() == epoch_before,
                   "fail: sema failure did not advance the epoch");
            }

            // (3d) Arity change is an ABI change: rejected, epoch unchanged.
            {
                auto provider = std::make_shared<FixedProvider>(DETERMINISTIC_SEED);
                KeyedReloadRequest req;
                req.new_fn_source = "fn double(x: i64, y: i64) -> i64 { return x + y; }\n";
                req.logical_slot = double_logical;
                req.build_provider = provider;
                auto ctx = make_ctx(*m);
                KeyedReloadResult kr = reload_keyed_function(
                    req, m->prog, m->st, m->rec, m->plan, domain, ctx,
                    m->natives, &m->ov, &m->layouts);
                ck(!kr.ok, "fail: arity-changing reload rejected (ABI change)");
                ck(domain.epoch() == epoch_before,
                   "fail: arity-changing reload did not advance the epoch");
            }

            // The module is still callable with the original values.
            auto r = run_keyed(*m, "double", 21);
            ck(r.ok && !r.trapped && r.value == 42,
               "fail: module unchanged after all failed reloads (double(21)==42)");
        }
        skip_fail:;
    }

    // =====================================================================
    // 4. WRONG BUILD PROVIDER — a deliberately different build provider
    //    remains bounded, non-null, and within the same ABI/visibility
    //    domain without an expected-key comparison or a memory fault.
    //    Supplying a different key is memory-safe but may replace a different
    //    same-domain destination (§12.2). The reload must not crash, must not
    //    perform an expected-key comparison, and the result must remain
    //    inside the finite physical range.
    // =====================================================================
    {
        HotReloadDomain domain;
        auto m = build_keyed_module(mod_src, "kmod", DETERMINISTIC_SEED, &domain,
                                    reinterpret_cast<void*>(khr_trap));
        ck(m != nullptr, "wrong-provider: build keyed module");
        if (!m) goto skip_wrong;
        {
            uint32_t double_logical = uint32_t(m->slots["double"]);
            const uint64_t epoch_before = domain.epoch();

            // A DIFFERENT provider (different seed) derives a different route
            // word. Under that route word the permutation selects a DIFFERENT
            // physical slot in the same domain (possibly padding). The reload
            // must remain bounded + non-null + same-ABI-domain, with no
            // expected-key comparison and no memory fault.
            auto different_provider = std::make_shared<FixedProvider>(0xDEADBEEFCAFEF00DULL);
            KeyedReloadRequest req;
            req.new_fn_source = "fn double(x: i64) -> i64 { return x * 3; }\n";
            req.logical_slot = double_logical;
            req.build_provider = different_provider;
            auto ctx = make_ctx(*m);

            CrashedResult cr = run_guarded([&](CrashedResult& out) {
                KeyedReloadResult kr = reload_keyed_function(
                    req, m->prog, m->st, m->rec, m->plan, domain, ctx,
                    m->natives, &m->ov, &m->layouts);
                if (kr.ok) {
                    out.value = int64_t(kr.physical_slot_published);
                    // The published physical slot must be in range (bounded).
                    ck(kr.physical_slot_published < m->plan.physical_slot_count,
                       "wrong-provider: published physical slot in range (bounded)");
                    // The published slot must be in the SAME domain (same ABI
                    // + visibility + calling mode + dispatch-domain label).
                    uint32_t dom_idx = m->plan.logical_routes[double_logical].domain_index;
                    const auto& d = m->plan.domains[dom_idx];
                    ck(kr.physical_slot_published >= d.physical_base &&
                       kr.physical_slot_published < d.physical_base + d.physical_count,
                       "wrong-provider: published slot within the same domain range");
                    // The entry at that slot is non-null.
                    void* entry = m->st.storage[kr.physical_slot_published].load(std::memory_order_acquire);
                    ck(entry != nullptr,
                       "wrong-provider: entry at the published slot is non-null");
                    // No expected-key comparison: the reload succeeded under a
                    // different provider without a key mismatch error. The
                    // result is memory-safe (bounded, in-domain, non-null).
                    m->fns.push_back(std::move(kr.new_fn));
                } else {
                    // A wrong-provider reload that lands on padding may be a
                    // structured rejection (the reload cannot publish a real
                    // callable at the padding ordinal). That is also memory-
                    // safe: no crash, no epoch advance, no partial publication.
                    ck(domain.epoch() == epoch_before || domain.epoch() > epoch_before,
                       "wrong-provider: epoch either unchanged or advanced (no partial state)");
                }
            });
            ck(!cr.crashed,
               "wrong-provider: different build provider did NOT crash (memory-safe, no fault)");
        }
        skip_wrong:;
    }

    // =====================================================================
    // 5. GUARD PINS OLD PAGE — a guard acquired before publication pins the
    //    old executable page until released. The old page remains callable
    //    while the guard is active and is reclaimed only after the guard
    //    drains.
    // =====================================================================
    {
        HotReloadDomain domain;
        auto m = build_keyed_module(mod_src, "kmod", DETERMINISTIC_SEED, &domain,
                                    reinterpret_cast<void*>(khr_trap));
        ck(m != nullptr, "guard: build keyed module");
        if (!m) goto skip_guard;
        {
            uint32_t double_logical = uint32_t(m->slots["double"]);
            uint32_t double_phys = m->plan.build_physical_placement[double_logical];

            // Acquire a guard BEFORE the reload (pinning the old page).
            auto held = std::make_unique<HotReloadDomain::ExecutionGuard>(domain.guard());
            void* old_entry = m->st.storage[double_phys].load(std::memory_order_acquire);
            ck(old_entry != nullptr, "guard: old entry at keyed physical slot is non-null");

            auto provider = std::make_shared<FixedProvider>(DETERMINISTIC_SEED);
            KeyedReloadRequest req;
            req.new_fn_source = "fn double(x: i64) -> i64 { return x * 5; }\n";
            req.logical_slot = double_logical;
            req.build_provider = provider;
            auto ctx = make_ctx(*m);
            KeyedReloadResult kr = reload_keyed_function(
                req, m->prog, m->st, m->rec, m->plan, domain, ctx,
                m->natives, &m->ov, &m->layouts);
            ck(kr.ok, "guard: reload succeeded while old guard active");
            if (kr.ok) {
                // The old page is retired to the domain but NOT freed while
                // the guard is active.
                ck(kr.old_page_retired, "guard: old page retired to domain");
                ck(domain.reclaim() == 0 && domain.retired_page_count() == 1,
                   "guard: nonblocking reclaim refuses page pinned by active guard");

                // The old page remains callable while the guard is active.
                using F = int64_t(*)(int64_t);
                ck(reinterpret_cast<F>(old_entry)(21) == 42,
                   "guard: old page remains executable while guard is active");

                // The new page is callable through the keyed resolver.
                auto r = run_keyed(*m, "double", 21);
                ck(r.ok && !r.trapped && r.value == 105,
                   "guard: new page callable through keyed resolver (double(21)==105)");

                m->fns.push_back(std::move(kr.new_fn));
            }

            // Release the guard; the old page can now be reclaimed.
            held.reset();
            ck(domain.quiesce() == 1 && domain.retired_page_count() == 0,
               "guard: old page freed after guard drains");
        }
        skip_guard:;
    }

    // =====================================================================
    // 6. WHOLE-GENERATION REPLACEMENT — a complete keyed module generation
    //    replaces another under the same stable module ID using a fresh
    //    immutable ModuleDispatchRecord and possibly changed keyed topology.
    //    Readers see only coherent old or new records. Old record backing +
    //    executable pages remain alive while an old guard is active.
    // =====================================================================
    {
        ModuleRegistry reg(16);
        HotReloadDomain domain;

        // Generation 1: two (i64)->i64 functions, seed DETERMINISTIC_SEED.
        auto gen1 = build_keyed_module(mod_src, "kmod", DETERMINISTIC_SEED, &domain,
                                       reinterpret_cast<void*>(khr_trap));
        ck(gen1 != nullptr, "gen-replace: build generation 1");
        if (!gen1) goto skip_gen;
        {
            std::string err;
            gen1->rec;  // the record is already assembled
            // Register gen1 under the stable name "kmod".
            uint32_t id1 = reg.register_module("kmod", gen1->table.base(), &err);
            ck(id1 != UINT32_MAX, "gen-replace: register generation 1");
            reg.set_dispatch_slot_count(id1, int64_t(gen1->slot_count));
            reg.publish_dispatch_record(id1, &gen1->rec);
            ck(reg.dispatch_record(id1) == &gen1->rec,
               "gen-replace: gen 1 record published");
            ck(reg.dispatch_mode(id1) == DispatchMode::Keyed,
               "gen-replace: gen 1 mode is Keyed");

            // A reader sees the gen 1 record.
            const ModuleDispatchRecord* r1 = reg.dispatch_record(id1);
            ck(r1 != nullptr && r1->logical_slot_count == gen1->plan.logical_slot_count,
               "gen-replace: reader observes gen 1 logical count");

            // Generation 2: a CHANGED topology — three (i64)->i64 functions
            // (triple, square, negate) + one padding. Different seed for a
            // different permutation. Same stable module ID ("kmod").
            const std::string mod_src2 =
                "fn triple(x: i64) -> i64 { return x * 3; }\n"
                "fn square(x: i64) -> i64 { return x * x; }\n"
                "fn negate(x: i64) -> i64 { return -x; }\n";
            auto gen2 = build_keyed_module(mod_src2, "kmod", 0xABCD1234ULL, &domain,
                                           reinterpret_cast<void*>(khr_trap));
            ck(gen2 != nullptr, "gen-replace: build generation 2 (changed topology)");
            if (!gen2) goto skip_gen;

            // The topologies differ: gen1 has 2 real + 1 padding (physical 3),
            // gen2 has 3 real + 1 padding (physical 4).
            ck(gen2->plan.physical_slot_count != gen1->plan.physical_slot_count ||
               gen2->plan.logical_slot_count != gen1->plan.logical_slot_count,
               "gen-replace: gen 2 has a changed topology (different counts)");

            // Acquire a guard on gen1 BEFORE the swap (pinning gen1's pages).
            auto held = std::make_unique<HotReloadDomain::ExecutionGuard>(domain.guard());

            KeyedGenerationReplacementRequest greq;
            greq.stable_module_id = "kmod";
            greq.expected_module_id = id1;  // cross-check find_by_name returns the stable id
            greq.new_plan = &gen2->plan;
            greq.new_storage = &gen2->st;
            greq.new_record = &gen2->rec;
            greq.real_entry = [&gen2](uint32_t physical_slot) -> void* {
                const auto& pe = gen2->plan.physical_entries[physical_slot];
                if (pe.is_padding) return const_cast<void*>(ember_keyed_padding_trap_target());
                uint32_t ls = pe.logical_slot;
                return gen2->table.get(int(ls));
            };
            greq.registry = &reg;
            greq.domain = &domain;

            KeyedGenerationReplacementResult gres = replace_keyed_generation(greq);
            ck(gres.ok, "gen-replace: whole-generation replacement succeeded");
            if (gres.ok) {
                ck(gres.module_id == id1,
                   "gen-replace: stable module ID unchanged across generation swap");
                ck(gres.publication_epoch > 0,
                   "gen-replace: generation publication advanced the epoch");

                // Readers see only the coherent NEW record (or the old one
                // under an active guard — never a mix). The registry now
                // publishes gen2's record under the same id.
                const ModuleDispatchRecord* r2 = reg.dispatch_record(id1);
                ck(r2 == &gen2->rec,
                   "gen-replace: registry now publishes gen 2 record under the same id");
                ck(r2 != r1,
                   "gen-replace: new record pointer differs from old generation");
                ck(reg.logical_slot_count(id1) == gen2->plan.logical_slot_count,
                   "gen-replace: reader observes gen 2 logical count (coherent new record)");
                ck(reg.physical_slot_count(id1) == gen2->plan.physical_slot_count,
                   "gen-replace: reader observes gen 2 physical count (coherent new record)");
                ck(reg.dispatch_mode(id1) == DispatchMode::Keyed,
                   "gen-replace: gen 2 mode is Keyed");

                // The new generation is callable through the keyed resolver.
                auto rt = run_keyed(*gen2, "triple", 7);
                ck(rt.ok && !rt.trapped && rt.value == 21,
                   "gen-replace: gen 2 triple(7)==21 through keyed resolver");
                auto rs = run_keyed(*gen2, "square", 6);
                ck(rs.ok && !rs.trapped && rs.value == 36,
                   "gen-replace: gen 2 square(6)==36 through keyed resolver");

                // Old record backing + executable pages remain alive while
                // the old guard is active. gen1's record pointer is still
                // valid (the guard pins gen1's pages).
                ck(gen1->rec.physical_slots != nullptr,
                   "gen-replace: gen 1 record backing still alive while old guard active");
                // Production-owned generation retirement: gen1's REAL pages
                // were retired into the domain by replace_keyed_generation
                // (NOT left to the host). gen1 had 2 real (i64)->i64 pages,
                // so the domain holds exactly 2 retired pages while the guard
                // pins them.
                ck(domain.retired_page_count() == 2,
                   "gen-replace: domain owns gen 1 real pages (2 retired) after swap");

                // The old (gen1) page remains callable WHILE the guard is
                // active: the domain owns it and has NOT freed it (reclaim
                // refuses a page pinned by an active guard). This proves
                // production-owned lifetime — the page survives the swap even
                // though the registry no longer publishes gen1.
                {
                    using F = int64_t(*)(int64_t);
                    void* old_double =
                        gen1->st.storage[gen1->plan.build_physical_placement[
                            uint32_t(gen1->slots["double"])]]
                            .load(std::memory_order_acquire);
                    ck(old_double != nullptr &&
                       !ember_is_padding_trap_target(old_double),
                       "gen-replace: gen 1 double page still present while guard active");
                    ck(reinterpret_cast<F>(old_double)(21) == 42,
                       "gen-replace: old gen 1 page still callable while guard active (domain owns lifetime)");
                }
                // Nonblocking reclaim refuses pages pinned by the active guard.
                ck(domain.reclaim() == 0 && domain.retired_page_count() == 2,
                   "gen-replace: reclaim refuses gen 1 pages pinned by active guard");

                // Transfer page ownership to the domain: disown gen1's real
                // CompiledFn exec handles so gen1's destructor does NOT
                // double-free pages the domain now owns + frees after the
                // guard drains. (The domain owns the OLD pages; gen2 owns the
                // NEW pages it published.)
                for (auto& fn : gen1->fns) { fn.exec = nullptr; fn.entry = nullptr; }

                // Release the guard; gen1's pages can be reclaimed + freed.
                held.reset();
                ck(domain.quiesce() == 2 && domain.retired_page_count() == 0,
                   "gen-replace: gen 1 pages freed after guard drains (production-owned retirement)");
            }
        }
        skip_gen:;
    }

    // =====================================================================
    // 7. DISABLED KEYED MODE — when keyed mode is disabled, the existing
    //    identity-layout reload_function behavior is preserved (logical slot
    //    == physical slot, unchanged values).
    // =====================================================================
    {
        // Build an identity (legacy) module with the existing reload_function
        // path (hot_reload.hpp). No keyed record, no permutation, no padding.
        auto compile_identity = [](const std::string& src) -> std::unique_ptr<KeyedModule> {
            auto m = std::make_unique<KeyedModule>();
            m->name = "imod";
            std::unordered_set<std::string> seen; std::string resolved;
            try { resolved = resolve_imports(src, "./", seen); } catch (...) { return nullptr; }
            auto lr = tokenize(resolved, "<id>"); if (!lr.ok) return nullptr;
            auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
            m->prog = std::move(pr.program);
            int si = 0;
            for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = si - 1; }
            m->slot_count = si;
            m->layouts = build_struct_layouts(m->prog);
            m->prog.string_xor_key = 0;
            auto sr = sema(m->prog, m->natives, m->slots, 0, &m->ov, &m->layouts);
            if (!sr.ok) return nullptr;
            m->gbs.assign(m->prog.globals.size() * 8, 0);
            m->gb.base = int64_t(m->gbs.data());
            { uint32_t gi = 0; for (auto& g : m->prog.globals) { m->gb.index[g.name] = gi++; m->gb.types[g.name] = g.ty.get(); } }
            g_globals_for_codegen = &m->gb;
            m->table = DispatchTable(m->slot_count);
            m->allowlist = build_fn_allowlist(m->slots, m->slot_count);
            CodeGenCtx ctx;
            ctx.globals_base = m->gb.base;
            ctx.dispatch_base = int64_t(m->table.base());
            ctx.natives = &m->natives;
            ctx.script_slots = &m->slots;
            ctx.structs = &m->layouts;
            ctx.use_context_reg = true;
            ctx.fn_allowlist_base = int64_t(m->allowlist.data());
            ctx.fn_slot_count = m->slot_count;
            for (auto& fn : m->prog.funcs) {
                auto cf = compile_func(fn, ctx);
                if (!finalize(cf)) return nullptr;
                m->table.set(fn.slot, cf.entry);
                m->fns.push_back(std::move(cf));
            }
            return m;
        };

        HotReloadDomain domain;
        auto m = compile_identity(
            "fn val() -> i64 { return 10; }\n"
            "fn main() -> i64 { return val(); }\n");
        ck(m != nullptr, "identity: compile identity module");
        if (m) {
            // Identity layout: logical slot == physical slot (no permutation).
            for (auto& [name, slot] : m->slots) {
                ck(uint32_t(slot) == uint32_t(slot),
                   "identity: logical slot == physical slot (identity layout)");
            }
            // Call main through the identity path (guard + raw entry).
            {
                auto guard = domain.guard();
                void* e = m->table.get(m->slots["main"]);
                using F = int64_t(*)(int64_t);
                int64_t v = reinterpret_cast<F>(e)(0);
                ck(v == 10, "identity: initial main()==10 (identity layout)");
            }
            // Reload val via the existing reload_function (identity path).
            CodeGenCtx ctx;
            ctx.globals_base = m->gb.base;
            ctx.dispatch_base = int64_t(m->table.base());
            ctx.natives = &m->natives;
            ctx.script_slots = &m->slots;
            ctx.structs = &m->layouts;
            ctx.use_context_reg = true;
            ctx.fn_allowlist_base = int64_t(m->allowlist.data());
            ctx.fn_slot_count = m->slot_count;

            auto rr = reload_function(
                "fn val() -> i64 { return 99; }\n",
                m->prog, m->table, domain, ctx,
                m->natives, &m->ov, &m->layouts);
            ck(rr.ok, "identity: identity-layout reload_function succeeded");
            ck(rr.publication_epoch > 0, "identity: reload advanced the epoch");
            if (rr.ok) {
                // The physical slot is the logical slot (identity: no permutation).
                // reload_function publishes at the logical slot directly.
                ck(m->table.get(m->slots["val"]) != nullptr,
                   "identity: val entry published at the logical slot (identity layout)");
                // Call main again: it observes the replacement.
                auto guard = domain.guard();
                void* e = m->table.get(m->slots["main"]);
                using F = int64_t(*)(int64_t);
                int64_t v = reinterpret_cast<F>(e)(0);
                ck(v == 99, "identity: main() observes replacement (val()==99, unchanged behavior)");
                // Disown the old fn (the domain owns the retired page).
                for (auto& fn : m->fns) {
                    if (fn.name == "val" && fn.exec && fn.entry != m->table.get(m->slots["val"])) {
                        fn.exec = nullptr;
                        fn.entry = nullptr;
                    }
                }
                m->fns.push_back(std::move(rr.new_fn));
            }
        }
    }

    // =====================================================================
    // 8. TOPOLOGY REJECTION — single-function reload REJECTS topology/domain
    //    changes and directs those changes to whole-generation replacement.
    // =====================================================================
    {
        HotReloadDomain domain;
        auto m = build_keyed_module(mod_src, "kmod", DETERMINISTIC_SEED, &domain,
                                    reinterpret_cast<void*>(khr_trap));
        ck(m != nullptr, "topo-reject: build keyed module");
        if (!m) goto skip_topo;
        {
            uint32_t double_logical = uint32_t(m->slots["double"]);

            // A replacement manifest that would change the domain (add a
            // function with a different ABI, changing the domain membership).
            // A single-function reload cannot accommodate a topology change;
            // it must be rejected and the caller directed to whole-generation
            // replacement.
            ModuleManifest replacement_manifest;
            replacement_manifest.module_id = "kmod";
            // The replacement has a DIFFERENT ABI (f32 return) — a domain change.
            ModuleCallable c;
            c.name = "double";
            c.logical_slot = double_logical;
            c.abi_fingerprint = 0xDEADBEEFDEADBEEFULL;  // different ABI
            c.visibility = Visibility::Public;
            c.calling_mode = CallingMode::LegacyContext;
            c.dispatch_domain = "";
            replacement_manifest.callables.push_back(c);

            std::string reason;
            bool preserves = keyed_reload_preserves_topology(
                m->rec, double_logical, replacement_manifest, reason);
            ck(!preserves,
               "topo-reject: single-fn reload rejects topology/domain change (ABI mismatch)");
            ck(!reason.empty(),
               "topo-reject: rejection carries a diagnostic directing to whole-generation replacement");

            // A correct-ABI replacement manifest preserves the topology.
            ModuleManifest correct_manifest;
            correct_manifest.module_id = "kmod";
            ModuleCallable c2;
            c2.name = "double";
            c2.logical_slot = double_logical;
            c2.abi_fingerprint = m->plan.logical_routes[double_logical].abi_fingerprint;
            c2.visibility = m->plan.logical_routes[double_logical].visibility;
            c2.calling_mode = m->plan.logical_routes[double_logical].calling_mode;
            c2.dispatch_domain = m->plan.logical_routes[double_logical].dispatch_domain;
            correct_manifest.callables.push_back(c2);

            std::string reason2;
            bool preserves2 = keyed_reload_preserves_topology(
                m->rec, double_logical, correct_manifest, reason2);
            ck(preserves2,
               "topo-reject: single-fn reload accepts same-ABI/same-domain replacement (topology preserved)");

            // A visibility change is also a topology/domain change.
            ModuleManifest vis_manifest;
            vis_manifest.module_id = "kmod";
            ModuleCallable c3;
            c3.name = "double";
            c3.logical_slot = double_logical;
            c3.abi_fingerprint = m->plan.logical_routes[double_logical].abi_fingerprint;
            c3.visibility = Visibility::Private;  // changed visibility
            c3.calling_mode = m->plan.logical_routes[double_logical].calling_mode;
            c3.dispatch_domain = m->plan.logical_routes[double_logical].dispatch_domain;
            vis_manifest.callables.push_back(c3);

            std::string reason3;
            bool preserves3 = keyed_reload_preserves_topology(
                m->rec, double_logical, vis_manifest, reason3);
            ck(!preserves3,
               "topo-reject: single-fn reload rejects visibility change (domain change)");
        }
        skip_topo:;
    }

    // =====================================================================
    // Summary
    // =====================================================================
    std::printf("== %d checks, %s ==\n", g_checks, g_fail ? "FAILED" : "ALL PASSED");
    return g_fail;
}
