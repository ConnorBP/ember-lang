#include "src/engine.hpp"
#include "src/context.hpp"
#include "src/dispatch_table.hpp"
#include "src/keyed_dispatch.hpp"
#include "src/module_layout.hpp"
#include "src/module_instance.hpp"
#include "src/key_provider.hpp"
#include "src/codegen.hpp"
#include "src/regalloc.hpp"
#include "src/lexer.hpp"
#include "src/parser.hpp"
#include "src/sema.hpp"
#include "src/globals.hpp"
#include "src/binding_builder.hpp"
#include "src/thin_ir.hpp"
#include "src/thin_lower.hpp"
#include "src/thin_emit.hpp"
#include "src/dispatch_abi.hpp"
#include "src/extension_registry.hpp"
#include "import.hpp"
#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <atomic>
#include <array>
#include <algorithm>
using namespace ember;

extern "C" void kt6_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1);
    }
    std::abort();
}

struct Compiled {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string, int> slots;
    std::unordered_map<std::string, NativeSig> natives;
    void* main_entry = nullptr;
    std::vector<uint8_t> allowlist;
    ~Compiled() { for (auto& fn : fns) if (fn.exec) free_executable(fn.exec); }
};

struct KeyedModule {
    std::unique_ptr<Compiled> compiled;
    ModuleManifest manifest;
    ModuleLayoutPlan plan;
    RecordBuilderStorage st;
    ModuleDispatchRecord rec{};
    uint64_t route_word = 0;
};

static AbiType prim_to_abi(Prim p) {
    switch (p) {
    case Prim::I64: return abi_scalar(PrimTag::I64);
    case Prim::F64: return abi_scalar(PrimTag::F64);
    default: return abi_scalar(PrimTag::Void);
    }
}
static AbiType type_to_abi(const Type& t) {
    if (t.is_slice) { AbiType at; at.kind=TypeKind::Slice; at.elem=std::make_shared<AbiType>(abi_scalar(PrimTag::Void)); return at; }
    if (t.is_lambda) { AbiType at; at.kind=TypeKind::Lambda; return at; }
    return prim_to_abi(t.prim);
}
static uint64_t fingerprint_for(const FuncDecl& f) {
    CallableDescriptor d;
    d.calling_mode = CallingMode::LegacyContext;
    d.visibility = Visibility::Public;
    const Type& rt = f.ret ? *f.ret : type_void();
    d.return_type = type_to_abi(rt);
    d.return_kind = rt.is_void() ? ReturnKind::Void : ReturnKind::GpScalar;
    d.has_hidden_env = f.is_lambda;
    uint32_t pos = d.has_hidden_env ? 1u : 0u;
    size_t begin = f.is_lambda ? 1u : 0u;
    for (size_t i = begin; i < f.params.size(); ++i) {
        const Type& pt = *f.params[i].ty;
        AbiType at = type_to_abi(pt);
        d.params.push_back(abi_param(std::move(at), 1, 8, 0, {WordClass::GP}, pos));
        pos += 1;
    }
    return classify_callable(d).fingerprint;
}

int main() {
    const char* src = "fn add(a: i64, b: i64) -> i64 { return a + b; }\nfn sub(a: i64, b: i64) -> i64 { return a - b; }\nfn main(a: i64, b: i64) -> i64 { return add(a, b); }\n";
    uint64_t seed = 0xCAFEBABE12345678ULL;
    auto km = std::make_unique<KeyedModule>();
    {
        std::unordered_set<std::string> seen; std::string resolved = resolve_imports(src, "./", seen);
        auto lr = tokenize(resolved, "<km>"); auto pr = parse(std::move(lr.toks));
        std::unordered_map<std::string,int> slots; int si=0;
        for(auto&fn:pr.program.funcs){slots[fn.name]=si++;}
        km->manifest.module_id = "test.mod";
        for (auto& [name, slot] : slots) {
            ModuleCallable c; c.name=name; c.logical_slot=uint32_t(slot);
            for (auto& fn : pr.program.funcs) if (fn.name == name) c.abi_fingerprint = fingerprint_for(fn);
            c.visibility = Visibility::Public; c.calling_mode = CallingMode::LegacyContext;
            km->manifest.callables.push_back(std::move(c));
        }
        std::sort(km->manifest.callables.begin(), km->manifest.callables.end(),
                  [](const ModuleCallable& a, const ModuleCallable& b){ return a.logical_slot < b.logical_slot; });
    }
    uint8_t key_bytes[8]; for(int i=0;i<8;++i) key_bytes[i]=uint8_t((seed>>(8*i))&0xFF);
    BuildKeyView key{key_bytes, 8};
    ImplicitKeyedLayoutV1 planner;
    auto pr = planner.plan(km->manifest, key);
    km->plan = std::move(*pr.value);
    km->route_word = derive_route_word(key);
    printf("route_word = 0x%llx\n", (unsigned long long)km->route_word);
    printf("logical_slot_count=%u physical_slot_count=%u\n", km->plan.logical_slot_count, km->plan.physical_slot_count);
    for (auto& pe : km->plan.physical_entries) {
        printf("  phys %u: logical=%u is_padding=%d\n", pe.physical_slot, pe.logical_slot, (int)pe.is_padding);
    }
    km->st.routes = km->plan.logical_routes;
    km->st.domains = km->plan.domains;
    km->st.allowlist.assign((km->plan.logical_slot_count + 7u) >> 3, 0);
    for (uint32_t i = 0; i < km->plan.logical_slot_count; ++i) km->st.allowlist[i >> 3] |= (uint8_t(1) << (i & 7u));
    km->st.descriptors.assign(km->plan.physical_slot_count, PhysicalEntryDescriptor{});
    for (uint32_t s = 0; s < km->plan.physical_slot_count; ++s) {
        const auto& pe = km->plan.physical_entries[s];
        auto& pd = km->st.descriptors[s];
        pd.physical_slot=pe.physical_slot; pd.abi_fingerprint=pe.abi_fingerprint;
        pd.visibility=pe.visibility; pd.calling_mode=pe.calling_mode; pd.dispatch_domain=pe.dispatch_domain;
        pd.is_padding=pe.is_padding; pd.logical_slot=pe.logical_slot; pd.domain_index=pe.domain_index; pd.ordinal=pe.ordinal;
    }
    const void* pad_stub = ember_keyed_padding_trap_target();
    km->st.storage = std::vector<std::atomic<void*>>(km->plan.physical_slot_count);
    for (uint32_t s = 0; s < km->plan.physical_slot_count; ++s) km->st.storage[s].store(const_cast<void*>(pad_stub), std::memory_order_release);
    km->rec.mode = DispatchMode::Keyed; km->rec.strategy_version = 1;
    km->rec.physical_slots = km->st.storage.data(); km->rec.physical_slot_count = km->plan.physical_slot_count;
    km->rec.logical_slot_count = km->plan.logical_slot_count;
    km->rec.logical_routes = km->st.routes.data(); km->rec.domains = km->st.domains.data();
    km->rec.domain_count = uint32_t(km->st.domains.size());
    km->rec.logical_allowlist = km->st.allowlist.data(); km->rec.logical_allowlist_bytes = uint32_t(km->st.allowlist.size());
    km->rec.physical_descriptors = km->st.descriptors.data(); km->rec.physical_descriptor_count = uint32_t(km->st.descriptors.size());
    km->rec.padding_trap_target = pad_stub;
    auto m = std::make_unique<Compiled>();
    std::unordered_set<std::string> seen; std::string resolved = resolve_imports(src, "./", seen);
    auto lr = tokenize(resolved, "<t>"); auto pr2 = parse(std::move(lr.toks));
    int si=0; for(auto&fn:pr2.program.funcs){m->slots[fn.name]=si++;fn.slot=m->slots[fn.name];}
    ember::OpOverloadTable ov; auto layouts = build_struct_layouts(pr2.program); pr2.program.string_xor_key=0;
    sema(pr2.program,m->natives,m->slots,0,&ov,&layouts);
    ember::GlobalsBlock gb; std::vector<uint8_t> gbs(0); gb.base=int64_t(gbs.data());
    ember::g_globals_for_codegen=&gb;
    m->table = DispatchTable(pr2.program.funcs.size());
    m->allowlist = build_fn_allowlist(m->slots, int(pr2.program.funcs.size()));
    ember::CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(m->table.base());
    ctx.natives=&m->natives; ctx.script_slots=&m->slots; ctx.structs=&layouts;
    ctx.use_context_reg=true; ctx.emit_depth_checks=true; ctx.max_call_depth=64;
    ctx.fn_allowlist_base=int64_t(m->allowlist.data()); ctx.fn_slot_count=int(pr2.program.funcs.size());
    ctx.trap_stub=(void*)&kt6_trap; ctx.trap_ctx=nullptr;
    KeyedDispatchCodegen kd{}; kd.runtime_key=RuntimeKeyLocation::R15; kd.module_record=&km->rec;
    ctx.keyed_dispatch=&kd; ctx.enable_ir_backend=false;
    for(auto&fn:pr2.program.funcs){auto cf=compile_func(fn,ctx);finalize(cf);m->table.set(fn.slot,cf.entry);m->fns.push_back(std::move(cf));}
    km->compiled = std::move(m);
    for (uint32_t s = 0; s < km->plan.physical_slot_count; ++s) {
        const auto& pe = km->plan.physical_entries[s];
        if (pe.is_padding) continue;
        void* entry = km->compiled->table.get(pe.logical_slot);
        km->st.storage[s].store(entry, std::memory_order_release);
    }
    uint64_t wrong_words[] = {
        km->route_word ^ 0xFFFFFFFFFFFFFFFFULL,
        km->route_word + 1,
        0ULL,
        UINT64_MAX,
        0x0123456789ABCDEFULL,
    };
    for (uint64_t wrong : wrong_words) {
        if (wrong == km->route_word) continue;
        printf("\nwrong key = 0x%llx\n", (unsigned long long)wrong);
        void* add_entry_wrong = ember_resolve_keyed_dispatch(&km->rec, 0, wrong);
        printf("add(0) under wrong key -> %p (pad? %d)\n", add_entry_wrong, ember_is_padding_trap_target(add_entry_wrong));
        void* main_entry = ember_resolve_keyed_dispatch(&km->rec, 2, km->route_word);
        context_t ectx; ectx.budget_remaining=1000000000; ectx.max_call_depth=64;
        ectx.has_checkpoint=true;
        if (__builtin_setjmp(ectx.checkpoint)) {
            printf("TRAPPED: %s (depth=%d): %s\n", trap_reason_str(ectx.last_trap), ectx.call_depth, ectx.last_error.c_str());
            continue;
        }
        int64_t v = ember_keyed_reentry_i64_i64(main_entry, &ectx, 6, 7, wrong);
        printf("result = %lld, last_trap=%s, depth=%d\n", (long long)v, trap_reason_str(ectx.last_trap), ectx.call_depth);
    }
    return 0;
}
