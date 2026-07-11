// ext_map_test - runtime coverage for the map<K,V> host-store extension
// (extensions/map/ext_map.{hpp,cpp}). D1: the map extension ships 7 natives
// (map_new, map_set, map_get, map_contains, map_length, map_remove, map_clear)
// but had zero ctest coverage. This test closes that gap.
//
// The map natives are pure host-side ops over opaque i64 handles (a 1-indexed
// slot into a host-side vector of std::unordered_map<int64_t,int64_t>). There is
// no script-level surface to exercise that the JIT would transform — the
// natives take i64 args and return i64/void directly — so the direct-coverage
// shape is to call the registered native function pointers from the natives
// table (the same pattern ext_lifecycle_test.cpp uses for register_routine /
// unregister_routine). This exercises the real registered natives end-to-end
// (register_natives -> fn_ptr -> host store) without needing a JIT round-trip,
// and asserts the host-store contract: 1-indexed handles, set/get/contains/
// length/remove/clear semantics, missing-key behavior.
//
// Links ember + ember_frontend + ember_ext_map (the CMake target created by
// ember_add_extension(map ...)). No prism dependency — proving the extension is
// reusable outside prism, matching ext_runtime_test's link shape.

#include "../src/binding_builder.hpp"
#include "../src/sema.hpp"
#include "../extensions/map/ext_map.hpp"

#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>

static int g_fail = 0;
static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    std::printf("=== ember map extension test (extensions/map/ext_map.cpp) ===\n");

    // Start from a clean host store.
    ember::ext_map::reset();

    // Register the 7 map natives into a table, then pull the fn_ptr for each.
    // This is the load-bearing line: if register_natives left a native
    // unregistered or mis-typed, the lookup below returns end() and we fail.
    std::unordered_map<std::string, ember::NativeSig> natives;
    ember::ext_map::register_natives(natives);

    auto grab = [&](const char* name) -> void* {
        auto it = natives.find(name);
        if (it == natives.end()) {
            std::printf("FAIL: native '%s' not registered\n", name);
            g_fail = 1;
            return nullptr;
        }
        return it->second.fn_ptr;
    };

    // Native function pointer typedefs (Win64 ABI; all i64/void).
    using F_map_new      = int64_t(*)();
    using F_map_set      = void(*)(int64_t, int64_t, int64_t);
    using F_map_get      = int64_t(*)(int64_t, int64_t);
    using F_map_contains = int64_t(*)(int64_t, int64_t);
    using F_map_length   = int64_t(*)(int64_t);
    using F_map_remove   = void(*)(int64_t, int64_t);
    using F_map_clear    = void(*)(int64_t);

    auto p_new      = reinterpret_cast<F_map_new>(grab("map_new"));
    auto p_set      = reinterpret_cast<F_map_set>(grab("map_set"));
    auto p_get      = reinterpret_cast<F_map_get>(grab("map_get"));
    auto p_contains = reinterpret_cast<F_map_contains>(grab("map_contains"));
    auto p_length   = reinterpret_cast<F_map_length>(grab("map_length"));
    auto p_remove   = reinterpret_cast<F_map_remove>(grab("map_remove"));
    auto p_clear    = reinterpret_cast<F_map_clear>(grab("map_clear"));
    if (g_fail) {
        std::printf("\nember map extension test: FAIL (registration incomplete)\n");
        return 1;
    }

    // ---- T1: map_new returns a 1-indexed handle (>= 1); length is 0 ----
    int64_t h = p_new();
    check(h >= 1, "T1: map_new() returned a 1-indexed handle (>= 1)");
    check(p_length(h) == 0, "T1: map_length on a fresh map is 0");

    // ---- T2: map_set inserts; map_get reads back; contains reflects it ----
    p_set(h, 10, 100);
    p_set(h, 20, 200);
    p_set(h, 30, 300);
    check(p_length(h) == 3, "T2: map_length is 3 after three map_set calls");
    check(p_get(h, 10) == 100, "T2: map_get(10) == 100");
    check(p_get(h, 20) == 200, "T2: map_get(20) == 200");
    check(p_get(h, 30) == 300, "T2: map_get(30) == 300");
    check(p_contains(h, 10) == 1, "T2: map_contains(10) == 1 (present)");
    check(p_contains(h, 20) == 1, "T2: map_contains(20) == 1 (present)");
    check(p_contains(h, 30) == 1, "T2: map_contains(30) == 1 (present)");

    // ---- T3: map_set on an existing key overwrites the value ----
    p_set(h, 20, 222);
    check(p_get(h, 20) == 222, "T3: map_set on existing key overwrites (20 -> 222)");
    check(p_length(h) == 3, "T3: overwrite does not grow the map (length still 3)");

    // ---- T4: map_remove drops a key; length decreases; get/contains reflect it ----
    p_remove(h, 20);
    check(p_length(h) == 2, "T4: map_length is 2 after removing key 20");
    check(p_contains(h, 20) == 0, "T4: map_contains(20) == 0 after removal");
    check(p_get(h, 20) == 0, "T4: map_get(20) == 0 after removal (missing key)");
    // The other keys are untouched.
    check(p_get(h, 10) == 100, "T4: map_get(10) still 100 (untouched by remove)");
    check(p_get(h, 30) == 300, "T4: map_get(30) still 300 (untouched by remove)");

    // ---- T5: map_get on a missing key returns 0 (never-present key) ----
    check(p_get(h, 999) == 0, "T5: map_get on a never-present key (999) returns 0");

    // ---- T6: map_remove on a missing key is a no-op (no crash, length unchanged) ----
    int64_t len_before = p_length(h);
    p_remove(h, 999);          // never present
    p_remove(h, 20);           // already removed
    check(p_length(h) == len_before, "T6: map_remove on missing keys is a no-op (length unchanged)");

    // ---- T7: map_contains on a missing key returns 0 ----
    check(p_contains(h, 999) == 0, "T7: map_contains on a missing key (999) returns 0");
    check(p_contains(h, 20) == 0, "T7: map_contains on an already-removed key (20) returns 0");

    // ---- T8: map_clear empties the map; length is 0; all keys gone ----
    p_clear(h);
    check(p_length(h) == 0, "T8: map_length is 0 after map_clear");
    check(p_contains(h, 10) == 0, "T8: map_contains(10) == 0 after clear");
    check(p_get(h, 10) == 0, "T8: map_get(10) == 0 after clear");
    // The handle is still valid — set/get work after a clear.
    p_set(h, 40, 400);
    check(p_length(h) == 1, "T8: map is reusable after clear (set -> length 1)");
    check(p_get(h, 40) == 400, "T8: map_get(40) == 400 after post-clear set");

    // ---- T9: ops on an invalid handle (0 / out-of-range) are safe no-ops ----
    // map_new returns 0 only at the MAX_MAPS cap; here we just confirm the
    // bounds-checked paths on a bogus handle don't crash and return 0/void.
    check(p_length(0) == 0, "T9: map_length(0) on invalid handle returns 0");
    check(p_get(0, 1) == 0, "T9: map_get(0, 1) on invalid handle returns 0");
    check(p_contains(0, 1) == 0, "T9: map_contains(0, 1) on invalid handle returns 0");
    p_set(0, 1, 1);      // must not crash
    p_remove(0, 1);      // must not crash
    p_clear(0);          // must not crash
    check(true, "T9: set/remove/clear on invalid handle did not crash");

    // ---- T10: reset() clears the host store; the handle is no longer valid ----
    ember::ext_map::reset();
    check(p_length(h) == 0, "T10: map_length on stale handle after reset() returns 0");

    std::printf("\nember map extension test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
