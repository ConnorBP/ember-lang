// ext_math.hpp - ember extension: math natives (sqrt/sin/cos/tan, f32).
//
// An ember *extension* (see ember/extensions/README.md): a reusable,
// non-cheat-specific addon. docs/ROADMAP.md Tier 0 standard `math` addon
// (a subset today: sqrt/sin/cos/tan, all f32). Pure functions over f32,
// no host store, so no reset() - stateless.
//
// The wider ROADMAP Tier 0 math set (floor/ceil/abs/min/max/pow, f32+f64)
// is not authored here speculatively (YAGNI); this extension relocates
// only the math natives that already existed in prism. A future consumer
// that wants more adds them to this extension's register_natives.
#pragma once
#include "sema.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_math {

// Register sqrt/sin/cos/tan (f32) into `m`. Stateless - no reset().
void register_natives(std::unordered_map<std::string, NativeSig>& m);

} // namespace ember::ext_math
