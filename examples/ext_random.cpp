// ext_random.cpp — complete, thread-safe Ember random-number extension.
#include "binding_builder.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ember::ext_random {
namespace {
std::mutex g_mutex;
uint64_t g_state = 0x4d595df4d0f33173ULL;

uint64_t next_u64() {
    uint64_t x = g_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_state = x;
    return x * 2685821657736338717ULL;
}

extern "C" int64_t n_random_next(int64_t max) {
    if (max <= 0) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int64_t>(next_u64() % static_cast<uint64_t>(max));
}

extern "C" void n_random_seed(int64_t seed) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state = seed ? static_cast<uint64_t>(seed) : 0x4d595df4d0f33173ULL;
}

extern "C" float n_random_float() {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Use 24 bits so every result is exactly representable and strictly < 1.
    return static_cast<float>(next_u64() >> 40) * (1.0f / 16777216.0f);
}
} // namespace

void register_natives(std::unordered_map<std::string, NativeSig>& natives) {
    BindingBuilder builder;
    builder.add("random_next", type_i64(), {type_i64()},
                reinterpret_cast<void*>(&n_random_next));
    builder.add("random_seed", type_void(), {type_i64()},
                reinterpret_cast<void*>(&n_random_seed));
    builder.add("random_float", type_f32(), {},
                reinterpret_cast<void*>(&n_random_float));
    NativeTable table = builder.build();
    for (auto& item : table.natives)
        natives[item.first] = std::move(item.second);
}

void reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state = 0x4d595df4d0f33173ULL;
}
} // namespace ember::ext_random
