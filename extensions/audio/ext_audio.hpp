// ext_audio.hpp - realtime-safe raw audio/MIDI buffer access natives.
#pragma once

#include "sema.hpp"
#include <string>
#include <unordered_map>

namespace ember::ext_audio {

// Register load/store natives for host-owned f32, f64, and i32 buffers.
// Every binding is PERM_FFI-gated because an Ember i64 is reinterpreted as a
// raw host pointer. Hosts must validate buffer ranges before entering JIT code.
void register_natives(std::unordered_map<std::string, NativeSig>& natives);

} // namespace ember::ext_audio
