// ext_graphics.hpp - Ember Win32 + D3D11 graphics extension.
#pragma once

#include "sema.hpp"

#include <string>
#include <unordered_map>

namespace ember::ext_graphics {

// Register the window_*, shader_*, and graphics_* native surface. Every native
// is PERM_FFI-gated. D3D and Win32 resources are created lazily by
// window_create(), never during registration.
void register_natives(std::unordered_map<std::string, NativeSig>& natives);

// Graphics has no operators, but exposes the standard extension hook so hosts
// can treat it uniformly with the other Ember extensions.
void register_overloads(OpOverloadTable& overloads);

// Destroy all programs/windows and release every native graphics resource.
// Safe to call repeatedly.
void reset();

} // namespace ember::ext_graphics
