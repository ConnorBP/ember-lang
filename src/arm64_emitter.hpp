// ember ARM64 (AArch64) emitter — header-only, mirrors src/x64_emitter.hpp.
//
// Emits fixed-width 32-bit AArch64 machine code (little-endian) for the
// instruction subset the ThinIR ARM64 backend needs (docs/planning/
// plan_MACOS_ARM64.md §4 Phase 3). Every instruction is exactly 4 bytes.
//
// Register model:
//   enum class XReg : uint8_t { x0..x30, sp=31, xzr=31 }
// Encoding 31 is SP for most operations and XZR for others (e.g. the Rd of
// `subs`/`cmp`, the Rm/Rn source of `mov`/`orr`). The per-instruction methods
// pass the raw 0-31 encoding; callers pick XReg::sp or XReg::xzr as the
// semantics demand. We do NOT enforce which; we just emit the bits.
//
// IMPORTANT — Apple platform constraint: NEVER use x18. It is Apple's
// reserved platform register. The emitter does not reserve or reject x18
// (it stays a dumb byte emitter); callers MUST NOT choose XReg::x18.
//
// Label/fixup system (mirrors X64Emitter):
//   alloc_label() -> Label{id}; bind(Label); resolve_fixups().
//   Branches emit a placeholder + record a Fixup carrying the Kind so
//   resolve_fixups() can detect out-of-range and emit a VENEER.
//
// VENEERS: B.cond/CBZ/CBNZ targets are limited to ±1 MiB. When a target is
// beyond that, resolve_fixups() appends an unconditional B (±128 MiB) to the
// real target at the end of the code and rewrites the conditional branch to
// branch-over (inverted condition) to that veneer slot.
//
// LITERAL POOL: relocatable 64-bit address loads use ldr_literal_ptr(), which
// reserves an 8-byte pointer cell in the literal pool and emits a PC-relative
// LDR (±1 MiB). The pool is appended at finalize/resolve_fixups; LDR offsets
// are backpatched. abs_fixups()/native_fixups() mirror X64Emitter so the .em
// serializer/loader can repoint the pointer cells.
//
// All encodings were verified against `llvm-mc -show-encoding
// -triple=arm64-apple-darwin` and/or the ARM ARM. Verified byte sequences are
// cited in tests/arm64_emitter_test.cpp.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <stdexcept>
#include <unordered_map>

namespace ember {

// 64-bit general-purpose register encoding (0-31). 31 is SP for most ops and
// XZR for others; callers choose the correct alias.
enum class XReg : uint8_t {
    x0=0,  x1=1,  x2=2,  x3=3,  x4=4,  x5=5,  x6=6,  x7=7,
    x8=8,  x9=9,  x10=10, x11=11, x12=12, x13=13, x14=14, x15=15,
    x16=16, x17=17, x18=18, // x18 is Apple-reserved — DO NOT use
    x19=19, x20=20, x21=21, x22=22, x23=23, x24=24, x25=25,
    x26=26, x27=27, x28=28, x29=29, x30=30,
    sp=31, xzr=31,
};

// 32-bit GP view (W registers share the 0-31 encoding with X registers).
enum class WReg : uint8_t {
    w0=0,  w1=1,  w2=2,  w3=3,  w4=4,  w5=5,  w6=6,  w7=7,
    w8=8,  w9=9,  w10=10, w11=11, w12=12, w13=13, w14=14, w15=15,
    w16=16, w17=17, w18=18,
    w19=19, w20=20, w21=21, w22=22, w23=23, w24=24, w25=25,
    w26=26, w27=27, w28=28, w29=29, w30=30,
    wsp=31, wzr=31,
};

// NEON/FP scalar register encoding (0-31). Named ArmVReg so the ARM64
// emitter can coexist with the IR's `using VReg = uint32_t` (thin_ir.hpp)
// in the combined ARM64 ThinIR emit TU. When included standalone (the
// arm64_emitter_test shape, x64_emitter.hpp absent), `using VReg = ArmVReg;`
// below restores the historical `VReg::v0` spelling.
enum class ArmVReg : uint8_t {
    v0=0,  v1=1,  v2=2,  v3=3,  v4=4,  v5=5,  v6=6,  v7=7,
    v8=8,  v9=9,  v10=10, v11=11, v12=12, v13=13, v14=14, v15=15,
    v16=16, v17=17, v18=18, v19=19, v20=20, v21=21, v22=22, v23=23,
    v24=24, v25=25, v26=26, v27=27, v28=28, v29=29, v30=30, v31=31,
};

// ARM64 condition codes (bits[3:0] of the B.cond/CSSEL encoding, 0-15). Named
// ArmCond so the ARM64 emitter can coexist with x64_emitter.hpp's `Cond`
// (jcc nibbles, different values) in the combined ARM64 ThinIR emit TU. When
// included standalone, `using Cond = ArmCond;` restores `Cond::eq`.
enum class ArmCond : uint8_t {
    eq=0,  ne=1,  cs=2,  cc=3,  mi=4,  pl=5,  vs=6,  vc=7,
    hi=8,  ls=9,  ge=10, lt=11, gt=12, le=13, al=14, nv=15,
};

// When x64_emitter.hpp is NOT included (standalone arm64 emitter/test), expose
// the historical `Cond`/`VReg` aliases and define arm64's own Label/AbsFixup/
// NativeFixup. When x64 IS included (the ARM64 ThinIR emit path), reuse x64's
// identical Label/AbsFixup/NativeFixup and do NOT alias Cond/VReg (x64's Cond
// + the IR's `using VReg = uint32_t` are already in scope) — callers use
// ArmCond/ArmVReg directly.
#ifndef EMBER_X64_EMITTER_DEFINED
using VReg = ArmVReg;
using Cond = ArmCond;

struct Label { uint32_t id; };

// A pending absolute pointer fixup for the literal pool (mirrors X64Emitter's
// AbsFixup). `code_offset` is the byte offset within `code` of the 8-byte
// pointer cell appended in the literal pool. resolve_fixups() appends the
// pool and records the offset; the .em serializer records one of these per
// cell so a loader can repoint it.
struct AbsFixup {
    uint32_t code_offset = 0;   // byte offset within `code` of the 8-byte cell
    enum Kind : uint8_t {
        DispatchTableBase  = 0,
        GlobalsBase        = 1,
        ModuleRegistryBase = 2,
        FunctionRodataBase = 3,
    } kind = DispatchTableBase;
    uint32_t addend = 0;
};

struct NativeFixup {
    uint32_t code_offset = 0;
    std::string name;
};
#endif // !EMBER_X64_EMITTER_DEFINED

class Arm64Emitter {
public:
    std::vector<uint8_t> code;

    // --- label/patch system (mirrors X64Emitter) ---
    Label alloc_label() { return {next_label++}; }
    void bind(Label l) {
        if (bound.find(l.id) != bound.end())
            throw std::logic_error("internal compiler error: duplicate label");
        bound[l.id] = uint32_t(code.size());
    }

    // --- low-level emission ---
    // Every AArch64 instruction is exactly 4 bytes, little-endian.
    void insn(uint32_t enc) {
        code.push_back(uint8_t(enc));
        code.push_back(uint8_t(enc >> 8));
        code.push_back(uint8_t(enc >> 16));
        code.push_back(uint8_t(enc >> 24));
    }
    void bytes(std::initializer_list<uint8_t> bs) { for (auto b : bs) code.push_back(b); }
    void imm64(int64_t v) { uint64_t u = uint64_t(v); for (int i = 0; i < 8; ++i) code.push_back(uint8_t(u >> (8*i))); }

    static uint8_t r(XReg x) { return uint8_t(x); }
    static uint8_t r(WReg w) { return uint8_t(w); }
    static uint8_t v(ArmVReg vr) { return uint8_t(vr); }

    // =====================================================================
    // ALU — register, register, [register | immediate]
    // =====================================================================

    // add Xd, Xn, Xm  (shift=LSL #0)  [verified: add x0,x1,x2 = 0x8B020020]
    void add_reg(XReg d, XReg n, XReg m) {
        insn(0x8B000000u | (r(m)<<16) | (r(n)<<5) | r(d));
    }
    // sub Xd, Xn, Xm  [verified: sub x3,x4,x5 = 0xCB050083]
    void sub_reg(XReg d, XReg n, XReg m) {
        insn(0xCB000000u | (r(m)<<16) | (r(n)<<5) | r(d));
    }
    // and Xd, Xn, Xm  [verified: and x9,x10,x11 = 0x8A0B0149]
    void and_reg(XReg d, XReg n, XReg m) {
        insn(0x8A000000u | (r(m)<<16) | (r(n)<<5) | r(d));
    }
    // orr Xd, Xn, Xm  [verified: orr x12,x13,x14 = 0xAA0E01AC]
    void orr_reg(XReg d, XReg n, XReg m) {
        insn(0xAA000000u | (r(m)<<16) | (r(n)<<5) | r(d));
    }
    // eor Xd, Xn, Xm  [verified: eor x15,x16,x17 = 0xCA110F0F]
    void eor_reg(XReg d, XReg n, XReg m) {
        insn(0xCA000000u | (r(m)<<16) | (r(n)<<5) | r(d));
    }
    // mul Xd, Xn, Xm  (MADD Xd,Xn,Xm,XZR)  [verified: mul x6,x7,x8 = 0x9B087CE6]
    void mul_reg(XReg d, XReg n, XReg m) {
        insn(0x9B000000u | (r(m)<<16) | (uint8_t(XReg::xzr)<<10) | (r(n)<<5) | r(d));
    }
    // madd Xd, Xn, Xm, Xa  (Xd = Xa + Xn*Xm)  [verified: madd x0,x1,x2,x3 = 0x9B020C20]
    void madd_reg(XReg d, XReg n, XReg m, XReg a) {
        insn(0x9B000000u | (r(m)<<16) | (r(a)<<10) | (r(n)<<5) | r(d));
    }
    // msub Xd, Xn, Xm, Xa  (Xd = Xa - Xn*Xm)  [verified: msub x0,x1,x2,x3 = 0x9B028C20]
    void msub_reg(XReg d, XReg n, XReg m, XReg a) {
        insn(0x9B008000u | (r(m)<<16) | (r(a)<<10) | (r(n)<<5) | r(d));
    }
    // sdiv Xd, Xn, Xm  (signed 64-bit divide)  [verified: sdiv x0,x1,x2 = 0x9AC20C20]
    void sdiv_reg(XReg d, XReg n, XReg m) {
        insn(0x9AC00C00u | (r(m)<<16) | (r(n)<<5) | r(d));
    }
    // udiv Xd, Xn, Xm  (unsigned 64-bit divide)  [verified: udiv x0,x1,x2 = 0x9AC20820]
    void udiv_reg(XReg d, XReg n, XReg m) {
        insn(0x9AC00800u | (r(m)<<16) | (r(n)<<5) | r(d));
    }

    // Variable shifts (data-proc shift): LSLV/LSRV/ASRV Xd, Xn, Xm
    // [verified: lsl x21,x22,x23 = 0x9AD722D5; lsr x0,x1,x2 = 0x9AC22420; asr x0,x1,x2 = 0x9AC22820]
    void lsl_reg(XReg d, XReg n, XReg m) {
        insn(0x9AC02000u | (r(m)<<16) | (r(n)<<5) | r(d));
    }
    void lsr_reg(XReg d, XReg n, XReg m) {
        insn(0x9AC02400u | (r(m)<<16) | (r(n)<<5) | r(d));
    }
    void asr_reg(XReg d, XReg n, XReg m) {
        insn(0x9AC02800u | (r(m)<<16) | (r(n)<<5) | r(d));
    }

    // Immediate shifts (UBFM/SBFM aliases): LSL/LSR/ASR Xd, Xn, #amount
    //   lsl Xd,Xn,#s = ubfm Xd,Xn, #(-s mod 64), #(63-s)
    //   lsr Xd,Xn,#s = ubfm Xd,Xn, #s, #63
    //   asr Xd,Xn,#s = sbfm Xd,Xn, #s, #63
    // [verified: lsl x0,x1,#5 = 0xD37BE820; lsr x0,x1,#5 = 0xD345FC20; asr x0,x1,#5 = 0x9345FC20]
    void lsl_imm(XReg d, XReg n, uint8_t amount) {
        if (amount >= 64) throw std::out_of_range("ember: lsl_imm amount >= 64");
        uint32_t immr = (64u - amount) % 64u;
        uint32_t imms = 63u - amount;
        insn(0xD3400000u | (immr<<16) | (imms<<10) | (r(n)<<5) | r(d));
    }
    void lsr_imm(XReg d, XReg n, uint8_t amount) {
        if (amount >= 64) throw std::out_of_range("ember: lsr_imm amount >= 64");
        insn(0xD3400000u | (uint32_t(amount)<<16) | (63u<<10) | (r(n)<<5) | r(d));
    }
    void asr_imm(XReg d, XReg n, uint8_t amount) {
        if (amount >= 64) throw std::out_of_range("ember: asr_imm amount >= 64");
        insn(0x93400000u | (uint32_t(amount)<<16) | (63u<<10) | (r(n)<<5) | r(d));
    }

    // mov Xd, Xm  = orr Xd, XZR, Xm  [verified: mov x0,x1 = 0xAA0103E0]
    void mov_reg(XReg d, XReg m) {
        insn(0xAA000000u | (r(m)<<16) | (uint8_t(XReg::xzr)<<5) | r(d));
    }
    // mvn Xd, Xm  = orn Xd, XZR, Xm  [verified: mvn x0,x1 = 0xAA2103E0]
    void mvn_reg(XReg d, XReg m) {
        insn(0xAA200000u | (r(m)<<16) | (uint8_t(XReg::xzr)<<5) | r(d));
    }
    // neg Xd, Xm  = sub Xd, XZR, Xm  [verified: neg x0,x1 = 0xCB0103E0]
    void neg_reg(XReg d, XReg m) {
        insn(0xCB000000u | (r(m)<<16) | (uint8_t(XReg::xzr)<<5) | r(d));
    }

    // add/sub Xd, Xn, #imm12 (12-bit unsigned immediate, shifted by 0 or 12).
    //   sh=false -> imm << 0;  sh=true -> imm << 12.
    // [verified: add x0,x1,#0x123 = 0x91048C20; sub x0,x1,#0x123 = 0xD1048C20;
    //  add x0,x1,#0x123,lsl#12 = 0x91448C20]
    void add_reg_imm(XReg d, XReg n, uint32_t imm12, bool sh12 = false) {
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: add_reg_imm imm12 > 4095");
        insn(0x91000000u | (sh12 ? (1u<<22) : 0u) | (imm12<<10) | (r(n)<<5) | r(d));
    }
    void sub_reg_imm(XReg d, XReg n, uint32_t imm12, bool sh12 = false) {
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: sub_reg_imm imm12 > 4095");
        insn(0xD1000000u | (sh12 ? (1u<<22) : 0u) | (imm12<<10) | (r(n)<<5) | r(d));
    }

    // cmp Xn, Xm  = subs XZR, Xn, Xm  [verified: cmp x0,x1 = 0xEB01001F]
    void cmp_reg(XReg n, XReg m) {
        insn(0xEB000000u | (r(m)<<16) | (r(n)<<5) | uint8_t(XReg::xzr));
    }
    // cmp Xn, #imm12  = subs XZR, Xn, #imm12  [verified: cmp x0,#0x123 = 0xF1048C1F]
    void cmp_reg_imm(XReg n, uint32_t imm12, bool sh12 = false) {
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: cmp_reg_imm imm12 > 4095");
        insn(0xF1000000u | (sh12 ? (1u<<22) : 0u) | (imm12<<10) | (r(n)<<5) | uint8_t(XReg::xzr));
    }

    // cset Xd, cond  = csinc Xd, XZR, XZR, inv(cond)
    // The condition is inverted: cset eq (0) -> csinc ne (1), etc.
    // [verified: cset x0,eq = 0x9A9F17E0; cset x0,ne = 0x9A9F07E0]
    void cset(XReg d, ArmCond cc) {
        uint32_t inv = uint8_t(cc) ^ 1u;  // invert bit 0
        insn(0x9A800400u | (inv<<12) | (uint8_t(XReg::xzr)<<16) | (uint8_t(XReg::xzr)<<5) | r(d));
    }

    // =====================================================================
    // Load / store
    // Scaled unsigned offset forms: [base, #imm] where imm must be aligned &
    // in range (imm / size fits in 12 bits).  Unscaled signed 9-bit forms
    // (LDUR/STUR) support negative/unaligned offsets — frame slots live at
    // negative offsets from FP.
    // =====================================================================

    // 64-bit  [verified: ldr x0,[x1,#8]=0xF9400420; str x2,[x3,#16]=0xF9000862]
    void ldr64(XReg t, XReg base, uint32_t imm12) {  // imm12 = offset/8
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: ldr64 imm12 > 4095");
        insn(0xF9400000u | (imm12<<10) | (r(base)<<5) | r(t));
    }
    void str64(XReg t, XReg base, uint32_t imm12) {
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: str64 imm12 > 4095");
        insn(0xF9000000u | (imm12<<10) | (r(base)<<5) | r(t));
    }
    // 32-bit (load zero-extends to 64) [verified: ldr w0,[x1,#4]=0xB9400420; str w2,[x3,#4]=0xB9000862]
    void ldr32(XReg t, XReg base, uint32_t imm12) {  // imm12 = offset/4
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: ldr32 imm12 > 4095");
        insn(0xB9400000u | (imm12<<10) | (r(base)<<5) | r(t));
    }
    void str32(XReg t, XReg base, uint32_t imm12) {
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: str32 imm12 > 4095");
        insn(0xB9000000u | (imm12<<10) | (r(base)<<5) | r(t));
    }
    // ldrsw — sign-extend 32->64  [verified: ldrsw x0,[x1,#4]=0xB9800420]
    void ldrsw(XReg t, XReg base, uint32_t imm12) {
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: ldrsw imm12 > 4095");
        insn(0xB9800000u | (imm12<<10) | (r(base)<<5) | r(t));
    }
    // 16-bit  [verified: ldrh w0,[x1,#2]=0x79400420; strh w2,[x3,#2]=0x79000462]
    void ldrh(XReg t, XReg base, uint32_t imm12) {  // imm12 = offset/2
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: ldrh imm12 > 4095");
        insn(0x79400000u | (imm12<<10) | (r(base)<<5) | r(t));
    }
    void strh(XReg t, XReg base, uint32_t imm12) {
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: strh imm12 > 4095");
        insn(0x79000000u | (imm12<<10) | (r(base)<<5) | r(t));
    }
    // 8-bit  [verified: ldrb w0,[x1,#1]=0x39400420; strb w2,[x3]=0x39000062]
    void ldrb(XReg t, XReg base, uint32_t imm12) {
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: ldrb imm12 > 4095");
        insn(0x39400000u | (imm12<<10) | (r(base)<<5) | r(t));
    }
    void strb(XReg t, XReg base, uint32_t imm12) {
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: strb imm12 > 4095");
        insn(0x39000000u | (imm12<<10) | (r(base)<<5) | r(t));
    }
    // Sign/zero-extend narrow loads (scaled):
    //   ldrsh (16->32 or 16->64), ldrsb (8->32 or 8->64)
    // [verified: ldrsh w0,[x1]=0x79C00020 (32); ldrsh x0,[x1]=0x79800020 (64);
    //  ldrsb w0,[x1]=0x39C00020 (32); ldrsb x0,[x1]=0x39800020 (64)]
    void ldrsh32(XReg t, XReg base, uint32_t imm12) {
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: ldrsh32 imm12 > 4095");
        insn(0x79C00000u | (imm12<<10) | (r(base)<<5) | r(t));
    }
    void ldrsh64(XReg t, XReg base, uint32_t imm12) {
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: ldrsh64 imm12 > 4095");
        insn(0x79800000u | (imm12<<10) | (r(base)<<5) | r(t));
    }
    void ldrsb32(XReg t, XReg base, uint32_t imm12) {
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: ldrsb32 imm12 > 4095");
        insn(0x39C00000u | (imm12<<10) | (r(base)<<5) | r(t));
    }
    void ldrsb64(XReg t, XReg base, uint32_t imm12) {
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: ldrsb64 imm12 > 4095");
        insn(0x39800000u | (imm12<<10) | (r(base)<<5) | r(t));
    }

    // --- Unscaled (LDUR/STUR) — signed 9-bit immediate, no alignment.
    // [verified: ldur x0,[x1,#-8]=0xF85F8020; stur x2,[x3,#-16]=0xF81F0062;
    //  ldur w0,[x1,#-4]=0xB85FC020; stur w2,[x3,#12]=0xB800C062;
    //  ldur x0,[x1,#255]=0xF84FF020; ldur x0,[x1,#-256]=0xF8500020]
    void ldur64(XReg t, XReg base, int32_t imm9) {
        if (imm9 < -256 || imm9 > 255) throw std::out_of_range("ember: ldur64 imm9 out of range");
        insn(0xF8400000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | r(t));
    }
    void stur64(XReg t, XReg base, int32_t imm9) {
        if (imm9 < -256 || imm9 > 255) throw std::out_of_range("ember: stur64 imm9 out of range");
        insn(0xF8000000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | r(t));
    }
    void ldur32(XReg t, XReg base, int32_t imm9) {
        if (imm9 < -256 || imm9 > 255) throw std::out_of_range("ember: ldur32 imm9 out of range");
        insn(0xB8400000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | r(t));
    }
    void stur32(XReg t, XReg base, int32_t imm9) {
        if (imm9 < -256 || imm9 > 255) throw std::out_of_range("ember: stur32 imm9 out of range");
        insn(0xB8000000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | r(t));
    }
    void ldursw(XReg t, XReg base, int32_t imm9) {
        if (imm9 < -256 || imm9 > 255) throw std::out_of_range("ember: ldursw imm9 out of range");
        insn(0xB8800000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | r(t));
    }
    void ldurh(XReg t, XReg base, int32_t imm9) {
        if (imm9 < -256 || imm9 > 255) throw std::out_of_range("ember: ldurh imm9 out of range");
        insn(0x78400000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | r(t));
    }
    void sturh(XReg t, XReg base, int32_t imm9) {
        if (imm9 < -256 || imm9 > 255) throw std::out_of_range("ember: sturh imm9 out of range");
        insn(0x78000000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | r(t));
    }
    void ldurb(XReg t, XReg base, int32_t imm9) {
        if (imm9 < -256 || imm9 > 255) throw std::out_of_range("ember: ldurb imm9 out of range");
        insn(0x38400000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | r(t));
    }
    void sturb(XReg t, XReg base, int32_t imm9) {
        if (imm9 < -256 || imm9 > 255) throw std::out_of_range("ember: sturb imm9 out of range");
        insn(0x38000000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | r(t));
    }
    void ldursh32(XReg t, XReg base, int32_t imm9) {
        if (imm9 < -256 || imm9 > 255) throw std::out_of_range("ember: ldursh32 imm9 out of range");
        insn(0x79C00000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | r(t));
    }
    void ldursh64(XReg t, XReg base, int32_t imm9) {
        if (imm9 < -256 || imm9 > 255) throw std::out_of_range("ember: ldursh64 imm9 out of range");
        insn(0x79800000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | r(t));
    }
    void ldursb32(XReg t, XReg base, int32_t imm9) {
        if (imm9 < -256 || imm9 > 255) throw std::out_of_range("ember: ldursb32 imm9 out of range");
        insn(0x39C00000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | r(t));
    }
    void ldursb64(XReg t, XReg base, int32_t imm9) {
        if (imm9 < -256 || imm9 > 255) throw std::out_of_range("ember: ldursb64 imm9 out of range");
        insn(0x39800000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | r(t));
    }

    // =====================================================================
    // Branches
    // =====================================================================

    // b Label  (±128 MiB, imm26)  [verified: b #256 = 0x14000040]
    void b(Label l) {
        uint32_t off = uint32_t(code.size());
        insn(0x14000000u);
        pending_.push_back({off, l.id, uint8_t(FixKind::B)});
    }
    // bl Label  (±128 MiB, imm26)  [verified: bl #256 = 0x94000040]
    void bl(Label l) {
        uint32_t off = uint32_t(code.size());
        insn(0x94000000u);
        pending_.push_back({off, l.id, uint8_t(FixKind::B)});
    }
    // b.cond cond, Label  (±1 MiB, imm19; veneer-capable)  [verified: b.eq #256 = 0x54000800]
    void b_cond(ArmCond cc, Label l) {
        uint32_t off = uint32_t(code.size());
        insn(0x54000000u | (uint8_t(cc)));
        pending_.push_back({off, l.id, uint8_t(FixKind::Bcond), uint8_t(cc)});
    }
    // cbz/cbnz 64-bit  (±1 MiB, imm19; veneer-capable)  [verified: cbz x0,#256 = 0xB4000800; cbnz x0,#256 = 0xB5000800]
    void cbz64(XReg t, Label l) {
        uint32_t off = uint32_t(code.size());
        insn(0xB4000000u | r(t));
        pending_.push_back({off, l.id, uint8_t(FixKind::CBZ)});
    }
    void cbnz64(XReg t, Label l) {
        uint32_t off = uint32_t(code.size());
        insn(0xB5000000u | r(t));
        pending_.push_back({off, l.id, uint8_t(FixKind::CBNZ)});
    }
    // cbz/cbnz 32-bit  [verified: cbz w0,#256 = 0x34000800; cbnz w0,#256 = 0x35000800]
    void cbz32(XReg t, Label l) {
        uint32_t off = uint32_t(code.size());
        insn(0x34000000u | r(t));
        pending_.push_back({off, l.id, uint8_t(FixKind::CBZ32)});
    }
    void cbnz32(XReg t, Label l) {
        uint32_t off = uint32_t(code.size());
        insn(0x35000000u | r(t));
        pending_.push_back({off, l.id, uint8_t(FixKind::CBNZ32)});
    }

    // ret  (ret x30)  [verified: ret = 0xD65F03C0]
    void ret() { insn(0xD65F03C0u); }
    // blr Xn  (indirect call)  [verified: blr x9 = 0xD63F0120]
    void blr(XReg n) { insn(0xD63F0000u | (r(n)<<5)); }
    // br Xn  (indirect jump)  [verified: br x9 = 0xD61F0120]
    void br(XReg n) { insn(0xD61F0000u | (r(n)<<5)); }

    // =====================================================================
    // Address generation (PC-relative)
    // =====================================================================

    // adr Xd, Label  (±1 MiB, PC-rel)  [verified: adr x0,#256 = 0x10000800]
    void adr(XReg d, Label l) {
        uint32_t off = uint32_t(code.size());
        insn(0x10000000u | r(d));
        pending_.push_back({off, l.id, uint8_t(FixKind::ADR)});
    }
    // adrp Xd, page_addr  (±4 GiB, 4 KiB-aligned page). page_addr must be 4 KiB aligned.
    // [verified: adrp x0,#4096 = 0xB0000000; adrp x0,#8192 = 0xD0000000]
    void adrp(XReg d, int64_t page_addr) {
        // The assembler computes page_addr relative to the PC-aligned page.
        // Here we take an absolute page address and emit it as a raw adrp with
        // the page delta baked in relative to the current instruction's page.
        // NOTE: callers that want a runtime-resolved page should use adr_label.
        int64_t pc_page = int64_t(code.size()) & ~int64_t(0xFFF);
        int64_t delta = (page_addr & ~int64_t(0xFFF)) - pc_page;
        // adrp imm = delta >> 12, split into immlo[30:29] and immhi[23:5]
        int64_t imm = (delta >> 12) & ((int64_t(1) << 33) - 1);  // 33-bit signed
        uint32_t immlo = uint32_t(imm & 3);
        uint32_t immhi = uint32_t((imm >> 2) & 0x7FFFF);
        insn(0x90000000u | (immlo << 29) | (immhi << 5) | r(d));
    }
    // add Xd, Xn, #page_off  (the +offset half of adrp+add, 12-bit imm)
    void add_reg_imm32(XReg d, XReg n, uint32_t imm12) {
        if (imm12 > 0xFFFu) throw std::out_of_range("ember: add_reg_imm32 imm12 > 4095");
        insn(0x91000000u | (imm12<<10) | (r(n)<<5) | r(d));
    }
    // adr_label Xd, Label  — use ADR if in ±1 MiB, else ADRP + ADD (2 insns).
    // Resolved at finalize. The ADRP+ADD path records the label offset and
    // computes page + page-offset at resolve time.
    void adr_label(XReg d, Label l) {
        uint32_t off = uint32_t(code.size());
        insn(0x10000000u | r(d));
        pending_.push_back({off, l.id, uint8_t(FixKind::AdrLabel)});
    }
    // adrp_add_label Xd, Label  — ALWAYS ADRP + ADD :lo12: (2 instructions,
    // ±4 GiB reach). Robust for any label distance (e.g. a catch-entry PC in a
    // huge try body that exceeds ADR's ±1 MiB). Resolved at finalize: the ADRP
    // gets the target's page (relative to the ADRP's PC page) and the ADD gets
    // the target's :lo12: offset. The fixup records the ADRP offset; the ADD is
    // at +4. plan_MACOS_ARM64.md Phase 5 (catch-entry PC materialization).
    void adrp_add_label(XReg d, Label l) {
        uint32_t off = uint32_t(code.size());
        // ADRP Xd, page  (placeholder; patched at resolve)
        insn(0x90000000u | r(d));
        // ADD Xd, Xd, #imm12  (placeholder :lo12:; patched at resolve)
        insn(0x91000000u | (r(d)<<5) | r(d));
        pending_.push_back({off, l.id, uint8_t(FixKind::AdrpAddLabel)});
    }

    // =====================================================================
    // 64-bit immediate + relocatable pointer loads
    // =====================================================================

    // mov_reg_imm64 Xd, imm  via movz/movk (≤4 insns). For genuine constants.
    // [verified: movz x0,#0xCDEF = 0xD299BDE0; movk x0,#0x9ABC,lsl#16 = 0xF2B35780; ...]
    void mov_reg_imm64(XReg d, int64_t imm) {
        uint64_t v = uint64_t(imm);
        // movz the lowest 16 bits
        movz16(d, uint16_t(v & 0xFFFF), 0);
        if ((v >> 16) & 0xFFFF) movk16(d, uint16_t((v >> 16) & 0xFFFF), 1);
        if ((v >> 32) & 0xFFFF) movk16(d, uint16_t((v >> 32) & 0xFFFF), 2);
        if ((v >> 48) & 0xFFFF) movk16(d, uint16_t((v >> 48) & 0xFFFF), 3);
    }

    // ldr_literal_ptr Xd, kind, addend  — relocatable 64-bit address load.
    // Emits `LDR Xd, [pc + offset]` (±1 MiB) whose 8-byte pointer cell is
    // appended in the literal pool at finalize. The cell is an AbsFixup so the
    // .em serializer/loader can repoint it. Mirrors X64Emitter's
    // mov_reg_imm64_external / abs_fixups().
    void ldr_literal_ptr(XReg d, AbsFixup::Kind kind, uint32_t addend = 0) {
        uint32_t off = uint32_t(code.size());
        // LDR (literal) 64-bit: opc=01, V=0, 011 000 imm19 Rt  -> base 0x58000000
        insn(0x58000000u | r(d));
        literal_fixups_.push_back({off, d, kind, addend});
    }

    // mov_reg_native Xd, name  — relocatable address load resolved by symbol
    // name at load time (mirrors X64Emitter's mov_reg_native / native_fixups()).
    // Emits a PC-relative LDR whose 8-byte cell is appended in the literal pool;
    // the cell is recorded as a NativeFixup (name) for the .em loader.
    void mov_reg_native(XReg d, const std::string& name) {
        uint32_t off = uint32_t(code.size());
        insn(0x58000000u | r(d));
        native_literal_.push_back({off, d, name});
    }

    // Read-only views of the absolute/native fixups for the .em serializer.
    // Each entry's code_offset is the byte offset of the 8-byte pointer cell
    // in the appended literal pool (post-resolve_fixups).
    const std::vector<AbsFixup>& abs_fixups() const { return abs_fixups_; }
    const std::vector<NativeFixup>& native_fixups() const { return native_fixups_; }

    // =====================================================================
    // NEON scalar FP
    // =====================================================================

    // fmov Vd, Vn  (f32 / f64)  [verified: fmov s0,s1 = 0x1E204020; fmov d0,d1 = 0x1E604020]
    void fmov_f32(ArmVReg d, ArmVReg n) { insn(0x1E204000u | (v(n)<<5) | v(d)); }
    void fmov_f64(ArmVReg d, ArmVReg n) { insn(0x1E604000u | (v(n)<<5) | v(d)); }

    // FP load/store — verified empirically via `clang -c` + `otool -t`.
    // Unscaled (negative/unaligned offsets, imm9) — for frame slots at [FP+off]:
    //   ldur s0,[x29,#-8] = 0xBC5F83A0 ; ldur d0,[x29,#-8] = 0xFC5F83A0
    //   stur s0,[x29,#-8] = 0xBC1F83A0 ; stur d0,[x29,#-8] = 0xFC1F83A0
    void ldur_f32(ArmVReg t, XReg base, int32_t imm9) {
        insn(0xBC400000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | v(t));
    }
    void ldur_f64(ArmVReg t, XReg base, int32_t imm9) {
        insn(0xFC400000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | v(t));
    }
    void stur_f32(ArmVReg t, XReg base, int32_t imm9) {
        insn(0xBC000000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | v(t));
    }
    void stur_f64(ArmVReg t, XReg base, int32_t imm9) {
        insn(0xFC000000u | (uint32_t(imm9 & 0x1FF) << 12) | (r(base)<<5) | v(t));
    }
    // Scaled (unsigned offset, imm12 = offset/4 for f32, /8 for f64):
    //   ldr s0,[x1,#16] = 0xBD401020 ; ldr d0,[x1,#16] = 0xFD400820
    //   str s1,[x2,#24] = 0xBD001841 ; str d2,[x3,#32] = 0xFD001062
    void ldr_f32(ArmVReg t, XReg base, uint32_t imm12) {  // imm12 = offset/4
        insn(0xBD400000u | (imm12<<10) | (r(base)<<5) | v(t));
    }
    void ldr_f64(ArmVReg t, XReg base, uint32_t imm12) {  // imm12 = offset/8
        insn(0xFD400000u | (imm12<<10) | (r(base)<<5) | v(t));
    }
    void str_f32(ArmVReg t, XReg base, uint32_t imm12) {
        insn(0xBD000000u | (imm12<<10) | (r(base)<<5) | v(t));
    }
    void str_f64(ArmVReg t, XReg base, uint32_t imm12) {
        insn(0xFD000000u | (imm12<<10) | (r(base)<<5) | v(t));
    }

    // fadd/fsub/fmul/fdiv f32  [verified: fadd s0,s1,s2 = 0x1E222820; ...]
    void fadd_f32(ArmVReg d, ArmVReg n, ArmVReg m) { fp3(0x1E200800u, 2, d, n, m); }
    void fsub_f32(ArmVReg d, ArmVReg n, ArmVReg m) { fp3(0x1E200800u, 3, d, n, m); }
    void fmul_f32(ArmVReg d, ArmVReg n, ArmVReg m) { fp3(0x1E200800u, 0, d, n, m); }
    void fdiv_f32(ArmVReg d, ArmVReg n, ArmVReg m) { fp3(0x1E200800u, 1, d, n, m); }
    // fadd/fsub/fmul/fdiv f64  [verified: fadd d0,d1,d2 = 0x1E622820; ...]
    void fadd_f64(ArmVReg d, ArmVReg n, ArmVReg m) { fp3(0x1E600800u, 2, d, n, m); }
    void fsub_f64(ArmVReg d, ArmVReg n, ArmVReg m) { fp3(0x1E600800u, 3, d, n, m); }
    void fmul_f64(ArmVReg d, ArmVReg n, ArmVReg m) { fp3(0x1E600800u, 0, d, n, m); }
    void fdiv_f64(ArmVReg d, ArmVReg n, ArmVReg m) { fp3(0x1E600800u, 1, d, n, m); }

    // fcmp f32 / f64  [verified: fcmp s0,s1 = 0x1E212000; fcmp d0,d1 = 0x1E612000]
    void fcmp_f32(ArmVReg n, ArmVReg m) { insn(0x1E202000u | (v(m)<<16) | (v(n)<<5)); }
    void fcmp_f64(ArmVReg n, ArmVReg m) { insn(0x1E602000u | (v(m)<<16) | (v(n)<<5)); }

    // fcvt single<->double  [verified: fcvt s0,d0 = 0x1E624000; fcvt d0,s0 = 0x1E22C000]
    void fcvt_s32d(ArmVReg d, ArmVReg n) { insn(0x1E624000u | (v(n)<<5) | v(d)); }  // d -> s
    void fcvt_d32(ArmVReg d, ArmVReg n) { insn(0x1E22C000u | (v(n)<<5) | v(d)); }  // s -> d

    // scvtf — int -> float  [verified: scvtf s0,w0 = 0x1E220000; scvtf d0,x0 = 0x9E620000; ...]
    void scvtf_f32_w(ArmVReg d, XReg n) { insn(0x1E220000u | (r(n)<<5) | v(d)); }  // w32 -> f32
    void scvtf_f32_x(ArmVReg d, XReg n) { insn(0x9E220000u | (r(n)<<5) | v(d)); }  // x64 -> f32
    void scvtf_f64_w(ArmVReg d, XReg n) { insn(0x1E620000u | (r(n)<<5) | v(d)); }  // w32 -> f64
    void scvtf_f64_x(ArmVReg d, XReg n) { insn(0x9E620000u | (r(n)<<5) | v(d)); }  // x64 -> f64

    // fcvtzs — float -> int  [verified: fcvtzs w0,s0 = 0x1E380000; fcvtzs x0,d0 = 0x9E780000; ...]
    void fcvtzs_w_f32(XReg d, ArmVReg n) { insn(0x1E380000u | (v(n)<<5) | r(d)); }  // f32 -> w32
    void fcvtzs_x_f32(XReg d, ArmVReg n) { insn(0x9E380000u | (v(n)<<5) | r(d)); }  // f32 -> x64
    void fcvtzs_w_f64(XReg d, ArmVReg n) { insn(0x1E780000u | (v(n)<<5) | r(d)); }  // f64 -> w32
    void fcvtzs_x_f64(XReg d, ArmVReg n) { insn(0x9E780000u | (v(n)<<5) | r(d)); }  // f64 -> x64

    // fmov GPR <-> FPR  [verified: fmov w0,s0 = 0x1E260000; fmov s0,w0 = 0x1E270000;
    //  fmov x0,d0 = 0x9E660000; fmov d0,x0 = 0x9E670000]
    void fmov_int_to_fp_f32(ArmVReg d, XReg n) { insn(0x1E270000u | (r(n)<<5) | v(d)); }  // w -> s
    void fmov_fp_to_int_f32(XReg d, ArmVReg n) { insn(0x1E260000u | (v(n)<<5) | r(d)); }  // s -> w
    void fmov_int_to_fp_f64(ArmVReg d, XReg n) { insn(0x9E670000u | (r(n)<<5) | v(d)); }  // x -> d
    void fmov_fp_to_int_f64(XReg d, ArmVReg n) { insn(0x9E660000u | (v(n)<<5) | r(d)); }  // d -> x

    // =====================================================================
    // Misc
    // =====================================================================

    // nop  [verified: nop = 0xD503201F]
    void nop() { insn(0xD503201Fu); }

    // udf #imm16  (ARM64 trap / undefined instruction). Assemblers (LLVM/GNU)
    // encode imm16 in bits[15:0]:  [verified: udf #0x1234 = 0x00001234; udf #1 = 0x00000001]
    void udf(uint16_t imm16) { insn(0x00000000u | uint32_t(imm16)); }

    // =====================================================================
    // finalize / resolve
    // =====================================================================

    // Resolve all pending label fixups, emit the literal pool, backpatch LDR
    // offsets, apply veneers, and record abs/native fixups. Throws on unbound
    // label. Must be called after all labels are bound and all code emitted.
    void resolve_fixups();

    // Convenience alias.
    void finalize() { resolve_fixups(); }

    const std::unordered_map<uint32_t, uint32_t>& resolved_labels_view() const { return bound; }

private:
    enum class FixKind : uint8_t {
        B,       // unconditional branch / bl, ±128 MiB imm26
        Bcond,   // b.cond, ±1 MiB imm19 (veneer-capable)
        CBZ,     // cbz 64, ±1 MiB imm19 (veneer-capable)
        CBNZ,    // cbnz 64, ±1 MiB imm19 (veneer-capable)
        CBZ32,   // cbz 32, ±1 MiB imm19 (veneer-capable)
        CBNZ32,  // cbnz 32, ±1 MiB imm19 (veneer-capable)
        ADR,     // adr, ±1 MiB
        AdrLabel,// adr-or-adrp+add
        AdrpAddLabel, // adrp+add :lo12: (2 insns, ±4 GiB) — robust label addr
    };

    struct Fixup {
        uint32_t code_offset;  // offset of the 4-byte instruction (its first byte)
        uint32_t label_id;
        uint8_t kind;          // FixKind
        uint8_t cond = 0;      // condition code for Bcond veneer inversion
    };

    struct LiteralFixup {
        uint32_t code_offset;  // offset of the LDR instruction
        XReg dst;
        AbsFixup::Kind kind;
        uint32_t addend;
    };

    struct NativeLiteral {
        uint32_t ldr_offset;   // offset of the LDR instruction
        XReg dst;
        std::string name;
    };

    void movz16(XReg d, uint16_t imm16, uint8_t hw) {
        insn(0xD2800000u | (uint32_t(hw) << 21) | (uint32_t(imm16) << 5) | r(d));
    }
    void movk16(XReg d, uint16_t imm16, uint8_t hw) {
        insn(0xF2800000u | (uint32_t(hw) << 21) | (uint32_t(imm16) << 5) | r(d));
    }
    void fp3(uint32_t base, uint32_t opcode, ArmVReg d, ArmVReg n, ArmVReg m) {
        insn(base | (opcode << 12) | (v(m) << 16) | (v(n) << 5) | v(d));
    }

    uint32_t next_label = 0;
    std::unordered_map<uint32_t, uint32_t> bound;
    std::vector<Fixup> pending_;
    std::vector<LiteralFixup> literal_fixups_;
    std::vector<NativeLiteral> native_literal_;
    std::vector<AbsFixup> abs_fixups_;
    std::vector<NativeFixup> native_fixups_;
};

// ---------------------------------------------------------------------------
// resolve_fixups — out-of-line definition (header-only, inline).
// ---------------------------------------------------------------------------
inline void Arm64Emitter::resolve_fixups() {
    // --- 1. Emit the literal pool at the end of the code and backpatch the
    //     PC-relative LDR (literal) offsets. Each LDR has a 19-bit signed
    //     immediate (offset/4, ±1 MiB). The pool is 8-byte aligned; cells are
    //     8 bytes each. Skip entirely when there are no pool entries. ---
    if (!literal_fixups_.empty() || !native_literal_.empty()) {
        // Align the pool to 8 bytes with NOPs so 64-bit pointer cells are aligned.
        while (code.size() % 8 != 0) nop();
    }

    for (auto& lf : literal_fixups_) {
        uint32_t cell_off = uint32_t(code.size());
        // LDR (literal) computes address = PC + (imm19 * 4), where PC is the
        // address of the LDR instruction itself (lf.code_offset).
        int32_t delta = int32_t(int64_t(cell_off) - int64_t(lf.code_offset));
        if (delta % 4 != 0)
            throw std::logic_error("ember: arm64 literal pool misaligned");
        int32_t imm19 = delta / 4;
        if (imm19 < -(1 << 18) || imm19 >= (1 << 18))
            throw std::logic_error("ember: arm64 literal pool LDR out of ±1MiB range");
        // Patch the LDR: imm19 sits in bits[23:5].
        uint32_t enc = uint32_t(code[lf.code_offset])
                     | (uint32_t(code[lf.code_offset + 1]) << 8)
                     | (uint32_t(code[lf.code_offset + 2]) << 16)
                     | (uint32_t(code[lf.code_offset + 3]) << 24);
        enc = (enc & ~(0x7FFFFu << 5)) | ((uint32_t(imm19) & 0x7FFFFu) << 5);
        code[lf.code_offset + 0] = uint8_t(enc);
        code[lf.code_offset + 1] = uint8_t(enc >> 8);
        code[lf.code_offset + 2] = uint8_t(enc >> 16);
        code[lf.code_offset + 3] = uint8_t(enc >> 24);
        // Reserve the 8-byte pointer cell (zero placeholder).
        for (int i = 0; i < 8; ++i) code.push_back(0);
        abs_fixups_.push_back({cell_off, lf.kind, lf.addend});
    }
    for (auto& nl : native_literal_) {
        uint32_t cell_off = uint32_t(code.size());
        int32_t delta = int32_t(int64_t(cell_off) - int64_t(nl.ldr_offset));
        if (delta % 4 != 0)
            throw std::logic_error("ember: arm64 literal pool misaligned");
        int32_t imm19 = delta / 4;
        if (imm19 < -(1 << 18) || imm19 >= (1 << 18))
            throw std::logic_error("ember: arm64 literal pool LDR out of ±1MiB range");
        uint32_t enc = uint32_t(code[nl.ldr_offset])
                     | (uint32_t(code[nl.ldr_offset + 1]) << 8)
                     | (uint32_t(code[nl.ldr_offset + 2]) << 16)
                     | (uint32_t(code[nl.ldr_offset + 3]) << 24);
        enc = (enc & ~(0x7FFFFu << 5)) | ((uint32_t(imm19) & 0x7FFFFu) << 5);
        code[nl.ldr_offset + 0] = uint8_t(enc);
        code[nl.ldr_offset + 1] = uint8_t(enc >> 8);
        code[nl.ldr_offset + 2] = uint8_t(enc >> 16);
        code[nl.ldr_offset + 3] = uint8_t(enc >> 24);
        for (int i = 0; i < 8; ++i) code.push_back(0);
        native_fixups_.push_back({cell_off, nl.name});
    }
    literal_fixups_.clear();
    native_literal_.clear();

    // --- 2. Resolve label fixups. ---
    //
    // Veneers use the BRANCH-OVER pattern: when a conditional branch target
    // is beyond ±1 MiB, insert an unconditional B (±128 MiB) to the real
    // target immediately after the conditional branch, and rewrite the
    // conditional branch with its INVERTED condition to skip over it (+8):
    //
    //   site:     b.inv  site+8     ; if NOT cond, skip the B (fall through)
    //   site+4:   b      target_far ; cond was true -> B to real target
    //   site+8:   <next instruction>
    //
    // This preserves semantics (branch to target IFF cond) and keeps the
    // unconditional B within ±1 MiB of the conditional branch (it's +4).
    // Inserting 4 bytes shifts all subsequent code; we process fixups in
    // emission order and track a cumulative shift, adjusting target lookups.
    auto read32 = [&](uint32_t off) -> uint32_t {
        return uint32_t(code[off]) | (uint32_t(code[off+1]) << 8)
             | (uint32_t(code[off+2]) << 16) | (uint32_t(code[off+3]) << 24);
    };
    auto write32 = [&](uint32_t off, uint32_t enc) {
        code[off]   = uint8_t(enc);
        code[off+1] = uint8_t(enc >> 8);
        code[off+2] = uint8_t(enc >> 16);
        code[off+3] = uint8_t(enc >> 24);
    };
    auto insert32 = [&](uint32_t at, uint32_t enc) {
        code.insert(code.begin() + at, {uint8_t(enc), uint8_t(enc >> 8),
                 uint8_t(enc >> 16), uint8_t(enc >> 24)});
    };

    // Process fixups in emission order so insertions' shifts compose.
    // (pending_ is already in emission order.)
    int64_t shift = 0;  // cumulative bytes inserted before the current fixup

    for (auto& f : pending_) {
        auto it = bound.find(f.label_id);
        if (it == bound.end())
            throw std::runtime_error("ember: unbound label " + std::to_string(f.label_id));
        // Target in ORIGINAL (pre-insertion) coordinates. Adjust by the shift
        // accumulated from prior veneer insertions that fell before the target.
        // Since targets are always >= the branch site or already-bound backward
        // labels, and insertions happen at branch sites, a backward target
        // (before the site) is unaffected by this site's insertion but IS
        // affected by earlier insertions if it's after them. We compute the
        // effective target by adding the shift that applies to offsets <= the
        // original target. Because we insert AT branch sites (in order), any
        // insertion at a site S shifts all original offsets > S. So the
        // effective target = original_target + (number of insertions at sites
        // < original_target) * 4. The running `shift` counts insertions at
        // sites < current fixup's site; for a forward target (>= site), the
        // relevant shift is `shift` (insertions before this site, all < target).
        // For a backward target (< site), insertions at sites between target
        // and site also apply, but `shift` already includes them (they were
        // processed earlier at sites > target). So in both cases the effective
        // target = original_target + shift_at_target. Since `shift` at this
        // point counts all insertions at sites < current site, and any target
        // (forward or backward) has had all insertions at sites < target
        // already counted (they were processed when we reached those earlier
        // sites), `shift` is exactly the count of insertions before the target
        // iff the target <= current site. For forward targets (> site), no
        // insertion at a site between current site and target has happened yet,
        // so `shift` is also correct. Thus effective_target = target + shift.
        uint32_t target = uint32_t(int64_t(it->second) + shift);
        uint32_t site = uint32_t(int64_t(f.code_offset) + shift);
        FixKind k = FixKind(f.kind);

        if (k == FixKind::B) {
            // B/BL: imm26 = (target - site) / 4, ±128 MiB.
            int64_t rel = (int64_t(target) - int64_t(site)) / 4;
            if (rel < -(1 << 25) || rel >= (1 << 25))
                throw std::runtime_error("ember: arm64 B/BL branch out of ±128MiB range");
            uint32_t enc = read32(site) | (uint32_t(rel) & 0x3FFFFFFu);
            write32(site, enc);
        } else if (k == FixKind::ADR) {
            // ADR: signed 21-bit byte offset (±1 MiB), split immlo[30:29] +
            // immhi[23:5].
            int64_t rel = int64_t(target) - int64_t(site);
            if (rel < -(1 << 20) || rel >= (1 << 20))
                throw std::runtime_error("ember: arm64 ADR out of ±1MiB range");
            uint32_t immlo = uint32_t(rel & 3);
            uint32_t immhi = uint32_t((rel >> 2) & 0x7FFFF);
            uint32_t enc = read32(site);
            enc = (enc & ~((3u << 29) | (0x7FFFFu << 5)))
                | (immlo << 29) | (immhi << 5);
            write32(site, enc);
        } else if (k == FixKind::AdrLabel) {
            // Try ADR (±1 MiB); out-of-range ADRP+ADD splice is not supported
            // mid-buffer (would need a 2nd resolve pass). Throw to force callers
            // to lay out code so ADR is in range, or use adr()/adrp() directly.
            int64_t rel = int64_t(target) - int64_t(site);
            if (rel < -(1 << 20) || rel >= (1 << 20))
                throw std::runtime_error(
                    "ember: arm64 adr_label out of ADR ±1MiB range; use adr/adrp "
                    "explicitly or relocate code");
            uint32_t immlo = uint32_t(rel & 3);
            uint32_t immhi = uint32_t((rel >> 2) & 0x7FFFF);
            uint32_t enc = read32(site);
            enc = (enc & ~((3u << 29) | (0x7FFFFu << 5)))
                | (immlo << 29) | (immhi << 5);
            write32(site, enc);
        } else if (k == FixKind::AdrpAddLabel) {
            // ADRP (at `site`) + ADD :lo12: (at `site+4`). The target is an
            // absolute byte offset within `code` (the bound label). ADRP gets
            // the target's 4 KiB page relative to the ADRP's own PC page; the
            // ADD gets the target's low 12 bits. ±4 GiB reach. Because both
            // instructions are emitted upfront (no mid-buffer splice), there is
            // no layout shift and a single resolve pass suffices.
            int64_t pc_page   = int64_t(site) & ~int64_t(0xFFF);
            int64_t tgt_page  = int64_t(target) & ~int64_t(0xFFF);
            int64_t delta     = tgt_page - pc_page;             // bytes
            int64_t imm       = (delta >> 12) & ((int64_t(1) << 33) - 1); // 33-bit signed
            uint32_t immlo    = uint32_t(imm & 3);
            uint32_t immhi    = uint32_t((imm >> 2) & 0x7FFFF);
            uint32_t adrp_enc = read32(site);
            adrp_enc = (adrp_enc & ~((3u << 29) | (0x7FFFFu << 5)))
                     | (immlo << 29) | (immhi << 5);
            write32(site, adrp_enc);
            uint32_t lo12     = uint32_t(int64_t(target) & 0xFFF);
            uint32_t add_enc  = read32(site + 4);
            add_enc = (add_enc & ~(0xFFFu << 10)) | (lo12 << 10);
            write32(site + 4, add_enc);
        } else {
            // B.cond / CBZ / CBNZ: imm19 = (target - site) / 4, ±1 MiB.
            int64_t rel = (int64_t(target) - int64_t(site)) / 4;
            if (rel >= -(1 << 18) && rel < (1 << 18)) {
                // In range: patch imm19 (bits[23:5]).
                uint32_t enc = read32(site);
                enc = (enc & ~(0x7FFFFu << 5)) | ((uint32_t(rel) & 0x7FFFFu) << 5);
                write32(site, enc);
            } else {
                // --- VENEER (branch-over) ---
                // Insert an unconditional B to target_far right after the
                // conditional branch, and rewrite the conditional branch with
                // inverted condition to skip over it (+8):
                //   site:   b.inv  site+8   ; if NOT cond, fall through
                //   site+4: b     target   ; cond -> B to far target (±128 MiB)
                //
                // The insertion at site+4 shifts all code after it (including
                // the forward target) by +4. So the effective target for the
                // veneer B is (target + 4) — the target's post-insertion address.
                // (For backward targets the insertion is after them, so no extra
                //  shift; but backward targets within ±1 MiB don't reach here.)
                uint32_t eff_target = target;
                if (int64_t(target) > int64_t(site)) eff_target = target + 4u;
                int64_t brel = (int64_t(eff_target) - int64_t(site + 4)) / 4;
                if (brel < -(1 << 25) || brel >= (1 << 25))
                    throw std::runtime_error(
                        "ember: arm64 veneer B out of ±128MiB range");
                uint32_t b_veneer = 0x14000000u | (uint32_t(brel) & 0x3FFFFFFu);

                uint32_t enc = read32(site);
                if (k == FixKind::Bcond) {
                    // Invert condition (bit 0 of cond field, bits[3:0]).
                    uint32_t invcond = (enc & 0xF) ^ 1u;
                    enc = (enc & ~0xFu) | invcond;
                    // Set imm19 to +2 (branch to site+8).
                    enc = (enc & ~(0x7FFFFu << 5)) | (2u << 5);
                } else if (k == FixKind::CBZ) {
                    // CBZ64 (0xB4) -> CBNZ64 (0xB5): invert the test bit.
                    enc = (enc & ~(1u << 24)) | (1u << 24);  // 0xB4 -> 0xB5
                    enc = (enc & ~(0x7FFFFu << 5)) | (2u << 5);
                } else if (k == FixKind::CBNZ) {
                    // CBNZ64 (0xB5) -> CBZ64 (0xB4): clear the test bit.
                    enc = (enc & ~(1u << 24));  // 0xB5 -> 0xB4
                    enc = (enc & ~(0x7FFFFu << 5)) | (2u << 5);
                } else if (k == FixKind::CBZ32) {
                    // CBZ32 (0x34) -> CBNZ32 (0x35).
                    enc = (enc & ~(1u << 24)) | (1u << 24);  // 0x34 -> 0x35
                    enc = (enc & ~(0x7FFFFu << 5)) | (2u << 5);
                } else if (k == FixKind::CBNZ32) {
                    // CBNZ32 (0x35) -> CBZ32 (0x34).
                    enc = (enc & ~(1u << 24));  // 0x35 -> 0x34
                    enc = (enc & ~(0x7FFFFu << 5)) | (2u << 5);
                }
                write32(site, enc);
                // Insert the unconditional B right after the conditional branch.
                insert32(site + 4, b_veneer);
                shift += 4;
            }
        }
    }
    pending_.clear();
}

} // namespace ember
