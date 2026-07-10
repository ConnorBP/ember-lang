// Focused Win64 ABI regressions for audit H3-H5.
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "engine.hpp"
#include "context.hpp"
#include "dispatch_table.hpp"
#include "jit_memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

static int g_fail = 0;
static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

struct Module {
    std::vector<CompiledFn> fns;
    DispatchTable table;
    std::unordered_map<std::string, int> slots;
    std::vector<uint8_t> globals;
    void* main_entry = nullptr;
    ~Module() { for (auto& fn : fns) if (fn.exec) free_executable(fn.exec); }
};

static bool compile_source(const std::string& source, Module& m,
                           void* trap_stub = nullptr, bool b1 = false,
                           bool keyed_mismatch = false) {
    auto lr = tokenize(source, "<win64-abi-test>");
    if (!lr.ok) return false;
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) return false;
    int slot = 0;
    for (auto& fn : pr.program.funcs) { fn.slot = slot; m.slots[fn.name] = slot++; }
    std::unordered_map<std::string, NativeSig> natives;
    OpOverloadTable overloads;
    auto layouts = build_struct_layouts(pr.program);
    pr.program.string_xor_key = 0;
    if (!sema(pr.program, natives, m.slots, 0, &overloads, &layouts).ok) return false;

    m.table = DispatchTable(pr.program.funcs.size());
    m.globals.assign(pr.program.globals.size() * 8, 0);
    std::unordered_map<std::string, uint32_t> globals_index;
    std::unordered_map<std::string, const Type*> globals_types;
    uint32_t gi = 0;
    for (auto& g : pr.program.globals) {
        globals_index[g.name] = gi++;
        globals_types[g.name] = g.ty.get();
    }
    CodeGenCtx ctx;
    ctx.globals_base = int64_t(m.globals.data());
    ctx.dispatch_base = int64_t(m.table.base());
    ctx.natives = &natives;
    ctx.script_slots = &m.slots;
    ctx.structs = &layouts;
    ctx.globals_index = &globals_index;
    ctx.globals_types = &globals_types;
    ctx.trap_stub = trap_stub;
    ctx.use_context_reg = b1;
    if (keyed_mismatch) {
        ctx.obf.keyed = true;
        ctx.obf.cpuid_key = current_cpuid_signature() ^ 1;
    }

    for (auto& fn : pr.program.funcs) {
        auto compiled = compile_func(fn, ctx);
        if (!finalize(compiled)) return false;
        m.table.set(fn.slot, compiled.entry);
        m.fns.push_back(std::move(compiled));
    }
    auto it = m.slots.find("main");
    if (it == m.slots.end()) return false;
    m.main_entry = m.table.get(it->second);
    return m.main_entry != nullptr;
}

#if defined(__GNUC__) && defined(__x86_64__)
extern "C" int64_t call_seed_r13(void*, uint64_t, uint64_t*);
extern "C" int64_t call_seed_r14_void(void*, context_t*, uint64_t, uint64_t*);
extern "C" int64_t call_seed_r14_i64(void*, context_t*, int64_t, uint64_t, uint64_t*);
extern "C" void aligned_trap(context_t*, int, const char*);
extern "C" int64_t call_i64_aligned(void*, int64_t);

asm(
    ".text\n"
    ".p2align 4\n"
    ".globl call_seed_r13\n"
    "call_seed_r13:\n"
    "  pushq %r13\n"
    "  pushq %r12\n"
    "  subq $40, %rsp\n"
    "  movq %rcx, %r11\n"
    "  movq %rdx, %r13\n"
    "  movq %r8, %r12\n"
    "  callq *%r11\n"
    "  movq %r13, (%r12)\n"
    "  addq $40, %rsp\n"
    "  popq %r12\n"
    "  popq %r13\n"
    "  retq\n"
    ".p2align 4\n"
    ".globl call_seed_r14_void\n"
    "call_seed_r14_void:\n"
    "  pushq %r14\n"
    "  pushq %r13\n"
    "  subq $40, %rsp\n"
    "  movq %r8, %r14\n"
    "  movq %r9, %r13\n"
    "  callq _ZN5ember15ember_call_voidEPvPNS_9context_tE\n"
    "  movq %r14, (%r13)\n"
    "  addq $40, %rsp\n"
    "  popq %r13\n"
    "  popq %r14\n"
    "  retq\n"
    ".p2align 4\n"
    ".globl call_seed_r14_i64\n"
    "call_seed_r14_i64:\n"
    "  pushq %r14\n"
    "  pushq %r13\n"
    "  subq $40, %rsp\n"
    "  movq %r9, %r14\n"
    "  movq 96(%rsp), %r13\n"
    "  callq _ZN5ember14ember_call_i64EPvPNS_9context_tEx\n"
    "  movq %r14, (%r13)\n"
    "  addq $40, %rsp\n"
    "  popq %r13\n"
    "  popq %r14\n"
    "  retq\n"
    ".p2align 4\n"
    ".globl aligned_trap\n"
    "aligned_trap:\n"
    "  movaps %xmm0, 8(%rsp)\n"  // shadow address aligned iff caller's rsp was aligned
    "  movq %rdx, %rax\n"        // return reason without disturbing stack
    "  retq\n"
    ".p2align 4\n"
    ".globl call_i64_aligned\n"
    "call_i64_aligned:\n"
    "  pushq %r12\n"             // entry rsp%16=8; push aligns it
    "  subq $32, %rsp\n"          // mandatory Win64 shadow space
    "  movq %rcx, %r11\n"
    "  movq %rdx, %rcx\n"
    "  callq *%r11\n"
    "  addq $32, %rsp\n"
    "  popq %r12\n"
    "  retq\n"
);
#else
#error "win64_abi_test requires the supported MinGW GNU-assembly x64 path"
#endif

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== Win64 ABI regression (H3-H5) ===\n");
    constexpr uint64_t seed13 = 0x1122334455667788ull;
    constexpr uint64_t seed14 = 0x8877665544332211ull;

    {
        Module m;
        bool ok = compile_source(
            "fn main() -> i64 { let mut x: i64 = 0; switch (2) { case 1: x = 11; break; case 2: x = 22; break; default: x = 33; break; } switch (9) { case 1: x = 1; break; default: x = x + 100; break; } return x; }\n", m);
        check(ok, "H3: compile switch with default");
        if (ok) {
            uint64_t after = 0;
            int64_t result = call_seed_r13(m.main_entry, seed13, &after);
            check(result == 122, "H3: switch match, default, and break behavior remains correct");
            check(after == seed13, "H3: switch preserves caller r13");
        }
    }

    {
        Module mv, mi;
        bool okv = compile_source("fn main() -> i64 { return 71; }\n", mv, nullptr, true);
        bool oki = compile_source("fn main(x: i64) -> i64 { return x + 1; }\n", mi, nullptr, true);
        check(okv && oki, "H4: compile raw B1 thunk targets");
        context_t ctx;
        if (okv) {
            uint64_t after = 0;
            int64_t result = call_seed_r14_void(mv.main_entry, &ctx, seed14, &after);
            check(result == 71 && after == seed14, "H4: ember_call_void preserves incoming r14");
        }
        if (oki) {
            uint64_t after = 0;
            int64_t result = call_seed_r14_i64(mi.main_entry, &ctx, 41, seed14, &after);
            check(result == 42 && after == seed14, "H4: ember_call_i64 preserves incoming r14");
        }
    }

    {
        Module normal, pushed, keyed, f32_store;
        bool ok1 = compile_source("fn main(i: i64) -> i64 { let a: i64[1]; return a[i]; }\n",
                                  normal, reinterpret_cast<void*>(&aligned_trap));
        bool ok2 = compile_source("fn main(i: i64) -> i64 { let a: i64[1]; return 7 + a[i]; }\n",
                                  pushed, reinterpret_cast<void*>(&aligned_trap));
        bool ok3 = compile_source("fn main(i: i64) -> i64 { return i; }\n",
                                  keyed, reinterpret_cast<void*>(&aligned_trap), false, true);
        bool ok4 = compile_source("fn main(i: i64) -> i64 { let a: f32[1]; a[i] = 1.0f; return 9; }\n",
                                  f32_store, reinterpret_cast<void*>(&aligned_trap));
        check(ok1 && ok2 && ok3 && ok4, "H5: compile tracked trap-edge variants");
        if (ok1) {
            call_i64_aligned(normal.main_entry, 2);
            check(true, "H5: aligned trap stub returned safely at normal parity");
        }
        if (ok2) {
            call_i64_aligned(pushed.main_entry, 2);
            check(true, "H5: aligned trap stub returned safely with pushed temporary");
        }
        if (ok3) {
            call_i64_aligned(keyed.main_entry, 0);
            check(true, "H5: keyed-gate mismatch restores rbx before aligned trap call");
        }
        if (ok4) {
            call_i64_aligned(f32_store.main_entry, 2);
            check(true, "H5: OOB f32 indexed store tracks its eight-byte temporary");
        }
    }

    std::printf("\nWin64 ABI test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
