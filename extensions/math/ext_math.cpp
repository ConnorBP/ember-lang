// ext_math.cpp - ember extension: math natives (sqrt/sin/cos/tan, f32).
// Relocated verbatim from prism/src/prism/prism_script_host.cpp (the
// sqrt/sin/cos/tan definitions) during the restructure Section 6 audit. Stateless
// pure functions; no host store, no reset(). Only namespace + registration
// entry point changed.
#include "ext_math.hpp"
#include "ast.hpp"
#include "binding_builder.hpp"  // BindingBuilder: deduped I/H/add registration
#include <cmath>

using namespace ember;  // bind_handle, BindingBuilder, type_* singletons

namespace ember::ext_math {

extern "C" {
    static float n_sqrt(float v) { return std::sqrt(v); }
    static float n_sin(float v) { return std::sin(v); }
    static float n_cos(float v) { return std::cos(v); }
    static float n_tan(float v) { return std::tan(v); }
    // f64 variants (deferred Tier 0 — broader f32/f64 math).
    static double n_sqrt_f64(double v) { return std::sqrt(v); }
    static double n_sin_f64(double v) { return std::sin(v); }
    static double n_cos_f64(double v) { return std::cos(v); }
    static double n_tan_f64(double v) { return std::tan(v); }
    static double n_floor_f64(double v) { return std::floor(v); }
    static double n_ceil_f64(double v) { return std::ceil(v); }
    static double n_abs_f64(double v) { return std::fabs(v); }
    static double n_pow_f64(double b, double e) { return std::pow(b, e); }
    static int64_t n_abs_i64(int64_t v) { return v < 0 ? -v : v; }

    // Broader math natives (2026-07-11)
    static double n_atan_f64(double v) { return std::atan(v); }
    static double n_atan2_f64(double y, double x) { return std::atan2(y, x); }
    static double n_exp_f64(double v) { return std::exp(v); }
    static double n_log_f64(double v) { return std::log(v); }
    static double n_log2_f64(double v) { return std::log2(v); }
    static double n_log10_f64(double v) { return std::log10(v); }
    static double n_fmod_f64(double a, double b) { return std::fmod(a, b); }
    static double n_round_f64(double v) { return std::round(v); }
    static double n_trunc_f64(double v) { return std::trunc(v); }
    static double n_min_f64(double a, double b) { return a < b ? a : b; }
    static double n_max_f64(double a, double b) { return a > b ? a : b; }
    static double n_clamp_f64(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static int64_t n_min_i64(int64_t a, int64_t b) { return a < b ? a : b; }
    static int64_t n_max_i64(int64_t a, int64_t b) { return a > b ? a : b; }
    static int64_t n_clamp_i64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float n_atan(float v) { return std::atan(v); }
    static float n_atan2(float y, float x) { return std::atan2(y, x); }
    static float n_exp(float v) { return std::exp(v); }
    static float n_log(float v) { return std::log(v); }
    static float n_floor(float v) { return std::floor(v); }
    static float n_ceil(float v) { return std::ceil(v); }
    static float n_abs(float v) { return std::fabs(v); }
    static float n_round(float v) { return std::roundf(v); }
}

// Registered surface is byte-identical to the old I/H/add lambda form
// (ext_registration_test asserts sqrt/sin/cos/tan -> f32, 1 f32 param;
//  ext_runtime_test JIT-calls sqrt through the full pipeline).
void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    b.add("sqrt", type_f32(), {type_f32()}, (void*)&n_sqrt);
    b.add("sin",  type_f32(), {type_f32()}, (void*)&n_sin);
    b.add("cos",  type_f32(), {type_f32()}, (void*)&n_cos);
    b.add("tan",  type_f32(), {type_f32()}, (void*)&n_tan);
    // f64 math (deferred Tier 0 — broader f32/f64 math).
    b.add("sqrt_f64", type_f64(), {type_f64()}, (void*)&n_sqrt_f64);
    b.add("sin_f64",  type_f64(), {type_f64()}, (void*)&n_sin_f64);
    b.add("cos_f64",  type_f64(), {type_f64()}, (void*)&n_cos_f64);
    b.add("tan_f64",  type_f64(), {type_f64()}, (void*)&n_tan_f64);
    b.add("floor_f64",type_f64(), {type_f64()}, (void*)&n_floor_f64);
    b.add("ceil_f64", type_f64(), {type_f64()}, (void*)&n_ceil_f64);
    b.add("abs_f64",  type_f64(), {type_f64()}, (void*)&n_abs_f64);
    b.add("pow_f64",  type_f64(), {type_f64(),type_f64()}, (void*)&n_pow_f64);
    b.add("abs_i64",  type_i64(), {type_i64()}, (void*)&n_abs_i64);
    // Broader math natives (2026-07-11)
    b.add("atan_f64", type_f64(), {type_f64()}, (void*)&n_atan_f64);
    b.add("atan2_f64",type_f64(), {type_f64(),type_f64()}, (void*)&n_atan2_f64);
    b.add("exp_f64",  type_f64(), {type_f64()}, (void*)&n_exp_f64);
    b.add("log_f64",  type_f64(), {type_f64()}, (void*)&n_log_f64);
    b.add("log2_f64", type_f64(), {type_f64()}, (void*)&n_log2_f64);
    b.add("log10_f64",type_f64(), {type_f64()}, (void*)&n_log10_f64);
    b.add("fmod_f64", type_f64(), {type_f64(),type_f64()}, (void*)&n_fmod_f64);
    b.add("round_f64",type_f64(), {type_f64()}, (void*)&n_round_f64);
    b.add("trunc_f64",type_f64(), {type_f64()}, (void*)&n_trunc_f64);
    b.add("min_f64",  type_f64(), {type_f64(),type_f64()}, (void*)&n_min_f64);
    b.add("max_f64",  type_f64(), {type_f64(),type_f64()}, (void*)&n_max_f64);
    b.add("clamp_f64",type_f64(), {type_f64(),type_f64(),type_f64()}, (void*)&n_clamp_f64);
    b.add("min_i64",  type_i64(), {type_i64(),type_i64()}, (void*)&n_min_i64);
    b.add("max_i64",  type_i64(), {type_i64(),type_i64()}, (void*)&n_max_i64);
    b.add("clamp_i64",type_i64(), {type_i64(),type_i64(),type_i64()}, (void*)&n_clamp_i64);
    b.add("atan",     type_f32(), {type_f32()}, (void*)&n_atan);
    b.add("atan2",    type_f32(), {type_f32(),type_f32()}, (void*)&n_atan2);
    b.add("exp",      type_f32(), {type_f32()}, (void*)&n_exp);
    b.add("log",      type_f32(), {type_f32()}, (void*)&n_log);
    b.add("floor",    type_f32(), {type_f32()}, (void*)&n_floor);
    b.add("ceil",     type_f32(), {type_f32()}, (void*)&n_ceil);
    b.add("abs",      type_f32(), {type_f32()}, (void*)&n_abs);
    b.add("round",    type_f32(), {type_f32()}, (void*)&n_round);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

} // namespace ember::ext_math
