// Focused coverage for the hand-built engine helpers and finalizer edges.
#include "engine.hpp"
#include "jit_memory.hpp"
#include "x64_emitter.hpp"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

extern "C" int64_t native_mix(int64_t a, int64_t b) {
    return a * 10 + b;
}

void release(ember::CompiledFn& fn) {
    if (fn.exec) ember::free_executable(fn.exec);
    fn.exec = fn.entry = nullptr;
}
} // namespace

int main() {
    using namespace ember;

    CompiledFn add = compile_add_i64();
    CompiledFn sub = compile_sub_i64();
    CompiledFn mul = compile_mul_i64();
    CompiledFn constant = compile_ret_const(-1234567890123LL);
    CompiledFn maximum = compile_max_i64();
    check(finalize(add) && finalize(sub) && finalize(mul) &&
          finalize(constant) && finalize(maximum),
          "all hand-built arithmetic functions finalize");
    if (!failures) {
        check(call_i64_i64_i64(add.entry, 20, 22) == 42, "add helper executes");
        check(call_i64_i64_i64(sub.entry, 20, 22) == -2, "sub helper executes");
        check(call_i64_i64_i64(mul.entry, -6, 7) == -42, "mul helper executes");
        check(call_i64_i64(constant.entry) == -1234567890123LL, "constant helper preserves imm64");
        check(call_i64_i64_i64(maximum.entry, 9, 4) == 9 &&
              call_i64_i64_i64(maximum.entry, 4, 9) == 9 &&
              call_i64_i64_i64(maximum.entry, 9, 9) == 9,
              "max helper covers both branch outcomes and equality");
    }

    CompiledFn native = compile_native_passthrough_2arg(reinterpret_cast<void*>(&native_mix));
    check(finalize(native), "native passthrough finalizes");
    if (native.entry)
        check(call_i64_i64_i64(native.entry, 4, 2) == 42, "native passthrough obeys the Win64 ABI");

    // Exercise finalize's code+rodata path and FunctionRodataBase patching.
    X64Emitter emitter;
    emitter.mov_reg_imm64(Reg::rax, 0);
    emitter.ret();
    CompiledFn rodata{"rodata", emitter.code, nullptr, nullptr};
    rodata.rodata = {1, 2, 3, 4};
    rodata.abs_fixups.push_back(AbsFixup{2, AbsFixup::FunctionRodataBase, 1});
    check(finalize(rodata), "function with rodata finalizes through RW then RX");
    if (rodata.entry) {
        auto returned = static_cast<uintptr_t>(call_i64_i64(rodata.entry));
        auto expected = reinterpret_cast<uintptr_t>(rodata.exec) + rodata.bytes.size() + 1;
        check(returned == expected, "rodata base fixup points at code end plus addend");
    }

    // A malformed out-of-range fixup must be ignored rather than writing past
    // the staged image.
    CompiledFn malformed{"malformed", compile_ret_const(7).bytes, nullptr, nullptr};
    malformed.rodata = {9};
    malformed.abs_fixups.push_back(AbsFixup{0xffffffffu, AbsFixup::FunctionRodataBase, 0});
    check(finalize(malformed), "out-of-range rodata fixup is safely ignored");
    if (malformed.entry) check(call_i64_i64(malformed.entry) == 7, "ignored malformed fixup leaves code executable");

    X64Emitter unresolved;
    Label missing = unresolved.alloc_label();
    unresolved.jmp(missing);
    bool unbound_threw = false;
    try {
        unresolved.resolve_fixups();
    } catch (const std::runtime_error& e) {
        unbound_threw = std::string(e.what()).find("unbound label") != std::string::npos;
    }
    check(unbound_threw, "unbound label reports a runtime error");

    release(add); release(sub); release(mul); release(constant); release(maximum);
    release(native); release(rodata); release(malformed);

    std::printf("engine edge test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
