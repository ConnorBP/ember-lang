// ext_string.cpp - ember extension: mutable string host-store type + overloads.
// Relocated from prism/src/prism/prism_script_host.cpp (string block) during
// the restructure Section 6 audit. The host-sink-coupled `print_string` native
// STAYED in prism (it routes through the host's print sink); this extension
// exposes slot() so that native can read a string handle's content. The
// type, handle ABI, from_* converters, concat/eq overloads are unchanged.
#include "ext_string.hpp"
#include "ast.hpp"
#include "binding_builder.hpp"  // BindingBuilder: deduped I/H/add registration
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ember;  // bind_handle, BindingBuilder, type_* singletons

namespace ember::ext_string {

// --- string host store (opaque i64 handle; host owns a mutable std::string).
static std::vector<std::string> g_strings;
// Serializes all g_strings store operations (push_back in str_new, str_slot
// lookups) so concurrent context_t's calling string natives cannot race on
// vector reallocation (Sec-5). Mirrors the g_store_mutex pattern in ext_sync.cpp
// and g_mutex in ext_lifecycle.cpp.
static std::mutex g_store_mutex;
// Match the other host containers: one string may own at most 1 GiB.
static constexpr size_t MAX_STRING_BYTES = size_t(1) << 30;
static int64_t str_new(std::string s) noexcept {
    if (s.size() > MAX_STRING_BYTES) return 0;
    try {
        g_strings.push_back(std::move(s));
        return int64_t(g_strings.size());
    } catch (const std::bad_alloc&) {
        return 0;
    } catch (const std::length_error&) {
        return 0;
    }
}
static std::string* str_slot(int64_t h) { if (h<1 || h>int64_t(g_strings.size())) return nullptr; return &g_strings[size_t(h-1)]; }
extern "C" {
    static int64_t n_string_new() { std::lock_guard<std::mutex> lock(g_store_mutex); return str_new(std::string()); }
    static int64_t n_string_from_slice(uint8_t* p, int64_t len) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        if (len < 0 || uint64_t(len) > uint64_t(MAX_STRING_BYTES) || (!p && len != 0)) return 0;
        try {
            if (len == 0) return str_new(std::string());
            return str_new(std::string(reinterpret_cast<const char*>(p), size_t(len)));
        }
        catch (const std::bad_alloc&) { return 0; }
        catch (const std::length_error&) { return 0; }
    }
    static int64_t n_string_length(int64_t h) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s = str_slot(h); return s ? int64_t(s->size()) : 0; }
    static int64_t n_string_char_at(int64_t h, int64_t i) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = str_slot(h);
        if (!s || i < 0 || size_t(i) >= s->size()) return 0;
        return int64_t(uint8_t((*s)[size_t(i)]));
    }
    static int64_t n_string_from_i64(int64_t v) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        try { return str_new(std::to_string(v)); }
        catch (const std::bad_alloc&) { return 0; }
        catch (const std::length_error&) { return 0; }
    }
    static int64_t n_string_from_f32(float v) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        char buf[64]; std::snprintf(buf, sizeof buf, "%g", v);
        try { return str_new(std::string(buf)); }
        catch (const std::bad_alloc&) { return 0; }
        catch (const std::length_error&) { return 0; }
    }
    // f-string interpolation converters (Item D). ember's codegen has no
    // real f64 runtime support at all today (no movsd/double-precision
    // emission anywhere) - registering this is safe/forward-compatible, but
    // f64 interpolation isn't exercised by the test suite until that
    // separate gap is closed.
    static int64_t n_string_from_f64(double v) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        char buf[64]; std::snprintf(buf, sizeof buf, "%g", v);
        try { return str_new(std::string(buf)); }
        catch (const std::bad_alloc&) { return 0; }
        catch (const std::length_error&) { return 0; }
    }
    static int64_t n_string_from_bool(int64_t v) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        try { return str_new(v != 0 ? std::string("true") : std::string("false")); }
        catch (const std::bad_alloc&) { return 0; }
        catch (const std::length_error&) { return 0; }
    }
    // interpolating an already-`string`-typed segment needs no conversion -
    // just pass the handle through, so every __fstring_to_string sentinel
    // always resolves to SOME native call (uniform, no codegen special case).
    static int64_t n_string_identity(int64_t h) { return h; }
    static int64_t n_string_concat(int64_t a, int64_t b) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* x = str_slot(a); auto* y = str_slot(b);
        if (!x || !y) return 0;
        const size_t xn = x->size(), yn = y->size();
        if (xn > MAX_STRING_BYTES || yn > MAX_STRING_BYTES - xn) return 0;
        try {
            std::string r; r.reserve(xn + yn);
            r += *x;
            r += *y;
            return str_new(std::move(r));
        } catch (const std::bad_alloc&) { return 0; }
        catch (const std::length_error&) { return 0; }
    }
    static int64_t n_string_eq(int64_t a, int64_t b) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* x = str_slot(a); auto* y = str_slot(b);
        return (x && y && *x == *y) ? 1 : 0;
    }
    // string_find: returns the index of the first occurrence of substring b in
    // string a, or -1 if not found.
    static int64_t n_string_find(int64_t a, int64_t b) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* x = str_slot(a); auto* y = str_slot(b);
        if (!x || !y) return -1;
        size_t pos = x->find(*y);
        return pos == std::string::npos ? int64_t(-1) : int64_t(pos);
    }
    // string_substr: returns a new string that is the substring of a starting
    // at index start with length len (clamped to the string's bounds).
    static int64_t n_string_substr(int64_t a, int64_t start, int64_t len) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* x = str_slot(a);
        if (!x || start < 0) return str_new("");
        size_t s = size_t(start);
        if (s >= x->size()) return str_new("");
        size_t actual_len = (len < 0) ? (x->size() - s) : std::min(size_t(len), x->size() - s);
        try {
            return str_new(x->substr(s, actual_len));
        } catch (const std::bad_alloc&) { return 0; }
        catch (const std::length_error&) { return 0; }
    }
}

const std::string* slot(int64_t handle) {
    std::lock_guard<std::mutex> lock(g_store_mutex);
    return str_slot(handle);
}

int64_t alloc(std::string s) {
    std::lock_guard<std::mutex> lock(g_store_mutex);
    return str_new(std::move(s));
}

// Registered surface is byte-identical to the old I/H/add lambda form
// (ext_registration_test asserts string_new -> struct "string" 0 params;
//  string_from_slice -> struct "string" 1 param; string_length -> i64;
//  string_from_i64 / string_identity -> struct "string").
//
// U8S: BindingBuilder has no slice helper (it covers prim + opaque handle,
// the two forms these six extensions use everywhere else), so the one slice
// parameter (string_from_slice's u8[]) is built inline with make_slice - the
// exact same expression the old U8S lambda used. Not a BindingBuilder spec
// change; flagged in the refactor report as the one local helper retained.
void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    auto U8S = [](){ return make_slice(std::make_shared<Type>(make_prim(Prim::U8))); };
    BindingBuilder b;
    b.add("string_new",        bind_handle("string"), {},                   (void*)&n_string_new);
    b.add("string_from_slice", bind_handle("string"), {U8S()},             (void*)&n_string_from_slice);
    b.add("string_length",     type_i64(), {bind_handle("string")},         (void*)&n_string_length);
    b.add("string_char_at",    type_i64(), {bind_handle("string"),type_i64()}, (void*)&n_string_char_at);
    b.add("string_from_i64",   bind_handle("string"), {type_i64()},        (void*)&n_string_from_i64);
    b.add("string_from_f32",   bind_handle("string"), {type_f32()},        (void*)&n_string_from_f32);
    b.add("string_from_f64",   bind_handle("string"), {type_f64()},        (void*)&n_string_from_f64);
    b.add("string_from_bool",  bind_handle("string"), {type_bool()},       (void*)&n_string_from_bool);
    b.add("string_identity",   bind_handle("string"), {bind_handle("string")}, (void*)&n_string_identity);
    b.add("string_find",      type_i64(), {bind_handle("string"),bind_handle("string")}, (void*)&n_string_find);
    b.add("string_substr",    bind_handle("string"), {bind_handle("string"),type_i64(),type_i64()}, (void*)&n_string_substr);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
    // NOTE: print_string stays in the host (it routes through the host
    // print sink). The host registers it itself, calling ext_string::slot()
    // to read the handle's content.
}

// Overload (type,op) entries preserved exactly. string: `+` concatenates,
// `==` compares content (not handle identity).
void register_overloads(OpOverloadTable& overloads) {
    BindingBuilder b;
    b.add_overload("string", int(BinExpr::Op::Add), bind_handle("string"), (void*)&n_string_concat);
    b.add_overload("string", int(BinExpr::Op::Eq),  type_bool(),           (void*)&n_string_eq);
    NativeTable t = b.build();
    for (auto& kv : t.overloads.entries) overloads.entries[kv.first] = std::move(kv.second);
}

void reset() {
    std::lock_guard<std::mutex> lock(g_store_mutex);
    g_strings.clear();
}

} // namespace ember::ext_string
