// ext_map.cpp — ember extension: map<K,V> host-store type.
// See ext_map.hpp for the API. Backed by std::unordered_map<int64_t,int64_t>.
// Same host-store pattern as ext_array (1-indexed handle into a vector of slots).

#include "ext_map.hpp"
#include "ast.hpp"
#include "../gc/ext_gc.hpp"     // c1 GC trace-callback + write-barrier facade

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

// ===========================================================================
// c1 GC trace-callback integration.
//
// A map<K,V> stores {int64_t key, int64_t value} pairs; BOTH halves are i64 and
// can hold a GC object pointer (a script may store a handle as a key or as a
// value). For an UNPINNED GC object stored either way to survive gc_collect,
// the map store must report candidate pointer-bearing keys AND values to the
// collector. We register ONE idempotent trace callback per thread (against that
// thread's thread-local GC runtime via the ext_gc facade) that walks g_maps
// under g_store_mutex and reports every entry's key and value via
// visitor.report(). The heap visitor validates each candidate (live?) and
// rejects ordinary integers, null, and stale / non-live addresses -- so a
// plain-integer key/value NEVER creates a false root.
//
// The token is THREAD-LOCAL (the GC runtime is thread-local); the callback
// walks the process-wide, mutex-protected store. See ext_array.cpp for the full
// synchronization / deadlock / lifecycle rationale (identical here): the
// callback acquires g_store_mutex during collect(); mutations acquire it and
// call gc_write_barrier (no mutex, never collects); gc_reset() invalidates
// tokens and the next mutation re-registers; reset() unregisters + clears.
static thread_local ember::gc::GcTraceToken g_trace_token = 0;

// The trace callback: walk every map, report each entry's key + value as root
// candidates. user_data is unused (the callback walks the file-static store).
// ACQUIRES g_store_mutex so the process-wide store is stable while it is
// walked: collect() is invoked from gc_collect / gc_alloc_env on the owning
// thread OUTSIDE any store-mutex-held mutation (a mutation holds g_store_mutex
// and calls gc_write_barrier, which acquires NO mutex and never collects), so
// locking here cannot self-deadlock + cannot nest with a mutation's lock.
// Without this lock a concurrent mutation on another thread (g_maps is
// process-wide) would race the callback's read of the entry table.
static void map_trace_cb(void* /*user_data*/, ember::gc::GcTraceVisitor& visitor) {
    std::lock_guard<std::mutex> lock(g_store_mutex);
    for (const MapSlot& slot : g_maps) {
        for (const auto& kv : slot.entries) {
            visitor.report(reinterpret_cast<void*>(static_cast<uintptr_t>(kv.first)));
            visitor.report(reinterpret_cast<void*>(static_cast<uintptr_t>(kv.second)));
        }
    }
}

// Ensure exactly one trace callback is registered on this thread's GC runtime,
// re-registering if the previous token was invalidated by gc_reset(). Called
// UNDER g_store_mutex by n_map_set. No-op when the GC runtime is not
// initialized on this thread (pure non-GC mode).
static void ensure_gc_trace_cb() {
    if (!ember::ext_gc::gc_runtime_initialized()) return;
    if (g_trace_token != 0) {
        ember::ext_gc::gc_unregister_trace_callback(g_trace_token);  // no-op if stale
    }
    g_trace_token = ember::ext_gc::gc_register_trace_callback(nullptr, &map_trace_cb);
}

// Unregister this thread's trace callback (teardown). Called UNDER g_store_mutex
// by reset(). A no-op if never registered / already invalidated by gc_reset.
static void drop_gc_trace_cb() {
    if (g_trace_token != 0) {
        ember::ext_gc::gc_unregister_trace_callback(g_trace_token);
        g_trace_token = 0;
    }
}


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
        if (s) {
            s->entries[k] = v;
            // c1: the key AND value are i64 candidates for a managed pointer
            // (insertion or replacement). Register the trace callback (idempotent)
            // so a future collect sees this entry, and invoke the write barrier for
            // both halves (owner = the external-root slot, a non-GC owner so the
            // barrier is a ceremonial no-op today; children = the key + value,
            // candidate GC pointers the visitor validates). A replacement also
            // fires the barrier for the new value; the OLD value is simply no
            // longer reported by the callback -> collected if otherwise unreachable.
            ensure_gc_trace_cb();
            void* owner = reinterpret_cast<void*>(s);
            ember::ext_gc::gc_write_barrier(owner,
                reinterpret_cast<void*>(static_cast<uintptr_t>(k)));
            ember::ext_gc::gc_write_barrier(owner,
                reinterpret_cast<void*>(static_cast<uintptr_t>(v)));
        }
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

void reset() {
    std::lock_guard<std::mutex> lock(g_store_mutex);
    g_maps.clear();
    // c1: drop this thread's trace callback so it does not outlive the store.
    drop_gc_trace_cb();
}

} // namespace ember::ext_map
