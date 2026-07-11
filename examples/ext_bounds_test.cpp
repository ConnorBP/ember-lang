// ext_bounds_test - runtime bounds-checking coverage for the array<T> and
// string host-store extensions (extensions/array/ext_array.{hpp,cpp},
// extensions/string/ext_string.{hpp,cpp}). Closes test gaps D4 + D5 from
// docs/audit/AUDIT_2026-07-11_DOCS_TESTS.md.
//
// D4: array_get_u8 / array_set_u8 / array_get_i64 / array_set_i64 index
// bounds. The array natives guard every index with `i>=0 && i<size` (u8:
// byte index vs bytes.size(); i64: element index vs bytes.size()/elem_size).
// An out-of-bounds or negative index is a no-op on set and returns 0 on get
// — never a runtime trap, never an auto-resize. This test exercises both
// sides of that contract: the read returns 0, and the write is a true no-op
// (leaves the valid slots untouched).
//
// D5: string_char_at / string_substr bounds. char_at returns 0 for a
// negative index or an index >= length. substr returns an empty string for a
// negative start or a start >= length, and otherwise clamps the length: a
// negative len means "to the end", and a len past the end is clamped to the
// remaining count. This test exercises every documented edge.
//
// The array/string indexing natives are pure host-side ops over opaque i64
// handles (no script-level surface the JIT would transform), so the direct-
// coverage shape is to call the registered native fn_ptrs from the natives
// table (the same pattern ext_map_test.cpp uses). This exercises the real
// registered natives end-to-end (register_natives -> fn_ptr -> host store)
// without needing a JIT round-trip.
//
// Links ember + ember_frontend + ember_ext_array + ember_ext_string. No
// prism dependency — proving the extensions are reusable outside prism,
// matching ext_map_test's link shape.

#include "../src/sema.hpp"
#include "ext_array.hpp"
#include "ext_string.hpp"

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
    std::printf("=== ember array+string bounds test (D4+D5) ===\n");

    // Start from a clean host store on both extensions.
    ember::ext_array::reset();
    ember::ext_string::reset();

    // Register the array + string natives into one table, then pull fn_ptrs.
    // If register_natives left a native unregistered, the lookup returns
    // end() and we fail fast.
    std::unordered_map<std::string, ember::NativeSig> natives;
    ember::ext_array::register_natives(natives);
    ember::ext_string::register_natives(natives);

    auto grab = [&](const char* name) -> void* {
        auto it = natives.find(name);
        if (it == natives.end()) {
            std::printf("FAIL: native '%s' not registered\n", name);
            g_fail = 1;
            return nullptr;
        }
        return it->second.fn_ptr;
    };

    // Native function pointer typedefs (Win64 ABI; i64 handles + indices).
    // n_array_get_u8 returns int64_t (the U8 prim is widened in the C impl);
    // n_string_char_at returns int64_t; n_string_substr returns a new handle.
    using F_array_new    = int64_t(*)(int64_t, int64_t);
    using F_array_get_u8 = int64_t(*)(int64_t, int64_t);
    using F_array_set_u8 = void(*)(int64_t, int64_t, int64_t);
    using F_array_get_i64 = int64_t(*)(int64_t, int64_t);
    using F_array_set_i64 = void(*)(int64_t, int64_t, int64_t);
    using F_string_char_at = int64_t(*)(int64_t, int64_t);
    using F_string_substr  = int64_t(*)(int64_t, int64_t, int64_t);

    auto p_array_new    = reinterpret_cast<F_array_new>(grab("array_new"));
    auto p_get_u8       = reinterpret_cast<F_array_get_u8>(grab("array_get_u8"));
    auto p_set_u8       = reinterpret_cast<F_array_set_u8>(grab("array_set_u8"));
    auto p_get_i64      = reinterpret_cast<F_array_get_i64>(grab("array_get_i64"));
    auto p_set_i64      = reinterpret_cast<F_array_set_i64>(grab("array_set_i64"));
    auto p_char_at      = reinterpret_cast<F_string_char_at>(grab("string_char_at"));
    auto p_substr       = reinterpret_cast<F_string_substr>(grab("string_substr"));
    if (g_fail) {
        std::printf("\nember array+string bounds test: FAIL (registration incomplete)\n");
        return 1;
    }

    // ========================================================================
    // D4: array<T> get/set index bounds
    // ========================================================================
    std::printf("\n--- D4: array bounds ---\n");

    // ---- u8 array: elem_size=1, count=4 -> bytes.size()=4, indices 0..3 ----
    // index is a BYTE index; size (boundary) == 4; last valid == 3.
    int64_t u = p_array_new(1, 4);
    check(u >= 1, "D4 u8: array_new(1,4) returned a 1-indexed handle");

    // valid index 0 (set + read-back)
    p_set_u8(u, 0, 42);
    check(p_get_u8(u, 0) == 42, "D4 u8: set/get at valid index 0");

    // last valid index (size-1 == 3)
    p_set_u8(u, 3, 99);
    check(p_get_u8(u, 3) == 99, "D4 u8: set/get at last valid index 3 (size-1)");

    // fill the middle so no-op writes can be cross-checked against it
    p_set_u8(u, 1, 10);
    p_set_u8(u, 2, 20);
    check(p_get_u8(u, 1) == 10 && p_get_u8(u, 2) == 20, "D4 u8: middle slots 1,2 set");

    // negative index: get returns 0
    check(p_get_u8(u, -1) == 0, "D4 u8: get(-1) returns 0 (negative index)");
    check(p_get_u8(u, -100) == 0, "D4 u8: get(-100) returns 0 (negative index)");

    // negative index: set is a no-op (valid slots unchanged)
    p_set_u8(u, -1, 7);
    p_set_u8(u, -100, 8);
    check(p_get_u8(u, 0) == 42 && p_get_u8(u, 1) == 10 && p_get_u8(u, 2) == 20 && p_get_u8(u, 3) == 99,
          "D4 u8: set(-1)/set(-100) are no-ops (all valid slots unchanged)");

    // OOB index == size (boundary just past the end): get returns 0
    check(p_get_u8(u, 4) == 0, "D4 u8: get(4) returns 0 (index == size boundary)");

    // OOB index == size: set is a no-op
    p_set_u8(u, 4, 7);
    check(p_get_u8(u, 3) == 99, "D4 u8: set(4) is a no-op (last valid slot 3 unchanged)");
    check(p_get_u8(u, 4) == 0, "D4 u8: set(4) did not grow the array (get(4) still 0)");

    // OOB index far past the end: get returns 0, set is a no-op
    check(p_get_u8(u, 100) == 0, "D4 u8: get(100) returns 0 (OOB far)");
    p_set_u8(u, 100, 7);
    check(p_get_u8(u, 0) == 42, "D4 u8: set(100) is a no-op (slot 0 unchanged)");

    // ops on an invalid handle are safe no-ops / return 0
    check(p_get_u8(0, 0) == 0, "D4 u8: get on invalid handle returns 0");
    check(p_get_u8(99999, 0) == 0, "D4 u8: get on out-of-range handle returns 0");
    p_set_u8(0, 0, 1);       // must not crash
    p_set_u8(99999, 0, 1);   // must not crash
    check(true, "D4 u8: set on invalid handle did not crash");

    // ---- i64 array: elem_size=8, count=4 -> bytes.size()=32, ----
    // index is an ELEMENT index vs bytes.size()/elem_size == 4; indices 0..3;
    // size (boundary) == 4; last valid == 3.
    int64_t q = p_array_new(8, 4);
    check(q >= 1, "D4 i64: array_new(8,4) returned a 1-indexed handle");

    // valid index 0 (set + read-back, use a value that exercises all 8 bytes)
    p_set_i64(q, 0, 123456789);
    check(p_get_i64(q, 0) == 123456789, "D4 i64: set/get at valid index 0");

    // last valid index (count-1 == 3), including a negative value
    p_set_i64(q, 3, -7);
    check(p_get_i64(q, 3) == -7, "D4 i64: set/get at last valid index 3 (count-1), negative value");

    // fill the middle
    p_set_i64(q, 1, 1111);
    p_set_i64(q, 2, 2222);
    check(p_get_i64(q, 1) == 1111 && p_get_i64(q, 2) == 2222, "D4 i64: middle slots 1,2 set");

    // negative index: get returns 0
    check(p_get_i64(q, -1) == 0, "D4 i64: get(-1) returns 0 (negative index)");
    check(p_get_i64(q, -100) == 0, "D4 i64: get(-100) returns 0 (negative index)");

    // negative index: set is a no-op
    p_set_i64(q, -1, 7);
    p_set_i64(q, -100, 8);
    check(p_get_i64(q, 0) == 123456789 && p_get_i64(q, 1) == 1111 &&
          p_get_i64(q, 2) == 2222 && p_get_i64(q, 3) == -7,
          "D4 i64: set(-1)/set(-100) are no-ops (all valid slots unchanged)");

    // OOB index == count (boundary): get returns 0
    check(p_get_i64(q, 4) == 0, "D4 i64: get(4) returns 0 (index == count boundary)");

    // OOB index == count: set is a no-op
    p_set_i64(q, 4, 7);
    check(p_get_i64(q, 3) == -7, "D4 i64: set(4) is a no-op (last valid slot 3 unchanged)");
    check(p_get_i64(q, 4) == 0, "D4 i64: set(4) did not grow the array (get(4) still 0)");

    // OOB far: get returns 0, set is a no-op
    check(p_get_i64(q, 100) == 0, "D4 i64: get(100) returns 0 (OOB far)");
    p_set_i64(q, 100, 7);
    check(p_get_i64(q, 0) == 123456789, "D4 i64: set(100) is a no-op (slot 0 unchanged)");

    // ops on an invalid handle are safe no-ops / return 0
    check(p_get_i64(0, 0) == 0, "D4 i64: get on invalid handle returns 0");
    check(p_get_i64(99999, 0) == 0, "D4 i64: get on out-of-range handle returns 0");
    p_set_i64(0, 0, 1);       // must not crash
    p_set_i64(99999, 0, 1);   // must not crash
    check(true, "D4 i64: set on invalid handle did not crash");

    // cross-type guard: i64 natives on a u8 array (elem_size != 8) return 0 /
    // are no-ops (the elem_size==8 guard in the i64 natives rejects them).
    check(p_get_i64(u, 0) == 0, "D4 cross: get_i64 on a u8 array returns 0 (elem_size guard)");
    p_set_i64(u, 0, 999);
    check(p_get_u8(u, 0) == 42, "D4 cross: set_i64 on a u8 array is a no-op (u8 slot 0 unchanged)");

    // ========================================================================
    // D5: string char_at / substr bounds
    // ========================================================================
    std::printf("\n--- D5: string bounds ---\n");

    // Source string "hello" (length 5, byte indices 0..4: h e l l o).
    // Built via the host-side alloc() helper (the documented path for a host
    // that mints a known string handle); char_at/substr are exercised through
    // their registered natives, which is what D5 covers.
    const std::string src_text = "hello";
    int64_t s = ember::ext_string::alloc(src_text);
    check(s >= 1, "D5: alloc(\"hello\") returned a 1-indexed handle");

    // ---- char_at ----
    // valid index 0
    check(p_char_at(s, 0) == 'h', "D5 char_at: index 0 -> 'h' (104)");
    // valid middle index
    check(p_char_at(s, 2) == 'l', "D5 char_at: index 2 -> 'l' (108)");
    // last valid index (length-1 == 4)
    check(p_char_at(s, 4) == 'o', "D5 char_at: index 4 -> 'o' (111) (last valid, length-1)");
    // negative index -> 0
    check(p_char_at(s, -1) == 0, "D5 char_at: index -1 -> 0 (negative)");
    check(p_char_at(s, -100) == 0, "D5 char_at: index -100 -> 0 (negative)");
    // index == length (boundary) -> 0
    check(p_char_at(s, 5) == 0, "D5 char_at: index 5 -> 0 (index == length boundary)");
    // index > length (OOB) -> 0
    check(p_char_at(s, 100) == 0, "D5 char_at: index 100 -> 0 (OOB)");
    // invalid handle -> 0
    check(p_char_at(0, 0) == 0, "D5 char_at: invalid handle -> 0");
    check(p_char_at(99999, 0) == 0, "D5 char_at: out-of-range handle -> 0");

    // ---- substr ----
    // Helper: call substr(src, start, len) and compare the result handle's
    // content (read back via ext_string::slot) to `expected`. The empty cases
    // return a VALID handle to "" (str_new("")), not 0, so slot() resolves it.
    auto substr_eq = [&](int64_t start, int64_t len, const std::string& expected) -> bool {
        int64_t r = p_substr(s, start, len);
        const std::string* p = ember::ext_string::slot(r);
        return p && *p == expected;
    };

    // negative start -> empty
    check(substr_eq(-1, 2, ""), "D5 substr: start=-1 -> empty (negative start)");
    check(substr_eq(-100, 2, ""), "D5 substr: start=-100 -> empty (negative start)");
    // start == length -> empty (boundary)
    check(substr_eq(5, 2, ""), "D5 substr: start=5 -> empty (start == length boundary)");
    // start > length -> empty
    check(substr_eq(6, 2, ""), "D5 substr: start=6 -> empty (start > length)");
    // start=0, len=-1 -> full string (negative len means to the end)
    check(substr_eq(0, -1, "hello"), "D5 substr: start=0 len=-1 -> full string");
    // start=2, len=100 -> clamps to end ("llo")
    check(substr_eq(2, 100, "llo"), "D5 substr: start=2 len=100 -> \"llo\" (len clamped to end)");
    // start=2, len=2 -> normal ("ll")
    check(substr_eq(2, 2, "ll"), "D5 substr: start=2 len=2 -> \"ll\" (normal)");
    // start=2, len=-1 -> from start to end ("llo")
    check(substr_eq(2, -1, "llo"), "D5 substr: start=2 len=-1 -> \"llo\" (negative len, to end)");
    // start=0, len=5 -> exact full string
    check(substr_eq(0, 5, "hello"), "D5 substr: start=0 len=5 -> \"hello\" (exact)");
    // start=0, len=0 -> empty (zero len)
    check(substr_eq(0, 0, ""), "D5 substr: start=0 len=0 -> empty (zero len)");
    // start=4, len=1 -> last char ("o")
    check(substr_eq(4, 1, "o"), "D5 substr: start=4 len=1 -> \"o\" (last char)");
    // start=4, len=-1 -> from last index to end ("o")
    check(substr_eq(4, -1, "o"), "D5 substr: start=4 len=-1 -> \"o\" (to end from last index)");

    // substr on an invalid handle -> empty (str_new("") via the !x guard)
    {
        int64_t r = p_substr(0, 0, 2);
        const std::string* p = ember::ext_string::slot(r);
        check(p && p->empty(), "D5 substr: invalid handle -> empty string (not crash, not 0-handle garbage)");
    }

    // ========================================================================
    // cleanup
    // ========================================================================
    ember::ext_array::reset();
    ember::ext_string::reset();

    std::printf("\nember array+string bounds test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
