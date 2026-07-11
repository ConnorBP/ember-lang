// host_struct_test.cpp — proof-of-concept: host C++ struct types as first-class
// by-value script arguments.
//
// Registers a Vec3 struct (3 × f32, C++-compatible layout: 12 bytes, align 4)
// via register_struct, then a native `dot(v: Vec3) -> f32` that reads the
// struct fields directly. The script constructs a Vec3 and passes it by value.
// If the layout is correct, the native receives the right bytes and the dot
// product is correct.

#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/engine.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/binding_builder.hpp"
#include "../src/globals.hpp"
#include "../src/context.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>

using namespace ember;

// The C++ struct we want to bind — this is what the host native expects.
struct CppVec3 {
    float x, y, z;
};

// The native: dot product of a Vec3 passed by value.
// Win64 ABI for a 12-byte struct: passed by hidden pointer (rcx = pointer to
// the struct, since 12 > 8 bytes).
// Actually — 12 bytes > 8, so it's passed by hidden pointer. The callee
// receives a pointer in rcx, reads the 3 floats, and returns f32 in xmm0.
// BUT: ember's current call ABI for >8-byte structs uses the hidden-pointer
// convention (caller allocates, passes pointer in the first arg slot).
// So the native's signature is: f32(void* vec3_ptr).
extern "C" float n_dot(CppVec3 v) {
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

int main() {
    const char* src =
        "fn main() -> i64 {\n"
        "    let v: Vec3 = Vec3 { x: 3.0f, y: 4.0f, z: 0.0f };\n"
        "    let d: f32 = dot(v);\n"
        "    return d as i64;\n"
        "}\n";

    // Lex + parse
    auto lr = tokenize(src, "<host_struct>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); return 1; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); return 1; }

    // Slot assignment
    std::unordered_map<std::string, int> slots;
    for (auto& fn : pr.program.funcs) { fn.slot = 0; slots[fn.name] = 0; }

    // Build script struct layouts (empty — no script structs)
    auto layouts = build_struct_layouts(pr.program);

    // Register the host Vec3 struct with C++-compatible layout
    register_struct(layouts, "Vec3", {
        {"x", bind_prim(Prim::F32)},
        {"y", bind_prim(Prim::F32)},
        {"z", bind_prim(Prim::F32)},
    });

    // Verify the layout
    auto& vl = layouts.at("Vec3");
    std::printf("Vec3 layout: size=%d, x@%d, y@%d, z@%d (C++ sizeof=%zu)\n",
                vl.size,
                vl.fields.at("x").offset,
                vl.fields.at("y").offset,
                vl.fields.at("z").offset,
                sizeof(CppVec3));
    bool layout_ok = vl.size == int32_t(sizeof(CppVec3)) &&
                     vl.fields.at("x").offset == 0 &&
                     vl.fields.at("y").offset == 4 &&
                     vl.fields.at("z").offset == 8;
    std::printf("[%s] Vec3 layout matches C++ (size=%d, sizeof=%zu)\n",
                layout_ok ? "PASS" : "FAIL", vl.size, sizeof(CppVec3));

    // Register the dot native
    // The native takes a Vec3 by value. Since Vec3 is 12 bytes (>8), ember's
    // ABI passes it by hidden pointer. But the C++ function expects a by-value
    // CppVec3. On Win64, a 12-byte struct IS passed by hidden pointer too
    // (non-POD or >8 bytes). So the ABI should match — but we need to verify.
    // For now, let's register it with the Vec3 type and see what happens.
    std::unordered_map<std::string, NativeSig> natives;
    BindingBuilder b;
    b.add("dot", bind_prim(Prim::F32), {bind_struct("Vec3")}, (void*)&n_dot);
    NativeTable nt = b.build();
    for (auto& [k, v] : nt.natives) natives[k] = std::move(v);

    // Sema
    auto sr = sema(pr.program, natives, slots, 0, nullptr, &layouts);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        return 1;
    }
    std::printf("[PASS] sema accepted Vec3 struct + dot(v: Vec3) call\n");

    // Codegen
    GlobalsBlock gb;
    std::vector<uint8_t> gbs(0);
    gb.base = 0;
    g_globals_for_codegen = &gb;
    DispatchTable table(1);
    CodeGenCtx ctx;
    ctx.globals_base = 0;
    ctx.dispatch_base = int64_t(table.base());
    ctx.natives = &natives;
    ctx.script_slots = &slots;
    ctx.structs = &layouts;
    ctx.use_context_reg = false;

    CompiledFn cf = compile_func(pr.program.funcs[0], ctx);
    if (cf.bytes.empty()) { std::printf("FAIL: codegen produced empty bytes\n"); return 1; }
    if (!finalize(cf)) { std::printf("FAIL: finalize (alloc_executable)\n"); return 1; }
    table.set(0, cf.entry);

    // Call
    context_t ectx;
    ectx.max_call_depth = 64;
    ectx.budget_remaining = INT64_MAX;
    ectx.has_checkpoint = false;
    int64_t result = ember_call_void(table.get(0), &ectx);
    free_executable(cf.exec);

    // Expected: dot(3,4,0) = 9+16+0 = 25
    std::printf("result = %lld (expected 25)\n", (long long)result);
    bool pass = (result == 25) && layout_ok;
    std::printf("\n%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
