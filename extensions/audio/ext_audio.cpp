// ext_audio.cpp - allocation-free access to host-owned DSP buffers.
#include "ext_audio.hpp"
#include "ast.hpp"
#include "binding_builder.hpp"

#include <cstdint>

namespace ember::ext_audio {
namespace {

float n_load_f32(int64_t ptr, int64_t index) {
    return reinterpret_cast<float*>(ptr)[index];
}

void n_store_f32(int64_t ptr, int64_t index, float value) {
    reinterpret_cast<float*>(ptr)[index] = value;
}

double n_load_f64(int64_t ptr, int64_t index) {
    return reinterpret_cast<double*>(ptr)[index];
}

void n_store_f64(int64_t ptr, int64_t index, double value) {
    reinterpret_cast<double*>(ptr)[index] = value;
}

int32_t n_load_i32(int64_t ptr, int64_t index) {
    return reinterpret_cast<int32_t*>(ptr)[index];
}

void n_store_i32(int64_t ptr, int64_t index, int32_t value) {
    reinterpret_cast<int32_t*>(ptr)[index] = value;
}

} // namespace

void register_natives(std::unordered_map<std::string, NativeSig>& natives) {
    BindingBuilder builder;
    const Type i32 = make_prim(Prim::I32);
    builder.add("load_f32",  type_f32(), {type_i64(), type_i64()},
                reinterpret_cast<void*>(&n_load_f32), PERM_FFI);
    builder.add("store_f32", type_void(), {type_i64(), type_i64(), type_f32()},
                reinterpret_cast<void*>(&n_store_f32), PERM_FFI);
    builder.add("load_f64",  type_f64(), {type_i64(), type_i64()},
                reinterpret_cast<void*>(&n_load_f64), PERM_FFI);
    builder.add("store_f64", type_void(), {type_i64(), type_i64(), type_f64()},
                reinterpret_cast<void*>(&n_store_f64), PERM_FFI);
    builder.add("load_i32",  i32, {type_i64(), type_i64()},
                reinterpret_cast<void*>(&n_load_i32), PERM_FFI);
    builder.add("store_i32", type_void(), {type_i64(), type_i64(), i32},
                reinterpret_cast<void*>(&n_store_i32), PERM_FFI);

    NativeTable table = builder.build();
    for (auto& item : table.natives) {
        natives[item.first] = std::move(item.second);
    }
}

} // namespace ember::ext_audio
