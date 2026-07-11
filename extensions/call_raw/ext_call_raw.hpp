// ext_call_raw.hpp - ember extension: raw x64 execution natives.
//
// An ember *extension* (see ember/extensions/README.md): a reusable,
// non-cheat-specific addon. This ships the THREE natives the self-hosting
// milestone (docs/planning/plan_SELF_HOSTING.md Stage 4 gap) identified as the
// bridge from "the ember-written codegen produces x64 bytes" to "those bytes
// actually run":
//
//   make_executable(bytes: i64) -> i64
//       Copy an array<u8> (the codegen's x64 output) to a W^X executable page
//       and return the entry pointer (0 on failure). RX after this call.
//
//   call_raw(fn_ptr: i64, arg: i64) -> i64
//       Cast fn_ptr to int64_t(*)(int64_t), call it with arg, return the
//       result. This invokes the entry make_executable returned.
//
//   free_executable_ptr(ptr: i64) -> void
//       Release a page make_executable minted.
//
// The end-to-end self-hosting flow is: the ember-written codegen produces x64
// bytes in an array<u8>; make_executable copies them to an RX page; call_raw
// invokes the entry point; free_executable_ptr releases the page. Without
// these, the self-hosted codegen can only produce bytes it cannot run —
// Stage 5 (the bootstrap / full-pipeline demo) is blocked.
//
// === SECURITY POSTURE ===
//
// All three natives are PERM_FFI-gated (minting + calling executable pages is
// a security surface — a script that can do this can branch to arbitrary host
// code). Sema enforces the permission at the call site BEFORE codegen (zero
// runtime cost, no "check bypassed" path; see SAFETY_AND_SANDBOX.md §6). The
// CLI grants PERM_FFI via its --ffi flag; without --ffi, sema rejects every
// call_raw / make_executable / free_executable_ptr call site at compile time.
//
// TWO LAYERS OF DEFENSE (mirrors ext_io):
//   1. REGISTRATION: the host chooses whether to register this extension at
//      all. A host that does not call register_natives has NO raw-execution
//      surface — a script calling these gets "unknown function" (the
//      strongest isolation: the capability does not exist in the module's
//      name resolution).
//   2. PERMISSION: even when registered, PERM_FFI gating means a module
//      must be compiled with the FFI bit to call them. The host grants
//      PERM_FFI only to modules it trusts with raw host calls.
//
// The natives provide RAW CAPABILITY, NOT POLICY. make_executable does not
// validate that the bytes are "safe" machine code (a host that wants that
// wraps the native or scans the bytes); call_raw does not validate that
// fn_ptr points to a JIT page (a host that wants that wraps the native or
// tracks live executable allocations). A caller passing a garbage pointer to
// call_raw gets a process crash — the same posture as a C function pointer
// dereference. A host that wants safety gates the call site (the v0.4
// fn-allowlist + context-reg safe-execution path is the JIT'd-ember-call
// guard; these natives are the escape hatch the self-hosting codegen uses by
// design).
//
// === STATE ===
//
// This extension is STATELESS: the three natives operate directly on their
// arguments with no host-side slot vector. The executable pages minted by
// make_executable are owned by the CALLER (freed via free_executable_ptr), not
// tracked here. reset() is a no-op (provided for API symmetry with the other
// extensions so a host that resets all extension stores on unload calls it
// uniformly); a host that wants to reclaim every live page on unload must
// track them itself (these natives deliberately do not, to keep the execution
// path zero-overhead).
#pragma once
#include "sema.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_call_raw {

// Register the three raw-execution natives into m. All PERM_FFI-gated. Mirrors
// ext_array/ext_string/ext_io's register_natives shape.
void register_natives(std::unordered_map<std::string, NativeSig>& m);

// Clear any host state. Stateless (no slot vector + no tracked pages), so this
// is a no-op -- provided for API symmetry with the other extensions.
void reset();

} // namespace ember::ext_call_raw
