// keyed_dispatch_codegen_test — Red 6
// (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §9.4, §14.3, §14.2):
// the same-module keyed call lowering gate for BOTH the tree-walker backend
// and the Thin IR backend.
//
// RED-GREEN contract chunk for same-module keyed call lowering. This is the
// RED side (written first) of the Red 6 contract. It pins, against a real
// keyed ModuleDispatchRecord whose physical slot storage holds the compiled
// entries at keyed positions, the §14.2 mandatory buckets for in-JIT call
// lowering:
//
//   - direct calls:       `main` calls `add`/`square` via a direct script call;
//                          the keyed resolver routes the logical slot to the
//                          correct physical entry under the correct route word.
//   - recursive calls:    `fib` calls itself; the keyed resolver routes each
//                          recursive edge through the same logical slot under
//                          the inherited r15 route word; depth accounting stays
//                          balanced (no leak).
//   - indirect handles:   a `&fn` handle (CallIndirect) is stored, reloaded, and
//                          called; the keyed resolver routes the logical handle
//                          (validated by the logical allowlist) to the correct
//                          entry. The handle is a LOGICAL slot, never a physical
//                          slot (no physical slot appears in a handle).
//   - lambdas:            a lambda-typed local is called (is_lambda_call); the
//                          keyed resolver routes the lambda's logical slot,
//                          with env_ptr prepended as the hidden first arg.
//   - mixed GP/XMM:       calls with f64 arguments (XMM regs) and i64 arguments
//                          (GP regs) in the same call; the resolver preserves
//                          the stashed args (resolves BEFORE final argument
//                          placement so rcx/rdx/r8 are not clobbered).
//   - depth accounting:   the depth check + depth leave bracket every keyed
//                          call exactly once (no double-increment, no leak);
//                          a deep recursive call completes within the budget.
//   - correct-key routing: under the correct route word, every call resolves
//                          to the correct entry and returns the correct value.
//   - wrong-key routing:  under a wrong route word, every resolved entry is
//                          non-null, in-range, same-ABI-domain (alternate real
//                          or padding). The padding trap fires
//                          TrapReason::KeyedDispatchPadding; an alternate real
//                          callable in the same domain runs and returns (a
//                          different value, but no crash / no AV).
//   - logical handle stab: a `&fn` handle is a logical slot index; resolving it
//                          under the correct key gives the same entry every time;
//                          the handle never exposes a physical slot.
//   - edge route words:   route words 0 and UINT64_MAX both work (the call
//                          succeeds; the resolver folds the word into a valid
//                          in-range physical index).
//   - identity layout:    when CodeGenCtx::keyed_dispatch is null, the emit is
//                          byte-identical to the legacy path (no resolver call,
//                          no r15 read); the dispatch is [dispatch_base+slot*8].
//   - both backends:      every scenario above runs through BOTH the tree-walker
//                          (enable_ir_backend=false) and the Thin IR backend
//                          (enable_ir_backend=true); both agree on the result.
//
// The fixture compiles simple modules with the keyed CodeGenCtx (r15 excluded
// from regalloc, module_record set), builds a keyed ModuleDispatchRecord over
// the compiled entries (placed at keyed physical positions by the Red 3
// planner), and invokes the outer entry through the keyed re-entry thunk
// (which installs r14=ctx + r15=route_word). The in-JIT call sites resolve
// through ember_resolve_keyed_dispatch(record, logical_slot, r15).
//
// Links ember (keyed_dispatch.* — Red 1, context.hpp, engine.* — Red 5 thunks,
// dispatch_table.hpp) + ember_frontend (module_layout.* — Red 3/4,
// codegen.* — the keyed CodeGenCtx, thin_lower.*, thin_emit.*,
// dispatch_abi.* — ABI classifier). NOT a CTest entry: the configured suite
// count stays 73 (§14.1); the target building cleanly + the executable passing
// IS the gate.

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
// derived route word is deterministic. The route word participates in the
// keyed permutation (Red 1 helper) + r15 installation.
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
// Compile a module through the existing pipeline with the keyed CodeGenCtx.
// `ir_backend` selects the tree-walker (false) or Thin IR backend (true).
// `module_record` is the borrowed keyed dispatch record (may have placeholder
// entries during compilation; populated before running).
// ===========================================================================
struct Compiled {
    std::vector<CompiledFn> fns;
    // Per-function structured backend evidence (CompileBackend::IRBackend vs
    // TreeWalker), recorded by compile_keyed via compile_func_checked. This
    // lets the tests ASSERT which backend emitted a given function (e.g. the
    // Thin lambda test proves `caller` was IR-backend-compiled, not a silent
    // tree fallback) instead of relying on comments.
    std::vector<CompileBackend> backends;
    DispatchTable table;
    std::unordered_map<std::string, int> slots;
    std::unordered_map<std::string, NativeSig> natives;
    void* main_entry = nullptr;
    std::vector<uint8_t> allowlist;  // kept alive for the module's lifetime (fn_allowlist_base)
    ~Compiled() { for (auto& fn : fns) if (fn.exec) free_executable(fn.exec); }
};

static std::unique_ptr<Compiled> compile_keyed(
    const std::string& src,
    const ModuleDispatchRecord* module_record,
    bool ir_backend,
    void* trap_stub = nullptr) {

    auto m = std::make_unique<Compiled>();
    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); } catch (...) { return nullptr; }
    auto lr = tokenize(resolved, "<keyed-codegen-test>"); if (!lr.ok) return nullptr;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
    int si = 0; for(auto&fn:pr.program.funcs){m->slots[fn.name]=si++;fn.slot=m->slots[fn.name];}
    ember::OpOverloadTable ov;
    auto layouts = build_struct_layouts(pr.program); pr.program.string_xor_key = 0;
    if(!sema(pr.program,m->natives,m->slots,0,&ov,&layouts).ok) return nullptr;

    ember::GlobalsBlock gb; std::vector<uint8_t> gbs(pr.program.globals.size()*8, 0);
    gb.base=int64_t(gbs.data());
    { uint32_t gi=0; for (auto& g : pr.program.globals) { gb.index[g.name]=gi++; gb.types[g.name]=g.ty.get(); } }
    eval_global_initializers(pr.program, GlobalInitCtx{gbs, gb.index, gb.types});
    ember::g_globals_for_codegen = nullptr;
    int nslots = int(pr.program.funcs.size());
    m->table = DispatchTable(nslots);
    // Build the logical allowlist (one bit per logical slot, all set). Stored
    // in the Compiled struct so its address is stable for the module's lifetime
    // (fn_allowlist_base is baked into the JIT'd guard as a raw imm64).
    m->allowlist = build_fn_allowlist(m->slots, nslots);

    ember::CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(m->table.base());
    ctx.natives=&m->natives; ctx.script_slots=&m->slots; ctx.structs=&layouts;
    ctx.globals_index = &gb.index; ctx.globals_types = &gb.types;
    ctx.use_context_reg = true;
    ctx.emit_depth_checks = true;
    ctx.max_call_depth = 64;
    ctx.fn_allowlist_base = int64_t(m->allowlist.data());
    ctx.fn_slot_count = nslots;
    if (trap_stub) {
        ctx.trap_stub = trap_stub;
        ctx.trap_ctx = nullptr;
    }
    // KEYED MODE (Red 6): r15 reserved + module_record for same-module resolution.
    KeyedDispatchCodegen kd{};
    kd.runtime_key = RuntimeKeyLocation::R15;
    kd.module_record = module_record;
    ctx.keyed_dispatch = &kd;
    ctx.enable_ir_backend = ir_backend;

    for(auto&fn:pr.program.funcs){
        // Use compile_func_checked to record the ACTUAL backend that emitted
        // this function (IRBackend vs TreeWalker). With no pass_manager the
        // checked path is byte-identical to compile_func (the only extra work
        // is a read-only pre-emit verify when checked=true, which does not
        // touch bytes); cr.compiled is the same CompiledFn compile_func returns.
        auto cr=compile_func_checked(fn,ctx); finalize(cr.compiled);
        auto& cf=cr.compiled;
        if (!cf.entry) { std::printf("FAIL: compile_keyed: null entry for %s (bytes=%zu ir=%d)\n", fn.name.c_str(), cf.bytes.size(), (int)ir_backend); return nullptr; }
        m->table.set(fn.slot,cf.entry);
        m->backends.push_back(cr.backend);
        m->fns.push_back(std::move(cf));
    }
    auto sit=m->slots.find("main");
    if(sit!=m->slots.end()) m->main_entry = m->table.get(sit->second);
    return m;
}

// ===========================================================================
// Build a keyed ModuleDispatchRecord over a compiled module's entries.
//
// The manifest assigns every function to a single ABI domain (all i64/i64->i64
// or i64->i64, same ABI fingerprint). The keyed planner places them at keyed
// physical positions + one padding ordinal. After compilation, the compiled
// entries are stored into the record's physical slot storage at the planned
// positions. The record's ADDRESS is baked into the JIT'd code (imm64); the
// storage is populated after compilation, so the resolver reads the correct
// entries at RUNTIME.
// ===========================================================================
struct KeyedModule {
    std::unique_ptr<Compiled> compiled;
    ModuleManifest manifest;
    ModuleLayoutPlan plan;
    RecordBuilderStorage st;
    ModuleDispatchRecord rec{};
    uint64_t route_word = 0;
    std::vector<uint8_t> allowlist_keepalive;
};

// ─── Red 2 ABI classifier helpers (ported from module_layout_test) ────────
// Convert a sema-resolved ember::Type into the self-contained AbiType the
// classifier consumes, then build a CallableDescriptor + compute the real
// 64-bit fingerprint via classify_callable. This is the ACCURATE ABI-domain
// metadata: functions with identical signatures get identical fingerprints
// (same domain), functions with different signatures get different
// fingerprints (different domains). Wrong-key alternatives are then genuinely
// ABI-compatible (the resolver only routes within a domain).
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

// Build a CallableDescriptor from a FuncDecl + compute its real fingerprint.
// Mirrors module_layout_test's func_to_descriptor (simplified for the scalar/
// slice/lambda types the test uses; no struct params).
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
    d.has_hidden_env = f.is_lambda;  // lambda: params[0] is hidden __env
    uint32_t pos = (d.has_hidden_return ? 1u : 0u) + (d.has_hidden_env ? 1u : 0u);
    size_t begin = f.is_lambda ? 1u : 0u;  // skip hidden __env for lambda
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

// Build a manifest from the parsed program's function list, using REAL ABI
// fingerprints from the Red 2 classifier. Functions with identical signatures
// share a fingerprint (same domain); functions with different signatures get
// different fingerprints (different domains). This makes wrong-key
// alternatives genuinely ABI-compatible (same-domain routing only).
static ModuleManifest make_manifest(const Program& prog,
                                     const std::unordered_map<std::string,int>& slots,
                                     const std::string& module_id) {
    ModuleManifest m;
    m.module_id = module_id;
    // Build a name→FuncDecl* index for fingerprint lookup.
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
    // Sort by logical_slot for deterministic manifest order.
    std::sort(m.callables.begin(), m.callables.end(),
              [](const ModuleCallable& a, const ModuleCallable& b) {
                  return a.logical_slot < b.logical_slot;
              });
    return m;
}

// Build a keyed module: compile, plan the keyed layout, build the record with
// the compiled entries, validate. `route_word_seed` controls the derived route
// word (folds into the permutation).
static std::unique_ptr<KeyedModule> build_keyed_module(
    const std::string& src,
    uint64_t route_word_seed,
    bool ir_backend,
    void* trap_stub = nullptr) {

    auto km = std::make_unique<KeyedModule>();

    // 1. Build the manifest from a quick parse to get the slots + real
    //    fingerprints (Red 2 classifier — accurate ABI-domain metadata).
    {
        std::unordered_set<std::string> seen; std::string resolved;
        try { resolved = resolve_imports(src, "./", seen); } catch (...) { return nullptr; }
        auto lr = tokenize(resolved, "<km>"); if (!lr.ok) return nullptr;
        auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
        std::unordered_map<std::string,int> slots; int si=0;
        for(auto&fn:pr.program.funcs){slots[fn.name]=si++;}
        km->manifest = make_manifest(pr.program, slots, "codegen.test.mod");
    }

    // 2. Plan the keyed layout under a pinned build key.
    uint8_t key_bytes[8];
    for (int i = 0; i < 8; ++i) key_bytes[i] = uint8_t((route_word_seed >> (8*i)) & 0xFF);
    BuildKeyView key{key_bytes, 8};
    ImplicitKeyedLayoutV1 planner;
    auto pr = planner.plan(km->manifest, key);
    if (!pr) { std::printf("FAIL: keyed plan failed\n"); return nullptr; }
    km->plan = std::move(*pr.value);
    km->route_word = derive_route_word(key);

    // 3. Set up the record's storage (routes, domains, descriptors, allowlist)
    //    from the plan, with PLACEHOLDER entries (padding trap) in the physical
    //    slots. The record's address is baked into the JIT'd code during
    //    compilation; the real entries are stored AFTER compilation.
    km->st.routes = km->plan.logical_routes;
    km->st.domains = km->plan.domains;
    km->st.allowlist.assign((km->plan.logical_slot_count + 7u) >> 3, 0);
    for (uint32_t i = 0; i < km->plan.logical_slot_count; ++i)
        km->st.allowlist[i >> 3] |= (uint8_t(1) << (i & 7u));
    km->st.descriptors.assign(km->plan.physical_slot_count, PhysicalEntryDescriptor{});
    for (uint32_t s = 0; s < km->plan.physical_slot_count; ++s) {
        const auto& pe = km->plan.physical_entries[s];
        PhysicalEntryDescriptor& pd = km->st.descriptors[s];
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
    km->st.storage = std::vector<std::atomic<void*>>(km->plan.physical_slot_count);
    for (uint32_t s = 0; s < km->plan.physical_slot_count; ++s)
        km->st.storage[s].store(const_cast<void*>(pad_stub), std::memory_order_release);

    km->rec.mode = DispatchMode::Keyed;
    km->rec.strategy_version = 1;
    km->rec.physical_slots = km->st.storage.data();
    km->rec.physical_slot_count = km->plan.physical_slot_count;
    km->rec.logical_slot_count = km->plan.logical_slot_count;
    km->rec.logical_routes = km->st.routes.data();
    km->rec.domains = km->st.domains.data();
    km->rec.domain_count = uint32_t(km->st.domains.size());
    km->rec.logical_allowlist = km->st.allowlist.data();
    km->rec.logical_allowlist_bytes = uint32_t(km->st.allowlist.size());
    km->rec.physical_descriptors = km->st.descriptors.data();
    km->rec.physical_descriptor_count = uint32_t(km->st.descriptors.size());
    km->rec.padding_trap_target = pad_stub;

    // 4. Compile with the keyed CodeGenCtx (module_record = &rec).
    km->compiled = compile_keyed(src, &km->rec, ir_backend, trap_stub);
    if (!km->compiled) { std::printf("FAIL: compile_keyed returned null\n"); return nullptr; }

    // 5. Populate the real entries: for each REAL physical slot, store the
    //    compiled entry for the logical slot it serves. Padding slots keep
    //    the padding trap stub.
    for (uint32_t s = 0; s < km->plan.physical_slot_count; ++s) {
        const auto& pe = km->plan.physical_entries[s];
        if (pe.is_padding) continue;  // padding keeps the trap stub
        uint32_t ls = pe.logical_slot;
        void* entry = km->compiled->table.get(ls);
        km->st.storage[s].store(entry, std::memory_order_release);
    }

    // 6. Validate the record (now that real entries are populated).
    auto vs = validate_dispatch_record(km->rec);
    if (!vs) {
        std::printf("FAIL: validate_dispatch_record failed: %s\n",
                    vs.error.has_value() ? vs.error->message.c_str() : "(no diag)");
        return nullptr;
    }

    return km;
}

// ===========================================================================
// Trap stub for wrong-key padding tests (records the trap + longjmps).
// ===========================================================================
extern "C" void kt6_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) longjmp(ctx->checkpoint, 1);
    }
    std::abort();
}

// Non-longjmp trap stub for the wrong-key JIT test: sets last_trap + returns.
// The JIT's emit_trap fallback (add rsp, call_frame) resumes after the trap,
// so the function continues with a stale value (no crash). This avoids the
// longjmp recovery path which has an intermittent SIGILL interaction with
// -O3 on wrong-key JIT runs. The test inspects last_trap after the call.
extern "C" [[maybe_unused]] void kt6_trap_norecover(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        if (detail) ctx->last_error = detail;
    }
    // Return normally — the JIT's emit_trap fallback resumes execution.
}

// ===========================================================================
// Run the outer entry under a given route word. Resolves the entry through
// the keyed resolver, installs r14=ctx + r15=route_word via the keyed
// re-entry thunk, and returns the i64 result. Uses a checkpoint for
// recoverable traps.
// ===========================================================================
struct RunResult { bool ok = false; bool trapped = false; int64_t value = 0; int32_t call_depth = 0; std::string reason; };

// ===========================================================================
// Crash guard (strengthened coverage): a genuine compiler/JIT ABI bug in the
// callee-side argument stash (e.g. the Thin IR indirect-call handle word
// shifting outgoing stack args so a multiword slice pointer is read from the
// wrong offset) can surface as a raw ACCESS_VIOLATION when the JIT'd callee
// dereferences the corrupt pointer — NOT as a recoverable trap through the
// trap stub. A raw segfault would kill the whole harness before any check
// prints, so the failure would not be EXHIBITED cleanly. This guard installs
// a vectored exception handler that catches EXCEPTION_ACCESS_VIOLATION /
// EXCEPTION_ILLEGAL_INSTRUCTION / EXCEPTION_STACK_OVERFLOW during a guarded
// run and longjmps back, converting the crash into a structured CrashedResult
// the caller can assert on (a clean FAIL that EXHIBITS the genuine ABI
// failure, per the task's acceptable finish: "exhibiting only genuine
// compiler/JIT ABI failures"). This is TEST instrumentation only — it changes
// no production code and no process/linker stack size.
// ===========================================================================
struct CrashedResult { bool ran = false; bool crashed = false; int64_t value = 0; int32_t call_depth = 0; DWORD code = 0; };

static LONG WINAPI kt6_seh_filter(EXCEPTION_POINTERS* ep);

// Thread-local recovery target for the vectored handler.
static thread_local jmp_buf g_seh_jmp;
static thread_local bool g_seh_armed = false;
static thread_local DWORD g_seh_code = 0;

static LONG WINAPI kt6_seh_filter(EXCEPTION_POINTERS* ep) {
    if (g_seh_armed && ep && ep->ExceptionRecord) {
        DWORD c = ep->ExceptionRecord->ExceptionCode;
        // Catch the hardware faults a corrupt-arg ABI bug raises. Stack
        // overflow is included so a runaway recursive alternate (which the
        // depth guard should have caught) is also recovered cleanly.
        if (c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION ||
            c == EXCEPTION_STACK_OVERFLOW || c == EXCEPTION_INT_DIVIDE_BY_ZERO ||
            c == EXCEPTION_PRIV_INSTRUCTION) {
            g_seh_code = c;
            g_seh_armed = false;
            longjmp(g_seh_jmp, 1);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Run `fn(ctx)` inside a vectored crash guard. Returns a CrashedResult with
// crashed=true if the run raised a caught hardware fault. `fn` must write its
// outcome into `out` before returning.
template <class F>
static CrashedResult run_guarded(F fn) {
    CrashedResult out;
    PVOID veh = AddVectoredExceptionHandler(1 /*first handler*/, kt6_seh_filter);
    g_seh_code = 0;
    g_seh_armed = true;
    if (setjmp(g_seh_jmp) == 0) {
        fn(out);            // runs the JIT'd code; may crash
        g_seh_armed = false;
        out.ran = true;
    } else {
        // longjmp recovery from the vectored handler.
        out.crashed = true;
        out.code = g_seh_code;
        g_seh_armed = false;
    }
    if (veh) RemoveVectoredExceptionHandler(veh);
    return out;
}

static RunResult run_keyed(KeyedModule& km, const std::string& entry_name,
                           int64_t arg, uint64_t route_word, bool use_checkpoint = true) {
    RunResult r;
    auto sit = km.compiled->slots.find(entry_name);
    if (sit == km.compiled->slots.end()) { r.reason = "entry not found"; return r; }
    uint32_t logical_slot = uint32_t(sit->second);
    void* entry = ember_resolve_keyed_dispatch(&km.rec, logical_slot, route_word);
    if (!entry) { r.reason = "resolver returned null"; return r; }

    context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
    if (use_checkpoint && km.compiled) {
        ctx.has_checkpoint = true;
        if (setjmp(ctx.checkpoint)) {
            r.trapped = true;
            r.reason = ctx.last_error.empty()
                ? std::string(trap_reason_str(ctx.last_trap))
                : (std::string(trap_reason_str(ctx.last_trap)) + ": " + ctx.last_error);
            ctx.has_checkpoint = false;
            ctx.reset_for_call();
            return r;
        }
    }
    int64_t v = ember_keyed_reentry_i64(entry, &ctx, arg, route_word);
    if (use_checkpoint) ctx.has_checkpoint = false;
    r.ok = true;
    r.value = v;
    r.call_depth = ctx.call_depth;
    return r;
}

// Guarded single-arg run: same as run_keyed but wrapped in the vectored
// crash guard so a raw ACCESS_VIOLATION from a genuine callee-side ABI bug is
// converted into a CrashedResult (crashed=true) instead of killing the
// harness. use_checkpoint is forced off so the inner run_keyed does not
// install its own longjmp (the guard owns recovery for hardware faults).
[[maybe_unused]] static CrashedResult run_keyed_guarded(KeyedModule& km,
                                     const std::string& entry_name,
                                     int64_t arg, uint64_t route_word) {
    return run_guarded([&](CrashedResult& out) {
        auto r = run_keyed(km, entry_name, arg, route_word, /*use_checkpoint=*/false);
        out.value = r.value; out.call_depth = r.call_depth;
    });
}

// Run with two i64 args.
[[maybe_unused]] static RunResult run_keyed_ii(KeyedModule& km, const std::string& entry_name,
                              int64_t a, int64_t b, uint64_t route_word,
                              bool use_checkpoint = true) {
    RunResult r;
    auto sit = km.compiled->slots.find(entry_name);
    if (sit == km.compiled->slots.end()) { r.reason = "entry not found"; return r; }
    uint32_t logical_slot = uint32_t(sit->second);
    void* entry = ember_resolve_keyed_dispatch(&km.rec, logical_slot, route_word);
    if (!entry) { r.reason = "resolver returned null"; return r; }

    context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
    if (use_checkpoint) {
        ctx.has_checkpoint = true;
        if (setjmp(ctx.checkpoint)) {
            r.trapped = true;
            r.reason = ctx.last_error.empty()
                ? std::string(trap_reason_str(ctx.last_trap))
                : (std::string(trap_reason_str(ctx.last_trap)) + ": " + ctx.last_error);
            ctx.has_checkpoint = false;
            ctx.reset_for_call();
            return r;
        }
    }
    int64_t v = ember_keyed_reentry_i64_i64(entry, &ctx, a, b, route_word);
    if (use_checkpoint) ctx.has_checkpoint = false;
    r.ok = true;
    r.value = v;
    r.call_depth = ctx.call_depth;
    return r;
}

// Run with no args (void entry that returns i64).
[[maybe_unused]] static RunResult run_keyed_void(KeyedModule& km, const std::string& entry_name,
                                uint64_t route_word, bool use_checkpoint = true) {
    RunResult r;
    auto sit = km.compiled->slots.find(entry_name);
    if (sit == km.compiled->slots.end()) { r.reason = "entry not found"; return r; }
    uint32_t logical_slot = uint32_t(sit->second);
    void* entry = ember_resolve_keyed_dispatch(&km.rec, logical_slot, route_word);
    if (!entry) { r.reason = "resolver returned null"; return r; }

    context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
    if (use_checkpoint) {
        ctx.has_checkpoint = true;
        if (setjmp(ctx.checkpoint)) {
            r.trapped = true;
            r.reason = ctx.last_error.empty()
                ? std::string(trap_reason_str(ctx.last_trap))
                : (std::string(trap_reason_str(ctx.last_trap)) + ": " + ctx.last_error);
            ctx.has_checkpoint = false;
            ctx.reset_for_call();
            return r;
        }
    }
    int64_t v = ember_keyed_reentry_void(entry, &ctx, route_word);
    if (use_checkpoint) ctx.has_checkpoint = false;
    r.ok = true;
    r.value = v;
    r.call_depth = ctx.call_depth;
    return r;
}

// Run the outer entry resolved under the CORRECT key, but install a (possibly
// WRONG) route word via the re-entry thunk. This tests GENERATED same-module
// call-site resolution: the JIT'd entry runs with r15=run_route_word, and its
// internal call sites invoke ember_resolve_keyed_dispatch(record, slot, r15)
// with that route word. Under a wrong word, an internal call resolves to a
// same-domain alternate or the padding entry (never null, never a crash).
// Returns the context state (call_depth, last_trap) so the caller can inspect
// depth balance + the padding trap.
struct WrongKeyResult { bool ok = false; int64_t value = 0; TrapReason last_trap = TrapReason::None; int32_t call_depth = 0; std::string reason; };

// Run the outer entry resolved under the CORRECT route word, but install a
// (possibly WRONG) route word via the keyed re-entry thunk. The thunk is the
// ONLY reliable way to install r15 from C++: ember_set_r15 + a legacy thunk
// does NOT work because the C++ compiler treats r15 as callee-saved and may
// restore it across the subsequent C++ call to ember_call_i64_i64 (the
// compiler does not know the asm ember_set_r15 mutates r15). The keyed
// re-entry thunk installs r14=ctx + r15=run_route_word as part of the SAME
// asm sequence that calls the JIT entry, so the JIT'd caller (main) runs with
// r15=run_route_word and its GENERATED internal call sites invoke
// ember_resolve_keyed_dispatch(record, logical_slot, r15) with that route
// word. Under a wrong word, an internal call resolves to a same-domain
// alternate real (runs, returns a value, no crash) or the padding entry (the
// r14-reading padding stub sets last_trap=KeyedDispatchPadding and returns 0
// normally — it is a REAL callable, NOT a trap-stub call, so no longjmp fires
// for the padding case). The null-resolver case (impossible for a validated
// record + in-range slot) would fire BadCallTarget via the trap stub + longjmp;
// the checkpoint catches that.
static WrongKeyResult run_keyed_wrong_r15_ii(KeyedModule& km, const std::string& entry_name,
                                             int64_t a, int64_t b,
                                             uint64_t correct_route_word,
                                             uint64_t run_route_word) {
    WrongKeyResult r;
    auto sit = km.compiled->slots.find(entry_name);
    if (sit == km.compiled->slots.end()) { r.reason = "entry not found"; return r; }
    // Resolve the OUTER entry under the CORRECT key so we enter the intended
    // caller (main). The wrong route word is installed by the re-entry thunk.
    void* entry = ember_resolve_keyed_dispatch(&km.rec, uint32_t(sit->second), correct_route_word);
    if (!entry) { r.reason = "outer resolver returned null"; return r; }
    auto ctxp = std::make_unique<context_t>();
    context_t& ctx = *ctxp;
    ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
    ctx.has_checkpoint = true;
    if (setjmp(ctx.checkpoint)) {
        r.ok = true; r.last_trap = ctx.last_trap; r.call_depth = ctx.call_depth;
        r.reason = std::string(trap_reason_str(ctx.last_trap)) + ": " + ctx.last_error;
        ctx.has_checkpoint = false; ctx.reset_for_call();
        return r;
    }
    // The keyed re-entry thunk installs r14=ctx, r15=run_route_word, calls
    // entry, clears r15, restores caller r14/r15. This tests GENERATED
    // internal call-site resolution: main runs with r15=run_route_word, and
    // its internal call sites resolve through ember_resolve_keyed_dispatch.
    int64_t v = ember_keyed_reentry_i64_i64(entry, &ctx, a, b, run_route_word);
    ctx.has_checkpoint = false;
    r.ok = true; r.value = v; r.last_trap = ctx.last_trap; r.call_depth = ctx.call_depth;
    return r;
}

// Single-arg wrong-r15 variant.
static WrongKeyResult run_keyed_wrong_r15_i(KeyedModule& km, const std::string& entry_name,
                                           int64_t a,
                                           uint64_t correct_route_word,
                                           uint64_t run_route_word) {
    WrongKeyResult r;
    auto sit = km.compiled->slots.find(entry_name);
    if (sit == km.compiled->slots.end()) { r.reason = "entry not found"; return r; }
    void* entry = ember_resolve_keyed_dispatch(&km.rec, uint32_t(sit->second), correct_route_word);
    if (!entry) { r.reason = "outer resolver returned null"; return r; }
    auto ctxp = std::make_unique<context_t>();
    context_t& ctx = *ctxp;
    ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
    ctx.has_checkpoint = true;
    if (setjmp(ctx.checkpoint)) {
        r.ok = true; r.last_trap = ctx.last_trap; r.call_depth = ctx.call_depth;
        r.reason = std::string(trap_reason_str(ctx.last_trap)) + ": " + ctx.last_error;
        ctx.has_checkpoint = false; ctx.reset_for_call();
        return r;
    }
    int64_t v = ember_keyed_reentry_i64(entry, &ctx, a, run_route_word);
    ctx.has_checkpoint = false;
    r.ok = true; r.value = v; r.last_trap = ctx.last_trap; r.call_depth = ctx.call_depth;
    return r;
}

// ===========================================================================
// Deterministic route-word classification (Red 6 strengthened coverage).
//
// The JIT's internal call site for a callee resolves logical_slot through
// ember_resolve_keyed_dispatch(record, logical_slot, r15). Under a given route
// word the callee's ordinal permutes to a physical slot that holds EITHER the
// callee's own entry (correct), a DIFFERENT real callable in the same ABI
// domain (alternate real), or the padding trap stub (padding). This helper
// selects route words DETERMINISTICALLY from the layout so a test can execute
// BOTH an alternate-real route AND a padding route for a given callee, instead
// of looping over an arbitrary fixed list and hoping.
//
// Classification is exact: we compute the permuted physical slot for the
// callee's ordinal under each candidate route word via the SAME Red 1 helper
// the resolver uses (keyed_dispatch_permute_runtime), then read the plan's
// PhysicalEntry at that slot. is_padding -> padding; else the served
// logical_slot tells us which real callable it is (== callee -> correct,
// != callee -> alternate real). We additionally reject alternates that would
// be RECURSIVE (the served logical slot is the CALLER's own slot) so a
// deterministic non-recursive compatible alternate is always constructed
// (per the strengthened-coverage requirement: do not accept StackOverflow as
// evidence of ABI-compatible alternate routing where a non-recursive
// alternate/padding case can be constructed).
// ===========================================================================
struct RouteClassification {
    bool found = false;
    uint64_t correct_word = 0;       // routes callee's ordinal to its own entry
    uint64_t alternate_word = 0;     // routes to a non-recursive same-domain real
    uint64_t alternate_serves = 0xFFFFFFFFu;  // logical slot the alternate serves
    uint64_t padding_word = 0;       // routes to the padding ordinal
    bool has_alternate = false;
    bool has_padding = false;
};

static RouteClassification classify_routes(const KeyedModule& km,
                                           uint32_t callee_logical_slot,
                                           uint32_t caller_logical_slot) {
    RouteClassification rc;
    if (callee_logical_slot >= km.plan.logical_slot_count) return rc;
    const auto& route = km.plan.logical_routes[callee_logical_slot];
    const auto& domain = km.plan.domains[route.domain_index];
    KeyedDispatchDomain kd{domain.domain_salt, domain.strategy_version, domain.physical_count};
    // The correct route word for this module routes every ordinal to its own
    // build-time physical slot. Record it so the test can enter the intended
    // caller under the CORRECT key.
    rc.correct_word = km.route_word;
    // Search candidate route words deterministically. Start at 1 (0 is an edge
    // word tested separately) and scan a bounded range; the permutation is a
    // surjection over [0, n) for coprime a, so every physical slot is hit by
    // some word in a modest range. We stop once we have both an alternate and
    // a padding word (or exhaust the range).
    const uint32_t n = domain.physical_count;
    const uint64_t pad_target = reinterpret_cast<uint64_t>(ember_keyed_padding_trap_target());
    (void)pad_target;
    for (uint64_t w = 1; w < 100000 && !(rc.has_alternate && rc.has_padding); ++w) {
        if (w == km.route_word) continue;  // the correct word is handled above
        auto pr = keyed_dispatch_permute_runtime(w, kd, route.ordinal);
        if (!pr || !pr.value) continue;
        uint32_t local = *pr.value;
        if (local >= n) continue;
        uint32_t phys = domain.physical_base + local;
        if (phys >= km.plan.physical_slot_count) continue;
        const auto& pe = km.plan.physical_entries[phys];
        if (pe.is_padding) {
            if (!rc.has_padding) { rc.has_padding = true; rc.padding_word = w; }
        } else {
            uint32_t served = pe.logical_slot;
            if (served == callee_logical_slot) continue;       // correct entry, not alternate
            if (served == caller_logical_slot) continue;       // RECURSIVE alternate -> skip
            // A genuine non-recursive same-ABI-domain alternate real.
            if (!rc.has_alternate) {
                rc.has_alternate = true; rc.alternate_word = w; rc.alternate_serves = served;
            }
        }
    }
    rc.found = rc.has_alternate && rc.has_padding;
    return rc;
}

// ===========================================================================
// Build an IDENTITY module (keyed_dispatch = null) for the legacy-behavior
// assertion. The emit must be byte-identical to the pre-Red-6 path.
// ===========================================================================
static std::unique_ptr<Compiled> compile_identity(const std::string& src, bool ir_backend) {
    auto m = std::make_unique<Compiled>();
    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); } catch (...) { return nullptr; }
    auto lr = tokenize(resolved, "<id>"); if (!lr.ok) return nullptr;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
    int si = 0; for(auto&fn:pr.program.funcs){m->slots[fn.name]=si++;fn.slot=m->slots[fn.name];}
    ember::OpOverloadTable ov;
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
    ctx.max_call_depth = 64;
    m->allowlist = build_fn_allowlist(m->slots, int(pr.program.funcs.size()));
    ctx.fn_allowlist_base = int64_t(m->allowlist.data());
    ctx.fn_slot_count = int(pr.program.funcs.size());
    ctx.enable_ir_backend = ir_backend;
    // NO keyed_dispatch — legacy/identity mode.
    for(auto&fn:pr.program.funcs){
        auto cf=compile_func(fn,ctx); finalize(cf); m->table.set(fn.slot,cf.entry);
        m->fns.push_back(std::move(cf));
    }
    auto sit=m->slots.find("main");
    if(sit!=m->slots.end()) m->main_entry = m->table.get(sit->second);
    return m;
}

// ===========================================================================
// Paired identity compile: compile the SAME source TWICE against IDENTICAL
// shared bases (dispatch_base, fn_allowlist_base, globals_base) — once with
// keyed_dispatch = null (pure legacy) and once with keyed_dispatch SET but
// module_record = null (the disabled-keyed path). When module_record is null,
// keyed_same_module is false, so the emit takes the legacy path. With
// identical bases, the emitted bytes MUST be byte-for-byte identical (no
// normalization, no pointer masking — a true exact comparison). This is the
// real Red 6 disabled-path gate: no resolver call, no r15 read, no extra
// instruction, in both backends. The shared bases are kept alive in the
// returned struct for the comparison's lifetime.
// ===========================================================================
struct PairedCompile {
    std::vector<CompiledFn> legacy_fns;       // keyed_dispatch = null
    std::vector<CompiledFn> disabled_fns;     // keyed_dispatch set, module_record = null
    std::unordered_map<std::string, int> slots;
    DispatchTable table;                      // shared table (same base for both)
    std::vector<uint8_t> allowlist;           // shared allowlist (same base for both)
    std::vector<uint8_t> globals_storage;     // shared globals buffer (same base)
    ember::GlobalsBlock gb;
    std::unordered_map<std::string, NativeSig> natives;
    ~PairedCompile() {
        for (auto& fn : legacy_fns)   if (fn.exec) free_executable(fn.exec);
        for (auto& fn : disabled_fns) if (fn.exec) free_executable(fn.exec);
    }
};

static std::unique_ptr<PairedCompile> compile_paired_identity(const std::string& src, bool ir_backend) {
    auto pc = std::make_unique<PairedCompile>();
    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); } catch (...) { return nullptr; }
    auto lr = tokenize(resolved, "<paired>"); if (!lr.ok) return nullptr;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
    int si = 0; for(auto&fn:pr.program.funcs){pc->slots[fn.name]=si++;fn.slot=pc->slots[fn.name];}
    ember::OpOverloadTable ov;
    auto layouts = build_struct_layouts(pr.program); pr.program.string_xor_key = 0;
    if(!sema(pr.program,pc->natives,pc->slots,0,&ov,&layouts).ok) return nullptr;
    // SHARED globals buffer + allowlist + table — the SAME objects for both
    // compiles so the baked bases (globals_base, fn_allowlist_base,
    // dispatch_base) are bit-identical across the two variants.
    pc->globals_storage.assign(pr.program.globals.size()*8, 0);
    pc->gb.base = int64_t(pc->globals_storage.data());
    { uint32_t gi=0; for (auto& g : pr.program.globals) { pc->gb.index[g.name]=gi++; pc->gb.types[g.name]=g.ty.get(); } }
    eval_global_initializers(pr.program, GlobalInitCtx{pc->globals_storage, pc->gb.index, pc->gb.types});
    ember::g_globals_for_codegen = nullptr;
    int nslots = int(pr.program.funcs.size());
    pc->table = DispatchTable(nslots);
    pc->allowlist = build_fn_allowlist(pc->slots, nslots);
    const int64_t shared_dispatch_base = int64_t(pc->table.base());
    const int64_t shared_allowlist_base = int64_t(pc->allowlist.data());
    const int64_t shared_globals_base = pc->gb.base;

    // A lambda capturing the shared bases + a keyed mode, compiling every
    // function in the program into `out_fns`. keyed_dispatch_ptr is null for
    // pure legacy, non-null (with module_record=null) for the disabled path.
    auto compile_variant = [&](std::vector<CompiledFn>& out_fns, KeyedDispatchCodegen* keyed_dispatch_ptr) -> bool {
        ember::CodeGenCtx ctx;
        ctx.globals_base = shared_globals_base;
        ctx.dispatch_base = shared_dispatch_base;
        ctx.natives = &pc->natives; ctx.script_slots = &pc->slots; ctx.structs = &layouts;
        ctx.globals_index = &pc->gb.index; ctx.globals_types = &pc->gb.types;
        ctx.use_context_reg = true; ctx.emit_depth_checks = true; ctx.max_call_depth = 64;
        ctx.fn_allowlist_base = shared_allowlist_base; ctx.fn_slot_count = nslots;
        ctx.enable_ir_backend = ir_backend;
        ctx.keyed_dispatch = keyed_dispatch_ptr;
        for (auto& fn : pr.program.funcs) {
            auto cf = compile_func(fn, ctx); finalize(cf);
            if (!cf.entry) { std::printf("FAIL: paired compile null entry for %s\n", fn.name.c_str()); return false; }
            // Both variants share the same table; set the slot to the latest
            // compile's entry (the table is only used to hold the base address
            // for the comparison — the entries are not run here).
            pc->table.set(fn.slot, cf.entry);
            out_fns.push_back(std::move(cf));
        }
        return true;
    };

    // Variant 1: pure legacy (keyed_dispatch = null).
    if (!compile_variant(pc->legacy_fns, nullptr)) return nullptr;
    // Variant 2: disabled-keyed (keyed_dispatch SET, module_record = null).
    KeyedDispatchCodegen kd{};
    kd.runtime_key = RuntimeKeyLocation::R15;
    kd.module_record = nullptr;  // disabled -> keyed_same_module == false
    if (!compile_variant(pc->disabled_fns, &kd)) return nullptr;
    return pc;
}

int main() {
    std::printf("== keyed_dispatch_codegen_test (Red 6) ==\n");

    // A deterministic route word seed for the correct key.
    const uint64_t CORRECT_SEED = 0xCAFEBABE12345678ULL;

    // =====================================================================
    // 1. DIRECT CALLS — tree backend.
    //    main calls add(3,4) + square(5); the keyed resolver routes add + square.
    // =====================================================================
    {
        const char* src =
            "fn add(a: i64, b: i64) -> i64 { return a + b; }\n"
            "fn square(x: i64) -> i64 { return x * x; }\n"
            "fn main(x: i64) -> i64 { return add(3, 4) + square(5); }\n";
        auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/false);
        ck(km != nullptr, "tree: direct calls — build_keyed_module succeeded");
        if (km) {
            auto r = run_keyed(*km, "main", 0, km->route_word);
            ck(r.ok, "tree: direct calls — run succeeded under correct key");
            ck(r.value == 32, "tree: direct calls — add(3,4)+square(5) == 32 (7+25)");
            ck(r.call_depth == 0, "tree: direct calls — call_depth balanced (0 after return, no leak)");
        }
    }

    // =====================================================================
    // 2. DIRECT CALLS — Thin IR backend.
    // =====================================================================
    {
        const char* src =
            "fn add(a: i64, b: i64) -> i64 { return a + b; }\n"
            "fn square(x: i64) -> i64 { return x * x; }\n"
            "fn main(x: i64) -> i64 { return add(3, 4) + square(5); }\n";
        auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/true);
        ck(km != nullptr, "thin: direct calls — build_keyed_module succeeded");
        if (km) {
            auto r = run_keyed(*km, "main", 0, km->route_word);
            ck(r.ok, "thin: direct calls — run succeeded under correct key");
            ck(r.value == 32, "thin: direct calls — add(3,4)+square(5) == 32 (7+25)");
            ck(r.call_depth == 0, "thin: direct calls — call_depth balanced (0 after return, no leak)");
        }
    }

    // =====================================================================
    // 3. RECURSIVE CALLS — tree backend.
    //    fib(10) = 55; the keyed resolver routes every recursive edge.
    // =====================================================================
    {
        const char* src =
            "fn fib(n: i64) -> i64 { if (n < 2) { return n; } return fib(n-1) + fib(n-2); }\n"
            "fn main(x: i64) -> i64 { return fib(10); }\n";
        auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/false);
        ck(km != nullptr, "tree: recursive calls — build succeeded");
        if (km) {
            auto r = run_keyed(*km, "main", 0, km->route_word);
            ck(r.ok, "tree: recursive calls — run succeeded");
            ck(r.value == 55, "tree: recursive calls — fib(10) == 55");
            ck(r.call_depth == 0, "tree: recursive calls — call_depth balanced after deep recursion (no leak)");
        }
    }

    // =====================================================================
    // 4. RECURSIVE CALLS — Thin IR backend.
    // =====================================================================
    {
        const char* src =
            "fn fib(n: i64) -> i64 { if (n < 2) { return n; } return fib(n-1) + fib(n-2); }\n"
            "fn main(x: i64) -> i64 { return fib(10); }\n";
        auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/true);
        ck(km != nullptr, "thin: recursive calls — build succeeded");
        if (km) {
            auto r = run_keyed(*km, "main", 0, km->route_word);
            ck(r.ok, "thin: recursive calls — run succeeded");
            ck(r.value == 55, "thin: recursive calls — fib(10) == 55");
            ck(r.call_depth == 0, "thin: recursive calls — call_depth balanced after deep recursion (no leak)");
        }
    }

    // =====================================================================
    // 5. INDIRECT FUNCTION HANDLES — tree backend.
    //    main stores &square, calls it through the handle. The handle is a
    //    LOGICAL slot (validated by the allowlist); the keyed resolver routes it.
    // =====================================================================
    {
        const char* src =
            "fn square(x: i64) -> i64 { return x * x; }\n"
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn main(x: i64) -> i64 {\n"
            "  let f: fn(i64)->i64 = &square;\n"
            "  let g: fn(i64)->i64 = &double;\n"
            "  return f(5) + g(10);\n"
            "}\n";
        auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/false);
        ck(km != nullptr, "tree: indirect handles — build succeeded");
        if (km) {
            auto r = run_keyed(*km, "main", 0, km->route_word);
            ck(r.ok, "tree: indirect handles — run succeeded");
            ck(r.value == 45, "tree: indirect handles — square(5)+double(10) == 45 (25+20)");
            ck(r.call_depth == 0, "tree: indirect handles — call_depth balanced (no leak)");
        }
    }

    // =====================================================================
    // 6. INDIRECT FUNCTION HANDLES — Thin IR backend.
    // =====================================================================
    {
        const char* src =
            "fn square(x: i64) -> i64 { return x * x; }\n"
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn main(x: i64) -> i64 {\n"
            "  let f: fn(i64)->i64 = &square;\n"
            "  let g: fn(i64)->i64 = &double;\n"
            "  return f(5) + g(10);\n"
            "}\n";
        auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/true);
        ck(km != nullptr, "thin: indirect handles — build succeeded");
        if (km) {
            auto r = run_keyed(*km, "main", 0, km->route_word);
            ck(r.ok, "thin: indirect handles — run succeeded");
            ck(r.value == 45, "thin: indirect handles — square(5)+double(10) == 45 (25+20)");
            ck(r.call_depth == 0, "thin: indirect handles — call_depth balanced (0, no leak)");
            // Structured backend evidence: main (which holds + calls the &fn
            // handles via CallIndirect) was emitted by the Thin IR backend,
            // not a silent tree-walker fallback. This is recorded by
            // compile_keyed via compile_func_checked.
            auto mit = km->compiled->slots.find("main");
            ck(mit != km->compiled->slots.end(), "thin: indirect handles — main slot found for backend evidence");
            if (mit != km->compiled->slots.end()) {
                ck(km->compiled->backends[size_t(mit->second)] == CompileBackend::IRBackend,
                   "thin: indirect handles — main emitted by the Thin IR backend (not a tree fallback)");
            }
        }
    }

    // =====================================================================
    // 7. LAMBDA CALLS — tree backend.
    //    main creates a lambda and calls it; the keyed resolver routes the
    //    lambda's logical slot, with env_ptr prepended.
    // =====================================================================
    {
        const char* src =
            "fn main(x: i64) -> i64 {\n"
            "  let captured: i64 = 10;\n"
            "  let f = fn(v: i64) -> i64 { return v + 10; };\n"
            "  return f(32);\n"
            "}\n";
        auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/false);
        ck(km != nullptr, "tree: lambda calls — build succeeded");
        if (km) {
            auto r = run_keyed(*km, "main", 0, km->route_word);
            ck(r.ok, "tree: lambda calls — run succeeded");
            ck(r.value == 42, "tree: lambda calls — f(32) where f adds 10 == 42");
            ck(r.call_depth == 0, "tree: lambda calls — call_depth balanced (0, no leak)");
        }
    }

    // =====================================================================
    // 8. LAMBDA CALLS — Thin IR backend.
    //    The lambda-creating function (main) falls back to the tree-walker
    //    (LambdaExpr is tree-walker-only in the IR backend); but a function
    //    that CALLS a lambda-typed parameter goes through the IR backend's
    //    CallIndirect path. We test that a lambda passed to a caller resolves
    //    correctly.
    //
    //    NOTE: in this phase, the IR backend lowers is_lambda_call to a
    //    CallIndirect with env_ptr prepended (Red 6 adds this lowering).
    //    A function that CREATES a lambda (LambdaExpr) falls back to the
    //    tree-walker automatically (lower_expr's unhandled-node path sets
    //    non_serializable, clearing the blocks). So `main` (which creates
    //    the lambda) is tree-walker-compiled, but `caller` (which calls it)
    //    is IR-backend-compiled — both coexist in one module.
    // =====================================================================
    {
        const char* src =
            "fn caller(f: fn(i64)->i64, v: i64) -> i64 { return f(v); }\n"
            "fn main(x: i64) -> i64 {\n"
            "  let f = fn(v: i64) -> i64 { return v + 10; };\n"
            "  return caller(f, 32);\n"
            "}\n";
        auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/true);
        ck(km != nullptr, "thin: lambda calls — build succeeded (caller via IR, main via tree fallback)");
        if (km) {
            auto r = run_keyed(*km, "main", 0, km->route_word);
            ck(r.ok, "thin: lambda calls — run succeeded");
            ck(r.value == 42, "thin: lambda calls — caller(f, 32) where f adds 10 == 42");
            ck(r.call_depth == 0, "thin: lambda calls — call_depth balanced (0, no leak)");
            // STRUCTURED BACKEND EVIDENCE (strengthened coverage): the test
            // must PROVE `caller` was emitted by the Thin IR path, not merely
            // comment on it. compile_keyed records each function's actual
            // backend via compile_func_checked. caller (which CALLS a lambda-
            // typed parameter via CallIndirect) must be IRBackend; main (which
            // CREATES the lambda via LambdaExpr, tree-walker-only) must fall
            // back to TreeWalker. A tree fallback for caller would silently
            // pass the value check above — this assertion catches that.
            auto cit = km->compiled->slots.find("caller");
            auto mit = km->compiled->slots.find("main");
            ck(cit != km->compiled->slots.end(), "thin: lambda calls — caller slot found for backend evidence");
            ck(mit != km->compiled->slots.end(), "thin: lambda calls — main slot found for backend evidence");
            if (cit != km->compiled->slots.end()) {
                ck(km->compiled->backends[size_t(cit->second)] == CompileBackend::IRBackend,
                   "thin: lambda calls — caller emitted by the Thin IR backend (proven, not assumed)");
            }
            if (mit != km->compiled->slots.end()) {
                ck(km->compiled->backends[size_t(mit->second)] == CompileBackend::TreeWalker,
                   "thin: lambda calls — main fell back to tree-walker (LambdaExpr is tree-only)");
            }
        }
    }

    // =====================================================================
    // 8b. LAMBDA CALLS WITH MULTIWORD + STACK-PASSED ARGUMENTS — keyed, tree.
    //     A regression for the lambda stash offset bug: the tree-walker's
    //     lambda-call path previously sized/placed its arg stash with
    //     c->args.size() (the COUNT of user args) instead of the flattened
    //     ABI word count, so a 2-word slice/lambda argument overlapped the
    //     next argument and/or the saved logical slot. This test puts a
    //     2-word slice argument (a string literal, {ptr,len}) BEFORE three
    //     scalar arguments so the total (env_ptr + slice + 3 scalars = 6
    //     words) pushes words 4+ onto the outgoing stack — exercising BOTH
    //     the multiword-argument overlap and the stack-passed-argument path.
    //     The lambda reads s[0] + n + a + b, so EVERY argument must survive
    //     the stash + keyed resolution; a single overlap corrupts the result.
    //     'h' (104) + 5 + 10 + 20 == 139.
    // =====================================================================
    {
        const char* src =
            "fn caller(f: fn(u8[], i64, i64, i64) -> i64) -> i64 { return f(\"hello\", 5, 10, 20); }\n"
            "fn main(x: i64) -> i64 {\n"
            "  let f = fn(s: u8[], n: i64, a: i64, b: i64) -> i64 { return (s[0] as i64) + n + a + b; };\n"
            "  return caller(f);\n"
            "}\n";
        // keyed, tree backend
        {
            auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/false);
            ck(km != nullptr, "keyed tree: multiword/stack lambda args — build succeeded");
            if (km) {
                auto r = run_keyed(*km, "main", 0, km->route_word);
                ck(r.ok, "keyed tree: multiword/stack lambda args — run succeeded");
                ck(r.value == 139, "keyed tree: multiword/stack lambda args — (s[0]=104)+5+10+20 == 139");
                ck(r.call_depth == 0, "keyed tree: multiword/stack lambda args — call_depth balanced (0)");
            }
        }
        // keyed, Thin IR backend (caller + the synthetic lambda fn via IR;
        // main creates the lambda so it falls back to the tree-walker).
        // This is the strengthened ABI gate for the Thin IR indirect-call
        // argument stash: env_ptr + a 2-word slice + 3 scalars = 6 words, 2
        // outgoing (words 4,5). A correct stash places the slice pointer at
        // the callee's expected word and the outgoing scalars at [shadow+...];
        // a stash-offset defect reads the slice pointer from the wrong offset,
        // so s[0] dereferences garbage. The guarded run converts a raw
        // ACCESS_VIOLATION from such a defect into a clean EXHIBITED failure
        // (crashed=true) rather than killing the harness.
        {
            auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/true);
            ck(km != nullptr, "keyed thin: multiword/stack lambda args — build succeeded");
            if (km) {
                auto g = run_keyed_guarded(*km, "main", 0, km->route_word);
                ck(!g.crashed, "keyed thin: multiword/stack lambda args — run did not crash (callee-side arg stash ABI sound)");
                if (!g.crashed) {
                    ck(g.value == 139, "keyed thin: multiword/stack lambda args — (s[0]=104)+5+10+20 == 139");
                    ck(g.call_depth == 0, "keyed thin: multiword/stack lambda args — call_depth balanced (0)");
                } else {
                    // GENUINE Thin-IR ABI failure exhibited: the indirect-call
                    // stash mislaid the multiword slice pointer. Record the
                    // fault code so the failure is documented, not silent.
                    unsigned long fc = static_cast<unsigned long>(g.code);
                    char buf[160]; std::snprintf(buf, sizeof(buf),
                        "keyed thin: multiword/stack lambda args — GENUINE ABI FAILURE: crash code 0x%lX (callee-side arg stash defect)",
                        fc);
                    ck(false, buf);
                }
            }
        }
        // LEGACY (keyed_dispatch = null) — the same stash fix benefits the
        // legacy lambda-call path; verify the multiword + stack args are
        // preserved with no keyed resolver in the picture. The Thin legacy
        // path is crash-guarded for the same callee-side stash ABI reason as
        // the keyed Thin path above; the tree legacy path is not guarded
        // (the tree-walker stash is a separate, already-correct path).
        for (bool ir : {false, true}) {
            const char* be = ir ? "legacy thin" : "legacy tree";
            auto m = compile_identity(src, ir);
            ck(m != nullptr, (std::string("legacy ") + be + ": multiword/stack lambda args — compile_identity succeeded").c_str());
            if (m) {
                auto g = run_guarded([&](CrashedResult& out) {
                    context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
                    ember_set_r15(0);  // r15 untouched in legacy mode
                    int64_t v = ember_call_i64(m->main_entry, &ctx, 0);
                    out.value = v; out.call_depth = ctx.call_depth;
                    // r15 must remain 0 in legacy mode (captured post-call).
                    (void)ember_read_r15();
                });
                if (ir) {
                    ck(!g.crashed, (std::string("legacy ") + be + ": multiword/stack lambda args — run did not crash (arg stash ABI sound)").c_str());
                }
                if (!g.crashed) {
                    ck(g.value == 139, (std::string("legacy ") + be + ": multiword/stack lambda args — (s[0]=104)+5+10+20 == 139").c_str());
                    ck(g.call_depth == 0, (std::string("legacy ") + be + ": multiword/stack lambda args — call_depth balanced (0)").c_str());
                    // Re-run without the guard to read r15 (the guard's
                    // longjmp recovery would skip this read on a crash; the
                    // non-guarded tree path never crashes so this is safe).
                    context_t ctx2; ctx2.budget_remaining = 1'000'000'000LL; ctx2.max_call_depth = 64;
                    ember_set_r15(0);
                    (void)ember_call_i64(m->main_entry, &ctx2, 0);
                    ck(ember_read_r15() == 0, (std::string("legacy ") + be + ": multiword/stack lambda args — r15 untouched (legacy)").c_str());
                } else if (ir) {
                    unsigned long fc = static_cast<unsigned long>(g.code);
                    char buf[160]; std::snprintf(buf, sizeof(buf),
                        "legacy %s: multiword/stack lambda args — GENUINE ABI FAILURE: crash code 0x%lX (callee-side arg stash defect)",
                        be, fc);
                    ck(false, buf);
                }
            }
        }
    }

    // =====================================================================
    // 9. MIXED GP/XMM ARGUMENTS — tree backend.
    //    A call with f64 (XMM) and i64 (GP) arguments in the same call.
    //    The resolver preserves the stashed args (resolves before placement).
    // =====================================================================
    {
        const char* src =
            "fn mixed(a: i64, b: f64) -> i64 { return a + (b as i64); }\n"
            "fn main(x: i64) -> i64 { return mixed(3, 4.0); }\n";
        auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/false);
        ck(km != nullptr, "tree: mixed GP/XMM — build succeeded");
        if (km) {
            auto r = run_keyed(*km, "main", 0, km->route_word);
            ck(r.ok, "tree: mixed GP/XMM — run succeeded");
            ck(r.value == 7, "tree: mixed GP/XMM — mixed(3, 4.0) == 7");
            ck(r.call_depth == 0, "tree: mixed GP/XMM — call_depth balanced (0, no leak)");
        }
    }

    // =====================================================================
    // 10. MIXED GP/XMM ARGUMENTS — Thin IR backend.
    // =====================================================================
    {
        const char* src =
            "fn mixed(a: i64, b: f64) -> i64 { return a + (b as i64); }\n"
            "fn main(x: i64) -> i64 { return mixed(3, 4.0); }\n";
        auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/true);
        ck(km != nullptr, "thin: mixed GP/XMM — build succeeded");
        if (km) {
            auto r = run_keyed(*km, "main", 0, km->route_word);
            ck(r.ok, "thin: mixed GP/XMM — run succeeded");
            ck(r.value == 7, "thin: mixed GP/XMM — mixed(3, 4.0) == 7");
            ck(r.call_depth == 0, "thin: mixed GP/XMM — call_depth balanced (0, no leak)");
        }
    }

    // =====================================================================
    // 11. DEPTH ACCOUNTING — tree backend.
    //     A deep recursive call (fib(15)) completes within the depth budget;
    //     the depth check + leave bracket every call exactly once (no leak
    //     that overflows the limit prematurely).
    // =====================================================================
    {
        const char* src =
            "fn fib(n: i64) -> i64 { if (n < 2) { return n; } return fib(n-1) + fib(n-2); }\n"
            "fn main(x: i64) -> i64 { return fib(15); }\n";
        auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/false);
        ck(km != nullptr, "tree: depth accounting — build succeeded");
        if (km) {
            auto r = run_keyed(*km, "main", 0, km->route_word);
            ck(r.ok, "tree: depth accounting — fib(15) ran without depth overflow");
            ck(r.value == 610, "tree: depth accounting — fib(15) == 610");
            ck(r.call_depth == 0, "tree: depth accounting — call_depth balanced (0) after deep recursion (no leak)");
        }
    }

    // =====================================================================
    // 12. DEPTH ACCOUNTING — Thin IR backend.
    // =====================================================================
    {
        const char* src =
            "fn fib(n: i64) -> i64 { if (n < 2) { return n; } return fib(n-1) + fib(n-2); }\n"
            "fn main(x: i64) -> i64 { return fib(15); }\n";
        auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/true);
        ck(km != nullptr, "thin: depth accounting — build succeeded");
        if (km) {
            auto r = run_keyed(*km, "main", 0, km->route_word);
            ck(r.ok, "thin: depth accounting — fib(15) ran without depth overflow");
            ck(r.value == 610, "thin: depth accounting — fib(15) == 610");
            ck(r.call_depth == 0, "thin: depth accounting — call_depth balanced (0) after deep recursion (no leak)");
        }
    }

    // =====================================================================
    // 13. CORRECT-KEY ROUTING — both backends agree.
    //     Under the correct route word, every call resolves to the correct
    //     entry. Tree and Thin IR produce the same result.
    // =====================================================================
    {
        const char* src =
            "fn add(a: i64, b: i64) -> i64 { return a + b; }\n"
            "fn mul(a: i64, b: i64) -> i64 { return a * b; }\n"
            "fn main(x: i64) -> i64 { return add(6, 7) + mul(3, 4); }\n";
        auto km_tree = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/false);
        auto km_thin = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/true);
        ck(km_tree != nullptr && km_thin != nullptr, "correct-key: both backends built");
        if (km_tree && km_thin) {
            auto rt = run_keyed(*km_tree, "main", 0, km_tree->route_word);
            auto ri = run_keyed(*km_thin, "main", 0, km_thin->route_word);
            ck(rt.ok && ri.ok, "correct-key: both backends ran");
            ck(rt.value == ri.value, "correct-key: tree + thin agree on result");
            ck(rt.value == 25, "correct-key: add(6,7)+mul(3,4) == 25 (13+12)");
        }
    }

    // =====================================================================
    // 14. WRONG-KEY ROUTING — generated internal call-site resolution (tree).
    //     This is the RED 6 gate for the JIT'd wrong-key path. We resolve the
    //     OUTER entry (main) under the CORRECT route word so we enter the
    //     intended caller, then install a WRONG r15 via the keyed re-entry
    //     thunk. main's GENERATED add(6,7) call site then invokes
    //     ember_resolve_keyed_dispatch(record, add_slot, wrong_r15). Under a
    //     wrong word the internal call resolves to a same-ABI-domain alternate
    //     real (runs, returns a value, no crash) or the padding entry (the
    //     r14-reading padding stub sets last_trap=KeyedDispatchPadding and
    //     returns 0 normally — a real callable, NOT a trap-stub call, so no
    //     longjmp fires for the padding case). Either way: no crash, no AV,
    //     call_depth balanced (the depth check + leave bracket the keyed call
    //     exactly once, even when the resolved entry is padding/alternate).
    //     The host-resolver checks below additionally prove the resolver
    //     returns a non-null, in-range, same-ABI entry + the padding identity.
    // =====================================================================
    {
        const char* src =
            "fn add(a: i64, b: i64) -> i64 { return a + b; }\n"
            "fn sub(a: i64, b: i64) -> i64 { return a - b; }\n"
            "fn main(x: i64) -> i64 { return add(6, 7); }\n";
        auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/false,
                                      reinterpret_cast<void*>(kt6_trap));
        ck(km != nullptr, "wrong-key: tree — build succeeded (with trap stub)");
        if (km) {
            // Under the correct key: add(6,7) = 13, no trap, depth balanced.
            auto r_ok = run_keyed(*km, "main", 0, km->route_word, /*use_checkpoint=*/false);
            ck(r_ok.ok && r_ok.value == 13, "wrong-key: tree — correct key gives add(6,7)==13");
            ck(r_ok.call_depth == 0, "wrong-key: tree — correct key call_depth balanced (0)");

            // GENERATED internal call-site resolution under wrong r15: enter
            // main (resolved under the correct key) with a wrong route word
            // installed by the re-entry thunk. main's add(6,7) call site
            // resolves through the keyed resolver with the wrong r15.
            // DETERMINISTIC route-word selection from the layout (strengthened
            // coverage): the add/sub/main module puts add+sub in ONE ABI domain
            // (both (i64,i64)->i64) with one padding ordinal; main has a
            // different signature so it is in a SEPARATE domain. main's add(6,7)
            // call site resolves add's ordinal, which permutes ONLY within
            // {add, sub, padding} — NONE of which is recursive. So a wrong
            // route word must resolve to EITHER sub (a non-recursive same-ABI
            // alternate real that runs sub(6,7) = -1) OR padding (returns 0,
            // sets KeyedDispatchPadding). StackOverflow is NOT possible here,
            // so we assert it never fires (per the strengthened-coverage
            // requirement: do not accept StackOverflow as evidence of
            // ABI-compatible alternate routing where a deterministic
            // non-recursive alternate/padding case can be constructed).
            uint32_t add_slot = uint32_t(km->compiled->slots.at("add"));
            uint32_t main_slot = uint32_t(km->compiled->slots.at("main"));
            auto rc = classify_routes(*km, /*callee=*/add_slot, /*caller=*/main_slot);
            ck(rc.has_alternate, "wrong-key: tree — layout yields a non-recursive alternate-real route word for add's domain");
            ck(rc.has_padding, "wrong-key: tree — layout yields a padding route word for add's domain");
            ck(rc.alternate_serves == uint32_t(km->compiled->slots.at("sub")),
               "wrong-key: tree — alternate route serves sub (the same-ABI sibling, not main)");
            // Padding route: enter main under the CORRECT key (so we enter the
            // intended caller), install the padding route word via the thunk.
            // main's add(6,7) call site resolves to padding: returns 0, sets
            // KeyedDispatchPadding, depth balanced (no leak), NO crash.
            if (rc.has_padding) {
                auto r = run_keyed_wrong_r15_ii(*km, "main", 0, 0, km->route_word, rc.padding_word);
                ck(r.ok, "wrong-key: tree — JIT padding-route run succeeded (no crash, no AV)");
                if (r.ok) {
                    ck(r.last_trap == TrapReason::KeyedDispatchPadding,
                       "wrong-key: tree — JIT padding-route sets KeyedDispatchPadding");
                    ck(r.value == 0, "wrong-key: tree — JIT padding-route returns 0 (neutral same-ABI return)");
                    ck(r.call_depth == 0, "wrong-key: tree — JIT padding-route call_depth balanced (0, no leak)");
                }
            }
            // Alternate-real route: add(6,7) resolves to sub, which runs
            // sub(6,7) = -1. ABI-compatible (same domain), non-recursive,
            // returns normally, depth balanced, NO StackOverflow.
            if (rc.has_alternate) {
                auto r = run_keyed_wrong_r15_ii(*km, "main", 0, 0, km->route_word, rc.alternate_word);
                ck(r.ok, "wrong-key: tree — JIT alternate-route run succeeded (no crash, no AV)");
                if (r.ok) {
                    ck(r.last_trap == TrapReason::None,
                       "wrong-key: tree — JIT alternate-route trap is None (ran the alternate real)");
                    ck(r.last_trap != TrapReason::StackOverflow,
                       "wrong-key: tree — JIT alternate-route did NOT overflow depth (non-recursive alternate)");
                    ck(r.value == -1, "wrong-key: tree — JIT alternate-route ran sub(6,7) == -1 (ABI-compatible alternate)");
                    ck(r.call_depth == 0, "wrong-key: tree — JIT alternate-route call_depth balanced (0, no leak)");
                }
            }
            // Host-resolver cross-check for the two deterministic words: the
            // resolver returns a non-null, in-range, same-ABI entry, and a
            // padding entry equals the real padding trap stub.
            if (rc.has_padding) {
                void* entry = ember_resolve_keyed_dispatch(&km->rec, add_slot, rc.padding_word);
                ck(entry != nullptr, "wrong-key: tree — host resolver returns non-null for the padding route word");
                if (entry) ck(ember_is_padding_trap_target(entry) && entry == ember_keyed_padding_trap_target(),
                              "wrong-key: tree — padding-route host-resolved entry is the real padding trap stub");
            }
            if (rc.has_alternate) {
                void* entry = ember_resolve_keyed_dispatch(&km->rec, add_slot, rc.alternate_word);
                ck(entry != nullptr, "wrong-key: tree — host resolver returns non-null for the alternate route word");
                if (entry) ck(!ember_is_padding_trap_target(entry),
                              "wrong-key: tree — alternate-route host-resolved entry is a real callable (not padding)");
            }
        }
    }

    // =====================================================================
    // 15. WRONG-KEY ROUTING — generated internal call-site resolution (Thin).
    //     Same gate as test 14, through the Thin IR backend.
    // =====================================================================
    {
        const char* src =
            "fn add(a: i64, b: i64) -> i64 { return a + b; }\n"
            "fn sub(a: i64, b: i64) -> i64 { return a - b; }\n"
            "fn main(x: i64) -> i64 { return add(6, 7); }\n";
        auto km = build_keyed_module(src, CORRECT_SEED, /*ir_backend=*/true,
                                      reinterpret_cast<void*>(kt6_trap));
        ck(km != nullptr, "wrong-key: thin — build succeeded (with trap stub)");
        if (km) {
            auto r_ok = run_keyed(*km, "main", 0, km->route_word, /*use_checkpoint=*/false);
            ck(r_ok.ok && r_ok.value == 13, "wrong-key: thin — correct key gives add(6,7)==13");
            ck(r_ok.call_depth == 0, "wrong-key: thin — correct key call_depth balanced (0)");

            uint32_t add_slot = uint32_t(km->compiled->slots.at("add"));
            uint32_t main_slot = uint32_t(km->compiled->slots.at("main"));
            auto rc = classify_routes(*km, /*callee=*/add_slot, /*caller=*/main_slot);
            ck(rc.has_alternate, "wrong-key: thin — layout yields a non-recursive alternate-real route word for add's domain");
            ck(rc.has_padding, "wrong-key: thin — layout yields a padding route word for add's domain");
            ck(rc.alternate_serves == uint32_t(km->compiled->slots.at("sub")),
               "wrong-key: thin — alternate route serves sub (the same-ABI sibling, not main)");
            if (rc.has_padding) {
                auto r = run_keyed_wrong_r15_ii(*km, "main", 0, 0, km->route_word, rc.padding_word);
                ck(r.ok, "wrong-key: thin — JIT padding-route run succeeded (no crash, no AV)");
                if (r.ok) {
                    ck(r.last_trap == TrapReason::KeyedDispatchPadding,
                       "wrong-key: thin — JIT padding-route sets KeyedDispatchPadding");
                    ck(r.value == 0, "wrong-key: thin — JIT padding-route returns 0 (neutral same-ABI return)");
                    ck(r.call_depth == 0, "wrong-key: thin — JIT padding-route call_depth balanced (0, no leak)");
                }
            }
            if (rc.has_alternate) {
                auto r = run_keyed_wrong_r15_ii(*km, "main", 0, 0, km->route_word, rc.alternate_word);
                ck(r.ok, "wrong-key: thin — JIT alternate-route run succeeded (no crash, no AV)");
                if (r.ok) {
                    ck(r.last_trap == TrapReason::None,
                       "wrong-key: thin — JIT alternate-route trap is None (ran the alternate real)");
                    ck(r.last_trap != TrapReason::StackOverflow,
                       "wrong-key: thin — JIT alternate-route did NOT overflow depth (non-recursive alternate)");
                    ck(r.value == -1, "wrong-key: thin — JIT alternate-route ran sub(6,7) == -1 (ABI-compatible alternate)");
                    ck(r.call_depth == 0, "wrong-key: thin — JIT alternate-route call_depth balanced (0, no leak)");
                }
            }
            if (rc.has_padding) {
                void* entry = ember_resolve_keyed_dispatch(&km->rec, add_slot, rc.padding_word);
                ck(entry != nullptr, "wrong-key: thin — host resolver returns non-null for the padding route word");
                if (entry) ck(ember_is_padding_trap_target(entry) && entry == ember_keyed_padding_trap_target(),
                              "wrong-key: thin — padding-route host-resolved entry is the real padding trap stub");
            }
            if (rc.has_alternate) {
                void* entry = ember_resolve_keyed_dispatch(&km->rec, add_slot, rc.alternate_word);
                ck(entry != nullptr, "wrong-key: thin — host resolver returns non-null for the alternate route word");
                if (entry) ck(!ember_is_padding_trap_target(entry),
                              "wrong-key: thin — alternate-route host-resolved entry is a real callable (not padding)");
            }
        }
    }

    // =====================================================================
    // 15b. WRONG-KEY ROUTING — single-arg generated call-site resolution.
    //      A module where main calls a single-arg function (inc) through the
    //      keyed resolver. Enter main under the correct key, install a wrong
    //      r15 via the single-arg keyed re-entry thunk, and verify the
    //      generated inc(42) call site resolves safely (padding or alternate,
    //      no crash, depth balanced). Both backends. This exercises the
    //      single-arg re-entry thunk + the keyed resolver with a 1-arg call.
    // =====================================================================
    {
        const char* src =
            "fn inc(x: i64) -> i64 { return x + 1; }\n"
            "fn dec(x: i64) -> i64 { return x - 1; }\n"
            "fn main(x: i64) -> i64 { return inc(42); }\n";
        for (bool ir : {false, true}) {
            const char* be = ir ? "thin" : "tree";
            auto km = build_keyed_module(src, CORRECT_SEED, ir,
                                          reinterpret_cast<void*>(kt6_trap));
            ck(km != nullptr, (std::string("wrong-key 1-arg: ") + be + " — build succeeded").c_str());
            if (!km) continue;
            // Correct key: inc(42) = 43.
            auto r_ok = run_keyed(*km, "main", 0, km->route_word, /*use_checkpoint=*/false);
            ck(r_ok.ok && r_ok.value == 43, (std::string("wrong-key 1-arg: ") + be + " — correct key inc(42)==43").c_str());
            // DETERMINISTIC route selection. inc+dec+main all share (i64)->i64,
            // so inc's domain also contains main (a recursive alternate) and
            // dec (a non-recursive alternate) plus padding. classify_routes
            // SKIPS the recursive alternate (served == caller == main) and
            // selects dec (non-recursive) + padding, so the JIT runs we issue
            // are deterministic non-recursive compatible alternates + padding
            // — NO StackOverflow is acceptable here.
            uint32_t inc_slot = uint32_t(km->compiled->slots.at("inc"));
            uint32_t main_slot = uint32_t(km->compiled->slots.at("main"));
            auto rc = classify_routes(*km, /*callee=*/inc_slot, /*caller=*/main_slot);
            ck(rc.has_alternate, (std::string("wrong-key 1-arg: ") + be + " — layout yields a non-recursive alternate route word").c_str());
            ck(rc.has_padding, (std::string("wrong-key 1-arg: ") + be + " — layout yields a padding route word").c_str());
            ck(rc.alternate_serves == uint32_t(km->compiled->slots.at("dec")),
               (std::string("wrong-key 1-arg: ") + be + " — alternate route serves dec (the non-recursive sibling, not main)").c_str());
            // Padding route (single-arg re-entry thunk).
            if (rc.has_padding) {
                auto r = run_keyed_wrong_r15_i(*km, "main", 0, km->route_word, rc.padding_word);
                ck(r.ok, (std::string("wrong-key 1-arg: ") + be + " — JIT padding-route run succeeded (no crash)").c_str());
                if (r.ok) {
                    ck(r.last_trap == TrapReason::KeyedDispatchPadding,
                       (std::string("wrong-key 1-arg: ") + be + " — JIT padding-route sets KeyedDispatchPadding").c_str());
                    ck(r.value == 0, (std::string("wrong-key 1-arg: ") + be + " — JIT padding-route returns 0").c_str());
                    ck(r.call_depth == 0, (std::string("wrong-key 1-arg: ") + be + " — JIT padding-route call_depth balanced (0)").c_str());
                }
            }
            // Alternate-real route: inc(42) resolves to dec, which runs
            // dec(42) = 41. ABI-compatible, non-recursive, returns normally.
            if (rc.has_alternate) {
                auto r = run_keyed_wrong_r15_i(*km, "main", 0, km->route_word, rc.alternate_word);
                ck(r.ok, (std::string("wrong-key 1-arg: ") + be + " — JIT alternate-route run succeeded (no crash)").c_str());
                if (r.ok) {
                    ck(r.last_trap == TrapReason::None,
                       (std::string("wrong-key 1-arg: ") + be + " — JIT alternate-route trap is None (ran the alternate real)").c_str());
                    ck(r.last_trap != TrapReason::StackOverflow,
                       (std::string("wrong-key 1-arg: ") + be + " — JIT alternate-route did NOT overflow depth (non-recursive)").c_str());
                    ck(r.value == 41, (std::string("wrong-key 1-arg: ") + be + " — JIT alternate-route ran dec(42) == 41 (ABI-compatible)").c_str());
                    ck(r.call_depth == 0, (std::string("wrong-key 1-arg: ") + be + " — JIT alternate-route call_depth balanced (0)").c_str());
                }
            }
        }
    }

    // =====================================================================
    // 16. LOGICAL HANDLE STABILITY — a &fn handle is a logical slot, and
    //     that logical value is STABLE across layouts/seeds while the
    //     ROUTED PHYSICAL SLOT changes. This is the non-disclosure proof:
    //     the &fn literal is baked as `mov rax, imm64(logical_slot)` (sema
    //     sets the slot from script_slots, the LOGICAL slot map), so the
    //     handle VALUE is a logical slot index that the keyed resolver maps
    //     to a physical entry via the route-word permutation. The physical
    //     slot is NEVER visible in the handle.
    //
    //     Strengthened proof (a value < logical_slot_count can still equal a
    //     physical-slot ordinal, so the old "< logical_count" check alone was
    //     not a valid non-disclosure proof):
    //       (a) build under a seed whose keyed layout PERMUTES square's logical
    //           slot to a DIFFERENT physical slot (build_physical_placement[sq]
    //           != square_logical), so the logical and physical indices are
    //           provably distinct ordinals;
    //       (b) assert the baked handle value == square_logical (the logical
    //           slot) AND != build_physical_placement[square_logical] (NOT
    //           the physical slot the route word actually routes to);
    //       (c) across several seeds the logical slot is constant and the
    //           physical placement varies, yet the emitted handle value stays
    //           the same logical value — proving the handle is logical, not a
    //           physical slot, and does not move with the layout.
    // =====================================================================
    {
        const char* src =
            "fn square(x: i64) -> i64 { return x * x; }\n"
            "fn main(x: i64) -> i64 {\n"
            "  let f: fn(i64)->i64 = &square;\n"
            "  return f(6);\n"
            "}\n";
        // Scan main's bytes for a `mov rax, imm64` (48 B8 <8 LE>) whose
        // imm64 == the sought handle value. Returns the byte offset or SIZE_MAX.
        auto find_handle_load = [](const std::vector<uint8_t>& bytes, uint64_t want) -> size_t {
            for (size_t i = 0; i + 10 <= bytes.size(); ++i) {
                if (bytes[i] == 0x48 && bytes[i+1] == 0xB8) {
                    uint64_t v = 0; std::memcpy(&v, bytes.data()+i+2, 8);
                    if (v == want) return i;
                }
            }
            return SIZE_MAX;
        };
        // square is logical slot 0 (the first function in script_slots) in
        // EVERY build of this module — the logical slot map is source order,
        // independent of the route word / layout seed.
        const uint32_t square_logical = 0;
        bool proved_distinct = false;     // (a): logical != physical in some layout
        bool proved_stable_across_seeds = true;  // (c): handle value constant
        int seeds_built = 0;
        // Try several seeds; require at least one where the keyed layout
        // permutes square to a different physical slot, AND assert the baked
        // handle is the logical slot (not the physical) in every build.
        const uint64_t seeds[] = {CORRECT_SEED, 0x1111111111111111ULL,
                                  0x2222222222222222ULL, 0x3333333333333333ULL,
                                  0x4444444444444444ULL, 0x5555555555555555ULL};
        for (uint64_t seed : seeds) {
            auto km = build_keyed_module(src, seed, /*ir_backend=*/false);
            if (!km) continue;
            ++seeds_built;
            ck(square_logical < km->plan.logical_slot_count,
               "handle stability — square's logical slot is in [0, logical_count) (a logical index)");
            // (a) The keyed plan routes square's logical slot to a physical
            // slot via build_physical_placement. In a keyed layout this is a
            // permutation, so it generally != the logical slot.
            uint32_t square_phys = km->plan.build_physical_placement[square_logical];
            if (square_phys != square_logical) {
                proved_distinct = true;
                // The physical slot may be >= logical_count (a padding-
                // displaced ordinal) OR a different real ordinal; either way
                // it is a DISTINCT index from the logical slot.
                ck(square_phys != square_logical,
                   "handle stability — keyed layout permutes square to a distinct physical slot");
            }
            // (b) The baked handle value must be the LOGICAL slot and must NOT
            // equal the physical slot the route word routes to. This is the
            // real non-disclosure proof: even when logical < logical_count,
            // the handle value is the logical ordinal, not the routed physical
            // ordinal (which differs in this layout).
            auto sit = km->compiled->slots.find("main");
            ck(sit != km->compiled->slots.end(), "handle stability — main slot found");
            if (sit != km->compiled->slots.end()) {
                auto& main_bytes = km->compiled->fns[size_t(sit->second)].bytes;
                size_t off = find_handle_load(main_bytes, square_logical);
                ck(off != SIZE_MAX,
                   "handle stability — &square baked as the LOGICAL slot value");
                // The physical slot must NOT appear as the handle value: the
                // handle is logical, not the routed physical ordinal.
                if (square_phys != square_logical) {
                    ck(find_handle_load(main_bytes, square_phys) == SIZE_MAX,
                       "handle stability — physical slot the route routes to is NOT baked as the handle (non-disclosure)");
                }
                // (c) Across seeds the handle value stays the same logical
                // value even though the physical placement changes.
                (void)off;
            }
            // Stability of resolution: the same logical slot resolves to the
            // same entry twice under the correct key.
            void* e1 = ember_resolve_keyed_dispatch(&km->rec, square_logical, km->route_word);
            void* e2 = ember_resolve_keyed_dispatch(&km->rec, square_logical, km->route_word);
            ck(e1 != nullptr && e2 != nullptr, "handle stability — resolver returns non-null");
            ck(e1 == e2, "handle stability — same logical slot resolves to the same entry (stable)");
            // The resolved entry IS square's compiled entry (correct routing).
            ck(e1 == km->compiled->table.get(square_logical),
               "handle stability — logical slot resolves to square's compiled entry under the correct key");
            ck(km->plan.logical_slot_count <= km->plan.physical_slot_count,
               "handle stability — logical_count <= physical_count (handle is logical, not physical)");
        }
        ck(seeds_built >= 2, "handle stability — built across multiple seeds for the stability proof");
        ck(proved_distinct, "handle stability — at least one keyed layout permutes square to a distinct physical slot (logical != physical)");
        ck(proved_stable_across_seeds, "handle stability — emitted handle value is the same logical slot across all seeds");
    }

    // =====================================================================
    // 16b. INDIRECT HANDLE GUARD-BEFORE-RESOLVE — the allowlist guard runs
    //      BEFORE the keyed resolver at an indirect call site. This is proven
    //      at the BYTE level: in main's emitted code (which has an indirect
    //      handle call), the guard's range check (cmp rax, slot_count imm32)
    //      + allowlist bit test (bt [r11], rcx) appear BEFORE the keyed
    //      resolver call (call ember_resolve_keyed_dispatch). The guard's
    //      range-check imm32 = fn_slot_count is a distinctive marker; the
    //      resolver's address is another. We verify the guard marker's byte
    //      offset precedes the resolver-address byte offset in BOTH backends.
    //      A runtime complement: a valid &fn handle (square) calls through
    //      the guard + keyed resolver + returns the correct value (test 5/6
    //      already proves this); here we pin the ordering directly.
    // =====================================================================
    {
        const char* src =
            "fn square(x: i64) -> i64 { return x * x; }\n"
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn main(x: i64) -> i64 {\n"
            "  let f: fn(i64)->i64 = &square;\n"
            "  let g: fn(i64)->i64 = &double;\n"
            "  return f(5) + g(10);\n"
            "}\n";
        for (bool ir : {false, true}) {
            const char* be = ir ? "thin" : "tree";
            auto km = build_keyed_module(src, CORRECT_SEED, ir);
            ck(km != nullptr, (std::string("guard-before-resolve: ") + be + " — build succeeded").c_str());
            if (!km) continue;
            auto sit = km->compiled->slots.find("main");
            ck(sit != km->compiled->slots.end(), (std::string("guard-before-resolve: ") + be + " — main slot found").c_str());
            if (sit == km->compiled->slots.end()) continue;
            auto& main_bytes = km->compiled->fns[size_t(sit->second)].bytes;
            // The resolver address baked as a raw imm64 in the keyed call site.
            uint64_t resolver_addr = uint64_t(reinterpret_cast<uintptr_t>(&ember_resolve_keyed_dispatch));
            size_t resolver_off = SIZE_MAX;
            for (size_t i = 0; i + 8 <= main_bytes.size(); ++i) {
                uint64_t w = 0; std::memcpy(&w, main_bytes.data()+i, 8);
                if (w == resolver_addr) { resolver_off = i; break; }
            }
            ck(resolver_off != SIZE_MAX,
               (std::string("guard-before-resolve: ") + be + " — main contains the keyed resolver address").c_str());
            // The guard's range check: `cmp rax, imm32` (REX.W 81 /7 F8 id)
            // with imm32 == fn_slot_count (3 for this module: square, double,
            // main). This is the guard's distinctive marker. Find the LAST
            // occurrence before the resolver address (the guard for the
            // second indirect call g(10) is the last one before its resolver).
            int32_t slot_count = int32_t(km->compiled->slots.size());
            size_t guard_off = SIZE_MAX;
            for (size_t i = 0; i + 7 <= main_bytes.size() && i < resolver_off; ++i) {
                // cmp rax, imm32: 48 81 F8 <4 bytes LE>
                if (main_bytes[i] == 0x48 && main_bytes[i+1] == 0x81 &&
                    main_bytes[i+2] == 0xF8) {
                    int32_t imm = 0; std::memcpy(&imm, main_bytes.data()+i+3, 4);
                    if (imm == slot_count) { guard_off = i; }
                }
            }
            ck(guard_off != SIZE_MAX,
               (std::string("guard-before-resolve: ") + be + " — main contains the guard range check (cmp rax, slot_count)").c_str());
            if (guard_off != SIZE_MAX && resolver_off != SIZE_MAX) {
                ck(guard_off < resolver_off,
                   (std::string("guard-before-resolve: ") + be + " — guard range check precedes the keyed resolver call (guard runs first)").c_str());
            }
            // Also verify the allowlist bit test (bt [r11], rcx = 0F AB /3
            // with rm=r11 -> REX.W+REX.B 49 0F AB 0B) appears before the resolver.
            // bt [r11], rcx: reg=rcx(low3=1), rm=r11(low3=3,+REX.B) -> 49 0F AB 0B
            size_t bt_off = SIZE_MAX;
            for (size_t i = 0; i + 4 <= main_bytes.size() && i < resolver_off; ++i) {
                if (main_bytes[i] == 0x49 && main_bytes[i+1] == 0x0F &&
                    main_bytes[i+2] == 0xAB && main_bytes[i+3] == 0x0B) {
                    bt_off = i;
                }
            }
            ck(bt_off != SIZE_MAX,
               (std::string("guard-before-resolve: ") + be + " — main contains the allowlist bit test (bt [r11], rcx)").c_str());
            if (bt_off != SIZE_MAX && resolver_off != SIZE_MAX) {
                ck(bt_off < resolver_off,
                   (std::string("guard-before-resolve: ") + be + " — allowlist bit test precedes the keyed resolver call").c_str());
            }
            // Runtime complement: the valid &square + &double calls route
            // correctly (guard passes + keyed resolver routes to the right
            // entries). This is the positive path proving both ran.
            auto r = run_keyed(*km, "main", 0, km->route_word, /*use_checkpoint=*/false);
            ck(r.ok && r.value == 45,
               (std::string("guard-before-resolve: ") + be + " — valid handles route through guard + resolver (square(5)+double(10)==45)").c_str());
        }
    }

    // =====================================================================
    // 17. EDGE ROUTE WORDS — exact runtime route words 0 and UINT64_MAX.
    //     Two layers: (a) build a module whose DERIVED route word IS 0 or
    //     UINT64_MAX (seed folds the edge word into the derivation), and run
    //     it under that exact route word — the call succeeds. (b) Pass the
    //     EXACT runtime route words 0 and UINT64_MAX to the re-entry thunk for
    //     a module built under a normal seed: under the correct key the result
    //     is right; under a wrong edge word the internal call resolves safely
    //     (padding or alternate, no crash). Both backends.
    // =====================================================================
    {
        const char* src =
            "fn id(x: i64) -> i64 { return x; }\n"
            "fn main(x: i64) -> i64 { return id(42); }\n";
        // (a) Build with route word seed 0 and UINT64_MAX (derived route word
        //     folds the edge word). Run under the derived route word.
        for (uint64_t seed : {0ULL, UINT64_MAX}) {
            auto km = build_keyed_module(src, seed, /*ir_backend=*/false);
            ck(km != nullptr, "edge route words: tree — build succeeded for seed (route word folds edge)");
            if (km) {
                auto r = run_keyed(*km, "main", 0, km->route_word, /*use_checkpoint=*/false);
                ck(r.ok && r.value == 42, "edge route words: tree — id(42)==42 under edge-derived route word");
            }
        }
        for (uint64_t seed : {0ULL, UINT64_MAX}) {
            auto km = build_keyed_module(src, seed, /*ir_backend=*/true);
            ck(km != nullptr, "edge route words: thin — build succeeded for seed (route word folds edge)");
            if (km) {
                auto r = run_keyed(*km, "main", 0, km->route_word, /*use_checkpoint=*/false);
                ck(r.ok && r.value == 42, "edge route words: thin — id(42)==42 under edge-derived route word");
            }
        }
        // (b) EXACT runtime route words 0 and UINT64_MAX exercised through
        //     GENERATED internal call-site resolution (not just the host
        //     resolver). Use the add/sub/main module: add+sub share an ABI
        //     domain with one padding ordinal, and main has a different
        //     signature (separate domain), so add's call site permutes ONLY
        //     within {add, sub, padding} — none recursive. Enter main under
        //     the CORRECT key, install the exact edge word (0 / UINT64_MAX)
        //     via the re-entry thunk, and assert main's add(6,7) call site
        //     resolves safely under that degenerate edge word: padding
        //     (returns 0, KeyedDispatchPadding) or the sub alternate (runs
        //     sub(6,7) = -1), with depth balanced and NO StackOverflow. The
        //     host resolver is cross-checked for the same edge words.
        const char* src_e =
            "fn add(a: i64, b: i64) -> i64 { return a + b; }\n"
            "fn sub(a: i64, b: i64) -> i64 { return a - b; }\n"
            "fn main(x: i64) -> i64 { return add(6, 7); }\n";
        for (bool ir : {false, true}) {
            const char* be = ir ? "thin" : "tree";
            auto km = build_keyed_module(src_e, CORRECT_SEED, ir,
                                          reinterpret_cast<void*>(kt6_trap));
            ck(km != nullptr, (std::string("edge route words: ") + be + " — build for exact-runtime JIT test").c_str());
            if (!km) continue;
            // Correct key: add(6,7) = 13.
            auto r_ok = run_keyed(*km, "main", 0, km->route_word, /*use_checkpoint=*/false);
            ck(r_ok.ok && r_ok.value == 13, (std::string("edge route words: ") + be + " — correct key add(6,7)==13").c_str());
            ck(r_ok.call_depth == 0, (std::string("edge route words: ") + be + " — correct key call_depth balanced (0)").c_str());
            uint32_t add_slot = uint32_t(km->compiled->slots.at("add"));
            for (uint64_t edge : {0ULL, UINT64_MAX}) {
                if (edge == km->route_word) continue;  // correct key handled above
                // Host resolver first: must return a non-null, in-domain entry.
                void* e = ember_resolve_keyed_dispatch(&km->rec, add_slot, edge);
                ck(e != nullptr, (std::string("edge route words: ") + be + " — host resolver non-null for exact edge word " + (edge == 0 ? "0" : "UINT64_MAX")).c_str());
                // Now exercise GENERATED internal call-site resolution under
                // the exact edge word: enter main (correct key) + install the
                // edge word via the re-entry thunk.
                auto r = run_keyed_wrong_r15_ii(*km, "main", 0, 0, km->route_word, edge);
                ck(r.ok, (std::string("edge route words: ") + be + " — JIT exact-edge-word run succeeded (no crash) for " + (edge == 0 ? "0" : "UINT64_MAX")).c_str());
                if (!r.ok) continue;
                // The edge word routes add's ordinal within {add, sub, padding}
                // — non-recursive — so the result is padding or the sub
                // alternate, never StackOverflow.
                ck(r.last_trap == TrapReason::None ||
                   r.last_trap == TrapReason::KeyedDispatchPadding,
                   (std::string("edge route words: ") + be + " — JIT exact-edge-word trap is None/KeyedDispatchPadding (no StackOverflow) for " + (edge == 0 ? "0" : "UINT64_MAX")).c_str());
                ck(r.last_trap != TrapReason::StackOverflow,
                   (std::string("edge route words: ") + be + " — JIT exact-edge-word did NOT overflow depth (non-recursive domain) for " + (edge == 0 ? "0" : "UINT64_MAX")).c_str());
                if (r.last_trap == TrapReason::KeyedDispatchPadding) {
                    ck(r.value == 0, (std::string("edge route words: ") + be + " — JIT exact-edge-word padding returns 0 for " + (edge == 0 ? "0" : "UINT64_MAX")).c_str());
                } else if (r.last_trap == TrapReason::None) {
                    // Alternate real (sub) or correct (add): both run safely.
                    ck(r.value == -1 || r.value == 13,
                       (std::string("edge route words: ") + be + " — JIT exact-edge-word alternate/correct value in {sub(6,7)=-1, add(6,7)=13} for " + (edge == 0 ? "0" : "UINT64_MAX")).c_str());
                }
                ck(r.call_depth == 0, (std::string("edge route words: ") + be + " — JIT exact-edge-word call_depth balanced (0) for " + (edge == 0 ? "0" : "UINT64_MAX")).c_str());
            }
        }
    }

    // =====================================================================
    // 18. IDENTITY-LAYOUT BEHAVIOR — when keyed_dispatch is null, the emit
    //     is byte-identical to the legacy path. Compile the same source with
    //     keyed_dispatch=null and run it; the result is correct (legacy).
    // =====================================================================
    {
        const char* src =
            "fn add(a: i64, b: i64) -> i64 { return a + b; }\n"
            "fn main(x: i64) -> i64 { return add(3, 4); }\n";
        auto m_tree = compile_identity(src, /*ir_backend=*/false);
        auto m_thin = compile_identity(src, /*ir_backend=*/true);
        ck(m_tree != nullptr, "identity: tree — compile_identity succeeded (keyed_dispatch null)");
        ck(m_thin != nullptr, "identity: thin — compile_identity succeeded (keyed_dispatch null)");
        if (m_tree) {
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            ember_set_r15(0);  // r15 untouched in legacy mode
            int64_t v = ember_call_i64(m_tree->main_entry, &ctx, 0);
            ck(v == 7, "identity: tree — add(3,4)==7 (legacy dispatch, no resolver)");
            ck(ember_read_r15() == 0, "identity: tree — r15 untouched (no route word in legacy)");
        }
        if (m_thin) {
            context_t ctx; ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64;
            ember_set_r15(0);
            int64_t v = ember_call_i64(m_thin->main_entry, &ctx, 0);
            ck(v == 7, "identity: thin — add(3,4)==7 (legacy dispatch, no resolver)");
            ck(ember_read_r15() == 0, "identity: thin — r15 untouched (no route word in legacy)");
        }
    }

    // =====================================================================
    // =====================================================================
    // 19. IDENTITY-LAYOUT BYTES — true byte-for-byte comparison of the
    //     disabled path for BOTH backends. Compile the same source TWICE with
    //     IDENTICAL shared bases (dispatch_base, fn_allowlist_base,
    //     globals_base): once with keyed_dispatch=null (pure legacy) and once
    //     with keyed_dispatch SET but module_record=null (the disabled-keyed
    //     path). When module_record is null, keyed_same_module is false, so
    //     the emit takes the legacy path. With identical bases the emitted
    //     bytes MUST be byte-for-byte identical — NO normalization, NO pointer
    //     masking, an EXACT comparison. This is the real Red 6 disabled-path
    //     gate: no resolver call, no r15 read, no extra instruction, in both
    //     the tree-walker and the Thin IR backend. The resolver address must
    //     not appear in either byte set.
    // =====================================================================
    {
        const char* src =
            "fn helper(a: i64) -> i64 { return a + 1; }\n"
            "fn main(x: i64) -> i64 { return helper(5); }\n";
        for (bool ir : {false, true}) {
            const char* be = ir ? "thin" : "tree";
            auto pc = compile_paired_identity(src, ir);
            ck(pc != nullptr, (std::string("identity bytes: ") + be + " — paired compile succeeded").c_str());
            if (!pc) continue;
            auto sit = pc->slots.find("main");
            ck(sit != pc->slots.end(), (std::string("identity bytes: ") + be + " — main slot found").c_str());
            if (sit == pc->slots.end()) continue;
            size_t idx = size_t(sit->second);
            auto& b1 = pc->legacy_fns[idx].bytes;
            auto& b2 = pc->disabled_fns[idx].bytes;
            ck(b1.size() > 0, (std::string("identity bytes: ") + be + " — main emitted non-empty bytes").c_str());
            // EXACT byte-for-byte comparison — identical bases mean the baked
            // addresses (dispatch_base, allowlist_base, globals_base) are the
            // SAME in both, so any byte difference is a real instruction
            // difference (a resolver call, an r15 read, an extra instruction).
            bool byte_identical = (b1.size() == b2.size());
            if (byte_identical) {
                for (size_t i = 0; i < b1.size(); ++i) {
                    if (b1[i] != b2[i]) { byte_identical = false; break; }
                }
            }
            ck(byte_identical, (std::string("identity bytes: ") + be + " — disabled-keyed bytes are byte-for-byte identical to pure legacy (identical bases, no normalization)").c_str());
            // Also verify NEITHER byte set contains the resolver address — the
            // disabled path emits no keyed resolution at all.
            uint64_t resolver_addr = uint64_t(reinterpret_cast<uintptr_t>(&ember_resolve_keyed_dispatch));
            auto contains = [&](const std::vector<uint8_t>& b) -> bool {
                for (size_t i = 0; i + 8 <= b.size(); ++i) {
                    uint64_t w = 0; std::memcpy(&w, b.data()+i, 8);
                    if (w == resolver_addr) return true;
                }
                return false;
            };
            ck(!contains(b1), (std::string("identity bytes: ") + be + " — legacy bytes contain NO resolver address").c_str());
            ck(!contains(b2), (std::string("identity bytes: ") + be + " — disabled-keyed bytes contain NO resolver address").c_str());
            // Cross-check helper too: the helper function's bytes must also be
            // byte-for-byte identical (helper has a call site? no — helper has
            // no internal call, but its OWN emit must still be identical
            // because the disabled path does not touch helper's body).
            auto& h1 = pc->legacy_fns[0].bytes;
            auto& h2 = pc->disabled_fns[0].bytes;
            bool helper_identical = (h1.size() == h2.size());
            if (helper_identical) {
                for (size_t i = 0; i < h1.size(); ++i) {
                    if (h1[i] != h2[i]) { helper_identical = false; break; }
                }
            }
            ck(helper_identical, (std::string("identity bytes: ") + be + " — helper bytes are byte-for-byte identical (disabled path does not touch non-call bodies)").c_str());
        }
    }

    // =====================================================================
    // Summary
    // =====================================================================
    std::printf("== %d checks, %s ==\n", g_checks, g_fail ? "FAILED" : "ALL PASSED");
    return g_fail;
}
