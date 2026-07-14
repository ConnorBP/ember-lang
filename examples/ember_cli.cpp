// ember_cli - standalone ember language runner (docs/planning/RESTRUCTURE_PLAN.md).
//
// A minimal, prism-decoupled CLI that exercises the full
// parse -> sema -> codegen -> finalize -> call pipeline against the
// REAL ember frontend + the full eight-extension addon surface. This is
// the host shape proven by examples/em_roundtrip_test.cpp (the
// parse/sema/codegen/finalize/call sequence) plus examples/ext_registration_test.cpp
// (the eight-extension natives + overloads registration), combined into one
// runnable program. No prism linkage: no prism_script_host, no VFS, no
// memory backend, no proc/shader api, no pak/assets. Just: read file,
// resolve imports, compile, run, exit code.
//
// Usage:
//   ember run <file.ember> [--fn <name>] [--dump] [--emit-em <output.em>]
//                           [--tick [--tick-count N] [--tick-interval MS]]
//   ember emit-em <input.ember> <output.em>
//   ember bundle <input.ember> <output.exe> [--stub PATH] [--fn NAME]
//                [--permissions none|ffi] [--output-permissions stub|preserve]
//   ember run --load-em <file.em> [--fn <name>]
//
//   run        compiles <file.ember> and calls the entry function unless
//              --emit-em selects precompile-only output.
//   emit-em    precompiles input to the explicit positional output path.
//   --fn NAME  entry function to call (default: main).
//   --dump     after compile, print each fn's name, slot, byte size, and
//              reloc (AbsFixup) count, mirroring em_roundtrip's reloc capture.
//
// Return convention: if the entry returns i64, that value is the process
// exit code (the validation signal); if it returns void, exit 0. The runner
// never echoes the return value - scripts that want output call a native.
// There is no print_i64 in the standard extension set (vec/quat/mat/string/
// array/math/sync/lifecycle), so YAGNI: keep it to exit-code-as-signal.
//
// Build: linked against ember + ember_frontend + all eight ember_ext_* libs.
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
#include "../src/lifecycle.hpp"    // v0.6 get_annotated_functions (@on_tick discovery)
#include "../src/globals.hpp"      // v1.0 eval_global_initializers (seed const globals at load)
#include "../src/context.hpp"     // context_t, TrapStub, TrapReason (v0.4 safe execution)
#include "../src/safety.hpp"      // process-wide failsafes: RSS memory cap, deadline (incident post-mortem)
#include "../src/module_registry.hpp" // v0.5 live modules (ModuleRegistry)
#include "../src/module_linker.hpp"  // v0.5 live modules (link_em_file, build_*_exports)
#include "../src/module_build.hpp"   // Red 9: compile_publish_module_checked (the required --profile/--passes host boundary)
#include "../src/hot_reload.hpp"    // Family C: `ember live` (HotReloadDomain + ExecutionGuard)
#include "../src/em_loader.hpp"      // v0.5 live modules (LoadedModule, for linked .em modules)
#include "../src/em_file.hpp"        // v0.5 --emit-em (EmModule/EmFunctionRecord)
#include "../src/em_writer.hpp"       // v0.5 --emit-em (write_em_file)

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"
#include "ext_map.hpp"
#include "ext_sync.hpp"
#include "ext_lifecycle.hpp"
#include "ext_opt.hpp"        // Stage C: register_passes (IR optimization passes)
#include "ext_obf.hpp"         // Stage C Step 5: register_passes (IR obfuscation passes)
#include "ext_io.hpp"          // OS I/O (console + file + path), core subset
#include "ext_thread.hpp"      // Tier 4: in-context threads (thread_spawn/join + atomics)
#include "ext_coroutine.hpp"     // #21 coroutines with yield (Windows fibers)
#include "ext_call_raw.hpp"     // self-hosting Stage 4 gap: call_raw(fn_ptr,arg)->i64
#include "ext_audio.hpp"        // realtime-safe raw f32/f64/i32 audio buffer access
#include "ext_gc.hpp"          // tracing GC runtime: lambda env heap management (#20)
#include "../src/ember_pass.hpp"       // Stage C: EmberPassManager
#include "../src/ember_pass_registry.hpp" // Stage C: EmberPassRegistry
#include "../src/ember_pass_pipeline.hpp" // Stage C: build_pipeline_from_string
#include "../src/pipeline_profile.hpp"   // Red 8: PipelineProfile + registry
#include "../src/polymorphic_options.hpp" // Red 8: PolymorphicPassOptions
#include "../src/seed_derivation.hpp"     // Red 8: u64_to_root / FixedRootSeedDeriver

#include <cstdio>
#include <csetjmp>   // setjmp/longjmp (v0.4 safe-execution checkpoint)
#include <thread>    // v0.6 --tick tick thread
#include <atomic>     // v0.6 --tick stop flag
#include <chrono>     // v0.6 --tick interval
#if defined(_WIN32)
#  include <conio.h>     // v0.6 --tick keybind (Windows _kbhit/_getch)
#else
// Linux stubs for the --tick keybind (no conio.h). Keyboard termination is
// Windows-only; on Linux the tick count / timeout stops the loop instead.
static inline int _kbhit() { return 0; }
static inline int _getch() { return 0; }
#endif
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
#include <algorithm>   // Family A: ember bench (std::sort for percentile stats)
#include <cmath>      // Family A: ember bench (std::sqrt for stddev)

// Extension headers resolve via each ember_ext_* lib's PUBLIC include dir
// (extensions/<name>), which flows transitively to this target because we
// link all eight extension libs below.

namespace fs = std::filesystem;

// Shared with ember_bundle.exe via the ember_bundler library.
namespace ember_bundle { int command(int argc, char** argv); }

static std::string read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

// ─── Red 8: fixed --pass-seed parsing ───
//
// Parse a 64-bit fixed root seed from a CLI string. Accepts decimal and hex
// (with a `0x` prefix, any case). 0 and UINT64_MAX are accepted (the documented
// edge seeds). A leading sign, any non-numeric character, an empty string, or a
// value that overflows uint64 is rejected with a structured diagnostic written
// to *err (the caller exits 2). The parse uses an explicit overflow-aware
// accumulation so a too-large hex value (e.g. 0x10000000000000000) is rejected,
// not silently wrapped to a small value, and so a legitimate UINT64_MAX
// (0xFFFFFFFFFFFFFFFF) is accepted regardless of letter case.
//
// Returns true + *out on success; false + *err on failure. Never prints.
static bool parse_pass_seed(const char* s, uint64_t* out, std::string* err) {
    if (!s || s[0] == '\0') {
        if (err) *err = "--pass-seed needs a value (decimal or 0x-prefixed hex u64)";
        return false;
    }
    // Reject a leading sign: the seed is an unsigned u64; '-1' is not UINT64_MAX
    // via a signed parse (it would wrap, hiding the rejection).
    if (s[0] == '-' || s[0] == '+') {
        if (err) *err = std::string("--pass-seed '") + s +
                        "' must not have a sign (expected an unsigned 64-bit value)";
        return false;
    }
    // Detect hex vs decimal by a `0x` / `0X` prefix.
    bool hex = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
    const char* digits = hex ? (s + 2) : s;
    if (hex && digits[0] == '\0') {
        if (err) *err = "--pass-seed '0x' has no hex digits";
        return false;
    }
    // Validate every digit is a legal char for the base, and accumulate with an
    // explicit overflow check. Leading zeros are allowed and skipped. The
    // accumulation detects overflow BEFORE the wrap so UINT64_MAX is accepted
    // but anything strictly greater is rejected.
    uint64_t value = 0;
    bool any_digit = false;
    for (const char* p = digits; *p; ++p) {
        char c = *p;
        uint64_t digit;
        if (c >= '0' && c <= '9') {
            digit = uint64_t(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
            digit = uint64_t(c - 'a' + 10);
        } else if (hex && c >= 'A' && c <= 'F') {
            digit = uint64_t(c - 'A' + 10);
        } else {
            if (err) *err = std::string("--pass-seed '") + s +
                            "' is malformed (expected " +
                            (hex ? "hex" : "decimal") + " digits)";
            return false;
        }
        any_digit = true;
        // Overflow-aware shift+add: value = value*base + digit, but detect
        // overflow before it wraps. For base 16 and 10 this is exact.
        uint64_t base = hex ? 16ull : 10ull;
        // value*base overflows iff value > (UINT64_MAX - digit) / base.
        if (value > (UINT64_MAX - digit) / base) {
            if (err) *err = std::string("--pass-seed '") + s +
                            "' overflows a 64-bit unsigned value";
            return false;
        }
        value = value * base + digit;
    }
    if (!any_digit) {
        if (err) *err = std::string("--pass-seed '") + s +
                        "' has no digits";
        return false;
    }
    *out = value;
    return true;
}

// v0.4 safe-execution trap stub: the JIT calls this (Win64: rcx=context_t*,
// edx=TrapReason, r8=const char* detail) when a trap fires (bounds, budget,
// stack-overflow, @obf_keyed). It records the reason + detail on the context,
// then longjmps back to the ember_call checkpoint so the trap becomes a
// recoverable error (printed + nonzero exit) instead of process death
// (REDSHELL V6-DoS / V7: pre-v0.4 these were SIGSEGV/SIGILL -> crash).
// `ctx` is the process-wide ember context (CLI is single-call; nested
// ember_call checkpoints are v1.0).
extern "C" void ember_cli_trap(ember::context_t* ctx, int reason, const char* detail) {
    if (ctx) {
        ctx->last_trap = static_cast<ember::TrapReason>(reason);
        ctx->last_error = detail ? detail : "<no detail>";
        if (ctx->has_checkpoint) {
            // longjmp (not std::longjmp): restores saved rsp/rbp/ip
            // WITHOUT walking SEH/unwind tables. JIT'd frames have no .pdata,
            // so std::longjmp's table walk faults; the builtin is a direct
            // register restore (matches the spec's "directly, not libc setjmp/longjmp").
            longjmp(ctx->checkpoint, 1);
        }
    }
    std::fprintf(stderr, "ember: unhandled trap (no checkpoint): %s\n", detail ? detail : "?");
    std::abort();
}

static void register_standard_bindings(
        std::unordered_map<std::string, ember::NativeSig>& natives,
        ember::OpOverloadTable* overloads_out=nullptr) {
    using namespace ember;
    ext_vec::register_natives(natives); ext_quat::register_natives(natives);
    ext_mat::register_natives(natives); ext_string::register_natives(natives);
    ext_array::register_natives(natives); ext_math::register_natives(natives);
    ext_map::register_natives(natives);
    ember::ext_sync::register_natives(natives); ember::ext_lifecycle::register_natives(natives);
    ember::ext_io::register_natives(natives);
    ember::ext_coroutine::register_natives(natives);
    ember::ext_call_raw::register_natives(natives);
    ember::ext_audio::register_natives(natives);
    ember::ext_thread::register_natives(natives);
    ember::ext_gc::register_natives(natives);   // __ember_gc_alloc_env/collect/live (lambda env heap)
    OpOverloadTable overloads;
    ext_vec::register_overloads(overloads); ext_quat::register_overloads(overloads);
    ext_mat::register_overloads(overloads); ext_string::register_overloads(overloads);
    // Overloads are semantically separate from ordinary source-call natives,
    // but .em loader binding uses one symbolic host allowlist. Publish every
    // exact sema-resolved overload name/signature into that allowlist.
    for(const auto& item:overloads.entries){
        const OpOverload& o=item.second;
        NativeSig sig;sig.name=o.fn_name;sig.fn_ptr=o.fn_ptr;sig.ret=o.ret;sig.params=o.params;
        natives[o.fn_name]=std::move(sig);
    }
    if(overloads_out)*overloads_out=std::move(overloads);
}

static void usage(FILE* out) {
    std::fprintf(out,
        "ember - standalone ember language runner\n"
        "usage:\n"
        "  ember run <input.ember> [--fn NAME] [--dump] [--emit-em OUTPUT.em]\n"
        "                          [--tick [--tick-count N] [--tick-interval MS]]\n"
        "                          [--passes SPEC] [--profile light|balanced|heavy]\n"
        "                          [--pass-seed <u64>]\n"
        "  ember emit-em <input.ember> <output.em>\n"
        "  ember bundle <input.ember> <output.exe> [--stub PATH] [--fn NAME]\n"
        "               [--permissions none|ffi]\n"
        "               [--output-permissions stub|preserve]\n"
        "  ember run --load-em <input.em> [--fn NAME]\n"
        "  ember bench <input.ember> [--fn NAME] [--iters N] [--warmup N]\n"
        "  ember test [dir]     run every .ember file in <dir> (default tests/lang/)\n"
        "  ember pipe <config>  run a dataflow pipeline (Family C)\n"
        "  ember live <file.ember> [--tick [--tick-count N] [--tick-interval MS]\n"
        "                       [--poll-ms MS]]  live-coding reload runner (Family C)\n"
        "\n"
        "  run                 compile and call the entry function\n"
        "  emit-em             compile without running and write <output.em>\n"
        "  bundle              create a standalone executable using the sibling\n"
        "                      ember_stub_main executable; source permissions are\n"
        "                      none by default, or FFI/IO with --permissions ffi\n"
        "  bench               microbenchmark the entry fn: warmup + N timed iters;\n"
        "                      print min/median/mean/p99/stddev + return value +\n"
        "                      machine/compiler metadata (closes the audit's\n"
        "                      benchmark-methodology gap)\n"
        "  test                native test runner: classify each .ember file by\n"
        "                      expected outcome (// expect: N comment or filename\n"
        "                      convention), compile+run or sema-check each, compare\n"
        "                      actual vs expected, print TAP-ish summary, exit\n"
        "                      non-zero if any failed\n"
        "  pipe                dataflow pipeline runner: load several modules (.ember\n"
        "                      compiled or .em loaded), wire their functions into a\n"
        "                      linear chain of i64->i64 stages, run a stream of i64\n"
        "                      values through it, report the transformed result.\n"
        "                      Exit code = sum of outputs (mod 2^31). Config is a\n"
        "                      text file: `module <alias> <path>`, `stage\n"
        "                      <alias>::<fn>`, `input <start> <count>` (default 1 5)\n"
        "  live                live-coding/reload runner: compile <file.ember>, run\n"
        "                      @on_tick in a loop, poll the file content, recompile +\n"
        "                      reload on change so the tick output evolves live.\n"
        "                      Uses HotReloadDomain + ExecutionGuard (the hot-reload\n"
        "                      API). --tick-count N auto-stops; otherwise runs until\n"
        "                      'q'\n"
        "  --poll-ms MS        live: file-content poll interval (default 500)\n"
        "  --fn NAME           entry function (default: main)\n"
        "  --dump              print each compiled fn: name, slot, byte size, reloc count\n"
        "  --emit-em PATH      run-mode precompile output; compile and write without running\n"
        "  --tick              run @on_tick and dynamic routines on a tick thread\n"
        "  --tick-count N      stop automatically after N ticks (default: keypress)\n"
        "  --tick-interval MS  tick interval in milliseconds (default: 16)\n"
        "  --passes SPEC      run IR optimization passes (e.g. constprop,cse,dce,licm,subst)\n"
        "  --profile NAME     ordinary pipeline profile: light, balanced, or heavy\n"
        "                     (a named recipe expanded through the pass registry;\n"
        "                      --pass-profile is a documented alias). heavy is an\n"
        "                      explicitly experimental bounded-density variant\n"
        "  --pass-seed U64    fixed 64-bit root seed (decimal or 0x-hex) for the\n"
        "                     profile's configured obfuscation factories; 0 and\n"
        "                     UINT64_MAX accepted; malformed/overflow rejected.\n"
        "                     An explicit --passes replaces the profile recipe while\n"
        "                     retaining the profile's seed/options\n"
        "  --ffi              grant FFI permission: enable I/O natives (print, file, path)\n");
}

// Compute the TYPED globals-block layout (chunk c3): per-global (offset, size)
// + per-slice-global backing offset + the total block byte size. Scalars land
// 8 bytes at an 8-aligned offset; structs at StructLayout::size; fixed arrays
// at elem_size*array_len; slices at 16 bytes ({ptr,len}) with their backing
// array appended after all primary slots (8-aligned). A slice global's ptr is
// stored as a RELATIVE offset within the block at bake time so the baked bytes
// round-trip through .em without loader fixup (codegen adds globals_base at
// runtime). Mirrors CG::value_bytes for the per-type byte width.
static uint32_t host_value_bytes(const ember::Type* t, const ember::StructLayoutTable* structs) {
    if (!t) return 8;
    if (t->is_slice) return 16;
    if (t->array_len > 0)
        return uint32_t(t->array_len) * host_value_bytes(t->elem.get(), structs);
    if (!t->struct_name.empty() && structs) {
        auto it = structs->find(t->struct_name);
        if (it != structs->end()) return uint32_t(it->second.size);
    }
    switch (t->prim) {
    case ember::Prim::Bool: case ember::Prim::I8: case ember::Prim::U8: return 1;
    case ember::Prim::I16: case ember::Prim::U16: return 2;
    case ember::Prim::I32: case ember::Prim::U32: case ember::Prim::F32: return 4;
    default: return 8;
    }
}

struct TypedGlobalsLayout {
    uint32_t total_size = 0;
    std::unordered_map<std::string, uint32_t> offsets;
    std::unordered_map<std::string, uint32_t> sizes;
    std::unordered_map<std::string, uint32_t> backing_offsets; // slice globals only
};

static TypedGlobalsLayout compute_typed_globals_layout(const ember::Program& prog,
                                                       const ember::StructLayoutTable& structs) {
    TypedGlobalsLayout L;
    uint32_t cur = 0;
    auto align8 = [](uint32_t v) -> uint32_t { return (v + 7u) & ~7u; };
    // Pass 1: primary slots (scalar/struct/fixed-array/slice {ptr,len}).
    for (const auto& g : prog.globals) {
        uint32_t sz = host_value_bytes(g.ty.get(), &structs);
        cur = align8(cur);
        L.offsets[g.name] = cur;
        L.sizes[g.name] = sz;
        cur += sz;
    }
    // Pass 2: backing regions for slice globals (appended after all primary
    // slots). A slice global with an ArrayLit initializer needs a backing
    // array of count*elem_size bytes; a slice global with no initializer
    // stays a zero slice ({ptr=0,len=0}) and needs no backing.
    for (const auto& g : prog.globals) {
        if (!g.ty || !g.ty->is_slice) continue;
        if (!g.init) continue;
        auto* al = dynamic_cast<const ember::ArrayLit*>(g.init.get());
        if (!al) continue;
        uint32_t elem_sz = host_value_bytes(g.ty->elem.get(), &structs);
        if (elem_sz == 0) elem_sz = 8;
        uint32_t count = uint32_t(al->elements.size());
        cur = align8(cur);
        L.backing_offsets[g.name] = cur;
        cur += count * elem_sz;
    }
    L.total_size = cur;
    return L;
}

// ---- Family A: reusable compile-to-entry helper ----
// Extracted from main()'s compile flow so `ember run`, `ember bench`,
// `--emit-em`, `--tick`, and the new `ember test` subcommand all share ONE
// pipeline. The helper takes a file path + RunOptions and returns a RunResult
// with the process exit code. On ALL return paths (success, compile error,
// runtime trap) it frees JIT executable memory + resets the extension host
// stores — critical for `ember test` which calls this N times in one process.
//
// `sema_only` (used by `ember test` for sema-valid/sema-invalid classification):
// stop after sema. Return 0 if sema OK, 2 if lex/parse/import/sema failed.
// No codegen, no call, no stdout output on success.
struct RunOptions {
    std::string fn_name = "main";
    bool dump = false;
    std::string emit_em_path;
    std::string passes_spec;
    // Red 8: ordinary pipeline profiles + fixed root seed. `profile` is the
    // selected built-in profile name ("light"/"balanced"/"heavy") or empty
    // (no profile). `pass_seed` is the fixed root seed for the profile's
    // configured obf factories (0 by default = fully deterministic). `seed_set`
    // is true when --pass-seed was given explicitly so the seed travels even
    // when its value is 0.
    std::string profile;
    uint64_t pass_seed = 0;
    bool seed_set = false;
    bool tick_mode = false;
    int tick_interval_ms = 16;
    int tick_max = 0;
    bool bench_mode = false;
    int bench_iters = 20;
    int bench_warmup = 5;
    bool ffi_mode = false;
    bool sema_only = false;   // ember test: stop after sema, return 0=OK / 2=fail
    bool parse_only = false;  // ember test: stop after parse, return 0=OK / 2=fail
    bool gc_env = false;      // --gc-env: allocate lambda envs on the tracing GC heap
};

struct RunResult {
    int exit_code = 0;
};

static RunResult run_ember_file(const std::string& file, const RunOptions& opts) {
    using namespace ember;

    // Declare fns early so do_cleanup can capture it by reference. On early
    // return paths (lex/parse/sema errors) fns is empty — the free loop is a
    // no-op, but the extension reset still runs (safe on empty stores; ensures
    // `ember test`'s N-th call starts from clean extension state).
    std::vector<CompiledFn> fns;
    auto do_cleanup = [&]() {
        for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
        ext_vec::reset(); ext_quat::reset(); ext_mat::reset();
        ext_string::reset(); ext_array::reset();
        ember::ext_sync::reset();
        ember::ext_lifecycle::reset();
        ember::ext_io::reset();
        ember::ext_coroutine::coroutine_reset();
        ember::ext_thread::thread_reset();
        ember::ext_call_raw::reset();  // stateless (no-op), for symmetry
        ember::ext_gc::gc_reset();     // clear the GC heap + roots (lambda envs)
        // ext_math is stateless (no reset()).
    };

    if (!fs::exists(file)) {
        std::fprintf(stderr, "ember: no such file: %s\n", file.c_str());
        do_cleanup(); return {2};
    }

    // ---- read + resolve imports (textual `import "path";` inlining) ----
    std::string raw = read_file(file.c_str());
    if (raw.empty() && fs::file_size(file) == 0) {
        // empty file is valid (will fail at parse with a clearer message).
    // (raw.empty() && file_size>0 can't happen - read_file covers that.)
    } else if (raw.empty()) {
        std::fprintf(stderr, "ember: cannot read '%s'\n", file.c_str()); do_cleanup(); return {2};
    }
    std::string base_dir = fs::path(file).parent_path().string();
    std::unordered_set<std::string> seen;
    std::string src;
    try {
        std::string canon = fs::weakly_canonical(fs::path(file)).string();
        seen.insert(canon);
        src = resolve_imports(raw, base_dir, seen);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ember: import error: %s\n", e.what()); do_cleanup(); return {2};
    }

    // ---- lex ----
    auto lr = tokenize(src, file.c_str());
    if (!lr.ok) {
        std::fprintf(stderr, "ember: lex error (%u:%u): %s\n", lr.err_line, lr.err_col, lr.error.c_str());
        do_cleanup(); return {2};
    }

    // ---- parse ----
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) {
        std::fprintf(stderr, "ember: parse error: %s\n", pr.error.c_str());
        do_cleanup(); return {2};
    }
    if (pr.program.funcs.empty()) {
        std::fprintf(stderr, "ember: no functions in '%s'\n", file.c_str()); do_cleanup(); return {2};
    }

    // ---- parse-only mode: `ember test` parse-only classification ----
    // Stop after parse (no sema, no codegen). Return 0 (OK) if we got here
    // without a lex/parse/import error, 2 if an earlier stage failed (those
    // paths already returned). Used for valid_* files that parse OK but may
    // have sema issues (the bash script only parse-checks these).
    if (opts.parse_only) {
        do_cleanup();
        return {0};
    }

    // ---- slot assignment (mirror em_roundtrip_test + prism_script_host) ----
    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : pr.program.funcs) {
        if (fn.ns.empty()) {
            slots[fn.name] = si++;
        } else {
            // Tier 1 namespaces: namespaced fns are only accessible via the
            // qualified name (Ns::fn), not the bare name. This avoids slot
            // collisions between a top-level fn and a namespaced fn with the
            // same short name.
            slots[fn.ns + "::" + fn.name] = si++;
        }
        fn.slot = si - 1;
    }

    // ---- register ALL eight extensions: natives + overloads ----
    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
    register_standard_bindings(natives,&overloads);
    // array, math, sync, and lifecycle expose natives but no operators.

    // ---- struct layouts + string key + sema ----
    auto struct_layouts = build_struct_layouts(pr.program);
    pr.program.string_xor_key = 0xA5;

    // ---- v0.5 live-module link resolution (docs/MODULES.md §5) ----
    ModuleRegistry registry(64);
    std::vector<LoadedModule> linked_ems;
    linked_ems.reserve(pr.program.links.size());
    ModuleExportTable module_exports;
    for (const auto& ld : pr.program.links) {
        if (ld.is_file) {
            std::string path = ld.target;
            if (!path.empty() && !(path[0]=='/' || path[0]=='\\') && (path.size()<2 || path[1]!=':')) {
                fs::path p = fs::path(base_dir) / path;
                path = fs::weakly_canonical(p).string();
            }
            linked_ems.emplace_back();
            std::string lerr;
            EmLoadPolicy em_policy{opts.ffi_mode ? PERM_FFI : 0u, true};
            if (!link_em_file(registry, path.c_str(), ld.alias, linked_ems.back(), &lerr, &natives, nullptr, &em_policy)) {
                std::fprintf(stderr, "ember: link '%s' failed: %s\n", ld.target.c_str(), lerr.c_str());
                do_cleanup(); return {2};
            }
            uint32_t id = registry.find_by_name(ld.alias);
            add_exports(module_exports, ld.alias, build_em_exports(linked_ems.back(), id));
        } else {
            uint32_t id = registry.find_by_name(ld.target);
            if (id != UINT32_MAX) {
                // already registered by a prior link; leave deferred.
            }
        }
    }

    SemaResult sr;
    try {
        sr = sema(pr.program, natives, slots, opts.ffi_mode ? PERM_FFI : 0u,
                  &overloads, &struct_layouts, &module_exports);
    } catch (const safety::DepthLimitExceeded& e) {
        std::fprintf(stderr, "ember: fatal: %s\n", e.what());
        do_cleanup(); return {2};
    }
    if (!sr.ok) {
        std::fprintf(stderr, "ember: sema errors (%zu):\n", sr.errors.size());
        for (auto& e : sr.errors) std::fprintf(stderr, "  line %u: %s\n", e.line, e.msg.c_str());
        do_cleanup(); return {2};
    }
    // Non-fatal deprecation warnings (e.g. `auto` -> use `let x = expr;`).
    if (!sr.warnings.empty()) {
        std::fprintf(stderr, "ember: sema warnings (%zu):\n", sr.warnings.size());
        for (auto& w : sr.warnings) std::fprintf(stderr, "  line %u: %s\n", w.line, w.msg.c_str());
    }

    // ---- sema-only mode: `ember test` sema-valid/sema-invalid classification ----
    // Stop after sema. Return 0 (OK) if we got here without a compile error,
    // 2 if any earlier stage failed (those paths already returned). No
    // codegen, no call, no stdout output — the test runner prints TAP lines.
    if (opts.sema_only) {
        do_cleanup();
        return {0};
    }

    // ---- globals block (TYPED layout, chunk c3; sized from declared globals) ----
    GlobalsBlock gb;
    TypedGlobalsLayout tgl = compute_typed_globals_layout(pr.program, struct_layouts);
    {
        uint32_t gi = 0;
        for (auto& g : pr.program.globals) {
            gb.index[g.name] = gi++;
            gb.types[g.name] = g.ty.get();
            gb.offsets[g.name] = tgl.offsets[g.name];
            gb.sizes[g.name] = tgl.sizes[g.name];
            // Tier 1 namespaces: also register the qualified name.
            if (!g.ns.empty()) {
                std::string qn = g.ns + "::" + g.name;
                gb.index[qn] = gb.index[g.name];
                gb.types[qn] = gb.types[g.name];
                gb.offsets[qn] = gb.offsets[g.name];
                gb.sizes[qn] = gb.sizes[g.name];
            }
        }
    }
    std::vector<uint8_t> gb_store(size_t(tgl.total_size), 0);
    gb.base = int64_t(gb_store.data());
    g_globals_for_codegen = nullptr;
    auto string_alloc_thunk = [](const char* bytes, int64_t len) -> int64_t {
        return ember::ext_string::alloc(std::string(bytes, size_t(len > 0 ? len : 0)));
    };
    GlobalInitCtx gic{gb_store, gb.index, gb.types};
    gic.string_alloc_fn = string_alloc_thunk;
    gic.offsets = &gb.offsets;
    gic.sizes = &gb.sizes;
    gic.backing_offsets = &tgl.backing_offsets;
    gic.structs = &struct_layouts;
    eval_global_initializers(pr.program, gic);

    // ---- dispatch table + codegen ctx (mirrors em_roundtrip_test) ----
    DispatchTable table(pr.program.funcs.size());
    CodeGenCtx ctx;
    ctx.globals_base = gb.base;
    ctx.globals_index = &gb.index;
    ctx.globals_types = &gb.types;
    ctx.globals_offsets = &gb.offsets;
    ctx.dispatch_base = int64_t(table.base());
    ctx.natives = &natives;
    ctx.script_slots = &slots;
    ctx.structs = &struct_layouts;
    ctx.registry_base = int64_t(registry.base());
    // Red 9 host boundary: the __main__ module-registry entry is published
    // ONLY after every function has compiled + finalized. The no-profile /
    // no-pass (legacy) path registers __main__ here (before compile) and uses
    // the existing compile_func loop; the --profile / --passes (required)
    // path DEFERS the __main__ registration to compile_publish_module_checked's
    // publication step so a compile/finalize failure leaves the registry +
    // dispatch table byte-for-byte unchanged (no partial record visible).
    // registry_base + dispatch_base are the STABLE bases baked into the JIT'd
    // code (both stable from construction), so deferring the __main__ ENTRY
    // does not move any baked address.
    const bool needs_recipe = !opts.passes_spec.empty() || !opts.profile.empty();
    std::string reg_err;
    uint32_t self_id = UINT32_MAX;
    if (!needs_recipe) {
        self_id = registry.register_module("__main__", table.base(), &reg_err);
        (void)self_id;
        // X1 redesign: publish __main__'s dispatch-table slot count so a loaded v5
        // .em that calls back into the host via CallCrossModule range-checks its
        // slot against the REAL host dispatch size at load time.
        registry.set_dispatch_slot_count(self_id, int64_t(table.slots.size()));
    }

    // ---- v0.4/v1.0 safe-execution context ----
    context_t ectx;
    ectx.budget_remaining = 100000000;
    ectx.max_call_depth = 512;
    ectx.has_checkpoint = false;
    if (opts.emit_em_path.empty()) ctx.trap_stub = reinterpret_cast<void*>(&ember_cli_trap);
    ctx.use_context_reg = opts.emit_em_path.empty();
    ctx.safe_defaults();
    ctx.max_call_depth = ectx.max_call_depth;
    if (!opts.emit_em_path.empty())
        std::fprintf(stderr,
            "ember: note: emitted modules retain sandbox guard metadata; the loading host "
            "must provide execution context/checkpoint storage for runtime enforcement\n");
    std::vector<uint8_t> fn_allowlist = build_fn_allowlist(slots, int(slots.size()));
    if (opts.emit_em_path.empty()) {
        ctx.fn_allowlist_base = int64_t(fn_allowlist.data());
        ctx.fn_slot_count = int64_t(slots.size());
    }

    // #21 coroutines: register the context + dispatch table the coroutine
    // natives call into, + convert the calling thread to a fiber (fibers
    // require the thread to be a fiber first so GetCurrentFiber() returns the
    // caller's fiber — coroutine_next uses it as the yield switch-back
    // target). Only in the JIT run path (emit-em is a pre-compile, no run).
    if (opts.emit_em_path.empty()) {
        ember::ext_coroutine::coroutine_init(&ectx, table.base(), int64_t(slots.size()));
        ember::ext_thread::thread_init(&ectx, table.base(), int64_t(slots.size()));
        // #20 GC-managed lambda envs: allocate the thread-local GcHeap so
        // __ember_gc_alloc_env has a heap to allocate on when use_gc_env is
        // set. Init unconditionally (cheap; idempotent) so the heap is ready
        // even if only some functions use GC envs.
        ember::ext_gc::gc_init();
    }
    ctx.use_gc_env = opts.gc_env;

    // Stage C: --passes <spec>  +  Red 8: --profile <name> [--pass-seed <u64>]
    //
    // Profiles are ORDINARY NAMED RECIPES expanded through the existing
    // EmberPassRegistry + build_pipeline_from_string into a fresh
    // EmberPassManager — no hidden pass-manager modes. Selecting a profile
    // configures the obf factories with the profile's options (derived from the
    // fixed root seed) ONLY AFTER the seed/options overrides are resolved, then
    // expands the profile's recipe. An explicit `--passes` REPLACES the selected
    // profile recipe while retaining the profile's options/seed (the obf factories
    // stay configured by the profile's options so a profile-selected seed still
    // drives diversification in the explicit recipe's obf passes). Neither
    // option silently appends or alters instrumentation. With no profile and no
    // --passes the pass manager stays empty (the existing no-pass behavior is
    // preserved exactly).
    EmberPassRegistry pass_reg;
    ext_opt::register_passes(pass_reg);
    // Configure the obf factories: if a profile is selected, use the profile's
    // options (seed-bound); otherwise use the deterministic defaults so an
    // explicit --passes recipe resolves obf names the way it always has.
    PolymorphicPassOptions obf_options;
    std::string profile_recipe;  // the profile's recipe (empty if no profile)
    bool profile_selected = !opts.profile.empty();
    if (profile_selected) {
        PipelineProfileRegistry preg;
        ExtensionStatus pst = register_builtin_profiles(preg, opts.pass_seed);
        if (!bool(pst)) {
            std::fprintf(stderr, "ember: --profile: internal error: %s\n",
                          pst.error ? pst.error->message.c_str() : "unknown");
            do_cleanup(); return {2};
        }
        const PipelineProfile* prof = preg.get(opts.profile);
        if (!prof) {
            std::fprintf(stderr, "ember: --profile: unknown profile '%s' "
                          "(expected light, balanced, or heavy)\n",
                          opts.profile.c_str());
            do_cleanup(); return {2};
        }
        obf_options = prof->options;
        profile_recipe = prof->recipe;
        if (prof->is_experimental) {
            std::fprintf(stderr, "ember: --profile: note: '%s' is an experimental "
                          "profile (bounded higher density)\n", opts.profile.c_str());
        }
        ext_obf::register_passes(pass_reg, obf_options);
    } else {
        ext_obf::register_passes(pass_reg);  // deterministic defaults
    }
    EmberPassManager pass_pm;
    // Decide the effective recipe. An explicit --passes REPLACES the profile
    // recipe (the profile's options/seed are retained for the obf factories).
    // If only a profile is selected, the profile recipe is the pipeline. If
    // neither is given, the pass manager stays empty.
    std::string effective_recipe;
    bool have_recipe = false;
    if (!opts.passes_spec.empty()) {
        // Explicit --passes replaces the profile recipe; the profile's options
        // already configured the obf factories above (profile_selected branch).
        effective_recipe = opts.passes_spec;
        have_recipe = true;
    } else if (profile_selected) {
        effective_recipe = profile_recipe;
        have_recipe = true;
    }
    if (have_recipe) {
        std::string pass_err;
        if (!build_pipeline_from_string(effective_recipe, pass_reg, pass_pm, &pass_err)) {
            std::fprintf(stderr, "ember: %s: %s\n",
                          opts.passes_spec.empty() ? "--profile" : "--passes",
                          pass_err.c_str());
            do_cleanup(); return {2};
        }
        ctx.pass_manager = &pass_pm;
        ctx.enable_ir_backend = true;
        ctx.enable_regalloc = true;  // regalloc runs once after the whole pass pipeline
    }

    // ---- compile + finalize + publish each function ----
    // Red 9 host boundary: the --profile / --passes (required) path uses
    // compile_publish_module_checked — compile_func_checked for EVERY fn,
    // private staged ownership, atomic publication of the dispatch slots +
    // __main__ registry metadata only after every fn compiled + finalized.
    // On any required failure it frees every staged exec allocation and
    // leaves the dispatch table + registry + name state byte-for-byte
    // unchanged (the structured ModuleBuildReport records per-function
    // compile/validation/regalloc/emission/finalization/publication status,
    // proving a validation failure reached none of the later stages).
    //
    // TreeWalker rejection: a named --profile is a REQUIRED optimized build —
    // a CompileBackend::TreeWalker fallback is a hard failure (the IR backend
    // is required for every fn in a profile build). Bare --passes (the pass-
    // VALIDATION path, e.g. optimization_validation.ember) intentionally
    // exercises features that lower to the tree-walker (for-each, match,
    // structs); the passes still run on the IR-backend functions and the
    // tree-walker functions run unoptimized, so a tree-walker fallback is
    // ACCEPTED for bare --passes (reject_trewalker = profile-selected only).
    // This preserves the documented pass-validation semantics (validation
    // returns 177) while a named profile enforces the optimized-build contract.
    // The no-profile / no-pass (legacy) path keeps the existing compile_func +
    // inline finalize + table.set loop (behavior unchanged; __main__ was
    // registered before compile in that path).
    fns.reserve(pr.program.funcs.size());
    if (needs_recipe) {
        std::string mb_err;
        ModuleBuildReport mb = compile_publish_module_checked(
            pr.program.funcs, slots, ctx, table, registry,
            "__main__", fns, /*reject_trewalker=*/profile_selected, &mb_err);
        if (!mb.ok) {
            // The helper freed every staged exec allocation; the dispatch
            // table + registry are unchanged. Report the first failure with
            // the per-function stage evidence so a host can see WHICH fn +
            // WHICH stage failed (compile / checked-passes / pre-emit-verify /
            // emission / finalize / publication / trewalker-fallback).
            std::fprintf(stderr, "ember: %s\n", mb.fail_reason.c_str());
            for (const auto& fr : mb.fn_reports) {
                if (!fr.compile_ok || !fr.first_failure.empty()) {
                    std::fprintf(stderr, "ember:   %s: backend=%d first_failure=%s reason=%s\n",
                                 fr.name.c_str(), int(fr.backend),
                                 fr.first_failure.c_str(), fr.reason.c_str());
                }
            }
            do_cleanup(); return {2};
        }
        // Publication succeeded: dispatch slots + __main__ registry metadata
        // are committed. fns holds the caller-owned finalized CompiledFns.
    } else {
        try {
            for (auto& fn : pr.program.funcs) {
                CompiledFn cf = compile_func(fn, ctx);
                if (!finalize(cf)) {
                    std::fprintf(stderr, "ember: alloc_executable failed for %s\n", fn.name.c_str());
                    do_cleanup(); return {2};
                }
                table.set(fn.slot, cf.entry);
                fns.push_back(std::move(cf));
            }
        } catch (const safety::DepthLimitExceeded& e) {
            std::fprintf(stderr, "ember: fatal: %s\n", e.what());
            do_cleanup(); return {2};
        }
    }

    // ---- v0.5 --emit-em: pre-compile the parsed module to a .em bundle ----
    if (!opts.emit_em_path.empty()) {
        EmModule mod;
        mod.functions.reserve(fns.size());
        const std::vector<uint8_t>& globals_bytes = gb_store;
        for (size_t fi=0; fi<fns.size(); ++fi) {
            const auto& cf=fns[fi];
            const auto& decl=pr.program.funcs[fi];
            EmFunctionRecord rec;
            rec.name = cf.name;
            rec.slot_index = uint32_t(decl.slot);
            rec.code = cf.bytes;
            rec.rodata = cf.rodata;
            rec.non_serializable_reason = cf.non_serializable_reason;
            rec.signature.ret=decl.ret?*decl.ret:Type{};
            for(const auto& p:decl.params)rec.signature.params.push_back(p.ty?*p.ty:Type{});
            for (const auto& af : cf.abs_fixups) { EmReloc r; r.offset = af.code_offset; r.kind = uint8_t(af.kind); r.addend=af.addend; rec.relocs.push_back(r); }
            for(const auto& nf:cf.native_fixups){EmNativeBinding b;b.offset=nf.code_offset;b.name=nf.name;b.signature.ret=nf.ret;b.signature.params=nf.params;rec.native_bindings.push_back(std::move(b));}
            mod.functions.push_back(std::move(rec));
        }
        mod.globals = globals_bytes;
        uint32_t entry_slot = EM_NO_ENTRY;
        for (const auto& fn : pr.program.funcs) for (const auto& a : fn.annotations) if (a.name == "entry") { entry_slot = uint32_t(fn.slot); break; }
        if (entry_slot == EM_NO_ENTRY) { auto sit2 = slots.find("main"); if (sit2 != slots.end()) entry_slot = uint32_t(sit2->second); }
        mod.entry_slot = entry_slot;
        mod.name_table.reserve(pr.program.funcs.size());
        for (const auto& fn : pr.program.funcs)
            if (fn.is_exported)
                mod.name_table.push_back({fn.name, uint32_t(fn.slot)});
        std::string werr;
        if (!write_em_file(mod, opts.emit_em_path.c_str(), &werr)) {
            std::fprintf(stderr, "ember: --emit-em write failed: %s\n", werr.c_str());
            do_cleanup(); return {2};
        }
        std::printf("ember: wrote %s (%zu fns, %zu globals block bytes, entry slot %u)\n",
                    opts.emit_em_path.c_str(), mod.functions.size(), mod.globals.size(), mod.entry_slot);
        do_cleanup(); return {0};
    }

    // ---- optional dump (reloc capture print, em_roundtrip style) ----
    if (opts.dump) {
        for (size_t i = 0; i < pr.program.funcs.size(); ++i) {
            const auto& fn = pr.program.funcs[i];
            const auto& cf = fns[i];
            std::printf("fn %-20s slot=%-3d bytes=%-6zu relocs=%zu\n",
                        fn.name.c_str(), fn.slot, cf.bytes.size(), cf.abs_fixups.size());
        }
    }
    // ---- locate the entry function ----
    auto sit = slots.find(opts.fn_name);
    if (sit == slots.end()) {
        std::fprintf(stderr, "ember: entry function '%s' not found\n", opts.fn_name.c_str());
        do_cleanup(); return {2};
    }
    const FuncDecl* entry_decl = nullptr;
    for (auto& fn : pr.program.funcs) if (fn.name == opts.fn_name) { entry_decl = &fn; break; }

    void* entry = table.get(sit->second);
    if (!entry) {
        std::fprintf(stderr, "ember: entry '%s' did not compile\n", opts.fn_name.c_str());
        do_cleanup(); return {2};
    }

    // ---- call the entry ----
    int exit_code = 0;
    int64_t entry_ret = 0;
    bool returns_void = entry_decl && entry_decl->ret && entry_decl->ret->is_void();
    ectx.has_checkpoint = true;

    // ---- Family A: `ember bench` — microbenchmark the entry fn ----
    if (opts.bench_mode) {
        for (int w = 0; w < opts.bench_warmup; ++w) {
            // In-context threads share ectx's checkpoint. Hold call_mutex for
            // every outer call so thread_join can hand it to a worker safely.
            // Raw lock/unlock is intentional: a trap longjmp skips destructors.
            ectx.call_mutex.lock();
            if (setjmp(ectx.checkpoint)) {
                ectx.call_mutex.unlock();
                std::fprintf(stderr, "ember: bench warmup trap: %s\n", ectx.last_error.c_str());
                ectx.has_checkpoint = false; do_cleanup(); return {70};
            }
            if (!returns_void) entry_ret = ember::ember_call_void(entry, &ectx);
            else              ember::ember_call_void(entry, &ectx);
            ectx.call_mutex.unlock();
        }
        std::vector<double> ns; ns.reserve(size_t(opts.bench_iters));
        bool bench_trapped = false;
        for (int it = 0; it < opts.bench_iters; ++it) {
            ectx.call_mutex.lock();
            if (setjmp(ectx.checkpoint)) {
                ectx.call_mutex.unlock();
                std::fprintf(stderr, "ember: bench iter %d trap: %s\n", it, ectx.last_error.c_str());
                bench_trapped = true; break;
            }
            auto t0 = std::chrono::steady_clock::now();
            if (!returns_void) entry_ret = ember::ember_call_void(entry, &ectx);
            else              (void)ember::ember_call_void(entry, &ectx);
            auto t1 = std::chrono::steady_clock::now();
            ectx.call_mutex.unlock();
            ns.push_back(double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }
        if (bench_trapped) { ectx.has_checkpoint = false; do_cleanup(); return {70}; }
        std::sort(ns.begin(), ns.end());
        double mn = ns.front(), mx = ns.back();
        double mean = 0; for (double v : ns) mean += v; mean /= double(ns.size());
        double sd = 0; for (double v : ns) sd += (v - mean) * (v - mean); sd = ns.size() > 1 ? std::sqrt(sd / double(ns.size() - 1)) : 0.0;
        double median = ns[ns.size() / 2];
        double p99 = ns[size_t(double(ns.size() - 1) * 0.99)];
#if defined(__GNUC__)
        const char* cc = "gcc"; const char* ccver = __VERSION__;
#elif defined(_MSC_VER)
        const char* cc = "msvc"; const char* ccver = "?";
#else
        const char* cc = "unknown"; const char* ccver = "?";
#endif
        std::printf("# ember bench %s [--fn %s] --iters %d --warmup %d\n", file.c_str(), opts.fn_name.c_str(), opts.bench_iters, opts.bench_warmup);
        std::printf("#   compiler: %s %s\n", cc, ccver);
        std::printf("#   platform: %s (ptr=%zu-bit)\n",
#if defined(__x86_64__) || defined(_M_X64)
                    "x86-64", sizeof(void*)*8
#else
                    "unknown", sizeof(void*)*8
#endif
        );
        std::printf("#   date:     %s %s\n", __DATE__, __TIME__);
        std::printf("#   iters=%d  warmup=%d  entry_returns=%s\n", opts.bench_iters, opts.bench_warmup, returns_void ? "void" : "i64");
        std::printf("result        %lld\n", (long long)entry_ret);
        std::printf("min_ns        %.1f\n", mn);
        std::printf("median_ns     %.1f\n", median);
        std::printf("mean_ns       %.1f\n", mean);
        std::printf("p99_ns        %.1f\n", p99);
        std::printf("max_ns        %.1f\n", mx);
        std::printf("stddev_ns     %.1f\n", sd);
        std::printf("cv_pct        %.2f\n", mean > 0 ? (sd / mean) * 100.0 : 0.0);
        ectx.has_checkpoint = false;
        do_cleanup();
        return {0};
    }

    // The host's outer call participates in the same mutex protocol as
    // spawned workers. This prevents a worker from replacing ectx.checkpoint
    // while the main thread is executing and potentially trapping.
    ectx.call_mutex.lock();
    if (setjmp(ectx.checkpoint)) {
        ectx.call_mutex.unlock();
        std::fprintf(stderr, "ember: RUNTIME TRAP: %s (%s)\n",
                     ectx.last_error.c_str(), ember::trap_reason_str(ectx.last_trap));
        exit_code = 70;
    } else {
        entry_ret = ember::ember_call_void(entry, &ectx);
        ectx.call_mutex.unlock();
        if (!returns_void)
            exit_code = int(uint64_t(entry_ret) & 0x7fffffff);
    }
    ectx.has_checkpoint = false;

    // ---- v0.6 --tick mode: run @on_tick fns on a thread until a keybind ----
    if (opts.tick_mode && exit_code != 70) {
        bool entry_says_stay = (entry_decl && entry_decl->ret && !entry_decl->ret->is_void())
                               ? (entry_ret > 0) : true;
        auto ticks = ember::get_annotated_functions(pr.program, "@on_tick");
        auto dyn_routines = ember::ext_lifecycle::host_routines();
        if (!entry_says_stay) {
            std::printf("ember: @entry returned <= 0, module unloaded (no tick)\n");
        } else if (ticks.empty() && dyn_routines.empty()) {
            std::printf("ember: --tick mode but no @on_tick functions and no dynamically-registered routines; nothing to tick\n");
        } else {
            std::printf("ember: --tick mode — %zu @on_tick fn(s) + %zu dynamic routine(s), %dms interval. Press 'q' to unload + exit.\n",
                        ticks.size(), dyn_routines.size(), opts.tick_interval_ms);
            std::atomic<bool> stop{false};
            std::atomic<uint64_t> tick_count{0};
            std::atomic<bool> tick_trapped{false};
            std::thread tick_thread([&]() {
                context_t tick_ctx; tick_ctx.max_call_depth = 512; tick_ctx.budget_remaining = 100000000;
                // Tick callbacks are outer calls too. Point thread_spawn at
                // this context and use its call_mutex/checkpoint as one unit.
                ember::ext_thread::thread_init(&tick_ctx, table.base(), int64_t(slots.size()));
                while (!stop.load(std::memory_order_relaxed)) {
                    if (opts.tick_max > 0 && tick_count.load(std::memory_order_relaxed) >= (uint64_t)opts.tick_max) { stop.store(true); break; }
                    tick_ctx.call_depth = 0;
                    tick_ctx.has_checkpoint = true;
                    tick_ctx.call_mutex.lock();
                    if (setjmp(tick_ctx.checkpoint)) {
                        tick_ctx.call_mutex.unlock();
                        tick_ctx.has_checkpoint = false;
                        tick_trapped.store(true); stop.store(true); break;
                    }
                    for (auto& af : ticks) {
                        void* f = table.get(af.slot);
                        if (f) {
                            ember::ember_call_void(f, &tick_ctx);
                        }
                    }
                    for (auto& r : ember::ext_lifecycle::host_routines()) {
                        void* f = table.get(int(r.slot));
                        if (f) {
                            ember::ember_call_i64(f, &tick_ctx, r.data);
                        }
                    }
                    tick_ctx.has_checkpoint = false;
                    tick_ctx.call_mutex.unlock();
                    tick_count.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(std::chrono::milliseconds(opts.tick_interval_ms));
                }
            });
            while (!stop.load(std::memory_order_relaxed)) {
                std::printf("\r ember tick: %llu ticks   [q to quit]   ",
                            (unsigned long long)tick_count.load());
                std::fflush(stdout);
                if (_kbhit()) { int c = _getch(); if (c == 'q' || c == 'Q') stop.store(true); }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            tick_thread.join();
            std::printf("\nember: stopped after %llu ticks%s\n",
                        (unsigned long long)tick_count.load(),
                        tick_trapped.load() ? " (a tick trapped — runtime error, see above)" : "");
            if (tick_trapped.load()) exit_code = 70;
        }
    }

    do_cleanup();
    return {exit_code};
}

// ---- Family A: `ember test [dir]` — native test runner ----
// Runs every .ember file in <dir> (default tests/lang/), classifies each by
// expected outcome, compiles+runs (or sema-checks) each, compares actual vs
// expected, and prints a TAP-ish summary. Exits non-zero if any test failed.
//
// Classification (mirrors tests/run_lang_tests.sh):
//   1. `// expect: N` comment anywhere in the file  -> RUN, expect exit N
//   2. filename starts with `runtime_trap_`         -> RUN, expect exit 70
//   3. filename starts with `invalid_` or `sema_invalid_` -> SEMA-ONLY,
//      expect exit 2 (compile fail: parse or sema error)
//   4. everything else (sema_valid_*, valid_*, runtime_*, import_*, etc.)
//      -> SEMA-ONLY, expect exit 0 (sema OK; no execution)
//
// `// expect: N` takes priority over filename conventions — a sema_valid_*
// file with `// expect: 6` is RUN (expected 6), not sema-only.
//
// Only top-level .ember files are enumerated (non-recursive) — subdirs like
// lib/, lib2/, lib3/, sub/ are import targets, not test files.
struct TestClassifier {
    enum class Kind { Run, ParseFail, SemaFail, SemaOk, ParseOk };
    Kind kind = Kind::ParseOk;
    int expected_exit = 0;   // for Run: the expected exit code
};

static TestClassifier classify_test(const fs::path& filepath) {
    std::string name = filepath.filename().string();

    // Scan the file for a `// expect: N` comment (anywhere, first match wins).
    std::ifstream f(filepath);
    std::string line;
    while (std::getline(f, line)) {
        // Strip leading whitespace for matching.
        size_t first_non_ws = line.find_first_not_of(" \t");
        if (first_non_ws == std::string::npos) continue;
        std::string trimmed = line.substr(first_non_ws);
        if (trimmed.size() >= 2 && trimmed[0] == '/' && trimmed[1] == '/') {
            // It's a comment line — check for `expect:`.
            size_t pos = trimmed.find("expect:");
            if (pos != std::string::npos) {
                // Parse the integer after `expect:`.
                size_t num_start = trimmed.find_first_of("0123456789-", pos + 7);
                if (num_start != std::string::npos) {
                    size_t num_end = num_start;
                    if (trimmed[num_start] == '-') num_end++;
                    while (num_end < trimmed.size() && (trimmed[num_end] >= '0' && trimmed[num_end] <= '9')) num_end++;
                    try {
                        int n = std::stoi(trimmed.substr(num_start, num_end - num_start));
                        TestClassifier tc; tc.kind = TestClassifier::Kind::Run; tc.expected_exit = n;
                        return tc;
                    } catch (...) { /* malformed — keep scanning */ }
                }
            }
        }
    }

    // No `// expect:` found — classify by filename (mirrors run_lang_tests.sh).
    if (name.rfind("runtime_trap_", 0) == 0) {
        TestClassifier tc; tc.kind = TestClassifier::Kind::Run; tc.expected_exit = 70;
        return tc;
    }
    if (name.rfind("invalid_realtime_", 0) == 0) {
        TestClassifier tc; tc.kind = TestClassifier::Kind::SemaFail; tc.expected_exit = 2;
        return tc;
    }
    if (name.rfind("invalid_", 0) == 0) {
        TestClassifier tc; tc.kind = TestClassifier::Kind::ParseFail; tc.expected_exit = 2;
        return tc;
    }
    if (name.rfind("sema_invalid_", 0) == 0) {
        TestClassifier tc; tc.kind = TestClassifier::Kind::SemaFail; tc.expected_exit = 2;
        return tc;
    }
    // sema_valid_* and the realtime contract positive probe must sema OK.
    if (name.rfind("sema_valid_", 0) == 0 || name == "valid_realtime.ember") {
        TestClassifier tc; tc.kind = TestClassifier::Kind::SemaOk; tc.expected_exit = 0;
        return tc;
    }
    // Everything else (valid_*, runtime_*, import_*, lifecycle_*, etc.):
    // parse-only check. The bash script only parse-checks these (some valid_*
    // files have sema issues that are not part of the test contract).
    TestClassifier tc; tc.kind = TestClassifier::Kind::ParseOk; tc.expected_exit = 0;
    return tc;
}

static int run_test_command(const std::string& dir) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        std::fprintf(stderr, "ember test: no such directory: %s\n", dir.c_str());
        return 2;
    }

    // Enumerate top-level .ember files (non-recursive), sorted for stable output.
    std::vector<fs::path> files;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".ember") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end(),
              [](const fs::path& a, const fs::path& b) {
                  return a.filename().string() < b.filename().string();
              });

    if (files.empty()) {
        std::fprintf(stderr, "ember test: no .ember files in %s\n", dir.c_str());
        return 2;
    }

    int test_num = 0;
    int passed = 0;
    int failed = 0;

    for (const auto& filepath : files) {
        std::string name = filepath.filename().string();
        std::string path_str = filepath.string();
        TestClassifier tc = classify_test(filepath);
        ++test_num;

        RunOptions opts;
        opts.fn_name = "main";
        opts.sema_only = (tc.kind == TestClassifier::Kind::SemaOk || tc.kind == TestClassifier::Kind::SemaFail);
        opts.parse_only = (tc.kind == TestClassifier::Kind::ParseOk || tc.kind == TestClassifier::Kind::ParseFail);

        RunResult result = run_ember_file(path_str, opts);
        int actual = result.exit_code;
        int expected = tc.expected_exit;

        if (actual == expected) {
            std::printf("ok %d - %s\n", test_num, name.c_str());
            ++passed;
        } else {
            std::printf("not ok %d - %s (expected %d got %d)\n", test_num, name.c_str(), expected, actual);
            ++failed;
        }
    }

    std::printf("\n# %d/%d passed, %d failed\n", passed, test_num, failed);
    return failed > 0 ? 1 : 0;
}

// ---- Family C: shared compile-to-state helper ----
// compile_script is the compile portion of run_ember_file (read -> imports ->
// lex -> parse -> sema -> codegen -> finalize), returning the compiled state
// (fns, dispatch table, slots, globals, program) WITHOUT calling any function
// and WITHOUT cleanup. The caller owns the returned state and must keep it
// alive for as long as any JIT'd entry is callable. Used by `ember pipe`
// (several modules alive simultaneously) and `ember live` (recompiled on file
// change). Mirrors run_ember_file's proven flow exactly; the difference is the
// state is RETURNED, not consumed + cleaned up in place.
//
// The returned CompiledScript keeps the ModuleRegistry + linked .em modules
// alive (their base/dispatch addresses are baked into the JIT'd code as imm64
// relocs and dereferenced at call time), so a pipe/live module that uses live
// `link "x.em" as x;` directives keeps working after compile returns.
struct CompiledScript {
    std::vector<ember::CompiledFn> fns;
    std::unique_ptr<ember::DispatchTable> table;
    std::unordered_map<std::string, int> slots;
    std::unordered_map<std::string, void*> entries;  // fn name -> entry ptr
    ember::GlobalsBlock gb;
    std::vector<uint8_t> gb_store;
    ember::Program prog;
    ember::StructLayoutTable struct_layouts;
    std::unique_ptr<ember::ModuleRegistry> registry;     // kept alive (base baked into fns)
    std::vector<ember::LoadedModule> linked_ems;          // kept alive (dispatch baked into fns)
    bool ok = false;

    CompiledScript() = default;
    ~CompiledScript() { for (auto& fn : fns) if (fn.exec) ember::free_executable(fn.exec); }
    CompiledScript(const CompiledScript&) = delete;
    CompiledScript& operator=(const CompiledScript&) = delete;
    CompiledScript(CompiledScript&&) noexcept = default;
    CompiledScript& operator=(CompiledScript&&) noexcept = default;
};

static std::unique_ptr<CompiledScript> compile_script(
        const std::string& file_path,
        const std::unordered_map<std::string, ember::NativeSig>& natives,
        const ember::OpOverloadTable& overloads,
        std::string& err) {
    using namespace ember;
    auto cs = std::make_unique<CompiledScript>();

    if (!fs::exists(file_path)) { err = "no such file: " + file_path; return nullptr; }
    std::string raw = read_file(file_path.c_str());
    if (raw.empty() && fs::file_size(file_path) > 0) { err = "cannot read " + file_path; return nullptr; }
    if (raw.empty()) { err = "empty file: " + file_path; return nullptr; }
    std::string base_dir = fs::path(file_path).parent_path().string();
    std::unordered_set<std::string> seen;
    std::string src;
    try {
        seen.insert(fs::weakly_canonical(fs::path(file_path)).string());
        src = resolve_imports(raw, base_dir, seen);
    } catch (const std::exception& e) { err = std::string("import error: ") + e.what(); return nullptr; }

    auto lr = tokenize(src, file_path.c_str());
    if (!lr.ok) { err = "lex error (" + std::to_string(lr.err_line) + ":" + std::to_string(lr.err_col) + "): " + lr.error; return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { err = "parse error: " + pr.error; return nullptr; }
    if (pr.program.funcs.empty()) { err = "no functions in " + file_path; return nullptr; }
    cs->prog = std::move(pr.program);

    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : cs->prog.funcs) {
        if (fn.ns.empty()) {
            slots[fn.name] = si++;
        } else {
            slots[fn.ns + "::" + fn.name] = si++;
        }
        fn.slot = si - 1;
    }
    cs->slots = slots;

    cs->struct_layouts = build_struct_layouts(cs->prog);
    cs->prog.string_xor_key = 0xA5;

    cs->registry = std::make_unique<ModuleRegistry>(64);
    ModuleExportTable module_exports;
    for (const auto& ld : cs->prog.links) {
        if (ld.is_file) {
            std::string path = ld.target;
            if (!path.empty() && !(path[0]=='/'||path[0]=='\\') && (path.size()<2||path[1]!=':')) {
                path = fs::weakly_canonical(fs::path(base_dir) / path).string();
            }
            cs->linked_ems.emplace_back();
            std::string lerr;
            EmLoadPolicy em_policy{0u, true};
            if (!link_em_file(*cs->registry, path.c_str(), ld.alias, cs->linked_ems.back(), &lerr, &natives, nullptr, &em_policy)) {
                err = "link '" + ld.target + "' failed: " + lerr; return nullptr;
            }
            uint32_t id = cs->registry->find_by_name(ld.alias);
            add_exports(module_exports, ld.alias, build_em_exports(cs->linked_ems.back(), id));
        }
    }

    SemaResult sr;
    try {
        sr = sema(cs->prog, natives, slots, 0, &overloads, &cs->struct_layouts, &module_exports);
    } catch (const safety::DepthLimitExceeded& e) {
        err = std::string("fatal: ") + e.what();
        return nullptr;
    }
    if (!sr.ok) {
        err = "sema errors: ";
        for (auto& e : sr.errors) err += "line " + std::to_string(e.line) + ": " + e.msg + "; ";
        return nullptr;
    }

    TypedGlobalsLayout tgl = compute_typed_globals_layout(cs->prog, cs->struct_layouts);
    cs->gb_store.assign(size_t(tgl.total_size), 0);
    cs->gb.base = int64_t(cs->gb_store.data());
    {
        uint32_t gi = 0;
        for (auto& g : cs->prog.globals) {
            cs->gb.index[g.name] = gi++;
            cs->gb.types[g.name] = g.ty.get();
            cs->gb.offsets[g.name] = tgl.offsets[g.name];
            cs->gb.sizes[g.name] = tgl.sizes[g.name];
            if (!g.ns.empty()) {
                std::string qn = g.ns + "::" + g.name;
                cs->gb.index[qn] = cs->gb.index[g.name];
                cs->gb.types[qn] = cs->gb.types[g.name];
                cs->gb.offsets[qn] = cs->gb.offsets[g.name];
                cs->gb.sizes[qn] = cs->gb.sizes[g.name];
            }
        }
    }
    g_globals_for_codegen = nullptr;
    auto string_alloc_thunk = [](const char* bytes, int64_t len) -> int64_t {
        return ember::ext_string::alloc(std::string(bytes, size_t(len > 0 ? len : 0)));
    };
    GlobalInitCtx gic{cs->gb_store, cs->gb.index, cs->gb.types};
    gic.string_alloc_fn = string_alloc_thunk;
    gic.offsets = &cs->gb.offsets;
    gic.sizes = &cs->gb.sizes;
    gic.backing_offsets = &tgl.backing_offsets;
    gic.structs = &cs->struct_layouts;
    eval_global_initializers(cs->prog, gic);

    cs->table = std::make_unique<DispatchTable>(cs->prog.funcs.size());
    CodeGenCtx ctx;
    ctx.globals_base = cs->gb.base;
    ctx.globals_index = &cs->gb.index;
    ctx.globals_types = &cs->gb.types;
    ctx.globals_offsets = &cs->gb.offsets;
    ctx.dispatch_base = int64_t(cs->table->base());
    ctx.natives = &natives;
    ctx.script_slots = &cs->slots;
    ctx.structs = &cs->struct_layouts;
    ctx.registry_base = int64_t(cs->registry->base());
    // safe-execution (match run_ember_file: context reg + budgets + trap stub)
    ctx.trap_stub = reinterpret_cast<void*>(&ember_cli_trap);
    ctx.use_context_reg = true;
    ctx.emit_budget_checks = true;
    ctx.max_call_depth = 512;
    ctx.emit_depth_checks = true;
    std::vector<uint8_t> fn_allowlist = build_fn_allowlist(cs->slots, int(cs->slots.size()));
    ctx.fn_allowlist_base = int64_t(fn_allowlist.data());
    ctx.fn_slot_count = int64_t(cs->slots.size());

    cs->fns.reserve(cs->prog.funcs.size());
    try {
        for (auto& fn : cs->prog.funcs) {
            CompiledFn cf = compile_func(fn, ctx);
            if (!finalize(cf)) { err = "alloc_executable failed for " + fn.name; return nullptr; }
            cs->table->set(fn.slot, cf.entry);
            cs->entries[fn.name] = cf.entry;
            cs->fns.push_back(std::move(cf));
        }
    } catch (const safety::DepthLimitExceeded& e) {
        err = std::string("fatal: ") + e.what();
        return nullptr;
    }
    cs->ok = true;
    return cs;
}

// ---- Family C: `ember pipe` — dataflow pipeline runner ----
// (docs/ROADMAP.md Family C). Loads several modules (.ember compiled from
// source or .em loaded via the bundler), wires their named functions into a
// linear chain of single-arg i64->i64 stages, runs a stream of i64 values
// through it, and reports the transformed result (each value's final output +
// the sum). The exit code is the sum (mod 2^31) so a test harness can verify
// the pipeline by exit code. Exercises the bundler (load_em_file for .em
// modules) + module linking (the host wires module outputs to module inputs)
// + the compile path. The host orchestrates the chain (call stage[0](value) ->
// stage[1](result) -> ...) — no script-to-script cross-module calls needed for
// v1's linear chain, so each module is compiled standalone against its own
// dispatch table + registry.
//
// Config format (a simple text file, one directive per line, `#` comments):
//   module <alias> <path>      path is .ember (compiled) or .em (loaded)
//   stage  <alias>::<function> one stage per line, in chain order
//   input  <start> <count>     stream: start..start+count-1 (default: 1 5)
// Module paths are resolved relative to the config file's directory.
static int run_pipe_command(const std::string& config_path) {
    using namespace ember;
    if (!fs::exists(config_path)) {
        std::fprintf(stderr, "ember pipe: no such config: %s\n", config_path.c_str());
        return 2;
    }
    std::ifstream cf(config_path);
    if (!cf) { std::fprintf(stderr, "ember pipe: cannot read %s\n", config_path.c_str()); return 2; }
    std::string config_dir = fs::path(config_path).parent_path().string();

    struct StageDecl { std::string alias; std::string fn; };
    struct ModuleDecl { std::string alias; std::string path; };
    std::vector<ModuleDecl> modules;
    std::vector<StageDecl> stages;
    long long input_start = 1; long long input_count = 5;

    std::string line;
    while (std::getline(cf, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        line = line.substr(first);
        std::istringstream iss(line);
        std::string kw; iss >> kw;
        if (kw == "module") {
            ModuleDecl m; iss >> m.alias >> m.path;
            if (m.alias.empty() || m.path.empty()) {
                std::fprintf(stderr, "ember pipe: bad module line (need <alias> <path>)\n"); return 2;
            }
            modules.push_back(m);
        } else if (kw == "stage") {
            std::string ref; iss >> ref;
            auto sep = ref.find("::");
            if (sep == std::string::npos) {
                std::fprintf(stderr, "ember pipe: bad stage '%s' (expected alias::fn)\n", ref.c_str()); return 2;
            }
            StageDecl s; s.alias = ref.substr(0, sep); s.fn = ref.substr(sep + 2);
            if (s.alias.empty() || s.fn.empty()) {
                std::fprintf(stderr, "ember pipe: bad stage '%s'\n", ref.c_str()); return 2;
            }
            stages.push_back(s);
        } else if (kw == "input") {
            iss >> input_start >> input_count;
            if (input_count < 0) input_count = 0;
        } else if (!kw.empty()) {
            std::fprintf(stderr, "ember pipe: unknown directive '%s'\n", kw.c_str()); return 2;
        }
    }

    if (modules.empty()) { std::fprintf(stderr, "ember pipe: no modules declared\n"); return 2; }
    if (stages.empty()) { std::fprintf(stderr, "ember pipe: no stages declared\n"); return 2; }

    // resolve module paths relative to the config file's directory
    for (auto& m : modules) {
        if (!m.path.empty() && !(m.path[0]=='/'||m.path[0]=='\\') && (m.path.size()<2||m.path[1]!=':')) {
            m.path = fs::weakly_canonical(fs::path(config_dir) / m.path).string();
        }
    }

    // register standard bindings once (shared across all modules — the native
    // fn_ptrs are stable process-global C functions; rebuilding the map is
    // idempotent. Each module bakes the same fn_ptrs into its JIT code).
    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
    register_standard_bindings(natives, &overloads);

    // compile/load each module. .ember -> compile_script; .em -> load_em_file
    // (exercises the bundler). Both populate the entries map (name -> entry).
    std::vector<std::unique_ptr<CompiledScript>> mods;
    std::unordered_map<std::string, CompiledScript*> mod_by_alias;
    std::vector<LoadedModule> em_mods;  // keep .em modules alive (own their pages)
    em_mods.reserve(modules.size());     // no reallocation: dispatch buffer addrs stay stable
    mods.reserve(modules.size());
    for (auto& m : modules) {
        auto cs = std::make_unique<CompiledScript>();
        std::string ext = fs::path(m.path).extension().string();
        if (ext == ".em") {
            em_mods.emplace_back();
            std::string lerr;
            EmLoadPolicy em_policy{0u, true};
            if (!load_em_file(m.path.c_str(), em_mods.back(), &lerr, nullptr, &natives, nullptr, &em_policy)) {
                std::fprintf(stderr, "ember pipe: module '%s' (.em load) failed: %s\n", m.alias.c_str(), lerr.c_str());
                return 2;
            }
            LoadedModule& lm = em_mods.back();
            for (const auto& kv : lm.name_table) {
                if (kv.second < lm.dispatch.size())
                    cs->entries[kv.first] = lm.dispatch[kv.second];
            }
        } else {
            std::string cerr;
            auto compiled = compile_script(m.path, natives, overloads, cerr);
            if (!compiled) {
                std::fprintf(stderr, "ember pipe: module '%s' failed: %s\n", m.alias.c_str(), cerr.c_str());
                return 2;
            }
            cs = std::move(compiled);
        }
        mod_by_alias[m.alias] = cs.get();
        mods.push_back(std::move(cs));
    }

    // resolve stages -> entry pointers
    struct StageEntry { std::string alias, fn; void* entry; };
    std::vector<StageEntry> chain;
    for (auto& s : stages) {
        auto it = mod_by_alias.find(s.alias);
        if (it == mod_by_alias.end()) {
            std::fprintf(stderr, "ember pipe: stage '%s::%s' references unknown module '%s'\n",
                         s.alias.c_str(), s.fn.c_str(), s.alias.c_str()); return 2;
        }
        auto eit = it->second->entries.find(s.fn);
        if (eit == it->second->entries.end()) {
            std::fprintf(stderr, "ember pipe: stage function '%s::%s' not found\n", s.alias.c_str(), s.fn.c_str());
            return 2;
        }
        StageEntry se; se.alias = s.alias; se.fn = s.fn; se.entry = eit->second;
        chain.push_back(se);
    }

    std::printf("ember pipe: %zu stage(s), %lld value(s)\n", chain.size(), input_count);
    for (auto& se : chain)
        std::printf("  stage %s::%s\n", se.alias.c_str(), se.fn.c_str());

    // run the stream through the chain. One context_t for the whole run;
    // call_depth is reset before each top-level stage call (the raw B1 thunk
    // does not reset it). Traps are recoverable via the checkpoint (matches
    // run_ember_file's safe-execution model).
    context_t ectx;
    ectx.budget_remaining = 100000000;
    ectx.max_call_depth = 512;
    int64_t sum = 0;
    bool trapped = false;
    for (long long i = 0; i < input_count; ++i) {
        int64_t value = int64_t(input_start + i);
        int64_t out = value;
        ectx.has_checkpoint = true;
        if (setjmp(ectx.checkpoint)) {
            std::fprintf(stderr, "ember pipe: RUNTIME TRAP at value %lld: %s (%s)\n",
                         (long long)value, ectx.last_error.c_str(), trap_reason_str(ectx.last_trap));
            trapped = true; break;
        }
        for (auto& se : chain) {
            ectx.call_depth = 0;
            out = ember_call_i64(se.entry, &ectx, out);
        }
        ectx.has_checkpoint = false;
        std::printf("[%lld] -> %lld\n", (long long)value, (long long)out);
        sum += out;
    }
    ectx.has_checkpoint = false;
    if (trapped) return 70;
    std::printf("pipe result sum: %lld\n", (long long)sum);
    return int(uint64_t(sum) & 0x7fffffff);
}

// ---- Family C: `ember live` — live-coding/reload runner ----
// (docs/ROADMAP.md Family C). Compiles <file.ember>, runs @on_tick in a loop
// (printing each tick's return value so the output evolves), polls the file's
// mtime/content every poll_ms, and on change recompiles + swaps the module so the tick
// output reflects the new code. Uses the hot-reload API (HotReloadDomain +
// ExecutionGuard around each tick call — the documented recipe) for safe page
// retirement; the full recompile + swap is a v1 simplification (per-function
// reload_function would require source-span tracking, deferred). --tick-count N
// auto-stops (for tests/non-interactive); otherwise runs until 'q'.
//
// @entry (if present) is called once at load; if it returns <= 0 the module
// unloads and live exits (matches the --tick stay-loaded contract). If there
// is no @entry, live starts ticking immediately (a live-coding file can be
// just @on_tick).
struct LiveModule {
    std::unique_ptr<CompiledScript> script;   // the compiled state (fns, table, globals, prog)
    ember::HotReloadDomain domain;                    // ExecutionGuard around each tick call (hot-reload API)
    std::vector<ember::AnnotatedFn> ticks;            // @on_tick functions (resolved against script->slots)
    int tick_slot = -1;                        // the (first) @on_tick slot for fast per-tick call
};

static std::unique_ptr<LiveModule> make_live_module(
        const std::string& file_path,
        const std::unordered_map<std::string, ember::NativeSig>& natives,
        const ember::OpOverloadTable& overloads,
        std::string& err) {
    using namespace ember;
    auto compiled = compile_script(file_path, natives, overloads, err);
    if (!compiled) return nullptr;
    auto lm = std::make_unique<LiveModule>();
    lm->script = std::move(compiled);
    auto ticks = get_annotated_functions(lm->script->prog, "@on_tick");
    if (ticks.empty()) { err = "no @on_tick functions (nothing to live-code)"; return nullptr; }
    lm->ticks = std::move(ticks);
    lm->tick_slot = lm->ticks.front().slot;
    return lm;
}

static int run_live_command(const std::string& file_path,
                            int tick_interval_ms, int tick_max, int poll_ms) {
    using namespace ember;
    if (!fs::exists(file_path)) {
        std::fprintf(stderr, "ember live: no such file: %s\n", file_path.c_str());
        return 2;
    }
    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
    register_standard_bindings(natives, &overloads);

    std::string err;
    auto lm = make_live_module(file_path, natives, overloads, err);
    if (!lm) {
        std::fprintf(stderr, "ember live: compile failed: %s\n", err.c_str());
        return 2;
    }

    // @entry: call once if present. If it returns <= 0, the module unloads
    // (no live tick). If no @entry, start ticking immediately.
    auto entry_fns = get_annotated_functions(lm->script->prog, "@entry");
    if (!entry_fns.empty()) {
        void* entry = lm->script->table->get(entry_fns.front().slot);
        if (entry) {
            context_t ectx; ectx.budget_remaining = 100000000; ectx.max_call_depth = 512;
            ectx.has_checkpoint = true;
            int64_t entry_ret = 0;
            if (setjmp(ectx.checkpoint)) {
                std::fprintf(stderr, "ember live: @entry trap: %s (%s)\n",
                             ectx.last_error.c_str(), trap_reason_str(ectx.last_trap));
                return 70;
            }
            entry_ret = ember_call_void(entry, &ectx);
            ectx.has_checkpoint = false;
            if (entry_ret <= 0) {
                std::printf("ember live: @entry returned %lld (<= 0), module unloaded\n",
                            (long long)entry_ret);
                return 0;
            }
        }
    }

    std::printf("ember live: %s — %zu @on_tick fn(s), %dms tick, %dms poll. ",
                file_path.c_str(), lm->ticks.size(), tick_interval_ms, poll_ms);
    if (tick_max > 0) std::printf("--tick-count %d\n", tick_max);
    else std::printf("press 'q' to quit\n");

    // Change detection: compare the file CONTENT, not just the mtime. Windows
    // NTFS caches/delays last-write-time updates, so an mtime-only check misses
    // rapid edits (verified: a 60ms-gap overwrite can leave mtime unchanged).
    // Reading the (small) file each poll and string-comparing is robust and
    // cheap. `last_source` is the source the current module was compiled from.
    std::string last_source = read_file(file_path.c_str());
    uint64_t tick_count = 0;
    int exit_code = 0;
    bool stopped = false;
    auto last_poll = std::chrono::steady_clock::now();

    while (!stopped) {
        if (tick_max > 0 && tick_count >= uint64_t(tick_max)) break;
        // ---- run one tick under an ExecutionGuard (hot-reload API recipe) ----
        int64_t tick_ret = 0;
        bool tick_trapped = false;
        {
            auto guard = lm->domain.guard();
            context_t ectx; ectx.budget_remaining = 100000000; ectx.max_call_depth = 512;
            ectx.call_depth = 0;
            ectx.has_checkpoint = true;
            if (setjmp(ectx.checkpoint)) {
                std::fprintf(stderr, "ember live: tick %llu trap: %s (%s)\n",
                             (unsigned long long)tick_count, ectx.last_error.c_str(),
                             trap_reason_str(ectx.last_trap));
                tick_trapped = true; exit_code = 70;
            } else {
                void* f = lm->script->table->get(lm->tick_slot);
                if (f) tick_ret = ember_call_void(f, &ectx);
            }
            ectx.has_checkpoint = false;
        }
        ++tick_count;
        std::printf("tick %llu: %lld\n", (unsigned long long)tick_count, (long long)tick_ret);
        std::fflush(stdout);
        if (tick_trapped) break;

        // ---- poll the file content every poll_ms; reload on change ----
        // (mtime is unreliable on Windows for short intervals — see the note
        // above last_source. Content comparison is the real detector. The poll
        // is throttled to poll_ms so a fast tick loop doesn't re-read the file
        // every tick.)
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_poll).count() >= poll_ms) {
            last_poll = now;
            std::string cur_source = read_file(file_path.c_str());
            if (cur_source != last_source) {
                last_source = cur_source;
                std::printf("ember live: file changed, recompiling...\n");
                std::string rerr;
                auto new_lm = make_live_module(file_path, natives, overloads, rerr);
                if (new_lm) {
                    // re-run @entry of the new module if present (a reload is a fresh
                    // load: @entry re-fires so globals/state reseed). If it returns
                    // <= 0, keep the old module (the new one chose to unload).
                    auto new_entry = get_annotated_functions(new_lm->script->prog, "@entry");
                    bool keep_new = true;
                    if (!new_entry.empty()) {
                        void* e = new_lm->script->table->get(new_entry.front().slot);
                        if (e) {
                            context_t ectx; ectx.budget_remaining = 100000000; ectx.max_call_depth = 512;
                            ectx.has_checkpoint = true;
                            int64_t er = 0;
                            if (setjmp(ectx.checkpoint)) {
                                std::fprintf(stderr, "ember live: reloaded @entry trap: %s (%s)\n",
                                             ectx.last_error.c_str(), trap_reason_str(ectx.last_trap));
                                keep_new = false;
                            } else {
                                er = ember_call_void(e, &ectx);
                            }
                            ectx.has_checkpoint = false;
                            if (keep_new && er <= 0) keep_new = false;
                        }
                    }
                    if (keep_new) {
                        lm = std::move(new_lm);  // old module destroyed (frees its pages; no active guard)
                        std::printf("ember live: reloaded (epoch %llu)\n",
                                    (unsigned long long)lm->domain.epoch());
                    } else {
                        std::printf("ember live: reloaded module @entry returned <= 0; keeping old module\n");
                    }
                } else {
                    std::fprintf(stderr, "ember live: recompile failed (%s); keeping old module\n", rerr.c_str());
                }
            }
        }

        // ---- keybind (non-interactive: --tick-count auto-stops above) ----
        if (tick_max <= 0 && _kbhit()) { int c = _getch(); if (c == 'q' || c == 'Q') stopped = true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(tick_interval_ms > 0 ? tick_interval_ms : 16));
    }
    std::printf("ember live: stopped after %llu ticks\n", (unsigned long long)tick_count);
    return exit_code;
}

int main(int argc, char** argv) {
    using namespace ember;

    // The bundler owns its argument grammar. Dispatch before the general CLI
    // parser so its positional output and --stub/--permissions options are not
    // reimplemented here. argv[0] becomes "bundle", matching a normal main().
    if (argc > 1 && std::string(argv[1]) == "bundle")
        return ember_bundle::command(argc - 1, argv + 1);

    // ---- arg parse ----
    std::string action;
    std::string file;
    std::string fn_name = "main";
    bool dump = false;
    std::string emit_em_path;   // v0.5: --emit-em <out> pre-compiles to a .em bundle
    std::string load_em_path;
    std::string passes_spec;     // Stage C: --passes <spec> run IR optimization passes
    std::string profile;         // Red 8: --profile <name> (alias --pass-profile)
    uint64_t pass_seed = 0;      // Red 8: --pass-seed <u64> fixed root seed
    bool seed_set = false;       // Red 8: --pass-seed was given (so 0 travels)
    bool tick_mode = false;     // v0.6: --tick runs @on_tick fns on a thread until a keybind
    int tick_interval_ms = 16;  // v0.6: --tick-interval (default ~60fps)
    int tick_max = 0;           // v0.6: --tick-count N auto-stop after N ticks (0 = until keybind; for tests/non-interactive)
    bool bench_mode = false;    // Family A: `ember bench` — microbenchmark the entry fn
    int bench_iters = 20;       // --iters N (measured iterations)
    int bench_warmup = 5;       // --warmup N (untimed warmup iterations)
    bool ffi_mode = false;      // --ffi: grant PERM_FFI to sema so I/O natives (print/file/path) are callable
    bool gc_env = false;        // --gc-env: allocate lambda envs on the tracing GC heap (#20)
    std::string test_dir;        // Family A: `ember test [dir]` (default tests/lang)
    int poll_ms = 500;           // Family C: `ember live` file-content poll interval (default 500ms)
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--fn") {
            if (++i >= argc) { std::fprintf(stderr, "ember: --fn needs a name\n"); return 2; }
            fn_name = argv[i];
        } else if (a == "--dump") {
            dump = true;
        } else if (a == "--passes") {
            if (++i >= argc) { std::fprintf(stderr, "ember: --passes needs a spec (e.g. constprop,cse,dce)\n"); return 2; }
            passes_spec = argv[i];
        } else if (a == "--profile" || a == "--pass-profile") {
            // Red 8: ordinary pipeline profile (light/balanced/heavy).
            // --pass-profile is a documented alias. Missing value -> exit 2;
            // an unknown value is rejected later (run_ember_file resolves it
            // against the built-in registry).
            if (++i >= argc) {
                std::fprintf(stderr, "ember: %s needs a profile name (light, balanced, or heavy)\n",
                             a.c_str());
                return 2;
            }
            profile = argv[i];
        } else if (a == "--pass-seed") {
            // Red 8: fixed 64-bit root seed for the profile's configured obf
            // factories. Decimal or 0x-prefixed hex; 0 and UINT64_MAX accepted;
            // malformed/overflow/sign rejected with a structured diagnostic.
            if (++i >= argc) {
                std::fprintf(stderr, "ember: --pass-seed needs a value (decimal or 0x-hex u64)\n");
                return 2;
            }
            std::string seed_err;
            if (!parse_pass_seed(argv[i], &pass_seed, &seed_err)) {
                std::fprintf(stderr, "ember: --pass-seed: %s\n", seed_err.c_str());
                return 2;
            }
            seed_set = true;
        } else if (a == "--emit-em") {
            if (++i >= argc) { std::fprintf(stderr, "ember: --emit-em needs a path\n"); return 2; }
            emit_em_path = argv[i];
        } else if (a == "--gc-env") {
            // #20: allocate lambda closure envs on the tracing GC heap
            // (ext_gc) instead of as stack-frame temps, so lambdas can outlive
            // their creating frame. Off by default (the lang suite + opt gate
            // use the stack-env path). The gc natives are always registered;
            // this flag only flips the codegen backend.
            gc_env = true;
        } else if (a == "--load-em") {
            if (++i >= argc) { std::fprintf(stderr, "ember: --load-em needs a path\n"); return 2; }
            load_em_path = argv[i];
        } else if (a == "--tick") {
            tick_mode = true;
        } else if (a == "--tick-interval") {
            if (++i >= argc) { std::fprintf(stderr, "ember: --tick-interval needs ms\n"); return 2; }
            tick_interval_ms = std::atoi(argv[i]);
            if (tick_interval_ms <= 0) tick_interval_ms = 16;
        } else if (a == "--tick-count") {
            if (++i >= argc) { std::fprintf(stderr, "ember: --tick-count needs N\n"); return 2; }
            tick_max = std::atoi(argv[i]);
        } else if (a == "--poll-ms") {
            // Family C: `ember live` file-content poll interval. Smaller = more
            // responsive reload (tests use a small value); default 500ms. The poll
            // compares file content (mtime is unreliable on Windows for short gaps).
            if (++i >= argc) { std::fprintf(stderr, "ember: --poll-ms needs MS\n"); return 2; }
            poll_ms = std::atoi(argv[i]); if (poll_ms < 0) poll_ms = 500;
        } else if (a == "--iters") {
            if (++i >= argc) { std::fprintf(stderr, "ember: --iters needs N\n"); return 2; }
            bench_iters = std::atoi(argv[i]); if (bench_iters < 1) bench_iters = 20;
        } else if (a == "--warmup") {
            if (++i >= argc) { std::fprintf(stderr, "ember: --warmup needs N\n"); return 2; }
            bench_warmup = std::atoi(argv[i]); if (bench_warmup < 0) bench_warmup = 0;
        } else if (a == "--ffi" || a == "--allow-io") {
            // Grant PERM_FFI to sema so the ext_io natives (print/println/
            // file_read_bytes/path_*/...) registered in register_standard_bindings
            // are callable. Without --ffi, sema is called with 0 and every I/O
            // call site is rejected at compile time (security by default -- the
            // ext_io natives are registered but not callable).
            ffi_mode = true;
        } else if (a == "-h" || a == "--help") {
            usage(stdout); return 0;
        } else if (a.size() > 0 && a[0] == '-') {
            std::fprintf(stderr, "ember: unknown option '%s'\n", a.c_str()); return 2;
        } else if (action.empty()) {
            action = a;                 // first positional = action ("run"/"emit-em"/"bench"/"test")
        } else if (action == "test" && test_dir.empty()) {
            test_dir = a;               // `ember test <dir>` — optional directory positional
        } else if (action == "pipe" && file.empty()) {
            file = a;                   // `ember pipe <config>` — config path positional
        } else if (action == "live" && file.empty()) {
            file = a;                   // `ember live <file.ember>` — script path positional
        } else if (file.empty()) {
            file = a;                    // second positional = input path
        } else if (action == "emit-em" && emit_em_path.empty()) {
            emit_em_path = a;            // third positional = required output path
        } else {
            std::fprintf(stderr, "ember: unexpected argument '%s'\n", a.c_str()); return 2;
        }
    }
    if (action == "test") {
        return run_test_command(test_dir.empty() ? "tests/lang" : test_dir);
    }
    if (action == "pipe") {
        // `ember pipe <config>` — dataflow pipeline runner (Family C).
        if (file.empty()) { std::fprintf(stderr, "ember: pipe needs <config>\n"); usage(stderr); return 2; }
        return run_pipe_command(file);
    }
    if (action == "live") {
        // `ember live <file.ember>` — live-coding/reload runner (Family C).
        // --tick is implied; --tick-count/--tick-interval/--poll-ms apply.
        if (file.empty()) { std::fprintf(stderr, "ember: live needs <file.ember>\n"); usage(stderr); return 2; }
        return run_live_command(file, tick_interval_ms, tick_max, poll_ms);
    }
    if (action != "run" && action != "emit-em" && action != "bench") { usage(stderr); return 2; }
    if (action == "bench") { bench_mode = true; tick_mode = false; }
    if (file.empty() && load_em_path.empty()) { std::fprintf(stderr, "ember: missing <input.ember>\n"); usage(stderr); return 2; }
    if (action == "emit-em" && emit_em_path.empty()) {
        std::fprintf(stderr, "ember: emit-em needs <output.em>\n"); usage(stderr); return 2;
    }
    if (!load_em_path.empty()) {
        std::unordered_map<std::string, NativeSig> load_natives;
        register_standard_bindings(load_natives);
        LoadedModule loaded; std::string lerr;
        EmLoadPolicy em_policy{ffi_mode ? PERM_FFI : 0u, true};
        if(!load_em_file(load_em_path.c_str(),loaded,&lerr,nullptr,&load_natives,nullptr,&em_policy)){std::fprintf(stderr,"ember: load failed: %s\n",lerr.c_str());return 2;}
        void* entry=loaded.entry_by_name(fn_name.c_str()); if(!entry)entry=loaded.entry();
        if(!entry){std::fprintf(stderr,"ember: loaded entry '%s' not found\n",fn_name.c_str());return 2;}
        uint32_t selected_slot=loaded.entry_slot;
        for(const auto& item:loaded.name_table)if(item.first==fn_name){selected_slot=item.second;break;}
        bool is_void=selected_slot<loaded.signatures_by_slot.size()&&loaded.signatures_by_slot[selected_slot].ret.is_void();
        // SAFETY FAILSAFE: --load-em previously called the loaded entry via the
        // raw call_i64_i64() with NO context, NO instruction budget, NO call-
        // depth limit, NO trap checkpoint, and NO memory ceiling. A loaded .em
        // with an infinite loop or unbounded allocation would run forever and
        // freeze the host. Now we route through the context-aware call path
        // with a finite budget + checkpoint + depth limit, so safety-on .em
        // code traps on budget/depth exhaustion instead of running forever.
        // The GC/JIT RSS failsafe (safety::check_memory_limit) catches the
        // unbounded-allocation case regardless of whether the .em has budget
        // checks baked in.
        ember::context_t ectx;
        ectx.budget_remaining = 100000000;  // 100M instruction budget (same as normal run)
        ectx.max_call_depth = 512;
        ectx.has_checkpoint = (setjmp(ectx.checkpoint) == 0);
        if (!ectx.has_checkpoint) {
            // We arrived here via a trap longjmp — recoverable exit.
            std::fprintf(stderr, "ember: loaded .em trapped: %s\n", ectx.last_error.c_str());
            return 70;
        }
        safety::check_memory_limit();  // pre-flight RSS check before executing untrusted .em
        int64_t result = is_void
            ? ember::ember_call_void(entry, &ectx)
            : ember::ember_call_i64(entry, &ectx, 0);
        return is_void ? 0 : int(result);
    }

    // ---- compile + run via the shared helper (extracted from main) ----
    // main() now just parses args + dispatches; the full
    // read→imports→lex→parse→sema→codegen→finalize→call→cleanup pipeline
    // lives in run_ember_file() above, shared by `run`/`bench`/`--emit-em`/
    // `--tick`/`test`.
    RunOptions opts;
    opts.fn_name = fn_name;
    opts.dump = dump;
    opts.emit_em_path = emit_em_path;
    opts.passes_spec = passes_spec;
    opts.profile = profile;
    opts.pass_seed = pass_seed;
    opts.seed_set = seed_set;
    opts.tick_mode = tick_mode;
    opts.tick_interval_ms = tick_interval_ms;
    opts.tick_max = tick_max;
    opts.bench_mode = bench_mode;
    opts.bench_iters = bench_iters;
    opts.bench_warmup = bench_warmup;
    opts.ffi_mode = ffi_mode;
    opts.gc_env = gc_env;

    RunResult result = run_ember_file(file, opts);
    return result.exit_code;
}
