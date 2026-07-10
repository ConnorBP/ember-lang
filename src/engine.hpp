// ember engine (v0.1 minimal).
// CODEGEN_SPEC.md Section 6/Section 8: Win64 prologue/epilogue, dispatch table.
// v0.1: hand-built IR for a single function (no lexer/parser yet);
// the full engine API (DESIGN.md Section 8) grows from here.
#pragma once
#include "x64_emitter.hpp"
#include "jit_memory.hpp"
#include "context.hpp"   // context_t (v1.0 ember_call ctx-reg indirection)
#include <cstdint>
#include <vector>
#include <string>

namespace ember {

// A compiled function: its JIT'd bytes, the exec-memory pointer, and
// the entry address. Lives as long as the owning module/engine.
// `abs_fixups` are the absolute-imm64 relocations recorded by the emitter's
// mov_reg_imm64_external (BUNDLING_AND_EM_MODULES.md Section 2.4) - the dispatch-
// table base and globals base loads that a `.em` serializer must repoint at
// load time. Populated by compile_func; empty for the hand-built engine.cpp
// proofs (which bake real addresses via raw mov_reg_imm64).
struct CompiledFn {
    std::string name;
    std::vector<uint8_t> bytes;   // emitted bytes (kept for inspection/debug)
    void* exec = nullptr;         // alloc_executable result
    void* entry = nullptr;        // == exec (alias for clarity at call sites)
    std::vector<AbsFixup> abs_fixups; // relocatable imm64 slots (for .em serialization)
};

// v0.1: build the compiled form of `fn add(a: i64, b: i64) -> i64 { return a + b; }`
// directly via the emitter. No parser yet - this proves the codegen +
// jit_memory + call round-trip (CODEGEN_SPEC.md Section 12 criterion 1).
//
// Win64: a=rcx, b=rdx, return=rax.
//   push rbp
//   mov  rbp, rsp
//   mov  rax, rcx        ; a
//   add  rax, rdx        ; + b
//   mov  rsp, rbp
//   pop  rbp
//   ret
CompiledFn compile_add_i64();

// Build `fn sub(a: i64, b: i64) -> i64 { return a - b; }` (criterion: sub path)
CompiledFn compile_sub_i64();

// Build `fn mul(a: i64, b: i64) -> i64 { return a * b; }` (criterion: imul path)
CompiledFn compile_mul_i64();

// Build a leaf `fn ret_const() -> i64 { return <imm>; }` (criterion: imm64 path)
CompiledFn compile_ret_const(int64_t imm);

// v0.2: `fn max(a: i64, b: i64) -> i64 { if (a > b) return a; return b; }`
// Tests the forward-label fixup system (CODEGEN_SPEC.md Section 4/Section 12 criterion 3).
// Win64: a=rcx, b=rdx, return=rax.
CompiledFn compile_max_i64();

// v0.2: `fn fib(n: i64) -> i64 { if (n <= 1) return n; return fib(n-1)+fib(n-2); }`
// Recursive call through the dispatch table (CODEGEN_SPEC.md Section 7/Section 12 criterion 5).
// n=rcx (arg1). Uses callee-saved rbx (n) and r12 (fib(n-1)).
// table_base baked as absolute imm64; must be set on the table before finalize.
CompiledFn compile_fib_i64(int64_t table_base, uint32_t slot);

// v0.3: `fn(p: i64, a: i64) -> i64 { return native(p, a); }`
// Script->native call path (CODEGEN_SPEC.md Section 8). Forwards (proc, addr)
// to a host C++ function whose Win64 signature is i64(i64,i64), returning
// its result. Proves the proc_api binding pipeline end-to-end.
CompiledFn compile_native_passthrough_2arg(void* native_fn);

// Allocate exec memory for a CompiledFn's bytes and set its entry pointer.
// Returns false on alloc failure.
bool finalize(CompiledFn& fn);

// Call a finalized i64(i64,i64) function pointer.
int64_t call_i64_i64_i64(void* entry, int64_t a, int64_t b);

// Call a finalized i64() function pointer.
int64_t call_i64_i64(void* entry);

// v1.0 thread-safety (Option B1, plan_CONTEXT_THREADSAFETY.md): call a JIT'd
// entry with a context_t* passed in r14 (the per-call context register). Use
// when the module was compiled with CodeGenCtx::use_context_reg = true (the
// budget/depth/trap reads go through r14). r14 = ctx at entry, callee-saved so
// preserved across script-to-script calls (the callee inherits the same ctx).
int64_t ember_call_void(void* entry, context_t* ctx);
int64_t ember_call_i64(void* entry, context_t* ctx, int64_t a);

} // namespace ember
