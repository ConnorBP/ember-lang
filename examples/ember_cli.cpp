// ember_cli - standalone ember language runner (RESTRUCTURE_PLAN.md).
//
// A minimal, prism-decoupled CLI that exercises the full
// parse -> sema -> codegen -> finalize -> call pipeline against the
// REAL ember frontend + the full six-extension addon surface. This is
// the host shape proven by examples/em_roundtrip_test.cpp (the
// parse/sema/codegen/finalize/call sequence) plus examples/ext_registration_test.cpp
// (the six-extension natives + overloads registration), combined into one
// runnable program. No prism linkage: no prism_script_host, no VFS, no
// memory backend, no proc/shader api, no pak/assets. Just: read file,
// resolve imports, compile, run, exit code.
//
// Usage:
//   ember run <file.ember> [--fn <name>] [--dump]
//
//   run        compiles <file.ember> and calls the entry function.
//   --fn NAME  entry function to call (default: main).
//   --dump     after compile, print each fn's name, slot, byte size, and
//              reloc (AbsFixup) count, mirroring em_roundtrip's reloc capture.
//
// Return convention: if the entry returns i64, that value is the process
// exit code (the validation signal); if it returns void, exit 0. The runner
// never echoes the return value - scripts that want output call a native.
// There is no print_i64 in the standard extension set (vec/quat/mat/string/
// array/math), so YAGNI: keep it to exit-code-as-signal.
//
// Build: linked against ember + ember_frontend + all six ember_ext_* libs.
//        The native host backing (ext_*::register_natives populates
//        NativeSig::fn_ptr, which sema stamps onto CallExpr::native_fn and
//        codegen bakes as a direct `mov rax, imm64(fn); call rax`) requires
//        NO extra trampoline - registration is the entire binding. Verified
//        against src/sema.cpp (c->native_fn = nit->second.fn_ptr) and
//        src/codegen.cpp (e.mov_reg_imm64(rax, c->native_fn); call rax).

#include "../src/engine.hpp"      // CompiledFn, finalize, free_executable
#include "../src/dispatch_table.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"     // CodeGenCtx, compile_func, g_globals_for_codegen, GlobalsBlock
#include "../src/import.hpp"      // resolve_imports
#include "../src/jit_memory.hpp"  // free_executable

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Extension headers resolve via each ember_ext_* lib's PUBLIC include dir
// (extensions/<name>), which flows transitively to this target because we
// link all six extension libs below.

namespace fs = std::filesystem;

static std::string read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static void usage(FILE* out) {
    std::fprintf(out,
        "ember - standalone ember language runner\n"
        "usage: ember run <file.ember> [--fn <name>] [--dump]\n"
        "  run         compile and call the entry function\n"
        "  --fn NAME   entry function (default: main)\n"
        "  --dump      print each compiled fn: name, slot, byte size, reloc count\n");
}

int main(int argc, char** argv) {
    using namespace ember;

    // ---- arg parse ----
    std::string action;
    std::string file;
    std::string fn_name = "main";
    bool dump = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--fn") {
            if (++i >= argc) { std::fprintf(stderr, "ember: --fn needs a name\n"); return 2; }
            fn_name = argv[i];
        } else if (a == "--dump") {
            dump = true;
        } else if (a == "-h" || a == "--help") {
            usage(stdout); return 0;
        } else if (a.size() > 0 && a[0] == '-') {
            std::fprintf(stderr, "ember: unknown option '%s'\n", a.c_str()); return 2;
        } else if (action.empty()) {
            action = a;                 // first positional = action ("run")
        } else if (file.empty()) {
            file = a;                    // second positional = file path
        } else {
            std::fprintf(stderr, "ember: unexpected argument '%s'\n", a.c_str()); return 2;
        }
    }
    if (action != "run") { usage(stderr); return 2; }
    if (file.empty()) { std::fprintf(stderr, "ember: missing <file.ember>\n"); usage(stderr); return 2; }
    if (!fs::exists(file)) { std::fprintf(stderr, "ember: no such file: %s\n", file.c_str()); return 2; }

    // ---- read + resolve imports (textual `import "path";` inlining) ----
    std::string raw = read_file(file.c_str());
    if (raw.empty() && fs::file_size(file) == 0) {
        // empty file is valid (will fail at parse with a clearer message).
    // (raw.empty() && file_size>0 can't happen - read_file covers that.)
    } else if (raw.empty()) {
        std::fprintf(stderr, "ember: cannot read '%s'\n", file.c_str()); return 2;
    }
    std::string base_dir = fs::path(file).parent_path().string();
    std::unordered_set<std::string> seen;
    std::string src;
    try {
        std::string canon = fs::weakly_canonical(fs::path(file)).string();
        seen.insert(canon);
        src = resolve_imports(raw, base_dir, seen);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ember: import error: %s\n", e.what()); return 2;
    }

    // ---- lex ----
    auto lr = tokenize(src, file.c_str());
    if (!lr.ok) {
        std::fprintf(stderr, "ember: lex error (%u:%u): %s\n", lr.err_line, lr.err_col, lr.error.c_str());
        return 2;
    }

    // ---- parse ----
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) {
        std::fprintf(stderr, "ember: parse error: %s\n", pr.error.c_str());
        return 2;
    }
    if (pr.program.funcs.empty()) {
        std::fprintf(stderr, "ember: no functions in '%s'\n", file.c_str()); return 2;
    }

    // ---- slot assignment (mirror em_roundtrip_test + prism_script_host) ----
    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = slots[fn.name]; }

    // ---- register ALL six extensions: natives + overloads ----
    std::unordered_map<std::string, NativeSig> natives;
    ext_vec::register_natives(natives);
    ext_quat::register_natives(natives);
    ext_mat::register_natives(natives);
    ext_string::register_natives(natives);
    ext_array::register_natives(natives);
    ext_math::register_natives(natives);

    OpOverloadTable overloads;
    ext_vec::register_overloads(overloads);
    ext_quat::register_overloads(overloads);
    ext_mat::register_overloads(overloads);
    ext_string::register_overloads(overloads);
    // array + math have no operator overloads (method-call natives only).

    // ---- struct layouts + string key + sema ----
    auto struct_layouts = build_struct_layouts(pr.program);
    // key=0: string literals bake as raw rodata pointers (codegen's unencrypted
    // branch). A nonzero key would enable encrypted-rodata codegen, which emits
    // a call to __str_decrypt — a host-side obfuscation native the standard
    // extension set doesn't register, so it would crash on any string literal.
    // Encrypted rodata is a host opt-in (the host registers __str_decrypt and
    // sets a nonzero key); a standalone language CLI has no such host, so it
    // must leave encryption off.
    pr.program.string_xor_key = 0;
    auto sr = sema(pr.program, natives, slots, 0, &overloads, &struct_layouts);
    if (!sr.ok) {
        std::fprintf(stderr, "ember: sema errors (%zu):\n", sr.errors.size());
        for (auto& e : sr.errors) std::fprintf(stderr, "  line %u: %s\n", e.line, e.msg.c_str());
        return 2;
    }

    // ---- globals block (sized from declared globals; empty if none) ----
    // Codegen addresses global i at [base + i*8]; size = 8 * #globals.
    GlobalsBlock gb;
    {
        uint32_t gi = 0;
        for (auto& g : pr.program.globals) {
            gb.index[g.name] = gi++;
            gb.types[g.name] = g.ty.get();
        }
    }
    std::vector<uint8_t> gb_store(pr.program.globals.size() * 8, 0);
    gb.base = int64_t(gb_store.data());
    g_globals_for_codegen = &gb;

    // ---- dispatch table + codegen ctx (mirrors em_roundtrip_test) ----
    DispatchTable table(pr.program.funcs.size());
    CodeGenCtx ctx;
    ctx.globals_base = gb.base;
    ctx.dispatch_base = int64_t(table.base());
    ctx.natives = &natives;
    ctx.script_slots = &slots;
    ctx.structs = &struct_layouts;
    // str_decrypt_fn stays null: key=0 above means string literals never
    // emit a decrypt call, so no __str_decrypt native is needed.

    // ---- compile + finalize each function ----
    std::vector<CompiledFn> fns;
    fns.reserve(pr.program.funcs.size());
    for (auto& fn : pr.program.funcs) {
        CompiledFn cf = compile_func(fn, ctx);
        if (!finalize(cf)) {
            std::fprintf(stderr, "ember: alloc_executable failed for %s\n", fn.name.c_str());
            for (auto& done : fns) if (done.exec) free_executable(done.exec);
            return 2;
        }
        table.set(fn.slot, cf.entry);
        fns.push_back(std::move(cf));
    }

    // ---- optional dump (reloc capture print, em_roundtrip style) ----
    if (dump) {
        for (size_t i = 0; i < pr.program.funcs.size(); ++i) {
            const auto& fn = pr.program.funcs[i];
            const auto& cf = fns[i];
            std::printf("fn %-20s slot=%-3d bytes=%-6zu relocs=%zu\n",
                        fn.name.c_str(), fn.slot, cf.bytes.size(), cf.abs_fixups.size());
        }
    }

    // ---- locate the entry function ----
    auto sit = slots.find(fn_name);
    if (sit == slots.end()) {
        std::fprintf(stderr, "ember: entry function '%s' not found\n", fn_name.c_str());
        for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
        return 2;
    }
    // find the FuncDecl to read its return type (void vs i64 -> exit-code rule).
    const FuncDecl* entry_decl = nullptr;
    for (auto& fn : pr.program.funcs) if (fn.name == fn_name) { entry_decl = &fn; break; }

    void* entry = table.get(sit->second);
    if (!entry) {
        std::fprintf(stderr, "ember: entry '%s' did not compile\n", fn_name.c_str());
        for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
        return 2;
    }

    // ---- call the entry (Win64: main takes no args; return in rax) ----
    // main() -> i64   : exit with that code (the validation signal)
    // main() -> void  : exit 0
    int exit_code = 0;
    bool returns_void = entry_decl && entry_decl->ret && entry_decl->ret->is_void();
    if (!returns_void) {
        using F0 = int64_t(*)();   // 0-arg, returns i64 in rax
        int64_t r = reinterpret_cast<F0>(entry)();
        exit_code = int(r & 0x7fffffff);   // clamp to a usable exit code
    }

    // ---- cleanup ----
    for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
    // reset extension host stores (array/string own process-global storage)
    ext_vec::reset(); ext_quat::reset(); ext_mat::reset();
    ext_string::reset(); ext_array::reset();
    // ext_math is stateless (no reset()).

    return exit_code;
}
