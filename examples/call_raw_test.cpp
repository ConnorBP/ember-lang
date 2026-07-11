// call_raw_test - runtime coverage for the call_raw native
// (extensions/call_raw, the self-hosting Stage 4 gap from
// docs/planning/plan_SELF_HOSTING.md).
//
// call_raw(fn_ptr: i64, arg: i64) -> i64 casts fn_ptr to
// int64_t(*)(int64_t) and calls it with arg. This test exercises it through
// the FULL lex->parse->sema->codegen->JIT->call path (the same shape as
// ext_runtime_test) AND directly at the C++ level, to prove:
//
//   [1] The native's C++ function pointer works: hand it a compiled ember
//       fn's entry + an arg, get the right result back (direct C++ call).
//   [2] The ember binding works end-to-end: compile an ember fn that CALLS
//       call_raw through sema resolution + codegen dispatch, JIT-call it
//       with (ptr, arg), and assert the result. This is the load-bearing
//       case — it's what the self-hosted codegen will do (produce x64 bytes,
//       hand the entry to call_raw, get the result).
//   [3] PERM_FFI gating: without the FFI permission bit, sema rejects a
//       call_raw call site at compile time (zero runtime cost). This is the
//       security posture the native ships with — a module not granted FFI
//       cannot call raw function pointers.
//
// The target fn compiled via the C++ codegen is `fn f(x: i64) -> i64
// { return x + 1; }`. Its JIT entry pointer is the `fn_ptr` we pass to
// call_raw — exactly the flow the task describes ("compile a simple fn via
// the C++ codegen, get its entry pointer, call it via call_raw, verify the
// result").
//
// Links only ember + ember_frontend + ember_ext_call_raw (no prism) — proves
// the extension is reusable outside prism, same as ext_runtime_test for math.

#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/jit_memory.hpp"
#include "../src/binding_builder.hpp"   // PERM_FFI
#include "../extensions/call_raw/ext_call_raw.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

int main() {
    using namespace ember;
    int failures = 0;
    auto passfail = [&](bool ok) { return ok ? "PASS" : "FAIL"; };

    // ---- helpers: compile a single-fn source to an executable entry ----
    // Mirrors ext_runtime_test's pipeline (lex/parse/slot/sema/codegen/finalize)
    // for a source with exactly one i64->i64 fn, returning its JIT entry.
    auto compile_one = [](const std::string& src,
                          uint32_t perms,
                          std::vector<CompiledFn>& fns_out,
                          std::string& err) -> void* {
        auto lr = tokenize(src, "<call_raw_test>");
        if (!lr.ok) { err = "lex: " + lr.error; return nullptr; }
        auto pr = parse(std::move(lr.toks));
        if (!pr.ok) { err = "parse: " + pr.error; return nullptr; }
        if (pr.program.funcs.size() != 1) { err = "expected 1 fn, got " + std::to_string(pr.program.funcs.size()); return nullptr; }

        std::unordered_map<std::string, int> slots;
        int si = 0;
        for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = slots[fn.name]; }

        std::unordered_map<std::string, NativeSig> natives;
        ember::ext_call_raw::register_natives(natives);

        auto struct_layouts = build_struct_layouts(pr.program);
        pr.program.string_xor_key = 0;
        auto sr = sema(pr.program, natives, slots, perms, nullptr, &struct_layouts);
        if (!sr.ok) {
            err = "sema (" + std::to_string(sr.errors.size()) + "): ";
            for (auto& e : sr.errors) err += "line " + std::to_string(e.line) + ": " + e.msg + "; ";
            return nullptr;
        }

        GlobalsBlock gb;
        std::vector<uint8_t> gb_store(0);
        gb.base = int64_t(gb_store.data());
        g_globals_for_codegen = &gb;

        DispatchTable table(pr.program.funcs.size());
        CodeGenCtx ctx;
        ctx.globals_base = gb.base;
        ctx.dispatch_base = int64_t(table.base());
        ctx.natives = &natives;
        ctx.script_slots = &slots;
        ctx.structs = &struct_layouts;

        for (auto& fn : pr.program.funcs) {
            CompiledFn cf = compile_func(fn, ctx);
            if (!finalize(cf)) { err = "alloc_executable for " + fn.name; for (auto& d : fns_out) if (d.exec) free_executable(d.exec); return nullptr; }
            table.set(fn.slot, cf.entry);
            fns_out.push_back(std::move(cf));
        }
        g_globals_for_codegen = nullptr;
        return fns_out.front().entry;
    };

    // =====================================================================
    // [1] C++ direct: the native's function pointer calls a compiled ember fn.
    // =====================================================================
    {
        std::vector<CompiledFn> fns;
        std::string err;
        // target: fn f(x: i64) -> i64 { return x + 1; }  (no FFI needed; no natives)
        void* f_entry = compile_one("fn f(x: i64) -> i64 { return x + 1; }\n",
                                    0, fns, err);
        if (!f_entry) {
            std::printf("[1] FAIL: compile target: %s\n", err.c_str());
            failures++;
        } else {
            // Call the native's underlying C++ function directly. We grab it
            // from the registered native table (the same fn_ptr sema stamps
            // onto the CallExpr + codegen bakes into the JIT). This proves the
            // cast+call works with a real compiled-fn entry pointer.
            std::unordered_map<std::string, NativeSig> natives;
            ember::ext_call_raw::register_natives(natives);
            auto it = natives.find("call_raw");
            if (it == natives.end()) {
                std::printf("[1] FAIL: call_raw not registered\n");
                failures++;
            } else {
                using NativeFn = int64_t(*)(int64_t, int64_t);
                NativeFn raw = reinterpret_cast<NativeFn>(it->second.fn_ptr);
                int64_t r41 = raw(reinterpret_cast<int64_t>(f_entry), 41);
                int64_t r0  = raw(reinterpret_cast<int64_t>(f_entry), 0);
                int64_t rn  = raw(reinterpret_cast<int64_t>(f_entry), -5);
                bool ok41 = (r41 == 42);
                bool ok0  = (r0  == 1);
                bool okn  = (rn  == -4);
                std::printf("[1] call_raw(f,41) == 42 : %s (got %lld)\n", passfail(ok41), (long long)r41);
                std::printf("[1] call_raw(f,0)  == 1  : %s (got %lld)\n", passfail(ok0),  (long long)r0);
                std::printf("[1] call_raw(f,-5) == -4 : %s (got %lld)\n", passfail(okn),  (long long)rn);
                if (!ok41) failures++;
                if (!ok0)  failures++;
                if (!okn)  failures++;
            }
            for (auto& cf : fns) if (cf.exec) free_executable(cf.exec);
        }
    }

    // =====================================================================
    // [2] ember end-to-end: an ember fn calls call_raw through the binding.
    //     fn run(ptr: i64, arg: i64) -> i64 { return call_raw(ptr, arg); }
    //     We hand it f's entry pointer (compiled separately) + an arg, and
    //     assert the result flows back through the JIT'd ember caller.
    // =====================================================================
    {
        // First compile the target f (its entry pointer is the arg to run).
        std::vector<CompiledFn> f_fns;
        std::string f_err;
        void* f_entry = compile_one("fn f(x: i64) -> i64 { return x + 1; }\n",
                                    0, f_fns, f_err);
        if (!f_entry) {
            std::printf("[2] FAIL: compile target f: %s\n", f_err.c_str());
            failures++;
        } else {
            // Compile the ember caller with PERM_FFI (call_raw is gated).
            std::vector<CompiledFn> run_fns;
            std::string run_err;
            void* run_entry = compile_one(
                "fn run(ptr: i64, arg: i64) -> i64 { return call_raw(ptr, arg); }\n",
                PERM_FFI, run_fns, run_err);
            if (!run_entry) {
                std::printf("[2] FAIL: compile caller run: %s\n", run_err.c_str());
                failures++;
            } else {
                // Win64 i64(i64,i64): args in rcx, rdx; return in rax.
                using Run = int64_t(*)(int64_t, int64_t);
                auto run = reinterpret_cast<Run>(run_entry);
                int64_t ptr = reinterpret_cast<int64_t>(f_entry);
                int64_t r = run(ptr, 41);
                bool ok = (r == 42);
                std::printf("[2] run(f_ptr,41) == 42 via call_raw : %s (got %lld)\n",
                            passfail(ok), (long long)r);
                if (!ok) failures++;
                for (auto& cf : run_fns) if (cf.exec) free_executable(cf.exec);
            }
            for (auto& cf : f_fns) if (cf.exec) free_executable(cf.exec);
        }
    }

    // =====================================================================
    // [3] PERM_FFI gating: without the FFI bit, sema REJECTS call_raw.
    //     (zero runtime cost — the rejection is at compile time, before
    //      codegen; this is the security posture the native ships with.)
    // =====================================================================
    {
        std::vector<CompiledFn> fns;
        std::string err;
        // Same source as [2] but perms=0 (no FFI) -> sema must fail.
        void* entry = compile_one(
            "fn run(ptr: i64, arg: i64) -> i64 { return call_raw(ptr, arg); }\n",
            0, fns, err);
        bool rejected = (entry == nullptr) && err.rfind("sema", 0) == 0;
        std::printf("[3] sema rejects call_raw without PERM_FFI : %s%s\n",
                    passfail(rejected),
                    rejected ? "" : (" (err=" + err + ")").c_str());
        if (!rejected) failures++;
        for (auto& cf : fns) if (cf.exec) free_executable(cf.exec);
    }

    ember::ext_call_raw::reset();  // stateless no-op, for symmetry

    if (failures == 0) { std::printf("\ncall_raw: PASS\n"); return 0; }
    std::printf("\ncall_raw: FAIL (%d)\n", failures);
    return 1;
}
