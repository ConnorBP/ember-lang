// tests/aapcs64_classify_test.cpp — unit tests for the AAPCS64 classifier.
// plan_MACOS_ARM64.md Phase 6c. Verifies scalar/slice/struct/HFA/indirect
// classification without executing any JIT code (pure logic test).
#include "aapcs64_classify.hpp"
#include "ast.hpp"
#include "sema.hpp"
#include <cstdio>
#include <memory>
#include <stdexcept>
using namespace ember;

static int g_fail = 0;
static void check(bool ok, const char* msg) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok) g_fail = 1;
}

static Type* make_scalar(Prim p) {
    Type t; t.prim = p; return new Type(t);  // leaked; test-only
}
static Type* make_struct_type(const char* name) {
    Type t; t.prim = Prim::I64; t.struct_name = name; return new Type(t);
}

int main() {
    StructLayoutTable structs;

    // struct Pair { a: i64; b: i64; }  size 16, align 1 (packed) -> 2 GP words
    StructLayout pair; pair.size = 16; pair.alignment = 1;
    pair.field_names = {"a", "b"};
    Type* i64t = make_scalar(Prim::I64);
    pair.fields["a"] = {i64t, 0};
    pair.fields["b"] = {i64t, 8};
    structs["Pair"] = pair;

    // struct Vec3 { x: f32; y: f32; z: f32; }  size 12 -> HFA (3 x f32) -> 3 FP regs
    StructLayout vec3; vec3.size = 12; vec3.alignment = 4;
    vec3.field_names = {"x", "y", "z"};
    Type* f32t = make_scalar(Prim::F32);
    vec3.fields["x"] = {f32t, 0};
    vec3.fields["y"] = {f32t, 4};
    vec3.fields["z"] = {f32t, 8};
    structs["Vec3"] = vec3;

    // struct Big { a: i64; b: i64; c: i64; }  size 24 -> indirect
    StructLayout big; big.size = 24; big.alignment = 1;
    big.field_names = {"a", "b", "c"};
    big.fields["a"] = {i64t, 0}; big.fields["b"] = {i64t, 8}; big.fields["c"] = {i64t, 16};
    structs["Big"] = big;

    // === AAPCS64 classifier boundary cases (audit M1) ===
    // Vec4 { x,y,z,w : f32 }  size 16 -> HFA 4xf32 (the MAX HFA count) -> 4 FP regs
    StructLayout vec4; vec4.size = 16; vec4.alignment = 4;
    vec4.field_names = {"x", "y", "z", "w"};
    vec4.fields["x"] = {f32t, 0}; vec4.fields["y"] = {f32t, 4};
    vec4.fields["z"] = {f32t, 8}; vec4.fields["w"] = {f32t, 12};
    structs["Vec4"] = vec4;

    // Vec5 { x,y,z,w,v : f32 }  size 20 -> NOT HFA (count 5 > 4) AND >16B -> indirect
    StructLayout vec5; vec5.size = 20; vec5.alignment = 4;
    vec5.field_names = {"x", "y", "z", "w", "v"};
    vec5.fields["x"] = {f32t, 0}; vec5.fields["y"] = {f32t, 4};
    vec5.fields["z"] = {f32t, 8};
    vec5.fields["w"] = {f32t, 12}; vec5.fields["v"] = {f32t, 16};
    structs["Vec5"] = vec5;

    // D2 { a,b : f64 }  size 16 -> HFA 2xf64 -> 2 FP regs (is_f32 == false)
    Type* f64t = make_scalar(Prim::F64);
    StructLayout d2; d2.size = 16; d2.alignment = 8;
    d2.field_names = {"a", "b"};
    d2.fields["a"] = {f64t, 0}; d2.fields["b"] = {f64t, 8};
    structs["D2"] = d2;

    // S17 { a: i64; b: i64; c: i8 }  size 17 -> just over 16B -> indirect
    StructLayout s17; s17.size = 17; s17.alignment = 1;
    s17.field_names = {"a", "b", "c"};
    Type* i8t = make_scalar(Prim::I8);
    s17.fields["a"] = {i64t, 0}; s17.fields["b"] = {i64t, 8}; s17.fields["c"] = {i8t, 16};
    structs["S17"] = s17;

    // S16x2 { a: i64; b: i64 }  size 16 exactly, non-HFA -> 2 GP words (boundary)
    StructLayout s16x2; s16x2.size = 16; s16x2.alignment = 1;
    s16x2.field_names = {"a", "b"};
    s16x2.fields["a"] = {i64t, 0}; s16x2.fields["b"] = {i64t, 8};
    structs["S16x2"] = s16x2;

    // Empty { }  size 0 -> no member slots (the classifier floors words to 1;
    // a zero-sized composite is an edge case — assert it does not crash +
    // records the expected classification the classifier produces).
    StructLayout empty_s; empty_s.size = 0; empty_s.alignment = 1;
    empty_s.field_names = {};
    structs["Empty"] = empty_s;

    // struct Mixed { x: f32; n: i64; }  size 12, mixed -> NOT HFA -> 2 GP words
    StructLayout mixed; mixed.size = 12; mixed.alignment = 1;
    mixed.field_names = {"x", "n"};
    mixed.fields["x"] = {f32t, 0}; mixed.fields["n"] = {i64t, 4};
    structs["Mixed"] = mixed;

    // scalar i64 -> 1 GP reg x0
    {
        auto c = classify_aapcs64_arg(i64t, &structs, 0, 0);
        check(c.slots.size() == 1 && c.slots[0].kind == Aapcs64Slot::Kind::GpReg
              && c.slots[0].reg_index == 0 && c.slots[0].width_bytes == 8,
              "scalar i64 -> 1 GP reg x0 (width 8)");
    }
    // scalar f32 -> 1 FP reg v0
    {
        auto c = classify_aapcs64_arg(f32t, &structs, 0, 0);
        check(c.slots.size() == 1 && c.slots[0].kind == Aapcs64Slot::Kind::FpReg
              && c.slots[0].reg_index == 0 && c.slots[0].is_f32,
              "scalar f32 -> 1 FP reg v0");
    }
    // slice -> 2 GP words x0, x1
    {
        Type* sl = new Type(); sl->prim = Prim::I64; sl->is_slice = true; sl->elem = std::make_shared<Type>(*i64t);
        auto c = classify_aapcs64_arg(sl, &structs, 0, 0);
        check(c.slots.size() == 2 && c.slots[0].kind == Aapcs64Slot::Kind::GpReg
              && c.slots[0].reg_index == 0 && c.slots[1].reg_index == 1,
              "slice -> 2 GP words x0, x1");
    }
    // Pair (16B, non-HFA) -> 2 GP words x0, x1
    {
        Type* pt = make_struct_type("Pair");
        auto c = classify_aapcs64_arg(pt, &structs, 0, 0);
        check(!c.indirect && c.slots.size() == 2
              && c.slots[0].kind == Aapcs64Slot::Kind::GpReg && c.slots[0].reg_index == 0
              && c.slots[1].kind == Aapcs64Slot::Kind::GpReg && c.slots[1].reg_index == 1,
              "Pair (16B non-HFA) -> 2 GP words x0, x1");
    }
    // Vec3 (HFA 3xf32) -> 3 FP regs v0, v1, v2
    {
        Type* vt = make_struct_type("Vec3");
        auto c = classify_aapcs64_arg(vt, &structs, 0, 0);
        check(!c.indirect && c.hfa_count == 3 && c.is_f32_hfa
              && c.slots.size() == 3
              && c.slots[0].kind == Aapcs64Slot::Kind::FpReg && c.slots[0].reg_index == 0
              && c.slots[1].reg_index == 1 && c.slots[2].reg_index == 2
              && c.slots[0].is_f32,
              "Vec3 (HFA 3xf32) -> 3 FP regs v0, v1, v2");
    }
    // Mixed (f32+i64, not pure HFA) -> 2 GP words
    {
        Type* mt = make_struct_type("Mixed");
        auto c = classify_aapcs64_arg(mt, &structs, 0, 0);
        check(!c.indirect && c.hfa_count == 0 && c.slots.size() == 2
              && c.slots[0].kind == Aapcs64Slot::Kind::GpReg,
              "Mixed (f32+i64, not HFA) -> 2 GP words");
    }
    // Big (24B) -> indirect (1 GP ptr slot)
    {
        Type* bt = make_struct_type("Big");
        auto c = classify_aapcs64_arg(bt, &structs, 0, 0);
        check(c.indirect && c.slots.size() == 1 && c.slots[0].kind == Aapcs64Slot::Kind::GpReg
              && c.slots[0].reg_index == 0,
              "Big (24B) -> indirect (ptr in x0)");
    }
    // GP + FP independent streams: f32 then i64 then f32 -> v0, x0, v1
    {
        auto c0 = classify_aapcs64_arg(f32t, &structs, 0, 0);
        auto c1 = classify_aapcs64_arg(i64t, &structs, 1, 1);  // gp_used=1 (after... actually f32 used fp not gp)
        // f32 consumes an FP reg, NOT a GP reg, so after arg0: gp_used=0, fp_used=1.
        // Reclassify arg1 with gp_used=0, fp_used=1:
        c1 = classify_aapcs64_arg(i64t, &structs, 0, 1);
        auto c2 = classify_aapcs64_arg(f32t, &structs, 1, 1);
        check(c0.slots[0].reg_index == 0 && c0.slots[0].kind == Aapcs64Slot::Kind::FpReg
              && c1.slots[0].reg_index == 0 && c1.slots[0].kind == Aapcs64Slot::Kind::GpReg
              && c2.slots[0].reg_index == 1 && c2.slots[0].kind == Aapcs64Slot::Kind::FpReg,
              "independent streams: f32->v0, i64->x0, f32->v1");
    }
    // Return: Vec3 HFA -> 3 FP regs (v0,v1,v2)
    {
        Type* vt = make_struct_type("Vec3");
        auto c = classify_aapcs64_return(vt, &structs);
        check(!c.indirect && c.hfa_count == 3 && c.slots.size() == 3
              && c.slots[0].kind == Aapcs64Slot::Kind::FpReg,
              "return Vec3 (HFA) -> 3 FP regs");
    }
    // Return: Big -> indirect (x8)
    {
        Type* bt = make_struct_type("Big");
        auto c = classify_aapcs64_return(bt, &structs);
        check(c.indirect, "return Big (24B) -> indirect (x8 dest)");
    }

    // === AAPCS64 classifier boundary-case assertions (audit M1) ===
    // (a) Vec4 (4xf32 HFA — the MAX) -> 4 FP regs v0..v3, hfa_count==4, is_f32.
    {
        Type* vt = make_struct_type("Vec4");
        auto c = classify_aapcs64_arg(vt, &structs, 0, 0);
        check(!c.indirect && c.hfa_count == 4 && c.is_f32_hfa && c.slots.size() == 4
              && c.slots[0].kind == Aapcs64Slot::Kind::FpReg && c.slots[0].reg_index == 0
              && c.slots[1].reg_index == 1 && c.slots[2].reg_index == 2 && c.slots[3].reg_index == 3
              && c.slots[0].is_f32,
              "Vec4 (HFA 4xf32, the max) -> 4 FP regs v0..v3");
    }
    // (b) Vec5 (5xf32 — NOT HFA, count>4; AND size 20>16) -> indirect (1 GP ptr).
    {
        Type* vt = make_struct_type("Vec5");
        auto c = classify_aapcs64_arg(vt, &structs, 0, 0);
        check(c.indirect && c.hfa_count == 0 && c.slots.size() == 1
              && c.slots[0].kind == Aapcs64Slot::Kind::GpReg && c.slots[0].reg_index == 0,
              "Vec5 (5xf32, >4 members + >16B) -> indirect (ptr in x0)");
    }
    // (c) D2 (2xf64 HFA) -> 2 FP regs, is_f32 == false (f64 width).
    {
        Type* dt = make_struct_type("D2");
        auto c = classify_aapcs64_arg(dt, &structs, 0, 0);
        check(!c.indirect && c.hfa_count == 2 && !c.is_f32_hfa && c.slots.size() == 2
              && c.slots[0].kind == Aapcs64Slot::Kind::FpReg && c.slots[0].reg_index == 0
              && c.slots[1].reg_index == 1 && !c.slots[0].is_f32 && c.slots[0].width_bytes == 8,
              "D2 (HFA 2xf64) -> 2 FP regs v0,v1 (f64 width)");
    }
    // (d) f32[3] array HFA -> 3 FP regs (array of identical floats is an HFA).
    {
        Type* arr = new Type(); arr->prim = Prim::F32; arr->array_len = 3;
        arr->elem = std::make_shared<Type>(*f32t);
        auto c = classify_aapcs64_arg(arr, &structs, 0, 0);
        check(!c.indirect && c.hfa_count == 3 && c.is_f32_hfa && c.slots.size() == 3
              && c.slots[0].kind == Aapcs64Slot::Kind::FpReg && c.slots[0].reg_index == 0
              && c.slots[1].reg_index == 1 && c.slots[2].reg_index == 2,
              "f32[3] (array HFA) -> 3 FP regs v0,v1,v2");
    }
    // (e) S17 (17B, just over 16) -> indirect.
    {
        Type* st = make_struct_type("S17");
        auto c = classify_aapcs64_arg(st, &structs, 0, 0);
        check(c.indirect && c.slots.size() == 1 && c.slots[0].kind == Aapcs64Slot::Kind::GpReg,
              "S17 (17B, just over 16) -> indirect");
    }
    // (f) S16x2 (16B exactly, non-HFA) -> 2 GP words (the <=16B boundary).
    {
        Type* st = make_struct_type("S16x2");
        auto c = classify_aapcs64_arg(st, &structs, 0, 0);
        check(!c.indirect && c.hfa_count == 0 && c.slots.size() == 2
              && c.slots[0].kind == Aapcs64Slot::Kind::GpReg && c.slots[0].reg_index == 0
              && c.slots[1].kind == Aapcs64Slot::Kind::GpReg && c.slots[1].reg_index == 1,
              "S16x2 (16B exactly, non-HFA) -> 2 GP words x0,x1");
    }
    // (g) >8 GP args: the 9th i64 arg (gp_used==8) must throw std::runtime_error.
    {
        bool threw = false;
        try {
            (void)classify_aapcs64_arg(i64t, &structs, 8, 0);  // gp_used=8 -> x8 OOB
        } catch (const std::runtime_error&) { threw = true; }
        check(threw, ">8 GP args: 9th i64 (gp_used==8) throws std::runtime_error");
    }
    // (h) >8 FP args: the 9th f32 arg (fp_used==8) must throw std::runtime_error.
    {
        bool threw = false;
        try {
            (void)classify_aapcs64_arg(f32t, &structs, 0, 8);  // fp_used=8 -> v8 OOB
        } catch (const std::runtime_error&) { threw = true; }
        check(threw, ">8 FP args: 9th f32 (fp_used==8) throws std::runtime_error");
    }
    // (i) Empty struct (size 0): must not crash; the classifier floors a
    //     zero-sized non-HFA composite to 1 GP word. Assert that stable
    //     behavior (the documented staging: a 0-byte composite is an edge case
    //     the classifier handles without faulting).
    {
        Type* et = make_struct_type("Empty");
        auto c = classify_aapcs64_arg(et, &structs, 0, 0);
        check(!c.indirect && c.hfa_count == 0 && c.slots.size() == 1
              && c.slots[0].kind == Aapcs64Slot::Kind::GpReg && c.slots[0].reg_index == 0,
              "Empty struct (size 0) -> 1 GP word (no crash, floors to 1 word)");
    }

    if (g_fail) { std::printf("\nFAILURE(S)\n"); return 1; }
    std::printf("\nALL PASS\n");
    return 0;
}
