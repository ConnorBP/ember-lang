// v0.6_hot_reload_test - single-function hot reload + epoch reclamation.
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "engine.hpp"
#include "dispatch_table.hpp"
#include "binding_builder.hpp"
#include "hot_reload.hpp"

#include "ext_vec.hpp"
#include "ext_math.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace ember;
static int g_fail = 0;
static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

struct Mod {
    // Each current page stays here. When it is replaced, reload_function moves
    // page ownership logically to domain; disown_retired prevents double-free.
    std::vector<CompiledFn> fns;
    std::unique_ptr<DispatchTable> table;
    HotReloadDomain domain;
    std::unordered_map<std::string,int> slots;
    GlobalsBlock gb; std::vector<uint8_t> gbs;
    Program prog;
    std::unordered_map<std::string,NativeSig> natives;
    OpOverloadTable ov;
    StructLayoutTable layouts;
    Mod() : table(std::make_unique<DispatchTable>(0)) {}
    ~Mod() { for (auto& fn : fns) if (fn.exec) free_executable(fn.exec); }

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

static std::unique_ptr<Mod> compile(const std::string& src) {
    auto m = std::make_unique<Mod>();
    auto lr = tokenize(src, "<t>"); if (!lr.ok) return nullptr;
    auto pr = parse(std::move(lr.toks)); if (!pr.ok) return nullptr;
    m->prog = std::move(pr.program);
    int si=0; for (auto& fn : m->prog.funcs) { m->slots[fn.name]=si++; fn.slot=m->slots[fn.name]; }
    ext_vec::register_natives(m->natives); ext_math::register_natives(m->natives);
    ext_vec::register_overloads(m->ov);
    m->layouts = build_struct_layouts(m->prog); m->prog.string_xor_key=0;
    if (!sema(m->prog, m->natives, m->slots, 0, &m->ov, &m->layouts).ok) return nullptr;
    m->gbs.assign(m->prog.globals.size()*8, 0); m->gb.base = int64_t(m->gbs.data());
    { uint32_t gi=0; for (auto& g : m->prog.globals) { m->gb.index[g.name]=gi++; m->gb.types[g.name]=g.ty.get(); } }
    g_globals_for_codegen = &m->gb;
    m->table = std::make_unique<DispatchTable>(m->prog.funcs.size());
    CodeGenCtx ctx; ctx.globals_base=m->gb.base; ctx.dispatch_base=int64_t(m->table->base());
    ctx.natives=&m->natives; ctx.script_slots=&m->slots; ctx.structs=&m->layouts;
    for (auto& fn : m->prog.funcs) {
        auto cf=compile_func(fn,ctx);
        if (!finalize(cf)) return nullptr;
        m->table->set(fn.slot,cf.entry);
        m->fns.push_back(std::move(cf));
    }
    return m;
}
static int64_t call_void(Mod& m, const std::string& fn) {
    auto guard = m.domain.guard();
    void* e = m.table->get(m.slots[fn]);
    using F=int64_t(*)(); return reinterpret_cast<F>(e)();
}
static int64_t call_i64(Mod& m, const std::string& fn, int64_t a) {
    auto guard = m.domain.guard();
    void* e = m.table->get(m.slots[fn]);
    using F=int64_t(*)(int64_t); return reinterpret_cast<F>(e)(a);
}
static CodeGenCtx make_ctx(Mod& m) {
    CodeGenCtx ctx; ctx.globals_base=m.gb.base; ctx.dispatch_base=int64_t(m.table->base());
    ctx.natives=&m.natives; ctx.script_slots=&m.slots; ctx.structs=&m.layouts;
    return ctx;
}
static ReloadResult reload(Mod& m, const std::string& source) {
    auto ctx = make_ctx(m);
    return reload_function(source, m.prog, *m.table, m.domain, ctx,
                           m.natives, &m.ov, &m.layouts);
}
static void accept(Mod& m, ReloadResult&& rr) { m.accept(std::move(rr)); }

int main() {
    std::printf("=== v0.6 single-function hot reload tests ===\n");

    // (1) Repeated publication: epochs increase, every newest page executes,
    // all replaced pages are reclaimed, and the current page remains callable.
    {
        auto m = compile("fn val() -> i64 { return 10; }\nfn main() -> i64 { return val(); }\n");
        check(m != nullptr, "compile module");
        check(call_void(*m,"main")==10, "(1) initial entry callable");
        const int values[] = {20, 30, 40, 50, 60};
        uint64_t prior_epoch = 0;
        for (int value : values) {
            auto rr = reload(*m, "fn val() -> i64 { return " + std::to_string(value) + "; }\n");
            check(rr.ok, "(1) repeated reload succeeded");
            check(rr.publication_epoch > prior_epoch && rr.old_page_retired &&
                  rr.retirement_epoch == rr.publication_epoch,
                  "(1) successful publication reports monotonic retirement epoch");
            prior_epoch = rr.publication_epoch;
            check(call_void(*m,"main") == value, "(1) newest entry callable after publication");
            accept(*m, std::move(rr));
        }
        check(m->domain.retired_page_count() == 5, "(1) all five replaced pages tracked by domain");
        check(m->domain.quiesce() == 5, "(1) single-threaded quiesce freed all replaced pages");
        check(m->domain.retired_page_count() == 0 && m->domain.reclaimed_page_count() == 5,
              "(1) retired set empty and freed-page counter proves reclamation");
        check(call_void(*m,"main") == 60, "(1) newest entry remains callable after reclamation");
    }

    // (2) Recursive/caller dispatch still observes the replacement.
    {
        auto m = compile("fn fib(n: i64) -> i64 { if (n <= 1) { return n; } return fib(n-1)+fib(n-2); }\nfn main() -> i64 { return fib(5); }\n");
        check(call_void(*m,"main")==5, "(2) pre-reload fib(5)==5");
        auto rr = reload(*m, "fn fib(n: i64) -> i64 { return n * 2; }\n");
        check(rr.ok, "(2) reload fib succeeded");
        check(call_i64(*m,"fib",5)==10, "(2) caller observes replacement through stable slot");
        accept(*m, std::move(rr));
        check(m->domain.quiesce() == 1, "(2) old recursive page reclaimed");
    }

    // (3) Failed parse/sema/signature reloads do not publish or advance epoch.
    {
        auto m = compile("fn val(x: i64) -> i64 { return x + 1; }\nfn main() -> i64 { return val(6); }\n");
        const uint64_t before = m->domain.epoch();
        auto type_error = reload(*m, "fn val(x: i64) -> i64 { return 1.5f; }\n");
        check(!type_error.ok && m->domain.epoch() == before && call_void(*m,"main")==7,
              "(3a) sema failure leaves body and epoch untouched");
        auto arity = reload(*m, "fn val() -> i64 { return 9; }\n");
        check(!arity.ok && m->domain.epoch() == before && call_void(*m,"main")==7,
              "(3b) changed arity rejected without publication");
        auto param = reload(*m, "fn val(x: f32) -> i64 { return 9; }\n");
        check(!param.ok && m->domain.epoch() == before, "(3c) changed parameter rejected without epoch advance");
        auto ret = reload(*m, "fn val(x: i64) -> f32 { return 9.0f; }\n");
        check(!ret.ok && m->domain.epoch() == before, "(3d) changed return rejected without epoch advance");
        auto absent = reload(*m, "fn nope() -> i64 { return 1; }\n");
        check(!absent.ok && m->domain.epoch() == before, "(3e) absent function rejected without epoch advance");
    }

    // (4) A pre-publication guard pins the old page. Nonblocking reclamation
    // must refuse it; blocking quiesce must wait until that guard leaves.
    {
        auto m = compile("fn val() -> i64 { return 111; }\n");
        auto held = std::make_unique<HotReloadDomain::ExecutionGuard>(m->domain.guard());
        void* old_entry = m->table->get(m->slots["val"]); // valid only under held
        auto rr = reload(*m, "fn val() -> i64 { return 222; }\n");
        check(rr.ok && rr.publication_epoch == 1, "(4) publish while old execution guard active");
        accept(*m, std::move(rr));
        check(m->domain.reclaim() == 0 && m->domain.retired_page_count() == 1,
              "(4) nonblocking reclaim refuses page pinned by in-flight guard");
        using F=int64_t(*)();
        check(reinterpret_cast<F>(old_entry)() == 111,
              "(4) old page remains executable while its guard is active");

        std::atomic<bool> started{false};
        std::atomic<bool> finished{false};
        size_t freed = 0;
        std::thread waiter([&] {
            started.store(true, std::memory_order_release);
            freed = m->domain.quiesce();
            finished.store(true, std::memory_order_release);
        });
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        check(!finished.load(std::memory_order_acquire),
              "(4) blocking quiesce waits while old guard is active");
        held.reset();
        waiter.join();
        check(finished.load(std::memory_order_acquire) && freed == 1 &&
              m->domain.retired_page_count() == 0,
              "(4) quiesce frees old page after guard drains");
        check(call_void(*m,"val") == 222, "(4) newest page callable after concurrent quiescence");
    }

    // (5) A null dispatch entry is a host-API misuse, not a process fault.
    // DispatchTable::set rejects null with std::invalid_argument so the host
    // can recover (try/catch) instead of taking 0xC0000005 at the call site.
    {
        DispatchTable t(1);
        bool threw_invalid = false;
        try { t.set(0, nullptr); }
        catch (const std::invalid_argument&) { threw_invalid = true; }
        catch (...) {}
        check(threw_invalid && t.get(0) == nullptr,
              "(5) null DispatchTable::set throws std::invalid_argument (recoverable, not 0xC0000005)");
    }

    // (6) Same-table alias: two slots in one table both hold page X; replacing
    // slot 0 with Y must NOT retire/free X while slot 1 still publishes it.
    // A guarded call through slot 1 after reclaim must still execute X
    // (calling a freed page would fault). This is the M-M3-2 regression: the
    // page is uniquely bound to one (domain,table,slot) for retirement, so a
    // page still current in another slot is not reclaimed.
    {
        auto make_ret = [](int v) -> void* {
            std::vector<uint8_t> code{0xB8, uint8_t(v), uint8_t(v >> 8),
                                      uint8_t(v >> 16), uint8_t(v >> 24), 0xC3};
            return alloc_executable(code);
        };
        auto call_entry = [](void* e) -> int {
            return reinterpret_cast<int(*)()>(e)();
        };
        DispatchTable t(2);
        HotReloadDomain d;
        void* x = make_ret(91);
        void* y = make_ret(92);
        t.set(0, x);
        t.set(1, x);              // alias: both slots publish X
        auto pub = d.publish(t, 0, y);   // replace slot 0 with Y
        size_t freed = d.reclaim();       // must NOT free X (slot 1 still holds it)
        bool slot0_y = t.get(0) == y && call_entry(t.get(0)) == 92;
        bool slot1_x = t.get(1) == x;
        {  // guarded call through slot 1 — X must still be executable
            auto g = d.guard();
            bool slot1_callable = slot1_x && call_entry(t.get(1)) == 91;
            check(pub.ok && freed == 0 && slot0_y && slot1_callable,
                  "(6) same-table alias: replacing slot 0 does not free X while slot 1 holds it");
        }
        // Now replace slot 1 too; X is no longer current anywhere and is retired.
        auto pub2 = d.publish(t, 1, make_ret(93));
        size_t freed2 = d.reclaim();
        check(pub2.ok && pub2.old_page_retired && freed2 == 1,
              "(6) replacing last alias slot retires and frees X");
        free_executable(y);
        free_executable(t.get(0));
        free_executable(t.get(1));
    }

    // (7) Migration-recipe shape: a persistent HotReloadDomain stored beside the
    // DispatchTable for the table's lifetime; guard before EVERY outer call;
    // disown the old CompiledFn on success (don't free it - the domain owns it);
    // reclaim after reload; and at shutdown drain guards then quiesce before
    // freeing current pages. This mirrors the docs/HOT_RELOAD.md Section 0 recipe.
    // It is a non-circular regression for the domain-based API: reverting the
    // old_entry-removing / HotReloadDomain& signature change would not compile.
    {
        // 1. Persistent domain beside the table for the module's lifetime.
        auto m = compile("fn val() -> i64 { return 7; }\nfn main() -> i64 { return val(); }\n");
        check(m != nullptr, "(7) compile migration-shape module");
        // 2. Guard before every outer call.
        check(call_void(*m, "main") == 7, "(7) initial call guarded (recipe step 2)");
        // 3+4. Reload: domain owns the replaced page; host disowns the old CompiledFn.
        auto rr = reload(*m, "fn val() -> i64 { return 9; }\n");
        check(rr.ok, "(7) domain-based reload succeeds");
        check(rr.old_page_retired, "(7) replaced page transferred to domain (no caller-owned old_entry)");
        accept(*m, std::move(rr));  // disowns the old fn, keeps the new
        // 2 again: guard before the post-reload call.
        check(call_void(*m, "main") == 9, "(7) post-reload call guarded");
        // 5. Periodic reclaim frees retired pages.
        check(m->domain.reclaim() == 1 && m->domain.retired_page_count() == 0,
              "(7) reclaim frees retired page (recipe step 5)");
        check(call_void(*m, "main") == 9, "(7) newest page callable after reclaim");
        // 6. Shutdown: guards drained (no active guards here), quiesce, then free
        // current pages (the destructor does this); the domain quiesces first.
        size_t shutdown_freed = m->domain.quiesce();
        check(shutdown_freed == 0, "(7) shutdown quiesce after reclaim finds nothing left");
        // The Mod destructor drains (no guards), frees current pages, then the
        // domain destructor runs a final quiesce. This is the recipe's teardown.
    }

    std::printf("\nv0.6 hot reload test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
