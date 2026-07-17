// ext_random.hpp — complete example extension from the extension authoring guide.
#pragma once
#include "sema.hpp"

#include <string>
#include <unordered_map>

namespace ember::ext_random {
void register_natives(std::unordered_map<std::string, NativeSig>& natives);
void reset();
} // namespace ember::ext_random
