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
#include "../src/module_registry.hpp" // v0.5 live modules (ModuleRegistry)
#include "../src/module_linker.hpp"  // v0.5 live modules (link_em_file, build_*_exports)
#include "../src/em_loader.hpp"      // v0.5 live modules (LoadedModule, for linked .em modules)
#include "../src/em_file.hpp"        // v0.5 --emit-em (EmModule/EmFunctionRecord)
#include "../src/em_writer.hpp"       // v0.5 --emit-em (write_em_file)

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"
#include "ext_sync.hpp"
#include "ext_lifecycle.hpp"
#include "ext_opt.hpp"        // Stage C: register_passes (IR optimization passes)
#include "../src/ember_pass.hpp"       // Stage C: EmberPassManager
#include "../src/ember_pass_registry.hpp" // Stage C: EmberPassRegistry
#include "../src/ember_pass_pipeline.hpp" // Stage C: build_pipeline_from_string

#include <cstdio>
#include <csetjmp>   // setjmp/longjmp (v0.4 safe-execution checkpoint)
#include <thread>    // v0.6 --tick tick thread
#include <atomic>     // v0.6 --tick stop flag
#include <chrono>     // v0.6 --tick interval
#include <conio.h>     // v0.6 --tick keybind (Windows _kbhit/_getch)
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

static std::string read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
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
            // __builtin_longjmp (not std::longjmp): restores saved rsp/rbp/ip
            // WITHOUT walking SEH/unwind tables. JIT'd frames have no .pdata,
            // so std::longjmp's table walk faults; the builtin is a direct
            // register restore (matches the spec's "directly, not libc setjmp/longjmp").
            __builtin_longjmp(ctx->checkpoint, 1);
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
    ember::ext_sync::register_natives(natives); ember::ext_lifecycle::register_natives(natives);
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
        "  ember emit-em <input.ember> <output.em>\n"
        "  ember run --load-em <input.em> [--fn NAME]\n"
        "  ember bench <input.ember> [--fn NAME] [--iters N] [--warmup N]\n"
        "\n"
        "  run                 compile and call the entry function\n"
        "  emit-em             compile without running and write <output.em>\n"
        "  bench               microbenchmark the entry fn: warmup + N timed iters;\n"
        "                      print min/median/mean/p99/stddev + return value +\n"
        "                      machine/compiler metadata (closes the audit's\n"
        "                      benchmark-methodology gap)\n"
        "  --fn NAME           entry function (default: main)\n"
        "  --dump              print each compiled fn: name, slot, byte size, reloc count\n"
        "  --emit-em PATH      run-mode precompile output; compile and write without running\n"
        "  --tick              run @on_tick and dynamic routines on a tick thread\n"
        "  --tick-count N      stop automatically after N ticks (default: keypress)\n"
        "  --tick-interval MS  tick interval in milliseconds (default: 16)\n");
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

int main(int argc, char** argv) {
    using namespace ember;

    // ---- arg parse ----
    std::string action;
    std::string file;
    std::string fn_name = "main";
    bool dump = false;
    std::string emit_em_path;   // v0.5: --emit-em <out> pre-compiles to a .em bundle
    std::string load_em_path;
    std::string passes_spec;     // Stage C: --passes <spec> run IR optimization passes
    bool tick_mode = false;     // v0.6: --tick runs @on_tick fns on a thread until a keybind
    int tick_interval_ms = 16;  // v0.6: --tick-interval (default ~60fps)
    int tick_max = 0;           // v0.6: --tick-count N auto-stop after N ticks (0 = until keybind; for tests/non-interactive)
    bool bench_mode = false;    // Family A: `ember bench` — microbenchmark the entry fn
    int bench_iters = 20;       // --iters N (measured iterations)
    int bench_warmup = 5;       // --warmup N (untimed warmup iterations)
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
        } else if (a == "--emit-em") {
            if (++i >= argc) { std::fprintf(stderr, "ember: --emit-em needs a path\n"); return 2; }
            emit_em_path = argv[i];
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
        } else if (a == "--iters") {
            if (++i >= argc) { std::fprintf(stderr, "ember: --iters needs N\n"); return 2; }
            bench_iters = std::atoi(argv[i]); if (bench_iters < 1) bench_iters = 20;
        } else if (a == "--warmup") {
            if (++i >= argc) { std::fprintf(stderr, "ember: --warmup needs N\n"); return 2; }
            bench_warmup = std::atoi(argv[i]); if (bench_warmup < 0) bench_warmup = 0;
        } else if (a == "-h" || a == "--help") {
            usage(stdout); return 0;
        } else if (a.size() > 0 && a[0] == '-') {
            std::fprintf(stderr, "ember: unknown option '%s'\n", a.c_str()); return 2;
        } else if (action.empty()) {
            action = a;                 // first positional = action ("run")
        } else if (file.empty()) {
            file = a;                    // second positional = input path
        } else if (action == "emit-em" && emit_em_path.empty()) {
            emit_em_path = a;            // third positional = required output path
        } else {
            std::fprintf(stderr, "ember: unexpected argument '%s'\n", a.c_str()); return 2;
        }
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
        if(!load_em_file(load_em_path.c_str(),loaded,&lerr,nullptr,&load_natives)){std::fprintf(stderr,"ember: load failed: %s\n",lerr.c_str());return 2;}
        void* entry=loaded.entry_by_name(fn_name.c_str()); if(!entry)entry=loaded.entry();
        if(!entry){std::fprintf(stderr,"ember: loaded entry '%s' not found\n",fn_name.c_str());return 2;}
        uint32_t selected_slot=loaded.entry_slot;
        for(const auto& item:loaded.name_table)if(item.first==fn_name){selected_slot=item.second;break;}
        bool is_void=selected_slot<loaded.signatures_by_slot.size()&&loaded.signatures_by_slot[selected_slot].ret.is_void();
        int64_t result=call_i64_i64(entry); return is_void?0:int(result);
    }
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

    // ---- register ALL eight extensions: natives + overloads ----
    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
    register_standard_bindings(natives,&overloads);
    // array, math, sync, and lifecycle expose natives but no operators.

    // ---- struct layouts + string key + sema ----
    auto struct_layouts = build_struct_layouts(pr.program);
    // String encryption is ON by default (key=0xA5). String literals bake
    // XOR-encrypted in rodata; codegen decrypts INLINE into a compiler-hidden
    // temp frame slot at each use site (see codegen's StringLit eval case /
    // alloc_str_temp), so the plaintext is TRANSIENT — it lives only on the
    // stack for the expression's lifetime and is reclaimed when the frame is
    // torn down. No heap, no host native call, no leak: the encrypted form
    // alone lives in rodata, and raw strings never appear in the JIT'd
    // executable memory. This is pure codegen — the host need only set a
    // nonzero key before sema; there is no __str_decrypt native to register
    // (the old heap-allocating native contract was removed when inline
    // stack-XOR landed). A fixed key (not random) keeps test output
    // deterministic.
    pr.program.string_xor_key = 0xA5;

    // ---- v0.5 live-module link resolution (docs/MODULES.md §5) ----
    // Resolve each `link "..." as alias;` directive: a .em target is loaded +
    // registered; a bare name links to an already-registered module (the host
    // must have registered it). Build the ModuleExportTable sema resolves
    // `mod::fn` against. Unresolvable links are deferred (sema marks those
    // calls cross_module_unresolved -> trap at runtime, not a hard error).
    ModuleRegistry registry(64);  // capacity 64 modules per process (generous)
    std::vector<LoadedModule> linked_ems;  // own the .em modules (keep alive for registry's lifetime)
    linked_ems.reserve(pr.program.links.size());  // no realloc: registry holds dispatch.data() ptrs (dangling if the vector grows)
    ModuleExportTable module_exports;
    for (const auto& ld : pr.program.links) {
        if (ld.is_file) {
            // resolve a relative .em path against the importing file's directory
            // (matches `import`'s textual resolver convention).
            std::string path = ld.target;
            if (!path.empty() && !(path[0]=='/' || path[0]=='\\') && (path.size()<2 || path[1]!=':')) {
                fs::path p = fs::path(base_dir) / path;
                path = fs::weakly_canonical(p).string();
            }
            linked_ems.emplace_back();
            std::string lerr;
            if (!link_em_file(registry, path.c_str(), ld.alias, linked_ems.back(), &lerr, &natives)) {
                std::fprintf(stderr, "ember: link '%s' failed: %s\n", ld.target.c_str(), lerr.c_str());
                return 2;
            }
            uint32_t id = registry.find_by_name(ld.alias);
            add_exports(module_exports, ld.alias, build_em_exports(linked_ems.back(), id));
        } else {
            // `link "foo" as foo;` -> link to an already-registered module.
            // (In the standalone CLI nothing pre-registers, so this is unresolved
            // unless the host wired it; sema will mark calls to it as deferred traps.)
            uint32_t id = registry.find_by_name(ld.target);
            if (id != UINT32_MAX) {
                // already registered by a prior link (e.g. a .em that exports it);
                // expose under the alias. Exports aren't available without a LoadedModule/Program
                // here, so this form is host-driven in practice; leave deferred.
            }
            // else: unresolved — sema marks mod::fn calls as deferred traps.
        }
    }

    auto sr = sema(pr.program, natives, slots, 0, &overloads, &struct_layouts, &module_exports);
    if (!sr.ok) {
        std::fprintf(stderr, "ember: sema errors (%zu):\n", sr.errors.size());
        for (auto& e : sr.errors) std::fprintf(stderr, "  line %u: %s\n", e.line, e.msg.c_str());
        return 2;
    }
    // Non-fatal deprecation warnings (e.g. `auto` -> use `let x = expr;`).
    if (!sr.warnings.empty()) {
        std::fprintf(stderr, "ember: sema warnings (%zu):\n", sr.warnings.size());
        for (auto& w : sr.warnings) std::fprintf(stderr, "  line %u: %s\n", w.line, w.msg.c_str());
    }

    // ---- globals block (TYPED layout, chunk c3; sized from declared globals) ----
    // Each global lands at a per-global byte OFFSET (8-aligned): scalars 8 bytes,
    // structs StructLayout::size, fixed arrays elem_size*array_len, slices 16
    // bytes ({ptr,len}) with their backing array appended after all primary
    // slots. The block is sized to the SUM of all aligned (slot + backing)
    // sizes. `gb.index` is kept as the legacy flat slot index (offset == i*8
    // for an all-scalar set, so scalar-only hosts keep working); `gb.offsets`/
    // `gb.sizes` are the typed c3 layout codegen + the initializer folder use.
    GlobalsBlock gb;
    TypedGlobalsLayout tgl = compute_typed_globals_layout(pr.program, struct_layouts);
    {
        uint32_t gi = 0;
        for (auto& g : pr.program.globals) {
            gb.index[g.name] = gi++;
            gb.types[g.name] = g.ty.get();
            gb.offsets[g.name] = tgl.offsets[g.name];
            gb.sizes[g.name] = tgl.sizes[g.name];
        }
    }
    std::vector<uint8_t> gb_store(size_t(tgl.total_size), 0);
    gb.base = int64_t(gb_store.data());
    // v1.0 thread-safety: thread globals index/types/offsets through CodeGenCtx
    // instead of the process-wide g_globals_for_codegen pointer (parallel-
    // compile-safe).
    g_globals_for_codegen = nullptr;   // not consulted — ctx.globals_index used
    // v1.0: seed const global initializers into the block (global g = 10; starts
    // at 10, not 0). Non-const inits (g = some_fn();) stay zero — a host or
    // @entry seeds those. The v1.0 integration found this gap; see
    // docs/v0.6_INTEGRATION_NOTES.md.
    // v1.0+: seed `string`-typed globals' literal initializers too, by
    // materializing the literal into a live string handle via the string
    // extension (the extension owns the handle store). Before this, a
    // `global g : string = "...";` compiled but its handle was never baked —
    // the first read saw a null handle (string_length 0). The compiler demo
    // surfaced this; the fix is the optional string_alloc_fn hook in
    // GlobalInitCtx.
    // c3: seed AGGREGATE global initializers too (struct StructLit / array
    // ArrayLit / slice ArrayLit) by const-folding fields/elements into the
    // typed block at each global's offset (struct/fixed-array) or its backing
    // region (slice, with the {ptr,len} slot seeded with a RELATIVE ptr so
    // the baked bytes round-trip through .em without loader fixup).
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
    ctx.globals_index = &gb.index;      // v1.0: parallel-compile-safe globals resolution
    ctx.globals_types = &gb.types;
    ctx.globals_offsets = &gb.offsets; // c3: typed byte offsets (aggregate globals)
    ctx.dispatch_base = int64_t(table.base());
    ctx.natives = &natives;
    ctx.script_slots = &slots;
    ctx.structs = &struct_layouts;
    // v0.5 cross-module: bake the registry base into kind-2 call sites. Also
    // register THIS module so a linked .em could call back into it (bidirectional).
    ctx.registry_base = int64_t(registry.base());
    std::string reg_err;
    uint32_t self_id = registry.register_module("__main__", table.base(), &reg_err);
    (void)self_id;  // registered so others can link "__main__"; not referenced here
    // String encryption needs no ctx field: it is pure codegen (inline
    // stack-XOR into a temp frame slot, see the StringLit eval case).

    // ---- v0.4/v1.0 safe-execution context (docs/spec/SAFETY_AND_SANDBOX.md §2-§4) ----
    // The CLI runs untrusted .ember input, so enable BOTH budgets + route
    // all traps through the stub (longjmp to a checkpoint). Trusted-tool
    // hosts leave these off/null for zero JIT overhead (see context.hpp).
    //
    // v1.0 thread-safety (Option D + B1): compile with use_context_reg=true so
    // the budget/depth/trap reads go through r14 (the per-call context register)
    // instead of a baked pointer. This FIXES the --tick bug: the tick thread can
    // use its OWN context_t (passed via ember_call_void -> r14) without corrupting
    // the main thread's context. One compiled body serves both threads' contexts.
    context_t ectx;
    ectx.budget_remaining = 100000000;   // 100M coarse instructions per call (~ generous)
    ectx.max_call_depth = 512;
    ectx.has_checkpoint = false;
    if (emit_em_path.empty()) ctx.trap_stub = reinterpret_cast<void*>(&ember_cli_trap);
    // B1: budget/depth/trap_ctx are NOT baked — read through r14 per call. So we
    // do NOT set ctx.budget_ptr/depth_ptr/trap_ctx; the JIT'd code loads them from
    // [r14 + context_offsets::*] at each check.
    ctx.use_context_reg = emit_em_path.empty();
    ctx.emit_budget_checks = emit_em_path.empty();
    ctx.max_call_depth = ectx.max_call_depth;
    ctx.emit_depth_checks = emit_em_path.empty();
    // v1.0 Tier 2: build + wire the fn allowlist so a CLI-loaded script that uses
    // &fn / handle(args) works (the guard validates runtime handles; no-op when
    // the script doesn't use function refs — fn_slot_count reflects the slots).
    std::vector<uint8_t> fn_allowlist = build_fn_allowlist(slots, int(slots.size()));
    if (emit_em_path.empty()) {
        ctx.fn_allowlist_base = int64_t(fn_allowlist.data());
        ctx.fn_slot_count = int64_t(slots.size());
    }

    // Stage C: --passes <spec> — build an IR optimization pass pipeline from
    // the registry + the user's pipeline string. Only effective when the IR
    // backend is on (enable_ir_backend). The IR backend is on by default when
    // --passes is given (the passes operate on the ThinFunction IR).
    EmberPassRegistry pass_reg;
    ext_opt::register_passes(pass_reg);
    EmberPassManager pass_pm;
    if (!passes_spec.empty()) {
        std::string pass_err;
        if (!build_pipeline_from_string(passes_spec, pass_reg, pass_pm, &pass_err)) {
            std::fprintf(stderr, "ember: --passes: %s\n", pass_err.c_str());
            return 2;
        }
        ctx.pass_manager = &pass_pm;
        ctx.enable_ir_backend = true;  // passes need the IR path
    }

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

    // ---- v0.5 --emit-em: pre-compile the parsed module to a .em bundle ----
    // (docs/BUNDLING_AND_EM_MODULES.md). Serializes the JIT'd per-function bytes +
    // relocs + the globals block + a name->slot table. The resulting .em is
    // loadable by ember_cli's `link "x.em"` path or any host using em_loader.
    if (!emit_em_path.empty()) {
        EmModule mod;
        mod.functions.reserve(fns.size());
        // Serialize the already-evaluated initial-value block. Constructing a
        // fresh zero-filled block here would silently discard const globals.
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
        // entry slot = the @entry fn if present, else `main` if present.
        uint32_t entry_slot = EM_NO_ENTRY;
        for (const auto& fn : pr.program.funcs) for (const auto& a : fn.annotations) if (a.name == "entry") { entry_slot = uint32_t(fn.slot); break; }
        if (entry_slot == EM_NO_ENTRY) { auto sit = slots.find("main"); if (sit != slots.end()) entry_slot = uint32_t(sit->second); }
        mod.entry_slot = entry_slot;
        mod.name_table.reserve(pr.program.funcs.size());
        // F1 visibility (docs/spec/SPEC_AUDIT_2026-07-10.md F1): the .em name
        // directory IS the module's EXPORT TABLE from v3. Publish only the
        // `is_exported` fns (bare `fn` is exported by default; `priv fn` is
        // hidden). A `priv fn` is still serialized above (its code/relocs are in
        // mod.functions, occupying its dispatch slot for intra-module calls) -
        // it is simply absent from the name directory, so other modules cannot
        // resolve it cross-module.
        for (const auto& fn : pr.program.funcs)
            if (fn.is_exported)
                mod.name_table.push_back({fn.name, uint32_t(fn.slot)});
        std::string werr;
        if (!write_em_file(mod, emit_em_path.c_str(), &werr)) {
            std::fprintf(stderr, "ember: --emit-em write failed: %s\n", werr.c_str());
            return 2;
        }
        std::printf("ember: wrote %s (%zu fns, %zu globals block bytes, entry slot %u)\n",
                    emit_em_path.c_str(), mod.functions.size(), mod.globals.size(), mod.entry_slot);
        return 0;  // pre-compile mode: do not run
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
    //
    // v0.4: the call runs under a setjmp checkpoint. Any trap (bounds,
    // budget exhaustion, stack overflow, @obf_keyed mismatch) longjmps back
    // here instead of crashing the process (REDSHELL V6-DoS / V7).
    int exit_code = 0;
    int64_t entry_ret = 0;   // SIGNED @entry return (lifecycle signal); exit_code is the clamped process code
    bool returns_void = entry_decl && entry_decl->ret && entry_decl->ret->is_void();
    ectx.has_checkpoint = true;

    // ---- Family A: `ember bench` — microbenchmark the entry fn ----
    // Warmup (untimed) then N timed iterations, each under its OWN fresh
    // checkpoint so a trap in one iter stops the bench cleanly (not the process).
    // Reports min/median/mean/p99/stddev over the timed iters + the return value
    // + machine/compiler metadata. Closes the 07-09 §6.1/§6.3 benchmark-methodology
    // gap (was: one mean, no variance/CI, no machine metadata, report written as
    // a test side-effect). bench writes to stdout ON REQUEST only, never as a side-effect.
    if (bench_mode) {
        // warmup
        for (int w = 0; w < bench_warmup; ++w) {
            if (__builtin_setjmp(ectx.checkpoint)) { std::fprintf(stderr, "ember: bench warmup trap: %s\n", ectx.last_error.c_str()); exit_code = 70; goto bench_done; }
            if (!returns_void) entry_ret = ember::ember_call_void(entry, &ectx);
            else              ember::ember_call_void(entry, &ectx);
        }
        // measured iterations
        std::vector<double> ns; ns.reserve(size_t(bench_iters));
        bool bench_trapped = false;
        for (int it = 0; it < bench_iters; ++it) {
            if (__builtin_setjmp(ectx.checkpoint)) { std::fprintf(stderr, "ember: bench iter %d trap: %s\n", it, ectx.last_error.c_str()); bench_trapped = true; break; }
            auto t0 = std::chrono::steady_clock::now();
            if (!returns_void) entry_ret = ember::ember_call_void(entry, &ectx);
            else              (void)ember::ember_call_void(entry, &ectx);
            auto t1 = std::chrono::steady_clock::now();
            ns.push_back(double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }
        if (bench_trapped) { exit_code = 70; goto bench_done; }
        // statistics
        std::sort(ns.begin(), ns.end());
        double mn = ns.front(), mx = ns.back();
        double mean = 0; for (double v : ns) mean += v; mean /= double(ns.size());
        double sd = 0; for (double v : ns) sd += (v - mean) * (v - mean); sd = ns.size() > 1 ? std::sqrt(sd / double(ns.size() - 1)) : 0.0;
        double median = ns[ns.size() / 2];
        double p99 = ns[size_t(double(ns.size() - 1) * 0.99)];
        // report (machine + compiler metadata — the audit's missing provenance)
#if defined(__GNUC__)
        const char* cc = "gcc"; const char* ccver = __VERSION__;
#elif defined(_MSC_VER)
        const char* cc = "msvc"; const char* ccver = "?";
#else
        const char* cc = "unknown"; const char* ccver = "?";
#endif
        std::printf("# ember bench %s [--fn %s] --iters %d --warmup %d\n", file.c_str(), fn_name.c_str(), bench_iters, bench_warmup);
        std::printf("#   compiler: %s %s\n", cc, ccver);
        std::printf("#   platform: %s (ptr=%zu-bit)\n",
#if defined(__x86_64__) || defined(_M_X64)
                    "x86-64", sizeof(void*)*8
#else
                    "unknown", sizeof(void*)*8
#endif
        );
        std::printf("#   date:     %s %s\n", __DATE__, __TIME__);
        std::printf("#   iters=%d  warmup=%d  entry_returns=%s\n", bench_iters, bench_warmup, returns_void ? "void" : "i64");
        std::printf("result        %lld\n", (long long)entry_ret);
        std::printf("min_ns        %.1f\n", mn);
        std::printf("median_ns     %.1f\n", median);
        std::printf("mean_ns       %.1f\n", mean);
        std::printf("p99_ns        %.1f\n", p99);
        std::printf("max_ns        %.1f\n", mx);
        std::printf("stddev_ns     %.1f\n", sd);
        std::printf("cv_pct        %.2f\n", mean > 0 ? (sd / mean) * 100.0 : 0.0);
        exit_code = 0;
        goto bench_done;
    }

    if (__builtin_setjmp(ectx.checkpoint)) {
        // A trap fired during the call: recoverable error, not a crash.
        std::fprintf(stderr, "ember: RUNTIME TRAP: %s (%s)\n",
                     ectx.last_error.c_str(), ember::trap_reason_str(ectx.last_trap));
        exit_code = 70;   // distinct from valid script return codes + parse/sema exit 2
    } else if (!returns_void) {
        // v1.0 B1: call via ember_call_void(entry, &ectx) — sets r14 = the
        // per-call context register before the call. The JIT'd budget/depth/trap
        // reads go through r14, so ectx is the live context for this call.
        entry_ret = ember::ember_call_void(entry, &ectx);
        // Clamp to a usable PROCESS exit code (POSIX exit codes are 0-255). The
        // clamp intentionally drops the sign for the *process* code, but the
        // SIGNED value (entry_ret) is what the lifecycle decision below uses —
        // @entry returning <= 0 means unload (docs/LIFECYCLE.md §1), and a negative
        // return must NOT be misread as stay-loaded by the clamp turning it
        // into a large positive. (The demo/game sim surfaced this: a probe
        // failure returned -2, the clamp made it 254 > 0, and --tick started
        // ticking a module that had asked to unload.)
        exit_code = int(uint64_t(entry_ret) & 0x7fffffff);
    }
    ectx.has_checkpoint = false;
    goto run_tick;   // skip the bench_done label (bench already returned)
bench_done:
    ectx.has_checkpoint = false;
run_tick:

    // ---- v0.6 --tick mode: run @on_tick fns on a thread until a keybind ----
    // (user suggestion: a tick lifecycle that works on the terminal runtime,
    // shared shape with prism's runtime). After @entry, start a tick thread
    // calling every @on_tick fn at --tick-interval; the main thread shows a
    // minimal TUI + waits for 'q' to stop, unload, and exit. If @entry returned
    // <= 0 the module asked to unload — don't tick.
    if (tick_mode && exit_code != 70) {
        // @entry return <= 0 = unload (docs/LIFECYCLE.md §1). Use the SIGNED return
        // (entry_ret), NOT the clamped process exit_code — the clamp turns a
        // negative unload signal into a large positive (e.g. -2 -> 254), which
        // would incorrectly satisfy `> 0` and start ticking a module that asked
        // to unload. If @entry is void or --fn pointed elsewhere, treat as
        // stay-loaded (the historical default); this is approximate there.
        bool entry_says_stay = (entry_decl && entry_decl->ret && !entry_decl->ret->is_void())
                               ? (entry_ret > 0) : true;
        auto ticks = ember::get_annotated_functions(pr.program, "@on_tick");
        auto dyn_routines = ember::ext_lifecycle::host_routines();  // v1.0: dynamically-registered routines
        if (!entry_says_stay) {
            std::printf("ember: @entry returned <= 0, module unloaded (no tick)\n");
        } else if (ticks.empty() && dyn_routines.empty()) {
            std::printf("ember: --tick mode but no @on_tick functions and no dynamically-registered routines; nothing to tick\n");
        } else {
            std::printf("ember: --tick mode — %zu @on_tick fn(s) + %zu dynamic routine(s), %dms interval. Press 'q' to unload + exit.\n",
                        ticks.size(), dyn_routines.size(), tick_interval_ms);
            std::atomic<bool> stop{false};
            std::atomic<uint64_t> tick_count{0};
            // The tick thread calls each @on_tick fn via the dispatch table.
            // Safety: each tick runs under a fresh checkpoint (a trap in a tick
            // stops the tick thread + sets a flag, not a process crash).
            std::atomic<bool> tick_trapped{false};
            std::thread tick_thread([&]() {
                // v1.0 B1: the tick thread uses its OWN context_t (passed via
                // ember_call_void -> r14), fully isolated from the main thread's
                // ectx. This fixes the pre-B1 --tick bug where the tick thread
                // reused ectx and a trap on one thread could longjmp to the other's
                // checkpoint. Each tick gets a fresh checkpoint; a trap stops THIS
                // tick (sets tick_trapped), not the process or the main thread.
                context_t tick_ctx; tick_ctx.max_call_depth = 512; tick_ctx.budget_remaining = 100000000;
                while (!stop.load(std::memory_order_relaxed)) {
                    if (tick_max > 0 && tick_count.load(std::memory_order_relaxed) >= (uint64_t)tick_max) { stop.store(true); break; }
                    tick_ctx.call_depth = 0;  // reset per tick (budget NOT reset — host's responsibility)
                    tick_ctx.has_checkpoint = true;
                    if (__builtin_setjmp(tick_ctx.checkpoint)) {
                        tick_trapped.store(true); stop.store(true); break;
                    }
                    for (auto& af : ticks) {
                        void* f = table.get(af.slot);
                        if (f) {
                            ember::ember_call_void(f, &tick_ctx);  // r14 = tick_ctx; isolated from ectx
                        }
                    }
                    // v1.0: also drive any DYNAMICALLY-REGISTERED routines (register_routine
                    // in the script stored (slot, data) pairs; the host calls each via the
                    // dispatch table with data as the arg — the §6.2 dynamic-registration
                    // call path, same mechanism as @on_tick, discovered by the script at runtime).
                    for (auto& r : ember::ext_lifecycle::host_routines()) {
                        void* f = table.get(int(r.slot));
                        if (f) {
                            // The routine takes one i64 arg (data); use ember_call_i64 (rcx=data).
                            ember::ember_call_i64(f, &tick_ctx, r.data);
                        }
                    }
                    tick_ctx.has_checkpoint = false;
                    tick_count.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(std::chrono::milliseconds(tick_interval_ms));
                }
            });
            // Minimal TUI: show tick count, poll for 'q'.
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
            // v1.0+: a tick-trap is a runtime error in the per-frame path. Propagate
            // it to the PROCESS exit code (70, the same trap code used for a trap in
            // the main @entry call) so a test harness running `--tick --tick-count N`
            // can distinguish a clean run (exit = @entry's stay-loaded return, e.g. 1)
            // from a tick-time assertion failure (exit 70). Without this, a trapped
            // tick printed a message but exited with @entry's positive return, so the
            // demo/game sim's deterministic assertion had no programmatic failure
            // channel — only the human-readable print. Now the assertion is harness-
            // observable. (This does not change the human-readable output above.)
            if (tick_trapped.load()) exit_code = 70;
        }
    }

    // ---- cleanup ----
    for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
    // reset extension host stores (array/string own process-global storage)
    ext_vec::reset(); ext_quat::reset(); ext_mat::reset();
    ext_string::reset(); ext_array::reset();
    ember::ext_sync::reset();
    ember::ext_lifecycle::reset();
    // ext_math is stateless (no reset()).

    return exit_code;
}
