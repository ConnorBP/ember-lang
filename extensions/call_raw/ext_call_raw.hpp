// ext_call_raw.hpp - ember extension: raw x64 function-pointer call native.
//
// An ember *extension* (see ember/extensions/README.md): a reusable,
// non-cheat-specific addon. This ships the ONE native the self-hosting
// milestone (docs/planning/plan_SELF_HOSTING.md Stage 4 gap) identified:
//
//   call_raw(fn_ptr: i64, arg: i64) -> i64
//
// `call_raw` casts `fn_ptr` to `int64_t(*)(int64_t)` and calls it with `arg`,
// returning the result. This is the bridge that lets the self-hosted
// codegen's output be EXECUTED: the ember-written codegen produces x64 bytes
// in an array<u8>, the host copies them to an executable page
// (alloc_executable_rw + seal_executable), and `call_raw` invokes the entry
// point. Without it, the self-hosted codegen can only produce bytes it cannot
// run — Stage 5 (the bootstrap) is blocked.
//
// === SECURITY POSTURE ===
//
// `call_raw` is a PERM_FFI-gated native (calling raw function pointers is a
// security surface — a script that can call any i64-as-pointer can branch to
// arbitrary host code). Sema enforces the permission at the call site BEFORE
// codegen (zero runtime cost, no "check bypassed" path; see SAFETY_AND_SANDBOX.md
// §6). The CLI grants PERM_FFI via its --ffi flag; without --ffi, sema rejects
// every call_raw call site at compile time.
//
// TWO LAYERS OF DEFENSE (mirrors ext_io):
//   1. REGISTRATION: the host chooses whether to register this extension at
//      all. A host that does not call register_natives has NO call_raw
//      surface — a script calling call_raw gets "unknown function" (the
//      strongest isolation: the capability does not exist in the module's
//      name resolution).
//   2. PERMISSION: even when registered, PERM_FFI gating means a module
//      must be compiled with the FFI bit to call call_raw. The host grants
//      PERM_FFI only to modules it trusts with raw host calls.
//
// The native provides RAW CAPABILITY, NOT POLICY. It does not validate that
// fn_ptr points to a JIT page (a host that wants that wraps the native or
// tracks live executable allocations). A caller passing a garbage pointer
// gets a process crash — the same posture as a C function pointer dereference.
// A host that wants safety gates the call site (the v0.4 fn-allowlist +
// context-reg safe-execution path is the JIT'd-ember-call guard; call_raw is
// the escape hatch the self-hosting codegen uses by design).
//
// === STATE ===
//
// This extension is STATELESS: call_raw dereferences fn_ptr directly with no
// host-side slot vector. reset() is a no-op (provided for API symmetry with
// the other extensions so a host that resets all extension stores on unload
// calls it uniformly).
#pragma once
#include "sema.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_call_raw {

// Register the call_raw native into m. PERM_FFI-gated. Mirrors
// ext_array/ext_string/ext_io's register_natives shape.
void register_natives(std::unordered_map<std::string, NativeSig>& m);

// Clear any host state. Stateless (no slot vector), so this is a no-op --
// provided for API symmetry with the other extensions.
void reset();

} // namespace ember::ext_call_raw
