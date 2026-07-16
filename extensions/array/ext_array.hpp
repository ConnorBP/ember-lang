// ext_array.hpp - ember extension: array<T> host-store type.
//
// An ember *extension* (see ember/extensions/README.md): a reusable,
// non-cheat-specific addon. docs/ROADMAP.md Tier 0 standard addon - a
// host-allocated growable byte buffer behind an opaque i64 handle, with
// typed get/set (u8/f32/i64) + length/resize/push. Internal bounds checks
// in every indexing native (matches the original prism semantics: an
// out-of-bounds index is a no-op read returning 0 / a no-op write, not a
// runtime trap and not an auto-resize).
//
// Opaque i64 handle into a host-owned vector of {bytes, elem_size}
// (handle = index+1, 0 = null). Host owns storage; reset() clears it.
//
// `get_bytes()` is exposed because host-side natives that receive an
// array<u8> handle (a host's process-memory-read-into-array native, or
// a host's custom shader draw calls that take array<u8> vertex data)
// need to read the array's backing bytes without going through a
// registered accessor native. The host keeps its own thin wrapper
// (e.g. prism::GetArrayBytes) that forwards here.
#pragma once
#include "sema.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_array {

void register_natives(std::unordered_map<std::string, NativeSig>& m);
// array<T> has no operator overloads (it's accessed via method-call
// natives, not infix operators), so no register_overloads().
void reset();

// Read-only/read-write view into an array<T> host object's backing bytes.
// Returns false for an invalid handle. Used by host natives that receive
// an array<u8> handle and need its raw bytes.
bool get_bytes(int64_t handle, uint8_t** out_data, int64_t* out_len);

// Copy an array<f32> into caller-owned storage while holding the array store
// lock. Returns false unless the handle's element size is exactly 4, the
// destination is valid, and the element count is within max_count. This keeps
// consumers such as the graphics constant-buffer upload from accepting a
// same-byte-length array<u8> or retaining a pointer across concurrent resize.
bool copy_f32(int64_t handle, float* out_data, int64_t max_count,
              int64_t* out_count);

// Allocate a new array<u8> (elem_size=1) handle owning a copy of [data, data+len).
// Returns a 1-based opaque handle (0 on allocation failure or bad len). Exposed
// so host-side natives that PRODUCE a byte buffer (e.g. ext_io::file_read_bytes)
// can mint an ember array<u8> handle the consumer resolves via get_bytes() --
// the same round-trip read-side/get_bytes already relies on for reading. This
// is the array-side analogue of ext_string::alloc. If data is null and len is
// 0, a valid empty array handle is returned (a 0-byte file reads as an empty
// array, not a failure). The handle is owned by this extension's host store
// and freed on reset().
int64_t alloc_bytes(const uint8_t* data, int64_t len);

// Allocate a typed array<f32> handle. Unlike alloc_bytes, this records an
// element size of four so array_get_f32 and for-each type checks accept it.
int64_t alloc_f32(const float* data, int64_t count);

// === GC trace-callback integration (c1) ====================================
// array<T> registers an idempotent trace callback against the current
// thread-local GC runtime (via ext_gc) so an UNPINNED GC object stored in an
// i64 element (elem_size==8) survives collection, then is reclaimed when the
// entry is removed / cleared / the extension is reset. The callback visits
// only aligned i64 elements of elem_size==8 slots; the heap visitor filters
// ordinary integers + stale/non-live addresses. array_set_i64 / array_push_i64
// also invoke the GC write barrier (the array is external-root storage, not a
// live GC object, so the barrier filters the owner today; the call site is
// forward-compatible for a future remembered set). No public-API change — the
// integration is internal to ext_array.cpp; native signatures, opaque-handle
// behavior, size limits, and reset() semantics are unchanged. When the GC
// extension is absent/inactive the facade no-ops, so array works unchanged.

} // namespace ember::ext_array
