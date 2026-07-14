#include "src/engine.hpp"
#include "src/context.hpp"
#include "src/dispatch_table.hpp"
#include "src/keyed_dispatch.hpp"
#include "src/module_layout.hpp"
#include "src/codegen.hpp"
#include "src/lexer.hpp"
#include "src/parser.hpp"
#include "src/sema.hpp"
#include "src/globals.hpp"
#include "src/binding_builder.hpp"
#include "src/dispatch_abi.hpp"
#include "import.hpp"
#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <atomic>
#include <algorithm>
using namespace ember;
extern "C" void kt6_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) { ctx->last_trap = static_cast<ember::TrapReason>(reason); ctx->last_error = detail?detail:""; if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1); }
    std::abort();
}
struct Compiled { std::vector<CompiledFn> fns; DispatchTable table; std::unordered_map<std::string,int> slots; std::unordered_map<std::string,NativeSig> natives; std::vector<uint8_t> allowlist; ~Compiled(){for(auto&f:fns)if(f.exec)free_executable(f.exec);} };
struct KeyedModule { std::unique_ptr<Compiled> compiled; ModuleManifest manifest; ModuleLayoutPlan plan; RecordBuilderStorage st; ModuleDispatchRecord rec{}; uint64_t route_word=0; };
static AbiType prim_to_abi(Prim p){return abi_scalar(p==Prim::F64?PrimTag::F64:PrimTag::I64);}
static AbiType type_to_abi(const Type& t){if(t.is_slice){AbiType at;at.kind=TypeKind::Slice;at.elem=std::make_shared<AbiType>(abi_scalar(PrimTag::Void));return at;}if(t.is_lambda){AbiType at;at.kind=TypeKind::Lambda;return at;}return prim_to_abi(t.prim);}
static uint64_t fp_for(const FuncDecl& f){CallableDescriptor d;d.calling_mode=CallingMode::LegacyContext;d.visibility=Visibility::Public;const Type& rt=f.ret?*f.ret:type_void();d.return_type=type_to_abi(rt);d.return_kind=rt.is_void()?ReturnKind::Void:ReturnKind::GpScalar;d.has_hidden_env=f.is_lambda;uint32_t pos=d.has_hidden_env?1:0;size_t begin=f.is_lambda?1:0;for(size_t i=begin;i<f.params.size();++i){d.params.push_back(abi_param(type_to_abi(*f.params[i].ty),1,8,0,{WordClass::GP},pos));pos+=1;}return classify_callable(d).fingerprint;}
int main() {
    const char* src="fn add(a: i64, b: i64) -> i64 { return a + b; }\nfn sub(a: i64, b: i64) -> i64 { return a - b; }\nfn main(a: i64, b: i64) -> i64 { return add(a, b); }\n";
    uint64_t seed=0xCAFEBABE12345678ULL;
    auto km=std::make_unique<KeyedModule>();
    {std::unordered_set<std::string> seen;std::string resolved=resolve_imports(src,"./",seen);auto lr=tokenize(resolved,"<km>");auto pr=parse(std::move(lr.toks));std::unordered_map<std::string,int> slots;int si=0;for(auto&fn:pr.program.funcs){slots[fn.name]=si++;}km->manifest.module_id="t";for(auto&[name,slot]:slots){ModuleCallable c;c.name=name;c.logical_slot=uint32_t(slot);for(auto&fn:pr.program.funcs)if(fn.name==name)c.abi_fingerprint=fp_for(fn);c.visibility=Visibility::Public;c.calling_mode=CallingMode::LegacyContext;km->manifest.callables.push_back(std::move(c));}std::sort(km->manifest.callables.begin(),km->manifest.callables.end(),[](const ModuleCallable&a,const ModuleCallable&b){return a.logical_slot<b.logical_slot;});}
    uint8_t kb[8];for(int i=0;i<8;++i)kb[i]=uint8_t((seed>>(8*i))&0xFF);BuildKeyView key{kb,8};ImplicitKeyedLayoutV1 planner;km->plan=std::move(*planner.plan(km->manifest,key).value);km->route_word=derive_route_word(key);
    km->st.routes=km->plan.logical_routes;km->st.domains=km->plan.domains;km->st.allowlist.assign((km->plan.logical_slot_count+7u)>>3,0);for(uint32_t i=0;i<km->plan.logical_slot_count;++i)km->st.allowlist[i>>3]|=(uint8_t(1)<<(i&7u));km->st.descriptors.assign(km->plan.physical_slot_count,PhysicalEntryDescriptor{});for(uint32_t s=0;s<km->plan.physical_slot_count;++s){const auto&pe=km->plan.physical_entries[s];auto&pd=km->st.descriptors[s];pd.physical_slot=pe.physical_slot;pd.abi_fingerprint=pe.abi_fingerprint;pd.visibility=pe.visibility;pd.calling_mode=pe.calling_mode;pd.dispatch_domain=pe.dispatch_domain;pd.is_padding=pe.is_padding;pd.logical_slot=pe.logical_slot;pd.domain_index=pe.domain_index;pd.ordinal=pe.ordinal;}
    const void* pad=ember_keyed_padding_trap_target();km->st.storage=std::vector<std::atomic<void*>>(km->plan.physical_slot_count);for(uint32_t s=0;s<km->plan.physical_slot_count;++s)km->st.storage[s].store(const_cast<void*>(pad),std::memory_order_release);
    km->rec.mode=DispatchMode::Keyed;km->rec.strategy_version=1;km->rec.physical_slots=km->st.storage.data();km->rec.physical_slot_count=km->plan.physical_slot_count;km->rec.logical_slot_count=km->plan.logical_slot_count;km->rec.logical_routes=km->st.routes.data();km->rec.domains=km->st.domains.data();km->rec.domain_count=uint32_t(km->st.domains.size());km->rec.logical_allowlist=km->st.allowlist.data();km->rec.logical_allowlist_bytes=uint32_t(km->st.allowlist.size());km->rec.physical_descriptors=km->st.descriptors.data();km->rec.physical_descriptor_count=uint32_t(km->st.descriptors.size());km->rec.padding_trap_target=pad;
    auto m=std::make_unique<Compiled>();std::unordered_set<std::string> seen;std::string resolved=resolve_imports(src,"./",seen);auto lr=tokenize(resolved,"<t>");auto pr=parse(std::move(lr.toks));int si=0;for(auto&fn:pr.program.funcs){m->slots[fn.name]=si++;fn.slot=m->slots[fn.name];}OpOverloadTable ov;auto layouts=build_struct_layouts(pr.program);pr.program.string_xor_key=0;sema(pr.program,m->natives,m->slots,0,&ov,&layouts);GlobalsBlock gb;std::vector<uint8_t> gbs(0);gb.base=int64_t(gbs.data());g_globals_for_codegen=&gb;m->table=DispatchTable(pr.program.funcs.size());m->allowlist=build_fn_allowlist(m->slots,int(pr.program.funcs.size()));CodeGenCtx ctx;ctx.globals_base=gb.base;ctx.dispatch_base=int64_t(m->table.base());ctx.natives=&m->natives;ctx.script_slots=&m->slots;ctx.structs=&layouts;ctx.use_context_reg=true;ctx.emit_depth_checks=true;ctx.max_call_depth=64;ctx.fn_allowlist_base=int64_t(m->allowlist.data());ctx.fn_slot_count=int(pr.program.funcs.size());ctx.trap_stub=(void*)&kt6_trap;ctx.trap_ctx=nullptr;KeyedDispatchCodegen kd{};kd.runtime_key=RuntimeKeyLocation::R15;kd.module_record=&km->rec;ctx.keyed_dispatch=&kd;ctx.enable_ir_backend=true;for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx);finalize(cf);m->table.set(fn.slot,cf.entry);m->fns.push_back(std::move(cf));}km->compiled=std::move(m);
    for(uint32_t s=0;s<km->plan.physical_slot_count;++s){const auto&pe=km->plan.physical_entries[s];if(pe.is_padding)continue;km->st.storage[s].store(km->compiled->table.get(pe.logical_slot),std::memory_order_release);}
    // Find a wrong key that routes add(0) to main(2)
    uint64_t wrong=0;
    uint64_t tk[]={km->route_word^0xFFFFFFFFFFFFFFFFULL,km->route_word+1,0ULL,UINT64_MAX,0x0123456789ABCDEFULL};
    for(uint64_t w:tk){void*e=ember_resolve_keyed_dispatch(&km->rec,0,w);printf("key 0x%llx: add(0)->%p pad=%d\n",(unsigned long long)w,e,ember_is_padding_trap_target(e));if(e&&!ember_is_padding_trap_target(e)&&e==km->compiled->table.get(2)){wrong=w;printf("FOUND 0x%llx routes add->main\n",(unsigned long long)w);}}
    if(wrong==0){wrong=km->route_word^0xFFFFFFFFFFFFFFFFULL;printf("using affe\n");}
    void* main_entry=ember_resolve_keyed_dispatch(&km->rec,2,km->route_word);
    auto ctxp=std::make_unique<context_t>();ctxp->budget_remaining=1000000000;ctxp->max_call_depth=64;ctxp->has_checkpoint=true;
    printf("thin keyed add->main recursion: calling main with wrong r15=0x%llx...\n",(unsigned long long)wrong);fflush(stdout);
    if(__builtin_setjmp(ctxp->checkpoint)){printf("TRAPPED: %s (depth=%d)\n",trap_reason_str(ctxp->last_trap),ctxp->call_depth);return 0;}
    int64_t v=ember_keyed_reentry_i64_i64(main_entry,ctxp.get(),6,7,wrong);
    printf("ERROR: should have trapped, got %lld\n",(long long)v);return 1;
}
