// ember_bundle - standalone exe bundler (docs/planning/plan_STANDALONE_BUNDLER.md).
//
// `ember_bundle <input.ember> <output.exe> [--stub <stub.exe>] [--fn NAME]
//               [--permissions none|ffi]
//               [--output-permissions stub|preserve]`
//
// This file is also the implementation of the ember_bundler library used by
// `ember bundle`. CMake compiles it once with EMBER_BUNDLE_LIBRARY_ONLY and
// links both front-ends to that library; EMBER_BUNDLE_MAIN_ONLY builds the
// tiny standalone front-end without cloning the implementation.

#ifdef EMBER_BUNDLE_MAIN_ONLY
namespace ember_bundle { int command(int argc, char** argv); }
int main(int argc, char** argv) { return ember_bundle::command(argc, argv); }
#else
//
// Compiles a .ember script to a .em blob (the existing emit-em pipeline:
// read → resolve imports → lex → parse → slot assignment → register bindings
// → sema → globals block → codegen + finalize → build EmModule →
// write_em_bytes), then produces <output.exe> by copying the pre-built stub
// (ember_stub_main.exe) + appending the .em blob + a 12-byte footer.
//
//   output.exe = [stub PE bytes] [ .em blob ] [ footer: u32 magic | u64 em_len ]
//
// The stub's main() (examples/ember_stub_main.cpp) reads its own file at
// runtime, finds the footer, loads the .em via load_em_bytes, and calls the
// entry. No C++ compiler at bundle time. No temp files at runtime.
//
// The stub path defaults to ember_stub_main.exe in the same directory as this
// bundler exe (found relative to the bundler's own path). --stub overrides.
//
// Links: the same libraries as ember_cli (ember, ember_frontend, ember_import,
// all extensions) because it runs the full compile pipeline. The OUTPUT exe
// (the stub) links only the load path (ember + ember_frontend + extensions).

#include "../src/engine.hpp"          // CompiledFn, finalize, free_executable
#include "../src/dispatch_table.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"         // CodeGenCtx, compile_func, g_globals_for_codegen, GlobalsBlock
#include "../src/import.hpp"          // resolve_imports
#include "../src/jit_memory.hpp"      // free_executable
#include "../src/globals.hpp"         // eval_global_initializers, GlobalInitCtx
#include "../src/module_registry.hpp"
#include "../src/module_linker.hpp"   // link_em_file, build_em_exports, add_exports
#include "../src/em_file.hpp"         // EmModule, EmFunctionRecord, EmReloc, EmNativeBinding
#include "../src/em_writer.hpp"       // write_em_bytes

#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"
#include "ext_map.hpp"
#include "ext_sync.hpp"
#include "ext_lifecycle.hpp"
#include "ext_io.hpp"
#include "ext_call_raw.hpp"     // self-hosting Stage 4 gap: call_raw(fn_ptr,arg)->i64
#include "ext_coroutine.hpp"   // #21 coroutines (set_coroutine_dispatch native)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace fs = std::filesystem;

// ---- bundle footer (must match ember_stub_main.cpp) ----
constexpr uint32_t EM_BUNDLE_MAGIC       = 0x454D4244u;  // "EMBD"
constexpr uint32_t EM_BUNDLE_FOOTER_SIZE  = 12u;          // u32 magic + u64 em_length

// ---- helpers mirrored from ember_cli.cpp (the compile pipeline) ----

static std::string read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static void register_standard_bindings(
        std::unordered_map<std::string, ember::NativeSig>& natives,
        ember::OpOverloadTable* overloads_out = nullptr) {
    using namespace ember;
    ext_vec::register_natives(natives); ext_quat::register_natives(natives);
    ext_mat::register_natives(natives); ext_string::register_natives(natives);
    ext_array::register_natives(natives); ext_math::register_natives(natives);
    ext_map::register_natives(natives);
    ext_sync::register_natives(natives); ext_lifecycle::register_natives(natives);
    ext_io::register_natives(natives);
    ext_call_raw::register_natives(natives);
    ext_coroutine::register_natives(natives);
    OpOverloadTable overloads;
    ext_vec::register_overloads(overloads); ext_quat::register_overloads(overloads);
    ext_mat::register_overloads(overloads); ext_string::register_overloads(overloads);
    for (const auto& item : overloads.entries) {
        const OpOverload& o = item.second;
        NativeSig sig; sig.name = o.fn_name; sig.fn_ptr = o.fn_ptr;
        sig.ret = o.ret; sig.params = o.params;
        natives[o.fn_name] = std::move(sig);
    }
    if (overloads_out) *overloads_out = std::move(overloads);
}

static uint32_t host_value_bytes(const ember::Type* t, const ember::StructLayoutTable* structs) {
    if (!t) return 8;
    if (t->is_slice) return 16;
    if (t->is_lambda) return 16;   // {fn_slot, env_ptr} (#20)
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
    std::unordered_map<std::string, uint32_t> backing_offsets;
};

static TypedGlobalsLayout compute_typed_globals_layout(const ember::Program& prog,
                                                       const ember::StructLayoutTable& structs) {
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

// Find this exe's own path (to locate the default stub sibling).
static fs::path get_own_exe_path() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return fs::path();
    return fs::path(buf);
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !p.empty()) return p;
    return fs::path();
#endif
}

// Write a u32 little-endian into a byte stream.
static void append_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

// Write a u64 little-endian into a byte stream.
static void append_u64_le(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}

static void usage(FILE* out) {
    std::fprintf(out,
        "ember_bundle - compile a .ember script + the ember runtime into one .exe\n"
        "usage:\n"
        "  ember_bundle <input.ember> <output.exe> [--stub <stub.exe>] [--fn NAME]\n"
        "               [--permissions none|ffi]\n"
        "               [--output-permissions stub|preserve]\n"
        "\n"
        "  Compiles <input.ember> to a .em blob, copies the pre-built stub exe\n"
        "  (ember_stub_main.exe) to <output.exe>, and appends the .em + a 12-byte\n"
        "  footer. The resulting <output.exe> is self-contained: it loads the\n"
        "  embedded .em at runtime and runs the entry function.\n"
        "\n"
        "  --stub PATH   path to the stub exe (default: ember_stub_main.exe next\n"
        "                to this bundler)\n"
        "  --fn NAME     entry function baked into the bundle (default: main)\n"
        "  --permissions POLICY\n"
        "                source capability policy: 'none' (default) rejects FFI/IO\n"
        "                natives; 'ffi' permits them. The trusted runtime stub has\n"
        "                the FFI host bindings needed to load explicitly permitted\n"
        "                bundles. This option does not alter filesystem ACLs.\n"
        "  --output-permissions POLICY\n"
        "                'stub' (default) copies the stub's filesystem mode;\n"
        "                'preserve' retains an existing output's mode when it is\n"
        "                overwritten. Windows ACL inheritance remains OS-managed.\n"
    );
}

// Add a module-init function for host-owned string globals. A `.em` globals
// block can persist plain scalar bytes, but a `string` value is a process-local
// handle into ext_string's store; serializing the bundler process's handle makes
// it invalid in the standalone stub process. Materialize each literal again at
// runtime and store the fresh handle into the relocated globals block.
static bool add_globals_init(ember::Program& program, std::string& err_out) {
    using namespace ember;

    FuncDecl init;
    init.name = "__globals_init";
    init.ret = std::make_shared<Type>(type_void());
    init.loc = Loc{1, 1};

    for (const auto& global : program.globals) {
        const auto* literal = dynamic_cast<const StringLit*>(global.init.get());
        // This runs before sema resolves named opaque handles from Prim::Void
        // to Prim::I64, so identify `string` by its nominal name here.
        const bool is_string = global.ty && global.ty->struct_name == "string";
        if (!literal || !is_string) continue;
        if (global.is_const || !global.ns.empty()) {
            err_out = "ember_bundle: portable string-global initialization does not yet support "
                      "const or namespaced global '" + global.name + "'";
            return false;
        }

        auto target = std::make_unique<Ident>();
        target->name = global.name;
        target->loc = global.loc;
        auto value = std::make_unique<StringLit>();
        value->s = literal->s;
        value->loc = literal->loc;
        auto assign = std::make_unique<AssignExpr>();
        assign->target = std::move(target);
        assign->value = std::move(value);
        assign->loc = global.loc;
        auto statement = std::make_unique<ExprStmt>();
        statement->expr = std::move(assign);
        statement->loc = global.loc;
        init.body.stmts.push_back(std::move(statement));
    }

    if (init.body.stmts.empty()) return true;
    for (const auto& fn : program.funcs) {
        if (fn.name == init.name && fn.ns.empty()) {
            err_out = "ember_bundle: function name '__globals_init' is reserved for module initialization";
            return false;
        }
    }
    program.funcs.push_back(std::move(init));
    return true;
}

// Compile a .ember file to an EmModule in memory. This is the emit-em pipeline
// from ember_cli.cpp's run_ember_file, factored out so the bundler can produce
// the .em without going through the CLI. Returns true + fills `mod` on success;
// false + prints errors to stderr on failure. `fns_out` receives the JIT'd
// CompiledFns (so the caller can free their exec pages).
static bool compile_to_em_module(const fs::path& file,
                                 const std::string& entry_name,
                                 uint32_t permissions,
                                 ember::EmModule& mod,
                                 std::vector<ember::CompiledFn>& fns_out,
                                 std::string& err_out) {
    using namespace ember;

    if (!fs::exists(file)) {
        err_out = "ember_bundle: no such file: " + file.u8string();
        return false;
    }

    // ---- read + resolve imports ----
    std::string raw = read_file(file);
    if (raw.empty() && fs::file_size(file) == 0) {
        // empty file — will fail at parse with a clearer message
    } else if (raw.empty()) {
        err_out = "ember_bundle: cannot read '" + file.u8string() + "'";
        return false;
    }
    std::string base_dir = file.parent_path().u8string();
    std::unordered_set<std::string> seen;
    std::string src;
    try {
        std::string canon = fs::weakly_canonical(file).u8string();
        seen.insert(canon);
        src = resolve_imports(raw, base_dir, seen);
    } catch (const std::exception& e) {
        err_out = std::string("ember_bundle: import error: ") + e.what();
        return false;
    }

    // ---- lex ----
    const std::string display_file = file.u8string();
    auto lr = tokenize(src, display_file.c_str());
    if (!lr.ok) {
        err_out = "ember_bundle: lex error (" + std::to_string(lr.err_line) + ":" +
                  std::to_string(lr.err_col) + "): " + lr.error;
        return false;
    }

    // ---- parse ----
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) {
        err_out = "ember_bundle: parse error: " + pr.error;
        return false;
    }
    if (pr.program.funcs.empty()) {
        err_out = "ember_bundle: no functions in '" + file.u8string() + "'";
        return false;
    }
    if (!add_globals_init(pr.program, err_out)) return false;

    // ---- slot assignment ----
    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = slots[fn.name]; }
    const auto requested_entry = slots.find(entry_name);
    if (requested_entry == slots.end()) {
        err_out = "ember_bundle: entry function '" + entry_name + "' not found";
        return false;
    }

    // ---- register bindings ----
    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
    register_standard_bindings(natives, &overloads);

    // ---- struct layouts + string key + sema ----
    auto struct_layouts = build_struct_layouts(pr.program);
    pr.program.string_xor_key = 0xA5;

    // ---- live-module link resolution ----
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
            // FIX 3 + Finding B: the bundler is a build tool processing trusted
            // source, so it opts in to raw x86 (v3 artifacts) and grants
            // PERM_FFI (the linked modules may use ext_io natives).
            ember::EmLoadPolicy em_policy{permissions, true};
            if (!link_em_file(registry, path.c_str(), ld.alias, linked_ems.back(), &lerr, &natives, nullptr, &em_policy)) {
                err_out = "ember_bundle: link '" + ld.target + "' failed: " + lerr;
                return false;
            }
            uint32_t id = registry.find_by_name(ld.alias);
            add_exports(module_exports, ld.alias, build_em_exports(linked_ems.back(), id));
        }
    }

    auto sr = sema(pr.program, natives, slots, permissions, &overloads, &struct_layouts, &module_exports);
    if (!sr.ok) {
        std::string msg = "ember_bundle: sema errors (" + std::to_string(sr.errors.size()) + "):";
        for (auto& e : sr.errors) msg += "\n  line " + std::to_string(e.line) + ": " + e.msg;
        err_out = msg;
        return false;
    }

    // ---- globals block ----
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
    g_globals_for_codegen = nullptr;
    GlobalInitCtx gic{gb_store, gb.index, gb.types};
    // String handles are process-local and cannot be serialized. The synthetic
    // __globals_init function above creates them in the loading process.
    gic.string_alloc_fn = nullptr;
    gic.offsets = &gb.offsets;
    gic.sizes = &gb.sizes;
    gic.backing_offsets = &tgl.backing_offsets;
    gic.structs = &struct_layouts;
    eval_global_initializers(pr.program, gic);

    // ---- dispatch table + codegen ctx ----
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
    std::string reg_err;
    uint32_t self_id = registry.register_module("__main__", table.base(), &reg_err);
    (void)self_id;
    // X1 redesign: publish __main__'s dispatch-table slot count so a linked
    // v5 .em that calls back into the bundler's host module via CallCrossModule
    // range-checks its slot against the REAL host dispatch size at load time.
    registry.set_dispatch_slot_count(self_id, int64_t(table.slots.size()));

    // emit-em path: no trap stub, no context reg, no budget/depth checks.
    // The .em is pre-compiled trusted code; the stub loads it raw.
    ctx.use_context_reg = false;
    ctx.emit_budget_checks = false;
    ctx.emit_depth_checks = false;

    // ---- compile + finalize each function ----
    fns_out.reserve(pr.program.funcs.size());
    for (auto& fn : pr.program.funcs) {
        CompiledFn cf = compile_func(fn, ctx);
        if (!finalize(cf)) {
            err_out = "ember_bundle: alloc_executable failed for " + fn.name;
            return false;
        }
        table.set(fn.slot, cf.entry);
        fns_out.push_back(std::move(cf));
    }

    // ---- build the EmModule ----
    mod.functions.reserve(fns_out.size());
    for (size_t fi = 0; fi < fns_out.size(); ++fi) {
        const auto& cf = fns_out[fi];
        const auto& decl = pr.program.funcs[fi];
        EmFunctionRecord rec;
        rec.name = cf.name;
        rec.slot_index = uint32_t(decl.slot);
        rec.code = cf.bytes;
        rec.rodata = cf.rodata;
        rec.non_serializable_reason = cf.non_serializable_reason;
        rec.signature.ret = decl.ret ? *decl.ret : Type{};
        for (const auto& p : decl.params) rec.signature.params.push_back(p.ty ? *p.ty : Type{});
        for (const auto& af : cf.abs_fixups) {
            EmReloc r; r.offset = af.code_offset; r.kind = uint8_t(af.kind); r.addend = af.addend;
            rec.relocs.push_back(r);
        }
        for (const auto& nf : cf.native_fixups) {
            EmNativeBinding b; b.offset = nf.code_offset; b.name = nf.name;
            b.signature.ret = nf.ret; b.signature.params = nf.params;
            rec.native_bindings.push_back(std::move(b));
        }
        mod.functions.push_back(std::move(rec));
    }
    mod.globals = gb_store;

    // `--fn` is a build-time choice: persist it as the module entry slot so
    // the produced executable runs that function without runtime arguments.
    mod.entry_slot = uint32_t(requested_entry->second);

    mod.name_table.reserve(pr.program.funcs.size());
    for (const auto& fn : pr.program.funcs)
        if (fn.is_exported)
            mod.name_table.push_back({fn.name, uint32_t(fn.slot)});

    return true;
}

namespace ember_bundle {

int command(int argc, char** argv) {
    fs::path input_file;
    fs::path output_file;
    fs::path stub_path;
    std::string fn_name = "main";
    uint32_t permissions = 0u;
    bool preserve_output_permissions = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--stub") {
            if (++i >= argc) { std::fprintf(stderr, "ember_bundle: --stub needs a path\n"); return 2; }
            stub_path = argv[i];
        } else if (a == "--fn") {
            if (++i >= argc) { std::fprintf(stderr, "ember_bundle: --fn needs a name\n"); return 2; }
            fn_name = argv[i];
        } else if (a == "--permissions") {
            if (++i >= argc) { std::fprintf(stderr, "ember_bundle: --permissions needs none or ffi\n"); return 2; }
            const std::string policy = argv[i];
            if (policy == "none") permissions = 0u;
            else if (policy == "ffi") permissions = ember::PERM_FFI;
            else { std::fprintf(stderr, "ember_bundle: invalid permission policy '%s' (expected none or ffi)\n", policy.c_str()); return 2; }
        } else if (a == "--output-permissions") {
            if (++i >= argc) { std::fprintf(stderr, "ember_bundle: --output-permissions needs stub or preserve\n"); return 2; }
            const std::string policy = argv[i];
            if (policy == "stub") preserve_output_permissions = false;
            else if (policy == "preserve") preserve_output_permissions = true;
            else { std::fprintf(stderr, "ember_bundle: invalid output permission policy '%s' (expected stub or preserve)\n", policy.c_str()); return 2; }
        } else if (a == "--help" || a == "-h") {
            usage(stdout);
            return 0;
        } else if (input_file.empty()) {
            input_file = fs::u8path(a);
        } else if (output_file.empty()) {
            output_file = fs::u8path(a);
        } else {
            std::fprintf(stderr, "ember_bundle: unexpected argument: %s\n", a.c_str());
            usage(stderr);
            return 2;
        }
    }

    if (input_file.empty() || output_file.empty()) {
        std::fprintf(stderr, "ember_bundle: need <input.ember> <output.exe>\n");
        usage(stderr);
        return 2;
    }

    // ---- resolve the stub path ----
    fs::path stub;
    if (!stub_path.empty()) {
        stub = stub_path;
    } else {
        // default: ember_stub_main.exe next to this bundler exe
        fs::path own = get_own_exe_path();
        if (own.empty()) {
            std::fprintf(stderr, "ember_bundle: cannot determine own path; use --stub\n");
            return 2;
        }
#if defined(_WIN32)
        stub = own.parent_path() / "ember_stub_main.exe";
#else
        stub = own.parent_path() / "ember_stub_main";
#endif
    }
    if (!fs::exists(stub)) {
        std::fprintf(stderr, "ember_bundle: stub not found: %s\n", stub.u8string().c_str());
        std::fprintf(stderr, "  (build ember_stub_main or pass --stub <path>)\n");
        return 2;
    }

    // ---- compile .ember -> EmModule ----
    ember::EmModule emod;
    std::vector<ember::CompiledFn> fns;
    std::string cerr;
    {
        // The compile pipeline JITs each function (allocates exec pages) to
        // produce the code bytes. We free those pages after serialization.
        if (!compile_to_em_module(input_file, fn_name, permissions, emod, fns, cerr)) {
            std::fprintf(stderr, "%s\n", cerr.c_str());
            return 2;
        }
    }

    // ---- serialize EmModule -> .em bytes (in memory, no temp file) ----
    std::vector<uint8_t> em_bytes;
    std::string werr;
    if (!ember::write_em_bytes(emod, em_bytes, &werr)) {
        std::fprintf(stderr, "ember_bundle: write_em_bytes failed: %s\n", werr.c_str());
        for (auto& fn : fns) if (fn.exec) ember::free_executable(fn.exec);
        return 2;
    }

    // Free the JIT'd exec pages now (the .em bytes are the serialized copy).
    for (auto& fn : fns) if (fn.exec) ember::free_executable(fn.exec);
    fns.clear();

    // reset extension host stores (the compile pipeline registered natives;
    // clean up so a future bundle in the same process starts fresh).
    ember::ext_vec::reset(); ember::ext_quat::reset(); ember::ext_mat::reset();
    ember::ext_string::reset(); ember::ext_array::reset();
    ember::ext_sync::reset(); ember::ext_lifecycle::reset();
    ember::ext_io::reset();

    std::error_code ec;
    // Validate all sizes before creating/replacing the output. Besides making
    // streamsize conversions explicit, this proves the final footer equation
    // cannot overflow: output = stub + em + 12.
    if (em_bytes.empty() || em_bytes.size() > size_t(std::numeric_limits<std::streamsize>::max())) {
        std::fprintf(stderr, "ember_bundle: serialized module has an invalid/unsupported size\n");
        return 2;
    }
    ec.clear();
    const uintmax_t stub_size = fs::file_size(stub, ec);
    if (ec) {
        std::fprintf(stderr, "ember_bundle: cannot determine stub size: %s\n", ec.message().c_str());
        return 2;
    }
    constexpr uint64_t U64_MAX_VALUE = std::numeric_limits<uint64_t>::max();
    if (stub_size > U64_MAX_VALUE - EM_BUNDLE_FOOTER_SIZE ||
        uint64_t(em_bytes.size()) > U64_MAX_VALUE - EM_BUNDLE_FOOTER_SIZE - uint64_t(stub_size)) {
        std::fprintf(stderr, "ember_bundle: output size would overflow the bundle footer format\n");
        return 2;
    }

    // ---- publish: write the full bundle to a same-directory temp file,
    //      flush/close successfully, then atomically replace the destination ----
    // The prior implementation copied the stub over output_file FIRST and then
    // appended the .em + footer in place; an append-open or write failure left
    // output_file as a truncated stub (a partial replacement) and the original
    // output was destroyed by the initial overwrite. The atomic temp-then-
    // rename below preserves an existing output on ANY failure path: the temp
    // is the only file mutated, and it is renamed over the destination only
    // after a successful flush+close. RAII removes the temp on every early
    // return so a crashed/errored bundle never leaves a stale temp behind.
    fs::perms output_perms = fs::perms::unknown;
    if (preserve_output_permissions && fs::exists(output_file, ec) && !ec) {
        output_perms = fs::status(output_file, ec).permissions();
        if (ec) {
            std::fprintf(stderr, "ember_bundle: cannot read existing output permissions: %s\n", ec.message().c_str());
            return 2;
        }
    } else {
        ec.clear();
        output_perms = fs::status(stub, ec).permissions();
        if (ec) {
            std::fprintf(stderr, "ember_bundle: cannot read stub permissions: %s\n", ec.message().c_str());
            return 2;
        }
    }

    // Same-directory temp so the final rename is same-volume (atomic).
    const fs::path temp_file = output_file.string() + ".ember-bundle-tmp";
    // RAII: remove the temp on scope exit unless release()d after a successful
    // rename. Uses the non-throwing remove so a cleanup race cannot terminate.
    struct TempGuard {
        fs::path path;
        bool keep = false;
        ~TempGuard() {
            if (!keep) { std::error_code r; fs::remove(path, r); }
        }
    } temp_guard{temp_file, false};
    // Belt-and-suspenders: if a previous crashed bundle left a stale temp,
    // remove it before we reuse the path (ignore failure — we'll catch a real
    // copy failure next).
    { std::error_code r; fs::remove(temp_file, r); }

    ec.clear();
    fs::copy_file(stub, temp_file, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr, "ember_bundle: copy stub to temp failed: %s\n", ec.message().c_str());
        return 2;
    }

    // Append the .em + footer to the temp (open in append mode so the stub
    // bytes already on disk are preserved; the copy above wrote them).
    std::ofstream out(temp_file, std::ios::binary | std::ios::app);
    if (!out) {
        std::fprintf(stderr, "ember_bundle: cannot open temp for append: %s\n", temp_file.u8string().c_str());
        return 2;
    }
    out.write(reinterpret_cast<const char*>(em_bytes.data()),
              static_cast<std::streamsize>(em_bytes.size()));
    // footer: u32 magic + u64 em_length
    std::vector<uint8_t> footer;
    append_u32_le(footer, EM_BUNDLE_MAGIC);
    append_u64_le(footer, em_bytes.size());
    out.write(reinterpret_cast<const char*>(footer.data()),
              static_cast<std::streamsize>(footer.size()));
    out.flush();
    if (!out) {
        std::fprintf(stderr, "ember_bundle: I/O error writing bundle\n");
        return 2;
    }
    out.close();
    if (!out) {  // close can also surface a delayed write error
        std::fprintf(stderr, "ember_bundle: I/O error closing bundle\n");
        return 2;
    }

    // Apply the chosen permission bits to the temp before the rename so the
    // published destination never appears with the wrong mode.
    fs::permissions(temp_file, output_perms, fs::perm_options::replace, ec);
    if (ec) {
        std::fprintf(stderr, "ember_bundle: cannot apply output permissions: %s\n", ec.message().c_str());
        return 2;  // temp removed by RAII; original output untouched
    }

    // Atomically replace the destination with the completed temp. On the
    // same volume this is a single MoveFileExW(MOVEFILE_REPLACE_EXISTING) /
    // rename(2) — the destination is replaced in one step or not at all, so a
    // reader never observes a half-written bundle and an existing output is
    // preserved on rename failure.
    ec.clear();
    fs::rename(temp_file, output_file, ec);
    if (ec) {
        std::fprintf(stderr, "ember_bundle: atomic replace failed: %s\n", ec.message().c_str());
        return 2;  // temp removed by RAII; original output untouched
    }
    temp_guard.keep = true;  // rename succeeded — temp is now the destination

    std::printf("ember_bundle: wrote %s (%zu .em bytes, %zu fns, entry slot %u, fn=%s, permissions=%s, output-permissions=%s)\n",
                output_file.u8string().c_str(), em_bytes.size(), emod.functions.size(),
                emod.entry_slot, fn_name.c_str(), permissions ? "ffi" : "none",
                preserve_output_permissions ? "preserve" : "stub");
    return 0;
}

} // namespace ember_bundle
#endif // !EMBER_BUNDLE_MAIN_ONLY
