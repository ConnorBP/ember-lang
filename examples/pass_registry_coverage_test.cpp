// Exhaustive API and concrete-factory coverage for ember_pass_registry.hpp.
#include "ember_pass_registry.hpp"
#include "ember_pass.hpp"
#include "thin_ir.hpp"
#include "ext_opt.hpp"
#include "ext_obf.hpp"
#include "custom_passes.hpp"
// Documentation example compiled into this existing CMake test target.
#include "custom_pass/mov_substitute_pass.cpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace ember;

namespace {
int failures = 0;
int cases = 0;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); ++failures; } } while (0)
#define CASE(name) do { ++cases; std::printf("[CASE] %s\n", name); } while (0)

struct ConfiguredPass : EmberPassInfoMixin<ConfiguredPass> {
    static constexpr const char* pass_name = "configured";
    int value = 0;
    explicit ConfiguredPass(int v = 0) : value(v) {}
    EmberPreserved run(ThinFunction& f, EmberAnalysisManager&) {
        f.frame.frame_size += value;
        return value ? EmberPreserved::none() : EmberPreserved::all();
    }
};

static ThinInstr constant(VReg dst, int64_t value) {
    ThinInstr i;
    i.op = ThinOp::ConstInt;
    i.dst = dst;
    i.imm.i = value;
    i.meta.type = &type_i64();
    i.meta.width = 8;
    return i;
}

static ThinFunction workload() {
    ThinFunction f;
    f.name = "registry-coverage";
    f.ret_type = &type_i64();
    ThinBlock b;
    b.id = 0;
    b.instrs.push_back(constant(1, 7));
    b.instrs.push_back(constant(2, 3));
    ThinInstr add;
    add.op = ThinOp::Add;
    add.dst = 3;
    add.src1 = 1;
    add.src2 = 2;
    add.meta.type = &type_i64();
    add.meta.width = 8;
    b.instrs.push_back(add);
    b.term.kind = TermKind::Return;
    b.term.ret = 3;
    f.blocks.push_back(std::move(b));
    return f;
}

} // namespace

int main() {
    CASE("empty registry and unknown lookup");
    {
        EmberPassRegistry reg;
        CHECK(reg.names().empty());
        CHECK(!reg.has("missing"));
        CHECK(reg.create("missing") == nullptr);
        EmberPassManager pm;
        EmberAnalysisManager am;
        ThinFunction f;
        CHECK(pm.empty());
        CHECK(pm.run(f, am).all_preserved());
    }

    CASE("add template validation and duplicate retention");
    {
        EmberPassRegistry reg;
        auto empty = reg.add<ConfiguredPass>("");
        CHECK(!empty && empty.error && empty.error->registry == EmberPassRegistry::registry_id);
        CHECK(bool(reg.add<ConfiguredPass>("first")));
        auto duplicate = reg.add<ConfiguredPass>("first");
        CHECK(!duplicate && duplicate.error && duplicate.error->name == "first");
        auto p = reg.create("first");
        CHECK(p && std::string(p->name()) == ConfiguredPass::pass_name);
    }

    CASE("configured factory validation, ownership, and fresh creation");
    {
        EmberPassRegistry reg;
        auto empty = reg.add_factory("", [] { return make_pass_concept(ConfiguredPass{1}); });
        CHECK(!empty && empty.error);
        auto null_factory = reg.add_factory("null", PassFactory{});
        CHECK(!null_factory && null_factory.error);
        CHECK(bool(reg.add_factory("configured", [] { return make_pass_concept(ConfiguredPass{9}); })));
        auto duplicate = reg.add_factory("configured", [] { return make_pass_concept(ConfiguredPass{99}); });
        CHECK(!duplicate && duplicate.error);
        auto a = reg.create("configured");
        auto b = reg.create("configured");
        CHECK(a && b && a.get() != b.get());
        CHECK(std::string(a->name()) == "configured");
        ThinFunction fa, fb;
        EmberAnalysisManager am;
        CHECK(!a->run(fa, am).all_preserved() && fa.frame.frame_size == 9);
        CHECK(!b->run(fb, am).all_preserved() && fb.frame.frame_size == 9);
    }

    CASE("deterministic mixed-registration names");
    {
        EmberPassRegistry reg;
        CHECK(bool(reg.add<ConfiguredPass>("zeta")));
        CHECK(bool(reg.add_factory("alpha", [] { return make_pass_concept(ConfiguredPass{1}); })));
        CHECK(bool(reg.add<ConfiguredPass>("middle")));
        CHECK((reg.names() == std::vector<std::string>{"alpha", "middle", "zeta"}));
    }

    static const char* opt_names[] = {
        "constprop", "dce", "simplifycfg", "cse", "gvn", "licm", "lsr",
        "forward", "copyprop", "instcombine", "dse", "bounds-elim", "sccp",
        "unroll", "spill_elim", "peephole", "branch_folding", "tailcall"
    };
    static const char* obf_names[] = {
        "subst", "mba_expand", "const_encode", "opaque_pred", "deadcode",
        "str_encrypt", "block_split"
    };

    CASE("all optimization and legacy obfuscation register functions");
    {
        EmberPassRegistry reg;
        ext_opt::register_passes(reg);
        ext_obf::register_passes(reg);
        CHECK(reg.names().size() == 25);
        for (const char* name : opt_names) CHECK(reg.has(name));
        for (const char* name : obf_names) CHECK(reg.has(name));

        // Every concrete default-constructor factory creates two independent
        // models and every pass executes on both empty and non-empty IR.
        EmberAnalysisManager am;
        for (const auto& name : reg.names()) {
            auto a = reg.create(name.c_str());
            auto b = reg.create(name.c_str());
            CHECK(a && b && a.get() != b.get());
            CHECK(std::string(a->name()) == name);
            ThinFunction empty;
            a->run(empty, am);
            ThinFunction f = workload();
            b->run(f, am);
        }

        // A second package registration exercises duplicate rejection while
        // retaining every original factory.
        ext_opt::register_passes(reg);
        ext_obf::register_passes(reg);
        CHECK(reg.names().size() == 25);
        for (const char* name : opt_names) CHECK(reg.create(name) != nullptr);
        for (const char* name : obf_names) CHECK(reg.create(name) != nullptr);
    }

    CASE("configured obfuscation register function and factories");
    {
        EmberPassRegistry reg;
        PolymorphicPassOptions options = legacy_defaults("subst");
        auto status = ext_obf::register_passes(reg, options);
        CHECK(bool(status));
        CHECK(reg.names().size() == 7);
        EmberAnalysisManager am;
        for (const char* name : obf_names) {
            auto a = reg.create(name);
            auto b = reg.create(name);
            CHECK(a && b && a.get() != b.get());
            CHECK(a->is_required());
            ThinFunction f = workload();
            a->run(f, am);
        }
        // The package wrapper is compatibility-oriented and currently reports
        // option-validation status; the registry itself rejects each duplicate.
        // Verify that calling it again cannot replace or grow the package.
        auto duplicate = ext_obf::register_passes(reg, options);
        CHECK(bool(duplicate));
        CHECK(reg.names().size() == 7);
    }

    CASE("custom example register function");
    {
        EmberPassRegistry reg;
        ember::examples::custom_pass::register_passes(reg);
        ember::examples::custom_pass::register_mov_substitute_pass(reg, 0x1234);
        const char* names[] = {"example-minimal", "example-nop-injection", "example-block-merge",
                               "example-mov-substitute"};
        EmberAnalysisManager am;
        for (const char* name : names) {
            CHECK(reg.has(name));
            auto p = reg.create(name);
            CHECK(p != nullptr);
            ThinFunction f = workload();
            p->run(f, am);
        }
    }

    CASE("full pipeline with every production pass enabled");
    {
        EmberPassRegistry reg;
        ext_opt::register_passes(reg);
        CHECK(bool(ext_obf::register_passes(reg, legacy_defaults("subst"))));
        EmberPassManager pm;
        for (const auto& name : reg.names()) pm.add_pass_concept(reg.create(name.c_str()));
        CHECK(pm.size() == 25);
        int before = 0;
        int after = 0;
        PassInstrumentationCallbacks callbacks;
        callbacks.before_pass = [&](const char*, const ThinFunction&) { ++before; return true; };
        callbacks.after_pass = [&](const char*, const ThinFunction&, EmberPreserved) { ++after; };
        pm.instrumentation.callbacks = &callbacks;
        EmberAnalysisManager am;
        ThinFunction f = workload();
        pm.run(f, am);
        // Required obfuscation passes bypass the before-pass skip gate by
        // design, but every pass invokes the after callback.
        CHECK(before == 18 && after == 25);
        CHECK(!f.blocks.empty());
    }

    std::printf("pass_registry_coverage_test: %d cases, %s\n", cases,
                failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
