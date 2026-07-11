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
    static float n_sqrt(float v) { return std::sqrtf(v); }
    static float n_sin(float v) { return std::sinf(v); }
    static float n_cos(float v) { return std::cosf(v); }
    static float n_tan(float v) { return std::tanf(v); }
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
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

} // namespace ember::ext_math
