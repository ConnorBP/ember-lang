// hotreload_demo.cpp - live module hot-reload + .em bundler + tick-loop demo.
//
// This is the demo the prior demo (demo/NOTES.md) skipped: it exercises the
// THREE runtime features that a multi-file language demo does not touch:
//
//   1. The `.em` bundler  -- write_em_file / load_em_file (BUNDLING_AND_EM_MODULES.md).
//      The harness compiles render_v1.ember, serializes it to a temp .em,
//      loads it back in a fresh code path, and calls the loaded renderer to
//      prove the bundle round-trips the same behavior as the JIT'd page.
//
//   2. HotReloadDomain (M3 epoch reclamation) -- reload_function + ExecutionGuard
//      + publish + reclaim/quiesce (HOT_RELOAD.md). The harness swaps the
//      `renderer` function body live, mid-tick-loop, from v1 -> v2 -> v3, and
//      asserts each reload publishes a monotonic epoch, the new page is callable
//      immediately after publication, and the old page is safely retired +
//      freed via quiesce (no UAF: the newest page is still callable after the
//      retired pages are freed).
//
//   3. --tick together -- the host runs a fixed, DETERMINISTIC tick loop (no
//      wall-clock, no keybind) that calls the renderer slot N times between
//      reloads. The reload happens between ticks; the next tick after
//      publication runs the NEW body, proving the swap takes effect atomically
//      (a tick in flight when publish happens finishes on the old page under
//      its guard; the next tick acquires a fresh guard + loads the new page).
//
// reload_function and HotReloadDomain are HOST C++ APIs (inline header); a
// pure-ember script CANNOT call them. So this is a C++ host harness that uses
// the ember frontend + JIT to compile/run a script `renderer`, and the host
// drives the reload. This mirrors examples/v0_6_hot_reload_test.cpp (the
// reload shape) + examples/game_host.cpp (the game-host compile + tick shape)
// + examples/em_roundtrip_test.cpp (the .em bundler shape), combined into one
// end-to-end live-reload demonstration.
//
// Build (the toolchain the task pins):
//   /c/msys64/mingw64/bin/g++.exe -std=c++17 -O2 -Isrc \
//     demo/hotreload/hotreload_demo.cpp buildt/libember_frontend.a buildt/libember.a \
//     -o demo/hotreload/hotreload_demo.exe
// (link order: frontend before ember; the extension include dirs are not
// needed because this harness uses BindingBuilder, not the extension headers
// directly.)

#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "engine.hpp"
#include "dispatch_table.hpp"
#include "binding_builder.hpp"
#include "lifecycle.hpp"
#include "globals.hpp"
#include "hot_reload.hpp"
#include "em_file.hpp"
#include "em_writer.hpp"
#include "em_loader.hpp"
#include "jit_memory.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace ember;

// ---------------------------------------------------------------------------
// host output capture (deterministic -- the natives append to this buffer)
// ---------------------------------------------------------------------------
// The renderer calls emit_frame(code) / emit_count(c) which append to a single
// host-owned string. The tick loop never touches wall-clock; the final string
// is the deterministic proof of the live-reload sequence.
static std::string g_out;

extern "C" {
static void n_emit_frame(int64_t code) { g_out += std::to_string(int(code)); g_out += ' '; }
static void n_emit_count(int64_t c)    { g_out += "c" + std::to_string(int(c)) + ' '; }
}

// ---------------------------------------------------------------------------
// the live module (mirrors examples/game_host.cpp's GameModule + v0_6's Mod)
// ---------------------------------------------------------------------------
struct LiveModule {
    std::vector<CompiledFn> fns;                 // current pages (host-owned while current)
    std::unique_ptr<DispatchTable> table;
    HotReloadDomain domain;                       // persistent beside the table (HOT_RELOAD.md §0)
    std::unordered_map<std::string,int> slots;
    GlobalsBlock gb; std::vector<uint8_t> gbs;
    Program prog;
    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable ov;
    StructLayoutTable layouts;

    LiveModule() : table(std::make_unique<DispatchTable>(0)) {}
    ~LiveModule() {
        // Shutdown recipe (HOT_RELOAD.md §0 step 6): guards drained (none active
        // here), domain quiesces (frees any still-retired pages), THEN free the
        // pages still current. The domain destructor runs its own final quiesce
        // after this block's body, but we free current pages first so the order
        // is unambiguous: current pages freed, then domain dtor quiesces (empty).
        for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);
    }

    // Disown the old CompiledFn for `name` on a successful reload: the domain
    // owns the replaced page now; nulling exec/entry here prevents the
    // destructor double-freeing it. (v0_6_hot_reload_test.cpp's disown_retired.)
    void disown_retired(const std::string& name, void* new_entry) {
        for (auto& fn : fns) {
            if (fn.name == name && fn.exec && fn.entry != new_entry) {
                fn.exec = nullptr;
                fn.entry = nullptr;
            }
        }
    }
    void accept(ReloadResult&& rr) {
        disown_retired(rr.new_fn.name, rr.new_fn.entry);
        fns.push_back(std::move(rr.new_fn));
    }
};

// read a .ember source file (the renderer versions live beside this exe).
static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

// compile a .ember source into a LiveModule (mirrors game_host's compile_game).
static std::unique_ptr<LiveModule> compile_module(const std::string& src,
                                                  const std::unordered_map<std::string, NativeSig>& natives) {
    auto m = std::make_unique<LiveModule>();
    auto lr = tokenize(src, "<hotreload>");
    if (!lr.ok) { std::fprintf(stderr, "lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::fprintf(stderr, "parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si = 0;
    for (auto& fn : m->prog.funcs) { m->slots[fn.name] = si++; fn.slot = m->slots[fn.name]; }
    m->layouts = build_struct_layouts(m->prog);
    m->prog.string_xor_key = 0;  // raw rodata (no __str_decrypt host native registered)
    auto sr = sema(m->prog, natives, m->slots, 0, nullptr, &m->layouts);
    if (!sr.ok) {
        std::fprintf(stderr, "sema:\n");
        for (auto& e : sr.errors) std::fprintf(stderr, "  line %u: %s\n", e.line, e.msg.c_str());
        return nullptr;
    }
    m->gbs.assign(m->prog.globals.size() * 8, 0);
    m->gb.base = int64_t(m->gbs.data());
    { uint32_t gi = 0; for (auto& g : m->prog.globals) { m->gb.index[g.name] = gi++; m->gb.types[g.name] = g.ty.get(); } }
    // seed const global initializers (v3's `global frame_count : i64 = 0` -> 0).
    eval_global_initializers(m->prog, GlobalInitCtx{m->gbs, m->gb.index, m->gb.types});
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());
    CodeGenCtx ctx; ctx.globals_base = m->gb.base; ctx.dispatch_base = int64_t(m->table->base());
    ctx.natives = &natives; ctx.script_slots = &m->slots; ctx.structs = &m->layouts;
    // v1.0 thread-safe globals resolution: thread the index+types through the
    // ctx (NOT the process-global g_globals_for_codegen pointer). Without this
    // a reloaded body that reads a global (v3's frame_count) cannot resolve the
    // global's slot index and emits a wild read. game_host / em_roundtrip use
    // the legacy g_globals_for_codegen path; the CLI uses this ctx path.
    ctx.globals_index = &m->gb.index;
    ctx.globals_types = &m->gb.types;
    for (auto& fn : m->prog.funcs) {
        auto cf = compile_func(fn, ctx);
        if (!finalize(cf)) { std::fprintf(stderr, "alloc_executable failed for %s\n", fn.name.c_str()); return nullptr; }
        m->table->set(fn.slot, cf.entry);
        m->fns.push_back(std::move(cf));
    }
    return m;
}

// guarded outer call (HOT_RELOAD.md §0 step 2: guard before loading the slot).
static int64_t call_renderer(LiveModule& m) {
    auto guard = m.domain.guard();
    void* e = m.table->get(m.slots["renderer"]);
    using F = int64_t(*)();
    return reinterpret_cast<F>(e)();
}

// build a CodeGenCtx matching the initial compile (reload_function needs one).
static CodeGenCtx make_ctx(LiveModule& m, const std::unordered_map<std::string, NativeSig>& natives) {
    CodeGenCtx ctx; ctx.globals_base = m.gb.base; ctx.dispatch_base = int64_t(m.table->base());
    ctx.natives = &natives; ctx.script_slots = &m.slots; ctx.structs = &m.layouts;
    ctx.globals_index = &m.gb.index;       // same thread-safe globals path as compile_module
    ctx.globals_types = &m.gb.types;
    return ctx;
}

// ---------------------------------------------------------------------------
// feature 1: the .em bundler round-trip
// ---------------------------------------------------------------------------
// Serialize the v1 module to a temp .em (the way ember_cli --emit-em does),
// load it back via load_em_file, and call the loaded renderer. Proves the
// bundler round-trips the same behavior as the JIT'd page (em_roundtrip shape).
static bool bundler_roundtrip(LiveModule& m, const std::unordered_map<std::string, NativeSig>& natives) {
    EmModule mod;
    mod.functions.reserve(m.fns.size());
    for (size_t i = 0; i < m.fns.size(); ++i) {
        const auto& cf = m.fns[i];
        const auto& decl = m.prog.funcs[i];
        EmFunctionRecord rec;
        rec.name = cf.name;
        rec.slot_index = uint32_t(decl.slot);
        rec.code = cf.bytes;
        rec.rodata = cf.rodata;
        rec.non_serializable_reason = cf.non_serializable_reason;
        rec.signature.ret = decl.ret ? *decl.ret : Type{};
        for (const auto& p : decl.params) rec.signature.params.push_back(p.ty ? *p.ty : Type{});
        for (const auto& af : cf.abs_fixups) { EmReloc r; r.offset = af.code_offset; r.kind = uint8_t(af.kind); r.addend = af.addend; rec.relocs.push_back(r); }
        for (const auto& nf : cf.native_fixups) { EmNativeBinding b; b.offset = nf.code_offset; b.name = nf.name; b.signature.ret = nf.ret; b.signature.params = nf.params; rec.native_bindings.push_back(std::move(b)); }
        mod.functions.push_back(std::move(rec));
    }
    mod.globals = m.gbs;
    mod.entry_slot = uint32_t(m.slots["renderer"]);
    mod.name_table.reserve(m.prog.funcs.size());
    for (const auto& fn : m.prog.funcs) mod.name_table.emplace_back(fn.name, uint32_t(fn.slot));

    auto tmp = std::filesystem::temp_directory_path() / "hotreload_demo_v1.em";
    std::string werr;
    if (!write_em_file(mod, tmp.string().c_str(), &werr)) {
        std::printf("[FAIL] .em bundler: write_em_file: %s\n", werr.c_str());
        return false;
    }
    std::printf("[PASS] .em bundler: wrote %s (%zu bytes, %zu fns)\n",
                tmp.string().c_str(), std::filesystem::file_size(tmp), mod.functions.size());

    LoadedModule lm;
    std::string lerr;
    if (!load_em_file(tmp.string().c_str(), lm, &lerr, nullptr, &natives)) {
        std::printf("[FAIL] .em bundler: load_em_file: %s\n", lerr.c_str());
        std::filesystem::remove(tmp);
        return false;
    }
    std::printf("[PASS] .em bundler: loaded back (%zu fns, entry_slot=%u)\n",
                lm.pages.size(), lm.entry_slot);

    // call the LOADED renderer (no reload domain for the throwaway loaded
    // module -- it is read-only after load; a single unguarded call is safe,
    // matching em_roundtrip_test's direct call_i64_i64 on the loaded entry).
    g_out.clear();
    void* loaded = lm.entry_by_name("renderer");
    if (!loaded) { std::printf("[FAIL] .em bundler: loaded renderer entry null\n"); std::filesystem::remove(tmp); return false; }
    using F = int64_t(*)();
    int64_t r = reinterpret_cast<F>(loaded)();
    std::printf("[PASS] .em bundler: loaded renderer ran (ret=%lld, out=\"%s\")\n",
                (long long)r, g_out.c_str());
    bool ok = (r == 1) && (g_out == "11 ");
    std::printf("[%s] .em bundler: round-trip behavior matches JIT v1\n", ok ? "PASS" : "FAIL");

    std::filesystem::remove(tmp);
    return ok;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: a SIGSEGV skips flush-on-exit
    std::printf("=== ember hot-reload + .em bundler + tick-loop demo ===\n");
    std::printf("(the three runtime features the prior demo skipped)\n\n");

    int failures = 0;
    auto ck = [&](bool cond, const char* msg) {
        std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
        if (!cond) failures++;
    };

    // ---- register the renderer natives via BindingBuilder ----
    BindingBuilder bb;
    bb.add("emit_frame", type_void(), {type_i64()}, (void*)&n_emit_frame);
    bb.add("emit_count", type_void(), {type_i64()}, (void*)&n_emit_count);
    NativeTable nt = bb.build();
    const auto& natives = nt.natives;

    // ---- locate the renderer .ember sources (beside this exe / from cwd) ----
    auto here = std::filesystem::path(__FILE__).parent_path();
    auto resolve = [&](const char* name) -> std::string {
        for (auto cand : { (here / name).string(), (std::filesystem::current_path() / "demo" / "hotreload" / name).string(),
                           (std::filesystem::current_path() / name).string() }) {
            if (std::filesystem::exists(cand)) return cand;
        }
        return (here / name).string();  // fall through to a readable error
    };
    std::string v1_path = resolve("render_v1.ember");
    std::string v2_path = resolve("render_v2.ember");
    std::string v3_path = resolve("render_v3.ember");

    std::string v1_src = read_file(v1_path);
    std::string v2_src = read_file(v2_path);
    std::string v3_src = read_file(v3_path);
    if (v1_src.empty() || v2_src.empty() || v3_src.empty()) {
        std::fprintf(stderr, "cannot read renderer sources (v1=%zu v2=%zu v3=%zu bytes)\n",
                     v1_src.size(), v2_src.size(), v3_src.size());
        std::fprintf(stderr, "  looked for: %s / %s / %s\n", v1_path.c_str(), v2_path.c_str(), v3_path.c_str());
        return 1;
    }

    // ---- compile the initial module (v1) ----
    auto m = compile_module(v1_src, natives);
    ck(m != nullptr, "compile renderer v1 (initial module)");
    if (!m) { std::printf("\nhot-reload demo: FAIL (could not compile v1)\n"); return 1; }
    ck(m->slots.count("renderer") && m->slots["renderer"] == 0, "renderer assigned slot 0");

    // ---- feature 1: the .em bundler round-trip (before the tick loop) ----
    std::printf("\n--- feature 1: the .em bundler (write_em_file / load_em_file) ---\n");
    bool bundler_ok = bundler_roundtrip(*m, natives);
    if (!bundler_ok) failures++;

    // ---- the deterministic live-reload sequence ----
    // 3 ticks v1 -> reload v2 -> 3 ticks v2 -> reload v3 -> 3 ticks v3.
    // The reload happens BETWEEN ticks; the next tick after publication runs
    // the new body. The final output string is the deterministic proof.
    //
    // The immediate-callability probe ("new page callable right after publish")
    // runs on a THROWAWAY buffer so it does not pollute the deterministic
    // sequence capture: probe_call saves g_out, clears it, calls, restores.
    std::printf("\n--- features 2+3: HotReloadDomain (epoch reclamation) + tick loop ---\n");
    g_out.clear();
    uint64_t prior_epoch = 0;
    auto probe_call = [&]() -> int64_t {
        std::string saved = std::move(g_out);
        g_out.clear();
        int64_t r = call_renderer(*m);
        g_out = std::move(saved);   // restore the sequence buffer; discard the probe output
        return r;
    };

    // 3 ticks of v1.
    std::printf("tick: 3 frames on v1...\n");
    for (int i = 0; i < 3; ++i) call_renderer(*m);
    ck(g_out == "11 11 11 ", "v1 tick sequence produced \"11 11 11 \"");
    g_out += "| ";

    // reload to v2, mid-loop.
    {
        auto ctx = make_ctx(*m, natives);
        auto rr = reload_function(v2_src, m->prog, *m->table, m->domain, ctx,
                                 natives, nullptr, &m->layouts);
        ck(rr.ok, "reload v1->v2 succeeded");
        if (!rr.ok) { std::printf("  [reload error] %s\n", rr.error.c_str()); failures++; }
        else {
            ck(rr.publication_epoch > prior_epoch, "v2 publication epoch is monotonic");
            ck(rr.old_page_retired, "v2 publication retired the v1 page to the domain");
            ck(rr.retirement_epoch == rr.publication_epoch, "v2 retirement epoch == publication epoch");
            prior_epoch = rr.publication_epoch;
            // newest page callable immediately after publication (probe buffer;
            // does not touch the sequence capture).
            ck(probe_call() == 1, "v2 page callable immediately after publication");
            m->accept(std::move(rr));
            // reclaim the retired v1 page (single-threaded: no in-flight guard).
            size_t freed = m->domain.quiesce();
            ck(freed == 1 && m->domain.retired_page_count() == 0, "quiesce freed the retired v1 page (no UAF)");
            ck(m->domain.reclaimed_page_count() == 1, "reclaimed-page counter == 1 after v1 retirement");
        }
    }

    // 3 ticks of v2 (the marker must have switched to 22).
    std::printf("tick: 3 frames on v2...\n");
    for (int i = 0; i < 3; ++i) call_renderer(*m);
    ck(g_out == "11 11 11 | 22 22 22 ", "v2 tick sequence produced \"22 22 22 \" after the live swap");
    g_out += "| ";

    // reload to v3 (uses the pre-existing frame_count global as a counter).
    {
        auto ctx = make_ctx(*m, natives);
        auto rr = reload_function(v3_src, m->prog, *m->table, m->domain, ctx,
                                 natives, nullptr, &m->layouts);
        ck(rr.ok, "reload v2->v3 succeeded (body uses the frame_count global as a counter)");
        if (!rr.ok) { std::printf("  [reload error] %s\n", rr.error.c_str()); failures++; }
        else {
            ck(rr.publication_epoch > prior_epoch, "v3 publication epoch is monotonic");
            ck(rr.old_page_retired, "v3 publication retired the v2 page to the domain");
            prior_epoch = rr.publication_epoch;
            ck(probe_call() == 1, "v3 page callable immediately after publication");
            m->accept(std::move(rr));
            size_t freed = m->domain.quiesce();
            ck(freed == 1 && m->domain.retired_page_count() == 0, "quiesce freed the retired v2 page (no UAF)");
            ck(m->domain.reclaimed_page_count() == 2, "reclaimed-page counter == 2 after v2 retirement");
            // The immediate-callability probe above genuinely executed v3 once,
            // which mutated the shared globals block (frame_count 0->1). That is
            // correct runtime behavior (the probe was a real call, not a no-op
            // peek). For a clean deterministic v3 sequence, reseed frame_count=0
            // -- the host seeding global state before a phase is exactly the
            // pattern the docs prescribe (a host or @entry seeds globals).
            // frame_count is global index 0 (the only global); zero its 8 bytes.
            std::memset(m->gbs.data(), 0, 8);
        }
    }

    // 3 ticks of v3 (marker 33 + advancing counter 1,2,3).
    std::printf("tick: 3 frames on v3 (counter advances 1,2,3)...\n");
    for (int i = 0; i < 3; ++i) call_renderer(*m);
    ck(g_out == "11 11 11 | 22 22 22 | 33 c1 33 c2 33 c3 ",
       "v3 tick sequence produced \"33 c1 33 c2 33 c3 \" (counter advances)");

    // capture the full deterministic sequence BEFORE the no-UAF probe reuses
    // the output buffer. This string is the canonical proof of the live-reload
    // order: v1 ticks | v2 ticks | v3 ticks (with the counter advancing).
    const std::string sequence = g_out;
    const std::string expected = "11 11 11 | 22 22 22 | 33 c1 33 c2 33 c3 ";

    // ---- the no-UAF + reclamation summary (a SEPARATE output buffer tick) ----
    std::printf("\n--- reclamation + no-UAF summary ---\n");
    ck(m->domain.reclaimed_page_count() == 2, "two pages reclaimed total (v1 + v2)");
    ck(m->domain.retired_page_count() == 0, "no retired pages remain pending");
    // the newest (v3) page is still callable AFTER both old pages were freed:
    // a call here would fault (0xC0000005) if quiesce had freed the live page.
    g_out.clear();
    call_renderer(*m);
    ck(g_out == "33 c4 ", "v3 page still callable after old pages reclaimed (no UAF; counter->4)");

    // ---- final deterministic output ----
    std::printf("\n=== deterministic reload-sequence output ===\n");
    ck(sequence == expected, "full sequence string matches the expected deterministic output");
    std::printf("expected: %s\n", expected.c_str());
    std::printf("actual:   %s\n", sequence.c_str());

    std::printf("\nhot-reload + .em bundler + tick-loop demo: %s\n",
                failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
