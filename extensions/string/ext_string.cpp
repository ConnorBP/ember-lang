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
#include <vector>

using namespace ember;  // bind_handle, BindingBuilder, type_* singletons

namespace ember::ext_string {

// --- string host store (opaque i64 handle; host owns a mutable std::string).
static std::vector<std::string> g_strings;
static int64_t str_new(std::string s) { g_strings.push_back(std::move(s)); return int64_t(g_strings.size()); }
static std::string* str_slot(int64_t h) { if (h<1 || h>int64_t(g_strings.size())) return nullptr; return &g_strings[size_t(h-1)]; }
extern "C" {
    static int64_t n_string_new() { return str_new(std::string()); }
    static int64_t n_string_from_slice(uint8_t* p, int64_t len) {
        return str_new(std::string(reinterpret_cast<const char*>(p), size_t(len > 0 ? len : 0)));
    }
    static int64_t n_string_length(int64_t h) { auto* s = str_slot(h); return s ? int64_t(s->size()) : 0; }
    static int64_t n_string_char_at(int64_t h, int64_t i) {
        auto* s = str_slot(h);
        if (!s || i < 0 || size_t(i) >= s->size()) return 0;
        return int64_t(uint8_t((*s)[size_t(i)]));
    }
    static int64_t n_string_from_i64(int64_t v) { return str_new(std::to_string(v)); }
    static int64_t n_string_from_f32(float v) { char buf[64]; std::snprintf(buf, sizeof buf, "%g", v); return str_new(std::string(buf)); }
    // f-string interpolation converters (Item D). ember's codegen has no
    // real f64 runtime support at all today (no movsd/double-precision
    // emission anywhere) - registering this is safe/forward-compatible, but
    // f64 interpolation isn't exercised by the test suite until that
    // separate gap is closed.
    static int64_t n_string_from_f64(double v) { char buf[64]; std::snprintf(buf, sizeof buf, "%g", v); return str_new(std::string(buf)); }
    static int64_t n_string_from_bool(int64_t v) { return str_new(v != 0 ? std::string("true") : std::string("false")); }
    // interpolating an already-`string`-typed segment needs no conversion -
    // just pass the handle through, so every __fstring_to_string sentinel
    // always resolves to SOME native call (uniform, no codegen special case).
    static int64_t n_string_identity(int64_t h) { return h; }
    static int64_t n_string_concat(int64_t a, int64_t b) {
        auto* x = str_slot(a); auto* y = str_slot(b);
        std::string r; if (x) r += *x; if (y) r += *y;
        return str_new(std::move(r));
    }
    static int64_t n_string_eq(int64_t a, int64_t b) {
        auto* x = str_slot(a); auto* y = str_slot(b);
        return (x && y && *x == *y) ? 1 : 0;
    }
}

const std::string* slot(int64_t handle) {
    return str_slot(handle);
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
    g_strings.clear();
}

} // namespace ember::ext_string
