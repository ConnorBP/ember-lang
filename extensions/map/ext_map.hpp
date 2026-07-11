// ext_map.hpp — ember extension: map<K,V> host-store type.
//
// An opaque i64 handle backed by a host-side std::unordered_map<int64_t,int64_t>.
// The handle is a 1-indexed slot into a process-wide vector of maps (same
// pattern as ext_array/ext_string). All ops are bounds-checked + exception-safe.
//
// API: map_new, map_set, map_get, map_contains, map_length, map_remove, map_clear.
// K and V are both i64 (the v1 opaque-handle convention; typed keys/values are
// a v2 concern). A host that needs string keys hashes them to i64 first.
#pragma once
#include "../src/binding_builder.hpp"
#include "../src/sema.hpp"
#include <string>
#include <unordered_map>

namespace ember::ext_map {

// Register the map natives. Mirrors register_natives(NativeTable&).
void register_natives(std::unordered_map<std::string, NativeSig>& m);

// Clear the host store (call between runs for determinism).
void reset();

} // namespace ember::ext_map
