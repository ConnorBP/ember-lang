// game_host - the v1.0 example game-engine integration.
//
// Proves ember works as a game-logic embedding target: a host compiles+runs a
// script with @entry / @on_tick / @event, the script drives HOST-OWNED entity
// state through natives registered via BindingBuilder, and the host hot-reloads
// a gameplay fn mid-loop. This is the first real "ember as embedding target"
// proof (every prior test is a language-feature test, not an end-to-end host).
//
// The shape the spec's v1.0 milestone asks for: "example game-engine integration
// (event hooks via annotations)." The event hooks shipped in v0.6 (lifecycle);
// this is the integration that uses them for real.
//
// natives (host C++ via BindingBuilder, the existing API — no fluent
// TypeBuilder needed; this is the test of whether that trigger fires):
//   get_entity_count() -> i64
//   get_entity_x(i) -> f32 / set_entity_x(i, x)
//   get_delta_time() -> f32
//   log(code) -> void   (a script->host signal, asserted in the test)
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
#include "context.hpp"

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace ember;

// ---- host-owned game state (the script reads/writes it through natives) ----
struct Entity { float x, y; };
static std::vector<Entity> g_entities;
static float g_dt = 0.016f;       // ~60fps frame delta
static float g_speed = 10.0f;     // host-owned movement speed (get/set_speed natives)
static int g_last_log_code = 0;    // the log() native records the script's signal here

extern "C" {
static int64_t n_get_entity_count() { return int64_t(g_entities.size()); }
static float    n_get_entity_x(int64_t i) {
    if (i < 0 || size_t(i) >= g_entities.size()) return 0.0f;
    return g_entities[size_t(i)].x;
}
static void     n_set_entity_x(int64_t i, float x) {
    if (i < 0 || size_t(i) >= g_entities.size()) return;
    g_entities[size_t(i)].x = x;
}
static float    n_get_entity_y(int64_t i) {
    if (i < 0 || size_t(i) >= g_entities.size()) return 0.0f;
    return g_entities[size_t(i)].y;
}
static void     n_set_entity_y(int64_t i, float y) {
    if (i < 0 || size_t(i) >= g_entities.size()) return;
    g_entities[size_t(i)].y = y;
}
static float    n_get_delta_time() { return g_dt; }
static float    n_get_speed() { return g_speed; }
static void     n_set_speed(float s) { g_speed = s; }
static void     n_log(int64_t code) { g_last_log_code = int(code); }
}

// ---- compile a .ember file to a runnable module (mirrors ember_cli) ----
struct GameModule {
    std::vector<CompiledFn> fns;
    std::unique_ptr<DispatchTable> table;
    HotReloadDomain reload_domain;
    std::unordered_map<std::string,int> slots;
    GlobalsBlock gb; std::vector<uint8_t> gbs;
    Program prog;
    std::vector<std::string> rodata;
    GameModule() : table(std::make_unique<DispatchTable>(0)) {}
};

static std::unique_ptr<GameModule> compile_game(const std::string& src, const NativeTable& natives) {
    auto m = std::make_unique<GameModule>();
    auto lr = tokenize(src, "<game>"); if (!lr.ok) { std::fprintf(stderr, "lex: %s\n", lr.error.c_str()); return nullptr; }
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) { std::fprintf(stderr, "parse: %s\n", pr.error.c_str()); return nullptr; }
    m->prog = std::move(pr.program);
    int si=0; for (auto& fn : m->prog.funcs) { m->slots[fn.name]=si++; fn.slot=m->slots[fn.name]; }
    OpOverloadTable ov;  // game natives have no operator overloads
    auto layouts = build_struct_layouts(m->prog); m->prog.string_xor_key = 0;
    auto sr = sema(m->prog, natives.natives, m->slots, 0, &ov, &layouts);
    if (!sr.ok) { std::fprintf(stderr, "sema:\n"); for (auto& e : sr.errors) std::fprintf(stderr, "  line %u: %s\n", e.line, e.msg.c_str()); return nullptr; }
    m->gbs.assign(m->prog.globals.size()*8, 0); m->gb.base = int64_t(m->gbs.data());
    { uint32_t gi=0; for (auto& g : m->prog.globals) { m->gb.index[g.name]=gi++; m->gb.types[g.name]=g.ty.get(); } }
    g_globals_for_codegen = &m->gb;
    eval_global_initializers(m->prog, GlobalInitCtx{m->gbs, m->gb.index, m->gb.types});  // v1.0: seed const global inits
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());
    CodeGenCtx ctx; ctx.globals_base=m->gb.base; ctx.dispatch_base=int64_t(m->table->base());
    ctx.natives=&natives.natives; ctx.script_slots=&m->slots; ctx.structs=&layouts;
    for (auto& fn : m->prog.funcs) { auto cf=compile_func(fn,ctx); finalize(cf); m->table->set(fn.slot,cf.entry); m->fns.push_back(std::move(cf)); }
    return m;
}
static int64_t call_void(GameModule& m, const std::string& fn) {
    auto guard = m.reload_domain.guard();
    void* e = m.table->get(m.slots[fn]); using F=int64_t(*)(); return reinterpret_cast<F>(e)();
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path); if (!f) return "";
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: a SIGSEGV skips flush-on-exit
    std::printf("=== ember v1.0 example game-engine integration ===\n");

    // locate the game_logic.ember script (next to this exe in examples/scripts/, or argv[1])
    std::string script = (argc > 1) ? argv[1]
        : (std::filesystem::path(__FILE__).parent_path() / "scripts" / "game_logic.ember").string();
    // fallback: try relative to cwd (the CTest run dir is the build dir; examples/scripts is one up)
    if (!std::filesystem::exists(script)) {
        for (auto cand : { "examples/scripts/game_logic.ember", "../examples/scripts/game_logic.ember" }) {
            if (std::filesystem::exists(cand)) { script = cand; break; }
        }
    }
    std::string src = read_file(script);
    if (src.empty()) { std::fprintf(stderr, "game_host: cannot read %s\n", script.c_str()); return 1; }

    // ---- register the game natives via BindingBuilder (the existing API) ----
    BindingBuilder bb;
    bb.add("get_entity_count", type_i64(), {}, (void*)&n_get_entity_count);
    bb.add("get_entity_x", type_f32(), {type_i64()}, (void*)&n_get_entity_x);
    bb.add("set_entity_x", type_void(), {type_i64(), type_f32()}, (void*)&n_set_entity_x);
    bb.add("get_entity_y", type_f32(), {type_i64()}, (void*)&n_get_entity_y);
    bb.add("set_entity_y", type_void(), {type_i64(), type_f32()}, (void*)&n_set_entity_y);
    bb.add("get_delta_time", type_f32(), {}, (void*)&n_get_delta_time);
    bb.add("get_speed", type_f32(), {}, (void*)&n_get_speed);
    bb.add("set_speed", type_void(), {type_f32()}, (void*)&n_set_speed);
    bb.add("log", type_void(), {type_i64()}, (void*)&n_log);
    NativeTable natives = bb.build();

    // ---- host game state: 3 entities at x=0,1,2; speed=10 (host-owned) ----
    g_entities = { {0.0f,0.0f}, {1.0f,0.0f}, {2.0f,0.0f} };
    g_dt = 1.0f;  // 1s per tick so movement is visible (speed=10 -> +10/tick)
    g_speed = 10.0f;
    g_last_log_code = 0;

    auto m = compile_game(src, natives);
    if (!m) { std::fprintf(stderr, "game_host: compile failed\n"); return 1; }

    int failures = 0;
    auto ck = [&](bool cond, const char* msg){ std::printf("[%s] %s\n", cond?"PASS":"FAIL", msg); if(!cond) failures++; };

    // ---- @entry: run once, must return >0 (stay loaded) + log(100) ----
    auto entry = get_entry_function(m->prog);
    ck(entry != nullptr, "@entry discovered");
    int64_t entry_r = call_void(*m, entry->name);
    ck(entry_r > 0, "@entry returned >0 (stay loaded)");
    ck(g_last_log_code == 100, "@entry called log(100) (script->host signal works)");

    // ---- @on_tick: 5 frames, each entity moves right by speed*dt = 10*1 = 10 ----
    auto ticks = get_annotated_functions(m->prog, "@on_tick");
    ck(ticks.size() == 1, "@on_tick discovered (1 fn)");
    float x0_before = g_entities[0].x;
    for (int f = 0; f < 5; ++f) for (auto& af : ticks) call_void(*m, af.name);
    ck(g_entities[0].x == x0_before + 50.0f, "5 @on_tick frames moved entity[0] by +50 (10/frame)");
    ck(g_entities[2].x == 2.0f + 50.0f, "entity[2] moved by +50 too (script iterated all entities)");

    // ---- @event("player_input"): fires once, doubles g_speed + log(200) ----
    // g_speed is host-owned; the handler updates it through get/set natives.
    auto handlers = get_event_handlers(m->prog, "player_input");
    ck(handlers.size() == 1, "@event(\"player_input\") discovered (1 handler)");
    g_last_log_code = 0;
    for (auto& af : handlers) call_void(*m, af.name);
    ck(g_last_log_code == 200, "@event called log(200)");
    // g_speed is host-owned now (set_speed native); @event doubled it 10->20.
    ck(g_speed == 20.0f, "@event doubled host speed 10->20 (via set_speed native)");

    // ---- hot reload mid-loop: reload `update` to move LEFT (x - speed*dt) ----
    // Build the codegen ctx matching the initial compile.
    OpOverloadTable ov; auto layouts = build_struct_layouts(m->prog);
    CodeGenCtx ctx; ctx.globals_base=m->gb.base; ctx.dispatch_base=int64_t(m->table->base());
    ctx.natives=&natives.natives; ctx.script_slots=&m->slots; ctx.structs=&layouts;
    float x0_before_reload = g_entities[0].x;
    auto rr = reload_function(
        "fn update() -> i64 { let n : i64 = get_entity_count(); let dt : f32 = get_delta_time();"
        " let speed : f32 = get_speed(); let mut i : i64 = 0; while (i < n) { let x : f32 = get_entity_x(i);"
        " set_entity_x(i, x - speed * dt); i = i + 1; } return n; }\n",
        m->prog, *m->table, m->reload_domain, ctx, natives.natives, &ov, &layouts);
    ck(rr.ok, "hot reload of `update` succeeded (mid-loop)");
    if (!rr.ok) std::printf("  [reload error] %s\n", rr.error.c_str());
    if (rr.ok) {
        // 5 more frames, now moving LEFT by 20*1 = 20/frame (speed is 20 after the event)
        for (int f = 0; f < 5; ++f) for (auto& af : ticks) call_void(*m, af.name);
        ck(g_entities[0].x == x0_before_reload - 100.0f, "post-reload: 5 frames moved entity[0] LEFT by 100 (20/frame, new body)");
        // The domain owns the retired page; the host retains only the new page.
        for (auto& fn : m->fns) if (fn.name == rr.new_fn.name) { fn.exec = nullptr; fn.entry = nullptr; }
        m->fns.push_back(std::move(rr.new_fn));
        ck(m->reload_domain.quiesce() == 1, "retired update page reclaimed at host quiescence");
    }

    std::printf("\n=== binding API friction check (v1.0 fluent-API decision) ===\n");
    std::printf("The host used BindingBuilder + i64 handles + StructLayoutTable for all game\n");
    std::printf("natives. No friction hit — the fluent TypeBuilder/StructBuilder/engine_t\n");
    std::printf("surface is NOT needed for this integration (the v0.3 re-entry trigger\n");
    std::printf("remains unfired). Defer per the v0.3 deferred-binding analysis.\n");

    std::printf("\ngame_host integration: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
