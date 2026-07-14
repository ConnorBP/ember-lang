#include "src/engine.hpp"
#include "src/context.hpp"
#include "src/dispatch_table.hpp"
#include "src/codegen.hpp"
#include "src/lexer.hpp"
#include "src/parser.hpp"
#include "src/sema.hpp"
#include "src/globals.hpp"
#include "src/binding_builder.hpp"
#include "import.hpp"
#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using namespace ember;
extern "C" void test_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) { ctx->last_trap = static_cast<ember::TrapReason>(reason); ctx->last_error = detail?detail:""; if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1); }
    std::abort();
}
int main() {
    const char* src = "fn r() -> i64 { return r(); }\nfn main() -> i64 { return r(); }\n";
    std::unordered_set<std::string> seen; std::string resolved = resolve_imports(src, "./", seen);
    auto lr = tokenize(resolved, "<t>"); auto pr = parse(std::move(lr.toks));
    std::unordered_map<std::string,int> slots; int si=0;
    for(auto&fn:pr.program.funcs){slots[fn.name]=si++;fn.slot=slots[fn.name];}
    std::unordered_map<std::string,NativeSig> natives; OpOverloadTable ov;
    auto layouts = build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    sema(pr.program,natives,slots,0,&ov,&layouts);
    DispatchTable table(pr.program.funcs.size());
    CodeGenCtx ctx;
    ctx.dispatch_base=int64_t(table.base()); ctx.natives=&natives; ctx.script_slots=&slots; ctx.structs=&layouts;
    ctx.use_context_reg=true; ctx.emit_depth_checks=true; ctx.max_call_depth=64;
    ctx.trap_stub=(void*)&test_trap; ctx.trap_ctx=nullptr;
    ctx.enable_ir_backend=false;  // THIN IR
    std::vector<CompiledFn> fns;
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx);finalize(cf);table.set(fn.slot,cf.entry);fns.push_back(std::move(cf));}
    void* entry=table.get(slots["main"]);
    auto ctxp = std::make_unique<context_t>();
    ctxp->budget_remaining=1000000000; ctxp->max_call_depth=64; ctxp->has_checkpoint=true;
    printf("thin IR infinite recursion: calling main...\n"); fflush(stdout);
    if (__builtin_setjmp(ctxp->checkpoint)) {
        printf("TRAPPED: %s (depth=%d)\n", trap_reason_str(ctxp->last_trap), ctxp->call_depth);
        return 0;
    }
    using F0=int64_t(*)(); reinterpret_cast<F0>(entry)();
    printf("ERROR: should have trapped\n");
    return 1;
}
