// ext_string.hpp - ember extension: mutable string host-store type + overloads.
//
// An ember *extension* (see ember/extensions/README.md): a reusable,
// non-cheat-specific addon. docs/ROADMAP.md Tier 0 standard addon. The
// mutable `string` type: `+` concatenates, `==` compares content (both via
// the operator-overload mechanism, dispatched by sema's overload table).
// Literals stay slice<u8> (rodata, immutable); construct a `string` from
// one explicitly via string_from_slice, the same way Rust's String::from
// or C++'s std::string("...") make an owned string from a literal.
//
// Opaque i64 handle into a host-owned vector of std::string
// (handle = index+1, 0 = null). Host owns storage; reset() clears it.
//
// `slot()` is exposed because the host-sink-coupled `print_string` native
// (which routes through the host's print sink and therefore stays in the
// host, not here) needs to read a string handle's content to print it.
#pragma once
#include "sema.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_string {

void register_natives(std::unordered_map<std::string, NativeSig>& m);
void register_overloads(OpOverloadTable& t);
void reset();

// Read-only access to a string handle's content, or nullptr if the handle
// is invalid. Used by the host's own `print_string` native (which stays in
// the host because it routes through the host print sink).
const std::string* slot(int64_t handle);

// Copy a handle's content while holding the store lock. Prefer this in native
// extensions that may run alongside other contexts, because slot()'s borrowed
// pointer can be invalidated by a concurrent string allocation/reset.
bool copy(int64_t handle, std::string& out);

// Allocate a new string handle owning `s`, returning a 1-based opaque handle
// (0 on allocation failure). Exposed so host-side natives that produce a
// path/id value the script then passes back to another host native (e.g.
// prism's write_temp_file_native -> delete_file_native) can mint an ember
// `string` handle the consumer resolves via slot() - the same round-trip
// print_string already relies on for reading. The handle is owned by this
// extension's host store and freed on reset().
int64_t alloc(std::string s);

// === GC trace-callback integration (c1) — LEAF =============================
// string owns UTF-8 bytes (no GC-managed children), so string registers a LEAF
// trace callback (reports nothing) against the current thread-local GC runtime
// (via ext_gc). The registration is observable via gc_trace_callback_count()
// (goes up by exactly 1, idempotent across string_new / string_from_*) and does
// NOT create a false root (a string existing keeps no unrelated object alive).
// reset() unregisters the callback. No public-API change — internal to
// ext_string.cpp; native signatures, opaque-handle behavior, size limits, and
// reset() semantics are unchanged. When the GC extension is absent/inactive
// the facade no-ops, so string works unchanged.

} // namespace ember::ext_string
