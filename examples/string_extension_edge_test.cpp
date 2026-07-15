// Focused edge coverage for the string host-store extension.
#include "ext_string.hpp"
#include "sema.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {
int failures = 0;

void check(bool condition, const char* message) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

const std::string* text(int64_t handle) {
    return ember::ext_string::slot(handle);
}

bool is_text(int64_t handle, const std::string& expected) {
    const std::string* value = text(handle);
    return value && *value == expected;
}
} // namespace

int main() {
    using namespace ember;
    ext_string::reset();

    std::unordered_map<std::string, NativeSig> natives;
    ext_string::register_natives(natives);
    OpOverloadTable overloads;
    ext_string::register_overloads(overloads);

    auto get = [&](const char* name) -> void* {
        auto it = natives.find(name);
        check(it != natives.end() && it->second.fn_ptr, name);
        return it == natives.end() ? nullptr : it->second.fn_ptr;
    };

    using F0 = int64_t(*)();
    using F1 = int64_t(*)(int64_t);
    using F2 = int64_t(*)(int64_t, int64_t);
    using F3 = int64_t(*)(int64_t, int64_t, int64_t);
    using F4 = int64_t(*)(int64_t, int64_t, int64_t, int64_t);
    using F5 = int64_t(*)(int64_t, int64_t, int64_t, int64_t, int64_t);
    using FSlice = int64_t(*)(uint8_t*, int64_t);
    using FF32 = int64_t(*)(float);
    using FF64 = int64_t(*)(double);

    auto string_new = reinterpret_cast<F0>(get("string_new"));
    auto from_slice = reinterpret_cast<FSlice>(get("string_from_slice"));
    auto length = reinterpret_cast<F1>(get("string_length"));
    auto from_i64 = reinterpret_cast<F1>(get("string_from_i64"));
    auto from_f32 = reinterpret_cast<FF32>(get("string_from_f32"));
    auto from_f64 = reinterpret_cast<FF64>(get("string_from_f64"));
    auto from_bool = reinterpret_cast<F1>(get("string_from_bool"));
    auto identity = reinterpret_cast<F1>(get("string_identity"));
    auto find = reinterpret_cast<F2>(get("string_find"));
    auto substr = reinterpret_cast<F3>(get("string_substr"));
    auto fmt1 = reinterpret_cast<F2>(get("fmt1"));
    auto fmt2 = reinterpret_cast<F3>(get("fmt2"));
    auto fmt3 = reinterpret_cast<F4>(get("fmt3"));
    auto fmt4 = reinterpret_cast<F5>(get("fmt4"));
    if (failures) return 1;

    check(is_text(string_new(), ""), "string_new creates an empty string");
    check(is_text(from_slice(nullptr, 0), ""), "empty null slice is accepted");
    check(from_slice(nullptr, 1) == 0, "non-empty null slice is rejected");
    check(from_slice(nullptr, -1) == 0, "negative slice length is rejected");
    uint8_t bytes[] = {'a', 'b', 'c', 0, 'd'};
    int64_t binary = from_slice(bytes, 5);
    check(text(binary) && text(binary)->size() == 5 && (*text(binary))[3] == 0,
          "from_slice preserves embedded null bytes");
    check(length(binary) == 5 && length(0) == 0, "length handles valid and invalid handles");

    check(is_text(from_i64(-9223372036854775807LL), "-9223372036854775807"),
          "i64 conversion handles negative values");
    check(is_text(from_f32(1.5f), "1.5"), "f32 conversion");
    check(is_text(from_f64(2.25), "2.25"), "f64 conversion");
    check(is_text(from_bool(0), "false") && is_text(from_bool(9), "true"),
          "bool conversion handles both values");
    check(identity(binary) == binary, "string identity preserves the handle");

    const OpOverload* add = overloads.find("string", int(BinExpr::Op::Add));
    const OpOverload* eq = overloads.find("string", int(BinExpr::Op::Eq));
    check(add && eq, "string add and equality overloads are registered");
    auto concat = reinterpret_cast<F2>(add ? add->fn_ptr : nullptr);
    auto equals = reinterpret_cast<F2>(eq ? eq->fn_ptr : nullptr);
    int64_t hello = ext_string::alloc("hello");
    int64_t world = ext_string::alloc(" world");
    int64_t joined = concat(hello, world);
    check(is_text(joined, "hello world"), "concat joins valid strings");
    check(concat(0, world) == 0, "concat rejects an invalid handle");
    check(equals(joined, ext_string::alloc("hello world")) == 1 &&
          equals(joined, hello) == 0 && equals(0, 0) == 0,
          "equality compares contents and rejects invalid handles");

    check(find(joined, ext_string::alloc("world")) == 6, "find returns the first match");
    check(find(joined, ext_string::alloc("missing")) == -1, "find reports no match");
    check(find(0, hello) == -1 && find(hello, 0) == -1, "find rejects invalid handles");
    check(is_text(substr(0, 0, 2), ""), "substr invalid handle returns empty string");
    check(is_text(substr(joined, 6, 99), "world"), "substr clamps length");

    int64_t word = ext_string::alloc("ok");
    int64_t format = ext_string::alloc("d=%d x=%x X=%X c=%c");
    check(is_text(fmt4(format, -7, 255, 255, 'A'), "d=-7 x=ff X=FF c=A"),
          "fmt4 covers decimal, lower/upper hex, and char");

    double pi = 3.5;
    int64_t pi_bits = 0;
    std::memcpy(&pi_bits, &pi, sizeof(pi));
    check(is_text(fmt2(ext_string::alloc("%s %f"), word, pi_bits), "ok 3.5"),
          "fmt2 covers string and floating formatting");
    check(is_text(fmt1(ext_string::alloc("%%/%q/%d"), 12), "%/%q/12"),
          "unknown format does not consume its argument");
    check(is_text(fmt1(ext_string::alloc("%d %x"), 10), "10 %x"),
          "missing format argument is preserved literally");
    check(is_text(fmt1(ext_string::alloc("tail%"), 1), "tail%"),
          "trailing percent is preserved");
    check(is_text(fmt3(ext_string::alloc("%d,%d,%d"), 1, 2, 3), "1,2,3"),
          "fmt3 consumes three arguments");
    check(fmt1(0, 1) == 0 && fmt2(0, 1, 2) == 0 &&
          fmt3(0, 1, 2, 3) == 0 && fmt4(0, 1, 2, 3, 4) == 0,
          "formatters reject invalid format handles");

    ext_string::reset();
    check(ext_string::slot(joined) == nullptr, "reset invalidates old handles");

    std::printf("string extension edge test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
