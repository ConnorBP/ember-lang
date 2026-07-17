// ext_random_test.cpp — registration and deterministic-sequence smoke test.
#include "binding_builder.hpp"

#include <cstdint>
#include <cstdio>
#include <unordered_map>

namespace ember::ext_random {
void register_natives(std::unordered_map<std::string, NativeSig>& natives);
void reset();
}

int main() {
    std::unordered_map<std::string, ember::NativeSig> natives;
    ember::ext_random::register_natives(natives);
    if (natives.size() != 3 || !natives.count("random_next") ||
        !natives.count("random_seed") || !natives.count("random_float"))
        return 1;

    using Seed = void (*)(int64_t);
    using Next = int64_t (*)(int64_t);
    using Float = float (*)();
    const auto seed = reinterpret_cast<Seed>(natives.at("random_seed").fn_ptr);
    const auto next = reinterpret_cast<Next>(natives.at("random_next").fn_ptr);
    const auto real = reinterpret_cast<Float>(natives.at("random_float").fn_ptr);

    seed(12345);
    const int64_t a = next(1000);
    const float f = real();
    seed(12345);
    if (a != next(1000) || f < 0.0f || f >= 1.0f) return 1;
    ember::ext_random::reset();
    std::printf("ext_random: deterministic registration smoke test passed\n");
    return 0;
}
