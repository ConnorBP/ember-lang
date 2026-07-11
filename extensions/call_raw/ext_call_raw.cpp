// ext_call_raw.cpp - ember extension: raw x64 function-pointer call native.
//
// Implements call_raw(fn_ptr: i64, arg: i64) -> i64: cast fn_ptr to
// int64_t(*)(int64_t) and call it with arg. PERM_FFI-gated (see ext_call_raw.hpp
// for the security posture). Stateless (no host slot vector); reset() is a
// no-op.
//
// This is the one native the self-hosting milestone (docs/planning/plan_SELF_HOSTING.md
// Stage 4) identified as the gap: it lets the ember-written codegen's output
// be executed (codegen produces x64 bytes in array<u8> -> host copies them to
// an executable page via alloc_executable_rw + seal_executable -> call_raw
// invokes the entry point). See ext_call_raw.hpp for the full framing.
#include "ext_call_raw.hpp"
#include "ast.hpp"              // type_i64, make_prim
#include "binding_builder.hpp"  // BindingBuilder, PERM_FFI

#include <cstdint>

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
    Fn f = reinterpret_cast<Fn>(fn_ptr);
    return f(arg);
}

// Register the single native, PERM_FFI-gated. Mirrors ext_io/ext_array's
// BindingBuilder shape verbatim.
void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    b.add("call_raw", type_i64(), {type_i64(), type_i64()},
          (void*)&n_call_raw, PERM_FFI);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

// Stateless extension: no host slot vector to clear. Provided for API symmetry
// with the other extensions (a host that resets all stores on unload calls it
// uniformly).
void reset() {}

} // namespace ember::ext_call_raw
