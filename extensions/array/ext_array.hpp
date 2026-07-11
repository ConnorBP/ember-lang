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

} // namespace ember::ext_array
