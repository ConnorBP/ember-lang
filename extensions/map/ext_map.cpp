// ext_map.cpp — ember extension: map<K,V> host-store type.
// See ext_map.hpp for the API. Backed by std::unordered_map<int64_t,int64_t>.
// Same host-store pattern as ext_array (1-indexed handle into a vector of slots).

#include "ext_map.hpp"
#include "ast.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ember::ext_map {

namespace {

struct MapSlot {
    std::unordered_map<int64_t, int64_t> entries;
};

std::vector<MapSlot> g_maps;

// Serializes all g_maps store operations (emplace_back in n_map_new, map_slot
// lookups) so concurrent context_t's calling map natives cannot race on
// vector reallocation (Sec-5). Mirrors the g_store_mutex pattern in ext_sync.cpp
// and g_mutex in ext_lifecycle.cpp.
std::mutex g_store_mutex;

constexpr int64_t MAX_MAPS = 100000;

MapSlot* map_slot(int64_t h) {
    if (h < 1 || h > int64_t(g_maps.size())) return nullptr;
    return &g_maps[size_t(h - 1)];
}

extern "C" {
    static int64_t n_map_new() {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        if (int64_t(g_maps.size()) >= MAX_MAPS) return 0;
        try {
            g_maps.emplace_back();
            return int64_t(g_maps.size());  // 1-indexed handle
        } catch (...) { return 0; }
    }
    static void n_map_set(int64_t h, int64_t k, int64_t v) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = map_slot(h);
        if (s) s->entries[k] = v;
    }
    static int64_t n_map_get(int64_t h, int64_t k) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = map_slot(h);
        if (!s) return 0;
        auto it = s->entries.find(k);
        return it != s->entries.end() ? it->second : 0;
    }
    static int64_t n_map_contains(int64_t h, int64_t k) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = map_slot(h);
        return (s && s->entries.count(k)) ? 1 : 0;
    }
    static int64_t n_map_length(int64_t h) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = map_slot(h);
        return s ? int64_t(s->entries.size()) : 0;
    }
    static void n_map_remove(int64_t h, int64_t k) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = map_slot(h);
        if (s) s->entries.erase(k);
    }
    static void n_map_clear(int64_t h) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = map_slot(h);
        if (s) s->entries.clear();
    }
}

} // namespace

void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    b.add("map_new",      type_i64(),  {},                           (void*)&n_map_new);
    b.add("map_set",      type_void(), {type_i64(),type_i64(),type_i64()}, (void*)&n_map_set);
    b.add("map_get",      type_i64(),  {type_i64(),type_i64()},      (void*)&n_map_get);
    b.add("map_contains", type_i64(),  {type_i64(),type_i64()},      (void*)&n_map_contains);
    b.add("map_length",   type_i64(),  {type_i64()},                 (void*)&n_map_length);
    b.add("map_remove",   type_void(), {type_i64(),type_i64()},      (void*)&n_map_remove);
    b.add("map_clear",    type_void(), {type_i64()},                 (void*)&n_map_clear);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

void reset() { std::lock_guard<std::mutex> lock(g_store_mutex); g_maps.clear(); }

} // namespace ember::ext_map
