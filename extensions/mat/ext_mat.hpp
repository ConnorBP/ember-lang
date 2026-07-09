// ext_mat.hpp - ember extension: mat4 host-store type + operator overloads.
//
// An ember *extension* (see ember/extensions/README.md): a reusable,
// non-cheat-specific addon. docs/ROADMAP.md Tier 0 standard addon - pure
// math, no cheat/process/render coupling. mat4's `*` overload is the
// standard 4x4 row-major matrix product; addition is not a commonly
// meaningful op so (unlike vec/quat) it is omitted.
//
// Opaque i64 handle into a host-owned vector of 16-float POD structs
// (row-major; handle = index+1, 0 = null). Host owns storage; reset()
// clears it.
#pragma once
#include "sema.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_mat {

void register_natives(std::unordered_map<std::string, NativeSig>& m);
void register_overloads(OpOverloadTable& t);
void reset();

} // namespace ember::ext_mat
