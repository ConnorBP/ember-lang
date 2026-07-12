// ext_call_raw.cpp - ember extension: raw x64 execution natives.
//
// Implements three PERM_FFI-gated natives that together bridge the
// self-hosted ember codegen's output to actual execution:
//
//   make_executable(bytes: i64) -> i64
//       Takes an array<u8> handle (the codegen's x64 byte buffer), copies the
//       bytes to a W^X executable page (alloc_executable_rw -> seal_executable),
//       and returns the entry pointer as an i64 (0 on failure). The returned
//       pointer is RX (not writable) and lives until free_executable_ptr is
//       called on it. The entry is the FIRST byte of the buffer — i.e. the
//       first function's prologue (the self-hosted codegen lays functions out
//       in declaration order, so a single-fn program's entry is at offset 0).
//
//   call_raw(fn_ptr: i64, arg: i64) -> i64
//       Casts fn_ptr to int64_t(*)(int64_t) and calls it with arg. Returns the
//       callee's i64 return value. A null or garbage fn_ptr crashes the process
//       (the same posture as a C function-pointer dereference — see the
//       header's SECURITY POSTURE: raw capability, not policy).
//
//   free_executable_ptr(ptr: i64) -> void
//       Frees a page returned by make_executable (via free_executable). A
//       script that mints executable pages should free them when done; a
//       long-lived host may instead let them live until unload (the host's
//       reset path does not track them — they are raw VirtualAlloc pages, not
//       extension-slot objects).
//
// These three are the natives the self-hosting milestone
// (docs/planning/plan_SELF_HOSTING.md Stage 4) identified as the gap: they let
// the ember-written codegen's output be EXECUTED — codegen produces x64 bytes
// in array<u8> -> make_executable copies them to an RX page -> call_raw invokes
// the entry point -> free_executable_ptr releases the page. See
// ext_call_raw.hpp for the full framing.
//
// Stateless (no host slot vector); reset() is a no-op (the executable pages
// are owned by the caller via free_executable_ptr, not by this extension).
#include "ext_call_raw.hpp"
#include "ast.hpp"              // type_i64, type_void
#include "binding_builder.hpp"  // BindingBuilder, PERM_FFI
#include "ext_array.hpp"        // ext_array::get_bytes
#include "jit_memory.hpp"       // alloc_executable, free_executable

#include <cstdint>
#include <cstring>
#include <vector>

using namespace ember;  // BindingBuilder, type_* singletons

namespace ember::ext_call_raw {

// call_raw(fn_ptr: i64, arg: i64) -> i64
// Cast fn_ptr to int64_t(*)(int64_t) and call it with arg. Returns the
// callee's i64 return value. A null or garbage fn_ptr crashes the process
// (the same posture as a C function-pointer dereference — see the header's
// SECURITY POSTURE: raw capability, not policy).
//
// The cast + call is a single C-level reinterpret_cast + indirect call. The
// Win64 / SysV calling convention for an int64_t(*)(int64_t) matches the
// ember i64->i64 call shape exactly (first int arg in rcx/rdi, return in
// rax), so no trampoline is needed — the native IS the call.
static int64_t n_call_raw(int64_t fn_ptr, int64_t arg) {
    using Fn = int64_t(*)(int64_t);
    // Security guard: reject null/garbage fn_ptr instead of crashing the process
    // (audit MEDIUM finding — the original design deliberately crashed, but a
    // recoverable error is safer for a scripting language). Returns INT64_MIN
    // as a sentinel error value (same convention as thread_join trap signal).
    if (fn_ptr == 0) {
        return INT64_MIN;  // null function pointer — recoverable error
    }
    Fn f = reinterpret_cast<Fn>(fn_ptr);
    return f(arg);
}

// make_executable(bytes: i64) -> i64
// Takes an array<u8> handle (the codegen's byte buffer), copies the bytes to a
// W^X executable page, and returns the entry pointer as an i64 (0 on failure).
// The returned page is RX (sealed, not writable) and owned by the caller until
// free_executable_ptr is called on it.
//
// The entry is the first byte of the buffer — i.e. the first function's
// prologue. The self-hosted codegen lays functions out in declaration order,
// so for a single-fn program the entry is at offset 0. (Multi-fn programs with
// calls use a placeholder fn-address that the host would patch at JIT time;
// make_executable does NOT patch placeholders, so a program that calls other
// fns would branch to address 0. The execution demo uses a single self-
// contained fn — pure arithmetic, no calls — whose bytes are directly
// executable.)
static int64_t n_make_executable(int64_t bytes_handle) {
    uint8_t* data = nullptr;
    int64_t  len  = 0;
    if (!ext_array::get_bytes(bytes_handle, &data, &len) || len <= 0) {
        return 0;  // invalid handle or empty buffer -> no page
    }
    std::vector<uint8_t> code(data, data + size_t(len));
    void* page = alloc_executable(code);  // RW -> memcpy -> seal RX (one-shot)
    return int64_t(page);                 // 0 (nullptr) on alloc/seal failure
}

// free_executable_ptr(ptr: i64) -> void
// Frees a page returned by make_executable. Safe to call with 0 (free_executable
// is a no-op on nullptr). Calling with a non-page i64 is UB (same posture as
// free() on a garbage pointer) — the caller is expected to pass exactly what
// make_executable returned.
static void n_free_executable_ptr(int64_t ptr) {
    free_executable(reinterpret_cast<void*>(ptr));
}

// Register the three natives, all PERM_FFI-gated (raw execution is a security
// surface — a script that can mint + call executable pages can branch to
// arbitrary host code). Mirrors ext_io/ext_array's BindingBuilder shape.
void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    b.add("call_raw",            type_i64(), {type_i64(), type_i64()},
          (void*)&n_call_raw,            PERM_FFI);
    b.add("make_executable",     type_i64(), {type_i64()},
          (void*)&n_make_executable,     PERM_FFI);
    b.add("free_executable_ptr", type_void(), {type_i64()},
          (void*)&n_free_executable_ptr, PERM_FFI);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

// Stateless extension: no host slot vector to clear. The executable pages
// minted by make_executable are owned by the caller (freed via
// free_executable_ptr), not tracked here, so reset() has nothing to free.
// Provided for API symmetry with the other extensions (a host that resets all
// stores on unload calls it uniformly).
void reset() {}

} // namespace ember::ext_call_raw
