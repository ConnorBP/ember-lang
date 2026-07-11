// aggregate_global_test - pins aggregate globals (chunk c3):
//   global cfg : Config = Config { name_id: 42, scale: 2.0f };  (struct global)
//   global arr : i64[3] = [10, 20, 30];                          (fixed-array global)
//   global s   : i64[]  = [1, 2, 3];                              (slice global)
//
// Each probe compiles a tiny .ember through the full parse->sema->codegen->
// finalize->call pipeline (the em_roundtrip_test / binding_abi_test shape) and
// reads the i64 return value DIRECTLY via a C reinterpret of the JIT entry
// (call0_i64), asserting the exit code. This is a NON-CIRCULAR direct-value
// probe: the host reads the i64 return out of rax in C, not an in-language
// equality check that could circularly hide a codegen bug. A reverted fix
// (the typed globals-block layout removed, or the StructLit/ArrayLit const-
// fold removed, or the FieldExpr/IndexExpr global-base path removed) -> a
// wrong exit code -> this test records FAIL.
//
// Probes (each must PASS; a revert turns it red):
//   [1] struct global field read:  cfg.name_id == 42
//   [2] fixed-array global element read: arr[1] == 20
//   [3] slice global element read:  s[2] == 3
//   [4] struct global by-value arg: useit(cfg) == 42
//   [5] struct global by-value return: getit().name_id == 42
//   [6] struct global .em round-trip (H12): bake -> load -> cfg.name_id == 42
//   [7] fixed-array global .em round-trip (H12): bake -> load -> arr[1] == 20
//   [8] slice global .em round-trip (H12, relative-ptr relocation): bake -> load -> s[2] == 3
//
// Links the core libs (ember ember_frontend ember_import) + em_writer/em_loader
// for the .em round-trip probes. The probes are pure language features (no native
// calls), so no ember_ext_* link is needed (independent of the extension refactor).

#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/globals.hpp"
#include "../src/em_file.hpp"
#include "../src/em_writer.hpp"
#include "../src/em_loader.hpp"
#include "../src/jit_memory.hpp"
#include "../src/binding_builder.hpp"  // NativeTable (empty here - no natives used)

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

struct M {
    std::vector<CompiledFn> fns;
    std::unique_ptr<DispatchTable> table;
    std::unordered_map<std::string, int> slots;
    GlobalsBlock gb;
    std::vector<uint8_t> gbs;
    StructLayoutTable layouts;
    Program prog;
    M() : table(std::make_unique<DispatchTable>(0)) {}
};

static int g_fail = 0;
static void ck(bool c, const char* m) {
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// Per-type byte width (mirrors CG::value_bytes for the typed globals-block
// layout). The test computes its own typed layout so it is independent of
// ember_cli's helper.
static uint32_t host_value_bytes(const Type* t, const StructLayoutTable* structs) {
    if (!t) return 8;
    if (t->is_slice) return 16;
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

// Compute the typed globals-block layout: per-global (offset, size) + per-
// slice-global backing offset + total block size (mirrors ember_cli's
// compute_typed_globals_layout). 8-aligned slots; slice backings appended.
static void compute_typed_layout(const Program& prog, const StructLayoutTable& structs,
                                 std::unordered_map<std::string, uint32_t>& offsets,
                                 std::unordered_map<std::string, uint32_t>& sizes,
                                 std::unordered_map<std::string, uint32_t>& backing,
                                 uint32_t& total) {
    uint32_t cur = 0;
    auto align8 = [](uint32_t v) -> uint32_t { return (v + 7u) & ~7u; };
    for (const auto& g : prog.globals) {
        uint32_t sz = host_value_bytes(g.ty.get(), &structs);
        cur = align8(cur);
        offsets[g.name] = cur; sizes[g.name] = sz; cur += sz;
    }
    for (const auto& g : prog.globals) {
        if (!g.ty || !g.ty->is_slice || !g.init) continue;
        auto* al = dynamic_cast<const ArrayLit*>(g.init.get());
        if (!al) continue;
        uint32_t elem_sz = host_value_bytes(g.ty->elem.get(), &structs);
        if (elem_sz == 0) elem_sz = 8;
        cur = align8(cur);
        backing[g.name] = cur;
        cur += uint32_t(al->elements.size()) * elem_sz;
    }
    total = cur;
}

// Compile `src` through the full pipeline with the TYPED globals-block layout
// (c3). Returns nullptr + prints the stage on any failure (so a sema/codegen
// regression surfaces as a compile FAIL, not a wrong exit code).
static std::unique_ptr<M> compile(const std::string& src) {
    auto m = std::make_unique<M>();
    auto lr = tokenize(src, "<agg_global>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = m->slots[fn.name]; }
    NativeTable nt;  // empty: no natives used by these probes
    m->layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;  // raw rodata (no string natives here)
    auto sr = sema(m->prog, nt.natives, m->slots, 0, &nt.overloads, &m->layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return nullptr;
    }
    // Typed globals-block layout (c3): per-global offset + size + slice backing.
    std::unordered_map<std::string, uint32_t> offsets, sizes, backing;
    uint32_t total = 0;
    compute_typed_layout(m->prog, m->layouts, offsets, sizes, backing, total);
    m->gbs.assign(size_t(total), 0);
    m->gb.base = int64_t(m->gbs.data());
    { uint32_t gi = 0; for (auto& g : m->prog.globals) {
        m->gb.index[g.name] = gi++;
        m->gb.types[g.name] = g.ty.get();
        m->gb.offsets[g.name] = offsets[g.name];
        m->gb.sizes[g.name] = sizes[g.name];
    } }
    // Seed const + aggregate initializers at load (c3).
    GlobalInitCtx gic{m->gbs, m->gb.index, m->gb.types};
    gic.offsets = &m->gb.offsets;
    gic.sizes = &m->gb.sizes;
    gic.backing_offsets = &backing;
    gic.structs = &m->layouts;
    eval_global_initializers(m->prog, gic);
    g_globals_for_codegen = &m->gb;
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());
    CodeGenCtx ctx;
    ctx.globals_base = m->gb.base;
    ctx.globals_index = &m->gb.index;
    ctx.globals_types = &m->gb.types;
    ctx.globals_offsets = &m->gb.offsets;   // c3: typed offsets
    ctx.dispatch_base = int64_t(m->table->base());
    ctx.natives = &nt.natives;
    ctx.script_slots = &m->slots;
    ctx.structs = &m->layouts;
    for (auto& fn : m->prog.funcs) {
        auto cf = compile_func(fn, ctx);
        if (!finalize(cf)) { std::printf("FAIL: alloc_executable for %s\n", fn.name.c_str()); return nullptr; }
        m->table->set(fn.slot, cf.entry);
        m->fns.push_back(std::move(cf));
    }
    return m;
}

// Call a no-arg script fn that returns i64. The i64 return value IS the
// assertion signal (read directly out of rax by the C host - a non-circular
// direct-value probe, NOT an in-language comparison).
static int64_t call0_i64(M& m, const std::string& fn) {
    auto it = m.slots.find(fn);
    if (it == m.slots.end()) return -1;
    void* e = m.table->get(it->second);
    using F = int64_t (*)();
    return reinterpret_cast<F>(e)();
}

// Build an EmModule from JIT'd fns + the globals block + the program (mirrors
// em_roundtrip_test's build_em_module). entry = the named fn's slot.
static EmModule build_em_module(M& m, const std::string& entry_fn) {
    EmModule mod;
    mod.functions.reserve(m.fns.size());
    for (size_t i = 0; i < m.fns.size(); ++i) {
        const auto& cf = m.fns[i];
        const auto& decl = m.prog.funcs[i];
        EmFunctionRecord rec;
        rec.name = cf.name; rec.slot_index = uint32_t(decl.slot);
        rec.code = cf.bytes; rec.rodata = cf.rodata;
        rec.non_serializable_reason = cf.non_serializable_reason;
        rec.signature.ret = decl.ret ? *decl.ret : Type{};
        for (const auto& p : decl.params) rec.signature.params.push_back(p.ty ? *p.ty : Type{});
        for (const auto& af : cf.abs_fixups) { EmReloc r; r.offset = af.code_offset; r.kind = uint8_t(af.kind); r.addend = af.addend; rec.relocs.push_back(r); }
        mod.functions.push_back(std::move(rec));
    }
    mod.globals = m.gbs;
    auto sit = m.slots.find(entry_fn);
    mod.entry_slot = (sit != m.slots.end()) ? uint32_t(sit->second) : EM_NO_ENTRY;
    mod.name_table.reserve(m.prog.funcs.size());
    for (const auto& fn : m.prog.funcs) mod.name_table.emplace_back(fn.name, uint32_t(fn.slot));
    return mod;
}

int main() {
    std::printf("=== aggregate-globals regression (chunk c3) ===\n");
    std::printf("(pins struct / fixed-array / slice globals + .em round-trip)\n\n");

    {
        auto m = compile(
            "struct Config { name_id: i64; scale: f32; }\n"
            "global cfg : Config = Config { name_id: 42, scale: 2.0f };\n"
            "fn main() -> i64 { return cfg.name_id; }\n");
        if (!m) { ck(false, "[1] struct global field read (compile)"); }
        else { ck(call0_i64(*m, "main") == 42, "[1] struct global field read: cfg.name_id == 42"); }
    }
    {
        auto m = compile(
            "global arr : i64[3] = [10, 20, 30];\n"
            "fn main() -> i64 { return arr[1]; }\n");
        if (!m) { ck(false, "[2] fixed-array global element read (compile)"); }
        else { ck(call0_i64(*m, "main") == 20, "[2] fixed-array global element read: arr[1] == 20"); }
    }
    {
        auto m = compile(
            "global s : i64[] = [1, 2, 3];\n"
            "fn main() -> i64 { return s[2]; }\n");
        if (!m) { ck(false, "[3] slice global element read (compile)"); }
        else { ck(call0_i64(*m, "main") == 3, "[3] slice global element read: s[2] == 3"); }
    }
    {
        // C3 regression: g[..] view of a GLOBAL fixed array must yield a live
        // slice whose elements read back from the globals block. Before the
        // fix the tree-walker's ViewExpr case only handled a LOCAL fixed-array
        // base and emitted nothing for a global, so rax/rdx were garbage and
        // s[1] read a junk address; the thin-IR MakeSlice likewise used a
        // frame-relative lea instead of globals_base. Both paths now resolve
        // the global base, so g[..] -> slice {ptr = globals_base + offset,
        // len = 3} and s[1] reads 20. Non-circular: the host reads the i64
        // return out of rax in C.
        auto m = compile(
            "global g : i64[3] = [10, 20, 30];\n"
            "fn main() -> i64 { let s : i64[] = g[..]; return s[1]; }\n");
        if (!m) { ck(false, "[3b] global fixed-array view g[..] (compile)"); }
        else { ck(call0_i64(*m, "main") == 20, "[3b] global fixed-array view g[..]: s[1] == 20"); }
    }
    {
        // struct global passed by-value as a call arg (c1 interaction: the
        // struct-arg-copy block must fall back to the global base for a global
        // Ident, not look up a frame slot that doesn't exist).
        auto m = compile(
            "struct Config { name_id: i64; scale: f32; }\n"
            "global cfg : Config = Config { name_id: 42, scale: 2.0f };\n"
            "fn useit(c: Config) -> i64 { return c.name_id; }\n"
            "fn main() -> i64 { return useit(cfg); }\n");
        if (!m) { ck(false, "[4] struct global by-value arg (compile)"); }
        else { ck(call0_i64(*m, "main") == 42, "[4] struct global by-value arg: useit(cfg) == 42"); }
    }
    {
        // struct global returned by name `return cfg;` (c1 interaction: the
        // ReturnStmt Ident path's copy_struct_to_ptr must fall back to the
        // global base for a global Ident).
        auto m = compile(
            "struct Config { name_id: i64; scale: f32; }\n"
            "global cfg : Config = Config { name_id: 42, scale: 2.0f };\n"
            "fn getit() -> Config { return cfg; }\n"
            "fn main() -> i64 { let c: Config = getit(); return c.name_id; }\n");
        if (!m) { ck(false, "[5] struct global by-value return (compile)"); }
        else { ck(call0_i64(*m, "main") == 42, "[5] struct global by-value return: getit().name_id == 42"); }
    }

    // ---- .em round-trip (H12): bake -> load -> run in a fresh module ----
    // The slice global's ptr is stored as a RELATIVE offset within the block
    // at bake time, so the baked bytes round-trip through .em WITHOUT loader
    // fixup (codegen adds globals_base at runtime, and the loader's
    // GlobalsBase reloc fixes up the base address). [6]+[7] have no internal
    // pointers (struct + fixed-array globals are plain bytes). [8] is the
    // slice .em round-trip - the one the task flagged as a potential gap; the
    // relative-ptr approach makes it work without a loader change.
    auto roundtrip = [&](const std::string& src, const std::string& tag, int64_t expect) {
        auto m = compile(src);
        if (!m) { ck(false, (tag + " (compile)").c_str()); return; }
        // ground-truth JIT result
        int64_t jit = call0_i64(*m, "main");
        auto mod = build_em_module(*m, "main");
        auto tmp = std::filesystem::temp_directory_path() / "agg_global_test.em";
        std::string werr;
        if (!write_em_file(mod, tmp.string().c_str(), &werr)) {
            std::printf("FAIL: write_em_file: %s\n", werr.c_str()); g_fail = 1; return;
        }
        LoadedModule lm; std::string lerr;
        std::unordered_map<std::string, NativeSig> empty_natives;
        if (!load_em_file(tmp.string().c_str(), lm, &lerr, nullptr, &empty_natives)) {
            std::printf("FAIL: load_em_file: %s\n", lerr.c_str()); g_fail = 1;
            std::filesystem::remove(tmp); return;
        }
        void* loaded = lm.entry();
        if (!loaded) { ck(false, (tag + " (load entry null)").c_str()); std::filesystem::remove(tmp); return; }
        using F = int64_t (*)();
        int64_t ld = reinterpret_cast<F>(loaded)();
        std::filesystem::remove(tmp);
        ck(ld == expect && ld == jit, (tag + ": load == " + std::to_string(expect)).c_str());
    };

    roundtrip(
        "struct Config { name_id: i64; scale: f32; }\n"
        "global cfg : Config = Config { name_id: 42, scale: 2.0f };\n"
        "fn main() -> i64 { return cfg.name_id; }\n",
        "[6] struct global .em round-trip", 42);
    roundtrip(
        "global arr : i64[3] = [10, 20, 30];\n"
        "fn main() -> i64 { return arr[1]; }\n",
        "[7] fixed-array global .em round-trip", 20);
    roundtrip(
        "global s : i64[] = [1, 2, 3];\n"
        "fn main() -> i64 { return s[2]; }\n",
        "[8] slice global .em round-trip (relative-ptr relocation)", 3);

    std::printf("\naggregate-globals regression: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
