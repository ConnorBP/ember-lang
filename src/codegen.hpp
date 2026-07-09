// ember codegen - tree-walking AST -> x86-64 (CODEGEN_SPEC.md).
// v1-of-frontend: a simple stack-spilling tree-walker (correctness first;
// the formal SSA-lite IR + linear-scan regalloc is a later refactor once
// v0.5 benchmarks say it matters - YAGNI). Handles bomb_timer's subset.
#pragma once
#include "ast.hpp"
#include "x64_emitter.hpp"
#include "jit_memory.hpp"
#include "sema.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace ember {

struct CompiledFn;

// Globals block: a flat 8-byte-per-global region the host reads/writes.
// Codegen bakes `base` as an absolute imm64; global i is at [base + i*8].
struct GlobalsBlock {
    int64_t base = 0;                       // set before compiling/calling
    std::unordered_map<std::string, uint32_t> index;  // name -> slot index
    std::unordered_map<std::string, const Type*> types;
};

// Obfuscation options (CODEGEN_SPEC.md Section obf).
// Applied at emission time (correct-by-construction, no post-JIT disassembly).
// These emitter-level transforms (MBA identities, opaque-predicate pattern,
// CPUID-keyed gate) operate directly on the X64Emitter. The full PE-pass
// integration is a build-time host-DLL protection phase.
struct ObfOptions {
    bool mba = false;          // MBA arithmetic substitution on add/sub/xor/and/or
    bool opaque = false;       // opaque-predicate junk insertion (4 variants)
    bool keyed = false;       // CPUID-keyed entry gate (system-keyed assembly)
    int64_t cpuid_key = 0;    // expected CPUID.1:EAX (CPU signature) for the gate
};

// Read the running machine's CPUID.1:EAX (CPU signature) - host helper
// for baking the key into @obf_keyed functions.
int64_t current_cpuid_signature();

// Dispatch table base (set before compiling functions that call scripts).
struct CodeGenCtx {
    int64_t globals_base = 0;
    int64_t dispatch_base = 0;
    const std::unordered_map<std::string, NativeSig>* natives = nullptr;
    const std::unordered_map<std::string, int>* script_slots = nullptr;
    ObfOptions obf;   // host-set defaults; @obf annotations layer on top
    const StructLayoutTable* structs = nullptr;  // struct value types (task 1.6)
    void* str_decrypt_fn = nullptr;  // __str_decrypt native (string encryption)
};

// Compile one function. Returns the JIT'd bytes + (after finalize) entry.
CompiledFn compile_func(const FuncDecl& f, const CodeGenCtx& ctx);

// Globals block used by codegen (set by the host before compiling/calling).
// A single process-wide pointer (v1 frontend; the host wires one block per
// engine). Defined in codegen.cpp.
extern GlobalsBlock* g_globals_for_codegen;

} // namespace ember
