// Differential execution test for MovSubstitutePass. Expected value: 177.
#include "ember_pass.hpp"
#include "ember_pass_registry.hpp"
#include "engine.hpp"
#include "thin_emit.hpp"
#include "thin_ir.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

namespace ember::examples::custom_pass {
void register_mov_substitute_pass(EmberPassRegistry&, uint64_t);
}

using namespace ember;

namespace {
ThinInstr constant(VReg dst, int64_t value, int32_t off) {
    ThinInstr in;
    in.op = ThinOp::ConstInt;
    in.dst = dst;
    in.imm.i = value;
    in.meta.type = &type_i64();
    in.meta.width = 8;
    in.meta.frame_off = off;
    return in;
}

ThinFunction make_workload() {
    ThinFunction f;
    f.name = "mov-substitute-validation";
    f.ret_type = &type_i64();
    f.frame.next_local_off = 24;
    f.frame.frame_size = 48;
    ThinBlock b;
    b.id = 0;
    b.instrs.push_back(constant(1, 100, -8));
    b.instrs.push_back(constant(2, 77, -16));
    ThinInstr add;
    add.op = ThinOp::Add;
    add.dst = 3;
    add.src1 = 1;
    add.src2 = 2;
    add.meta.type = &type_i64();
    add.meta.width = 8;
    add.meta.frame_off = -24;
    b.instrs.push_back(add);
    b.term.kind = TermKind::Return;
    b.term.ret = 3;
    f.blocks.push_back(std::move(b));
    f.declared_max_vreg = 4;
    return f;
}

int64_t emit_and_run(ThinFunction f) {
    CodeGenCtx ctx;
    CompiledFn compiled = emit_x64(f, ctx);
    if (compiled.bytes.empty() || !finalize(compiled)) return INT64_MIN;
    const int64_t value = call_i64_i64(compiled.entry);
    free_executable(compiled.exec);
    compiled.exec = nullptr;
    compiled.entry = nullptr;
    return value;
}
} // namespace

int main() {
    ThinFunction original = make_workload();
    ThinFunction transformed = original;

    EmberPassRegistry registry;
    ember::examples::custom_pass::register_mov_substitute_pass(
        registry, 0x123456789abcdef0ULL);
    EmberPassManager manager;
    manager.add_pass_concept(registry.create("example-mov-substitute"));
    EmberAnalysisManager analyses;
    const PassRunReport report = manager.run_checked(transformed, analyses);
    if (report.stop_reason != PassStopReason::Completed) {
        std::fprintf(stderr, "pass failed: %s\n", report.error.c_str());
        return 1;
    }

    const int64_t before = emit_and_run(std::move(original));
    const int64_t after = emit_and_run(std::move(transformed));
    if (before != 177 || after != before) {
        std::fprintf(stderr, "expected 177, before=%lld after=%lld\n",
                     static_cast<long long>(before),
                     static_cast<long long>(after));
        return 1;
    }
    std::printf("mov_substitute_pass: %lld before, %lld after (PASS)\n",
                static_cast<long long>(before),
                static_cast<long long>(after));
    return 0;
}
