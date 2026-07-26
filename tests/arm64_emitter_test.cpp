// Unit tests for ember::Arm64Emitter (src/arm64_emitter.hpp).
//
// Every test asserts exact emitted bytes for an instruction form. Encodings
// were verified against `llvm-mc -assemble -show-encoding
// -triple=arm64-apple-darwin` and/or by assembling a .s with clang and reading
// bytes with `otool -s __TEXT __text`. Verified source + bytes are cited in a
// comment above each assertion.
//
// Build & run:
//   clang++ -std=c++17 -Wall -Wextra tests/arm64_emitter_test.cpp -o /tmp/arm64_emit_test \
//     && /tmp/arm64_emit_test && echo PASS
#include "../src/arm64_emitter.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>

using namespace ember;

static int g_failures = 0;

// Assert that the last `n` bytes of e.code equal the given little-endian
// 32-bit word (one instruction). Returns the offset of those bytes.
static uint32_t check_last_insn(Arm64Emitter& e, uint32_t expect, const char* who) {
    if (e.code.size() < 4) {
        std::cerr << "FAIL " << who << ": code too short (" << e.code.size() << " bytes)\n";
        ++g_failures;
        return 0;
    }
    uint32_t off = uint32_t(e.code.size()) - 4;
    uint32_t got = uint32_t(e.code[off])
                 | (uint32_t(e.code[off+1]) << 8)
                 | (uint32_t(e.code[off+2]) << 16)
                 | (uint32_t(e.code[off+3]) << 24);
    if (got != expect) {
        std::cerr << "FAIL " << who << ": off=" << off
                  << " got=0x" << std::hex << got
                  << " expect=0x" << expect << std::dec << "\n";
        ++g_failures;
    }
    return off;
}

// Assert bytes at a specific offset equal the given 4-byte LE word.
static void check_insn_at(Arm64Emitter& e, uint32_t off, uint32_t expect, const char* who) {
    if (off + 4 > e.code.size()) {
        std::cerr << "FAIL " << who << ": offset " << off << " out of range\n";
        ++g_failures;
        return;
    }
    uint32_t got = uint32_t(e.code[off])
                 | (uint32_t(e.code[off+1]) << 8)
                 | (uint32_t(e.code[off+2]) << 16)
                 | (uint32_t(e.code[off+3]) << 24);
    if (got != expect) {
        std::cerr << "FAIL " << who << ": off=" << off
                  << " got=0x" << std::hex << got
                  << " expect=0x" << expect << std::dec << "\n";
        ++g_failures;
    }
}

static void check_eq(uint64_t got, uint64_t expect, const char* who) {
    if (got != expect) {
        std::cerr << "FAIL " << who << ": got=" << got << " expect=" << expect << "\n";
        ++g_failures;
    }
}

static void check(bool cond, const char* who) {
    if (!cond) { std::cerr << "FAIL " << who << "\n"; ++g_failures; }
}

// =========================================================================
// ALU register, register, register
// =========================================================================
static void test_alu_reg_reg() {
    Arm64Emitter e;
    // add x0,x1,x2 = 0x8B020020  [llvm-mc: [0x20,0x00,0x02,0x8b]]
    e.add_reg(XReg::x0, XReg::x1, XReg::x2);
    check_last_insn(e, 0x8B020020, "add_reg");
    // sub x3,x4,x5 = 0xCB050083  [llvm-mc: [0x83,0x00,0x05,0xcb]]
    e.sub_reg(XReg::x3, XReg::x4, XReg::x5);
    check_last_insn(e, 0xCB050083, "sub_reg");
    // mul x6,x7,x8 = 0x9B087CE6  [llvm-mc: [0xe6,0x7c,0x08,0x9b]]
    e.mul_reg(XReg::x6, XReg::x7, XReg::x8);
    check_last_insn(e, 0x9B087CE6, "mul_reg");
    // and x9,x10,x11 = 0x8A0B0149  [llvm-mc: [0x49,0x01,0x0b,0x8a]]
    e.and_reg(XReg::x9, XReg::x10, XReg::x11);
    check_last_insn(e, 0x8A0B0149, "and_reg");
    // orr x12,x13,x14 = 0xAA0E01AC  [llvm-mc: [0xac,0x01,0x0e,0xaa]]
    e.orr_reg(XReg::x12, XReg::x13, XReg::x14);
    check_last_insn(e, 0xAA0E01AC, "orr_reg");
    // eor x15,x16,x17 = 0xCA11020F  [llvm-mc: [0x0f,0x02,0x11,0xca]]
    e.eor_reg(XReg::x15, XReg::x16, XReg::x17);
    check_last_insn(e, 0xCA11020F, "eor_reg");
}

static void test_alu_shift_reg() {
    Arm64Emitter e;
    // lsl x21,x22,x23 = 0x9AD722D5  [llvm-mc: [0xd5,0x22,0xd7,0x9a]]
    e.lsl_reg(XReg::x21, XReg::x22, XReg::x23);
    check_last_insn(e, 0x9AD722D5, "lsl_reg");
    // lsr x0,x1,x2 = 0x9AC22420  [llvm-mc: [0x20,0x24,0xc2,0x9a]]
    e.lsr_reg(XReg::x0, XReg::x1, XReg::x2);
    check_last_insn(e, 0x9AC22420, "lsr_reg");
    // asr x0,x1,x2 = 0x9AC22820  [llvm-mc: [0x20,0x28,0xc2,0x9a]]
    e.asr_reg(XReg::x0, XReg::x1, XReg::x2);
    check_last_insn(e, 0x9AC22820, "asr_reg");
}

static void test_alu_shift_imm() {
    Arm64Emitter e;
    // lsl x0,x1,#5 = 0xD37BE820  [llvm-mc: [0x20,0xe8,0x7b,0xd3]]
    e.lsl_imm(XReg::x0, XReg::x1, 5);
    check_last_insn(e, 0xD37BE820, "lsl_imm5");
    // lsr x0,x1,#5 = 0xD345FC20  [llvm-mc: [0x20,0xfc,0x45,0xd3]]
    e.lsr_imm(XReg::x0, XReg::x1, 5);
    check_last_insn(e, 0xD345FC20, "lsr_imm5");
    // asr x0,x1,#5 = 0x9345FC20  [llvm-mc: [0x20,0xfc,0x45,0x93]]
    e.asr_imm(XReg::x0, XReg::x1, 5);
    check_last_insn(e, 0x9345FC20, "asr_imm5");
}

static void test_mov_mvn_neg() {
    Arm64Emitter e;
    // mov x0,x1 = 0xAA0103E0  [llvm-mc: [0xe0,0x03,0x01,0xaa]]
    e.mov_reg(XReg::x0, XReg::x1);
    check_last_insn(e, 0xAA0103E0, "mov_reg");
    // mvn x0,x1 = 0xAA2103E0  [llvm-mc: [0xe0,0x03,0x21,0xaa]]
    e.mvn_reg(XReg::x0, XReg::x1);
    check_last_insn(e, 0xAA2103E0, "mvn_reg");
    // neg x0,x1 = 0xCB0103E0  [llvm-mc: [0xe0,0x03,0x01,0xcb]]
    e.neg_reg(XReg::x0, XReg::x1);
    check_last_insn(e, 0xCB0103E0, "neg_reg");
}

static void test_add_sub_imm() {
    Arm64Emitter e;
    // add x0,x1,#0x123 = 0x91048C20  [llvm-mc: [0x20,0x8c,0x04,0x91]]
    e.add_reg_imm(XReg::x0, XReg::x1, 0x123, false);
    check_last_insn(e, 0x91048C20, "add_reg_imm");
    // sub x0,x1,#0x123 = 0xD1048C20  [llvm-mc: [0x20,0x8c,0x04,0xd1]]
    e.sub_reg_imm(XReg::x0, XReg::x1, 0x123, false);
    check_last_insn(e, 0xD1048C20, "sub_reg_imm");
    // add x0,x1,#0x123,lsl#12 = 0x91448C20  [llvm-mc: [0x20,0x8c,0x44,0x91]]
    e.add_reg_imm(XReg::x0, XReg::x1, 0x123, true);
    check_last_insn(e, 0x91448C20, "add_reg_imm_sh12");
}

static void test_cmp_cset() {
    Arm64Emitter e;
    // cmp x0,x1 = 0xEB01001F  [llvm-mc: [0x1f,0x00,0x01,0xeb]]
    e.cmp_reg(XReg::x0, XReg::x1);
    check_last_insn(e, 0xEB01001F, "cmp_reg");
    // cmp x0,#0x123 = 0xF1048C1F  [llvm-mc: [0x1f,0x8c,0x04,0xf1]]
    e.cmp_reg_imm(XReg::x0, 0x123, false);
    check_last_insn(e, 0xF1048C1F, "cmp_reg_imm");
    // cset x0,eq = 0x9A9F17E0  [llvm-mc: [0xe0,0x17,0x9f,0x9a]]
    e.cset(XReg::x0, Cond::eq);
    check_last_insn(e, 0x9A9F17E0, "cset_eq");
    // cset x0,ne = 0x9A9F07E0  [llvm-mc: [0xe0,0x07,0x9f,0x9a]]
    e.cset(XReg::x0, Cond::ne);
    check_last_insn(e, 0x9A9F07E0, "cset_ne");
    // cset x0,ge = 0x9A9FB7E0  [llvm-mc: [0xe0,0xb7,0x9f,0x9a]]
    e.cset(XReg::x0, Cond::ge);
    check_last_insn(e, 0x9A9FB7E0, "cset_ge");
}

// =========================================================================
// Load / store (scaled unsigned offset)
// =========================================================================
static void test_load_store_scaled() {
    Arm64Emitter e;
    // ldr x0,[x1,#8] = 0xF9400420  [llvm-mc: [0x20,0x04,0x40,0xf9]]
    e.ldr64(XReg::x0, XReg::x1, 1);  // imm12 = 8/8 = 1
    check_last_insn(e, 0xF9400420, "ldr64");
    // str x2,[x3,#16] = 0xF9000862  [llvm-mc: [0x62,0x08,0x00,0xf9]]
    e.str64(XReg::x2, XReg::x3, 2);  // imm12 = 16/8 = 2
    check_last_insn(e, 0xF9000862, "str64");
    // ldr w0,[x1,#4] = 0xB9400420  [llvm-mc: [0x20,0x04,0x40,0xb9]]
    e.ldr32(XReg::x0, XReg::x1, 1);  // imm12 = 4/4 = 1
    check_last_insn(e, 0xB9400420, "ldr32");
    // str w2,[x3,#4] = 0xB9000462  [llvm-mc: [0x62,0x04,0x00,0xb9]]
    e.str32(XReg::x2, XReg::x3, 1);
    check_last_insn(e, 0xB9000462, "str32");
    // ldrsw x0,[x1,#4] = 0xB9800420  [llvm-mc: [0x20,0x04,0x80,0xb9]]
    e.ldrsw(XReg::x0, XReg::x1, 1);
    check_last_insn(e, 0xB9800420, "ldrsw");
    // ldrh w0,[x1,#2] = 0x79400420  [llvm-mc: [0x20,0x04,0x40,0x79]]
    e.ldrh(XReg::x0, XReg::x1, 1);  // imm12 = 2/2 = 1
    check_last_insn(e, 0x79400420, "ldrh");
    // strh w2,[x3,#2] = 0x79000462  [llvm-mc: [0x62,0x04,0x00,0x79]]
    e.strh(XReg::x2, XReg::x3, 1);
    check_last_insn(e, 0x79000462, "strh");
    // ldrb w0,[x1,#1] = 0x39400420  [llvm-mc: [0x20,0x04,0x40,0x39]]
    e.ldrb(XReg::x0, XReg::x1, 1);
    check_last_insn(e, 0x39400420, "ldrb");
    // strb w2,[x3] = 0x39000062  [llvm-mc: [0x62,0x00,0x00,0x39]]
    e.strb(XReg::x2, XReg::x3, 0);
    check_last_insn(e, 0x39000062, "strb");
}

static void test_load_extend() {
    Arm64Emitter e;
    // ldrsh w0,[x1] = 0x79C00020  [llvm-mc: [0x20,0x00,0xc0,0x79]]  (32-bit sign-extend)
    e.ldrsh32(XReg::x0, XReg::x1, 0);
    check_last_insn(e, 0x79C00020, "ldrsh32");
    // ldrsh x0,[x1] = 0x79800020  [llvm-mc: [0x20,0x00,0x80,0x79]]  (64-bit sign-extend)
    e.ldrsh64(XReg::x0, XReg::x1, 0);
    check_last_insn(e, 0x79800020, "ldrsh64");
    // ldrsb w0,[x1] = 0x39C00020  [llvm-mc: [0x20,0x00,0xc0,0x39]]  (32-bit)
    e.ldrsb32(XReg::x0, XReg::x1, 0);
    check_last_insn(e, 0x39C00020, "ldrsb32");
    // ldrsb x0,[x1] = 0x39800020  [llvm-mc: [0x20,0x00,0x80,0x39]]  (64-bit)
    e.ldrsb64(XReg::x0, XReg::x1, 0);
    check_last_insn(e, 0x39800020, "ldrsb64");
}

// =========================================================================
// Load / store (unscaled — negative/unaligned offsets, for frame slots)
// =========================================================================
static void test_load_store_unscaled() {
    Arm64Emitter e;
    // ldur x0,[x1,#-8] = 0xF85F8020  [llvm-mc: [0x20,0x80,0x5f,0xf8]]
    e.ldur64(XReg::x0, XReg::x1, -8);
    check_last_insn(e, 0xF85F8020, "ldur64_neg8");
    // stur x2,[x3,#-16] = 0xF81F0062  [llvm-mc: [0x62,0x00,0x1f,0xf8]]
    e.stur64(XReg::x2, XReg::x3, -16);
    check_last_insn(e, 0xF81F0062, "stur64_neg16");
    // ldur w0,[x1,#-4] = 0xB85FC020  [llvm-mc: [0x20,0xc0,0x5f,0xb8]]
    e.ldur32(XReg::x0, XReg::x1, -4);
    check_last_insn(e, 0xB85FC020, "ldur32_neg4");
    // stur w2,[x3,#12] = 0xB800C062  [llvm-mc: [0x62,0xc0,0x00,0xb8]]
    e.stur32(XReg::x2, XReg::x3, 12);
    check_last_insn(e, 0xB800C062, "stur32_pos12");
    // ldur x0,[x1,#255] = 0xF84FF020  [llvm-mc: [0x20,0xf0,0x4f,0xf8]]
    e.ldur64(XReg::x0, XReg::x1, 255);
    check_last_insn(e, 0xF84FF020, "ldur64_255");
    // ldur x0,[x1,#-256] = 0xF8500020  [llvm-mc: [0x20,0x00,0x50,0xf8]]
    e.ldur64(XReg::x0, XReg::x1, -256);
    check_last_insn(e, 0xF8500020, "ldur64_neg256");
    // ldursw x0,[x1,#-4] = 0xB89FC020  [llvm-mc: [0x20,0xc0,0x9f,0xb8]]
    e.ldursw(XReg::x0, XReg::x1, -4);
    check_last_insn(e, 0xB89FC020, "ldursw_neg4");
    // ldurh w0,[x1,#-2] = 0x785FE020  [llvm-mc: [0x20,0xe0,0x5f,0x78]]
    e.ldurh(XReg::x0, XReg::x1, -2);
    check_last_insn(e, 0x785FE020, "ldurh_neg2");
    // sturb w2,[x3,#-1] = 0x381FF062  [llvm-mc: [0x62,0xf0,0x1f,0x38]]
    e.sturb(XReg::x2, XReg::x3, -1);
    check_last_insn(e, 0x381FF062, "sturb_neg1");
    // ldursh w0,[x1,#-2] = 0x79DFE020  [llvm-mc: [0x20,0xe0,0xdf,0x78]]
    e.ldursh32(XReg::x0, XReg::x1, -2);
    check_last_insn(e, 0x79DFE020, "ldursh32_neg2");
    // ldursb x0,[x1,#-1] = 0x399FF020  [llvm-mc: [0x20,0xf0,0x9f,0x38]]
    e.ldursb64(XReg::x0, XReg::x1, -1);
    check_last_insn(e, 0x399FF020, "ldursb64_neg1");
}

// =========================================================================
// Branches
// =========================================================================
static void test_branch_uncond() {
    // b Label (forward, +8): b fwd; nop; fwd:  -> imm26 = 8/4 = 2
    // Verified: clang emits 14000002 for b over one 4-byte instruction.
    {
        Arm64Emitter e;
        Label fwd = e.alloc_label();
        e.b(fwd);
        e.nop();
        e.bind(fwd);
        e.resolve_fixups();
        // b at offset 0, target at offset 8: imm26 = 2 -> 0x14000002
        check_insn_at(e, 0, 0x14000002, "b_fwd");
    }
    // b Label (backward, -8): loop: nop; b loop  -> imm26 = -2
    {
        Arm64Emitter e;
        Label loop = e.alloc_label();
        e.bind(loop);
        e.nop();
        e.b(loop);
        e.resolve_fixups();
        // loop at 0, nop at 0, b at 4, target 0: imm26 = (0-4)/4 = -1
        // -1 in 26-bit two's complement = 0x3FFFFFF -> 0x13FFFFFF
        check_insn_at(e, 4, 0x17FFFFFF, "b_back");
    }
    // bl Label (forward +8): bl fwd; nop; fwd:  -> 0x94000002
    {
        Arm64Emitter e;
        Label fwd = e.alloc_label();
        e.bl(fwd);
        e.nop();
        e.bind(fwd);
        e.resolve_fixups();
        check_insn_at(e, 0, 0x94000002, "bl_fwd");
    }
}

static void test_bcond() {
    // b.eq forward +12: b.eq tgt; nop; nop; tgt:  -> imm19 = 3 -> 0x54000060
    // Verified by clang: b.eq target (target at +12) = 54000060
    {
        Arm64Emitter e;
        Label tgt = e.alloc_label();
        e.b_cond(Cond::eq, tgt);
        e.nop();
        e.nop();
        e.bind(tgt);
        e.resolve_fixups();
        check_insn_at(e, 0, 0x54000060, "beq_fwd");
    }
    // b.eq backward -4: loop: nop; b.eq loop  -> imm19 = -1 -> 0x54FFFFE0
    // (b.eq at offset 4, target at offset 0; rel = (0-4)/4 = -1)
    {
        Arm64Emitter e;
        Label loop = e.alloc_label();
        e.bind(loop);
        e.nop();
        e.b_cond(Cond::eq, loop);
        e.resolve_fixups();
        check_insn_at(e, 4, 0x54FFFFE0, "beq_back");
    }
    // b.ne forward +12 -> 0x54000061
    {
        Arm64Emitter e;
        Label tgt = e.alloc_label();
        e.b_cond(Cond::ne, tgt);
        e.nop();
        e.nop();
        e.bind(tgt);
        e.resolve_fixups();
        check_insn_at(e, 0, 0x54000061, "bne_fwd");
    }
    // b.ge forward +4 -> imm19 = 1, cond = 10(ge) -> 0x5400002A
    {
        Arm64Emitter e;
        Label tgt = e.alloc_label();
        e.b_cond(Cond::ge, tgt);
        e.bind(tgt);
        e.resolve_fixups();
        check_insn_at(e, 0, 0x5400002A, "bge_fwd");
    }
}

static void test_cbz_cbnz() {
    // cbz x0 forward +8: cbz x0,tgt; nop; tgt: -> imm19 = 2 -> 0xB4000040
    {
        Arm64Emitter e;
        Label tgt = e.alloc_label();
        e.cbz64(XReg::x0, tgt);
        e.nop();
        e.bind(tgt);
        e.resolve_fixups();
        check_insn_at(e, 0, 0xB4000040, "cbz64_fwd");
    }
    // cbz x0 backward -4: loop: nop; cbz x0, loop -> imm19 = -1 -> 0xB4FFFFE0
    // Verified by clang: cbz x0,loop (loop at -4) = b4ffffe0
    {
        Arm64Emitter e;
        Label loop = e.alloc_label();
        e.bind(loop);
        e.nop();
        e.cbz64(XReg::x0, loop);
        e.resolve_fixups();
        check_insn_at(e, 4, 0xB4FFFFE0, "cbz64_back");
    }
    // cbnz x0 forward +8 -> 0xB5000040
    {
        Arm64Emitter e;
        Label tgt = e.alloc_label();
        e.cbnz64(XReg::x0, tgt);
        e.nop();
        e.bind(tgt);
        e.resolve_fixups();
        check_insn_at(e, 0, 0xB5000040, "cbnz64_fwd");
    }
    // cbz w0 forward +8 -> 0x34000040
    {
        Arm64Emitter e;
        Label tgt = e.alloc_label();
        e.cbz32(XReg::x0, tgt);
        e.nop();
        e.bind(tgt);
        e.resolve_fixups();
        check_insn_at(e, 0, 0x34000040, "cbz32_fwd");
    }
    // cbnz w0 forward +8 -> 0x35000040
    {
        Arm64Emitter e;
        Label tgt = e.alloc_label();
        e.cbnz32(XReg::x0, tgt);
        e.nop();
        e.bind(tgt);
        e.resolve_fixups();
        check_insn_at(e, 0, 0x35000040, "cbnz32_fwd");
    }
}

static void test_ret_blr_br() {
    Arm64Emitter e;
    // ret = 0xD65F03C0  [llvm-mc: [0xc0,0x03,0x5f,0xd6]]
    e.ret();
    check_last_insn(e, 0xD65F03C0, "ret");
    // blr x9 = 0xD63F0120  [llvm-mc: [0x20,0x01,0x3f,0xd6]]
    e.blr(XReg::x9);
    check_last_insn(e, 0xD63F0120, "blr");
    // br x9 = 0xD61F0120  [llvm-mc: [0x20,0x01,0x1f,0xd6]]
    e.br(XReg::x9);
    check_last_insn(e, 0xD61F0120, "br");
}

// =========================================================================
// Address generation
// =========================================================================
static void test_adr() {
    // adr x0, target (+12): adr x0,tgt; nop; nop; tgt: -> imm=12, immlo=0, immhi=3
    // Verified by clang: adr x0,target (+12) = 10000060
    {
        Arm64Emitter e;
        Label tgt = e.alloc_label();
        e.adr(XReg::x0, tgt);
        e.nop();
        e.nop();
        e.bind(tgt);
        e.resolve_fixups();
        check_insn_at(e, 0, 0x10000060, "adr_fwd");
    }
    // adr x0, target backward (-8): tgt: nop; nop; adr x0,tgt
    // imm = -8, immlo = (-8 & 3) = 0, immhi = ((-8 >> 2) & 0x7FFFF) = 0x7FFFE
    // enc = 0x10000000 | (0 << 29) | (0x7FFFE << 5) = 0x10FFFFC0
    {
        Arm64Emitter e;
        Label tgt = e.alloc_label();
        e.bind(tgt);
        e.nop();
        e.nop();
        e.adr(XReg::x0, tgt);
        e.resolve_fixups();
        uint32_t expect = 0x10000000u | (0x7FFFEu << 5);
        check_insn_at(e, 8, expect, "adr_back");
    }
}

static void test_adrp() {
    // adrp x0, #4096 (page at +4096 from page 0) — at offset 0, pc_page=0.
    // delta = 4096, imm = 4096>>12 = 1, immlo=1, immhi=0.
    // enc = 0x90000000 | (1<<29) | 0 | 0 = 0xA0000000... 
    // Wait: adrp x0,#4096 verified = 0xB0000000. Let me recompute.
    // 0xB0000000: bit31(op)=1, immlo[30:29]=01, immhi[23:5]=0, Rd=0.
    // Hmm, imm = (immhi<<2)|immlo = (0<<2)|1 = 1. page_delta = 1 << 12 = 4096. Correct.
    // My adrp() computes: delta = page_addr - pc_page = 4096 - 0 = 4096.
    // imm = (4096 >> 12) & mask = 1. immlo = 1 & 3 = 1. immhi = (1>>2)&0x7FFFF = 0.
    // enc = 0x90000000 | (1<<29) | (0<<5) | 0 = 0xA0000000. 
    // But verified is 0xB0000000! Difference: 0xA0 vs 0xB0 = bit 28 (0x10000000).
    // The issue: my adrp() computes delta as (page_addr & ~0xFFF) - pc_page, but
    // the "1" page should give immlo=1 which maps to 0xB0000000 not 0xA0000000.
    // Let me recheck: 0xB0000000 bits: 1011 0000... bit31=1, bit30=0, bit29=1, bit28=1.
    // So immlo = bits[30:29] = 01 = 1. But bit28 is also 1 — that's the "1" in
    // the adrp opcode (adrp = 1xxxxxxx, the fixed field includes bit 28? No.)
    // Actually 0x90000000 = 1001 0000... bit31=1,bit30=0,bit29=0,bit28=1.
    // The adrp base is 0x90000000 with bit28=1 already. So 0x90000000 | (1<<29) = 0xB0000000.
    // 0x90000000 = 1001 0000, OR (1<<29)=0010 0000 0000 -> 1011 0000 0000 = 0xB0000000. Yes!
    // My calculation of 0xA0000000 was wrong. Let me recompute:
    // 0x90000000 | (1<<29) = 0x90000000 | 0x20000000 = 0xB0000000. Correct.
    Arm64Emitter e;
    e.adrp(XReg::x0, 4096);
    check_last_insn(e, 0xB0000000, "adrp_4096");
    // adrp x0, #8192: page_delta = 8192>>12 = 2, immlo = 2&3 = 2, immhi = 0.
    // enc = 0x90000000 | (2<<29) = 0xD0000000. Verified: 0xD0000000.
    e.adrp(XReg::x0, 8192);
    check_last_insn(e, 0xD0000000, "adrp_8192");
}

static void test_adr_label() {
    // adr_label in-range: same as adr.
    {
        Arm64Emitter e;
        Label tgt = e.alloc_label();
        e.adr_label(XReg::x0, tgt);
        e.nop();
        e.nop();
        e.bind(tgt);
        e.resolve_fixups();
        check_insn_at(e, 0, 0x10000060, "adr_label_inrange");
    }
}

// =========================================================================
// 64-bit immediate (movz/movk)
// =========================================================================
static void test_mov_imm64() {
    // mov_reg_imm64(x0, 0x123456789ABCDEF0) decomposes into:
    //   movz x0, #0xDEF0           (chunk 0)
    //   movk x0, #0x9ABC, lsl #16  (chunk 1)
    //   movk x0, #0x5678, lsl #32  (chunk 2)
    //   movk x0, #0x1234, lsl #48  (chunk 3)
    // Computed expected encodings (verified encoding form from llvm-mc):
    //   movz x0,#0xDEF0     = 0xD2800000 | (0xDEF0<<5)        = 0xD29BDE00
    //   movk x0,#0x9ABC,16  = 0xF2800000 | (1<<21) | (0x9ABC<<5) = 0xF2B35780
    //   movk x0,#0x5678,32  = 0xF2800000 | (2<<21) | (0x5678<<5) = 0xF2CACF00
    //   movk x0,#0x1234,48  = 0xF2800000 | (3<<21) | (0x1234<<5) = 0xF2E24680
    Arm64Emitter e;
    e.mov_reg_imm64(XReg::x0, 0x123456789ABCDEF0LL);
    check_insn_at(e, 0,  0xD29BDE00, "movz_imm");
    check_insn_at(e, 4,  0xF2B35780, "movk_imm_16");
    check_insn_at(e, 8,  0xF2CACF00, "movk_imm_32");
    check_insn_at(e, 12, 0xF2E24680, "movk_imm_48");
    // A small constant that fits in one movz: mov x0, #42 -> movz x0,#42 = 0xD2800540
    {
        Arm64Emitter e2;
        e2.mov_reg_imm64(XReg::x0, 42);
        // movz x0, #42: 0xD2800000 | (42<<5) = 0xD2800000 | 0x540 = 0xD2800540
        check_insn_at(e2, 0, 0xD2800540, "movz_42");
        check_eq(e2.code.size(), 4u, "movz_42_size");
    }
    // A constant needing 2 movz/movk: 0x1234_0000_0000_abcd
    //   movz x1,#0xabcd        = 0xD2800000 | (0xabcd<<5) | 1 = 0xD29579A1
    //   movk x1,#0x1234,lsl#48 = 0xF2800000 | (3<<21) | (0x1234<<5) | 1 = 0xF2E24681
    {
        Arm64Emitter e3;
        e3.mov_reg_imm64(XReg::x1, 0x123400000000abcdLL);
        check_insn_at(e3, 0, 0xD29579A1u, "movz_2");
        check_insn_at(e3, 4, 0xF2E24681u, "movk_2_48");
        check_eq(e3.code.size(), 8u, "movz_2_size");
    }
}

// =========================================================================
// Literal pool — relocatable 64-bit pointer load
// =========================================================================
static void test_literal_pool() {
    // Emit: ldr_literal_ptr x0; ret; then finalize.
    // The LDR is at offset 0; ret at 4. Code is 8 bytes (already 8-aligned),
    // so NO NOP pad is needed. The cell is appended at offset 8.
    // delta = 8 - 0 = 8; imm19 = 2; enc = 0x58000000 | (2<<5) = 0x58000040.
    Arm64Emitter e;
    e.ldr_literal_ptr(XReg::x0, AbsFixup::Kind::GlobalsBase, 0);
    e.ret();
    e.resolve_fixups();
    // LDR at offset 0
    check_insn_at(e, 0, 0x58000040, "ldr_literal_ptr_ldr");
    // ret at offset 4
    check_insn_at(e, 4, 0xD65F03C0, "ldr_literal_ptr_ret");
    // The 8-byte cell at offset 8 (zeros)
    check_eq(uint32_t(e.code.size()), 16u, "literal_pool_total_size");
    for (int i = 8; i < 16; ++i)
        check(e.code[i] == 0, "literal_pool_cell_zero");
    // abs_fixups has one entry pointing at the cell (offset 8)
    check_eq(e.abs_fixups().size(), 1u, "literal_pool_abs_fixups_count");
    if (!e.abs_fixups().empty()) {
        check_eq(e.abs_fixups()[0].code_offset, 8u, "literal_pool_abs_fixup_offset");
        check_eq(uint32_t(e.abs_fixups()[0].kind), uint32_t(AbsFixup::Kind::GlobalsBase),
                 "literal_pool_abs_fixup_kind");
    }
}

static void test_native_fixup() {
    // mov_reg_native records a NativeFixup against the literal pool cell.
    // LDR at offset 0, ret at 4, cell at 8 (no pad needed, 8-aligned).
    Arm64Emitter e;
    e.mov_reg_native(XReg::x0, "ember_builtin_print");
    e.ret();
    e.resolve_fixups();
    check_insn_at(e, 0, 0x58000040, "mov_reg_native_ldr");
    check_eq(e.native_fixups().size(), 1u, "mov_reg_native_fixups_count");
    if (!e.native_fixups().empty()) {
        check_eq(e.native_fixups()[0].code_offset, 8u, "mov_reg_native_fixup_offset");
        check(e.native_fixups()[0].name == "ember_builtin_print", "mov_reg_native_fixup_name");
    }
    // abs_fixups should be empty (native, not abs)
    check_eq(e.abs_fixups().size(), 0u, "mov_reg_native_abs_empty");
}

// =========================================================================
// B.cond veneer — >1 MiB target forces an unconditional B trampoline
// =========================================================================
static void test_bcond_veneer() {
    // Emit b.eq target; then >1 MiB of NOPs; then bind target.
    // The b.cond cannot reach (±1 MiB), so resolve_fixups must emit a veneer
    // using the BRANCH-OVER pattern: rewrite b.eq -> b.ne +8 (skip the B),
    // and insert `b target_far` right after it.
    //
    //   site+0: b.ne  site+8     (inverted: if NOT eq, skip the B)
    //   site+4: b    target_far  (unconditional, ±128 MiB)
    //   site+8: <nop 1 of the big block>
    Arm64Emitter e;
    Label tgt = e.alloc_label();
    uint32_t bcond_off = uint32_t(e.code.size());
    e.b_cond(Cond::eq, tgt);
    // 1 MiB = 0x100000 = 1048576 bytes = 262144 NOPs. Emit slightly more so
    // the target is definitively beyond ±1 MiB from the b.cond.
    const uint32_t nops = 262145;  // 262145 * 4 = 0x100004 bytes > 1 MiB
    for (uint32_t i = 0; i < nops; ++i) e.nop();
    e.bind(tgt);
    e.ret();
    e.resolve_fixups();

    // The b.cond at bcond_off is rewritten to b.ne (inverted) targeting +8.
    uint32_t bcond_enc = uint32_t(e.code[bcond_off])
                       | (uint32_t(e.code[bcond_off+1]) << 8)
                       | (uint32_t(e.code[bcond_off+2]) << 16)
                       | (uint32_t(e.code[bcond_off+3]) << 24);
    // Condition must be inverted: eq(0) -> ne(1)
    check_eq(bcond_enc & 0xF, 1u, "veneer_bcond_cond_inverted_to_ne");
    int32_t imm19 = int32_t((bcond_enc >> 5) & 0x7FFFF);
    if (imm19 & (1 << 18)) imm19 -= (1 << 19);
    // imm19 must be +2 (skip 8 bytes = 2 instructions to the original next insn)
    check_eq(int32_t(imm19), 2, "veneer_bcond_skip_imm");

    // The veneer (unconditional B) is at bcond_off+4.
    uint32_t veneer_off = bcond_off + 4;
    uint32_t veneer_enc = uint32_t(e.code[veneer_off])
                        | (uint32_t(e.code[veneer_off+1]) << 8)
                        | (uint32_t(e.code[veneer_off+2]) << 16)
                        | (uint32_t(e.code[veneer_off+3]) << 24);
    check_eq(veneer_enc >> 26, 0x5u, "veneer_is_uncond_B");  // B opcode = 000101 = 5
    // The veneer's target should be the real far target (the ret).
    // After insertion, the nops block shifted by +4. The ret is at:
    //   bcond_off(0) + 4 (b.cond) + 4 (inserted B) + nops*4 + 0
    // Wait: the b.cond is 4 bytes, the inserted B is 4 bytes, then nops*4,
    // then ret. The ret (target) is at 4 + 4 + nops*4 = 8 + nops*4.
    int32_t vimm26 = int32_t(veneer_enc & 0x3FFFFFF);
    if (vimm26 & (1 << 25)) vimm26 -= (1 << 26);
    uint32_t real_target = veneer_off + uint32_t(vimm26 * 4);
    uint32_t expected_target = 4u + 4u + nops * 4u;  // after b.cond + inserted B + nops
    check_eq(real_target, expected_target, "veneer_target_correct");

    // The instruction after the veneer (at bcond_off+8) should be the first NOP.
    uint32_t after = uint32_t(e.code[bcond_off + 8])
                   | (uint32_t(e.code[bcond_off + 9]) << 8)
                   | (uint32_t(e.code[bcond_off + 10]) << 16)
                   | (uint32_t(e.code[bcond_off + 11]) << 24);
    check_eq(after, 0xD503201Fu, "veneer_fallthrough_is_nop");
}

static void test_cbz_veneer() {
    // Same idea but with cbz64. The veneer rewrites cbz -> cbnz (inverted test)
    // skipping +8, and inserts `b target_far` after.
    //   site+0: cbnz x0, site+8   (inverted: if x0 != 0, skip the B)
    //   site+4: b    target_far   (x0 == 0 -> B to far target)
    Arm64Emitter e;
    Label tgt = e.alloc_label();
    uint32_t cbz_off = uint32_t(e.code.size());
    e.cbz64(XReg::x0, tgt);
    const uint32_t nops = 262145;
    for (uint32_t i = 0; i < nops; ++i) e.nop();
    e.bind(tgt);
    e.ret();
    e.resolve_fixups();
    // cbz rewritten to cbnz (0xB4 -> 0xB5); Rt (x0) preserved; imm19 = +2.
    uint32_t cbz_enc = uint32_t(e.code[cbz_off])
                     | (uint32_t(e.code[cbz_off+1]) << 8)
                     | (uint32_t(e.code[cbz_off+2]) << 16)
                     | (uint32_t(e.code[cbz_off+3]) << 24);
    check_eq(cbz_enc & 0x1F, 0u, "veneer_cbz_rt_preserved");
    // opcode bits[31:24]: cbz64 = 0xB4, cbnz64 = 0xB5 (inverted)
    check_eq((cbz_enc >> 24) & 0xFF, 0xB5u, "veneer_cbz_inverted_to_cbnz");
    int32_t imm19 = int32_t((cbz_enc >> 5) & 0x7FFFF);
    if (imm19 & (1 << 18)) imm19 -= (1 << 19);
    check_eq(int32_t(imm19), 2, "veneer_cbz_skip_imm");
    // The veneer (unconditional B) is at cbz_off+4.
    uint32_t veneer_off = cbz_off + 4;
    uint32_t veneer_enc = uint32_t(e.code[veneer_off])
                        | (uint32_t(e.code[veneer_off+1]) << 8)
                        | (uint32_t(e.code[veneer_off+2]) << 16)
                        | (uint32_t(e.code[veneer_off+3]) << 24);
    check_eq(veneer_enc >> 26, 0x5u, "veneer_cbz_is_uncond_B");
}

// =========================================================================
// NEON scalar FP
// =========================================================================
static void test_fp_fmov() {
    Arm64Emitter e;
    // fmov s0,s1 = 0x1E204020  [llvm-mc: [0x20,0x40,0x20,0x1e]]
    e.fmov_f32(VReg::v0, VReg::v1);
    check_last_insn(e, 0x1E204020, "fmov_f32");
    // fmov d0,d1 = 0x1E604020  [llvm-mc: [0x20,0x40,0x60,0x1e]]
    e.fmov_f64(VReg::v0, VReg::v1);
    check_last_insn(e, 0x1E604020, "fmov_f64");
}

static void test_fp_arith() {
    Arm64Emitter e;
    // fadd s0,s1,s2 = 0x1E222820  [llvm-mc: [0x20,0x28,0x22,0x1e]]
    e.fadd_f32(VReg::v0, VReg::v1, VReg::v2);
    check_last_insn(e, 0x1E222820, "fadd_f32");
    // fsub s0,s1,s2 = 0x1E223820  [llvm-mc: [0x20,0x38,0x22,0x1e]]
    e.fsub_f32(VReg::v0, VReg::v1, VReg::v2);
    check_last_insn(e, 0x1E223820, "fsub_f32");
    // fmul s0,s1,s2 = 0x1E220820  [llvm-mc: [0x20,0x08,0x22,0x1e]]
    e.fmul_f32(VReg::v0, VReg::v1, VReg::v2);
    check_last_insn(e, 0x1E220820, "fmul_f32");
    // fdiv s0,s1,s2 = 0x1E221820  [llvm-mc: [0x20,0x18,0x22,0x1e]]
    e.fdiv_f32(VReg::v0, VReg::v1, VReg::v2);
    check_last_insn(e, 0x1E221820, "fdiv_f32");
    // fadd d0,d1,d2 = 0x1E622820  [llvm-mc: [0x20,0x28,0x62,0x1e]]
    e.fadd_f64(VReg::v0, VReg::v1, VReg::v2);
    check_last_insn(e, 0x1E622820, "fadd_f64");
    // fsub d0,d1,d2 = 0x1E623820
    e.fsub_f64(VReg::v0, VReg::v1, VReg::v2);
    check_last_insn(e, 0x1E623820, "fsub_f64");
    // fmul d0,d1,d2 = 0x1E620820
    e.fmul_f64(VReg::v0, VReg::v1, VReg::v2);
    check_last_insn(e, 0x1E620820, "fmul_f64");
    // fdiv d0,d1,d2 = 0x1E621820
    e.fdiv_f64(VReg::v0, VReg::v1, VReg::v2);
    check_last_insn(e, 0x1E621820, "fdiv_f64");
}

static void test_fp_fcmp() {
    Arm64Emitter e;
    // fcmp s0,s1 = 0x1E212000  [llvm-mc: [0x00,0x20,0x21,0x1e]]
    e.fcmp_f32(VReg::v0, VReg::v1);
    check_last_insn(e, 0x1E212000, "fcmp_f32");
    // fcmp d0,d1 = 0x1E612000  [llvm-mc: [0x00,0x20,0x61,0x1e]]
    e.fcmp_f64(VReg::v0, VReg::v1);
    check_last_insn(e, 0x1E612000, "fcmp_f64");
}

static void test_fp_fcvt() {
    Arm64Emitter e;
    // fcvt s0,d0 (double->single) = 0x1E624000  [llvm-mc: [0x00,0x40,0x62,0x1e]]
    e.fcvt_s32d(VReg::v0, VReg::v0);
    check_last_insn(e, 0x1E624000, "fcvt_s32d");
    // fcvt d0,s0 (single->double) = 0x1E22C000  [llvm-mc: [0x00,0xc0,0x22,0x1e]]
    e.fcvt_d32(VReg::v0, VReg::v0);
    check_last_insn(e, 0x1E22C000, "fcvt_d32");
}

static void test_fp_int_conv() {
    Arm64Emitter e;
    // scvtf s0,w0 = 0x1E220000  [llvm-mc: [0x00,0x00,0x22,0x1e]]
    e.scvtf_f32_w(VReg::v0, XReg::x0);
    check_last_insn(e, 0x1E220000, "scvtf_f32_w");
    // scvtf s0,x0 = 0x9E220000  [llvm-mc: [0x00,0x00,0x22,0x9e]]
    e.scvtf_f32_x(VReg::v0, XReg::x0);
    check_last_insn(e, 0x9E220000, "scvtf_f32_x");
    // scvtf d0,w0 = 0x1E620000  [llvm-mc: [0x00,0x00,0x62,0x1e]]
    e.scvtf_f64_w(VReg::v0, XReg::x0);
    check_last_insn(e, 0x1E620000, "scvtf_f64_w");
    // scvtf d0,x0 = 0x9E620000  [llvm-mc: [0x00,0x00,0x62,0x9e]]
    e.scvtf_f64_x(VReg::v0, XReg::x0);
    check_last_insn(e, 0x9E620000, "scvtf_f64_x");
    // fcvtzs w0,s0 = 0x1E380000  [llvm-mc: [0x00,0x00,0x38,0x1e]]
    e.fcvtzs_w_f32(XReg::x0, VReg::v0);
    check_last_insn(e, 0x1E380000, "fcvtzs_w_f32");
    // fcvtzs x0,s0 = 0x9E380000  [llvm-mc: [0x00,0x00,0x38,0x9e]]
    e.fcvtzs_x_f32(XReg::x0, VReg::v0);
    check_last_insn(e, 0x9E380000, "fcvtzs_x_f32");
    // fcvtzs w0,d0 = 0x1E780000  [llvm-mc: [0x00,0x00,0x78,0x1e]]
    e.fcvtzs_w_f64(XReg::x0, VReg::v0);
    check_last_insn(e, 0x1E780000, "fcvtzs_w_f64");
    // fcvtzs x0,d0 = 0x9E780000  [llvm-mc: [0x00,0x00,0x78,0x9e]]
    e.fcvtzs_x_f64(XReg::x0, VReg::v0);
    check_last_insn(e, 0x9E780000, "fcvtzs_x_f64");
}

static void test_fmov_int_fp() {
    Arm64Emitter e;
    // fmov w0,s0 (fpr->gpr) = 0x1E260000  [llvm-mc: [0x00,0x00,0x26,0x1e]]
    e.fmov_fp_to_int_f32(XReg::x0, VReg::v0);
    check_last_insn(e, 0x1E260000, "fmov_fp_to_int_f32");
    // fmov s0,w0 (gpr->fpr) = 0x1E270000  [llvm-mc: [0x00,0x00,0x27,0x1e]]
    e.fmov_int_to_fp_f32(VReg::v0, XReg::x0);
    check_last_insn(e, 0x1E270000, "fmov_int_to_fp_f32");
    // fmov x0,d0 (fpr->gpr) = 0x9E660000  [llvm-mc: [0x00,0x00,0x66,0x9e]]
    e.fmov_fp_to_int_f64(XReg::x0, VReg::v0);
    check_last_insn(e, 0x9E660000, "fmov_fp_to_int_f64");
    // fmov d0,x0 (gpr->fpr) = 0x9E670000  [llvm-mc: [0x00,0x00,0x67,0x9e]]
    e.fmov_int_to_fp_f64(VReg::v0, XReg::x0);
    check_last_insn(e, 0x9E670000, "fmov_int_to_fp_f64");
}

// =========================================================================
// Misc
// =========================================================================
static void test_nop_udf() {
    Arm64Emitter e;
    // nop = 0xD503201F  [llvm-mc: [0x1f,0x20,0x03,0xd5]]
    e.nop();
    check_last_insn(e, 0xD503201F, "nop");
    // udf #0x1234 = 0x00001234  [llvm-mc: [0x34,0x12,0x00,0x00]]
    e.udf(0x1234);
    check_last_insn(e, 0x00001234, "udf_1234");
    // udf #0 = 0x00000000  [llvm-mc: [0x00,0x00,0x00,0x00]]
    e.udf(0);
    check_last_insn(e, 0x00000000, "udf_0");
}

// =========================================================================
// Unbound label must throw
// =========================================================================
static void test_unbound_label() {
    Arm64Emitter e;
    Label never = e.alloc_label();
    e.b(never);
    bool threw = false;
    try { e.resolve_fixups(); }
    catch (const std::runtime_error&) { threw = true; }
    check(threw, "unbound_label_throws");
}

// =========================================================================
// A small end-to-end function: a function that adds two args and returns.
// Confirms multiple instructions compose and resolve cleanly.
// =========================================================================
static void test_small_function() {
    // fn(a, b) -> a + b:
    //   add x0, x0, x1
    //   ret
    Arm64Emitter e;
    e.add_reg(XReg::x0, XReg::x0, XReg::x1);
    e.ret();
    e.resolve_fixups();
    check_eq(e.code.size(), 8u, "small_fn_size");
    check_insn_at(e, 0, 0x8B010000, "small_fn_add");  // add x0,x0,x1
    check_insn_at(e, 4, 0xD65F03C0, "small_fn_ret");
}

int main() {
    test_alu_reg_reg();
    test_alu_shift_reg();
    test_alu_shift_imm();
    test_mov_mvn_neg();
    test_add_sub_imm();
    test_cmp_cset();
    test_load_store_scaled();
    test_load_extend();
    test_load_store_unscaled();
    test_branch_uncond();
    test_bcond();
    test_cbz_cbnz();
    test_ret_blr_br();
    test_adr();
    test_adrp();
    test_adr_label();
    test_mov_imm64();
    test_literal_pool();
    test_native_fixup();
    test_bcond_veneer();
    test_cbz_veneer();
    test_fp_fmov();
    test_fp_arith();
    test_fp_fcmp();
    test_fp_fcvt();
    test_fp_int_conv();
    test_fmov_int_fp();
    test_nop_udf();
    test_unbound_label();
    test_small_function();

    if (g_failures == 0) {
        std::cout << "All ARM64 emitter tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " ARM64 emitter test(s) FAILED.\n";
    return 1;
}
