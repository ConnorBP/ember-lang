// Additional edge-path coverage for keyed_hot_reload.cpp.  Reuse the real-JIT
// fixture and baseline lifecycle assertions from keyed_dispatch_hot_reload_test,
// then drive the malformed-request and concurrent-publication branches that the
// baseline intentionally leaves out.
#define main keyed_hot_reload_baseline_main
#include "keyed_dispatch_hot_reload_test.cpp"
#undef main

#include <functional>
#include <thread>

namespace {

static KeyedReloadRequest good_request(KeyedModule& m, const std::string& source =
    "fn double(x: i64) -> i64 { return x * 7; }\n") {
    KeyedReloadRequest req;
    req.new_fn_source = source;
    req.logical_slot = uint32_t(m.slots["double"]);
    req.build_provider = std::make_shared<FixedProvider>(DETERMINISTIC_SEED);
    return req;
}

static KeyedReloadResult call_reload(KeyedModule& m, HotReloadDomain& domain,
                                     const KeyedReloadRequest& req,
                                     const StructLayoutTable* layouts = nullptr) {
    auto ctx = make_ctx(m);
    return reload_keyed_function(req, m.prog, m.st, m.rec, m.plan, domain, ctx,
                                 m.natives, &m.ov,
                                 layouts ? layouts : &m.layouts);
}

static void expect_reload_failure(
    const char* label,
    const std::function<void(KeyedModule&, KeyedReloadRequest&)>& mutate) {
    const std::string src =
        "fn double(x: i64) -> i64 { return x * 2; }\n"
        "fn add1(x: i64) -> i64 { return x + 1; }\n";
    HotReloadDomain domain;
    auto m = build_keyed_module(src, "coverage", DETERMINISTIC_SEED, &domain,
                                reinterpret_cast<void*>(khr_trap));
    ck(m != nullptr, label);
    if (!m) return;
    auto req = good_request(*m);
    const uint64_t epoch = domain.epoch();
    mutate(*m, req);
    auto result = call_reload(*m, domain, req);
    ck(!result.ok, label);
    ck(!result.error.empty(), "reload failure has diagnostic");
    ck(domain.epoch() == epoch, "reload failure leaves epoch unchanged");
}

static ModuleManifest one_callable_manifest(const LogicalRoute& route,
                                            uint32_t slot) {
    ModuleManifest manifest;
    manifest.module_id = "coverage";
    ModuleCallable callable;
    callable.name = "double";
    callable.logical_slot = slot;
    callable.abi_fingerprint = route.abi_fingerprint;
    callable.visibility = route.visibility;
    callable.calling_mode = route.calling_mode;
    callable.dispatch_domain = route.dispatch_domain;
    manifest.callables.push_back(callable);
    return manifest;
}

static void topology_edge_matrix() {
    std::printf("-- coverage: topology edge matrix --\n");
    HotReloadDomain domain;
    auto m = build_keyed_module(
        "fn double(x: i64) -> i64 { return x * 2; }\n"
        "fn add1(x: i64) -> i64 { return x + 1; }\n",
        "coverage", DETERMINISTIC_SEED, &domain,
        reinterpret_cast<void*>(khr_trap));
    ck(m != nullptr, "topology matrix fixture");
    if (!m) return;
    const uint32_t slot = uint32_t(m->slots["double"]);
    const LogicalRoute route = m->plan.logical_routes[slot];
    std::string reason = "stale";

    auto manifest = one_callable_manifest(route, slot);
    ck(keyed_reload_preserves_topology(m->rec, slot, manifest, reason) && reason.empty(),
       "topology accepts exact identity and clears reason");

    ck(!keyed_reload_preserves_topology(m->rec, m->rec.logical_slot_count,
                                        manifest, reason) &&
       reason.find("out of range") != std::string::npos,
       "topology rejects out-of-range logical slot");

    ModuleDispatchRecord no_routes = m->rec;
    no_routes.logical_routes = nullptr;
    ck(!keyed_reload_preserves_topology(no_routes, slot, manifest, reason),
       "topology rejects missing logical routes");

    ModuleManifest missing;
    ck(!keyed_reload_preserves_topology(m->rec, slot, missing, reason),
       "topology rejects missing replacement callable");

    auto changed = manifest;
    changed.callables[0].calling_mode = CallingMode::KeyedR15;
    ck(!keyed_reload_preserves_topology(m->rec, slot, changed, reason),
       "topology rejects calling-mode change");
    changed = manifest;
    changed.callables[0].dispatch_domain = "different";
    ck(!keyed_reload_preserves_topology(m->rec, slot, changed, reason),
       "topology rejects dispatch-domain change");
}

static void single_reload_failure_matrix() {
    std::printf("-- coverage: single-function failure matrix --\n");
    expect_reload_failure("missing provider", [](KeyedModule&, KeyedReloadRequest& r) {
        r.build_provider.reset();
    });
    expect_reload_failure("identity record rejected", [](KeyedModule& m, KeyedReloadRequest&) {
        m.rec.mode = DispatchMode::Identity;
    });
    expect_reload_failure("storage count mismatch", [](KeyedModule& m, KeyedReloadRequest&) {
        m.st.storage = std::vector<std::atomic<void*>>(m.plan.physical_slot_count - 1);
    });
    expect_reload_failure("record logical count mismatch", [](KeyedModule& m, KeyedReloadRequest&) {
        ++m.rec.logical_slot_count;
    });
    expect_reload_failure("record physical count mismatch", [](KeyedModule& m, KeyedReloadRequest&) {
        ++m.rec.physical_slot_count;
    });
    expect_reload_failure("record domain count mismatch", [](KeyedModule& m, KeyedReloadRequest&) {
        ++m.rec.domain_count;
    });
    expect_reload_failure("logical slot out of range", [](KeyedModule& m, KeyedReloadRequest& r) {
        r.logical_slot = m.plan.logical_slot_count;
    });
    expect_reload_failure("logical routes too short", [](KeyedModule& m, KeyedReloadRequest&) {
        m.plan.logical_routes.clear();
    });
    expect_reload_failure("route domain out of range", [](KeyedModule& m, KeyedReloadRequest&) {
        m.plan.logical_routes[0].domain_index = uint32_t(m.plan.domains.size());
    });
    expect_reload_failure("record domains missing", [](KeyedModule& m, KeyedReloadRequest&) {
        m.rec.domains = nullptr;
    });
    expect_reload_failure("domain metadata mismatch", [](KeyedModule& m, KeyedReloadRequest&) {
        ++m.plan.domains[0].domain_salt;
    });
    expect_reload_failure("route ordinal outside logical count", [](KeyedModule& m, KeyedReloadRequest&) {
        m.plan.logical_routes[0].ordinal = m.plan.domains[0].logical_count;
    });
    expect_reload_failure("route logical-slot map mismatch", [](KeyedModule& m, KeyedReloadRequest&) {
        m.plan.domains[0].logical_slots[m.plan.logical_routes[0].ordinal] = 1;
    });
    expect_reload_failure("route ABI identity mismatch", [](KeyedModule& m, KeyedReloadRequest&) {
        ++m.plan.logical_routes[0].abi_fingerprint;
    });
    // The permutation deliberately treats strategy_version as mix input; it
    // does not reject unknown versions. A zero-sized domain does exercise the
    // helper's structured permutation-failure path after consistency checks.
    expect_reload_failure("invalid permutation domain size", [](KeyedModule& m, KeyedReloadRequest&) {
        m.plan.domains[0].physical_count = 1;
        const_cast<DispatchDomain*>(m.rec.domains)[0].physical_count = 1;
    });
    expect_reload_failure("selected descriptor is padding", [](KeyedModule& m, KeyedReloadRequest& r) {
        const uint32_t slot = m.plan.build_physical_placement[r.logical_slot];
        m.st.descriptors[slot].is_padding = true;
    });
    expect_reload_failure("selected descriptor ABI mismatch", [](KeyedModule& m, KeyedReloadRequest& r) {
        const uint32_t slot = m.plan.build_physical_placement[r.logical_slot];
        ++m.st.descriptors[slot].abi_fingerprint;
    });
    expect_reload_failure("lexer error", [](KeyedModule&, KeyedReloadRequest& r) {
        r.new_fn_source = "fn double(x: i64) -> i64 { return x; } \x01";
    });
    expect_reload_failure("zero replacement functions", [](KeyedModule&, KeyedReloadRequest& r) {
        r.new_fn_source = "struct S { x: i64; }\n";
    });
    expect_reload_failure("multiple replacement functions", [](KeyedModule&, KeyedReloadRequest& r) {
        r.new_fn_source =
            "fn double(x: i64) -> i64 { return x; }\n"
            "fn extra(x: i64) -> i64 { return x; }\n";
    });
    expect_reload_failure("no function at logical slot", [](KeyedModule& m, KeyedReloadRequest& r) {
        m.prog.funcs.clear();
        r.new_fn_source = "fn absent(x: i64) -> i64 { return x; }\n";
    });
    expect_reload_failure("replacement function name mismatch", [](KeyedModule&, KeyedReloadRequest& r) {
        r.new_fn_source = "fn renamed(x: i64) -> i64 { return x; }\n";
    });
    expect_reload_failure("parameter type changed", [](KeyedModule&, KeyedReloadRequest& r) {
        r.new_fn_source = "fn double(x: u64) -> i64 { return 1; }\n";
    });
    expect_reload_failure("sema failure restores AST", [](KeyedModule&, KeyedReloadRequest& r) {
        r.new_fn_source = "fn double(x: i64) -> i64 { return missing_name; }\n";
    });
}

static void same_content_reload_and_cleanup() {
    std::printf("-- coverage: same-content reload and teardown --\n");
    HotReloadDomain domain;
    auto m = build_keyed_module(
        "fn double(x: i64) -> i64 { return x * 2; }\n"
        "fn add1(x: i64) -> i64 { return x + 1; }\n",
        "coverage", DETERMINISTIC_SEED, &domain,
        reinterpret_cast<void*>(khr_trap));
    ck(m != nullptr, "same-content fixture");
    if (!m) return;
    const uint32_t logical = uint32_t(m->slots["double"]);
    const uint32_t physical = m->plan.build_physical_placement[logical];
    void* old_entry = m->st.storage[physical].load(std::memory_order_acquire);
    auto result = call_reload(*m, domain, good_request(
        *m, "fn double(x: i64) -> i64 { return x * 2; }\n"));
    ck(result.ok, "same-content reload succeeds as a fresh generation");
    if (!result.ok) return;
    void* new_entry = m->st.storage[physical].load(std::memory_order_acquire);
    ck(new_entry && new_entry != old_entry, "same content publishes a fresh page");
    ck(result.old_page_retired, "same content retires old generation page");
    for (auto& fn : m->fns) {
        if (fn.entry == old_entry) { fn.exec = nullptr; fn.entry = nullptr; }
    }
    m->fns.push_back(std::move(result.new_fn));
    ck(domain.quiesce() == 1, "teardown reclaims same-content old page");
    ck(domain.retired_page_count() == 0, "teardown leaves no retired pages");
}

struct GenerationFixture {
    std::unique_ptr<KeyedModule> module;
    ModuleRegistry registry{8};
    uint32_t id = UINT32_MAX;

    explicit GenerationFixture(const char* name = "generation") {
        HotReloadDomain unused;
        module = build_keyed_module(
            "fn double(x: i64) -> i64 { return x * 2; }\n"
            "fn add1(x: i64) -> i64 { return x + 1; }\n",
            name, DETERMINISTIC_SEED, &unused,
            reinterpret_cast<void*>(khr_trap));
        if (module) {
            std::string error;
            id = registry.register_module(name, module->table.base(), &error);
        }
    }

    KeyedGenerationReplacementRequest request(const char* name = "generation") {
        KeyedGenerationReplacementRequest req;
        req.stable_module_id = name;
        req.expected_module_id = id;
        req.new_plan = &module->plan;
        req.new_storage = &module->st;
        req.new_record = &module->rec;
        req.real_entry = [this](uint32_t physical) -> void* {
            const auto& pe = module->plan.physical_entries[physical];
            return pe.is_padding ? const_cast<void*>(ember_keyed_padding_trap_target())
                                 : module->table.get(pe.logical_slot);
        };
        req.registry = &registry;
        return req;
    }
};

static void generation_preflight_matrix() {
    std::printf("-- coverage: generation preflight and validation matrix --\n");
    auto run = [](const char* label,
                  const std::function<void(GenerationFixture&, KeyedGenerationReplacementRequest&)>& mutate) {
        GenerationFixture f;
        ck(f.module && f.id != UINT32_MAX, label);
        if (!f.module || f.id == UINT32_MAX) return;
        auto req = f.request();
        const auto* before = f.registry.dispatch_record(f.id);
        mutate(f, req);
        auto result = replace_keyed_generation(req);
        ck(!result.ok, label);
        ck(!result.error.empty(), "generation failure has diagnostic");
        ck(f.registry.dispatch_record(f.id) == before,
           "generation failure does not publish record");
    };

    run("empty stable module id", [](GenerationFixture&, auto& r) { r.stable_module_id.clear(); });
    run("null new plan", [](GenerationFixture&, auto& r) { r.new_plan = nullptr; });
    run("null new storage", [](GenerationFixture&, auto& r) { r.new_storage = nullptr; });
    run("null new record", [](GenerationFixture&, auto& r) { r.new_record = nullptr; });
    run("null real-entry callback", [](GenerationFixture&, auto& r) { r.real_entry = {}; });
    run("null registry", [](GenerationFixture&, auto& r) { r.registry = nullptr; });
    run("unregistered stable name", [](GenerationFixture&, auto& r) { r.stable_module_id = "absent"; });
    run("stable module id mismatch", [](GenerationFixture&, auto& r) { ++r.expected_module_id; });
    run("identity plan rejected", [](GenerationFixture& f, auto&) { f.module->plan.keyed = false; });
    run("zero logical count rejected", [](GenerationFixture& f, auto&) { f.module->plan.logical_slot_count = 0; });
    run("zero physical count rejected", [](GenerationFixture& f, auto&) { f.module->plan.physical_slot_count = 0; });
    run("logical route count mismatch", [](GenerationFixture& f, auto&) { f.module->plan.logical_routes.pop_back(); });
    run("physical entry count mismatch", [](GenerationFixture& f, auto&) { f.module->plan.physical_entries.pop_back(); });
    run("real entry callback error", [](GenerationFixture&, auto& r) {
        r.real_entry = [](uint32_t) -> void* { return nullptr; };
    });
    run("fresh record validation error", [](GenerationFixture& f, auto&) {
        for (auto& pe : f.module->plan.physical_entries) {
            if (!pe.is_padding) { ++pe.abi_fingerprint; break; }
        }
    });
}

static void generation_publication_paths() {
    std::printf("-- coverage: no-active, no-domain, and concurrent generations --\n");
    {
        GenerationFixture f;
        auto req = f.request();
        req.expected_module_id = UINT32_MAX; // optional-id path
        ck(f.registry.dispatch_record(f.id) == nullptr,
           "generation begins with no active dispatch record");
        auto result = replace_keyed_generation(req);
        ck(result.ok, "reload with no active module record publishes first generation");
        ck(result.publication_epoch == 0, "no-domain publication has no domain epoch");
        ck(f.registry.dispatch_record(f.id) == &f.module->rec,
           "no-domain path updates dispatch table record");
        f.registry.publish_dispatch_record(f.id, nullptr);
        ck(f.registry.dispatch_record(f.id) == nullptr, "explicit teardown unpublishes record");
    }

    // Two independently built same-name generations race through the registry's
    // release-store path.  They share no mutable plan/storage/record state.
    ModuleRegistry registry(8);
    HotReloadDomain unused1, unused2;
    auto a = build_keyed_module(
        "fn double(x: i64) -> i64 { return x * 2; }\nfn add1(x: i64) -> i64 { return x + 1; }\n",
        "race", DETERMINISTIC_SEED, &unused1, reinterpret_cast<void*>(khr_trap));
    auto b = build_keyed_module(
        "fn double(x: i64) -> i64 { return x * 9; }\nfn add1(x: i64) -> i64 { return x + 4; }\n",
        "race", DETERMINISTIC_SEED, &unused2, reinterpret_cast<void*>(khr_trap));
    ck(a && b, "concurrent generation fixtures");
    if (!a || !b) return;
    std::string error;
    const uint32_t id = registry.register_module("race", a->table.base(), &error);
    auto make_req = [&](KeyedModule& m) {
        KeyedGenerationReplacementRequest req;
        req.stable_module_id = "race";
        req.expected_module_id = id;
        req.new_plan = &m.plan;
        req.new_storage = &m.st;
        req.new_record = &m.rec;
        req.real_entry = [&m](uint32_t physical) -> void* {
            const auto& pe = m.plan.physical_entries[physical];
            return pe.is_padding ? const_cast<void*>(ember_keyed_padding_trap_target())
                                 : m.table.get(pe.logical_slot);
        };
        req.registry = &registry;
        return req;
    };
    auto ra = make_req(*a);
    auto rb = make_req(*b);
    KeyedGenerationReplacementResult ar, br;
    std::thread ta([&] { ar = replace_keyed_generation(ra); });
    std::thread tb([&] { br = replace_keyed_generation(rb); });
    ta.join();
    tb.join();
    ck(ar.ok && br.ok, "concurrent generation replacements both complete");
    const auto* final_record = registry.dispatch_record(id);
    ck(final_record == &a->rec || final_record == &b->rec,
       "concurrent readers observe one complete generation");
    ck(final_record && final_record->physical_slot_count == a->plan.physical_slot_count,
       "concurrent final generation has coherent dispatch metadata");
    registry.publish_dispatch_record(id, nullptr);
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int baseline = keyed_hot_reload_baseline_main();
    ck(baseline == 0, "baseline keyed hot-reload lifecycle remains green");
    topology_edge_matrix();
    single_reload_failure_matrix();
    same_content_reload_and_cleanup();
    generation_preflight_matrix();
    generation_publication_paths();
    std::printf("keyed_hot_reload_coverage_test: %d checks, %s\n",
                g_checks, g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
