// thin_emit_arm64.cpp — Phase 4 (plan_MACOS_ARM64.md): the ThinFunction ->
// AArch64 (ARM64) emit pass.
//
// emit_arm64(thf, ctx) consumes a lowered ThinFunction and emits ARM64 bytes
// whose JIT'd EXECUTION is value-equivalent to emit_x64 for the supported
// subset. See thin_emit.hpp for the contract + thin_ir.hpp for the IR.
//
// ARM64 JIT ABI CONTRACT (the emit + the host thunks agree):
//   • The host thunk installs x19 = context_t* and calls the entry with the
//     first script arg in x0 (AAPCS64). The entry is a standard AAPCS64 fn:
//     int/ptr args in x0-x7, float args in v0-v7, return in x0 (int/ptr) / v0
//     (float).
//   • x19 = context_t* (callee-saved; the r14 role on Win64). Reserved — do NOT
//     use as scratch. NEVER use x18 (Apple platform register).
//   • Frame pointer = x29 (FP); link register = x30 (LR). Frame plan offsets
//     are ABSOLUTE frame-pointer-NEGATIVE offsets (same semantics as rbp-
//     negative on x86) — used verbatim with ldur/stur.
//   • Scratch (caller-saved): x9, x10, x11, x12 (+ x0-x8). x9/x10 are the
//     primary scratch pair (the rax/rcx roles).
//   • "rbx role" (callee-saved temp): x20. Saved in the prologue to
//     thf.frame.rbx_save_offset (=-8), restored in the epilogue.
//
// FRAME-ONLY (no regalloc): thf.ra is IGNORED (ra.enabled treated as false).
// Every VReg materializes from its frame slot to x9 (int) / v0 (float); every
// def computes the result in x9 / v0 then stores to the dst frame slot. Scalar
// frame slots are 8 bytes (mirrors emit_x64: store_rax_to_rbp is an 8-byte
// store; narrow ints are normalized in the register via lsl+asr/lsr, then
// stored as 8 bytes, and loaded as 8 bytes + normalized). Narrow PARAM spills
// store the full 8-byte arg reg (upper bits may be garbage); the use-site
// normalize fixes them — exactly as emit_x64's spill_word + normalize_rax.
//
// Unsupported ThinOp variants (float, slice, struct-by-value, try/catch, obf,
// indirect/cross-module calls) throw std::runtime_error("emit_arm64: <op> not
// yet supported") — NEVER silently miscompile.

#include "thin_emit.hpp"
#include "codegen.hpp"    // CodeGenCtx
#include "engine.hpp"     // CompiledFn, CompiledNativeBinding
#include "context.hpp"    // TrapReason, context_offsets
#include "arm64_emitter.hpp"
#include "thin_ir.hpp"
#include "ast.hpp"
#include "aapcs64_classify.hpp"   // classify_aapcs64_arg / classify_aapcs64_return

#include <cassert>
#include <cstring>
#include <cmath>      // std::fmod, std::fmodf (FMod host-native call)
#include <algorithm>
#include <array>
#include <limits>
#include <unordered_map>
#include <cstdio>     // tmp debug dump
#include <cstdlib>    // getenv (tmp debug dump)

namespace ember {

namespace {

// ─── helpers (mirrors of thin_emit.cpp's file-scope helpers) ───

static int int_bits(const Type* t) {
    if (!t) return 64;
    switch (t->prim) {
    case Prim::Bool: case Prim::I8: case Prim::U8: return 8;
    case Prim::I16: case Prim::U16: return 16;
    case Prim::I32: case Prim::U32: return 32;
    default: return 64;
    }
}

static bool is_registered_struct(const Type* t, const StructLayoutTable* structs) {
    return t && !t->struct_name.empty() && structs && structs->count(t->struct_name) != 0;
}

// Byte size of a value type (mirrors codegen.cpp's value_bytes). Used for
// narrow element stores/loads + struct-by-value copy widths.
static int32_t value_bytes(const Type* t, const StructLayoutTable* structs) {
    if (!t) return 8;
    if (t->is_slice || t->is_lambda) return 16;
    if (t->array_len > 0)
        return int32_t(t->array_len) * value_bytes(t->elem.get(), structs);
    if (!t->struct_name.empty() && structs) {
        auto it = structs->find(t->struct_name);
        if (it != structs->end()) return it->second.size;
    }
    switch (t->prim) {
    case Prim::Bool: case Prim::I8: case Prim::U8: return 1;
    case Prim::I16: case Prim::U16: return 2;
    case Prim::I32: case Prim::U32: case Prim::F32: return 4;
    default: return 8;
    }
}

// AAPCS64 argument registers. GP args -> x0-x7 and FP args -> v0-v7 are
// INDEPENDENT streams (a float arg does NOT consume a GP slot). Slice = 2 GP
// words (consecutive x regs). Struct-by-value is unsupported this phase
// (throw). Float args beyond v0-v7 (stack args) throw.
static const XReg kGpArgRegs[8] = {
    XReg::x0, XReg::x1, XReg::x2, XReg::x3,
    XReg::x4, XReg::x5, XReg::x6, XReg::x7,
};
static const ArmVReg kFpArgRegs[8] = {
    ArmVReg::v0, ArmVReg::v1, ArmVReg::v2, ArmVReg::v3,
    ArmVReg::v4, ArmVReg::v5, ArmVReg::v6, ArmVReg::v7,
};

// ─── the emit context ───
struct EmitCtx {
    Arm64Emitter e;
    const ThinFunction& thf;
    const CodeGenCtx& ctx;

    // VReg -> storage info. Frame-only: every used VReg has a frame slot
    // (frame_off != 0). The "live in x9" track is a best-effort fallback for
    // VRegs the lowering left in x9 without a frame slot (rare; a well-formed
    // lowering frame-backs every live value).
    struct VRegInfo {
        int32_t frame_off = 0;   // 0 = not frame-backed
        const Type* type = nullptr;
    };
    std::unordered_map<VReg, VRegInfo> vregs;
    VReg x9_vreg = 0;    // which VReg's int value is currently in x9 (0 = unknown)
    VReg v0_vreg = 0;    // which VReg's float value is currently in v0 (0 = unknown)

    std::vector<Label> block_labels;

    // pending native bindings (for CompiledNativeBinding + JIT-time ptr fill)
    struct PendingNative {
        CompiledNativeBinding binding;
        void* target = nullptr;
    };
    std::vector<PendingNative> pending_natives;

    std::string non_serializable_reason;

    // Precise GC: the compile-time GcFrameMap built from
    // thf.frame.gc_ptr_frame_offs; its ADDRESS is baked into the prologue's
    // frame-record link. The prologue links a GcFrameRecord (in the frame's
    // reserved 24-byte region) onto context_t::gc_frame_head; the epilogue
    // unlinks it. gc_rec_off / gc_rec_base_off / gc_rec_map_off are the
    // x29-relative field offsets (from thf.frame). Mirrors emit_x64
    // (thin_emit.cpp:155-165, 687-727).
    std::shared_ptr<gc::GcFrameMap> gc_map;
    bool gc_active() const { return ctx.use_gc_env; }

    EmitCtx(const ThinFunction& f, const CodeGenCtx& c) : thf(f), ctx(c) {}

    const StructLayoutTable* structs() const { return ctx.structs; }

    // Stage B rebind: resolve a CallNative target by name from ctx.natives.
    void* resolve_native_ptr(const std::string& name) const {
        if (!ctx.natives) return nullptr;
        auto it = ctx.natives->find(name);
        if (it == ctx.natives->end() || !it->second.fn_ptr) return nullptr;
        return it->second.fn_ptr;
    }

    bool vreg_is_float(VReg v) const {
        auto it = vregs.find(v);
        return it != vregs.end() && it->second.type && it->second.type->is_float();
    }
    bool vreg_is_slice(VReg v) const {
        auto it = vregs.find(v);
        return it != vregs.end() && it->second.type &&
               (it->second.type->is_slice || it->second.type->is_lambda);
    }
    const Type* vreg_type(VReg v) const {
        auto it = vregs.find(v);
        return it != vregs.end() ? it->second.type : nullptr;
    }

    // ─── frame access helpers (x29 = FP; offsets are negative) ───
    // ldur/stur cover [-256, 255]. Frame offsets can be more negative; for
    // out-of-range offsets, materialize the slot address in x10 (sub_reg_imm
    // if the magnitude fits 12 bits, else mov_reg_imm64 + sub_reg) and use a
    // zero-offset scaled ldr/str. x10 is the secondary scratch — safe to
    // clobber around a single frame access (callers reload values they need).
    void frame_load64(XReg t, int32_t off) {
        if (off >= -256 && off <= 255) {
            e.ldur64(t, XReg::x29, off);
        } else {
            materialize_frame_addr(XReg::x10, off);
            e.ldr64(t, XReg::x10, 0);
        }
    }
    void frame_store64(XReg t, int32_t off) {
        if (off >= -256 && off <= 255) {
            e.stur64(t, XReg::x29, off);
        } else {
            materialize_frame_addr(XReg::x10, off);
            e.str64(t, XReg::x10, 0);
        }
    }
    // FP frame access — mirrors frame_load64/frame_store64 but uses the
    // ldur_f32/f64 / stur_f32/f64 unscaled loads/stores (verified byte-exact
    // against clang). Out-of-range offsets (beyond ldur's ±256 imm9) are
    // materialized in x10 and accessed at [x10, 0]. x10 is a scratch — safe to
    // clobber around a single FP frame access (the FP operand stays in its
    // ArmVReg, untouched by the address materialization).
    void frame_load_f32(ArmVReg t, int32_t off) {
        if (off >= -256 && off <= 255) e.ldur_f32(t, XReg::x29, off);
        else { materialize_frame_addr(XReg::x10, off); e.ldr_f32(t, XReg::x10, 0); }
    }
    void frame_load_f64(ArmVReg t, int32_t off) {
        if (off >= -256 && off <= 255) e.ldur_f64(t, XReg::x29, off);
        else { materialize_frame_addr(XReg::x10, off); e.ldr_f64(t, XReg::x10, 0); }
    }
    void frame_store_f32(ArmVReg t, int32_t off) {
        if (off >= -256 && off <= 255) e.stur_f32(t, XReg::x29, off);
        else { materialize_frame_addr(XReg::x10, off); e.str_f32(t, XReg::x10, 0); }
    }
    void frame_store_f64(ArmVReg t, int32_t off) {
        if (off >= -256 && off <= 255) e.stur_f64(t, XReg::x29, off);
        else { materialize_frame_addr(XReg::x10, off); e.str_f64(t, XReg::x10, 0); }
    }
    void materialize_frame_addr(XReg dst, int32_t off) {
        // dst = x29 + off (off negative)
        if (off == 0) { e.mov_reg(dst, XReg::x29); return; }
        int32_t mag = -off;  // positive magnitude (off is negative)
        if (mag > 0 && mag <= 0xFFF) {
            e.sub_reg_imm(dst, XReg::x29, uint32_t(mag));
        } else {
            e.mov_reg_imm64(dst, int64_t(off));
            e.add_reg(dst, XReg::x29, dst);
        }
    }

    // ─── narrow element load/store at [base + off] (off any int32) ───
    // Load a narrow int element from [base + off] into x9, sign/zero-extended
    // to 64 bits (mirrors emit_x64's load_elem_to_rax). width = 1/2/4/8.
    // signed = true → sign-extend (ldursb/ldursh/ldursw); false → zero-extend
    // (ldurb/ldurh/ldur32). For width 8 → ldur64. off outside ±256 is
    // materialized in x10 (a scratch — the value stays in x9, untouched).
    void load_elem_x9(XReg base, int32_t off, int32_t width, bool signed_) {
        if (width >= 8) {
            if (off >= -256 && off <= 255) e.ldur64(XReg::x9, base, off);
            else { materialize_off(XReg::x10, base, off); e.ldr64(XReg::x9, XReg::x10, 0); }
        } else if (width == 4) {
            if (off >= -256 && off <= 255) {
                if (signed_) e.ldursw(XReg::x9, base, off);
                else         e.ldur32(XReg::x9, base, off);
            } else {
                materialize_off(XReg::x10, base, off);
                if (signed_) e.ldur32(XReg::x9, XReg::x10, 0);  // ldr w (zero-extend) then sxtw
                else         e.ldr32(XReg::x9, XReg::x10, 0);
                // ldr32 zero-extends; for signed we need sign-extend of the low32.
                // Use ldursw path instead via a temp: simpler to sxtw.
            }
            // For the out-of-range signed-32 case, sign-extend w9->x9.
            if (signed_ && (off < -256 || off > 255)) {
                // x9 = sign-extend of w9: lsl 32 + asr 32
                e.lsl_imm(XReg::x9, XReg::x9, 32);
                e.asr_imm(XReg::x9, XReg::x9, 32);
            }
        } else if (width == 2) {
            if (off >= -256 && off <= 255) {
                if (signed_) e.ldursh64(XReg::x9, base, off);
                else         e.ldurh(XReg::x9, base, off);
            } else {
                materialize_off(XReg::x10, base, off);
                if (signed_) e.ldursh64(XReg::x9, XReg::x10, 0);
                else         e.ldurh(XReg::x9, XReg::x10, 0);
            }
        } else {  // width == 1
            if (off >= -256 && off <= 255) {
                if (signed_) e.ldursb64(XReg::x9, base, off);
                else         e.ldurb(XReg::x9, base, off);
            } else {
                materialize_off(XReg::x10, base, off);
                if (signed_) e.ldursb64(XReg::x9, XReg::x10, 0);
                else         e.ldurb(XReg::x9, XReg::x10, 0);
            }
        }
    }
    // Store x9 (a narrow int) to [base + off] at the given byte width
    // (1/2/4/8). Mirrors emit_x64's store_rax_elem. off outside ±256 is
    // materialized in x10 — but x10 may collide if base == x10; callers pass
    // base != x10 (the base is a frame/global/computed ptr in x11/x29/etc).
    void store_x9_elem(XReg base, int32_t off, int32_t width) {
        if (width >= 8) {
            if (off >= -256 && off <= 255) e.stur64(XReg::x9, base, off);
            else { XReg a = (base == XReg::x10) ? XReg::x12 : XReg::x10;
                   materialize_off(a, base, off); e.str64(XReg::x9, a, 0); }
        } else if (width == 4) {
            if (off >= -256 && off <= 255) e.stur32(XReg::x9, base, off);
            else { XReg a = (base == XReg::x10) ? XReg::x12 : XReg::x10;
                   materialize_off(a, base, off); e.str32(XReg::x9, a, 0); }
        } else if (width == 2) {
            if (off >= -256 && off <= 255) e.sturh(XReg::x9, base, off);
            else { XReg a = (base == XReg::x10) ? XReg::x12 : XReg::x10;
                   materialize_off(a, base, off); e.strh(XReg::x9, a, 0); }
        } else {  // width == 1
            if (off >= -256 && off <= 255) e.sturb(XReg::x9, base, off);
            else { XReg a = (base == XReg::x10) ? XReg::x12 : XReg::x10;
                   materialize_off(a, base, off); e.strb(XReg::x9, a, 0); }
        }
    }
    // dst = base + off (off any int32). Used to materialize a computed element
    // address. dst must differ from base when off is out of the add_reg_imm
    // 12-bit range (the mov_reg_imm64 path writes dst before reading base).
    void materialize_off(XReg dst, XReg base, int32_t off) {
        if (off == 0) { if (dst != base) e.mov_reg(dst, base); return; }
        if (off > 0 && off <= 0xFFF) {
            e.add_reg_imm(dst, base, uint32_t(off));
        } else if (off < 0 && -off <= 0xFFF) {
            e.sub_reg_imm(dst, base, uint32_t(-off));
        } else {
            e.mov_reg_imm64(dst, int64_t(off));
            e.add_reg(dst, base, dst);
        }
    }
    // dst = x29 + (frame_off + field_off). Used by FieldAddr + StructLitInit /
    // ArrayLitInit (packed aggregate field addresses). The combined offset can
    // be more negative than the add_reg_imm 12-bit range; materialize via
    // mov_reg_imm64 + add in that case.
    void materialize_frame_field_addr(XReg dst, int32_t frame_off, int32_t field_off) {
        int32_t addr = frame_off + field_off;
        materialize_frame_addr(dst, addr);
    }
    // memcpy-like forward byte copy from [src_base + src_off] to
    // [dst_base + dst_off], total_bytes bytes. Uses x9 as the chunk transfer
    // reg, x11 as a loop counter/limit scratch. Forward copy (ember struct /
    // array temps are non-overlapping; mirrors emit_x64's copy_bytes). Chunks
    // of 8/4/2/1 like emit_x64. For large copies a small loop is used (>32
    // bytes) to keep code size bounded; small copies are unrolled.
    void copy_bytes(XReg dst_base, int32_t dst_off, XReg src_base, int32_t src_off,
                    int32_t total_bytes) {
        if (total_bytes <= 0) return;
        if (total_bytes <= 32) {
            // unrolled chunk copy
            int32_t done = 0;
            while (done < total_bytes) {
                int32_t remaining = total_bytes - done;
                int chunk = remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
                load_elem_x9(src_base, src_off + done, chunk, /*signed_=*/false);
                store_x9_elem(dst_base, dst_off + done, chunk);
                done += chunk;
            }
            return;
        }
        // loop: copy 8 bytes per iteration, advancing the base pointers.
        // x11 = remaining byte counter. dreg/sreg are the CURRENT dst/src base
        // registers (advanced by 8 each iter via add_reg_imm). For out-of-
        // ldur-range starting offsets, pre-materialize the base pointers into
        // scratch regs (x10/x12) so the loop body uses [reg + 0].
        e.mov_reg_imm64(XReg::x11, int64_t(total_bytes));
        XReg dreg = dst_base, sreg = src_base;
        // Pre-materialize if the starting offset is out of ldur range, so the
        // loop body can use a fixed [reg + 0] and advance the reg itself.
        if (dst_off != 0) {
            XReg scratch = (dst_base == XReg::x10) ? XReg::x12 : XReg::x10;
            materialize_off(scratch, dst_base, dst_off);
            dreg = scratch;
        }
        if (src_off != 0) {
            XReg scratch = (src_base == XReg::x10) ? XReg::x12 : XReg::x10;
            if (dreg == XReg::x10) scratch = (src_base == XReg::x12) ? XReg::x9 : XReg::x12;
            // avoid clobbering dreg's scratch; pick a free one
            if (scratch == dreg) scratch = (dreg == XReg::x12) ? XReg::x10 : XReg::x12;
            materialize_off(scratch, src_base, src_off);
            sreg = scratch;
        }
        Label loop = e.alloc_label(), tail = e.alloc_label();
        e.bind(loop);
        // if (x11 < 8) goto tail
        e.cmp_reg_imm(XReg::x11, 8);
        e.b_cond(ArmCond::cc, tail);   // unsigned < 8
        // copy 8 bytes: x9 = [sreg]; [dreg] = x9
        e.ldur64(XReg::x9, sreg, 0);
        e.stur64(XReg::x9, dreg, 0);
        // advance base pointers by 8
        e.add_reg_imm(sreg, sreg, 8);
        e.add_reg_imm(dreg, dreg, 8);
        e.sub_reg_imm(XReg::x11, XReg::x11, 8);
        e.b(loop);
        e.bind(tail);
        // tail: x11 = remaining bytes (0..7). copy narrow chunks via the
        // (now-advanced) base pointers at [reg + 0].
        Label no4 = e.alloc_label();
        e.cmp_reg_imm(XReg::x11, 4);
        e.b_cond(ArmCond::cc, no4);
        e.ldur32(XReg::x9, sreg, 0);
        e.stur32(XReg::x9, dreg, 0);
        e.add_reg_imm(sreg, sreg, 4);
        e.add_reg_imm(dreg, dreg, 4);
        e.sub_reg_imm(XReg::x11, XReg::x11, 4);
        e.bind(no4);
        Label no2 = e.alloc_label();
        e.cmp_reg_imm(XReg::x11, 2);
        e.b_cond(ArmCond::cc, no2);
        e.ldurh(XReg::x9, sreg, 0);
        e.sturh(XReg::x9, dreg, 0);
        e.add_reg_imm(sreg, sreg, 2);
        e.add_reg_imm(dreg, dreg, 2);
        e.sub_reg_imm(XReg::x11, XReg::x11, 2);
        e.bind(no2);
        Label no1 = e.alloc_label();
        e.cmp_reg_imm(XReg::x11, 1);
        e.b_cond(ArmCond::cc, no1);
        e.ldurb(XReg::x9, sreg, 0);
        e.sturb(XReg::x9, dreg, 0);
        e.bind(no1);
    }

    // ─── normalize x9 to a type's int width (mirrors CG::normalize_rax) ───
    // Narrow ints: shift left (64-bits), then arithmetic (signed) or logical
    // (unsigned) shift right by the same amount. 64-bit + non-int: no-op.
    void normalize_x9(const Type* t) {
        if (!t || !t->is_int() || t->is_fn_handle || !t->struct_name.empty()) return;
        int bits = int_bits(t);
        if (bits >= 64) return;
        uint8_t sh = uint8_t(64 - bits);
        e.lsl_imm(XReg::x9, XReg::x9, sh);
        if (t->is_uint()) e.lsr_imm(XReg::x9, XReg::x9, sh);
        else              e.asr_imm(XReg::x9, XReg::x9, sh);
    }

    // ─── VReg materialization (frame-only) ───
    // Load a scalar int VReg's value into x9. Priority: frame slot (with
    // normalize) > x9_vreg (already in x9) > best-effort trust x9.
    void load_int_vreg(VReg v) {
        auto it = vregs.find(v);
        int32_t off = (it != vregs.end()) ? it->second.frame_off : 0;
        if (off != 0) {
            frame_load64(XReg::x9, off);
            normalize_x9(it != vregs.end() ? it->second.type : vreg_type(v));
            x9_vreg = v;
        } else if (v != 0 && v == x9_vreg) {
            // already in x9
        } else if (v != 0) {
            // no frame slot + not x9_vreg: best-effort trust x9 (well-formed
            // lowering gives such VRegs frame slots).
        }
    }
    // Load a float VReg's value into v0 (the primary FP scratch, the
    // "xmm0" role). f32 slots load via ldur_f32; f64 via ldur_f64. If the
    // VReg is already in v0 (v0_vreg == v), no-op. Best-effort trust v0 if
    // the VReg has no frame slot (well-formed lowering frame-backs floats).
    void load_float_vreg(VReg v) {
        load_float_vreg_into(ArmVReg::v0, v);
    }
    // Load a float VReg's value into a SPECIFIED ArmVReg (e.g. v1 for a
    // binary op's second operand). Updates v0_vreg only when dst == v0.
    void load_float_vreg_into(ArmVReg dst, VReg v) {
        auto it = vregs.find(v);
        if (it != vregs.end() && it->second.frame_off != 0) {
            bool is_f32 = it->second.type && it->second.type->prim == Prim::F32;
            if (is_f32) frame_load_f32(dst, it->second.frame_off);
            else        frame_load_f64(dst, it->second.frame_off);
            if (dst == ArmVReg::v0) v0_vreg = v;
        } else if (v != 0 && dst == ArmVReg::v0 && v == v0_vreg) {
            // already in v0
        } else if (v != 0) {
            // no frame slot + not v0_vreg: best-effort trust the dst reg (a
            // well-formed lowering gives floats frame slots).
        }
    }
    // Materialize an immediate double/float constant into a specified ArmVReg
    // via a GP-reg bit-cast then fmov_int_to_fp (no direct FP-imm load in the
    // emitter; mirrors the VALUE of emit_x64's ConstFloat rodata/movq path).
    void load_float_imm_into(ArmVReg dst, double f, bool is_f32) {
        if (is_f32) {
            float fv = float(f);
            uint32_t bits; std::memcpy(&bits, &fv, 4);
            e.mov_reg_imm64(XReg::x9, int64_t(int32_t(bits)));
            e.fmov_int_to_fp_f32(dst, XReg::x9);   // w9 -> s_dst
        } else {
            uint64_t bits; std::memcpy(&bits, &f, 8);
            e.mov_reg_imm64(XReg::x9, int64_t(bits));
            e.fmov_int_to_fp_f64(dst, XReg::x9);   // x9 -> d_dst
        }
    }
    // Record a dst float VReg's production. The result is in v0. If
    // meta.frame_off is set, store v0 to the frame slot and mark frame-backed.
    void pin_float_dst(VReg dst, const ThinMeta& meta, const Type* ty) {
        if (dst == 0) return;
        int32_t off = meta.frame_off;
        if (off != 0) {
            bool is_f32 = ty && ty->prim == Prim::F32;
            if (is_f32) frame_store_f32(ArmVReg::v0, off);
            else        frame_store_f64(ArmVReg::v0, off);
            vregs[dst].frame_off = off;
        }
        vregs[dst].type = ty;
        v0_vreg = dst;
    }
    // Record a dst float VReg's production where the result is in a SPECIFIED
    // ArmVReg (not v0) — moves it to v0 first, then pins. Used by call results
    // (the float result arrives in v0 from AAPCS64 anyway) and cast paths that
    // land in v0.
    void record_dst_v0(VReg dst, const Type* ty) {
        if (dst == 0) return;
        vregs[dst].type = ty;
        v0_vreg = dst;
    }
    // Load a slice VReg's {ptr, len} into {x0, x1} (AAPCS64 two-word; mirrors
    // the {rax, rdx} slice ABI on x86). The slice VReg v is the ptr VReg;
    // v+1 is the len VReg. If frame-backed, load from [x29+off] and
    // [x29+off+8]. Uses x0/x1 (the AAPCS64 slice arg pair). A variant loads
    // into a SPECIFIED pair (for marshaling into consecutive GP arg regs).
    void load_slice_vreg(VReg v) {
        load_slice_vreg_into(XReg::x0, XReg::x1, v);
    }
    void load_slice_vreg_into(XReg ptr_reg, XReg len_reg, VReg v) {
        auto it = vregs.find(v);
        if (it != vregs.end() && it->second.frame_off != 0) {
            frame_load64(ptr_reg, it->second.frame_off);
            frame_load64(len_reg, it->second.frame_off + 8);
        }
        // else: assume ptr_reg/len_reg already hold {ptr, len} (best-effort).
    }
    // Record a dst slice VReg's production. The result is {ptr_reg, len_reg}
    // (default x0/x1). If meta.frame_off is set, store both words to the dst's
    // frame slot (ptr at off, len at off+8) and mark the slice + companion len
    // VReg frame-backed. Mirrors emit_x64's slice record_dst path.
    void pin_slice_dst(VReg dst, const ThinMeta& meta, const Type* ty,
                       XReg ptr_reg = XReg::x0, XReg len_reg = XReg::x1) {
        if (dst == 0) return;
        vregs[dst].type = ty;
        int32_t off = meta.frame_off;
        if (off != 0) {
            frame_store64(ptr_reg, off);
            frame_store64(len_reg, off + 8);
            vregs[dst].frame_off = off;
            vregs[dst + 1].frame_off = off + 8;
            vregs[dst + 1].type = ty;
        }
    }

    // Record a dst int VReg's production. The result is in x9. If meta.frame_off
    // is set, normalize + store to the frame slot and mark frame-backed.
    void pin_int_dst(VReg dst, const ThinMeta& meta, const Type* ty) {
        if (dst == 0) return;
        int32_t off = meta.frame_off;
        if (off != 0) {
            normalize_x9(ty);
            frame_store64(XReg::x9, off);
            vregs[dst].frame_off = off;
        }
        vregs[dst].type = ty;
        x9_vreg = dst;
    }
    // Record a dst without a frame store (result left in x9). Used when the
    // lowering does not assign a frame slot (rare).
    void record_dst_x9(VReg dst, const Type* ty) {
        if (dst == 0) return;
        vregs[dst].type = ty;
        x9_vreg = dst;
    }

    // ─── trap / safety guards ───
    // emit_trap: if ctx.trap_stub is set, marshal (ctx, reason, detail) into
    // x0/x1/x2 and blr the stub; else emit udf (the hard-fault fallback). The
    // stub never returns (it longjmps); the udf path traps in place.
    //
    // AAPCS64: args in x0-x7, no shadow space needed. The stub is a C fn
    // `void(*)(context_t*, TrapReason, const char*)`. ctx is x19 when
    // use_context_reg, else the baked ctx.trap_ctx. detail is a string literal
    // address (mov_reg_imm64).
    void emit_trap(int reason_ord, const char* detail) {
        if (ctx.trap_stub) {
            if (non_serializable_reason.empty())
                non_serializable_reason = "trap stub/context/detail pointers require a host runtime binding";
            // x0 = ctx (x19 if use_context_reg, else baked trap_ctx)
            if (ctx.use_context_reg) {
                e.mov_reg(XReg::x0, XReg::x19);
            } else {
                e.mov_reg_imm64(XReg::x0, int64_t(ctx.trap_ctx));
            }
            // x1 = reason ordinal
            e.mov_reg_imm64(XReg::x1, int64_t(reason_ord));
            // x2 = detail string literal address
            e.mov_reg_imm64(XReg::x2, int64_t(reinterpret_cast<uintptr_t>(detail)));
            // x11 = trap_stub fn ptr (relocatable-by-name not needed; bake the ptr)
            e.mov_reg_imm64(XReg::x11, int64_t(ctx.trap_stub));
            e.blr(XReg::x11);
            // stub does not return; emit udf as a safety net
            e.udf(0);
        } else {
            e.udf(uint16_t(reason_ord & 0xFFFF));
        }
    }

    // Budget check (mirrors emit_x64's emit_budget_check, ARM64 + [x19+off]).
    // Compare-then-subtract so a very negative counter cannot wrap positive.
    void emit_budget_check(int64_t body_cost, const char* detail) {
        if (!ctx.emit_budget_checks || body_cost <= 0) return;
        const int32_t encoded_cost = body_cost > std::numeric_limits<int32_t>::max()
            ? std::numeric_limits<int32_t>::max() : int32_t(body_cost);
        if (!ctx.use_context_reg && !ctx.budget_ptr) return;
        if (!ctx.use_context_reg && non_serializable_reason.empty())
            non_serializable_reason = "instruction-budget storage is process-local";
        // x9 = budget_remaining
        if (ctx.use_context_reg) {
            int32_t off = context_offsets::budget();
            if (off >= -256 && off <= 255) e.ldur64(XReg::x9, XReg::x19, off);
            else { materialize_ctx_addr(XReg::x9, off); e.ldr64(XReg::x9, XReg::x9, 0); }
        } else {
            e.mov_reg_imm64(XReg::x11, int64_t(ctx.budget_ptr));
            e.ldr64(XReg::x9, XReg::x11, 0);
        }
        Label cont = e.alloc_label(), trap = e.alloc_label();
        // if (budget <= 0) trap
        e.cmp_reg_imm(XReg::x9, 0);
        e.b_cond(ArmCond::le, trap);
        // if (budget <= cost) trap  (unsigned cmp after positivity: use ls)
        e.cmp_reg_imm(XReg::x9, uint32_t(encoded_cost));
        e.b_cond(ArmCond::ls, trap);
        // budget -= cost
        e.sub_reg_imm(XReg::x9, XReg::x9, uint32_t(encoded_cost));
        if (ctx.use_context_reg) {
            int32_t off = context_offsets::budget();
            if (off >= -256 && off <= 255) e.stur64(XReg::x9, XReg::x19, off);
            else { materialize_ctx_addr(XReg::x11, off); e.str64(XReg::x9, XReg::x11, 0); }
        } else {
            e.mov_reg_imm64(XReg::x11, int64_t(ctx.budget_ptr));
            e.str64(XReg::x9, XReg::x11, 0);
        }
        e.b(cont);
        e.bind(trap);
        emit_trap(int(TrapReason::BudgetExceeded), detail);
        e.bind(cont);
    }

    // Depth check (mirrors emit_x64's emit_depth_check). 32-bit counter.
    void emit_depth_check() {
        if (!ctx.emit_depth_checks) return;
        if (!ctx.use_context_reg && !ctx.depth_ptr) return;
        if (!ctx.use_context_reg && non_serializable_reason.empty())
            non_serializable_reason = "call-depth storage is process-local";
        // w9 = call_depth; w10 = max_call_depth - 1
        if (ctx.use_context_reg) {
            int32_t doff = context_offsets::depth();
            int32_t moff = context_offsets::max_depth();
            if (doff >= -256 && doff <= 255) e.ldur32(XReg::x9, XReg::x19, doff);
            else { materialize_ctx_addr(XReg::x10, doff); e.ldr32(XReg::x9, XReg::x10, 0); }
            if (moff >= -256 && moff <= 255) e.ldur32(XReg::x10, XReg::x19, moff);
            else { materialize_ctx_addr(XReg::x11, moff); e.ldr32(XReg::x10, XReg::x11, 0); }
        } else {
            e.mov_reg_imm64(XReg::x11, int64_t(ctx.depth_ptr));
            e.ldr32(XReg::x9, XReg::x11, 0);
            e.mov_reg_imm64(XReg::x10, int64_t(ctx.max_call_depth));
        }
        e.sub_reg_imm(XReg::x10, XReg::x10, 1);
        // if (depth < 0) trap (corruption)
        e.cmp_reg_imm(XReg::x9, 0);
        Label ok = e.alloc_label(), trap = e.alloc_label(), after = e.alloc_label();
        e.b_cond(ArmCond::lt, trap);
        // if (depth >= max-1) trap
        e.cmp_reg(XReg::x9, XReg::x10);
        e.b_cond(ArmCond::lt, ok);
        e.b(trap);
        e.bind(ok);
        // depth += 1
        e.add_reg_imm(XReg::x9, XReg::x9, 1);
        if (ctx.use_context_reg) {
            int32_t doff = context_offsets::depth();
            if (doff >= -256 && doff <= 255) e.stur32(XReg::x9, XReg::x19, doff);
            else { materialize_ctx_addr(XReg::x11, doff); e.str32(XReg::x9, XReg::x11, 0); }
        } else {
            e.mov_reg_imm64(XReg::x11, int64_t(ctx.depth_ptr));
            e.str32(XReg::x9, XReg::x11, 0);
        }
        e.b(after);
        e.bind(trap);
        emit_trap(int(TrapReason::StackOverflow), "stack overflow: call depth exceeded");
        e.bind(after);
    }
    // Decrement call_depth after a normal call return (mirrors emit_depth_leave).
    void emit_depth_leave() {
        if (!ctx.emit_depth_checks) return;
        if (!ctx.use_context_reg && !ctx.depth_ptr) return;
        if (ctx.use_context_reg) {
            int32_t doff = context_offsets::depth();
            if (doff >= -256 && doff <= 255) {
                e.ldur32(XReg::x9, XReg::x19, doff);
                e.sub_reg_imm(XReg::x9, XReg::x9, 1);
                e.stur32(XReg::x9, XReg::x19, doff);
            } else {
                materialize_ctx_addr(XReg::x11, doff);
                e.ldr32(XReg::x9, XReg::x11, 0);
                e.sub_reg_imm(XReg::x9, XReg::x9, 1);
                e.str32(XReg::x9, XReg::x11, 0);
            }
        } else {
            e.mov_reg_imm64(XReg::x11, int64_t(ctx.depth_ptr));
            e.ldr32(XReg::x9, XReg::x11, 0);
            e.sub_reg_imm(XReg::x9, XReg::x9, 1);
            e.str32(XReg::x9, XReg::x11, 0);
        }
    }

    // CallTargetGuard (function-ref provenance). The handle is in x9 (loaded by
    // the caller — for an indirect call the handle vreg's ConstInt/LoadFrame
    // leaves it in x9 immediately before this guard). Validates it against the
    // allowlist bitset before dispatch.
    //
    // v1.0 Tier 2 cross-module handles (plan_MACOS_ARM64.md Phase 8): if the
    // per-module records table is configured, a handle with bit 63 set is a
    // cross-module handle — it is NOT validated here (this is the INTRA-module
    // allowlist). The cross-module validation + dispatch happens at the call
    // site (emit_indirect_call, below) which reads the target module's own
    // allowlist from the records table. Skip the intra range / bit checks for a
    // cross-module handle so a valid `&mod::fn` (a huge value with bit 63 set)
    // does not wrongly fail THIS module's range check. Mirrors the tree-walker's
    // emit_call_target_guard cross_aware path (codegen.cpp). When the records
    // table is NOT configured, no bit-63 test is emitted — a cross-module handle
    // (huge) fails the intra range check below and traps, which is correct.
    void emit_call_target_guard() {
        if (ctx.fn_slot_count <= 0 || ctx.fn_allowlist_base == 0) return;
        if (non_serializable_reason.empty())
            non_serializable_reason = "function-reference allowlist storage is process-local";
        Label trap = e.alloc_label();
        // v1.0 Tier 2: if the records table is configured, test bit 63 of the
        // handle. A cross-module handle (bit 63 set) skips the intra guard —
        // the cross-module validation + dispatch is emit_indirect_call's job.
        // lsr x10, x9, 63 ; cbnz x10, cross_skip  (bit 63 -> 1 -> cross).
        const bool cross_aware = (ctx.module_handle_records_base != 0);
        Label cross_skip;
        if (cross_aware) {
            cross_skip = e.alloc_label();
            e.lsr_imm(XReg::x10, XReg::x9, 63);
            e.cbnz64(XReg::x10, cross_skip);
        }
        // if (handle >= fn_slot_count) trap  (unsigned)
        e.mov_reg_imm64(XReg::x10, int64_t(ctx.fn_slot_count));
        e.cmp_reg(XReg::x9, XReg::x10);
        e.b_cond(ArmCond::cs, trap);  // handle >= fn_slot_count -> out of range (cs = unsigned >=, same as hs)
        // x11 = allowlist_base + (handle >> 3)
        e.mov_reg_imm64(XReg::x11, ctx.fn_allowlist_base);
        e.mov_reg(XReg::x12, XReg::x9);
        e.lsr_imm(XReg::x12, XReg::x12, 3);
        e.add_reg(XReg::x11, XReg::x11, XReg::x12);
        // x10 = handle & 7  (low 3 bits). ARM64 and-imm isn't exposed by the
        // emitter, so synthesize via lsl+lsr (drop high bits, then zero-extend
        // the low 3 back).
        e.mov_reg(XReg::x10, XReg::x9);
        e.lsl_imm(XReg::x10, XReg::x10, 61);
        e.lsr_imm(XReg::x10, XReg::x10, 61);
        // test bit: x12 = [x11]; (x12 >> x10) & 1
        e.ldurb(XReg::x12, XReg::x11, 0);
        e.lsr_reg(XReg::x12, XReg::x12, XReg::x10);
        // isolate bit 0: (byte >> bit) & 1. Without this mask a set HIGHER bit
        // in the same byte would make cbz skip the trap + authorize a slot
        // whose own bit is clear (call-target-provenance bypass). lsl 63 then
        // lsr 63 keeps only bit 0 (the same idiom used above for & 7).
        e.lsl_imm(XReg::x12, XReg::x12, 63);
        e.lsr_imm(XReg::x12, XReg::x12, 63);
        // if bit == 0 -> trap (cbz x12)
        e.cbz64(XReg::x12, trap);
        Label after = e.alloc_label();
        e.b(after);
        e.bind(trap);
        emit_trap(int(TrapReason::BadCallTarget),
                  "call-target provenance: handle is not a registered function");
        e.bind(after);
        if (cross_aware)
            e.bind(cross_skip);  // cross handle's skip target lands here (past the intra guard)
    }

    // Bounds check (index < len). idx in x9, len in x10 (reg) or imm.
    void emit_bounds_check_reg(XReg idx_reg, XReg len_reg) {
        e.cmp_reg(idx_reg, len_reg);
        Label ok = e.alloc_label();
        e.b_cond(ArmCond::cc, ok);
        emit_trap(int(TrapReason::BoundsCheck), "bounds check: index out of range");
        e.bind(ok);
    }
    void emit_bounds_check_imm(XReg idx_reg, int64_t len) {
        // cmp idx, #len (len must fit imm12; else materialize). Use unsigned.
        if (len >= 0 && len <= 0xFFF) {
            e.cmp_reg_imm(idx_reg, uint32_t(len));
        } else {
            e.mov_reg_imm64(XReg::x10, len);
            e.cmp_reg(idx_reg, XReg::x10);
        }
        Label ok = e.alloc_label();
        e.b_cond(ArmCond::cc, ok);
        emit_trap(int(TrapReason::BoundsCheck), "bounds check: index out of range");
        e.bind(ok);
    }

    // Integer div/mod with div-by-zero + signed-overflow guards (mirrors
    // emit_x64's emit_integer_divmod). On entry: x9 = dividend, x10 = divisor.
    // ARM64 has sdiv/udiv + msub (Mod = dividend - (div*quotient)). The full
    // implementation is emit_int_divmod_instr (operand load + guards + divide
    // + msub for mod).

    // materialize a context_t field address into dst (for out-of-ldur-range
    // offsets). dst = x19 + off.
    void materialize_ctx_addr(XReg dst, int32_t off) {
        if (off == 0) { e.mov_reg(dst, XReg::x19); return; }
        if (off > 0 && off <= 0xFFF) {
            e.add_reg_imm(dst, XReg::x19, uint32_t(off));
        } else if (off < 0 && -off <= 0xFFF) {
            e.sub_reg_imm(dst, XReg::x19, uint32_t(-off));
        } else {
            e.mov_reg_imm64(dst, int64_t(off));
            e.add_reg(dst, XReg::x19, dst);
        }
    }
    // Load/store a context_t field (32 or 64 bit) via [x19 + off], handling
    // offsets outside the ldur/stur imm9 range (±256) by materializing the addr.
    // dst is a scratch (x9/x10/x11) — not x19. plan_MACOS_ARM64.md Phase 5.
    void ld_ctx64(XReg dst, int32_t off) {
        if (off >= -256 && off <= 255) e.ldur64(dst, XReg::x19, off);
        else { materialize_ctx_addr(dst, off); e.ldr64(dst, dst, 0); }
    }
    void ld_ctx32(XReg dst, int32_t off) {
        if (off >= -256 && off <= 255) e.ldur32(dst, XReg::x19, off);
        else { materialize_ctx_addr(dst, off); e.ldr32(dst, dst, 0); }
    }
    void st_ctx64(XReg src, int32_t off) {
        if (off >= -256 && off <= 255) e.stur64(src, XReg::x19, off);
        else { XReg a = (src == XReg::x11) ? XReg::x12 : XReg::x11;
               materialize_ctx_addr(a, off); e.str64(src, a, 0); }
    }
    void st_ctx32(XReg src, int32_t off) {
        if (off >= -256 && off <= 255) e.stur32(src, XReg::x19, off);
        else { XReg a = (src == XReg::x11) ? XReg::x12 : XReg::x11;
               materialize_ctx_addr(a, off); e.str32(src, a, 0); }
    }

    // ─── prologue / epilogue ───
    // AAPCS64: stp x29,x30,[sp,-16]! ; mov x29,sp ; sub sp,sp,frame_size ;
    // stur x20,[x29,rbx_save_offset]. frame_size is round16 so sp stays
    // 16-byte aligned. x19 (ctx) is callee-saved + installed by the thunk; we
    // do NOT clobber it, so we do not save it (preserved implicitly).
    //
    // Precise GC: shadow-stack frame-record maintenance. The prologue links a
    // GcFrameRecord (in the frame's reserved 24-byte region at gc_rec_off)
    // onto context_t::gc_frame_head; the epilogue unlinks it. The collector
    // walks the chain from the head. Emitted only when gc_active() (use_gc_env)
    // AND a record region was reserved (gc_rec_off != 0) AND a gc_map was built.
    // The head is addressed via [x19 + off] when use_context_reg, else via the
    // baked gc_frame_head_ptr. CRITICAL: the epilogue must NOT clobber x0 (the
    // i64 return value) — uses x10 as the scratch (volatile, not x0). Mirrors
    // emit_x64 (thin_emit.cpp:692-727).
    void emit_load_gc_head(XReg dst) {
        if (ctx.use_context_reg) {
            int32_t off = context_offsets::gc_frame_head();
            if (off >= -256 && off <= 255) e.ldur64(dst, XReg::x19, off);
            else { materialize_off(dst, XReg::x19, off); e.ldr64(dst, dst, 0); }
        } else {
            e.mov_reg_imm64(dst, reinterpret_cast<int64_t>(ctx.gc_frame_head_ptr));
            e.ldr64(dst, dst, 0);
        }
    }
    void emit_store_gc_head(XReg src) {
        if (ctx.use_context_reg) {
            int32_t off = context_offsets::gc_frame_head();
            if (off >= -256 && off <= 255) e.stur64(src, XReg::x19, off);
            else { materialize_off(XReg::x10, XReg::x19, off); e.str64(src, XReg::x10, 0); }
        } else {
            XReg scratch = (src == XReg::x10) ? XReg::x11 : XReg::x10;
            e.mov_reg_imm64(scratch, reinterpret_cast<int64_t>(ctx.gc_frame_head_ptr));
            e.str64(src, scratch, 0);
        }
    }
    void emit_gc_frame_record_prologue() {
        if (!gc_active() || thf.frame.gc_rec_off == 0 || !gc_map) return;
        // prev = head  (store the current head into [x29 + gc_rec_off])
        emit_load_gc_head(XReg::x9);
        frame_store64(XReg::x9, thf.frame.gc_rec_off);
        // frame_base = x29  (store the frame pointer into [x29 + gc_rec_base_off])
        frame_store64(XReg::x29, thf.frame.gc_rec_base_off);
        // map = gc_map.get()  (store the map ptr into [x29 + gc_rec_map_off])
        e.mov_reg_imm64(XReg::x9, reinterpret_cast<int64_t>(gc_map.get()));
        frame_store64(XReg::x9, thf.frame.gc_rec_map_off);
        // head = &record  (x29 + gc_rec_off)
        materialize_frame_addr(XReg::x9, thf.frame.gc_rec_off);
        emit_store_gc_head(XReg::x9);
    }
    void emit_gc_frame_record_epilogue() {
        if (!gc_active() || thf.frame.gc_rec_off == 0 || !gc_map) return;
        // head = prev  (load prev from [x29 + gc_rec_off] into x10 — NOT x0)
        frame_load64(XReg::x10, thf.frame.gc_rec_off);
        emit_store_gc_head(XReg::x10);
    }
    void emit_prologue() {
        // stp x29, x30, [sp, -16]!  — pre-indexed store pair.
        // Encoding: STP X-reg pre-indexed: 0xA9BF0000 | (Rm=30<<(15+10)) ...
        // The Arm64Emitter does not expose stp/ldp; emit the raw encoding.
        // STP <Xt1>, <Xt2>, [<Xn|SP>, #imm]!  (pre-index, 64-bit, imm=-16/8=-2)
        //   encoding: 1 0 1 0 1 0 0 1 0 0 imm7 Rt2 Rn Rt  (pre-index L=0)
        //   base = 0xA9800000 (pre-index, 64-bit, store); imm7 = offset/8 = -2
        //   imm7 is signed 7-bit at bits[21:15]; -2 = 0x7E.
        // Rt2=x30(30)<<10, Rn=sp(31)<<5, Rt=x29(29)
        e.insn(0xA9800000u | (uint32_t(-2 & 0x7F) << 15)
               | (uint8_t(XReg::x30) << 10) | (uint8_t(XReg::sp) << 5)
               | uint8_t(XReg::x29));
        // mov x29, sp  — ADD-based (mov_reg uses ORR, which treats reg 31 as
        // XZR, so it cannot read/write SP). add x29, sp, #0 is the canonical
        // alias of `mov x29, sp` and uses SP for both Rd and Rn.
        e.add_reg_imm(XReg::x29, XReg::sp, 0);
        // sub sp, sp, frame_size  (frame_size round16).
        // CRITICAL: the register-form sub (0xCB000000, "Subtract (shifted
        // register)") treats R31 as XZR, NOT SP — so `sub_reg(sp, sp, x9)`
        // silently encodes `sub xzr, xzr, x9` (a discarded subtraction) and
        // NEVER decrements SP. For frame_size > 0xFFF the prior code hit this
        // path and left SP unchanged, so every frame store (stur to [x29 +
        // negative offset]) wrote BELOW the real stack into the caller's frame /
        // redzone / unmapped memory — miscompiling any function whose rounded
        // frame exceeds 4096 bytes (e.g. self-hosted keyword_kind=4112,
        // lex=6064). The bug is context-dependent: only large functions
        // trigger it, so small standalone repros compiled correctly.
        //
        // Fix: use the IMMEDIATE-form sub (0xD1000000, "Subtract (immediate)"),
        // which IS SP-aware (R31 = SP). It encodes a 12-bit unsigned immediate,
        // optionally shifted left by 12 (so 0..4095 unshifted + 0..0xFFF000 in
        // 4096-steps shifted). frame_size is round16; split it into a low
        // unshifted part (<= 0xFFF) + a high shifted-by-12 part (multiples of
        // 4096) and emit two SP-aware immediate subs. This covers frame sizes
        // up to ~16.7MB (0xFFF + 0xFFF<<12) — far beyond any realistic
        // function (the largest self-hosted fn is ~6KB). For a pathological
        // larger frame, fall back to a per-4K-chunk loop (also immediate-form,
        // SP-aware) so SP is always decremented correctly.
        int32_t fs = thf.frame.frame_size;
        if (fs <= 0xFFF) {
            e.sub_reg_imm(XReg::sp, XReg::sp, uint32_t(fs));
        } else {
            uint32_t lo = uint32_t(fs) & 0xFFFu;        // unshifted part (<=4095)
            uint32_t hi = uint32_t(fs) >> 12;            // 4096-chunk count
            if (lo != 0) e.sub_reg_imm(XReg::sp, XReg::sp, lo);
            while (hi > 0xFFFu) {
                // pathological frame > ~16.7MB: peel off 0xFFF<<12 per iter
                e.sub_reg_imm(XReg::sp, XReg::sp, 0xFFFu, /*sh12=*/true);
                hi -= 0xFFFu;
            }
            if (hi != 0) e.sub_reg_imm(XReg::sp, XReg::sp, hi, /*sh12=*/true);
        }
        // stur x20, [x29, rbx_save_offset]
        int32_t rso = thf.frame.rbx_save_offset;
        if (rso >= -256 && rso <= 255) {
            e.stur64(XReg::x20, XReg::x29, rso);
        } else {
            materialize_frame_addr(XReg::x10, rso);
            e.str64(XReg::x20, XReg::x10, 0);
        }
        // Precise GC: link this frame's record onto the shadow stack. x9 is
        // volatile + free here (no value live yet).
        emit_gc_frame_record_prologue();
    }
    void emit_epilogue() {
        // Precise GC: unlink this frame's record BEFORE tearing down the frame
        // (the record lives in this frame). Uses x10 (NOT x0, which holds the
        // i64 return value at every exit).
        emit_gc_frame_record_epilogue();
        // ldur x20, [x29, rbx_save_offset]
        int32_t rso = thf.frame.rbx_save_offset;
        if (rso >= -256 && rso <= 255) {
            e.ldur64(XReg::x20, XReg::x29, rso);
        } else {
            materialize_frame_addr(XReg::x10, rso);
            e.ldr64(XReg::x20, XReg::x10, 0);
        }
        // mov sp, x29  — ADD-based (mov_reg/ORR cannot write SP).
        e.add_reg_imm(XReg::sp, XReg::x29, 0);
        // ldp x29, x30, [sp], 16  — post-index load pair.
        // LDP post-index 64-bit: 0xA8C00000 | (imm7<<15) | (Rt2<<10) | (Rn<<5) | Rt
        //   imm7 = 16/8 = 2; Rt2=x30, Rn=sp, Rt=x29
        e.insn(0xA8C00000u | (uint32_t(2 & 0x7F) << 15)
               | (uint8_t(XReg::x30) << 10) | (uint8_t(XReg::sp) << 5)
               | uint8_t(XReg::x29));
        // ret
        e.ret();
    }

    // ─── param spills (AAPCS64, NOT Win64) ───
    // Recompute AAPCS64 placement from thf.frame.params TYPES (IGNORE the
    // Win64 word0/nwords fields). GP args -> x0-x7 (declaration order); FP args
    // -> v0-v7 (independent stream). Slice/lambda = 2 GP words (consecutive x
    // regs). Struct-by-value: classified via classify_aapcs64_arg (HFA ->
    // consecutive FP regs, <=16B composite -> GP words, >16B -> indirect ptr).
    //
    // STRUCT-BY-PTR RETURN (returns_struct_by_ptr): the hidden result-dest ptr
    // arrives in x8 (AAPCS64 indirect-result reg), NOT x0. The lowerer encodes
    // it as the FIRST param entry (p.ty == nullptr, the __struct_ret_ptr
    // sentinel). We spill x8 to struct_ret_ptr_offset and do NOT consume a GP
    // arg slot — the real params still start at x0. This mirrors emit_x64's
    // word-0 hidden-ptr spill but uses x8 (AAPCS64) instead of rcx (Win64).
    void emit_param_spills() {
        int gp_idx = 0;   // next GP arg reg (x0-x7)
        int fp_idx = 0;   // next FP arg reg (v0-v7) — independent stream
        uint32_t next_vreg = 1;  // VReg numbering: 1-indexed, param order

        // If this function returns a struct by ptr, the hidden dest ptr
        // arrives in x8 (AAPCS64). Spill it to struct_ret_ptr_offset. The
        // lowerer's first param entry is the null sentinel for it; skip that
        // entry in the loop (p.ty == nullptr).
        if (thf.frame.returns_struct_by_ptr && thf.frame.struct_ret_ptr_offset != 0) {
            frame_store64(XReg::x8, thf.frame.struct_ret_ptr_offset);
        }

        for (const auto& p : thf.frame.params) {
            const Type* pt = p.ty;
            if (pt == nullptr) continue;  // skip the __struct_ret_ptr sentinel
            if (is_registered_struct(pt, structs()) || pt->array_len > 0) {
                // struct / fixed-array by value: classify via AAPCS64.
                spill_struct_param(pt, p.off, gp_idx, fp_idx);
                // structs/arrays are frame slots (no VReg); advance next_vreg
                // by 0 (the lowerer assigns no VReg to a struct param). The
                // register streams were advanced inside spill_struct_param.
                continue;
            }
            if (pt->is_slice || pt->is_lambda) {
                // slice: 2 consecutive GP words (ptr, len). Needs gp_idx AND
                // gp_idx+1 < 8 (x0-x7); a +1>8 guard let gp_idx==7 slip through
                // and access kGpArgRegs[8] out of bounds.
                if (gp_idx + 2 > 8) {
                    throw std::runtime_error(
                        "emit_arm64: slice param beyond x0-x7 (stack args) not yet supported");
                }
                // spill ptr (xN) and len (xN+1)
                spill_gp_reg(kGpArgRegs[gp_idx], p.off);
                spill_gp_reg(kGpArgRegs[gp_idx + 1], p.off + 8);
                vregs[next_vreg]     = {p.off,     pt};
                vregs[next_vreg + 1] = {p.off + 8, pt};
                next_vreg += 2;
                gp_idx += 2;
            } else if (pt->is_float()) {
                if (fp_idx > 7) {
                    throw std::runtime_error(
                        "emit_arm64: float param beyond v0-v7 (stack args) not yet supported");
                }
                // Spill the incoming FP arg reg (v0-v7, INDEPENDENT stream
                // from GP) to the param's frame slot. f64 -> stur_f64 (8 bytes);
                // f32 -> stur_f32 (4 bytes, upper 4 bytes of the slot are
                // garbage; the use-site load_float_vreg reads f32 width so the
                // garbage is ignored — mirrors emit_x64's movss spill).
                if (pt->prim == Prim::F32) spill_fp_reg_f32(kFpArgRegs[fp_idx], p.off);
                else                       spill_fp_reg_f64(kFpArgRegs[fp_idx], p.off);
                vregs[next_vreg] = {p.off, pt};
                next_vreg += 1;
                fp_idx += 1;
            } else {
                // scalar int/bool: 1 GP word
                if (gp_idx > 7) {
                    throw std::runtime_error(
                        "emit_arm64: int param beyond x0-x7 (stack args) not yet supported");
                }
                spill_gp_reg(kGpArgRegs[gp_idx], p.off);
                vregs[next_vreg] = {p.off, pt};
                next_vreg += 1;
                gp_idx += 1;
            }
        }
    }
    // Spill an incoming struct/fixed-array-by-value param to its frame slot
    // `off`, using the AAPCS64 classification. Advances gp_idx/fp_idx by the
    // registers this param consumed. HFA: each FP member is spilled to its
    // field offset within the struct frame slot (via the StructLayout).
    // <=16B composite: GP words spilled to consecutive 8-byte slots. >16B:
    // indirect — the incoming GP reg holds a POINTER to the caller's copy;
    // spill the ptr, then copy the struct bytes from [ptr] to the frame slot.
    void spill_struct_param(const Type* pt, int32_t off, int& gp_idx, int& fp_idx) {
        Aapcs64ArgClass c = classify_aapcs64_arg(pt, structs(), uint8_t(gp_idx), uint8_t(fp_idx));
        if (c.indirect) {
            // >16B composite: the param arrives as a POINTER in the next GP reg.
            // Spill the ptr to a temp (off is the struct frame slot; store the
            // ptr at off, then copy bytes [ptr] -> [off]). Actually the struct
            // occupies `byte_size` bytes starting at `off`; the ptr is a single
            // 8-byte value. Use a temp slot at off for the ptr, copy through.
            // Simpler: spill the ptr reg to x9-via-frame, then copy_bytes from
            // [ptr] to [x29+off] for byte_size bytes.
            if (gp_idx > 7) throw std::runtime_error(
                "emit_arm64: indirect struct param ptr beyond x0-x7 (stack args) not yet supported");
            XReg ptr_reg = kGpArgRegs[gp_idx];
            gp_idx += 1;
            // Copy struct bytes from [ptr_reg] to the frame slot [x29+off].
            // copy_bytes uses x9/x11 + may materialize x10 — ptr_reg (x0-x7)
            // is caller-saved and not x9/x10/x11, so it's safe as a base.
            copy_bytes(XReg::x29, off, ptr_reg, 0, c.byte_size);
            return;
        }
        if (c.hfa_count > 0) {
            // HFA: each FP member -> its field offset in the struct frame slot.
            // Walk the struct layout's fields in declaration order, spilling
            // each consecutive FP arg reg to [x29 + off + field_offset].
            std::vector<std::pair<int32_t,bool>> members; // (offset, is_f32)
            collect_float_members(pt, members);
            if (int(members.size()) != int(c.hfa_count)) {
                // defensive: layout mismatch; fall back to contiguous FP spill
                for (uint8_t i = 0; i < c.hfa_count; ++i) {
                    int32_t foff = off + int32_t(i) * (c.is_f32_hfa ? 4 : 8);
                    if (c.is_f32_hfa) frame_store_f32(kFpArgRegs[fp_idx + i], foff);
                    else               frame_store_f64(kFpArgRegs[fp_idx + i], foff);
                }
            } else {
                for (uint8_t i = 0; i < c.hfa_count; ++i) {
                    int32_t foff = off + members[i].first;
                    if (members[i].second) frame_store_f32(kFpArgRegs[fp_idx + i], foff);
                    else                   frame_store_f64(kFpArgRegs[fp_idx + i], foff);
                }
            }
            fp_idx += int(c.hfa_count);
            return;
        }
        // <=16B non-HFA composite: GP words. The classifier gave ceil(size/8)
        // GP slots (max 2). Spill each GP word to consecutive 8-byte frame
        // offsets; the last word may be narrower (byte_size % 8).
        int32_t words = int(c.slots.size());
        int32_t byte_pos = 0;
        for (int i = 0; i < words; ++i) {
            int32_t wbytes = std::min<int32_t>(8, c.byte_size - byte_pos);
            if (wbytes >= 8) {
                spill_gp_reg(kGpArgRegs[gp_idx + i], off + byte_pos);
            } else {
                // narrow last word: load the GP reg into x9, store narrow.
                e.mov_reg(XReg::x9, kGpArgRegs[gp_idx + i]);
                store_x9_elem(XReg::x29, off + byte_pos, wbytes);
            }
            byte_pos += wbytes;
        }
        gp_idx += words;
    }
    // Collect a composite's float-leaf (offset, is_f32) pairs in declaration
    // order (recursing into nested structs / fixed arrays). Used to spill HFA
    // members to their field offsets.
    void collect_float_members(const Type* t, std::vector<std::pair<int32_t,bool>>& out) {
        if (!t) return;
        if (t->is_float()) { out.push_back({0, t->prim == Prim::F32}); return; }
        if (t->array_len > 0) {
            int32_t esz = value_bytes(t->elem.get(), structs());
            for (uint32_t i = 0; i < t->array_len; ++i) {
                std::vector<std::pair<int32_t,bool>> sub;
                collect_float_members(t->elem.get(), sub);
                for (auto& m : sub) out.push_back({int32_t(i)*esz + m.first, m.second});
            }
            return;
        }
        if (!t->struct_name.empty() && structs()) {
            auto it = structs()->find(t->struct_name);
            if (it != structs()->end()) {
                for (const auto& fn : it->second.field_names) {
                    auto fit = it->second.fields.find(fn);
                    if (fit == it->second.fields.end()) continue;
                    std::vector<std::pair<int32_t,bool>> sub;
                    collect_float_members(fit->second.ty, sub);
                    for (auto& m : sub) out.push_back({fit->second.offset + m.first, m.second});
                }
            }
        }
    }
    // Spill a GP arg reg to a frame slot (8-byte store). Narrow int params keep
    // the full 8-byte reg (upper bits may be garbage); the use-site normalize
    // fixes them (mirrors emit_x64's spill_word + normalize_rax).
    void spill_gp_reg(XReg src, int32_t off) {
        frame_store64(src, off);
    }
    // Spill an FP arg reg to a frame slot. f64 -> 8-byte store; f32 -> 4-byte
    // store (upper slot bytes untouched — the use-site load_float_vreg reads
    // f32 width). Uses the FP frame store helpers (ldur-range dispatch).
    void spill_fp_reg_f32(ArmVReg src, int32_t off) { frame_store_f32(src, off); }
    void spill_fp_reg_f64(ArmVReg src, int32_t off) { frame_store_f64(src, off); }

    // ─── call arg marshaling (AAPCS64) ───
    // Marshal a call's args from their frame slots into x0-x7 (GP) and v0-v7
    // (FP). AAPCS64: GP and FP are INDEPENDENT streams — a float arg does NOT
    // consume a GP slot. ≤8 GP + ≤8 FP args -> all in registers, no stack
    // args, sp stays 16-aligned.
    //
    // Arg kinds:
    //  • scalar int/bool -> 1 GP reg
    //  • scalar float    -> 1 FP reg
    //  • slice/lambda    -> 2 consecutive GP regs (ptr, len) [the slice ABI;
    //    the len VReg is the NEXT args[] entry, consumed here]
    //  • struct/fixed-array by value -> classify_aapcs64_arg:
    //      - HFA  -> consecutive FP regs (each float member from its field offset)
    //      - <=16B composite -> GP words (ceil(size/8), each from its frame slot)
    //      - >16B indirect   -> the CALLER allocates a temp frame slot, copies the
    //        struct bytes there, and passes the temp's ADDRESS as the next GP arg
    //  • struct-by-ptr RETURN call (ret_type is registered struct): args[0] is
    //    the hidden dest encoding (vreg holding the dest ptr, or a sentinel
    //    vreg=0 + arg_frame_offs[0] = the dest frame slot). The dest ptr goes in
    //    x8 (AAPCS64 indirect-result reg), NOT a GP arg slot; the real args
    //    still start at x0.
    //
    // `ret_struct` is true when this call returns a struct by ptr (the caller
    // passes x8). Returns the number of GP regs used by the REAL args (excl x8).
    int marshal_call_args(const ThinInstr& in, bool ret_struct) {
        int gp_idx = 0;
        int fp_idx = 0;   // INDEPENDENT FP stream (v0-v7)

        // Decode the hidden dest for a struct-by-ptr return call (args[0]).
        // lower_call encodes it as: a vreg (hidden_dest_vreg != 0, the dest
        // ptr — a forward-return forwarding the incoming hidden ptr, or a
        // computed dest address) OR a sentinel (args[0]==0 +
        // arg_frame_offs[0] = the dest frame slot offset). The dest ptr goes
        // in x8 (AAPCS64), not a GP arg slot.
        size_t arg_start = 0;
        if (ret_struct) {
            VReg a0 = in.args.empty() ? 0 : in.args[0];
            int32_t afo0 = in.arg_frame_offs.empty() ? -1 : in.arg_frame_offs[0];
            if (a0 != 0 && afo0 == -1) {
                // dest ptr is in this vreg (a loaded/computed ptr)
                load_int_vreg(a0);             // -> x9
                e.mov_reg(XReg::x8, XReg::x9);  // x8 = dest ptr
            } else {
                // dest is a frame slot: x8 = x29 + afo0
                materialize_frame_addr(XReg::x8, afo0);
            }
            arg_start = 1;  // skip the hidden dest in the real-arg loop
        }

        for (size_t i = arg_start; i < in.args.size(); ++i) {
            VReg v = in.args[i];
            const Type* ty = i < in.arg_types.size() ? in.arg_types[i] : vreg_type(v);
            int32_t afo = i < in.arg_frame_offs.size() ? in.arg_frame_offs[i] : -1;
            // struct-by-value arg: vreg sentinel (v==0) + arg_frame_offs[i] =
            // the struct's frame-slot offset.
            if (v == 0 && afo != -1) {
                marshal_struct_arg(ty, afo, gp_idx, fp_idx);
                continue;
            }
            if (ty && (ty->is_slice || ty->is_lambda)) {
                // slice: 2 consecutive GP regs (ptr, len). The len VReg is the
                // NEXT args[] entry — consume it here.
                if (gp_idx + 2 > 8) throw std::runtime_error(
                    // slice/lambda needs two consecutive GP regs (x0-x7); a
                    // +1>8 check let gp_idx==7 slip through and access
                    // kGpArgRegs[8] out of bounds.
                    "emit_arm64: slice arg beyond x0-x7 (stack args) not yet supported");
                load_slice_vreg_into(kGpArgRegs[gp_idx], kGpArgRegs[gp_idx + 1], v);
                gp_idx += 2;
                ++i;  // consume the len VReg
                continue;
            }
            if (ty && ty->is_float()) {
                if (fp_idx > 7) throw std::runtime_error(
                    "emit_arm64: call with >8 FP args (stack args) not yet supported");
                load_float_vreg_into(kFpArgRegs[fp_idx], v);
                ++fp_idx;
                continue;
            }
            // scalar int/bool arg
            if (gp_idx > 7) throw std::runtime_error(
                "emit_arm64: call with >8 GP args (stack args) not yet supported");
            load_int_vreg(v);                // -> x9
            e.mov_reg(kGpArgRegs[gp_idx], XReg::x9);
            ++gp_idx;
        }
        return gp_idx;
    }
    // Marshal a struct/fixed-array-by-value arg from its frame slot `off` into
    // the AAPCS64-classified registers. Advances gp_idx/fp_idx. For an indirect
    // (>16B) arg: the caller must allocate a temp frame slot, copy the struct
    // bytes there, and pass the temp's ADDRESS — but the lowerer passes the
    // ORIGINAL frame slot, so we copy it to a scratch frame area first. For
    // simplicity + correctness we materialize the original slot's address as
    // the GP arg (the callee reads through it; it does not need a separate
    // copy since the callee treats it as read-only for an arg). NOTE: this
    // matches emit_x64's struct-by-value arg path semantics (the callee copies
    // through the ptr for an arg; the caller does not need a writable copy).
    void marshal_struct_arg(const Type* ty, int32_t off, int& gp_idx, int& fp_idx) {
        Aapcs64ArgClass c = classify_aapcs64_arg(ty, structs(), uint8_t(gp_idx), uint8_t(fp_idx));
        if (c.indirect) {
            // >16B: pass a pointer to the struct frame slot as the next GP arg.
            if (gp_idx > 7) throw std::runtime_error(
                "emit_arm64: indirect struct arg ptr beyond x0-x7 (stack args) not yet supported");
            materialize_frame_addr(kGpArgRegs[gp_idx], off);
            gp_idx += 1;
            return;
        }
        if (c.hfa_count > 0) {
            // HFA: load each float member from its field offset into the next FP regs.
            std::vector<std::pair<int32_t,bool>> members;
            collect_float_members(ty, members);
            if (int(members.size()) != int(c.hfa_count)) {
                for (uint8_t i = 0; i < c.hfa_count; ++i) {
                    int32_t foff = off + int32_t(i) * (c.is_f32_hfa ? 4 : 8);
                    if (c.is_f32_hfa) frame_load_f32(kFpArgRegs[fp_idx + i], foff);
                    else               frame_load_f64(kFpArgRegs[fp_idx + i], foff);
                }
            } else {
                for (uint8_t i = 0; i < c.hfa_count; ++i) {
                    int32_t foff = off + members[i].first;
                    if (members[i].second) frame_load_f32(kFpArgRegs[fp_idx + i], foff);
                    else                   frame_load_f64(kFpArgRegs[fp_idx + i], foff);
                }
            }
            fp_idx += int(c.hfa_count);
            return;
        }
        // <=16B non-HFA composite: load GP words from the frame slot.
        int32_t words = int(c.slots.size());
        int32_t byte_pos = 0;
        for (int i = 0; i < words; ++i) {
            int32_t wbytes = std::min<int32_t>(8, c.byte_size - byte_pos);
            if (wbytes >= 8) {
                frame_load64(kGpArgRegs[gp_idx + i], off + byte_pos);
            } else {
                // narrow last word: load + zero-extend into the GP arg reg.
                load_elem_x9(XReg::x29, off + byte_pos, wbytes, false);
                e.mov_reg(kGpArgRegs[gp_idx + i], XReg::x9);
            }
            byte_pos += wbytes;
        }
        gp_idx += words;
    }

    // ─── the main emit ───
    CompiledFn emit() {
        if (thf.blocks.empty()) {
            // nothing to emit
            CompiledFn out; out.name = thf.name; return out;
        }
        // allocate block labels
        block_labels.resize(thf.blocks.size());
        for (size_t i = 0; i < thf.blocks.size(); ++i)
            block_labels[i] = e.alloc_label();

        // Precise GC: build the GcFrameMap from the frame plan's GC-pointer
        // slot offsets BEFORE the prologue so its address can be baked into the
        // frame-record link. The map's `offs` are stable once emit completes.
        // Mirrors emit_x64 (thin_emit.cpp:1149-1159).
        if (gc_active() && thf.frame.gc_rec_off != 0) {
            gc_map = std::make_shared<gc::GcFrameMap>();
            if (!thf.frame.gc_ptr_frame_offs.empty())
                gc_map->offs = thf.frame.gc_ptr_frame_offs;
        }

        // prologue
        emit_prologue();
        // param spills + VReg map init
        emit_param_spills();

        // Fail-safe coarse entry budget charge (unless block 0 has an explicit
        // BudgetCheck). Mirrors emit_x64.
        bool has_explicit_entry_budget = false;
        for (const ThinInstr& in : thf.blocks[0].instrs) {
            if (in.op == ThinOp::BudgetCheck) { has_explicit_entry_budget = true; break; }
        }
        if (ctx.emit_budget_checks && !has_explicit_entry_budget) {
            int64_t cost = 0;
            for (const auto& b : thf.blocks) cost += int64_t(b.instrs.size()) + 1;
            emit_budget_check(cost, "budget exceeded at function entry");
        }

        // walk blocks in order
        for (size_t bi = 0; bi < thf.blocks.size(); ++bi) {
            const ThinBlock& blk = thf.blocks[bi];
            e.bind(block_labels[blk.id]);
            for (const ThinInstr& in : blk.instrs)
                emit_instr(in);
            emit_term(blk.term);
        }

        // resolve label fixups + append literal pool + backpatch + veneers
        e.resolve_fixups();

        // fill AbsFixup placeholders (dispatch/globals/registry/rodata bases)
        for (const auto& af : e.abs_fixups()) {
            if (af.code_offset + 8 > e.code.size()) continue;
            uint8_t* p = e.code.data() + af.code_offset;
            uint64_t v = 0;
            switch (af.kind) {
            case AbsFixup::DispatchTableBase:   v = uint64_t(ctx.dispatch_base); break;
            case AbsFixup::GlobalsBase:         v = uint64_t(ctx.globals_base); break;
            case AbsFixup::ModuleRegistryBase:  v = uint64_t(ctx.registry_base); break;
            case AbsFixup::FunctionRodataBase:  v = uint64_t(thf.rodata.data() + af.addend); break;
            }
            for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
        }

        // build native_fixups + fill JIT-time ptrs. Pair pending_natives
        // (emission order) with e.native_fixups() (populated in the same order
        // by resolve_fixups) to learn each binding's literal-pool cell offset.
        std::vector<CompiledNativeBinding> native_bindings;
        const auto& nf = e.native_fixups();
        for (size_t i = 0; i < pending_natives.size(); ++i) {
            PendingNative& pn = pending_natives[i];
            if (i < nf.size()) pn.binding.code_offset = nf[i].code_offset;
            native_bindings.push_back(pn.binding);
            if (pn.binding.code_offset + 8 > e.code.size()) continue;
            uint64_t v = reinterpret_cast<uintptr_t>(pn.target);
            for (int k = 0; k < 8; ++k)
                e.code[pn.binding.code_offset + k] = uint8_t(v >> (8 * k));
        }

        // assemble CompiledFn
        CompiledFn out;
        out.name = thf.name;
        out.abs_fixups = e.abs_fixups();
        out.native_fixups = std::move(native_bindings);
        out.rodata = thf.rodata;
        if (non_serializable_reason.empty() && !thf.non_serializable_reason.empty())
            non_serializable_reason = thf.non_serializable_reason;
        out.non_serializable_reason = std::move(non_serializable_reason);
        out.gc_frame_map = std::move(gc_map);  // precise GC frame map (null when off)
        out.bytes = std::move(e.code);
        return out;
    }

    // ─── term emission ───
    void emit_term(const ThinTerm& term) {
        switch (term.kind) {
        case TermKind::Jmp:
            e.b(block_labels[term.target]);
            break;
        case TermKind::Branch: {
            // cond is a bool: nonzero = true. cbnz x9, true_target; b false_target.
            load_int_vreg(term.cond);
            e.cbnz64(XReg::x9, block_labels[term.target]);
            e.b(block_labels[term.false_target]);
            break;
        }
        case TermKind::Return:
            emit_return(term);
            break;
        case TermKind::Trap:
            emit_trap(int(term.trap_reason), "trap");
            break;
        case TermKind::None:
            break;
        }
    }

    void emit_return(const ThinTerm& term) {
        // struct-by-ptr return: load the hidden dest ptr (spilled to
        // struct_ret_ptr_offset in the prologue) into x0, epilogue. The struct
        // bytes were already copied through that ptr by earlier instrs. Mirrors
        // emit_x64's returns_struct_by_ptr path but x8 (spilled) -> x0.
        if (thf.frame.returns_struct_by_ptr) {
            if (thf.frame.struct_ret_ptr_offset != 0) {
                frame_load64(XReg::x0, thf.frame.struct_ret_ptr_offset);
            } else {
                e.mov_reg(XReg::x0, XReg::x8);
            }
            emit_epilogue();
            return;
        }
        const Type* rt = thf.ret_type;
        if (term.ret == 0 || rt == nullptr || rt->is_void()) {
            emit_epilogue();
            return;
        }
        if (rt->is_float()) {
            // float return: materialize into v0 (AAPCS64 FP return reg), epilogue
            load_float_vreg(term.ret);   // -> v0
            emit_epilogue();
            return;
        }
        if (rt->is_slice || rt->is_lambda) {
            // slice return: {x0=ptr, x1=len} (AAPCS64 two-word return).
            load_slice_vreg(term.ret);   // -> x0=ptr, x1=len
            emit_epilogue();
            return;
        }
        // int/bool return: materialize into x0, normalize, epilogue
        load_int_vreg(term.ret);   // -> x9
        normalize_x9(rt);
        e.mov_reg(XReg::x0, XReg::x9);
        emit_epilogue();
    }

    // ─── instruction emission (the big switch) ───
    void emit_instr(const ThinInstr& in) {
        switch (in.op) {
        // ── constants ──
        case ThinOp::ConstInt:
            e.mov_reg_imm64(XReg::x9, in.imm.i);
            record_dst_x9(in.dst, in.meta.type);
            pin_int_dst(in.dst, in.meta, in.meta.type);
            break;
        case ThinOp::ConstBool:
            e.mov_reg_imm64(XReg::x9, in.imm.i ? 1 : 0);
            record_dst_x9(in.dst, in.meta.type);
            pin_int_dst(in.dst, in.meta, in.meta.type);
            break;
        case ThinOp::ConstFloat: {
            // Materialize the double constant into v0 via a GP-reg bit-cast
            // (mov_reg_imm64 of the reinterpreted bits) then fmov_int_to_fp.
            // Value-equivalent to emit_x64's movq/movd-from-rax rodata path.
            bool is_f32 = (in.meta.is_f32 != 0);
            load_float_imm_into(ArmVReg::v0, in.imm.f, is_f32);
            record_dst_v0(in.dst, in.meta.type);
            pin_float_dst(in.dst, in.meta, in.meta.type);
            break;
        }
        case ThinOp::ConstStringRef: {
            // slice {ptr=rodata_addr, len}. ptr = ldr_literal_ptr(x0,
            // FunctionRodataBase, addend) (the rodata base reloc + addend = the
            // string's offset; the literal-pool cell resolves to the rodata
            // address at JIT time — already absolute, do NOT absolute-ize).
            // len = mov_reg_imm64(x1, meta.len). pin_slice_dst.
            e.ldr_literal_ptr(XReg::x0, AbsFixup::FunctionRodataBase, in.meta.addend);
            e.mov_reg_imm64(XReg::x1, int64_t(in.meta.len));
            record_dst_x9(in.dst, in.meta.type);  // (tracking; result is a slice)
            pin_slice_dst(in.dst, in.meta, in.meta.type, XReg::x0, XReg::x1);
            break;
        }
        case ThinOp::StringDecrypt: {
            // inline XOR decrypt of rodata bytes into a temp frame buffer,
            // then yield slice {ptr=&temp, len}. Mirrors emit_x64 StringDecrypt.
            // data_temp_off = decrypted-data buffer; frame_off = slice result
            // slot {ptr,len}. imm.i = XOR key. meta.addend = rodata offset.
            // meta.len = byte length.
            const int32_t data_off = in.meta.data_temp_off != 0
                ? in.meta.data_temp_off : in.meta.frame_off;
            const int32_t slice_off = in.meta.frame_off;
            const int64_t len = in.meta.len;
            const uint8_t key = uint8_t(in.imm.i);
            // x11 = enc source = rodata base + addend (the literal-pool cell;
            // already absolute after JIT patching — do NOT re-absolute-ize).
            e.ldr_literal_ptr(XReg::x11, AbsFixup::FunctionRodataBase, in.meta.addend);
            // x10 = x29 + data_off (the temp buffer address; data_off negative)
            materialize_frame_addr(XReg::x10, data_off);
            // inline byte XOR: for each i in [0,len): x9 = [x11+i] ^ key; [x10+i] = x9.
            // Use an unrolled loop for len <= 256 (ldurb/sturb with imm offsets;
            // offsets > 255 need materialization, so for larger i use x12 as
            // an index). For len > 256, a counted loop.
            if (len <= 256) {
                for (int64_t i = 0; i < len; ++i) {
                    int32_t io = int32_t(i);
                    // x9 = [x11 + io]
                    if (io >= -256 && io <= 255) e.ldurb(XReg::x9, XReg::x11, io);
                    else { materialize_off(XReg::x12, XReg::x11, io); e.ldrb(XReg::x9, XReg::x12, 0); }
                    // x9 ^= key  (eor_imm not exposed; use mov_reg_imm64 + eor_reg)
                    e.mov_reg_imm64(XReg::x12, int64_t(key));
                    e.eor_reg(XReg::x9, XReg::x9, XReg::x12);
                    // [x10 + io] = x9
                    if (io >= -256 && io <= 255) e.sturb(XReg::x9, XReg::x10, io);
                    else { materialize_off(XReg::x12, XReg::x10, io); e.strb(XReg::x9, XReg::x12, 0); }
                }
            } else {
                // counted loop: x12 = len (counter); x11=src, x10=dst (offset 0).
                // x9 = byte scratch, x13 = key. Advance x11/x10 each iter.
                e.mov_reg_imm64(XReg::x12, len);
                e.mov_reg_imm64(XReg::x13, int64_t(key));
                Label loop = e.alloc_label(), done = e.alloc_label();
                e.bind(loop);
                e.cbz64(XReg::x12, done);
                e.ldrb(XReg::x9, XReg::x11, 0);
                e.eor_reg(XReg::x9, XReg::x9, XReg::x13);
                e.strb(XReg::x9, XReg::x10, 0);
                e.add_reg_imm(XReg::x11, XReg::x11, 1);
                e.add_reg_imm(XReg::x10, XReg::x10, 1);
                e.sub_reg_imm(XReg::x12, XReg::x12, 1);
                e.b(loop);
                e.bind(done);
                // re-derive the temp base (x10 was advanced): x10 = x29 + data_off
                materialize_frame_addr(XReg::x10, data_off);
            }
            // slice result: ptr = x10 (the temp buffer), len = x1
            e.mov_reg(XReg::x0, XReg::x10);
            e.mov_reg_imm64(XReg::x1, len);
            // record the slice dst + store {ptr,len} to slice_off if frame-backed
            if (in.dst != 0) vregs[in.dst].type = in.meta.type;
            pin_slice_dst(in.dst, in.meta, in.meta.type, XReg::x0, XReg::x1);
            (void)slice_off;
            break;
        }

        // ── moves / memory ──
        case ThinOp::Move: {
            const Type* ty = in.meta.type ? in.meta.type : vreg_type(in.src1);
            if (ty && ty->is_float()) {
                // float Move: load src1 -> v0, pin dst. (record + pin store v0.)
                load_float_vreg(in.src1);
                record_dst_v0(in.dst, ty);
                pin_float_dst(in.dst, in.meta, ty);
                break;
            }
            if (ty && (ty->is_slice || ty->is_lambda)) {
                // slice Move: load src1 slice {x0,x1}, pin dst.
                load_slice_vreg(in.src1);
                pin_slice_dst(in.dst, in.meta, ty);
                break;
            }
            load_int_vreg(in.src1);
            normalize_x9(ty);
            record_dst_x9(in.dst, ty);
            pin_int_dst(in.dst, in.meta, ty);
            break;
        }
        case ThinOp::LoadFrame: {
            // dst = [base + displacement]. base = x29 for an ordinary frame
            // load; src1 != 0 -> a computed IndexAddr/FieldAddr VReg (the base
            // ptr), with field_off the within-base displacement + frame_off a
            // separate spill slot for the result. Computed loads need
            // slice/struct support (Phase 6); throw for src1 != 0 this phase.
            const Type* ty = in.meta.type;
            if (in.src1 != 0) {
                // computed address load: base ptr in src1 (a FieldAddr/IndexAddr
                // result, frame-backed or in x9). displacement = meta.field_off.
                load_int_vreg(in.src1);             // x9 = base ptr
                // move base to x11 (a scratch) so loading the element can use x9
                e.mov_reg(XReg::x11, XReg::x9);
                if (ty && ty->is_float()) {
                    bool is_f32 = ty->prim == Prim::F32;
                    if (is_f32) {
                        if (in.meta.field_off >= -256 && in.meta.field_off <= 255)
                            e.ldur_f32(ArmVReg::v0, XReg::x11, in.meta.field_off);
                        else { materialize_off(XReg::x10, XReg::x11, in.meta.field_off);
                               e.ldr_f32(ArmVReg::v0, XReg::x10, 0); }
                    } else {
                        if (in.meta.field_off >= -256 && in.meta.field_off <= 255)
                            e.ldur_f64(ArmVReg::v0, XReg::x11, in.meta.field_off);
                        else { materialize_off(XReg::x10, XReg::x11, in.meta.field_off);
                               e.ldr_f64(ArmVReg::v0, XReg::x10, 0); }
                    }
                    record_dst_v0(in.dst, ty);
                    if (in.dst != 0 && in.meta.frame_off != 0) {
                        if (is_f32) frame_store_f32(ArmVReg::v0, in.meta.frame_off);
                        else        frame_store_f64(ArmVReg::v0, in.meta.frame_off);
                        vregs[in.dst].frame_off = in.meta.frame_off;
                    }
                } else if (ty && (ty->is_slice || ty->is_lambda)) {
                    // load a 16-byte slice from [base + field_off]: ptr+len
                    int32_t fo = in.meta.field_off;
                    if (fo >= -256 && fo <= 255) {
                        e.ldur64(XReg::x0, XReg::x11, fo);
                        e.ldur64(XReg::x1, XReg::x11, fo + 8);
                    } else {
                        materialize_off(XReg::x10, XReg::x11, fo);
                        e.ldr64(XReg::x0, XReg::x10, 0);
                        e.ldr64(XReg::x1, XReg::x10, 8);
                    }
                    pin_slice_dst(in.dst, in.meta, ty, XReg::x0, XReg::x1);
                } else {
                    // narrow int element load from [base + field_off], width = meta.width
                    bool signed_ = ty && ty->is_int() && !ty->is_uint() && !ty->is_fn_handle;
                    load_elem_x9(XReg::x11, in.meta.field_off, in.meta.width, signed_);
                    if (ty) normalize_x9(ty);
                    record_dst_x9(in.dst, ty);
                    pin_int_dst(in.dst, in.meta, ty);
                }
                break;
            }
            if (ty && ty->is_float()) {
                // ordinary float frame load: v0 = [x29 + meta.frame_off]
                bool is_f32 = ty->prim == Prim::F32;
                if (is_f32) frame_load_f32(ArmVReg::v0, in.meta.frame_off);
                else        frame_load_f64(ArmVReg::v0, in.meta.frame_off);
                record_dst_v0(in.dst, ty);
                if (in.dst != 0) {
                    vregs[in.dst].frame_off = in.meta.frame_off;
                    vregs[in.dst].type = ty;
                }
                break;
            }
            if (ty && (ty->is_slice || ty->is_lambda)) {
                // ordinary slice frame load: {x0,x1} = [x29 + off]
                frame_load64(XReg::x0, in.meta.frame_off);
                frame_load64(XReg::x1, in.meta.frame_off + 8);
                pin_slice_dst(in.dst, in.meta, ty, XReg::x0, XReg::x1);
                break;
            }
            // ordinary frame load: x9 = [x29 + meta.frame_off]
            frame_load64(XReg::x9, in.meta.frame_off);
            normalize_x9(ty);
            record_dst_x9(in.dst, ty);
            if (in.dst != 0) {
                vregs[in.dst].frame_off = in.meta.frame_off;
                vregs[in.dst].type = ty;
            }
            break;
        }
        case ThinOp::StoreFrame: {
            // Store to [x29 + meta.frame_off] = src1 (ordinary), or to a
            // computed address in src2 (aggregate element). src2 != 0 needs
            // slice/struct support (Phase 6); throw.
            const Type* ty = in.meta.type ? in.meta.type : vreg_type(in.src1);
            if (in.src2 != 0) {
                // computed-address store: [src2-ptr + meta.frame_off] = src1.
                load_int_vreg(in.src2);             // x9 = base ptr
                e.mov_reg(XReg::x11, XReg::x9);     // x11 = base ptr
                if (ty && ty->is_float()) {
                    load_float_vreg(in.src1);       // v0 = value
                    bool is_f32 = ty->prim == Prim::F32;
                    if (in.meta.frame_off >= -256 && in.meta.frame_off <= 255) {
                        if (is_f32) e.stur_f32(ArmVReg::v0, XReg::x11, in.meta.frame_off);
                        else        e.stur_f64(ArmVReg::v0, XReg::x11, in.meta.frame_off);
                    } else {
                        materialize_off(XReg::x10, XReg::x11, in.meta.frame_off);
                        if (is_f32) e.str_f32(ArmVReg::v0, XReg::x10, 0);
                        else        e.str_f64(ArmVReg::v0, XReg::x10, 0);
                    }
                } else if (ty && (ty->is_slice || ty->is_lambda)) {
                    load_slice_vreg(in.src1);       // x0=ptr, x1=len
                    int32_t fo = in.meta.frame_off;
                    if (fo >= -256 && fo <= 255) {
                        e.stur64(XReg::x0, XReg::x11, fo);
                        e.stur64(XReg::x1, XReg::x11, fo + 8);
                    } else {
                        materialize_off(XReg::x10, XReg::x11, fo);
                        e.str64(XReg::x0, XReg::x10, 0);
                        e.str64(XReg::x1, XReg::x10, 8);
                    }
                } else {
                    load_int_vreg(in.src1);         // x9 = value
                    normalize_x9(ty);
                    store_x9_elem(XReg::x11, in.meta.frame_off, in.meta.width);
                }
                break;
            }
            if (ty && ty->is_float()) {
                // float frame store: v0 = src1; stur_f32/f64 to [x29 + off]
                load_float_vreg(in.src1);   // -> v0
                bool is_f32 = ty->prim == Prim::F32;
                if (is_f32) frame_store_f32(ArmVReg::v0, in.meta.frame_off);
                else        frame_store_f64(ArmVReg::v0, in.meta.frame_off);
                if (in.src1 != 0) {
                    vregs[in.src1].frame_off = in.meta.frame_off;
                    vregs[in.src1].type = ty;
                }
                break;
            }
            if (ty && (ty->is_slice || ty->is_lambda)) {
                // slice frame store: {x0,x1} = src1; store ptr+len to [x29+off]
                load_slice_vreg(in.src1);
                frame_store64(XReg::x0, in.meta.frame_off);
                frame_store64(XReg::x1, in.meta.frame_off + 8);
                if (in.src1 != 0) {
                    vregs[in.src1].frame_off = in.meta.frame_off;
                    vregs[in.src1 + 1].frame_off = in.meta.frame_off + 8;
                    vregs[in.src1].type = ty;
                    vregs[in.src1 + 1].type = ty;
                }
                break;
            }
            load_int_vreg(in.src1);
            normalize_x9(ty);
            // Aggregate fields (field_off != 0) are packed — honor exact width.
            // For StoreFrame, field_off is a FLAG (==1) meaning "exact width";
            // the address is frame_off itself (NOT frame_off + field_off). This
            // mirrors emit_x64's StoreFrame (store_rax_elem at frame_off, width).
            // Ordinary scalar locals: 8-byte store (the established slot width).
            if (in.meta.field_off != 0) {
                store_x9_elem(XReg::x29, in.meta.frame_off, in.meta.width);
            } else {
                frame_store64(XReg::x9, in.meta.frame_off);
            }
            if (in.src1 != 0) {
                vregs[in.src1].frame_off = in.meta.frame_off;
                vregs[in.src1].type = ty;
            }
            break;
        }
        case ThinOp::StoreAddr: {
            // [src2 + meta.frame_off] = src1 (indirect store through a
            // computed address). src2 holds an address from FieldAddr/IndexAddr.
            const Type* ty = in.meta.type ? in.meta.type : vreg_type(in.src1);
            load_int_vreg(in.src2);             // x9 = base ptr (the address)
            e.mov_reg(XReg::x11, XReg::x9);     // x11 = base ptr
            if (ty && ty->is_float()) {
                load_float_vreg(in.src1);       // v0 = value
                bool is_f32 = ty->prim == Prim::F32;
                if (in.meta.frame_off >= -256 && in.meta.frame_off <= 255) {
                    if (is_f32) e.stur_f32(ArmVReg::v0, XReg::x11, in.meta.frame_off);
                    else        e.stur_f64(ArmVReg::v0, XReg::x11, in.meta.frame_off);
                } else {
                    materialize_off(XReg::x10, XReg::x11, in.meta.frame_off);
                    if (is_f32) e.str_f32(ArmVReg::v0, XReg::x10, 0);
                    else        e.str_f64(ArmVReg::v0, XReg::x10, 0);
                }
            } else if (ty && (ty->is_slice || ty->is_lambda)) {
                load_slice_vreg(in.src1);       // x0=ptr, x1=len
                int32_t fo = in.meta.frame_off;
                if (fo >= -256 && fo <= 255) {
                    e.stur64(XReg::x0, XReg::x11, fo);
                    e.stur64(XReg::x1, XReg::x11, fo + 8);
                } else {
                    materialize_off(XReg::x10, XReg::x11, fo);
                    e.str64(XReg::x0, XReg::x10, 0);
                    e.str64(XReg::x1, XReg::x10, 8);
                }
            } else {
                load_int_vreg(in.src1);         // x9 = value
                normalize_x9(ty);
                store_x9_elem(XReg::x11, in.meta.frame_off, in.meta.width);
            }
            break;
        }
        case ThinOp::LoadGlobal: {
            const Type* ty = in.meta.type;
            if (ty && (ty->is_slice || ty->is_lambda)) {
                // slice/lambda global load: {ptr,len} = [globals_base + addend].
                // A slice global's ptr is stored as a RELATIVE offset within the
                // block (baked at load so the bytes round-trip through .em
                // without loader fixup); turn it into an ABSOLUTE address by
                // adding globals_base (mirrors emit_x64's LoadGlobal slice path
                // + CG::eval's global-slice Ident case). A lambda global's
                // env_ptr (word1) is an ABSOLUTE GC heap pointer, NOT a relative
                // block offset, so it is loaded verbatim — NO globals_base add.
                e.ldr_literal_ptr(XReg::x9, AbsFixup::GlobalsBase);
                int32_t add = int32_t(in.meta.addend);
                load_global_slot(XReg::x0, XReg::x9, add);       // x0 = ptr (relative for slice)
                load_global_slot(XReg::x1, XReg::x9, add + 8);   // x1 = len / env_ptr
                if (ty->is_slice) {
                    // x0 = absolute ptr = relative ptr + globals_base
                    e.add_reg(XReg::x0, XReg::x0, XReg::x9);
                }
                pin_slice_dst(in.dst, in.meta, ty, XReg::x0, XReg::x1);
                break;
            }
            if (ty && ty->is_float()) {
                // float global load: x9 = globals_base; v0 = [x9 + addend]
                e.ldr_literal_ptr(XReg::x9, AbsFixup::GlobalsBase);
                int32_t add = int32_t(in.meta.addend);
                bool is_f32 = ty->prim == Prim::F32;
                if (is_f32) load_global_slot_f32(ArmVReg::v0, XReg::x9, add);
                else        load_global_slot_f64(ArmVReg::v0, XReg::x9, add);
                record_dst_v0(in.dst, ty);
                if (in.dst != 0 && in.meta.frame_off != 0) {
                    if (is_f32) frame_store_f32(ArmVReg::v0, in.meta.frame_off);
                    else        frame_store_f64(ArmVReg::v0, in.meta.frame_off);
                    vregs[in.dst].frame_off = in.meta.frame_off;
                }
                break;
            }
            // x9 = globals_base (relocatable); x9 = [x9 + addend]
            e.ldr_literal_ptr(XReg::x9, AbsFixup::GlobalsBase);
            int32_t add = int32_t(in.meta.addend);
            load_global_slot(XReg::x9, XReg::x9, add);
            normalize_x9(ty);
            record_dst_x9(in.dst, ty);
            // globals result is not frame-backed by default; if meta.frame_off,
            // store it (the dst's spill slot).
            if (in.dst != 0 && in.meta.frame_off != 0) {
                frame_store64(XReg::x9, in.meta.frame_off);
                vregs[in.dst].frame_off = in.meta.frame_off;
            }
            break;
        }
        case ThinOp::StoreGlobal: {
            const Type* ty = in.meta.type ? in.meta.type : vreg_type(in.src1);
            if (ty && ty->is_float()) {
                // float global store: v0 = src1; x11 = globals_base;
                // str_f32/f64 v0, [x11 + addend]. v0 survives the GP base load.
                load_float_vreg(in.src1);   // -> v0
                e.ldr_literal_ptr(XReg::x11, AbsFixup::GlobalsBase);
                bool is_f32 = ty->prim == Prim::F32;
                if (is_f32) store_global_slot_f32(ArmVReg::v0, XReg::x11, int32_t(in.meta.addend));
                else        store_global_slot_f64(ArmVReg::v0, XReg::x11, int32_t(in.meta.addend));
                break;
            }
            if (ty && (ty->is_slice || ty->is_lambda)) {
                // slice global store: {ptr,len} -> [globals_base + addend]
                load_slice_vreg(in.src1);   // x0=ptr, x1=len
                e.ldr_literal_ptr(XReg::x11, AbsFixup::GlobalsBase);
                store_global_slot(XReg::x0, XReg::x11, int32_t(in.meta.addend));
                store_global_slot(XReg::x1, XReg::x11, int32_t(in.meta.addend) + 8);
                break;
            }
            load_int_vreg(in.src1);
            normalize_x9(ty);
            // stash value in x10 across the globals-base load (clobbers x9)
            e.mov_reg(XReg::x10, XReg::x9);
            e.ldr_literal_ptr(XReg::x11, AbsFixup::GlobalsBase);
            store_global_slot(XReg::x10, XReg::x11, int32_t(in.meta.addend));
            break;
        }
        case ThinOp::CopyBytes: {
            // Copy meta.len bytes. Representation convention (set by the
            // copy_* helpers in thin_lower.cpp):
            //   meta.field_off = SOURCE offset
            //   meta.frame_off = DEST offset (0 when the dest is a vreg-held ptr)
            //   in.dst (vreg) != 0            -> dest = [vreg + 0] (a runtime ptr,
            //                                   e.g. the struct-return hidden ptr)
            //   meta.base_kind == GlobalsBase -> one side lives in the globals
            //                                   block; which side is disambiguated
            //                                   by the src1 sentinel (see below):
            //     in.dst != 0                 -> SOURCE is global (copy_global_vptr)
            //     in.dst == 0 && in.src1 != 0 -> DEST   is global (copy_frame_global)
            //     in.dst == 0 && in.src1 == 0 -> SOURCE is global (copy_global_frame)
            //   otherwise both sides are x29-relative (copy_frame_frame /
            //   copy_frame_vptr).
            const int32_t len = in.meta.len;
            const bool dst_is_vreg  = (in.dst != 0);
            const bool global       = (in.meta.base_kind == AbsFixup::GlobalsBase);
            const bool src_is_global = global && (dst_is_vreg || in.src1 == 0);
            const bool dst_is_global = global && !dst_is_vreg && in.src1 != 0;

            XReg dst_base = XReg::x29; int32_t dst_off = in.meta.frame_off;
            XReg src_base = XReg::x29; int32_t src_off = in.meta.field_off;

            if (dst_is_vreg) {
                // dest ptr is in the dst vreg (a runtime ptr, e.g. the struct-
                // return hidden ptr). Load it into x13 — a scratch that
                // copy_bytes NEVER touches internally. copy_bytes uses x9
                // (temp), x11 (loop byte-counter for copies > 32 B), and
                // x10/x12 (out-of-range offset scratch). Loading the dest ptr
                // into x11 (as this formerly did) collides with the x11 loop
                // counter: for a struct > 32 B (e.g. the 48-B Token), copy_bytes
                // overwrites x11 with the byte count, so the first `stur x9,
                // [x11]` writes to address 0x30 (= 48 = the counter) -> SIGSEGV.
                // x13 avoids that collision in BOTH the loop and unrolled paths
                // (neither load_elem_x9 / store_x9_elem / materialize_off use
                // x13), so the dest ptr survives the whole copy.
                load_int_vreg(in.dst);             // x9 = dest ptr
                e.mov_reg(XReg::x13, XReg::x9);    // x13 = dest ptr
                dst_base = XReg::x13; dst_off = 0;
            } else if (dst_is_global) {
                e.ldr_literal_ptr(XReg::x10, AbsFixup::GlobalsBase);
                dst_base = XReg::x10; dst_off = in.meta.frame_off;
            }
            if (src_is_global) {
                // globals base into a scratch not used as the dest base. If dst
                // is also global we already used x10 for dst; use x12 for src.
                XReg gsrc = dst_is_global ? XReg::x12 : XReg::x10;
                e.ldr_literal_ptr(gsrc, AbsFixup::GlobalsBase);
                src_base = gsrc; src_off = in.meta.field_off;
            }
            copy_bytes(dst_base, dst_off, src_base, src_off, len);
            // copy_bytes clobbers x9/x10/x11/x12; reset the register tracking.
            x9_vreg = 0;
            break;
        }

        // ── integer arithmetic ──
        case ThinOp::Add:
        case ThinOp::Sub:
        case ThinOp::Mul:
        case ThinOp::And:
        case ThinOp::Or:
        case ThinOp::Xor:
        case ThinOp::Shl:
        case ThinOp::Shr:
            emit_int_binop(in);
            break;
        case ThinOp::Div:
            emit_int_divmod_instr(in, /*want_mod=*/false);
            break;
        case ThinOp::Mod:
            emit_int_divmod_instr(in, /*want_mod=*/true);
            break;
        case ThinOp::Neg: {
            load_int_vreg(in.src1);
            e.neg_reg(XReg::x9, XReg::x9);
            normalize_x9(in.meta.type);
            record_dst_x9(in.dst, in.meta.type);
            pin_int_dst(in.dst, in.meta, in.meta.type);
            break;
        }
        case ThinOp::Not: {
            // logical not: x9 = (x9 == 0) ? 1 : 0
            load_int_vreg(in.src1);
            e.cmp_reg_imm(XReg::x9, 0);
            e.cset(XReg::x9, ArmCond::eq);   // x9 = (x9 == 0) ? 1 : 0
            record_dst_x9(in.dst, in.meta.type ? in.meta.type : &type_bool());
            pin_int_dst(in.dst, in.meta, in.meta.type ? in.meta.type : &type_bool());
            break;
        }
        case ThinOp::BitNot: {
            load_int_vreg(in.src1);
            e.mvn_reg(XReg::x9, XReg::x9);
            normalize_x9(in.meta.type);
            record_dst_x9(in.dst, in.meta.type);
            pin_int_dst(in.dst, in.meta, in.meta.type);
            break;
        }

        // ── float arithmetic ──
        case ThinOp::FAdd:
        case ThinOp::FSub:
        case ThinOp::FMul:
        case ThinOp::FDiv:
        case ThinOp::FMod:
            emit_float_binop(in);
            break;

        // ── compare ──
        case ThinOp::Cmp:
            emit_cmp(in);
            break;

        // ── short-circuit logical ──
        case ThinOp::LAnd:
        case ThinOp::LOr:
            emit_logical(in);
            break;

        // ── cast ──
        case ThinOp::Cast:
            emit_cast(in);
            break;

        // ── calls ──
        case ThinOp::CallNative:
        case ThinOp::CallScript:
        case ThinOp::CallIndirect:
        case ThinOp::CallCrossModule:
            emit_call(in);
            break;

        // ── addresses / aggregates ──
        case ThinOp::FieldAddr: {
            // element address = x29 + (frame_off + field_off). Compute x9 = the
            // address; pin_int_dst (the address is an i64). Mirrors emit_x64
            // FieldAddr (lea rax, [rbp + frame_off + field_off]).
            int32_t addr = in.meta.frame_off + in.meta.field_off;
            materialize_frame_addr(XReg::x9, addr);
            record_dst_x9(in.dst, in.meta.type ? in.meta.type : &type_i64());
            pin_int_dst(in.dst, in.meta, in.meta.type ? in.meta.type : &type_i64());
            break;
        }
        case ThinOp::IndexAddr: {
            // addr = base + index*width  (emit convention: src1=base, src2=index)
            // Result addr in x9; pin_int_dst (the address is an i64).
            // Materialize index into x12, scale by width (lsl_imm for pow2 widths,
            // else mul_reg), then add the base (slice ptr / frame array base /
            // global array base / vreg-held address).
            int32_t width = in.meta.width;
            // materialize index into x12
            if (in.src2 == 0) {
                e.mov_reg_imm64(XReg::x12, in.imm.i);
            } else {
                load_int_vreg(in.src2);             // x9 = index
                e.mov_reg(XReg::x12, XReg::x9);
            }
            // scale by width
            if (width > 1) {
                // power-of-2 width? lsl_imm; else mul_reg by a materialized width.
                if ((width & (width - 1)) == 0) {
                    uint8_t sh = 0; int w = width; while (w > 1) { w >>= 1; ++sh; }
                    e.lsl_imm(XReg::x12, XReg::x12, sh);
                } else {
                    e.mov_reg_imm64(XReg::x9, int64_t(width));
                    e.mul_reg(XReg::x12, XReg::x12, XReg::x9);
                }
            }
            // add base -> x9 = base + index*width
            if (in.src1 != 0 && vreg_is_slice(in.src1)) {
                // slice base: ptr is the slice's first word
                load_slice_vreg_into(XReg::x9, XReg::x10, in.src1);  // x9=ptr, x10=len
                e.add_reg(XReg::x9, XReg::x9, XReg::x12);
            } else if (in.src1 != 0) {
                // vreg-held base address
                load_int_vreg(in.src1);             // x9 = base
                e.add_reg(XReg::x9, XReg::x9, XReg::x12);
            } else if (in.meta.base_kind == AbsFixup::GlobalsBase) {
                // global fixed-array base: globals_base + addend
                e.ldr_literal_ptr(XReg::x9, AbsFixup::GlobalsBase);
                if (in.meta.addend != 0) {
                    e.mov_reg_imm64(XReg::x10, int64_t(int32_t(in.meta.addend)));
                    e.add_reg(XReg::x9, XReg::x9, XReg::x10);
                }
                e.add_reg(XReg::x9, XReg::x9, XReg::x12);
            } else {
                // local fixed-array base at meta.frame_off: x9 = x29 + frame_off
                materialize_frame_addr(XReg::x9, in.meta.frame_off);
                e.add_reg(XReg::x9, XReg::x9, XReg::x12);
            }
            // meta.frame_off identifies a local fixed-array BASE; it is NOT a
            // spill slot for the address result. Record as i64 + pin via a
            // temporary meta (frame_off=0) so the address is not stored to the
            // array base slot (mirrors emit_x64).
            record_dst_x9(in.dst, &type_i64());
            { ThinMeta home{}; home.type = &type_i64(); home.width = 8;
              pin_int_dst(in.dst, home, &type_i64()); }
            break;
        }
        case ThinOp::BoundsCheck: {
            // idx (src1) into x9, len (src2 vreg or imm) into x10.
            emit_bounds_check_instr(in);
            x9_vreg = 0;
            break;
        }
        case ThinOp::DivOverflowCheck: {
            // Standalone div-overflow guard. This phase's div/mod emit their
            // OWN inline div-by-zero + signed-overflow guards, so a standalone
            // DivOverflowCheck is a no-op (the paired Div/Mod re-checks). Phase
            // 6 may wire a standalone guard for the separated lowerer form.
            break;
        }
        case ThinOp::MakeSlice: {
            // materialize slice {ptr,len} from a backing array.
            //   LOCAL fixed array: ptr = x29 + frame_off (frame_off is the
            //     BACKING ARRAY base, NOT the slice's own slot — the lowerer
            //     emits a separate StoreFrame to store the slice to its slot)
            //   GLOBAL fixed array (base_kind==GlobalsBase): ptr = globals_base + addend
            // len = meta.len. Result {x0=ptr, x1=len} left in regs (NOT pinned
            // to frame_off — that would overwrite the backing array). The
            // following StoreFrame stores the slice to its own slot. Mirrors
            // emit_x64 MakeSlice (which does not store to frame_off).
            if (in.meta.base_kind == AbsFixup::GlobalsBase) {
                e.ldr_literal_ptr(XReg::x0, AbsFixup::GlobalsBase);
                if (in.meta.addend != 0) {
                    e.mov_reg_imm64(XReg::x9, int64_t(int32_t(in.meta.addend)));
                    e.add_reg(XReg::x0, XReg::x0, XReg::x9);
                }
            } else {
                // local fixed array: ptr = x29 + frame_off (the backing base)
                materialize_frame_addr(XReg::x0, in.meta.frame_off);
            }
            e.mov_reg_imm64(XReg::x1, int64_t(in.meta.len));
            if (in.dst != 0) {
                vregs[in.dst].type = in.meta.type;
                // do NOT set frame_off (frame_off is the backing array base;
                // the slice result stays in {x0,x1} until a StoreFrame pins it)
            }
            break;
        }
        case ThinOp::StructLitInit: {
            // store src1 (field value) at [x29 + frame_off + field_off]
            int32_t addr = in.meta.frame_off + in.meta.field_off;
            const Type* ft = in.meta.type;
            if (ft && ft->is_float()) {
                load_float_vreg(in.src1);   // v0 = value
                bool is_f32 = ft->prim == Prim::F32;
                if (is_f32) frame_store_f32(ArmVReg::v0, addr);
                else        frame_store_f64(ArmVReg::v0, addr);
            } else if (ft && (ft->is_slice || ft->is_lambda)) {
                load_slice_vreg(in.src1);   // x0=ptr, x1=len
                frame_store64(XReg::x0, addr);
                frame_store64(XReg::x1, addr + 8);
            } else {
                load_int_vreg(in.src1);     // x9 = value
                normalize_x9(ft);
                int32_t w = value_bytes(ft, structs());
                store_x9_elem(XReg::x29, addr, w < 8 ? w : 8);
            }
            break;
        }
        case ThinOp::ArrayLitInit: {
            // store src1 (element value) at [x29 + frame_off + field_off]
            int32_t addr = in.meta.frame_off + in.meta.field_off;
            const Type* et = in.meta.type;
            if (et && et->is_float()) {
                load_float_vreg(in.src1);   // v0 = value
                bool is_f32 = et->prim == Prim::F32;
                if (is_f32) frame_store_f32(ArmVReg::v0, addr);
                else        frame_store_f64(ArmVReg::v0, addr);
            } else if (et && (et->is_slice || et->is_lambda)) {
                load_slice_vreg(in.src1);   // x0=ptr, x1=len
                frame_store64(XReg::x0, addr);
                frame_store64(XReg::x1, addr + 8);
            } else {
                load_int_vreg(in.src1);     // x9 = value
                normalize_x9(et);
                int32_t w = value_bytes(et, structs());
                store_x9_elem(XReg::x29, addr, w < 8 ? w : 8);
            }
            break;
        }

        // ── guards (safety) ──
        case ThinOp::DepthCheck:
            emit_depth_check();
            x9_vreg = 0;
            break;
        case ThinOp::BudgetCheck:
            emit_budget_check(int64_t(in.imm.i), "budget exceeded");
            break;
        case ThinOp::CallTargetGuard:
            emit_call_target_guard();
            break;

        // ── try/catch/throw (Phase 5) ──
        // The save area is context_t::catch_bufs[catch_depth] (64-byte stride,
        // opaque — we control both save+restore, no libc jmp_buf). ARM64 layout:
        //   [0]=x19(ctx) [8]=x20(rbx-role) [16]=x29(FP) [24]=x30(LR)
        //   [32]=SP [40]=catch-entry PC [48..63]=reserved
        // plan_MACOS_ARM64.md Phase 5 (advisor-guided). Self-contained register
        // save/restore — no host setjmp checkpoint needed for in-JIT catch.
        case ThinOp::TryCatch: {
            if (!ctx.use_context_reg) {
                emit_trap(int(TrapReason::IllegalInstruction),
                          "try/catch requires a context register (use_context_reg)");
                break;
            }
            const int32_t cd_off  = context_offsets::catch_depth();
            const int32_t cb_off  = context_offsets::catch_bufs();
            const int32_t csd_off = context_offsets::catch_saved_depths();
            // x9 = catch_depth; reject a full/corrupted catch stack.
            ld_ctx32(XReg::x9, cd_off);
            e.cmp_reg_imm(XReg::x9, uint32_t(context_t::MAX_CATCH_DEPTH));
            Label cd_ok = e.alloc_label();
            e.b_cond(ArmCond::cc, cd_ok);  // unsigned < MAX -> ok (cc = carry clear = lo)
            emit_trap(int(TrapReason::StackOverflow),
                      "try/catch nesting exceeded MAX_CATCH_DEPTH");
            e.bind(cd_ok);
            // x10 = &catch_bufs[catch_depth] = (x19 + cb_off) + catch_depth*64
            materialize_ctx_addr(XReg::x10, cb_off);   // x10 = x19 + cb_off
            e.lsl_imm(XReg::x9, XReg::x9, 6);          // x9 = catch_depth * 64
            e.add_reg(XReg::x10, XReg::x10, XReg::x9); // x10 = &catch_bufs[cd]
            // save callee-saved regs the JIT uses: [0]=x19 [8]=x20 [16]=x29 [24]=x30
            e.stur64(XReg::x19, XReg::x10, 0);
            e.stur64(XReg::x20, XReg::x10, 8);
            e.stur64(XReg::x29, XReg::x10, 16);
            e.stur64(XReg::x30, XReg::x10, 24);
            // save SP: [32]. add x9, sp, #0 reads SP (reg 31 = SP in ADD).
            e.add_reg_imm(XReg::x9, XReg::sp, 0);
            e.stur64(XReg::x9, XReg::x10, 32);
            // save catch-entry PC: [40]. adrp+add :lo12: (robust for any distance).
            e.adrp_add_label(XReg::x9, block_labels[in.meta.slot]);
            e.stur64(XReg::x9, XReg::x10, 40);
            // save call_depth into catch_saved_call_depths[catch_depth].
            // x11 = &catch_saved_call_depths[catch_depth] = (x19 + csd_off) + cd*4
            ld_ctx32(XReg::x9, cd_off);                 // x9 = catch_depth (reload)
            materialize_ctx_addr(XReg::x11, csd_off);   // x11 = x19 + csd_off
            e.lsl_imm(XReg::x9, XReg::x9, 2);           // x9 = cd * 4
            e.add_reg(XReg::x11, XReg::x11, XReg::x9);  // x11 = &csd[cd]
            ld_ctx32(XReg::x9, context_offsets::depth());
            e.stur32(XReg::x9, XReg::x11, 0);
            // catch_depth++
            ld_ctx32(XReg::x9, cd_off);
            e.add_reg_imm(XReg::x9, XReg::x9, 1);
            st_ctx32(XReg::x9, cd_off);
            // The in-x9 model is unsound across the try body (calls/traps may
            // clobber x9); the lowerer frame-backs every live value before the
            // TryCatch, so no tracking to clear here (emit_arm64 is frame-only).
            break;
        }
        case ThinOp::CatchCleanup: {
            if (!ctx.use_context_reg) break;
            const int64_t pops = in.imm.i > 0 ? in.imm.i : 1;
            const int32_t cd_off = context_offsets::catch_depth();
            ld_ctx32(XReg::x9, cd_off);
            e.sub_reg_imm(XReg::x9, XReg::x9, uint32_t(pops));
            st_ctx32(XReg::x9, cd_off);
            break;
        }
        case ThinOp::CatchEntry: {
            // catch-block prologue: load context_t::thrown_value into the
            // catch_name i64 slot (meta.frame_off). The throw's restore already
            // landed us at this block's label with regs/SP/PC restored.
            if (!ctx.use_context_reg) break;
            // Precise GC: a cross-frame throw longjmp'd past abandoned inner
            // frames (no epilogues ran), so their records are still linked on
            // gc_frame_head. Restore head to THIS catching frame's record
            // (x29 + gc_rec_off), unlinking the stale records. No-op when off.
            // Uses x10 (NOT x9, which holds thrown_value below). Mirrors
            // emit_x64's CatchEntry (thin_emit.cpp:1690-1697).
            if (gc_active() && thf.frame.gc_rec_off != 0 && gc_map) {
                materialize_frame_addr(XReg::x10, thf.frame.gc_rec_off);
                emit_store_gc_head(XReg::x10);
            }
            ld_ctx64(XReg::x9, context_offsets::thrown_value());
            if (in.meta.frame_off != 0) {
                // store x9 to [x29 + frame_off] (the catch_name slot)
                int32_t off = in.meta.frame_off;
                if (off >= -256 && off <= 255) e.stur64(XReg::x9, XReg::x29, off);
                else { e.mov_reg_imm64(XReg::x10, int64_t(off));
                       e.add_reg(XReg::x10, XReg::x29, XReg::x10);
                       e.str64(XReg::x9, XReg::x10, 0); }
            }
            break;
        }
        case ThinOp::Throw: {
            if (!ctx.use_context_reg) {
                emit_trap(int(TrapReason::IllegalInstruction),
                          "throw requires a context register (use_context_reg)");
                break;
            }
            // eval thrown i64 (src1) -> x9, store into context_t::thrown_value.
            load_int_vreg(in.src1);  // -> x9
            st_ctx64(XReg::x9, context_offsets::thrown_value());
            // if catch_depth == 0 -> no handler: trap UnhandledThrow.
            ld_ctx32(XReg::x9, context_offsets::catch_depth());
            e.cmp_reg_imm(XReg::x9, 0);
            Label no_handler = e.alloc_label();
            e.b_cond(ArmCond::eq, no_handler);
            // has a handler: catch_depth-- ; restore call_depth from csd[cd-1].
            e.sub_reg_imm(XReg::x9, XReg::x9, 1);
            st_ctx32(XReg::x9, context_offsets::catch_depth());
            // x10 = &catch_bufs[catch_depth-1] = (x19 + cb_off) + x9*64
            materialize_ctx_addr(XReg::x10, context_offsets::catch_bufs());
            e.lsl_imm(XReg::x9, XReg::x9, 6);
            e.add_reg(XReg::x10, XReg::x10, XReg::x9);  // x10 = &buf
            // restore call_depth from catch_saved_call_depths[cd-1]:
            //   x11 = (x19 + csd_off) + (cd-1)*4 ; load 32 -> x9 ; st_ctx32 depth.
            ld_ctx32(XReg::x9, context_offsets::catch_depth());  // x9 = cd-1
            materialize_ctx_addr(XReg::x11, context_offsets::catch_saved_depths());
            e.lsl_imm(XReg::x9, XReg::x9, 2);
            e.add_reg(XReg::x11, XReg::x11, XReg::x9);
            e.ldur32(XReg::x9, XReg::x11, 0);
            st_ctx32(XReg::x9, context_offsets::depth());
            // load catch-entry PC into x9 BEFORE restoring regs/SP (buf addr in x10).
            e.ldur64(XReg::x9, XReg::x10, 40);  // x9 = catch PC
            // restore callee-saved regs.
            e.ldur64(XReg::x19, XReg::x10, 0);
            e.ldur64(XReg::x20, XReg::x10, 8);
            e.ldur64(XReg::x29, XReg::x10, 16);
            e.ldur64(XReg::x30, XReg::x10, 24);
            // restore SP LAST (switches to the catching frame's stack).
            // add sp, x11, #0 needs x11 = saved SP; load it from [buf+32] into x11.
            e.ldur64(XReg::x11, XReg::x10, 32);
            e.add_reg_imm(XReg::sp, XReg::x11, 0);  // sp = saved SP (reg 31 = SP)
            e.br(XReg::x9);  // jump to the catch-entry PC
            // no handler: trap (unhandled throw -> host checkpoint).
            e.bind(no_handler);
            emit_trap(int(TrapReason::UnhandledThrow),
                      "unhandled throw (no enclosing try/catch)");
            break;
        }
        }
    }

    // BoundsCheck: idx (src1) into x9, len (src2 vreg or imm) into x10.
    void emit_bounds_check_instr(const ThinInstr& in) {
        if (in.src1 != 0) load_int_vreg(in.src1);   // x9 = idx
        if (in.src2 != 0) {
            // load len into x10 (without disturbing x9). load_int_vreg writes
            // x9, so save idx to x11 first.
            e.mov_reg(XReg::x11, XReg::x9);          // x11 = idx
            load_int_vreg(in.src2);                   // x9 = len
            e.mov_reg(XReg::x10, XReg::x9);           // x10 = len
            e.mov_reg(XReg::x9, XReg::x11);           // x9 = idx
            emit_bounds_check_reg(XReg::x9, XReg::x10);
        } else {
            emit_bounds_check_imm(XReg::x9, in.imm.i);
        }
    }

    // ─── integer binary op ───
    void emit_int_binop(const ThinInstr& in) {
        const Type* ty = in.meta.type;
        bool is_unsigned = in.meta.is_unsigned != 0;
        load_int_vreg(in.src1);                 // x9 = lhs
        if (in.src2 == 0) {
            // immediate form: x10 = imm.i
            e.mov_reg_imm64(XReg::x10, in.imm.i);
        } else {
            // VReg form: save lhs (x9) in x11, load rhs into x9, move to x10,
            // restore lhs into x9.
            e.mov_reg(XReg::x11, XReg::x9);      // x11 = lhs
            load_int_vreg(in.src2);               // x9 = rhs
            e.mov_reg(XReg::x10, XReg::x9);       // x10 = rhs
            e.mov_reg(XReg::x9, XReg::x11);       // x9 = lhs
        }
        switch (in.op) {
        case ThinOp::Add: e.add_reg(XReg::x9, XReg::x9, XReg::x10); break;
        case ThinOp::Sub: e.sub_reg(XReg::x9, XReg::x9, XReg::x10); break;
        case ThinOp::Mul: e.mul_reg(XReg::x9, XReg::x9, XReg::x10); break;
        case ThinOp::And: e.and_reg(XReg::x9, XReg::x9, XReg::x10); break;
        case ThinOp::Or:  e.orr_reg(XReg::x9, XReg::x9, XReg::x10); break;
        case ThinOp::Xor: e.eor_reg(XReg::x9, XReg::x9, XReg::x10); break;
        case ThinOp::Shl: e.lsl_reg(XReg::x9, XReg::x9, XReg::x10); break;
        case ThinOp::Shr:
            if (is_unsigned) e.lsr_reg(XReg::x9, XReg::x9, XReg::x10);
            else             e.asr_reg(XReg::x9, XReg::x9, XReg::x10);
            break;
        default: break;
        }
        normalize_x9(ty);
        record_dst_x9(in.dst, ty);
        pin_int_dst(in.dst, in.meta, ty);
    }

    // ─── integer div/mod (with guards) ───
    // x9 = dividend (src1), x10 = divisor (src2 or imm). Preserve dividend
    // across the divide for Mod (msub needs it).
    void emit_int_divmod_instr(const ThinInstr& in, bool want_mod) {
        bool is_unsigned = in.meta.is_unsigned != 0;
        load_int_vreg(in.src1);                 // x9 = dividend
        if (in.src2 == 0) {
            e.mov_reg_imm64(XReg::x10, in.imm.i);
        } else {
            e.mov_reg(XReg::x11, XReg::x9);      // x11 = dividend (saved)
            load_int_vreg(in.src2);               // x9 = divisor
            e.mov_reg(XReg::x10, XReg::x9);       // x10 = divisor
            e.mov_reg(XReg::x9, XReg::x11);       // x9 = dividend
        }
        // For Mod, we need the dividend AFTER the divide; save it in x11.
        e.mov_reg(XReg::x11, XReg::x9);          // x11 = dividend (preserved)
        // div-by-zero guard
        Label nonzero = e.alloc_label();
        e.cmp_reg_imm(XReg::x10, 0);
        e.b_cond(ArmCond::ne, nonzero);
        emit_trap(int(TrapReason::DivByZero), "integer division by zero");
        e.bind(nonzero);
        if (!is_unsigned) {
            // signed overflow: dividend == INT64_MIN && divisor == -1
            e.mov_reg_imm64(XReg::x12, -1);
            Label not_minus1 = e.alloc_label(), safe = e.alloc_label();
            e.cmp_reg(XReg::x10, XReg::x12);
            e.b_cond(ArmCond::ne, not_minus1);
            // divisor == -1: check dividend == INT64_MIN
            e.mov_reg_imm64(XReg::x12, int64_t(INT64_MIN));
            Label overflow = e.alloc_label();
            e.cmp_reg(XReg::x9, XReg::x12);
            e.b_cond(ArmCond::eq, overflow);
            e.b(safe);
            e.bind(overflow);
            emit_trap(int(TrapReason::DivByZero), "signed division overflow");
            e.bind(not_minus1);
            e.bind(safe);
            // quotient = sdiv x12, x9, x10
            e.sdiv_reg(XReg::x12, XReg::x9, XReg::x10);
        } else {
            // quotient = udiv x12, x9, x10
            e.udiv_reg(XReg::x12, XReg::x9, XReg::x10);
        }
        if (want_mod) {
            // remainder = dividend - (divisor * quotient)
            // msub Xd, Xn, Xm, Xa: Xd = Xa - Xn*Xm
            // Xd=x9, Xn=x10(divisor), Xm=x12(quotient), Xa=x11(dividend)
            e.msub_reg(XReg::x9, XReg::x10, XReg::x12, XReg::x11);
        } else {
            e.mov_reg(XReg::x9, XReg::x12);
        }
        normalize_x9(in.meta.type);
        record_dst_x9(in.dst, in.meta.type);
        pin_int_dst(in.dst, in.meta, in.meta.type);
    }

    // ─── float binary op (fadd/fsub/fmul/fdiv; FMod calls host fmod/fmodf) ───
    // ARM64 has native fadd/fsub/fmul/fdiv (f32 + f64); there is NO fmod
    // instruction, so FMod marshals the operands into v0/v1 (AAPCS64 FP args)
    // and calls the host fmod (f64) / fmodf (f32) native via blr. The dividend
    // is src1, the divisor is src2 — matching emit_x64's FMod (which also
    // expects the lowering to use CallNative for %, but when FMod reaches emit
    // the semantics are dividend % divisor).
    void emit_float_binop(const ThinInstr& in) {
        bool is_f32 = (in.meta.is_f32 != 0);
        if (in.op == ThinOp::FMod) {
            emit_fmod(in, is_f32);
            return;
        }
        // eval src1 -> v0; src2 -> v1 (each into its OWN FP reg — no clobber).
        load_float_vreg(in.src1);                       // v0 = lhs
        if (in.src2 == 0) {
            load_float_imm_into(ArmVReg::v1, in.imm.f, is_f32);  // v1 = imm rhs
        } else {
            load_float_vreg_into(ArmVReg::v1, in.src2);          // v1 = rhs
        }
        switch (in.op) {
        case ThinOp::FAdd:
            if (is_f32) e.fadd_f32(ArmVReg::v0, ArmVReg::v0, ArmVReg::v1);
            else        e.fadd_f64(ArmVReg::v0, ArmVReg::v0, ArmVReg::v1);
            break;
        case ThinOp::FMul:
            if (is_f32) e.fmul_f32(ArmVReg::v0, ArmVReg::v0, ArmVReg::v1);
            else        e.fmul_f64(ArmVReg::v0, ArmVReg::v0, ArmVReg::v1);
            break;
        case ThinOp::FSub:
            // v0 = v0 - v1 (ARM fsub is Vd = Vn - Vm, so Vd=v0, Vn=v0, Vm=v1)
            if (is_f32) e.fsub_f32(ArmVReg::v0, ArmVReg::v0, ArmVReg::v1);
            else        e.fsub_f64(ArmVReg::v0, ArmVReg::v0, ArmVReg::v1);
            break;
        case ThinOp::FDiv:
            // v0 = v0 / v1 (ARM fdiv is Vd = Vn / Vm, so Vd=v0, Vn=v0, Vm=v1)
            if (is_f32) e.fdiv_f32(ArmVReg::v0, ArmVReg::v0, ArmVReg::v1);
            else        e.fdiv_f64(ArmVReg::v0, ArmVReg::v0, ArmVReg::v1);
            break;
        default: break;
        }
        record_dst_v0(in.dst, in.meta.type);
        pin_float_dst(in.dst, in.meta, in.meta.type);
    }

    // FMod: call host fmod (f64) / fmodf (f32). AAPCS64 passes FP args in
    // v0/v1 and returns the FP result in v0. The host fmod/fmodf is a standard
    // C library function (callee-preserved per AAPCS64); we load its address
    // into x9 (a caller-saved scratch — safe to clobber across the call) and
    // blr. SP stays 16-byte aligned (no outgoing stack args). The result is in
    // v0 -> pin_float_dst.
    void emit_fmod(const ThinInstr& in, bool is_f32) {
        // Marshal args: dividend (src1) -> v0, divisor (src2) -> v1.
        load_float_vreg(in.src1);                       // v0 = dividend
        if (in.src2 == 0) {
            load_float_imm_into(ArmVReg::v1, in.imm.f, is_f32);  // v1 = imm divisor
        } else {
            load_float_vreg_into(ArmVReg::v1, in.src2);          // v1 = divisor
        }
        // x9 = &fmod (f64) or the float overload (f32). Bake the host fn ptr
        // directly — fmod is a libc symbol, always present on the host. (The
        // mov_reg_imm64 bakes a JIT-time ptr; non-serializable but the whole
        // JIT is process-local.) std::fmod is overloaded (float/double/long
        // double), so take the address via a typed function-pointer variable
        // to disambiguate the overload. NOTE: use std::fmod (overloaded) for
        // BOTH widths — std::fmodf is not guaranteed by <cmath> on all libstdc++
        // (g++ strict rejects it); std::fmod(float,float) is the portable float form.
        if (is_f32) {
            float (*fp)(float, float) = static_cast<float(*)(float, float)>(&std::fmod);
            e.mov_reg_imm64(XReg::x9, int64_t(reinterpret_cast<uintptr_t>(fp)));
        } else {
            double (*fp)(double, double) = &std::fmod;
            e.mov_reg_imm64(XReg::x9, int64_t(reinterpret_cast<uintptr_t>(fp)));
        }
        e.blr(XReg::x9);
        // result in v0 -> pin_float_dst. (No depth-leave: FMod is not a
        // script/native call in the ThinIR sense — it's a host-libc helper.)
        record_dst_v0(in.dst, in.meta.type);
        pin_float_dst(in.dst, in.meta, in.meta.type);
    }

    // ─── compare (cmp + cset) ───
    void emit_cmp(const ThinInstr& in) {
        const Type* ty = in.meta.type;
        bool is_float = (in.meta.is_f32 != 0) || (ty && ty->is_float());
        uint8_t cmp_pred = in.meta.cmp;  // 0=Eq,1=Neq,2=Lt,3=Le,4=Gt,5=Ge
        if (is_float) {
            // ARM fcmp sets NZCV with V=1 for unordered (NaN) operands. The
            // advisor-confirmed condition mapping (lt/le treat unordered as
            // true, which is WRONG for ember's float < / <= — they must be
            // false on NaN): == -> eq, != -> ne, < -> mi, <= -> ls, > -> gt,
            // >= -> ge. The Cmp result is an int bool (0/1), NOT a float.
            bool is_f32 = (in.meta.is_f32 != 0);
            load_float_vreg(in.src1);                       // v0 = lhs
            if (in.src2 == 0) {
                load_float_imm_into(ArmVReg::v1, in.imm.f, is_f32);  // v1 = imm rhs
            } else {
                load_float_vreg_into(ArmVReg::v1, in.src2);          // v1 = rhs
            }
            if (is_f32) e.fcmp_f32(ArmVReg::v0, ArmVReg::v1);
            else        e.fcmp_f64(ArmVReg::v0, ArmVReg::v1);
            ArmCond cc;
            switch (cmp_pred) {
            case 0: cc = ArmCond::eq; break;   // Eq
            case 1: cc = ArmCond::ne; break;   // Neq
            case 2: cc = ArmCond::mi; break;   // Lt  (NOT lt — mi is false on unordered)
            case 3: cc = ArmCond::ls; break;   // Le  (NOT le — ls is false on unordered)
            case 4: cc = ArmCond::gt; break;   // Gt
            case 5: cc = ArmCond::ge; break;   // Ge
            default: cc = ArmCond::eq; break;
            }
            e.cset(XReg::x9, cc);   // x9 = (cond) ? 1 : 0
            const Type* resty = in.meta.type ? in.meta.type : &type_bool();
            record_dst_x9(in.dst, resty);
            pin_int_dst(in.dst, in.meta, resty);
            return;
        }
        bool is_unsigned = in.meta.is_unsigned != 0;
        load_int_vreg(in.src1);                 // x9 = lhs
        if (in.src2 == 0) {
            e.mov_reg_imm64(XReg::x10, in.imm.i);
        } else {
            e.mov_reg(XReg::x11, XReg::x9);      // x11 = lhs
            load_int_vreg(in.src2);               // x9 = rhs
            e.mov_reg(XReg::x10, XReg::x9);       // x10 = rhs
            e.mov_reg(XReg::x9, XReg::x11);       // x9 = lhs
        }
        e.cmp_reg(XReg::x9, XReg::x10);
        ArmCond cc;
        switch (cmp_pred) {
        case 0: cc = ArmCond::eq; break;                       // Eq
        case 1: cc = ArmCond::ne; break;                       // Neq
        case 2: cc = is_unsigned ? ArmCond::cc : ArmCond::lt; break;  // Lt
        case 3: cc = is_unsigned ? ArmCond::ls : ArmCond::le; break;  // Le
        case 4: cc = is_unsigned ? ArmCond::hi : ArmCond::gt; break;  // Gt
        case 5: cc = is_unsigned ? ArmCond::cs : ArmCond::ge; break;  // Ge
        default: cc = ArmCond::eq; break;
        }
        e.cset(XReg::x9, cc);   // x9 = (cond) ? 1 : 0
        const Type* resty = in.meta.type ? in.meta.type : &type_bool();
        record_dst_x9(in.dst, resty);
        pin_int_dst(in.dst, in.meta, resty);
    }

    // ─── short-circuit logical (LAnd / LOr) ───
    void emit_logical(const ThinInstr& in) {
        bool is_and = (in.op == ThinOp::LAnd);
        Label false_l = e.alloc_label(), end_l = e.alloc_label();
        load_int_vreg(in.src1);
        e.cmp_reg_imm(XReg::x9, 0);
        if (is_and) e.b_cond(ArmCond::eq, false_l);   // LAnd: lhs false -> result false
        else        e.b_cond(ArmCond::ne, end_l);     // LOr: lhs true -> result true
        load_int_vreg(in.src2);
        e.cmp_reg_imm(XReg::x9, 0);
        e.b_cond(ArmCond::ne, end_l);  // rhs true -> 1
        e.bind(false_l);
        e.mov_reg_imm64(XReg::x9, 0);
        e.bind(end_l);
        // normalize: x9 = (x9 != 0) ? 1 : 0
        e.cmp_reg_imm(XReg::x9, 0);
        Label done = e.alloc_label();
        e.mov_reg_imm64(XReg::x9, 0);
        e.b_cond(ArmCond::eq, done);
        e.mov_reg_imm64(XReg::x9, 1);
        e.bind(done);
        const Type* resty = in.meta.type ? in.meta.type : &type_bool();
        record_dst_x9(in.dst, resty);
        pin_int_dst(in.dst, in.meta, resty);
    }

    // ─── cast (int<->int width; int<->float + f32<->f64 throw) ───
    void emit_cast(const ThinInstr& in) {
        const Type* from = vreg_type(in.src1);
        const Type* to = in.meta.type;
        if (!from) from = in.meta.type;
        const bool plain_from_int = from && from->is_int() && !from->is_fn_handle && from->struct_name.empty();
        const bool plain_to_int = to && to->is_int() && !to->is_fn_handle && to->struct_name.empty();
        if (from && to && from->same(*to)) {
            load_int_vreg(in.src1);
            record_dst_x9(in.dst, to);
            pin_int_dst(in.dst, in.meta, to);
            return;
        }
        if (plain_from_int && plain_to_int) {
            load_int_vreg(in.src1);
            normalize_x9(to);   // sign/zero-extend to the target width
            record_dst_x9(in.dst, to);
            pin_int_dst(in.dst, in.meta, to);
            return;
        }
        if (from && to && from->is_float() && to->is_float()) {
            // f32<->f64: fcvt_s32d (d->s) / fcvt_d32 (s->d). Result in v0.
            load_float_vreg(in.src1);   // v0 = src (in its width)
            if (from->prim == Prim::F32 && to->prim == Prim::F64)
                e.fcvt_d32(ArmVReg::v0, ArmVReg::v0);   // s -> d
            else if (from->prim == Prim::F64 && to->prim == Prim::F32)
                e.fcvt_s32d(ArmVReg::v0, ArmVReg::v0);  // d -> s
            // same-width float cast is a no-op (the lowering rejects it, but
            // be safe).
            record_dst_v0(in.dst, to);
            pin_float_dst(in.dst, in.meta, to);
            return;
        }
        if (plain_from_int && to && to->is_float()) {
            // int->float (signed; unsigned->float is rejected at sema).
            // normalize the int to its width, then scvtf_*_x (64-bit source)
            // — matches emit_x64's normalize + 64-bit cvtsi2sd/ss.
            load_int_vreg(in.src1);    // x9 = src (normalized)
            normalize_x9(from);
            bool to_f32 = to->prim == Prim::F32;
            if (to_f32) e.scvtf_f32_x(ArmVReg::v0, XReg::x9);
            else        e.scvtf_f64_x(ArmVReg::v0, XReg::x9);
            record_dst_v0(in.dst, to);
            pin_float_dst(in.dst, in.meta, to);
            return;
        }
        if (from && from->is_float() && plain_to_int) {
            // float->int (truncating; float->u* rejected at sema).
            // fcvtzs_x_f32/f64 truncates toward zero -> 64-bit x9; then
            // normalize_x9(to) sign/zero-extends to the target width.
            // Matches emit_x64's cvttsd2si + normalize_rax.
            load_float_vreg(in.src1);   // v0 = src
            bool from_f32 = from->prim == Prim::F32;
            if (from_f32) e.fcvtzs_x_f32(XReg::x9, ArmVReg::v0);
            else          e.fcvtzs_x_f64(XReg::x9, ArmVReg::v0);
            normalize_x9(to);
            record_dst_x9(in.dst, to);
            pin_int_dst(in.dst, in.meta, to);
            return;
        }
        // unknown cast
        emit_trap(int(TrapReason::IllegalInstruction), "internal: invalid cast reached emit");
    }

    // ─── call emission ───
    void emit_call(const ThinInstr& in) {
        // Does this call return a struct by ptr (hidden dest in x8)? On ARM64
        // ember uses the Win64-style hidden-ptr convention for struct/fixed-array
        // returns (the lowerer sets returns_struct_by_ptr + a __struct_ret_ptr
        // param). A `string` is an opaque i64 HANDLE (Prim::I64 +
        // struct_name="string"), NOT a struct — it returns in x0 like an i64.
        // So ret_struct is true ONLY for a registered struct or a fixed-array
        // return (composite value types), not for string/fn-handle scalars.
        bool ret_struct = is_registered_struct(in.ret_type, structs()) ||
                          (in.ret_type && in.ret_type->array_len > 0);
        // Marshal args into x0-x7 (GP) + v0-v7 (FP) + x8 (hidden dest if ret_struct).
        marshal_call_args(in, ret_struct);

        if (in.op == ThinOp::CallNative) {
            emit_native_call(in);
        } else if (in.op == ThinOp::CallIndirect) {
            emit_indirect_call(in);
        } else if (in.op == ThinOp::CallCrossModule) {
            emit_cross_module_call(in);
        } else {  // CallScript
            emit_script_call(in);
        }

        // Any call clobbers x0-x18 (caller-saved) + v0-v7; reset tracking.
        x9_vreg = 0;
        v0_vreg = 0;

        if (ret_struct) {
            // struct-by-ptr: the callee wrote through x8 (the dest ptr); no
            // register result. The dst represents the struct at the dest frame
            // slot. Record the dst's frame_off as the dest slot so subsequent
            // LoadFrame/FieldAddr of dst resolve to it.
            if (in.dst != 0) {
                // The dest slot is encoded in args[0]: either a vreg (the dest
                // ptr's source — but for a plain local dest it's the sentinel)
                // or arg_frame_offs[0]. Use arg_frame_offs[0] when available.
                int32_t afo0 = in.arg_frame_offs.empty() ? -1 : in.arg_frame_offs[0];
                VReg a0 = in.args.empty() ? 0 : in.args[0];
                if (a0 == 0 && afo0 != -1) {
                    vregs[in.dst].frame_off = afo0;
                    vregs[in.dst].type = in.ret_type;
                } else if (a0 != 0) {
                    // dest ptr was in a vreg; the struct lives at that ptr. We
                    // cannot frame-back it from a register ptr alone; record
                    // the type only (the caller will reload via the ptr vreg).
                    vregs[in.dst].type = in.ret_type;
                }
            }
            return;
        }
        // Record the result (scalar int / float / slice).
        if (in.dst != 0) {
            if (in.ret_type && in.ret_type->is_float()) {
                // float result: AAPCS64 returns it in v0 -> pin_float_dst.
                record_dst_v0(in.dst, in.ret_type);
                pin_float_dst(in.dst, in.meta, in.ret_type);
                return;
            }
            if (in.ret_type && (in.ret_type->is_slice || in.ret_type->is_lambda)) {
                // slice result: AAPCS64 returns {ptr=x0, len=x1}. pin_slice_dst.
                pin_slice_dst(in.dst, in.meta, in.ret_type, XReg::x0, XReg::x1);
                return;
            }
            // int/bool result in x0 -> x9 -> frame slot
            e.mov_reg(XReg::x9, XReg::x0);
            if (in.ret_type && in.ret_type->is_int()) normalize_x9(in.ret_type);
            record_dst_x9(in.dst, in.ret_type);
            pin_int_dst(in.dst, in.meta, in.ret_type);
        }
    }

    // Emit a native call: load the fn ptr (relocatable-by-name) into x11, blr.
    // Depth leave balances the DepthCheck that precedes the call.
    void emit_native_call(const ThinInstr& in) {
        const std::string& name = in.meta.native_name;
        void* target = in.native_fn;
        if (!target && !name.empty()) target = resolve_native_ptr(name);
        if (name.empty() || !in.ret_type || in.arg_types.empty()) {
            if (non_serializable_reason.empty())
                non_serializable_reason = "native call has no complete symbolic NativeSig binding";
            e.mov_reg_imm64(XReg::x11, int64_t(target));
        } else {
            // relocatable-by-name load: a literal-pool cell recorded as a
            // NativeFixup. The cell is filled with `target` at finalize. The
            // Arm64Emitter defers native_fixups_ population to resolve_fixups
            // (the cell offset is not known until the literal pool is laid out),
            // so we record the binding now and pair it with its cell offset
            // AFTER resolve_fixups (same emission order).
            e.mov_reg_native(XReg::x11, name);
            PendingNative pn;
            pn.binding.code_offset = 0;  // patched after resolve_fixups
            pn.binding.name = name;
            pn.binding.ret = *in.ret_type;
            for (const Type* t : in.arg_types) pn.binding.params.push_back(*t);
            pn.target = target;
            pending_natives.push_back(std::move(pn));
        }
        e.blr(XReg::x11);
        emit_depth_leave();
    }

    // Emit a script call: resolve the dispatch slot to the entry ptr, blr.
    // ldr_literal_ptr x11, DispatchTableBase ; ldr64 x11, [x11, slot*8] ; blr x11.
    void emit_script_call(const ThinInstr& in) {
        e.ldr_literal_ptr(XReg::x11, AbsFixup::DispatchTableBase);
        // load the slot's entry ptr. slot*8 must fit ldr64's scaled imm12
        // (offset/8 <= 4095 -> slot <= 4095). For larger slot counts,
        // materialize the offset.
        uint32_t slot = uint32_t(in.meta.slot);
        if (slot <= 0xFFF) {
            e.ldr64(XReg::x11, XReg::x11, slot);   // imm12 = (slot*8)/8 = slot
        } else {
            e.mov_reg_imm64(XReg::x10, int64_t(slot) * 8);
            e.add_reg(XReg::x11, XReg::x11, XReg::x10);
            e.ldr64(XReg::x11, XReg::x11, 0);
        }
        e.blr(XReg::x11);
        emit_depth_leave();
    }
    // CallIndirect: the call target is a RUNTIME fn handle (in.src1 = a vreg
    // holding a dispatch-slot index, validated by the preceding CallTargetGuard).
    // Dispatch via DispatchTableBase + handle*8 -> entry ptr -> blr. The handle
    // is loaded AFTER arg marshaling (args are already in x0-x7; the handle is a
    // separate vreg into x10). plan_MACOS_ARM64.md Phase 8.
    //
    // v1.0 Tier 2 cross-module handles (plan_MACOS_ARM64.md Phase 8): if the
    // per-module records table is configured, test bit 63 of the handle. A
    // cross-module handle (bit 63 set) dispatches via the handle-records table,
    // mirroring the tree-walker's emit_cross_module_indirect_dispatch:
    //   handle = (1<<63) | (module_id << 32) | slot
    //   x9  = slot = handle & 0xFFFFFFFF
    //   x12 = mod_id = (handle >> 32) & 0x7FFFFFFF
    //   range-check mod_id < records_count  -> trap
    //   x11 = handle_records_base + mod_id*24  (record_ptr)
    //   range-check slot < [x11+16] (slot_count)  -> trap
    //   bt [x11+8] (allowlist_base), slot  -> trap if bit clear
    //   x11 = [x11+0] (dispatch_base) ; x11 = [x11 + slot*8] (callee entry) ; blr
    // The records/allowlist bases are process-local (raw imm64, NOT relocs) so
    // this is non-serializable to .em (same constraint as the intra allowlist).
    // When the records table is NOT configured, only the intra path is emitted
    // (byte-identical to the pre-cross-module code).
    void emit_indirect_call(const ThinInstr& in) {
        // load the handle (src1) into x10. load_int_vreg writes x9; move to x10
        // so the dispatch math doesn't collide with a future x9 use.
        load_int_vreg(in.src1);          // x9 = handle (slot index)
        e.mov_reg(XReg::x10, XReg::x9);
        const bool cross_aware = (ctx.module_handle_records_base != 0);
        Label cross, after, xtrap;
        if (cross_aware) {
            cross = e.alloc_label();
            after = e.alloc_label();
            xtrap = e.alloc_label();
            // test bit 63: lsr x9, x10, 63 ; cbnz x9, cross
            e.lsr_imm(XReg::x9, XReg::x10, 63);
            e.cbnz64(XReg::x9, cross);
        }
        // intra: x11 = [dispatch_base + handle*8]
        e.ldr_literal_ptr(XReg::x11, AbsFixup::DispatchTableBase);
        e.lsl_imm(XReg::x10, XReg::x10, 3);     // x10 = handle * 8
        e.add_reg(XReg::x11, XReg::x11, XReg::x10); // x11 = &dispatch_table[handle]
        e.ldr64(XReg::x11, XReg::x11, 0);       // x11 = entry ptr
        if (cross_aware) {
            e.b(after);
            // ---- cross: extract mod_id + slot, validate via records table ----
            // x10 = handle. Register allocation (mirrors the tree-walker):
            //   x9  = slot (kept for the final lea+load)
            //   x11 = record_ptr (kept for field loads)
            //   x12 = scratch (mod_id -> slot_count -> allowlist_base -> dispatch_base)
            //   x13 = scratch (allowlist byte / bit index)
            e.bind(cross);
            // x9 = slot = handle & 0xFFFFFFFF (zero-extend low 32). lsl 32 then
            // lsr 32 clears bits 32-63 (the flag + mod_id).
            e.mov_reg(XReg::x9, XReg::x10);
            e.lsl_imm(XReg::x9, XReg::x9, 32);
            e.lsr_imm(XReg::x9, XReg::x9, 32);   // x9 = slot (low 32)
            // x12 = mod_id = (handle >> 32) & 0x7FFFFFFF. shr 32 brings bit 63
            // (the cross-module flag) into bit 31; lsl 33 then lsr 33 clears it.
            e.lsr_imm(XReg::x12, XReg::x10, 32); // x12 = handle >> 32 (bit 31 = flag)
            e.lsl_imm(XReg::x12, XReg::x12, 33);
            e.lsr_imm(XReg::x12, XReg::x12, 33); // x12 = mod_id (low 31)
            // Range-check mod_id < records_count (unsigned). Materialize the
            // count (process-local imm64) + cmp + b.hs trap.
            e.mov_reg_imm64(XReg::x13, int64_t(ctx.module_handle_records_count));
            e.cmp_reg(XReg::x12, XReg::x13);
            e.b_cond(ArmCond::cs, xtrap);        // mod_id >= count -> out of range
            // x11 = handle_records_base + mod_id*24 (record_ptr). mod_id*24 via
            // mul (mod_id * 24); add to the records base.
            e.mov_reg_imm64(XReg::x11, ctx.module_handle_records_base);
            e.mov_reg_imm64(XReg::x13, 24);
            e.mul_reg(XReg::x13, XReg::x12, XReg::x13);  // x13 = mod_id * 24
            e.add_reg(XReg::x11, XReg::x11, XReg::x13);  // x11 = record_ptr
            // Range-check slot < slot_count = [record_ptr + 16].
            e.ldur64(XReg::x12, XReg::x11, 16);   // x12 = slot_count
            e.cmp_reg(XReg::x9, XReg::x12);
            e.b_cond(ArmCond::cs, xtrap);         // slot >= slot_count -> trap
            // Allowlist bit test: byte = [allowlist_base + (slot >> 3)];
            // bit = slot & 7; (byte >> bit) & 1 -> 0 means not registered -> trap.
            e.ldur64(XReg::x12, XReg::x11, 8);    // x12 = allowlist_base
            e.mov_reg(XReg::x13, XReg::x9);
            e.lsr_imm(XReg::x13, XReg::x13, 3);   // x13 = slot >> 3 (byte offset)
            e.add_reg(XReg::x12, XReg::x12, XReg::x13);
            e.ldurb(XReg::x12, XReg::x12, 0);     // x12 = allowlist byte
            // bit index = slot & 7 (lsl 61 then lsr 61 zero-extends low 3).
            e.mov_reg(XReg::x13, XReg::x9);
            e.lsl_imm(XReg::x13, XReg::x13, 61);
            e.lsr_imm(XReg::x13, XReg::x13, 61);  // x13 = slot & 7
            e.lsr_reg(XReg::x12, XReg::x12, XReg::x13);  // x12 = byte >> bit
            // isolate bit 0: (byte >> bit) & 1. Without this mask a set HIGHER
            // bit in the same byte would make cbz skip the trap + authorize a
            // cross-module slot whose own bit is clear (provenance bypass).
            e.lsl_imm(XReg::x12, XReg::x12, 63);
            e.lsr_imm(XReg::x12, XReg::x12, 63);
            e.cbz64(XReg::x12, xtrap);           // bit clear -> not registered
            // Dispatch: dispatch_base = [record_ptr + 0]; x11 = [dispatch_base + slot*8].
            e.ldur64(XReg::x11, XReg::x11, 0);    // x11 = dispatch_base
            e.lsl_imm(XReg::x9, XReg::x9, 3);     // x9 = slot * 8
            e.add_reg(XReg::x11, XReg::x11, XReg::x9);
            e.ldr64(XReg::x11, XReg::x11, 0);     // x11 = callee entry
            e.b(after);                           // valid cross path -> join blr
            // cross-module trap block (only reached via the b_cond / cbz branches
            // above; the valid intra + cross paths jump to `after` past it).
            e.bind(xtrap);
            emit_trap(int(TrapReason::BadCallTarget),
                      "cross-module call-target provenance: handle is not a registered function in the target module");
            e.bind(after);                        // valid paths land here
            e.blr(XReg::x11);
            emit_depth_leave();
            return;  // emit_trap longjmps (never returns); keep the shape clean
        }
        e.blr(XReg::x11);
        emit_depth_leave();
    }
    // Whether the caller is keyed (has a keyed_dispatch descriptor). Mirrors
    // emit_x64's keyed_caller(). plan_MACOS_ARM64.md Phase 8.
    bool keyed_caller() const { return ctx.keyed_dispatch != nullptr; }
    // CallCrossModule: cross-module call via the module registry. Legacy/identity
    // mode = registry hop: ModuleRegistryBase -> [mod_id*8] = the target module's
    // dispatch table -> [slot*8] = the entry ptr -> blr. The keyed paths (keyed
    // caller -> keyed target via ember_resolve_keyed_dispatch) need the Darwin
    // ARM64 keyed thunks (Phase 8 tail) + are rejected here with a clear message.
    void emit_cross_module_call(const ThinInstr& in) {
        // Red 7: reject legacy caller -> keyed target (no runtime-key contract).
        if (!keyed_caller() && in.meta.cross_module_target_mode == 1) {
            emit_trap(int(TrapReason::BadCallTarget),
                      "cross-module call rejected: legacy caller cannot call keyed target");
            emit_depth_leave();
            if (non_serializable_reason.empty())
                non_serializable_reason = "legacy-to-keyed cross-module call rejected at codegen";
            return;
        }
        // Keyed caller -> keyed target: needs the keyed resolver (Darwin ARM64
        // keyed thunks — Phase 8 tail). Not yet supported on arm64.
        if (keyed_caller() && in.meta.cross_module_target_mode == 1) {
            emit_trap(int(TrapReason::BadCallTarget),
                      "emit_arm64: keyed cross-module call not yet supported (Phase 8 keyed thunks)");
            emit_depth_leave();
            return;
        }
        // Legacy / keyed->identity: registry hop.
        e.ldr_literal_ptr(XReg::x11, AbsFixup::ModuleRegistryBase);
        uint32_t mod_id = uint32_t(in.meta.mod_id);
        if (mod_id <= 0xFFF) {
            e.ldr64(XReg::x11, XReg::x11, mod_id);   // [registry + mod_id*8] = target dispatch table
        } else {
            e.mov_reg_imm64(XReg::x10, int64_t(mod_id) * 8);
            e.add_reg(XReg::x11, XReg::x11, XReg::x10);
            e.ldr64(XReg::x11, XReg::x11, 0);
        }
        uint32_t slot = uint32_t(in.meta.slot);
        if (slot <= 0xFFF) {
            e.ldr64(XReg::x11, XReg::x11, slot);     // [table + slot*8] = entry ptr
        } else {
            e.mov_reg_imm64(XReg::x10, int64_t(slot) * 8);
            e.add_reg(XReg::x11, XReg::x11, XReg::x10);
            e.ldr64(XReg::x11, XReg::x11, 0);
        }
        e.blr(XReg::x11);
        emit_depth_leave();
    }

    // ─── global slot load/store (base in a reg, byte offset add) ───
    // AAPCS64 globals are addressed [base + offset]. offset may be > 255; use
    // scaled ldr64/str64 when offset/8 fits imm12, else materialize.
    void load_global_slot(XReg dst, XReg base, int32_t off) {
        if (off >= 0 && (off % 8 == 0) && uint32_t(off) / 8 <= 0xFFF) {
            e.ldr64(dst, base, uint32_t(off) / 8);
        } else if (off >= 0 && off <= 0xFFF) {
            // try 32-bit scaled etc.; for 8-byte globals use materialize
            e.mov_reg_imm64(XReg::x10, int64_t(off));
            e.add_reg(XReg::x10, base, XReg::x10);
            e.ldr64(dst, XReg::x10, 0);
        } else {
            e.mov_reg_imm64(XReg::x10, int64_t(off));
            e.add_reg(XReg::x10, base, XReg::x10);
            e.ldr64(dst, XReg::x10, 0);
        }
    }
    void store_global_slot(XReg src, XReg base, int32_t off) {
        if (off >= 0 && (off % 8 == 0) && uint32_t(off) / 8 <= 0xFFF) {
            e.str64(src, base, uint32_t(off) / 8);
        } else {
            e.mov_reg_imm64(XReg::x10, int64_t(off));
            e.add_reg(XReg::x10, base, XReg::x10);
            e.str64(src, XReg::x10, 0);
        }
    }
    // FP global load/store — mirrors load_global_slot/store_global_slot but
    // uses ldr_f32/f64 / str_f32/f64 (scaled: imm12 = off/4 for f32, /8 for
    // f64). The FP value stays in the ArmVReg; x10 materializes the address.
    void load_global_slot_f32(ArmVReg dst, XReg base, int32_t off) {
        if (off >= 0 && (off % 4 == 0) && uint32_t(off) / 4 <= 0xFFF) {
            e.ldr_f32(dst, base, uint32_t(off) / 4);
        } else {
            e.mov_reg_imm64(XReg::x10, int64_t(off));
            e.add_reg(XReg::x10, base, XReg::x10);
            e.ldr_f32(dst, XReg::x10, 0);
        }
    }
    void load_global_slot_f64(ArmVReg dst, XReg base, int32_t off) {
        if (off >= 0 && (off % 8 == 0) && uint32_t(off) / 8 <= 0xFFF) {
            e.ldr_f64(dst, base, uint32_t(off) / 8);
        } else {
            e.mov_reg_imm64(XReg::x10, int64_t(off));
            e.add_reg(XReg::x10, base, XReg::x10);
            e.ldr_f64(dst, XReg::x10, 0);
        }
    }
    void store_global_slot_f32(ArmVReg src, XReg base, int32_t off) {
        if (off >= 0 && (off % 4 == 0) && uint32_t(off) / 4 <= 0xFFF) {
            e.str_f32(src, base, uint32_t(off) / 4);
        } else {
            e.mov_reg_imm64(XReg::x10, int64_t(off));
            e.add_reg(XReg::x10, base, XReg::x10);
            e.str_f32(src, XReg::x10, 0);
        }
    }
    void store_global_slot_f64(ArmVReg src, XReg base, int32_t off) {
        if (off >= 0 && (off % 8 == 0) && uint32_t(off) / 8 <= 0xFFF) {
            e.str_f64(src, base, uint32_t(off) / 8);
        } else {
            e.mov_reg_imm64(XReg::x10, int64_t(off));
            e.add_reg(XReg::x10, base, XReg::x10);
            e.str_f64(src, XReg::x10, 0);
        }
    }
};

} // anon namespace

CompiledFn emit_arm64(const ThinFunction& thf, const CodeGenCtx& ctx) {
    EmitCtx ec(thf, ctx);
    CompiledFn out = ec.emit();
    if (const char* dv = std::getenv("EMBER_DUMP_ARM64")) {
        std::string pat = dv ? dv : "";
        if (!pat.empty() && thf.name.find(pat) != std::string::npos) {
            std::FILE* f = std::fopen(("/tmp/ember_arm64_" + thf.name + ".ir").c_str(), "w");
            if (f) { std::fprintf(f, "%s", dump(thf).c_str()); std::fclose(f); }
            f = std::fopen(("/tmp/ember_arm64_" + thf.name + ".bin").c_str(), "wb");
            if (f) { std::fwrite(out.bytes.data(), 1, out.bytes.size(), f); std::fclose(f); }
            std::fprintf(stderr, "[EMBER_DUMP_ARM64] %s: %zu bytes, frame_size=%d\n",
                         thf.name.c_str(), out.bytes.size(), thf.frame.frame_size);
        }
    }
    return out;
}

} // namespace ember
