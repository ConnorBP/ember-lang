// Direct coverage for every native exported by extensions/math.
#include "ext_math.hpp"
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
static Fn native(const std::unordered_map<std::string, NativeSig>& natives, const char* name) {
    const auto it = natives.find(name);
    if (it == natives.end() || !it->second.fn_ptr) std::abort();
    return reinterpret_cast<Fn>(it->second.fn_ptr);
}

int main() {
    std::unordered_map<std::string, NativeSig> n;
    ext_math::register_natives(n);
    const auto close = [](double a, double b) { return std::fabs(a - b) < 1e-5; };

    CHECK(close(native<float(*)(float)>(n, "sqrt")(9.0f), 3.0));
    CHECK(close(native<float(*)(float)>(n, "sin")(0.0f), 0.0));
    CHECK(close(native<float(*)(float)>(n, "cos")(0.0f), 1.0));
    CHECK(close(native<float(*)(float)>(n, "tan")(0.0f), 0.0));
    CHECK(close(native<float(*)(float)>(n, "atan")(1.0f), std::atan(1.0)));
    CHECK(close(native<float(*)(float, float)>(n, "atan2")(1.0f, 1.0f), std::atan2(1.0, 1.0)));
    CHECK(close(native<float(*)(float)>(n, "exp")(1.0f), std::exp(1.0)));
    CHECK(close(native<float(*)(float)>(n, "log")(std::exp(1.0f)), 1.0));
    CHECK(close(native<float(*)(float)>(n, "floor")(2.75f), 2.0));
    CHECK(close(native<float(*)(float)>(n, "ceil")(2.25f), 3.0));
    CHECK(close(native<float(*)(float)>(n, "abs")(-2.5f), 2.5));
    CHECK(close(native<float(*)(float)>(n, "round")(2.6f), 3.0));

    CHECK(close(native<double(*)(double)>(n, "sqrt_f64")(16.0), 4.0));
    CHECK(close(native<double(*)(double)>(n, "sin_f64")(0.0), 0.0));
    CHECK(close(native<double(*)(double)>(n, "cos_f64")(0.0), 1.0));
    CHECK(close(native<double(*)(double)>(n, "tan_f64")(0.0), 0.0));
    CHECK(close(native<double(*)(double)>(n, "floor_f64")(2.9), 2.0));
    CHECK(close(native<double(*)(double)>(n, "ceil_f64")(2.1), 3.0));
    CHECK(close(native<double(*)(double)>(n, "abs_f64")(-7.25), 7.25));
    CHECK(close(native<double(*)(double, double)>(n, "pow_f64")(2.0, 5.0), 32.0));
    CHECK(native<int64_t(*)(int64_t)>(n, "abs_i64")(-9) == 9);
    CHECK(native<int64_t(*)(int64_t)>(n, "abs_i64")(9) == 9);

    CHECK(close(native<double(*)(double)>(n, "atan_f64")(1.0), std::atan(1.0)));
    CHECK(close(native<double(*)(double, double)>(n, "atan2_f64")(1.0, -1.0), std::atan2(1.0, -1.0)));
    CHECK(close(native<double(*)(double)>(n, "exp_f64")(1.0), std::exp(1.0)));
    CHECK(close(native<double(*)(double)>(n, "log_f64")(std::exp(2.0)), 2.0));
    CHECK(close(native<double(*)(double)>(n, "log2_f64")(8.0), 3.0));
    CHECK(close(native<double(*)(double)>(n, "log10_f64")(1000.0), 3.0));
    CHECK(close(native<double(*)(double, double)>(n, "fmod_f64")(7.0, 4.0), 3.0));
    CHECK(close(native<double(*)(double)>(n, "round_f64")(2.6), 3.0));
    CHECK(close(native<double(*)(double)>(n, "trunc_f64")(-2.6), -2.0));

    const auto minf = native<double(*)(double, double)>(n, "min_f64");
    const auto maxf = native<double(*)(double, double)>(n, "max_f64");
    const auto clampf = native<double(*)(double, double, double)>(n, "clamp_f64");
    CHECK(minf(2.0, 3.0) == 2.0 && minf(4.0, 3.0) == 3.0);
    CHECK(maxf(2.0, 3.0) == 3.0 && maxf(4.0, 3.0) == 4.0);
    CHECK(clampf(-1.0, 0.0, 10.0) == 0.0);
    CHECK(clampf(11.0, 0.0, 10.0) == 10.0);
    CHECK(clampf(5.0, 0.0, 10.0) == 5.0);

    const auto mini = native<int64_t(*)(int64_t, int64_t)>(n, "min_i64");
    const auto maxi = native<int64_t(*)(int64_t, int64_t)>(n, "max_i64");
    const auto clampi = native<int64_t(*)(int64_t, int64_t, int64_t)>(n, "clamp_i64");
    CHECK(mini(2, 3) == 2 && mini(4, 3) == 3);
    CHECK(maxi(2, 3) == 3 && maxi(4, 3) == 4);
    CHECK(clampi(-1, 0, 10) == 0 && clampi(11, 0, 10) == 10 && clampi(5, 0, 10) == 5);

    std::puts("math coverage: PASS");
    return 0;
}
