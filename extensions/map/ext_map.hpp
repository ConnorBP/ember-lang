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

// === GC trace-callback integration (c1) ====================================
// map<K,V> registers an idempotent trace callback against the current
// thread-local GC runtime (via ext_gc) so an UNPINNED GC object stored as a
// key or value survives collection, then is reclaimed when the entry is
// removed / cleared / the extension is reset. The callback reports every
// entry's key AND value as a candidate pointer; the heap visitor filters
// ordinary integers + stale/non-live addresses. map_set also invokes the GC
// write barrier (the map is external-root storage, not a live GC object, so
// the barrier filters the owner today; forward-compatible for a remembered
// set). No public-API change — internal to ext_map.cpp; native signatures,
// opaque-handle behavior, size limits, and reset() semantics are unchanged.
// When the GC extension is absent/inactive the facade no-ops, so map works
// unchanged.

} // namespace ember::ext_map
