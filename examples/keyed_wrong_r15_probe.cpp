// Probe: test the wrong-r15 generated internal call-site resolution in isolation.
// This mirrors the keyed_dispatch_codegen_test's run_keyed_wrong_r15_ii helper
// to observe the actual behavior (padding trap, alternate real, or crash).
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

#include <algorithm>
#include <atomic>
#include <array>
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

struct FixedProvider : public DerivedMaterialProvider {
    std::array<uint8_t, 32> material{};
    explicit FixedProvider(uint64_t route_word_seed) {
        material.fill(0);
        for (int i = 0; i < 8; ++i) material[i] = uint8_t((route_word_seed >> (8 * i)) & 0xFF);
        for (int i = 8; i < 32; ++i) material[i] = uint8_t(0x42 + i);
    }
    ExtensionResult<std::array<uint8_t, 32>> derive(const DerivationRequest&) const override {
        return make_extension_result_ok(material);
    }
};

struct Compiled {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string, int> slots;
    std::unordered_map<std::string, NativeSig> natives;
    void* main_entry = nullptr;
    std::vector<uint8_t> allowlist;
    ~Compiled() { for (auto& fn : fns) if (fn.exec) free_executable(fn.exec); }
};

static std::unique_ptr<Compiled> compile_keyed(const std::string& src,
    const ModuleDispatchRecord* module_record, bool ir_backend, void* trap_stub = nullptr) {
    auto m = std::make_unique<Compiled>();
    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); } catch (...) { return nullptr; }
    auto lr = tokenize(resolved, "<probe>"); if (!lr.ok) return nullptr;
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
    m->allowlist = build_fn_allowlist(m->slots, nslots);
    ember::CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(m->table.base());
    ctx.natives=&m->natives; ctx.script_slots=&m->slots; ctx.structs=&layouts;
    ctx.globals_index = &gb.index; ctx.globals_types = &gb.types;
    ctx.use_context_reg = true; ctx.emit_depth_checks = true; ctx.max_call_depth = 64;
    ctx.fn_allowlist_base = int64_t(m->allowlist.data()); ctx.fn_slot_count = nslots;
    if (trap_stub) { ctx.trap_stub = trap_stub; ctx.trap_ctx = nullptr; }
    KeyedDispatchCodegen kd{}; kd.runtime_key = RuntimeKeyLocation::R15; kd.module_record = module_record;
    ctx.keyed_dispatch = &kd; ctx.enable_ir_backend = ir_backend;
    for(auto&fn:pr.program.funcs){
        auto cf=compile_func(fn,ctx); finalize(cf);
        if (!cf.entry) { std::printf("FAIL: null entry for %s\n", fn.name.c_str()); return nullptr; }
        m->table.set(fn.slot,cf.entry); m->fns.push_back(std::move(cf));
    }
    auto sit=m->slots.find("main"); if(sit!=m->slots.end()) m->main_entry = m->table.get(sit->second);
    return m;
}

static AbiType prim_to_abi(Prim p) {
    switch (p) {
    case Prim::Void:  return abi_scalar(PrimTag::Void);
    case Prim::I64:   return abi_scalar(PrimTag::I64);
    case Prim::F64:   return abi_scalar(PrimTag::F64);
    default:          return abi_scalar(PrimTag::I64);
    }
    return abi_scalar(PrimTag::Void);
}
static AbiType type_to_abi(const Type& t) {
    if (t.is_slice) { AbiType at; at.kind = TypeKind::Slice; at.elem = std::make_shared<AbiType>(abi_scalar(PrimTag::Void)); return at; }
    return prim_to_abi(t.prim);
}
static uint32_t words_for_type(const Type& t) { if (t.is_slice) return 2; return 1; }
static uint64_t fingerprint_for(const FuncDecl& f) {
    CallableDescriptor d; d.arch = TargetArch::X64_Win64; d.convention_version = kWin64ConventionVersion;
    d.calling_mode = CallingMode::LegacyContext; d.visibility = Visibility::Public;
    const Type& rt = f.ret ? *f.ret : type_void(); d.return_type = type_to_abi(rt);
    if (rt.is_void()) d.return_kind = ReturnKind::Void;
    else if (rt.is_float()) d.return_kind = (rt.prim == Prim::F64) ? ReturnKind::XmmF64 : ReturnKind::XmmF32;
    else d.return_kind = ReturnKind::GpScalar;
    d.is_coroutine = false; d.is_special_entry = false; d.has_hidden_env = false;
    uint32_t pos = 0;
    for (size_t i = 0; i < f.params.size(); ++i) {
        const Type& pt = *f.params[i].ty; AbiType at = type_to_abi(pt);
        uint32_t wc = words_for_type(pt); uint32_t byte_ext = 8u;
        uint32_t partial = 0; WordClass wc0 = word_class_for_type(at);
        std::vector<WordClass> wcs; for (uint32_t w = 0; w < wc; ++w) wcs.push_back(wc0);
        d.params.push_back(abi_param(std::move(at), wc, byte_ext, partial, std::move(wcs), pos));
        pos += wc;
    }
    return classify_callable(d).fingerprint;
}
static ModuleManifest make_manifest(const Program& prog, const std::unordered_map<std::string,int>& slots) {
    ModuleManifest m; m.module_id = "probe.mod";
    std::unordered_map<std::string, const FuncDecl*> by_name;
    for (auto& fn : prog.funcs) by_name[fn.name] = &fn;
    for (auto& [name, slot] : slots) {
        ModuleCallable c; c.name = name; c.logical_slot = uint32_t(slot);
        auto it = by_name.find(name);
        c.abi_fingerprint = (it != by_name.end()) ? fingerprint_for(*it->second) : 0xA0A0A0A0A0A0A0A0ULL;
        c.visibility = Visibility::Public; c.calling_mode = CallingMode::LegacyContext; c.dispatch_domain = "";
        m.callables.push_back(std::move(c));
    }
    std::sort(m.callables.begin(), m.callables.end(), [](const ModuleCallable& a, const ModuleCallable& b){ return a.logical_slot < b.logical_slot; });
    return m;
}

struct KeyedModule {
    std::unique_ptr<Compiled> compiled; ModuleManifest manifest; ModuleLayoutPlan plan;
    RecordBuilderStorage st; ModuleDispatchRecord rec{}; uint64_t route_word = 0;
};

static std::unique_ptr<KeyedModule> build_keyed_module(const std::string& src, uint64_t route_word_seed, bool ir_backend, void* trap_stub = nullptr) {
    auto km = std::make_unique<KeyedModule>();
    {
        std::unordered_set<std::string> seen; std::string resolved;
        try { resolved = resolve_imports(src, "./", seen); } catch (...) { return nullptr; }
        auto lr = tokenize(resolved, "<km>"); if (!lr.ok) return nullptr;
        auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
        std::unordered_map<std::string,int> slots; int si=0;
        for(auto&fn:pr.program.funcs){slots[fn.name]=si++;}
        km->manifest = make_manifest(pr.program, slots);
    }
    uint8_t key_bytes[8]; for (int i = 0; i < 8; ++i) key_bytes[i] = uint8_t((route_word_seed >> (8*i)) & 0xFF);
    BuildKeyView key{key_bytes, 8}; ImplicitKeyedLayoutV1 planner;
    auto pr = planner.plan(km->manifest, key); if (!pr) { std::printf("FAIL: plan\n"); return nullptr; }
    km->plan = std::move(*pr.value); km->route_word = derive_route_word(key);
    km->st.routes = km->plan.logical_routes; km->st.domains = km->plan.domains;
    km->st.allowlist.assign((km->plan.logical_slot_count + 7u) >> 3, 0);
    for (uint32_t i = 0; i < km->plan.logical_slot_count; ++i) km->st.allowlist[i >> 3] |= (uint8_t(1) << (i & 7u));
    km->st.descriptors.assign(km->plan.physical_slot_count, PhysicalEntryDescriptor{});
    for (uint32_t s = 0; s < km->plan.physical_slot_count; ++s) {
        const auto& pe = km->plan.physical_entries[s]; auto& pd = km->st.descriptors[s];
        pd.physical_slot=pe.physical_slot; pd.abi_fingerprint=pe.abi_fingerprint; pd.visibility=pe.visibility;
        pd.calling_mode=pe.calling_mode; pd.dispatch_domain=pe.dispatch_domain; pd.is_padding=pe.is_padding;
        pd.logical_slot=pe.logical_slot; pd.domain_index=pe.domain_index; pd.ordinal=pe.ordinal;
    }
    const void* pad_stub = ember_keyed_padding_trap_target();
    km->st.storage = std::vector<std::atomic<void*>>(km->plan.physical_slot_count);
    for (uint32_t s = 0; s < km->plan.physical_slot_count; ++s) km->st.storage[s].store(const_cast<void*>(pad_stub), std::memory_order_release);
    km->rec.mode=DispatchMode::Keyed; km->rec.strategy_version=1;
    km->rec.physical_slots=km->st.storage.data(); km->rec.physical_slot_count=km->plan.physical_slot_count;
    km->rec.logical_slot_count=km->plan.logical_slot_count; km->rec.logical_routes=km->st.routes.data();
    km->rec.domains=km->st.domains.data(); km->rec.domain_count=uint32_t(km->st.domains.size());
    km->rec.logical_allowlist=km->st.allowlist.data(); km->rec.logical_allowlist_bytes=uint32_t(km->st.allowlist.size());
    km->rec.physical_descriptors=km->st.descriptors.data(); km->rec.physical_descriptor_count=uint32_t(km->st.descriptors.size());
    km->rec.padding_trap_target = pad_stub;
    km->compiled = compile_keyed(src, &km->rec, ir_backend, trap_stub);
    if (!km->compiled) { std::printf("FAIL: compile\n"); return nullptr; }
    for (uint32_t s = 0; s < km->plan.physical_slot_count; ++s) {
        const auto& pe = km->plan.physical_entries[s]; if (pe.is_padding) continue;
        void* entry = km->compiled->table.get(pe.logical_slot);
        km->st.storage[s].store(entry, std::memory_order_release);
    }
    auto vs = validate_dispatch_record(km->rec);
    if (!vs) { std::printf("FAIL: validate\n"); return nullptr; }
    return km;
}

extern "C" void probe_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1);
    }
    std::abort();
}

struct WrongKeyResult { bool ok=false; int64_t value=0; TrapReason last_trap=TrapReason::None; int32_t call_depth=0; std::string reason; };

static WrongKeyResult run_wrong_r15_ii(KeyedModule& km, const std::string& entry_name, int64_t a, int64_t b, uint64_t correct_rw, uint64_t run_rw) {
    WrongKeyResult r;
    auto sit = km.compiled->slots.find(entry_name);
    if (sit == km.compiled->slots.end()) { r.reason = "entry not found"; return r; }
    // Resolve the OUTER entry under the CORRECT key so we enter the intended
    // caller (main). The wrong route word is installed by the keyed re-entry
    // thunk (r15 = run_rw), which is the ONLY reliable way to install r15 from
    // C++ (ember_set_r15 cannot work: the compiler treats r15 as callee-saved
    // and may restore it across the subsequent C++ call). The thunk installs
    // r15 = run_rw as part of the same asm sequence that calls the JIT entry.
    void* entry = ember_resolve_keyed_dispatch(&km.rec, uint32_t(sit->second), correct_rw);
    if (!entry) { r.reason = "outer resolver null"; return r; }
    auto ctxp = std::make_unique<context_t>(); context_t& ctx = *ctxp;
    ctx.budget_remaining = 1'000'000'000LL; ctx.max_call_depth = 64; ctx.has_checkpoint = true;
    if (__builtin_setjmp(ctx.checkpoint)) {
        r.ok=true; r.last_trap=ctx.last_trap; r.call_depth=ctx.call_depth;
        r.reason = std::string(trap_reason_str(ctx.last_trap)) + ": " + ctx.last_error;
        ctx.has_checkpoint=false; ctx.reset_for_call(); return r;
    }
    // The keyed re-entry thunk installs r14=ctx, r15=run_rw, calls entry,
    // clears r15, restores caller r14/r15. This tests GENERATED internal
    // call-site resolution: main runs with r15=run_rw, and its internal
    // add(6,7) call site invokes ember_resolve_keyed_dispatch(record, slot, r15).
    int64_t v = ember_keyed_reentry_i64_i64(entry, &ctx, a, b, run_rw);
    ctx.has_checkpoint = false;
    r.ok=true; r.value=v; r.last_trap=ctx.last_trap; r.call_depth=ctx.call_depth;
    return r;
}

int main() {
    std::printf("== wrong-r15 probe ==\n");
    const uint64_t SEED = 0xCAFEBABE12345678ULL;
    const char* src =
        "fn add(a: i64, b: i64) -> i64 { return a + b; }\n"
        "fn sub(a: i64, b: i64) -> i64 { return a - b; }\n"
        "fn main(x: i64) -> i64 { return add(6, 7); }\n";
    for (bool ir : {false, true}) {
        std::printf("--- backend %s ---\n", ir ? "thin" : "tree");
        auto km = build_keyed_module(src, SEED, ir, reinterpret_cast<void*>(probe_trap));
        if (!km) { std::printf("  build FAILED\n"); continue; }
        std::printf("  route_word=0x%016llx\n", (unsigned long long)km->route_word);
        // correct key
        auto r_ok = run_wrong_r15_ii(*km, "main", 0, 0, km->route_word, km->route_word);
        std::printf("  correct key: ok=%d value=%lld last_trap=%d depth=%d\n", r_ok.ok, (long long)r_ok.value, (int)r_ok.last_trap, r_ok.call_depth);
        // wrong keys
        uint64_t wrongs[] = { km->route_word ^ 0xFFFFFFFFFFFFFFFFULL, km->route_word + 1, 0ULL, UINT64_MAX, 0x0123456789ABCDEFULL };
        for (uint64_t w : wrongs) {
            if (w == km->route_word) continue;
            auto r = run_wrong_r15_ii(*km, "main", 0, 0, km->route_word, w);
            std::printf("  wrong 0x%016llx: ok=%d value=%lld last_trap=%d(%s) depth=%d reason=%s\n",
                (unsigned long long)w, r.ok, (long long)r.value, (int)r.last_trap,
                trap_reason_str(r.last_trap), r.call_depth, r.reason.c_str());
        }
    }
    std::printf("== probe done ==\n");
    return 0;
}
