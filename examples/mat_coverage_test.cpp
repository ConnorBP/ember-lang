// Direct coverage for mat4 construction, mutation, multiplication and equality.
#include "ext_mat.hpp"
#include "ast.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

using namespace ember;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); return 1; } } while (0)

template <typename Fn>
static Fn native(const std::unordered_map<std::string, NativeSig>& n, const char* name) {
    const auto it = n.find(name); if (it == n.end() || !it->second.fn_ptr) std::abort();
    return reinterpret_cast<Fn>(it->second.fn_ptr);
}

int main() {
    std::unordered_map<std::string, NativeSig> n;
    OpOverloadTable overloads;
    ext_mat::register_natives(n);
    ext_mat::register_overloads(overloads);

    const auto make = native<int64_t(*)()>(n, "mat4_new");
    const auto identity = native<int64_t(*)()>(n, "mat4_identity");
    const auto get = native<float(*)(int64_t, int64_t, int64_t)>(n, "mat4_get");
    const auto set = native<void(*)(int64_t, int64_t, int64_t, float)>(n, "mat4_set");
    const auto mul = reinterpret_cast<int64_t(*)(int64_t, int64_t)>(
        overloads.find("mat4", int(BinExpr::Op::Mul))->fn_ptr);
    const auto eq = reinterpret_cast<int64_t(*)(int64_t, int64_t)>(
        overloads.find("mat4", int(BinExpr::Op::Eq))->fn_ptr);

    const int64_t a = make();
    const int64_t id = identity();
    CHECK(a > 0 && id > 0);
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
        CHECK(get(id, r, c) == (r == c ? 1.0f : 0.0f));

    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
        set(a, r, c, float(r * 4 + c + 1));
    CHECK(get(a, 2, 3) == 12.0f);
    CHECK(get(0, 0, 0) == 0.0f && get(a, -1, 0) == 0.0f && get(a, 0, 4) == 0.0f);
    set(0, 0, 0, 5.0f); set(a, -1, 0, 5.0f); set(a, 0, 4, 5.0f);

    const int64_t aid = mul(a, id);
    CHECK(aid > 0 && eq(a, aid) == 1);
    CHECK(eq(a, id) == 0 && eq(0, id) == 0 && mul(0, id) == 0);

    const int64_t b = make();
    set(b, 0, 0, 2.0f); set(b, 1, 1, 3.0f); set(b, 2, 2, 4.0f); set(b, 3, 3, 5.0f);
    const int64_t product = mul(a, b);
    CHECK(product > 0);
    for (int r = 0; r < 4; ++r) {
        CHECK(get(product, r, 0) == get(a, r, 0) * 2.0f);
        CHECK(get(product, r, 1) == get(a, r, 1) * 3.0f);
        CHECK(get(product, r, 2) == get(a, r, 2) * 4.0f);
        CHECK(get(product, r, 3) == get(a, r, 3) * 5.0f);
    }

    ext_mat::reset();
    CHECK(get(a, 0, 0) == 0.0f);
    ext_mat::reset();
    std::puts("mat coverage: PASS");
    return 0;
}
