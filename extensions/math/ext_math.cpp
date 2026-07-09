// ext_math.cpp - ember extension: math natives (sqrt/sin/cos/tan, f32).
// Relocated verbatim from prism/src/prism/prism_script_host.cpp (the
// sqrt/sin/cos/tan definitions) during the restructure Section 6 audit. Stateless
// pure functions; no host store, no reset(). Only namespace + registration
// entry point changed.
#include "ext_math.hpp"
#include "ast.hpp"
#include <cmath>

namespace ember::ext_math {

extern "C" {
    static float n_sqrt(float v) { return std::sqrtf(v); }
    static float n_sin(float v) { return std::sinf(v); }
    static float n_cos(float v) { return std::cosf(v); }
    static float n_tan(float v) { return std::tanf(v); }
}

void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    auto I = [](Prim p){ return Type(make_prim(p)); };
    auto add = [&](const char* n, void* fn, Type r, std::vector<Type> ps) {
        m[n] = NativeSig{n, fn, std::move(r), std::move(ps), 0};
    };
    add("sqrt", (void*)&n_sqrt, I(Prim::F32), {I(Prim::F32)});
    add("sin", (void*)&n_sin, I(Prim::F32), {I(Prim::F32)});
    add("cos", (void*)&n_cos, I(Prim::F32), {I(Prim::F32)});
    add("tan", (void*)&n_tan, I(Prim::F32), {I(Prim::F32)});
}

} // namespace ember::ext_math
