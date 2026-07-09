// ext_quat.hpp - ember extension: quat host-store type + operator overloads.
//
// An ember *extension* (see ember/extensions/README.md): a reusable,
// non-cheat-specific addon. docs/ROADMAP.md Tier 0 standard addon - pure
// math, no cheat/process/render coupling. The quat's `*` overload is the
// Hamilton product (the mathematically meaningful "multiply" for a
// quaternion), not component-wise.
//
// Opaque i64 handle into a host-owned vector of POD structs
// (handle = index+1, 0 = null). Host owns storage; reset() clears it.
#pragma once
#include "sema.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_quat {

void register_natives(std::unordered_map<std::string, NativeSig>& m);
void register_overloads(OpOverloadTable& t);
void reset();

} // namespace ember::ext_quat
