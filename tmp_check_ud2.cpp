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
using namespace ember;
extern "C" void kt6_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) { ctx->last_trap = static_cast<ember::TrapReason>(reason); ctx->last_error = detail?detail:""; if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1); }
    std::abort();
}
int main() {
    const char* src = "fn add(a: i64, b: i64) -> i64 { return a + b; }\nfn sub(a: i64, b: i64) -> i64 { return a - b; }\nfn main(a: i64, b: i64) -> i64 { return add(a, b); }\n";
    std::unordered_set<std::string> seen; std::string resolved = resolve_imports(src, "./", seen);
    auto lr = tokenize(resolved, "<t>"); auto pr = parse(std::move(lr.toks));
    std::unordered_map<std::string,int> slots; int si=0;
    for(auto&fn:pr.program.funcs){slots[fn.name]=si++;fn.slot=slots[fn.name];}
    std::unordered_map<std::string,NativeSig> natives; OpOverloadTable ov;
    auto layouts = build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    sema(pr.program,natives,slots,0,&ov,&layouts);
    DispatchTable table(pr.program.funcs.size());
    std::vector<uint8_t> allowlist = build_fn_allowlist(slots, int(pr.program.funcs.size()));
    CodeGenCtx ctx;
    ctx.dispatch_base=int64_t(table.base()); ctx.natives=&natives; ctx.script_slots=&slots; ctx.structs=&layouts;
    ctx.use_context_reg=true; ctx.emit_depth_checks=true; ctx.max_call_depth=64;
    ctx.fn_allowlist_base=int64_t(allowlist.data()); ctx.fn_slot_count=int(pr.program.funcs.size());
    ctx.trap_stub=(void*)&kt6_trap; ctx.trap_ctx=nullptr;
    KeyedDispatchCodegen kd{}; kd.runtime_key=RuntimeKeyLocation::R15; kd.module_record=(ModuleDispatchRecord*)0x1234;
    ctx.keyed_dispatch=&kd; ctx.enable_ir_backend=true;
    for(auto&fn:pr.program.funcs){
        if (fn.name != "main") continue;
        auto cf=compile_func(fn,ctx); finalize(cf);
        printf("thin main bytes (%zu):\n", cf.bytes.size());
        int ud2_count = 0;
        for (size_t i = 0; i < cf.bytes.size(); ++i) {
            printf("%02x ", cf.bytes[i]); if ((i+1)%16==0) printf("\n");
            if (i+1 < cf.bytes.size() && cf.bytes[i]==0x0F && cf.bytes[i+1]==0x0B) { ud2_count++; printf("<<UD2 at %zu>> ", i); }
        }
        printf("\nud2 count: %d\n", ud2_count);
    }
    return 0;
}
