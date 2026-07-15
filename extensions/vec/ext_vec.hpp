// ext_vec.hpp - ember extension: vec2/vec3/vec4 host-store types + operator overloads.
//
// An ember *extension* (see ember/extensions/README.md): a reusable,
// non-cheat-specific set of ember::NativeSig registrations +
// ember::OpOverloadTable entries + the host C++ backing them, shipped
// under ember/extensions/ so any ember consumer can register the same
// vec addon. This is a docs/ROADMAP.md Tier 0 standard addon - pure
// math, no cheat/process/render coupling.
//
// Each vecN is an opaque i64 handle indexing into a host-owned vector
// of POD structs (handle = index+1, so 0 is the universal "null handle").
// The host owns the storage; a host that wants per-run independence calls
// reset() in its own per-run reset (as prism's ResetScriptHostState does).
#pragma once
#include "sema.hpp"      // ember public header: NativeSig, OpOverloadTable
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_vec {

// Register all vec2_*/vec3_*/vec4_* natives into `m`. Only inserts these
// keys; does not touch keys the host already registered.
void register_natives(std::unordered_map<std::string, NativeSig>& m);

// Register vec2/vec3/vec4 Add/Sub/Mul/Eq operator overloads into `t`.
void register_overloads(OpOverloadTable& t);

// Clear the host stores so the next run starts with no stale handles.
// A host that does not care about run-to-run isolation may skip this.
void reset();

// === GC trace-callback integration (c1) — LEAF =============================
// vec2/vec3/vec4 own only floats (no GC-managed children), so vec registers a
// LEAF trace callback (reports nothing) against the current thread-local GC
// runtime (via ext_gc). The registration is observable via
// gc_trace_callback_count() (goes up by exactly 1, idempotent across vec2/3/4)
// and does NOT create a false root (a vec existing keeps no unrelated object
// alive). reset() unregisters the callback. No public-API change — internal to
// ext_vec.cpp; native signatures, opaque-handle behavior, and reset() semantics
// are unchanged. When the GC extension is absent/inactive the facade no-ops,
// so vec works unchanged.

} // namespace ember::ext_vec
