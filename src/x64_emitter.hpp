// ember x86-64 emitter (v0.1 subset + v0.2 control flow).
// Minimal but real: emits correct Win64 bytes for the instructions
// needed by the v0.1-v0.2 acceptance criteria (docs/spec/CODEGEN_SPEC.md Section 3/Section 4/Section 7/Section 12).
// Grows into the full encoder from docs/spec/CODEGEN_SPEC.md Section 3 incrementally.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace ember {

// x86-64 register encoding numbers (0-15), shared by GP and XMM
// (REX bit logic is identical per docs/spec/CODEGEN_SPEC.md Section 3).
enum class Reg : uint8_t {
    rax = 0, rcx = 1, rdx = 2, rbx = 3, rsp = 4, rbp = 5, rsi = 6, rdi = 7,
    r8  = 8, r9  = 9, r10=10, r11=11, r12=12, r13=13, r14=14, r15=15,
};

// Condition codes for jcc (low nibble of 0F 8x / 7x opcode).
enum class Cond : uint8_t {
    e = 0x4, ne = 0x5,
    l = 0xC, ge = 0xD, le = 0xE, g = 0xF,
    b = 0x2, ae = 0x3, be = 0x6, a = 0x7,
};

// XMM register encoding (0-15); REX bit logic identical to GP regs
// (docs/spec/CODEGEN_SPEC.md Section 3 - register number is always 0-15 regardless of class).
enum class Xmm : uint8_t {
    xmm0=0, xmm1=1, xmm2=2, xmm3=3, xmm4=4, xmm5=5, xmm6=6, xmm7=7,
    xmm8=8, xmm9=9, xmm10=10, xmm11=11, xmm12=12, xmm13=13, xmm14=14, xmm15=15,
};

struct Label { uint32_t id; };

// A pending absolute-imm64 fixup (docs/BUNDLING_AND_EM_MODULES.md Section 2.4). Unlike the
// label fixups (rel32 branch) held in `pending`, this captures a raw 8-byte
// absolute address baked by an imm64 load (mov r, imm64). The `.em` serializer
// records one of these per baked absolute so a loader can repoint it at the
// load-time address of the module's dispatch table or globals block.
// `code_offset` is the byte offset within `code` of the imm64 placeholder
// (8 zero bytes emitted by X64Emitter::mov_reg_imm64_external). resolve_fixups
// does NOT touch these (absolute relocation is deferred to the loader; at JIT
// time the driver fills them just like raw mov_reg_imm64).
struct AbsFixup {
    uint32_t code_offset = 0;   // byte offset within `code` of the imm64 to patch
    enum Kind : uint8_t {
        DispatchTableBase  = 0,  // patch with &module_dispatch_table
        GlobalsBase        = 1,  // patch with &module_globals_block
        ModuleRegistryBase = 2,  // cross-module registry
        FunctionRodataBase = 3,  // patch with function code-end + addend
    } kind = DispatchTableBase;
    uint32_t addend = 0;
};

struct NativeFixup {
    uint32_t code_offset = 0;
    std::string name;
};

class X64Emitter {
public:
    std::vector<uint8_t> code;

    // Win64 stack invariant used by every emitted call: rsp is 16-byte
    // aligned immediately before `call`, and the caller owns at least 32
    // bytes of shadow space.  A function enters with rsp % 16 == 8; the
    // emitter tracks every push/pop and rsp add/sub it emits from there.
    // `win64_call_frame_size` adds only the padding required by the current
    // parity. `require_win64_call_alignment` lets standardized call sites
    // enforce the invariant explicitly.
    uint8_t rsp_mod16() const { return rsp_mod16_; }
    int32_t win64_call_frame_size(int32_t minimum_bytes) const {
        if (minimum_bytes < 32) minimum_bytes = 32;
        const int32_t residue = int32_t(rsp_mod16_);
        return ((minimum_bytes - residue + 15) & ~15) + residue;
    }
    void require_win64_call_alignment() const {
        if (rsp_mod16_ != 0)
            throw std::logic_error("ember: x64 emitter call site is not 16-byte aligned");
    }
    // --- label/patch system (docs/spec/CODEGEN_SPEC.md Section 4) ---
    // Two-pass: branches emit a rel32 placeholder + record a fixup;
    // resolve_fixups() backpatches once all labels are bound.
    Label alloc_label() { return {next_label++}; }
    void bind(Label l) {
        if (bound.find(l.id) != bound.end())
            throw std::logic_error("internal compiler error: duplicate label");
        bound[l.id] = uint32_t(code.size());
    }

    // --- low-level byte emission ---
    void byte(uint8_t b) { code.push_back(b); }
    void bytes(std::initializer_list<uint8_t> bs) { for (auto b : bs) code.push_back(b); }
    void imm32(int32_t v) { for (int i = 0; i < 4; ++i) code.push_back(uint8_t(v >> (8*i))); }
    void imm64(int64_t v) { for (int i = 0; i < 8; ++i) code.push_back(uint8_t(v >> (8*i))); }

    // REX prefix: 0100 W R X B. W=1 for 64-bit operand size.
    static uint8_t rex(bool w, bool r, bool x, bool b) {
        return uint8_t(0x40 | (w?8:0) | (r?4:0) | (x?2:0) | (b?1:0));
    }
    static bool is_extended(Reg r) { return uint8_t(r) >= 8; }

    // ModRM byte: mod(2) reg(3) rm(3).
    static uint8_t modrm(uint8_t mod, Reg reg, Reg rm) {
        return uint8_t((mod << 6) | ((uint8_t(reg) & 7) << 3) | (uint8_t(rm) & 7));
    }

    // --- instructions (v0.1 subset) ---

    // mov r64, r64  -> REX.W 89 /r (mod=11, reg=src, rm=dst)
    void mov_reg_reg(Reg dst, Reg src) {
        byte(rex(true, is_extended(src), false, is_extended(dst)));
        byte(0x89);
        byte(modrm(0b11, src, dst));
        // Ember frames establish rbp only after `push rbp`, so rbp is always
        // 16-byte aligned.  This is the sole non-arithmetic rsp assignment.
        if (dst == Reg::rsp && src == Reg::rbp) rsp_mod16_ = 0;
    }

    // mov r64, imm64 -> REX.W B8+rd io
    void mov_reg_imm64(Reg dst, int64_t imm) {
        byte(rex(true, false, false, is_extended(dst)));
        byte(uint8_t(0xB8 + (uint8_t(dst) & 7)));
        imm64(imm);
    }

    // docs/BUNDLING_AND_EM_MODULES.md Section 2.4 - external-base imm64 load (relocatable).
    // Emits the SAME byte sequence as mov_reg_imm64 (REX.W + 0xB8+reg + 8
    // bytes), but the 8 bytes are zero placeholders and an AbsFixup
    // {imm64_offset, kind} is recorded for the `.em` serializer. At JIT time
    // the driver/codegen fills the placeholder with the real address
    // (identical to today's raw form, byte-for-byte); at `.em`-write time the
    // serializer records the reloc; at `.em`-load time the loader patches it.
    // The existing mov_reg_imm64(dst, imm) with a real immediate is left
    // unchanged for genuinely-constant pointers (native fn ptrs, etc.).
    void mov_reg_imm64_external(Reg dst, AbsFixup::Kind kind, uint32_t addend = 0) {
        byte(rex(true, false, false, is_extended(dst)));
        byte(uint8_t(0xB8 + (uint8_t(dst) & 7)));
        uint32_t imm_off = uint32_t(code.size()); // imm64 placeholder lives here
        for (int i = 0; i < 8; ++i) code.push_back(0); // 8 zero placeholder bytes
        abs_fixups_.push_back({imm_off, kind, addend});
    }

    void mov_reg_native(Reg dst, const std::string& name) {
        byte(rex(true, false, false, is_extended(dst)));
        byte(uint8_t(0xB8 + (uint8_t(dst) & 7)));
        uint32_t imm_off = uint32_t(code.size());
        for (int i = 0; i < 8; ++i) code.push_back(0);
        native_fixups_.push_back({imm_off, name});
    }

    // Read-only view of the absolute-imm64 fixups for the `.em` serializer
    // (docs/BUNDLING_AND_EM_MODULES.md Section 2.4). Not consumed by resolve_fixups.
    const std::vector<AbsFixup>& abs_fixups() const { return abs_fixups_; }
    const std::vector<NativeFixup>& native_fixups() const { return native_fixups_; }

    // add r64, r64  -> REX.W 01 /r (mod=11, reg=src, rm=dst)
    void add_reg_reg(Reg dst, Reg src) {
        byte(rex(true, is_extended(src), false, is_extended(dst)));
        byte(0x01);
        byte(modrm(0b11, src, dst));
    }

    // sub r64, r64  -> REX.W 29 /r
    void sub_reg_reg(Reg dst, Reg src) {
        byte(rex(true, is_extended(src), false, is_extended(dst)));
        byte(0x29);
        byte(modrm(0b11, src, dst));
    }

    // sub r64, imm32 -> REX.W 81 /5 id
    void sub_reg_imm32(Reg r, int32_t imm) {
        byte(rex(true, false, false, is_extended(r)));
        byte(0x81);
        byte(modrm(0b11, Reg(5), r));
        imm32(imm);
        if (r == Reg::rsp) rsp_mod16_ = uint8_t((rsp_mod16_ + 16 - (imm & 15)) & 15);
    }

    // add r64, imm32 -> REX.W 81 /0 id
    void add_reg_imm32(Reg r, int32_t imm) {
        byte(rex(true, false, false, is_extended(r)));
        byte(0x81);
        byte(modrm(0b11, Reg(0), r));
        imm32(imm);
        if (r == Reg::rsp) rsp_mod16_ = uint8_t((rsp_mod16_ + (imm & 15)) & 15);
    }

    // shr r64, imm8 -> REX.W C1 /5 ib (logical shift right)
    void shr_reg_imm8(Reg r, uint8_t imm) {
        byte(rex(true, false, false, is_extended(r)));
        byte(0xC1);
        byte(modrm(0b11, Reg(5), r));
        byte(imm);
    }

    // imul r64, r64 -> REX.W 0F AF /r (dest=reg, reads reg*r/m)
    void imul_reg_reg(Reg dst, Reg src) {
        byte(rex(true, is_extended(dst), false, is_extended(src)));
        byte(0x0F); byte(0xAF);
        byte(modrm(0b11, dst, src));
    }

    // cmp r64, r64  -> REX.W 39 /r
    void cmp_reg_reg(Reg lhs, Reg rhs) {
        byte(rex(true, is_extended(rhs), false, is_extended(lhs)));
        byte(0x39);
        byte(modrm(0b11, rhs, lhs));
    }

    // cmp r64, imm32 -> REX.W 81 /7 id
    void cmp_reg_imm32(Reg r, int32_t imm) {
        byte(rex(true, false, false, is_extended(r)));
        byte(0x81);
        byte(modrm(0b11, Reg(7), r));
        imm32(imm);
    }

    // mov r64, [r64 + disp32]  (load from memory)
    // mov r/m64, r64 form: REX.W 89 /r with mod=10, reg=src, rm=base
    void load_reg_mem(Reg dst, Reg base, int32_t disp) {
        byte(rex(true, is_extended(dst), false, is_extended(base)));
        byte(0x8B);
        if ((uint8_t(base) & 7) == 4) { // rsp/registers-with-rm=4 need SIB
            byte(modrm(0b10, dst, Reg(4)));
            byte(0x24); // SIB: scale=0, index=none(4), base=rsp(4)
        } else {
            byte(modrm(0b10, dst, base));
        }
        imm32(disp);
    }

    // mov [r64 + disp32], r64  (store to memory) - REX.W 89 /r, mod=10, reg=src, rm=base
    // generic counterpart to load_reg_mem; any GP src/base pair.
    void store_reg_mem(Reg base, int32_t disp, Reg src) {
        byte(rex(true, is_extended(src), false, is_extended(base)));
        byte(0x89);
        if ((uint8_t(base) & 7) == 4) { // rsp/r12 need SIB
            byte(modrm(0b10, src, Reg(4)));
            byte(0x24);
        } else {
            byte(modrm(0b10, src, base));
        }
        imm32(disp);
    }

    // --- narrow (sub-8-byte) element load/store, always via rax (array/slice
    // element access with element width < 8 bytes). Loads zero/sign-extend
    // into the full 64-bit rax - this codegen's convention is that a logical
    // int value always lives as a fully-defined 64-bit register, never a
    // partial one, so narrower reads must extend correctly on load.
    void load_elem_to_rax(Reg base, int32_t disp, int width, bool sign_extend) {
        switch (width) {
        case 1:
            byte(rex(true, false, false, is_extended(base)));
            byte(0x0F); byte(sign_extend ? 0xBE : 0xB6);
            break;
        case 2:
            byte(rex(true, false, false, is_extended(base)));
            byte(0x0F); byte(sign_extend ? 0xBF : 0xB7);
            break;
        case 4:
            if (sign_extend) { byte(rex(true, false, false, is_extended(base))); byte(0x63); } // movsxd
            else { if (is_extended(base)) byte(rex(false, false, false, true)); byte(0x8B); }   // mov r32 (zero-extends)
            break;
        default: load_reg_mem(Reg::rax, base, disp); return; // 8 bytes
        }
        if ((uint8_t(base) & 7) == 4) { byte(modrm(0b10, Reg::rax, Reg(4))); byte(0x24); }
        else byte(modrm(0b10, Reg::rax, base));
        imm32(disp);
    }
    // store rax's low `width` bytes to [base+disp].
    void store_rax_elem(Reg base, int32_t disp, int width) {
        switch (width) {
        case 1:
            if (is_extended(base)) byte(rex(false, false, false, true));
            byte(0x88);
            break;
        case 2:
            byte(0x66);
            if (is_extended(base)) byte(rex(false, false, false, true));
            byte(0x89);
            break;
        case 4:
            if (is_extended(base)) byte(rex(false, false, false, true));
            byte(0x89);
            break;
        default: store_reg_mem(base, disp, Reg::rax); return; // 8 bytes
        }
        if ((uint8_t(base) & 7) == 4) { byte(modrm(0b10, Reg::rax, Reg(4))); byte(0x24); }
        else byte(modrm(0b10, Reg::rax, base));
        imm32(disp);
    }

    // push r64 / pop r64
    void push(Reg r) {
        if (is_extended(r)) byte(0x41);
        byte(uint8_t(0x50 + (uint8_t(r) & 7)));
        rsp_mod16_ = uint8_t((rsp_mod16_ + 8) & 15);
    }
    void pop(Reg r) {
        if (is_extended(r)) byte(0x41);
        byte(uint8_t(0x58 + (uint8_t(r) & 7)));
        rsp_mod16_ = uint8_t((rsp_mod16_ + 8) & 15);
    }

    // --- v0.2 control flow (label/patch, docs/spec/CODEGEN_SPEC.md Section 4) ---

    // jmp Label -> E9 rel32 (placeholder, backpatched)
    void jmp(Label l) {
        byte(0xE9);
        pending.push_back({uint32_t(code.size()), l.id, false});
        imm32(0);
    }

    // jcc Label -> 0F 8x rel32
    void jcc(Cond cc, Label l) {
        byte(0x0F); byte(uint8_t(0x80 | uint8_t(cc)));
        pending.push_back({uint32_t(code.size()), l.id, false});
        imm32(0);
    }

    // --- v0.2 calls (docs/spec/CODEGEN_SPEC.md Section 7/Section 8) ---

    // call r/m64 (indirect) -> FF /2 with mod=11, rm=reg
    void call_reg(Reg r) {
        if (is_extended(r)) byte(rex(false, false, false, true));
        byte(0xFF);
        byte(modrm(0b11, Reg(2), r));
    }

    // call [r64 + disp32] (indirect through memory, e.g. dispatch table slot)
    void call_mem(Reg base, int32_t disp) {
        if (is_extended(base)) byte(rex(false, false, false, true));
        byte(0xFF);
        byte(modrm(0b10, Reg(2), base));
        imm32(disp);
    }

    // v1.0 Tier 2 (docs/planning/plan_FUNCTION_REFS.md §5.3/§9.1): lea r64, [base + index*scale]
    // — the indirect-call dispatch uses `lea r11,[r11+rax*8]` to compute the
    // dispatch-table slot address from a runtime handle. Centralized here so the
    // SIB/REX bytes are built in ONE place (the §9.1 highest-risk detail).
    //   REX = W(64-bit addr) | R(dst extended in reg field) | X(index extended)
    //         | B(base extended in SIB base field)
    //   ModRM = mod=00 (no disp) | reg=dst | rm=4 (SIB escape)
    //   SIB = (scale_log2<<6) | (index&7)<<3 | (base&7)   [scale_log2: 0->1,1->2,2->4,3->8]
    void lea_reg_mem_sib(Reg dst, Reg base, Reg index, uint8_t scale_log2) {
        byte(rex(true, is_extended(dst), is_extended(index), is_extended(base)));
        byte(0x8D);                       // lea opcode
        byte(modrm(0b00, dst, Reg(4)));   // rm=4 = SIB follows, mod=00 = no disp
        byte(uint8_t((scale_log2 & 3) << 6) | ((uint8_t(index) & 7) << 3) | (uint8_t(base) & 7));
    }

    // script->native call: mov rax, imm64; call rax
    void call_imm64(int64_t target) {
        mov_reg_imm64(Reg::rax, target);
        call_reg(Reg::rax);
    }

    // ret
    void ret() { byte(0xC3); }

    // nop (pad)
    void nop() { byte(0x90); }

    // --- SSE scalar float (docs/spec/CODEGEN_SPEC.md Section 3) ---
    // movss xmm, xmm  (F3 0F 10 /r, mod=11, reg=dst, rm=src)
    void movss_xmm_xmm(Xmm dst, Xmm src) {
        byte(0xF3);
        byte(rex(false, is_extended(Reg(uint8_t(dst))), false, is_extended(Reg(uint8_t(src)))));
        byte(0x0F); byte(0x10);
        byte(modrm(0b11, Reg(uint8_t(dst)), Reg(uint8_t(src))));
    }
    // movss xmm, [r64 + disp32]  (load)
    void movss_xmm_mem(Xmm dst, Reg base, int32_t disp) {
        byte(0xF3);
        byte(rex(false, is_extended(Reg(uint8_t(dst))), false, is_extended(base)));
        byte(0x0F); byte(0x10);
        if ((uint8_t(base) & 7) == 4) {
            byte(modrm(0b10, Reg(uint8_t(dst)), Reg(4)));
            byte(0x24);
        } else {
            byte(modrm(0b10, Reg(uint8_t(dst)), base));
        }
        imm32(disp);
    }
    // movss [r64 + disp32], xmm  (store)
    void movss_mem_xmm(Reg base, int32_t disp, Xmm src) {
        byte(0xF3);
        byte(rex(false, is_extended(Reg(uint8_t(src))), false, is_extended(base)));
        byte(0x0F); byte(0x11);
        if ((uint8_t(base) & 7) == 4) {
            byte(modrm(0b10, Reg(uint8_t(src)), Reg(4)));
            byte(0x24);
        } else {
            byte(modrm(0b10, Reg(uint8_t(src)), base));
        }
        imm32(disp);
    }
    // scalar double forms use F2 in place of the f32 F3 prefix.
    void movsd_xmm_xmm(Xmm dst, Xmm src) { sse_op_prefix(0xF2, 0x10, dst, src); }
    void movsd_xmm_mem(Xmm dst, Reg base, int32_t disp) { sse_mem(0xF2, 0x10, dst, base, disp); }
    void movsd_mem_xmm(Reg base, int32_t disp, Xmm src) { sse_mem(0xF2, 0x11, src, base, disp); }

    // addss/subss/mulss/divss xmm, xmm  (F3 0F 58/5C/59/5E /r, reg=dst, rm=src)
    void addss_xmm(Xmm dst, Xmm src) { sse_op(0x58, dst, src); }
    void subss_xmm(Xmm dst, Xmm src) { sse_op(0x5C, dst, src); }
    void mulss_xmm(Xmm dst, Xmm src) { sse_op(0x59, dst, src); }
    void divss_xmm(Xmm dst, Xmm src) { sse_op(0x5E, dst, src); }
    void addsd_xmm(Xmm dst, Xmm src) { sse_op_prefix(0xF2, 0x58, dst, src); }
    void subsd_xmm(Xmm dst, Xmm src) { sse_op_prefix(0xF2, 0x5C, dst, src); }
    void mulsd_xmm(Xmm dst, Xmm src) { sse_op_prefix(0xF2, 0x59, dst, src); }
    void divsd_xmm(Xmm dst, Xmm src) { sse_op_prefix(0xF2, 0x5E, dst, src); }
    // ucomiss xmm, xmm  (0F 2E /r, reg=src-for-cmp, rm=dst) - sets ZF/PF/CF
    void ucomiss_xmm(Xmm a, Xmm b) {
        byte(rex(false, is_extended(Reg(uint8_t(a))), false, is_extended(Reg(uint8_t(b)))));
        byte(0x0F); byte(0x2E);
        byte(modrm(0b11, Reg(uint8_t(a)), Reg(uint8_t(b))));
    }
    void ucomisd_xmm(Xmm a, Xmm b) {
        byte(0x66);
        byte(rex(false, is_extended(Reg(uint8_t(a))), false, is_extended(Reg(uint8_t(b)))));
        byte(0x0F); byte(0x2E);
        byte(modrm(0b11, Reg(uint8_t(a)), Reg(uint8_t(b))));
    }
    // pxor xmm, xmm  (zero a float reg)  66 0F EF /r
    void pxor_xmm(Xmm dst, Xmm src) {
        byte(0x66);
        byte(rex(false, is_extended(Reg(uint8_t(dst))), false, is_extended(Reg(uint8_t(src)))));
        byte(0x0F); byte(0xEF);
        byte(modrm(0b11, Reg(uint8_t(dst)), Reg(uint8_t(src))));
    }

private:
    void sse_op_prefix(uint8_t prefix, uint8_t op, Xmm dst, Xmm src) {
        byte(prefix);
        byte(rex(false, is_extended(Reg(uint8_t(dst))), false, is_extended(Reg(uint8_t(src)))));
        byte(0x0F); byte(op);
        byte(modrm(0b11, Reg(uint8_t(dst)), Reg(uint8_t(src))));
    }
    void sse_mem(uint8_t prefix, uint8_t op, Xmm x, Reg base, int32_t disp) {
        byte(prefix);
        byte(rex(false, is_extended(Reg(uint8_t(x))), false, is_extended(base)));
        byte(0x0F); byte(op);
        if ((uint8_t(base) & 7) == 4) { byte(modrm(0b10, Reg(uint8_t(x)), Reg(4))); byte(0x24); }
        else byte(modrm(0b10, Reg(uint8_t(x)), base));
        imm32(disp);
    }
    void sse_op(uint8_t op, Xmm dst, Xmm src) {
        sse_op_prefix(0xF3, op, dst, src);
    }
public:

    // Resolve all pending label fixups. Must be called after all labels
    // are bound. Throws std::runtime_error on unbound label (codegen
    // internal error, docs/spec/CODEGEN_SPEC.md Section 4).
    void resolve_fixups();

    // Read-only view of the resolved label map (label_id -> byte offset) for
    // the Stage-1 post-emit peephole (src/peephole.{hpp,cpp}). Only meaningful
    // after resolve_fixups(). The Stage-1 peepholes are strictly local in-place
    // rewrites padded with trailing NOPs, so they do NOT shift label offsets
    // and do NOT consume this map; it is threaded for the Stage-2 cross-block
    // passes (rel32->rel8 branch shrink) that will need to re-resolve fixups.
    const std::unordered_map<uint32_t, uint32_t>& resolved_labels_view() const { return bound; }

private:
    uint8_t rsp_mod16_ = 8; // Win64 function-entry parity (return address is on stack)
    uint32_t next_label = 0;
    std::unordered_map<uint32_t, uint32_t> bound;
    struct Fixup { uint32_t code_offset; uint32_t label_id; bool is_rel8; };
    std::vector<Fixup> pending;
    std::vector<AbsFixup> abs_fixups_;
    std::vector<NativeFixup> native_fixups_;
};

} // namespace ember
