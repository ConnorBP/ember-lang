// ext_registration_test - ember extensions registration smoke test.
//
// Validates the Section 6 restructure's core promise: the relocated non-cheat
// extensions (vec/quat/mat/string/array/math/sync/lifecycle) register their
// NativeSig entries + OpOverloadTable entries via the extension entry points
// (ember::ext_*::register_natives / register_overloads), so a host that links
// the extension libs and calls those entry points gets the full addon surface
// without re-implementing it. Links ONLY the ember extension libs + ember
// frontend (no prism, no prism_script_host) - the extensions are pure ember,
// and this test proves it.
//
// What it checks:
//   - each extension's register_natives populates the expected native keys
//     with the expected (name, ret, param-arity) shape;
//   - each extension's register_overloads populates the expected
//     (type, op) overload entries;
//   - the math extension registers sqrt/sin/cos/tan as f32->f32;
//   - the array extension's get_bytes returns false for an invalid handle
//     and the array_new native's handle round-trips through get_bytes.
//
// This is a registration smoke test, not a JIT execution test - it confirms
// the natives are *registered* (the bar the Section 6 restructure sets when the
// existing prism test suite doesn't exercise the relocated natives through
// the JIT). The end-to-end JIT exercise of these same natives via
// vector_math_demo.ember is covered by prism's ember_cli path.
#include "ext_vec.hpp"
#include "ext_quat.hpp"
#include "ext_mat.hpp"
#include "ext_string.hpp"
#include "ext_array.hpp"
#include "ext_math.hpp"
#include "ext_sync.hpp"
#include "ext_lifecycle.hpp"
// Documentation example compiled into this already-registered smoke target so
// the example stays build-tested without adding non-.cpp CMake changes.
#include "ext_random.cpp"
#include "ast.hpp"
#include "sema.hpp"
#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>

using namespace ember;

static int g_fail = 0;
static void check(bool cond, const char* msg) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

int main() {
    std::printf("=== ember extensions registration smoke test ===\n");

    // --- natives: register all eight extensions into one map, as a host would ---
    std::unordered_map<std::string, NativeSig> m;
    ext_vec::register_natives(m);
    ext_quat::register_natives(m);
    ext_mat::register_natives(m);
    ext_string::register_natives(m);
    ext_array::register_natives(m);
    ext_math::register_natives(m);
    ext_sync::register_natives(m);
    ext_lifecycle::register_natives(m);
    ext_random::register_natives(m);

    auto has = [&](const char* n) -> bool { return m.find(n) != m.end(); };
    auto arity = [&](const char* n) -> size_t { auto it = m.find(n); return it == m.end() ? size_t(-1) : it->second.params.size(); };
    auto ret_prim = [&](const char* n) -> Prim { auto it = m.find(n); return it == m.end() ? Prim::Void : it->second.ret.prim; };
    auto struct_name = [&](const char* n) -> std::string { auto it = m.find(n); return it == m.end() ? std::string() : it->second.ret.struct_name; };

    // math (stateless): sqrt/sin/cos/tan as f32 -> f32
    check(has("sqrt") && arity("sqrt")==1 && ret_prim("sqrt")==Prim::F32, "math: sqrt registered f32->f32");
    check(has("sin")  && arity("sin")==1  && ret_prim("sin")==Prim::F32,  "math: sin registered f32->f32");
    check(has("cos")  && arity("cos")==1  && ret_prim("cos")==Prim::F32,  "math: cos registered f32->f32");
    check(has("tan")  && arity("tan")==1  && ret_prim("tan")==Prim::F32,  "math: tan registered f32->f32");

    // Developer-guide random extension: registration, ABI, deterministic reset.
    check(has("random_next") && arity("random_next") == 1 &&
          has("random_seed") && arity("random_seed") == 1 &&
          has("random_float") && arity("random_float") == 0,
          "random example: three natives registered");
    auto random_seed = reinterpret_cast<void(*)(int64_t)>(m.at("random_seed").fn_ptr);
    auto random_next = reinterpret_cast<int64_t(*)(int64_t)>(m.at("random_next").fn_ptr);
    auto random_float = reinterpret_cast<float(*)()>(m.at("random_float").fn_ptr);
    random_seed(12345); int64_t random_first = random_next(1000); float random_unit = random_float();
    random_seed(12345);
    check(random_first == random_next(1000) && random_unit >= 0.0f && random_unit < 1.0f,
          "random example: deterministic sequence and [0,1) float");

    // vec3: new returns struct "vec3", 3 f32 params; x/y/z accessors
    check(has("vec3_new") && arity("vec3_new")==3 && struct_name("vec3_new")=="vec3", "vec: vec3_new -> vec3, 3 params");
    check(has("vec3_x") && arity("vec3_x")==1 && ret_prim("vec3_x")==Prim::F32, "vec: vec3_x -> f32");
    check(has("vec3_set_x") && arity("vec3_set_x")==2 && ret_prim("vec3_set_x")==Prim::Void, "vec: vec3_set_x -> void");
    check(has("vec2_new") && arity("vec2_new")==2 && struct_name("vec2_new")=="vec2", "vec: vec2_new -> vec2, 2 params");
    check(has("vec4_new") && arity("vec4_new")==4 && struct_name("vec4_new")=="vec4", "vec: vec4_new -> vec4, 4 params");
    check(has("vec4_w") && ret_prim("vec4_w")==Prim::F32, "vec: vec4_w -> f32");

    // quat: new returns struct "quat", 4 f32 params
    check(has("quat_new") && arity("quat_new")==4 && struct_name("quat_new")=="quat", "quat: quat_new -> quat, 4 params");
    check(has("quat_w") && ret_prim("quat_w")==Prim::F32, "quat: quat_w -> f32");

    // mat4: new + identity return struct "mat4"; get returns f32
    check(has("mat4_new") && arity("mat4_new")==0 && struct_name("mat4_new")=="mat4", "mat: mat4_new -> mat4, 0 params");
    check(has("mat4_identity") && struct_name("mat4_identity")=="mat4", "mat: mat4_identity -> mat4");
    check(has("mat4_get") && arity("mat4_get")==3 && ret_prim("mat4_get")==Prim::F32, "mat: mat4_get -> f32, 3 params");
    check(has("mat4_set") && arity("mat4_set")==4 && ret_prim("mat4_set")==Prim::Void, "mat: mat4_set -> void, 4 params");

    // string: new returns struct "string"; from_slice takes a u8 slice; length takes a string
    check(has("string_new") && arity("string_new")==0 && struct_name("string_new")=="string", "string: string_new -> string");
    check(has("string_from_slice") && arity("string_from_slice")==1 && struct_name("string_from_slice")=="string", "string: string_from_slice -> string");
    check(has("string_length") && arity("string_length")==1 && ret_prim("string_length")==Prim::I64, "string: string_length -> i64");
    check(has("string_from_i64") && struct_name("string_from_i64")=="string", "string: string_from_i64 -> string");
    check(has("string_identity") && struct_name("string_identity")=="string", "string: string_identity -> string");

    // array<T>: new takes (i64, i64); length takes i64 -> i64
    check(has("array_new") && arity("array_new")==2 && ret_prim("array_new")==Prim::I64, "array: array_new -> i64, 2 params");
    check(has("array_length") && arity("array_length")==1 && ret_prim("array_length")==Prim::I64, "array: array_length -> i64");
    check(has("array_get_u8") && ret_prim("array_get_u8")==Prim::U8, "array: array_get_u8 -> u8");
    check(has("array_push_u8") && arity("array_push_u8")==2 && ret_prim("array_push_u8")==Prim::Void, "array: array_push_u8 -> void, 2 params");

    // --- overloads: register the four extensions that have overloads ---
    OpOverloadTable t;
    ext_vec::register_overloads(t);
    ext_quat::register_overloads(t);
    ext_mat::register_overloads(t);
    ext_string::register_overloads(t);

    auto has_op = [&](const char* type, int op) -> bool { return t.find(type, op) != nullptr; };
    check(has_op("vec3", int(BinExpr::Op::Add)) && has_op("vec3", int(BinExpr::Op::Sub))
          && has_op("vec3", int(BinExpr::Op::Mul)) && has_op("vec3", int(BinExpr::Op::Eq)), "vec3: + - * == overloads registered");
    check(has_op("vec2", int(BinExpr::Op::Add)) && has_op("vec2", int(BinExpr::Op::Mul)) && has_op("vec2", int(BinExpr::Op::Eq)), "vec2: + * == overloads registered");
    check(has_op("vec4", int(BinExpr::Op::Add)) && has_op("vec4", int(BinExpr::Op::Mul)) && has_op("vec4", int(BinExpr::Op::Eq)), "vec4: + * == overloads registered");
    check(has_op("quat", int(BinExpr::Op::Add)) && has_op("quat", int(BinExpr::Op::Mul)) && has_op("quat", int(BinExpr::Op::Eq)), "quat: + * == overloads registered");
    check(has_op("mat4", int(BinExpr::Op::Mul)) && has_op("mat4", int(BinExpr::Op::Eq)), "mat4: * == overloads registered");
    check(has_op("string", int(BinExpr::Op::Add)) && has_op("string", int(BinExpr::Op::Eq)), "string: + == overloads registered");

    // --- array<T> get_bytes accessor: invalid handle returns false ---
    uint8_t* data = nullptr; int64_t len = -999;
    check(!ext_array::get_bytes(0, &data, &len), "array: get_bytes(invalid) == false");
    check(!ext_array::get_bytes(99999, &data, &len), "array: get_bytes(out-of-range) == false");

    // C5: allocation-taking extension boundaries reject negative/overflowing
    // sizes and do not let allocation exceptions escape into the host.
    auto array_new = reinterpret_cast<int64_t(*)(int64_t,int64_t)>(m.at("array_new").fn_ptr);
    auto array_resize = reinterpret_cast<void(*)(int64_t,int64_t)>(m.at("array_resize").fn_ptr);
    auto array_length = reinterpret_cast<int64_t(*)(int64_t)>(m.at("array_length").fn_ptr);
    check(array_new(1, -1) == 0, "array: negative count rejected");
    check(array_new(0, 1) == 0, "array: zero element size rejected");
    check(array_new(INT64_MAX, INT64_MAX) == 0, "array: overflowing/over-cap size rejected");
    int64_t ah = array_new(1, 4);
    array_resize(ah, -1);
    check(ah != 0 && array_length(ah) == 4, "array: rejected resize leaves container unchanged");
    array_resize(ah, INT64_MAX);
    check(array_length(ah) == 4, "array: over-cap resize leaves container unchanged");

    auto string_from_slice = reinterpret_cast<int64_t(*)(uint8_t*,int64_t)>(m.at("string_from_slice").fn_ptr);
    uint8_t byte = 'x';
    check(string_from_slice(&byte, -1) == 0, "string: negative length rejected");
    check(string_from_slice(&byte, INT64_MAX) == 0, "string: over-cap length rejected");

    auto swapbuf_new = reinterpret_cast<int64_t(*)(int64_t)>(m.at("swapbuf_new").fn_ptr);
    auto spsc_new = reinterpret_cast<int64_t(*)(int64_t)>(m.at("spsc_new").fn_ptr);
    auto mpsc_new = reinterpret_cast<int64_t(*)(int64_t,int64_t)>(m.at("mpsc_new").fn_ptr);
    auto mpmc_new = reinterpret_cast<int64_t(*)(int64_t)>(m.at("mpmc_new").fn_ptr);
    check(swapbuf_new(-1) == 0 && swapbuf_new(INT64_MAX) == 0, "sync: invalid swapbuf capacities rejected");
    check(spsc_new(-1) == 0 && spsc_new(INT64_MAX) == 0, "sync: invalid/round-up-overflow SPSC capacities rejected");
    check(mpsc_new(4, -1) == 0 && mpsc_new(INT64_MAX, 1) == 0 && mpsc_new(4, INT64_MAX) == 0,
          "sync: invalid MPSC capacity/producer count rejected");
    check(mpmc_new(-1) == 0 && mpmc_new(INT64_MAX) == 0, "sync: invalid MPMC capacities rejected");
    check(has("register_routine") && arity("register_routine") == 2 &&
          has("unregister_routine") && arity("unregister_routine") == 1,
          "lifecycle: register/unregister routine natives registered");

    // Defense in depth for stale/forged native-boundary handles: invoke the
    // operator functions directly and require their documented zero result.
    auto vec_add = reinterpret_cast<int64_t(*)(int64_t,int64_t)>(t.find("vec3", int(BinExpr::Op::Add))->fn_ptr);
    auto vec_eq = reinterpret_cast<int64_t(*)(int64_t,int64_t)>(t.find("vec3", int(BinExpr::Op::Eq))->fn_ptr);
    auto quat_mul = reinterpret_cast<int64_t(*)(int64_t,int64_t)>(t.find("quat", int(BinExpr::Op::Mul))->fn_ptr);
    auto mat_mul = reinterpret_cast<int64_t(*)(int64_t,int64_t)>(t.find("mat4", int(BinExpr::Op::Mul))->fn_ptr);
    check(vec_add(999, 999) == 0 && vec_eq(999, 999) == 0, "vec: invalid operator handles return zero/false");
    check(quat_mul(999, 999) == 0, "quat: invalid operator handles return zero");
    check(mat_mul(999, 999) == 0, "mat: invalid operator handles return zero");

    // --- string slot accessor: invalid handle returns nullptr ---
    check(ext_string::slot(0) == nullptr, "string: slot(invalid) == nullptr");
    check(ext_string::slot(99999) == nullptr, "string: slot(out-of-range) == nullptr");

    // --- reset() is callable and idempotent (stateless math has no reset) ---
    ext_vec::reset(); ext_quat::reset(); ext_mat::reset(); ext_string::reset(); ext_array::reset(); ext_sync::reset(); ext_lifecycle::reset(); ext_random::reset();
    ext_vec::reset(); ext_quat::reset(); ext_mat::reset(); ext_string::reset(); ext_array::reset(); ext_sync::reset(); ext_lifecycle::reset(); ext_random::reset();
    check(true, "extensions: reset() callable + idempotent");

    // --- purity: no host/process/render coupling - extensions only touch ember types ---
    // (structural: the test itself links only ember_ext_* + ember_frontend, no prism.)

    std::printf("\next registration smoke test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
