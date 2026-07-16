// Comprehensive branch coverage for the legacy AST -> x64 tree walker.
//
// This is deliberately a C++ integration test rather than another .ember file:
// every case goes through tokenize -> parse -> sema -> compile_func_checked ->
// finalize -> dispatch -> execution, and the host reads the full i64 result.
// It concentrates on codegen.cpp's uncommon aggregate, closure, exception,
// coroutine, cleanup, conversion, and defensive-failure paths.

#include "engine.hpp"
#include "dispatch_table.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "globals.hpp"
#include "context.hpp"
#include "ember_pass.hpp"
#include "jit_memory.hpp"

#include "ext_array.hpp"
#include "ext_string.hpp"
#include "ext_coroutine.hpp"
#include "ext_gc.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

static int failures = 0;
static int checks = 0;
#define CHECK(expr, msg) do { ++checks; bool ok_ = !!(expr); \
    std::printf("  [%s] %s\n", ok_ ? "PASS" : "FAIL", msg); \
    if (!ok_) ++failures; } while (0)

extern "C" void coverage_trap(context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) EMBER_LONGJMP(ctx->checkpoint, 1);
    }
    std::abort();
}

static uint32_t value_bytes(const Type* t, const StructLayoutTable& layouts) {
    if (!t) return 8;
    if (t->is_managed_ptr) return 8;
    if (t->is_slice || t->is_lambda) return 16;
    if (t->array_len) return t->array_len * value_bytes(t->elem.get(), layouts);
    if (!t->struct_name.empty()) {
        auto it = layouts.find(t->struct_name);
        if (it != layouts.end()) return uint32_t(it->second.size);
    }
    switch (t->prim) {
    case Prim::Bool: case Prim::I8: case Prim::U8: return 1;
    case Prim::I16: case Prim::U16: return 2;
    case Prim::I32: case Prim::U32: case Prim::F32: return 4;
    default: return 8;
    }
}

struct Module {
    Program program;
    StructLayoutTable layouts;
    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
    std::unordered_map<std::string, int> slots;
    GlobalsBlock globals;
    std::vector<uint8_t> global_store;
    std::unordered_map<std::string, uint32_t> backing_offsets;
    std::unique_ptr<DispatchTable> table;
    std::vector<CompiledFn> functions;
    std::vector<uint8_t> allowlist;
    context_t context{};
    bool use_gc = false;
    bool coroutine_runtime = false;

    ~Module() {
        if (coroutine_runtime) ext_coroutine::coroutine_reset();
        for (auto& fn : functions) if (fn.exec) free_executable(fn.exec);
        if (use_gc) {
            ext_gc::gc_detach_context(&context);
            ext_gc::gc_reset();
        }
    }

    bool run(int64_t& result, TrapReason* trap = nullptr) {
        auto it = slots.find("main");
        if (!table || it == slots.end() || !table->get(it->second)) return false;
        context.reset_for_call();
        context.budget_remaining = 1'000'000'000LL;
        context.max_call_depth = 512;
        context.has_checkpoint = true;
        if (use_gc) ext_gc::gc_attach_context(&context, nullptr);
        if (EMBER_SETJMP(context.checkpoint)) {
            context.has_checkpoint = false;
            if (trap) *trap = context.last_trap;
            result = int64_t(context.last_trap);
            return false;
        }
        result = ember_call_void(table->get(it->second), &context);
        context.has_checkpoint = false;
        if (trap) *trap = TrapReason::None;
        return true;
    }
};

struct BuildOptions {
    bool encrypt_strings = false;
    bool use_gc = false;
    bool init_coroutines = false;
    bool register_array = true;
    bool register_string = true;
    bool register_coroutine = true;
    bool register_gc = true;
};

static bool prepare_module(const std::string& source, Module& m,
                           const BuildOptions& opt = {}) {
    auto lr = tokenize(source, "<codegen_coverage>");
    if (!lr.ok) {
        std::printf("  lex failed: %s\n", lr.error.c_str());
        return false;
    }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) {
        std::printf("  parse failed: %s\n", pr.error.c_str());
        return false;
    }
    m.program = std::move(pr.program);
    int slot = 0;
    for (auto& fn : m.program.funcs) {
        m.slots[fn.name] = slot;
        fn.slot = slot++;
    }
    if (opt.register_array) ext_array::register_natives(m.natives);
    if (opt.register_string) {
        ext_string::register_natives(m.natives);
        ext_string::register_overloads(m.overloads);
    }
    if (opt.register_coroutine) ext_coroutine::register_natives(m.natives);
    if (opt.register_gc) ext_gc::register_natives(m.natives);

    m.layouts = build_struct_layouts(m.program);
    m.program.string_xor_key = opt.encrypt_strings ? 0xA5 : 0;
    auto sr = sema(m.program, m.natives, m.slots, 0, &m.overloads, &m.layouts);
    if (!sr.ok) {
        std::printf("  sema failed (%zu):\n", sr.errors.size());
        for (const auto& e : sr.errors)
            std::printf("    line %u: %s\n", e.line, e.msg.c_str());
        return false;
    }

    uint32_t cursor = 0;
    auto align8 = [](uint32_t n) { return (n + 7u) & ~7u; };
    uint32_t index = 0;
    for (auto& g : m.program.globals) {
        cursor = align8(cursor);
        m.globals.index[g.name] = index++;
        m.globals.types[g.name] = g.ty.get();
        m.globals.offsets[g.name] = cursor;
        m.globals.sizes[g.name] = value_bytes(g.ty.get(), m.layouts);
        cursor += m.globals.sizes[g.name];
    }
    for (auto& g : m.program.globals) {
        if (!g.ty || !g.ty->is_slice || !g.init) continue;
        auto* literal = dynamic_cast<ArrayLit*>(g.init.get());
        if (!literal) continue;
        cursor = align8(cursor);
        m.backing_offsets[g.name] = cursor;
        cursor += uint32_t(literal->elements.size()) * value_bytes(g.ty->elem.get(), m.layouts);
    }
    m.global_store.assign(cursor, 0);
    m.globals.base = int64_t(m.global_store.data());
    GlobalInitCtx init{m.global_store, m.globals.index, m.globals.types};
    init.offsets = &m.globals.offsets;
    init.sizes = &m.globals.sizes;
    init.backing_offsets = &m.backing_offsets;
    init.structs = &m.layouts;
    eval_global_initializers(m.program, init);
    m.use_gc = opt.use_gc;
    return true;
}

static bool finish_module(Module& m, const BuildOptions& opt = {}) {
    if (opt.use_gc) {
        ext_gc::gc_init();
        ext_gc::gc_reset();
    }
    m.table = std::make_unique<DispatchTable>(m.program.funcs.size());
    m.allowlist = build_fn_allowlist(m.slots, int(m.slots.size()));
    g_globals_for_codegen = &m.globals;

    CodeGenCtx cg;
    cg.globals_base = m.globals.base;
    cg.globals_index = &m.globals.index;
    cg.globals_offsets = &m.globals.offsets;
    cg.globals_types = &m.globals.types;
    cg.dispatch_base = int64_t(m.table->base());
    cg.natives = &m.natives;
    cg.script_slots = &m.slots;
    cg.structs = &m.layouts;
    cg.fn_allowlist_base = int64_t(m.allowlist.data());
    cg.fn_slot_count = int64_t(m.slots.size());
    cg.trap_stub = reinterpret_cast<void*>(&coverage_trap);
    cg.use_context_reg = true;
    cg.emit_budget_checks = true;
    cg.emit_depth_checks = true;
    cg.max_call_depth = 512;
    cg.use_gc_env = opt.use_gc;

    for (auto& fn : m.program.funcs) {
        CompileResult cr = compile_func_checked(fn, cg);
        if (!cr.ok() || cr.backend != CompileBackend::TreeWalker || cr.compiled.bytes.empty()) {
            std::printf("  codegen failed for %s: %s\n", fn.name.c_str(), cr.reason.c_str());
            return false;
        }
        if (!finalize(cr.compiled)) {
            std::printf("  finalize failed for %s\n", fn.name.c_str());
            return false;
        }
        m.table->set(fn.slot, cr.compiled.entry);
        m.functions.push_back(std::move(cr.compiled));
    }
    if (opt.init_coroutines) {
        m.coroutine_runtime = ext_coroutine::coroutine_init(
            &m.context, m.table->base(), int64_t(m.slots.size()));
        if (!m.coroutine_runtime) return false;
    }
    return true;
}

static std::unique_ptr<Module> compile_source(const std::string& source,
                                               const BuildOptions& opt = {}) {
    ext_array::reset();
    ext_string::reset();
    ext_coroutine::coroutine_reset();
    auto m = std::make_unique<Module>();
    if (!prepare_module(source, *m, opt) || !finish_module(*m, opt)) return nullptr;
    return m;
}

static void expect_value(const char* name, const std::string& source, int64_t expected,
                         const BuildOptions& opt = {}) {
    std::printf("\n-- %s --\n", name);
    auto m = compile_source(source, opt);
    CHECK(m != nullptr, "full pipeline compiles");
    if (!m) return;
    int64_t got = 0;
    bool ran = m->run(got);
    CHECK(ran, "JIT execution completes without a trap");
    if (ran) {
        char msg[160];
        std::snprintf(msg, sizeof(msg), "host observes expected i64 result (%lld)",
                      static_cast<long long>(expected));
        CHECK(got == expected, msg);
        if (got != expected) std::printf("    got %lld\n", static_cast<long long>(got));
    }
}

static void expect_trap(const char* name, std::unique_ptr<Module> m, TrapReason expected) {
    std::printf("\n-- %s --\n", name);
    CHECK(m != nullptr, "defensive program compiles to a loud trap path");
    if (!m) return;
    int64_t ignored = 0;
    TrapReason got = TrapReason::None;
    bool ran = m->run(ignored, &got);
    CHECK(!ran, "execution is rejected rather than silently miscompiled");
    CHECK(got == expected, "trap reason matches the defensive codegen path");
}

static void expect_sema_reject(const char* name, const std::string& source) {
    std::printf("\n-- %s --\n", name);
    Module m;
    CHECK(!prepare_module(source, m), "invalid source is rejected before code emission");
}

struct UnknownExpr final : Expr {};
struct UnknownStmt final : Stmt {};

struct ThrowPass : EmberPassInfoMixin<ThrowPass> {
    static constexpr const char* pass_name = "coverage-throw";
    EmberPreserved run(ThinFunction&, EmberAnalysisManager&) {
        throw PassError(pass_name, "intentional checked-codegen failure");
    }
};
struct StdThrowPass : EmberPassInfoMixin<StdThrowPass> {
    static constexpr const char* pass_name = "coverage-std-throw";
    EmberPreserved run(ThinFunction&, EmberAnalysisManager&) {
        throw std::runtime_error("intentional unexpected pass exception");
    }
};

static void checked_compile_error_paths(bool unexpected) {
    Module m;
    CHECK(prepare_module("fn main() -> i64 { return 7; }", m),
          "checked-error fixture reaches sema");
    if (m.program.funcs.empty()) return;
    EmberPassManager pm;
    if (unexpected) pm.add_pass(StdThrowPass{}); else pm.add_pass(ThrowPass{});
    CodeGenCtx cg;
    cg.enable_ir_backend = true;
    cg.pass_manager = &pm;
    cg.natives = &m.natives;
    cg.script_slots = &m.slots;
    cg.structs = &m.layouts;
    CompileResult cr = compile_func_checked(m.program.funcs.front(), cg);
    CHECK(!cr.ok() && cr.compiled.bytes.empty(),
          "checked compile returns structured failure and no executable bytes");
    CHECK(!cr.pass_reports.empty(), "checked compile preserves a pass failure report");
    if (!cr.pass_reports.empty()) {
        CHECK(cr.pass_reports.front().stop_reason ==
              (unexpected ? PassStopReason::ExceptionError : PassStopReason::PassError),
              "checked report classifies the exception path");
    }
    const auto* emit = cr.stage(CompileStage::Emission);
    CHECK(emit && !emit->reached, "failure prevents the emission stage");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("=== codegen.cpp comprehensive tree-walker coverage ===");

    std::puts("\n================ scalar expressions and stores ================");
    expect_value("integer widths, signedness, bitwise, shifts, unary and logical short circuit",
        "fn side() -> bool { return false; }\n"
        "fn main() -> i64 { let a:i8=255 as i8; let b:u8=255 as u8; let c:i16=32768 as i16; "
        "let d:u32=4294967295 as u32; let x:i64=5; "
        "if ((false && side()) || !(true && !false)) { return -1; } "
        "return (a as i64) + (b as i64) + (c as i64) + ((d >> (31 as u32)) as i64) + "
        "((~x) & 15) + ((3 | 8) ^ 1) + (1 << 4); }",  -32477);

    expect_value("f32/f64 arithmetic, comparisons, unary negation and conversions",
        "fn main() -> i64 { let i:i64=-7; let a:f64=i as f64; let b:f32=a as f32; "
        "let c:f64=b as f64; let q:f64=(-c + 21.0) / 2.0; "
        "if (q >= 14.0 && q <= 14.0 && q != 13.0 && !(q < 14.0)) { return q as i64; } return 0; }", 14);

    expect_value("ternary values (both arms and nested float ternary)",
        "fn choose(b:bool)->i64 { return b ? 10 : 20; } "
        "fn main()->i64 { let f:f64 = true ? (false ? 1.0 : 2.5) : 9.0; "
        "return choose(true)+choose(false)+(f as i64); }", 32);

    expect_value("all integer compound assignments including bitwise and shift",
        "fn main()->i64 { let mut x:i64=10; x+=5; x-=3; x*=4; x/=6; x%=5; "
        "x|=8; x&=11; x^=3; x<<=2; x>>=1; return x; }", 16);

    expect_value("float compound assignment and integer pre/post increment/decrement",
        "fn main()->i64 { let mut f:f64=5.0; f+=2.0; f-=1.0; f*=3.0; f/=2.0; "
        "let mut i:i64=9; let a=i++; let b=++i; let c=i--; let d=--i; "
        "return (f as i64)*1000+a*100+b*10+c+d; }", 10030);

    expect_value("sizeof, offsetof, enum access and passing static_assert elision",
        "struct I { a:u8; b:i16; } struct O { x:u8; i:I; z:i32[2]; } "
        "enum E:i32 { V = 7 } static_assert(1+1==2, \"fold\"); "
        "fn main()->i64 { static_assert(2*3==6, \"body fold\"); "
        "return (sizeof(I) as i64)*100 + (offsetof(O,i) as i64)*10 + ((E::V as i32) as i64); }", 317);

    std::puts("\n================ aggregates, addressing and ABI ================");
    expect_value("nested struct literals, exact packed copies, by-value args and hidden returns",
        "struct Inner { a:u8; b:i16; } struct P { tag:u8; inr:Inner; tail:i64; } "
        "fn make(v:i64)->P { return P{tag:2,inr:Inner{a:3,b:4},tail:v}; } "
        "fn use(p:P, a:i64,b:i64,c:i64,d:i64)->i64 { return (p.tag as i64)+(p.inr.a as i64)+(p.inr.b as i64)+p.tail+a+b+c+d; } "
        "fn main()->i64 { let p:P=make(20); let mut q:P=P{tag:0,inr:Inner{a:0,b:0},tail:0}; "
        "q=make(30); return use(P{tag:1,inr:Inner{a:2,b:3},tail:10},1,2,3,4)+q.tail; }", 54);

    expect_value("fixed arrays and slices at 1/2/4/8-byte strides with view/index stores",
        "fn main()->i64 { let mut a:u8[3]=[1,2,3]; let mut b:i16[3]=[10,20,30]; "
        "let mut c:i32[2]=[100,200]; let mut d:i64[2]=[1000,2000]; "
        "let i:i64=1; a[i]=9; b[i]=40; c[i]=300; d[i]=4000; "
        "let s:i16[]=b[..]; s[2]=50; return (a[1] as i64)+(s[1] as i64)+(c[1] as i64)+d[1]+(s[2] as i64); }", 4399);

    expect_value("f32/f64 array loads and stores plus field stores",
        "struct F { a:f32; b:f64; } fn main()->i64 { let mut x:f32[2]=[1.5f,2.5f]; "
        "let mut y:f64[2]=[3.0,4.0]; let i:i64=1; x[i]=5.5f; y[i]=6.5; "
        "let mut f:F=F{a:1.0f,b:2.0}; f.a=x[i]; f.b=y[i]; return (f.a as i64)*10+(f.b as i64); }", 56);

    expect_value("slice literal as a call argument",
        "fn sum(s:i64[])->i64 { let mut r:i64=0; for (x in s) { r+=x; } return r; } "
        "fn main()->i64 { return sum([1,2,3]); }", 6);

    expect_value("slice literal as a return value",
        "fn mk()->i64[] { return [4,5,6]; } fn main()->i64 { let m:i64[]=mk(); return m[0]+m[1]+m[2]; }", 15);

    expect_value("for-each over signed narrow slices, break and continue",
        "fn main()->i64 { let a:i32[5]=[1,2,3,4,5]; let s:i32[]=a[..]; let mut r:i64=0; "
        "for (x in s) { if (x==2) { continue; } if (x==5) { break; } r += x as i64; } return r; }", 8);

    expect_value("for-each array-handle lowering selects u8, f32 and i64 natives",
        "fn main()->i64 { let a=array_new(1,2); array_set_u8(a,0,7); array_set_u8(a,1,8); "
        "let b=array_new(4,2); array_set_f32(b,0,1.5f); array_set_f32(b,1,2.5f); "
        "let c=array_new(8,2); array_set_i64(c,0,10); array_set_i64(c,1,20); "
        "let mut r:i64=0; for(x in a){r+=x as i64;} for(x in b){r+=x as i64;} for(x in c){r+=x;} return r; }", 48);

    std::puts("\n================ control flow and cleanups ================");
    expect_value("while/for/do-while, nested switch, break and continue targets",
        "fn main()->i64 { let mut r:i64=0; let mut i:i64=0; do { i++; if(i==2){continue;} r+=i; } while(i<3); "
        "for(let mut j:i64=0;j<4;j++){ switch(j){case 0:r+=10;break; case 1:r+=20;continue; default:r+=1;break;} if(j==3){break;} } "
        "while(false){r+=1000;} return r; }", 36);

    expect_value("switch matched/default/no-match paths",
        "fn sw(x:i64)->i64 { let mut r:i64=0; switch(x){case 1:r=10;break; case 2:r=20;break; default:r=30;break;} return r;} "
        "fn none(x:i64)->i64 { switch(x){case 1:return 1;} return 7;} fn main()->i64{return sw(1)+sw(9)+none(2);}", 47);

    expect_value("literal match wildcard and no-wildcard exits",
        "fn m(x:i64)->i64 { match(x){1=>{return 10;},2=>{return 20;},_=>{return 30;}} return 0;} "
        "fn n(x:i64)->i64 { match(x){1=>{return 1;}} return 7;} fn main()->i64{return m(2)+m(8)+n(9);}", 57);

    expect_value("struct destructuring, capture locals, literal fields, guards and wildcard",
        "struct P{x:i64;y:i64;} fn f(p:P)->i64 { match(p){P{x:0,y:0}=>{return 1;}, "
        "P{x,y} if x>0=>{return x+y;}, P{x,y} if y>0=>{return y;}, _=>{return 9;}} return 0;} "
        "fn main()->i64 {let a=P{x:0,y:0};let b=P{x:3,y:4};let c=P{x:0,y:5};return f(a)+f(b)+f(c);}", 19);

    expect_value("defer activation flags, lexical LIFO, return/break/continue cleanup",
        "global t:i64=0; fn mark(x:i64)->i64{t=t*10+x;return 0;} "
        "fn loop()->i64{let mut i:i64=0;while(i<3){i++;defer mark(i);if(i==1){continue;}if(i==2){break;}}return t;} "
        "fn ret()->i64{defer mark(8);defer mark(9);return 7;} "
        "fn main()->i64{let a=loop();t=0;let b=ret();return a*1000+t*10+b;}", 12987);

    std::puts("\n================ calls, strings, closures, exceptions, coroutines ================");
    expect_value("direct and first-class function calls with stack arguments",
        "fn add6(a:i64,b:i64,c:i64,d:i64,e:i64,f:i64)->i64{return a+b+c+d+e+f;} "
        "fn main()->i64{let h:fn(i64,i64,i64,i64,i64,i64)->i64=&add6;return add6(1,2,3,4,5,6)+h(6,5,4,3,2,1);}", 42);

    expect_value("encrypted short and long string literals plus implicit string conversion",
        std::string("fn main()->i64{let a:string=\"hello\";let b:string=\"") +
        std::string(300, 'x') +
        "\";return string_length(a)*100+string_length(b)+string_char_at(b,0)+string_char_at(b,127)+string_char_at(b,128)+string_char_at(b,299);}",
        1280, BuildOptions{true});

    expect_value("stack closure environments: no capture, by-value, by-ref write and nested transitive capture",
        "fn main()->i64{let mut x:i64=3;let y:i64=4;let z=fn()->i64{return 1;};"
        "let byv=fn[x,y]()->i64{return x+y;};let byr=fn[&x]()->i64{x+=5;return x;};"
        "let outer=fn[&x]()->i64{let inner=fn[&x]()->i64{return x;};return inner();};"
        "x=10;return z()+byv()*10+byr()*100+outer();}", 1586);

    BuildOptions gcopt; gcopt.use_gc = true;
    expect_value("GC closure environment allocation and managed new/delete lowering",
        "fn main()->i64{let v:i64=42;let f=fn[v]()->i64{return v;};let p=new i64;"
        "let before=gc_live();delete p;let after=gc_live();return f()+before*10+after;}",
        63, gcopt);

    expect_value("try/catch setjmp-longjmp, nested throw and catch variable",
        "fn boom(v:i64)->i64{throw v+1;} fn main()->i64{let mut r:i64=0;try{try{boom(40);}catch(e){throw e+1;}}catch(o){r=o;}return r;}", 42);

    BuildOptions coro; coro.init_coroutines = true;
    expect_value("coroutine yield lowering (value and resume/final return)",
        "fn gen(x:i64)->i64{yield x;yield x+1;return x+2;} fn main()->i64{let c=coroutine_start(&gen,10);return coroutine_next(c)+coroutine_next(c)+coroutine_next(c);}",
        33, coro);

    std::puts("\n================ compile-time and generated-code failures ================");
    expect_sema_reject("invalid cast never reaches codegen",
        "fn main()->i64{let x:u64=5 as u64;let f:f64=x as f64;return f as i64;}");
    expect_sema_reject("fixed-array constant out-of-range index",
        "fn main()->i64{let a:i64[2]=[1,2];return a[2];}");
    expect_sema_reject("unsupported non-local struct match subject",
        "struct P{x:i64;} fn mk()->P{return P{x:1};} fn main()->i64{match(mk()){P{x}=>{return x;}}return 0;}");

    {
        BuildOptions opt;
        auto m = std::make_unique<Module>();
        CHECK(prepare_module("fn main()->i64{return 1;}", *m, opt),
              "unknown-expression fixture reaches sema");
        if (!m->program.funcs.empty()) {
            auto* ret = dynamic_cast<ReturnStmt*>(m->program.funcs.front().body.stmts.front().get());
            auto bad = std::make_unique<UnknownExpr>();
            bad->ty = &type_i64();
            if (ret) ret->value = std::move(bad);
        }
        if (!finish_module(*m, opt)) m.reset();
        expect_trap("defensive unhandled expression node", std::move(m),
                    TrapReason::IllegalInstruction);
    }
    {
        BuildOptions opt;
        auto m = std::make_unique<Module>();
        CHECK(prepare_module("fn main()->i64{return 1;}", *m, opt),
              "unknown-statement fixture reaches sema");
        if (!m->program.funcs.empty())
            m->program.funcs.front().body.stmts.insert(
                m->program.funcs.front().body.stmts.begin(), std::make_unique<UnknownStmt>());
        if (!finish_module(*m, opt)) m.reset();
        expect_trap("defensive unhandled statement node", std::move(m),
                    TrapReason::IllegalInstruction);
    }
    {
        BuildOptions no_gc;
        no_gc.register_gc = false;
        expect_trap("missing GC extension for new",
            compile_source("fn main()->i64{let p=new i64;return 1;}", no_gc),
            TrapReason::IllegalInstruction);
    }
    {
        BuildOptions no_coro;
        no_coro.register_coroutine = false;
        expect_trap("missing coroutine extension for yield",
            compile_source("fn main()->i64{yield 1;return 2;}", no_coro),
            TrapReason::IllegalInstruction);
    }

    std::puts("\n-- checked PassError boundary --");
    checked_compile_error_paths(false);
    std::puts("\n-- checked unexpected-exception boundary --");
    checked_compile_error_paths(true);

    std::printf("\ncodegen_coverage_test: %s (%d checks)\n",
                failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
