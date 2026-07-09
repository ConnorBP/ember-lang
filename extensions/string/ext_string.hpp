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

} // namespace ember::ext_string
