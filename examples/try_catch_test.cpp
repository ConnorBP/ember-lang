// try_catch_test - Tier 4 in-language exceptions (try/catch/throw).
//
// Proves the full try/catch/throw mechanism works through the JIT:
//   A. basic throw + catch (thrown value is bound to the catch variable)
//   B. throw in a nested call (unwinds through frames via longjmp)
//   C. throw a computed value + verify it's caught correctly
//   D. try/catch with a return in the try block (catch does NOT run)
//   E. try/catch with a return in the catch block
//   F. throw in the catch block (re-raise to an enclosing catch)
//   G. nested try/catch (independent handler scoping)
//   H. throw with no try/catch (unwinds to host = UnhandledThrow trap)
//
// Uses the B1 context-register model (use_context_reg=true) — the catch stack
// lives in context_t, accessed via r14. Mirrors function_refs_test's harness
// (resolve_imports -> tokenize -> parse -> slot-assign -> register six
// extensions -> sema -> compile -> call under the safe-trap surface).
#include "import.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "context.hpp"
#include "engine.hpp"
#include "globals.hpp"
#include "dispatch_table.hpp"
#include "binding_builder.hpp"
#include "jit_memory.hpp"

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace ember;

static int g_fail = 0;
static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

extern "C" void tc_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "";
        if (ctx->has_checkpoint) __builtin_longjmp(ctx->checkpoint, 1);
    }
    std::abort();
}

struct TCModule {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string,int> slots;
    void* main_entry = nullptr;
    std::unordered_map<std::string, NativeSig> natives;
    // The baked-ptr trap_ctx is the ADDRESS of this context, baked at compile.
    // The run MUST use this same context (same address) or the longjmp lands
    // wrong. So the module owns it + run reuses it.
    ember::context_t ctx{};
};

// Compile `main()->i64` (plus any helper fns). Returns false on error.
static bool compile_tc(const std::string& src, TCModule& m) {
    std::unordered_set<std::string> seen; std::string resolved;
    try { resolved = resolve_imports(src, "./", seen); } catch (...) { return false; }
    auto lr = tokenize(resolved, "<t>"); if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return false; }
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return false; }

    int si=0; for(auto&fn:pr.program.funcs){m.slots[fn.name]=si++;fn.slot=m.slots[fn.name];}
    ember::OpOverloadTable ov;
    ext_vec::register_natives(m.natives);ext_quat::register_natives(m.natives);
    ext_mat::register_natives(m.natives);ext_string::register_natives(m.natives);
    ext_array::register_natives(m.natives);ext_math::register_natives(m.natives);
    ext_vec::register_overloads(ov);ext_quat::register_overloads(ov);
    ext_mat::register_overloads(ov);ext_string::register_overloads(ov);
    auto layouts=build_struct_layouts(pr.program); pr.program.string_xor_key=0;
    auto sr=sema(pr.program,m.natives,m.slots,0,&ov,&layouts);
    if(!sr.ok){ std::printf("FAIL: sema (%zu errors):\n",sr.errors.size());
        for(auto&e:sr.errors) std::printf("  line %u: %s\n",e.line,e.msg.c_str());
        return false;
    }

    ember::GlobalsBlock gb; std::vector<uint8_t> gbs(pr.program.globals.size()*8, 0);
    gb.base=int64_t(gbs.data());
    { uint32_t gi=0; for (auto& g : pr.program.globals) { gb.index[g.name]=gi++; gb.types[g.name]=g.ty.get(); } }
    eval_global_initializers(pr.program, GlobalInitCtx{gbs, gb.index, gb.types});
    ember::g_globals_for_codegen = &gb;
    m.table = DispatchTable(pr.program.funcs.size());
    ember::CodeGenCtx ctx;
    ctx.globals_base=gb.base; ctx.dispatch_base=int64_t(m.table.base());
    ctx.natives=&m.natives; ctx.script_slots=&m.slots; ctx.structs=&layouts;
    // B1 context-register model: try/catch requires use_context_reg=true
    // (the catch stack lives in context_t, accessed via r14).
    m.ctx.budget_remaining=2'000'000'000LL; m.ctx.max_call_depth=512; m.ctx.call_depth=0;
    ctx.trap_stub=(void*)&tc_trap; ctx.trap_ctx=&m.ctx;
    ctx.use_context_reg = true;
    ctx.emit_budget_checks = true;
    ctx.emit_depth_checks = true;
    ctx.max_call_depth = 512;
    for(auto&fn:pr.program.funcs){auto cf=compile_func(fn,ctx); finalize(cf); m.table.set(fn.slot,cf.entry); m.fns.push_back(std::move(cf));}
    auto sit=m.slots.find("main");
    if(sit==m.slots.end()) return false;
    m.main_entry = m.table.get(sit->second);
    return m.main_entry != nullptr;
}

// Run main under the module's own context (the baked trap_ctx); returns the
// i64 result or *trapped=true (with m.ctx.last_trap set).
static int64_t run_main(TCModule& m, bool* trapped) {
    *trapped = false;
    m.ctx.call_depth = 0;
    m.ctx.catch_depth = 0;
    m.ctx.thrown_value = 0;
    m.ctx.has_checkpoint=true;
    if (__builtin_setjmp(m.ctx.checkpoint)) { *trapped=true; m.ctx.has_checkpoint=false;
        return int64_t(m.ctx.last_trap); }
    // Use the B1 thunk (installs r14 = context before the JIT'd entry).
    int64_t r = ember::ember_call_void(m.main_entry, &m.ctx);
    m.ctx.has_checkpoint=false;
    return r;
}

static void cleanup(TCModule& m) {
    for(auto&fn:m.fns) if(fn.exec) free_executable(fn.exec);
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    std::printf("=== Tier 4: in-language exceptions (try/catch/throw) ===\n");

    // ---- A. basic throw + catch ----
    {
        TCModule m;
        bool ok = compile_tc(
            "fn main() -> i64 {\n"
            "    try {\n"
            "        throw 42;\n"
            "    } catch (e) {\n"
            "        return e;\n"
            "    }\n"
            "}\n", m);
        check(ok, "A: compile basic try/catch/throw");
        if (ok) { bool t=false; int64_t r=run_main(m,&t); check(!t && r==42, "A: throw 42 -> catch e -> return e == 42"); }
        cleanup(m);
    }

    // ---- B. throw in a nested call (unwinds through frames) ----
    {
        TCModule m;
        bool ok = compile_tc(
            "fn deep_throw() -> i64 {\n"
            "    throw 99;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    try {\n"
            "        deep_throw();\n"
            "    } catch (e) {\n"
            "        return e;\n"
            "    }\n"
            "    return 0;\n"
            "}\n", m);
        check(ok, "B: compile throw-in-nested-call");
        if (ok) { bool t=false; int64_t r=run_main(m,&t); check(!t && r==99, "B: throw 99 in deep_throw -> caught by main's catch == 99"); }
        cleanup(m);
    }

    // ---- C. throw a computed value + verify it's caught ----
    {
        TCModule m;
        bool ok = compile_tc(
            "fn compute_and_throw(v: i64) -> i64 {\n"
            "    throw v + 1;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    try {\n"
            "        compute_and_throw(6);\n"
            "    } catch (caught) {\n"
            "        return caught;\n"
            "    }\n"
            "    return 0;\n"
            "}\n", m);
        check(ok, "C: compile throw-computed-value");
        if (ok) { bool t=false; int64_t r=run_main(m,&t); check(!t && r==7, "C: throw (6+1) -> catch -> return 7"); }
        cleanup(m);
    }

    // ---- D. return in the try block (catch does NOT run) ----
    {
        TCModule m;
        bool ok = compile_tc(
            "fn main() -> i64 {\n"
            "    try {\n"
            "        return 100;\n"
            "    } catch (e) {\n"
            "        return 999;\n"
            "    }\n"
            "}\n", m);
        check(ok, "D: compile return-in-try");
        if (ok) { bool t=false; int64_t r=run_main(m,&t); check(!t && r==100, "D: return 100 in try -> catch skipped -> 100"); }
        cleanup(m);
    }

    // ---- E. return in the catch block ----
    {
        TCModule m;
        bool ok = compile_tc(
            "fn main() -> i64 {\n"
            "    try {\n"
            "        throw 1;\n"
            "    } catch (e) {\n"
            "        return 200;\n"
            "    }\n"
            "}\n", m);
        check(ok, "E: compile return-in-catch");
        if (ok) { bool t=false; int64_t r=run_main(m,&t); check(!t && r==200, "E: throw -> catch -> return 200"); }
        cleanup(m);
    }

    // ---- F. throw in the catch block (re-raise to outer catch) ----
    {
        TCModule m;
        bool ok = compile_tc(
            "fn main() -> i64 {\n"
            "    try {\n"
            "        try {\n"
            "            throw 7;\n"
            "        } catch (inner) {\n"
            "            throw 55;\n"
            "        }\n"
            "    } catch (outer) {\n"
            "        return outer;\n"
            "    }\n"
            "}\n", m);
        check(ok, "F: compile throw-in-catch (re-raise)");
        if (ok) { bool t=false; int64_t r=run_main(m,&t); check(!t && r==55, "F: inner throw 7 -> inner catch -> throw 55 -> outer catch -> 55"); }
        cleanup(m);
    }

    // ---- G. nested try/catch (independent handler scoping) ----
    {
        TCModule m;
        bool ok = compile_tc(
            "fn main() -> i64 {\n"
            "    let mut result: i64 = 0;\n"
            "    try {\n"
            "        try {\n"
            "            throw 11;\n"
            "        } catch (inner) {\n"
            "            result = inner;\n"
            "        }\n"
            "        throw 33;\n"
            "    } catch (outer) {\n"
            "        result = result + outer;\n"
            "    }\n"
            "    return result;\n"
            "}\n", m);
        check(ok, "G: compile nested try/catch");
        if (ok) { bool t=false; int64_t r=run_main(m,&t); check(!t && r==44, "G: inner catch 11 + outer catch 33 -> 11+33=44"); }
        cleanup(m);
    }

    // ---- H. throw with no try/catch (unhandled -> trap) ----
    {
        TCModule m;
        bool ok = compile_tc(
            "fn throw_uncaught() -> i64 {\n"
            "    throw 123;\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    return throw_uncaught();\n"
            "}\n", m);
        check(ok, "H: compile throw-no-try (sema accepts, runtime traps)");
        if (ok) {
            bool t=false; run_main(m,&t);
            check(t, "H: uncaught throw traps (recoverable, not a crash)");
            check(m.ctx.last_trap == TrapReason::UnhandledThrow,
                  "H: trap reason is UnhandledThrow");
        }
        cleanup(m);
    }

    // ---- I. deep nesting: throw unwinds through 3 frames ----
    {
        TCModule m;
        bool ok = compile_tc(
            "fn level3() -> i64 {\n"
            "    throw 777;\n"
            "}\n"
            "fn level2() -> i64 {\n"
            "    return level3();\n"
            "}\n"
            "fn level1() -> i64 {\n"
            "    return level2();\n"
            "}\n"
            "fn main() -> i64 {\n"
            "    try {\n"
            "        level1();\n"
            "    } catch (e) {\n"
            "        return e;\n"
            "    }\n"
            "    return 0;\n"
            "}\n", m);
        check(ok, "I: compile 3-frame-deep throw");
        if (ok) { bool t=false; int64_t r=run_main(m,&t); check(!t && r==777, "I: throw 777 in level3 -> unwinds through level2, level1 -> caught by main == 777"); }
        cleanup(m);
    }

    // ---- J. throw inside a loop (catch is outside the loop) ----
    {
        TCModule m;
        bool ok = compile_tc(
            "fn main() -> i64 {\n"
            "    try {\n"
            "        let mut i: i64 = 0;\n"
            "        while (i < 10) {\n"
            "            i = i + 1;\n"
            "            if (i == 5) { throw i; }\n"
            "        }\n"
            "        return 0;\n"
            "    } catch (e) {\n"
            "        return e;\n"
            "    }\n"
            "}\n", m);
        check(ok, "J: compile throw-inside-loop");
        if (ok) { bool t=false; int64_t r=run_main(m,&t); check(!t && r==5, "J: throw at i==5 inside while -> caught by enclosing try == 5"); }
        cleanup(m);
    }

    if (g_fail) {
        std::printf("\n*** SOME TESTS FAILED ***\n");
        return 1;
    }
    std::printf("\n=== all try/catch/throw tests passed ===\n");
    return 0;
}
