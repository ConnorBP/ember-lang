// ext_ui.hpp - Dear ImGui widget bindings for an Ember-owned UI frame.
#pragma once

#include "sema.hpp"

#include <string>
#include <unordered_map>

namespace ember::ext_ui {

// Registers PERM_FFI-gated bindings. Calls are harmless when no ImGui frame is
// active, allowing CLI/bundled scripts to be validated without an editor.
void register_natives(std::unordered_map<std::string, NativeSig>& natives);

} // namespace ember::ext_ui
