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
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

} // namespace ember::ext_math
