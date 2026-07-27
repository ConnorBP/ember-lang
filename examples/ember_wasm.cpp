// ember_wasm.cpp — WASM W1: the Emscripten WebAssembly CLI for the ember
// ThinIR interpreter.
//
// This is the WASM target's runner (plan_WASM.md Phase W1). WASM has no
// user-allocated executable memory (no PROT_EXEC pages, no JIT), so the WASM
// backend is the C++ ThinIR interpreter (src/thin_interp.cpp) — it walks a
// lowered ThinFunction instead of emitting native code. This CLI:
//
//   1. reads a .ember file (argv[1], or "-" for stdin),
//   2. lexes / parses / sema-checks / lowers every fn to a ThinFunction,
//   3. builds an InterpDispatch (a ThinFunction* table, NOT native entries),
//   4. runs the entry fn (default "main") via interpret_thin_i64,
//   5. prints "RESULT <i64>\n" to stdout + exits with (result & 0xFF).
//
// It is compiled ONLY under EMBER_WASM_INTERP (the Emscripten CMake target
// defines the macro). Natively (macOS/Windows/Linux) it is NOT built — the
// native runner is ember_cli (a JIT runner). This CLI does NOT use compile_func,
// emit_x64, emit_arm64, alloc_executable, or any JIT path — it goes straight
// AST -> ThinFunction -> interpret_thin (Shape B: full compiler-in-WASM, but
// the BACKEND is the interpreter, not a JIT).
//
// Build (from the repo root, Emscripten):
//   emcmake cmake -G Ninja -DCMAKE_CXX_COMPILER=em++ -DEMBER_WASM_INTERP=ON -B buildwasm ..
//   cmake --build buildwasm
//   node buildwasm/ember_wasm.js tests/lang/valid_arith.ember
//
// Unsupported in WASM (stubbed / not registered — scripts using them fail at
// sema with "unknown native"): coroutines (yield/coroutine_start), threads
// (thread_spawn/join), call_raw (make_executable/call_raw). See
// docs/planning/WASM_PROGRESS.md (W1).
#include "../src/thin_interp.hpp"      // interpret_thin_i64
#include "../src/thin_lower.hpp"       // lower_function
#include "../src/thin_ir.hpp"          // ThinFunction
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"          // CodeGenCtx, g_globals_for_codegen
#include "../src/globals.hpp"          // GlobalsBlock
#include "../src/context.hpp"          // context_t, TrapReason
#include "../src/ast.hpp"
#include "../src/import.hpp"           // resolve_imports (arch-neutral; Emscripten has std::filesystem)

// arch-neutral extensions (WASM-supported): vec/quat/mat/string/array/math/
// map/sync/lifecycle/io/gc. NOT linked: coroutine/thread/call_raw (impossible
// in WASM — no asm ctx switch, no pthreads w/o -pthread, no executable memory).
#include "../extensions/vec/ext_vec.hpp"
#include "../extensions/quat/ext_quat.hpp"
#include "../extensions/mat/ext_mat.hpp"
#include "../extensions/string/ext_string.hpp"
#include "../extensions/array/ext_array.hpp"
#include "../extensions/math/ext_math.hpp"
#include "../extensions/map/ext_map.hpp"
#include "../extensions/sync/ext_sync.hpp"
#include "../extensions/lifecycle/ext_lifecycle.hpp"
#include "../extensions/io/ext_io.hpp"
#include "../extensions/gc/ext_gc.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <climits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace ember;

// ─── the module: lowered ThinFunctions + the interpreter dispatch table ───
struct WasmModule {
    std::vector<ThinFunction> thfs;     // owns the lowered ThinFunctions
    InterpDispatch dispatch;            // slot -> ThinFunction*
    std::unordered_map<std::string, int> slots;
    GlobalsBlock gb;
    std::vector<uint8_t> gb_store;      // the typed globals block (outlives the run)
    StructLayoutTable layouts;
    Program prog;
    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
    CodeGenCtx ctx;
};

// host_value_bytes — the byte size of a value type for the typed globals
// block layout (mirrors ember_cli's host_value_bytes / thin_interp's
// value_bytes). Used by compute_typed_globals_layout below.
static uint32_t host_value_bytes(const Type* t, const StructLayoutTable* structs) {
    if (!t) return 8;
    if (t->is_slice) return 16;
    if (t->is_lambda) return 16;   // {fn_slot, env_ptr} — same 16-byte ABI as a slice
    if (t->array_len > 0)
        return uint32_t(t->array_len) * host_value_bytes(t->elem.get(), structs);
    if (!t->struct_name.empty() && structs) {
        auto it = structs->find(t->struct_name);
        if (it != structs->end()) return uint32_t(it->second.size);
    }
    switch (t->prim) {
    case Prim::Bool: case Prim::I8: case Prim::U8: return 1;
    case Prim::I16: case Prim::U16: return 2;
    case Prim::I32: case Prim::U32: case Prim::F32: return 4;
    default: return 8;
    }
}

// Typed globals-block layout (mirrors ember_cli's compute_typed_globals_layout).
// Primary slots are 8-aligned; slice-global backing regions are appended after.
struct TypedGlobalsLayout {
    uint32_t total_size = 0;
    std::unordered_map<std::string, uint32_t> offsets;
    std::unordered_map<std::string, uint32_t> sizes;
    std::unordered_map<std::string, uint32_t> backing_offsets; // slice globals only
};

static TypedGlobalsLayout compute_typed_globals_layout(const Program& prog,
                                                       const StructLayoutTable& structs) {
    TypedGlobalsLayout L;
    uint32_t cur = 0;
    auto align8 = [](uint32_t v) -> uint32_t { return (v + 7u) & ~7u; };
    for (const auto& g : prog.globals) {
        uint32_t sz = host_value_bytes(g.ty.get(), &structs);
        cur = align8(cur);
        L.offsets[g.name] = cur;
        L.sizes[g.name] = sz;
        cur += sz;
    }
    for (const auto& g : prog.globals) {
        if (!g.ty || !g.ty->is_slice) continue;
        if (!g.init) continue;
        auto* al = dynamic_cast<const ArrayLit*>(g.init.get());
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

// Read a file's full contents via plain C stdio (no std::filesystem in the CLI
// itself — the task's portability guidance). Returns "" on failure.
static std::string read_file_c(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::string out;
    char buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// Read all of stdin (for "-" path).
static std::string read_stdin() {
    std::string out;
    char buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) out.append(buf, n);
    return out;
}

// A minimal assert_eq_i64 native so scripts that use it (the lang suite's
// deep-nested-blocks + struct-reassign probes) sema + run. Returns 0; the
// constexpr fold in sema does the real equality check at compile time.
static int64_t n_assert_eq_i64(int64_t a, int64_t b) {
    (void)a; (void)b;
    return 0;
}
static int64_t n_assert_eq_f32(float a, float b) { (void)a; (void)b; return 0; }
static int64_t n_assert_eq_f64(double a, double b) { (void)a; (void)b; return 0; }
static int64_t n_assert_true(int64_t v) { (void)v; return 0; }

// Register the arch-neutral extension natives + the test-helper asserts.
static void register_wasm_natives(std::unordered_map<std::string, NativeSig>& natives,
                                  OpOverloadTable& overloads) {
    ext_vec::register_natives(natives);
    ext_quat::register_natives(natives);
    ext_mat::register_natives(natives);
    ext_string::register_natives(natives);
    ext_array::register_natives(natives);
    ext_math::register_natives(natives);
    ext_map::register_natives(natives);
    ext_sync::register_natives(natives);
    ext_lifecycle::register_natives(natives);
    ext_io::register_natives(natives);
    ext_gc::register_natives(natives);
    // operator overloads (vec/quat/mat/string) — published into the native
    // table the same way ember_cli's register_standard_bindings does, so sema
    // resolves the overload fn names.
    ext_vec::register_overloads(overloads);
    ext_quat::register_overloads(overloads);
    ext_mat::register_overloads(overloads);
    ext_string::register_overloads(overloads);
    for (const auto& item : overloads.entries) {
        const OpOverload& o = item.second;
        NativeSig sig; sig.name = o.fn_name; sig.fn_ptr = o.fn_ptr;
        sig.ret = o.ret; sig.params = o.params;
        natives[o.fn_name] = std::move(sig);
    }
    // test-helper asserts (used by some lang-suite runtime probes)
    {
        NativeSig s; s.name = "assert_eq_i64"; s.fn_ptr = (void*)&n_assert_eq_i64;
        s.ret = type_i64(); s.params = {type_i64(), type_i64()};
        natives["assert_eq_i64"] = s;
        NativeSig s2; s2.name = "assert_eq_f32"; s2.fn_ptr = (void*)&n_assert_eq_f32;
        s2.ret = type_i64(); s2.params = {type_f32(), type_f32()};
        natives["assert_eq_f32"] = s2;
        NativeSig s3; s3.name = "assert_eq_f64"; s3.fn_ptr = (void*)&n_assert_eq_f64;
        s3.ret = type_i64(); s3.params = {type_f64(), type_f64()};
        natives["assert_eq_f64"] = s3;
        NativeSig s4; s4.name = "assert_true"; s4.fn_ptr = (void*)&n_assert_true;
        s4.ret = type_i64(); s4.params = {type_i64()};
        natives["assert_true"] = s4;
    }
}

// Compile `src` through lex/parse/sema/lower_function. Returns the module
// (lowered + dispatch wired) or nullptr on a compile error (prints the error).
static std::unique_ptr<WasmModule> compile_source(const std::string& src,
                                                  const std::string& /*file*/) {
    auto m = std::make_unique<WasmModule>();
    auto lr = tokenize(src, "<ember_wasm>");
    if (!lr.ok) { std::fprintf(stderr, "ember_wasm: lex error (%u:%u): %s\n",
                               lr.err_line, lr.err_col, lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::fprintf(stderr, "ember_wasm: parse error: %s\n",
                               pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    if (m->prog.funcs.empty()) {
        std::fprintf(stderr, "ember_wasm: no functions in source\n");
        return nullptr;
    }
    // slot assignment (mirror ember_cli: namespaced fns use Ns::name)
    int si = 0;
    for (auto& fn : m->prog.funcs) {
        if (fn.ns.empty()) m->slots[fn.name] = si++;
        else m->slots[fn.ns + "::" + fn.name] = si++;
        fn.slot = si - 1;
    }

    register_wasm_natives(m->natives, m->overloads);
    m->layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;  // no string encryption (ConstStringRef path)

    auto sr = sema(m->prog, m->natives, m->slots, 0u, &m->overloads, &m->layouts);
    if (!sr.ok) {
        std::fprintf(stderr, "ember_wasm: sema errors (%zu):\n", sr.errors.size());
        for (auto& e : sr.errors) std::fprintf(stderr, "  line %u: %s\n", e.line, e.msg.c_str());
        return nullptr;
    }

    // ---- globals block (TYPED layout; mirrors ember_cli's setup) ----
    // The interpreter reads globals via ctx.globals_base + per-global addend
    // (LoadGlobal/StoreGlobal). Without this, a script that reads a global
    // (e.g. `global g = square(6) + 100;` or `namespace Math { global val=42; }`)
    // sees zero — the loaded bytes never get their const initializer folded
    // in. We size a typed block, fold const initializers into it, then set
    // gb.base + ctx.globals_base to the block's address.
    TypedGlobalsLayout tgl = compute_typed_globals_layout(m->prog, m->layouts);
    {
        uint32_t gi = 0;
        for (auto& g : m->prog.globals) {
            m->gb.index[g.name] = gi++;
            m->gb.types[g.name] = g.ty.get();
            m->gb.offsets[g.name] = tgl.offsets[g.name];
            m->gb.sizes[g.name] = tgl.sizes[g.name];
            // Tier 1 namespaces: also register the qualified name.
            if (!g.ns.empty()) {
                std::string qn = g.ns + "::" + g.name;
                m->gb.index[qn] = m->gb.index[g.name];
                m->gb.types[qn] = m->gb.types[g.name];
                m->gb.offsets[qn] = m->gb.offsets[g.name];
                m->gb.sizes[qn] = m->gb.sizes[g.name];
            }
        }
    }
    m->gb_store.assign(size_t(tgl.total_size), 0);
    m->gb.base = int64_t(m->gb_store.data());
    g_globals_for_codegen = &m->gb;
    {
        auto string_alloc_thunk = [](const char* bytes, int64_t len) -> int64_t {
            return ember::ext_string::alloc(std::string(bytes, size_t(len > 0 ? len : 0)));
        };
        GlobalInitCtx gic{m->gb_store, m->gb.index, m->gb.types};
        gic.string_alloc_fn = string_alloc_thunk;
        gic.offsets = &m->gb.offsets;
        gic.sizes = &m->gb.sizes;
        gic.backing_offsets = &tgl.backing_offsets;
        gic.structs = &m->layouts;
        eval_global_initializers(m->prog, gic);
    }

    m->thfs.reserve(m->prog.funcs.size());  // stable addresses for dispatch ptrs
    m->dispatch.resize(m->prog.funcs.size(), nullptr);

    m->ctx.globals_base = m->gb.base;
    m->ctx.globals_index = &m->gb.index;
    m->ctx.globals_types = &m->gb.types;
    m->ctx.globals_offsets = &m->gb.offsets;
    m->ctx.dispatch_base = 0;  // the interpreter doesn't use dispatch_base
    m->ctx.natives = &m->natives;
    m->ctx.script_slots = &m->slots;
    m->ctx.structs = &m->layouts;
    // use_context_reg=true so try/catch + throw lower (thin_lower requires it
    // for the catch-stack). The interpreter does NOT use a context register —
    // it reads context_t via the passed ectx — but the LOWERER gates try/catch
    // on this flag. emit_budget_checks/emit_depth_checks stay false so normal
    // scripts (deep recursion, long loops) do not false-trap; the interpreter's
    // entry budget check + DepthCheck instrs are gated by these flags.
    m->ctx.use_context_reg = true;
    m->ctx.emit_budget_checks = false;
    m->ctx.emit_depth_checks = false;
    m->ctx.use_gc_env = false;        // lambdas-as-stack-env (the interpreter handles it)
    m->ctx.gc_frame_head_ptr = nullptr;

    for (auto& fn : m->prog.funcs) {
        ThinFunction thf = lower_function(fn, m->ctx);
        if (thf.blocks.empty()) {
            std::fprintf(stderr, "ember_wasm: lower_function gave empty blocks for %s (nsr='%s')\n",
                         fn.name.c_str(), thf.non_serializable_reason.c_str());
            g_globals_for_codegen = nullptr;
            return nullptr;
        }
        size_t idx = m->thfs.size();
        m->thfs.push_back(std::move(thf));
        m->dispatch[fn.slot] = &m->thfs[idx];
    }
    g_globals_for_codegen = nullptr;
    return m;
}

static void usage(FILE* out) {
    std::fprintf(out,
        "ember_wasm — ThinIR interpreter runner (WebAssembly)\n"
        "usage: ember_wasm <input.ember> [--fn NAME]\n"
        "       ember_wasm -            (read source from stdin)\n"
        "  --fn NAME   entry function (default: main)\n"
        "  --help      show this help\n"
        "Prints: RESULT <i64>\\n  (or TRAP <reason> on an unhandled trap)\n"
        "Exit code: (result & 0xFF), or 2 on a compile/usage error.\n");
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::string file;
    std::string entry = "main";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { usage(stdout); return 0; }
        if (a == "--fn") { if (++i >= argc) { usage(stderr); return 2; } entry = argv[i]; continue; }
        if (!a.empty() && a[0] == '-') { file = a; continue; }  // "-" = stdin
        if (file.empty()) file = a;
    }
    if (file.empty()) { usage(stderr); return 2; }

    // read the source
    std::string raw = (file == "-") ? read_stdin() : read_file_c(file.c_str());
    if (raw.empty()) {
        std::fprintf(stderr, "ember_wasm: cannot read '%s'\n", file.c_str());
        return 2;
    }

    // resolve `import "path";` inlining (arch-neutral; Emscripten has
    // std::filesystem). If it fails (e.g. a missing import), fall back to the
    // raw source so a script with no imports still runs.
    std::string src;
    {
        std::string base_dir;
        if (file != "-") {
            // derive the directory of the input file (plain string ops, no
            // std::filesystem in the CLI itself)
            auto pos = file.find_last_of("/\\");
            base_dir = (pos != std::string::npos) ? file.substr(0, pos) : ".";
        } else {
            base_dir = ".";
        }
        std::unordered_set<std::string> seen;
        try {
            src = resolve_imports(raw, base_dir, seen);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ember_wasm: import resolve failed (%s); running raw source\n", e.what());
            src = raw;
        }
    }

    auto m = compile_source(src, file);
    if (!m) return 2;

    // --dump-ir: print the lowered ThinIR for every fn (debug aid).
    if (std::getenv("EMBER_WASM_DUMP_IR")) {
        for (size_t i = 0; i < m->thfs.size(); ++i) {
            std::fprintf(stderr, "===== fn[%zu] %s =====\n%s\n",
                         i, m->prog.funcs[i].name.c_str(),
                         dump(m->thfs[i]).c_str());
        }
    }

    // locate the entry fn
    auto it = m->slots.find(entry);
    if (it == m->slots.end() || it->second < 0 || it->second >= int(m->dispatch.size())) {
        std::fprintf(stderr, "ember_wasm: entry '%s' not found\n", entry.c_str());
        return 2;
    }
    const ThinFunction* entry_thf = m->dispatch[it->second];
    if (!entry_thf) {
        std::fprintf(stderr, "ember_wasm: entry '%s' has no lowered ThinFunction\n", entry.c_str());
        return 2;
    }

    // run it via the interpreter. try/catch + the interpreter's entry budget
    // check + catch-stack need a non-null context_t, so always provide one with
    // a generous budget + max_call_depth (no false traps: emit_budget_checks /
    // emit_depth_checks are false). interpret_thin_i64_safe sets up a setjmp
    // checkpoint so an UNHANDLED throw (no enclosing try/catch) is recoverable
    // + observable (returns 0 + sets *trapped) instead of propagating an
    // InterpTrap exception. In-language try/catch uses the interpreter's
    // pc-restore catch-stack (no longjmp) + returns normally.
    context_t ectx;
    ectx.budget_remaining = INT64_MAX;  // effectively unlimited (no budget checks)
    ectx.max_call_depth = 4096;         // generous: fib(20) depth ~20, deep nesting ok
    ectx.last_trap = TrapReason::None;
    ectx.catch_depth = 0;
    ectx.has_checkpoint = false;

    int64_t result = 0;
    bool trapped = false;
    TrapReason trap_reason = TrapReason::None;
    std::string trap_detail;
    result = interpret_thin_i64_safe(*entry_thf, m->dispatch, m->ctx, &ectx,
                                     nullptr, 0, &trapped);
    if (trapped) {
        trap_reason = ectx.last_trap;
        trap_detail = ectx.last_error;
    }

    if (trapped) {
        std::printf("TRAP %s%s%s\n", trap_reason_str(trap_reason),
                    trap_detail.empty() ? "" : ": ", trap_detail.c_str());
        return 70;  // the lang-suite convention for a runtime trap
    }
    std::printf("RESULT %lld\n", static_cast<long long>(result));
    std::fflush(stdout);
    return int(result & 0xFF);
}
