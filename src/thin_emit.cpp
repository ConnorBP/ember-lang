// thin_emit.cpp — Stage A c3: the ThinFunction -> x86-64 emit pass.
//
// See thin_emit.hpp for the contract + the VReg materialization model. This
// file reproduces the tree-walker's (src/codegen.cpp compile_func / eval) byte
// sequences keyed off ThinOp, producing a CompiledFn whose JIT'd execution is
// value-equivalent to the tree-walker JIT'ing the same source.
//
// LAZY MODE: no optimization beyond the tree-walker. Value-equivalence, not
// byte-identity. The peephole + AbsFixup/native-fixup lists are correct so the
// .em serializer + Stage 1 peephole compose with the IR path.

#include "thin_emit.hpp"
#include "codegen.hpp"    // CodeGenCtx, GlobalsBlock, ObfOptions
#include "engine.hpp"     // CompiledFn, CompiledNativeBinding
#include "context.hpp"    // TrapReason, context_offsets
#include "peephole.hpp"   // PeepholeGuardedRegions, make_stage1_pipeline, PeepholeCtx
#include "x64_emitter.hpp"
#include "thin_ir.hpp"
#include "ast.hpp"

#include <cassert>
#include <cstring>
#include <algorithm>
#include <array>
#include <limits>
#include <unordered_map>

namespace ember {

namespace {

// ─── helpers mirroring CG's static utilities (value_bytes / words_for_type /
//     int_bits / normalize) so this TU is self-contained and does NOT reach
//     into codegen.cpp's anonymous CG struct ───

inline int32_t round16(int32_t n) { return (n + 15) & ~15; }

static int32_t value_bytes(const Type* t, const StructLayoutTable* structs) {
    if (!t) return 8;
    if (t->is_slice) return 16;
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

static int32_t words_for_type(const Type* t, const StructLayoutTable* structs) {
    if (t && t->is_slice) return 2;
    if (t && !t->struct_name.empty() && structs) {
        auto it = structs->find(t->struct_name);
        if (it != structs->end()) return (it->second.size + 7) / 8;
    }
    return 1;
}

static int int_bits(const Type* t) {
    if (!t) return 64;
    switch (t->prim) {
    case Prim::I8: case Prim::U8: return 8;
    case Prim::I16: case Prim::U16: return 16;
    case Prim::I32: case Prim::U32: return 32;
    default: return 64;
    }
}

static bool is_registered_struct(const Type* t, const StructLayoutTable* structs) {
    return t && !t->struct_name.empty() && structs && structs->count(t->struct_name) != 0;
}

// ─── free-function byte helpers (mirrors of codegen.cpp's file-scope helpers) ───

static void store_rax_to_rbp(X64Emitter& e, int32_t off) {
    // REX.W 89 /r  mod=10 reg=rax(0) rm=rbp(5) -> 48 89 85 <disp32>
    e.byte(0x48); e.byte(0x89); e.byte(0x85); e.imm32(off);
}
static void load_rbp_to_rax(X64Emitter& e, int32_t off) {
    // REX.W 8B /r mod=10 reg=rax(0) rm=rbp(5) -> 48 8B 85 <disp32>
    e.byte(0x48); e.byte(0x8B); e.byte(0x85); e.imm32(off);
}
static void load_global_to_rax(X64Emitter& e, int32_t off) {
    // mov r11, <globals_base reloc>; mov rax, [r11 + off]
    e.mov_reg_imm64_external(Reg::r11, AbsFixup::GlobalsBase);
    e.load_reg_mem(Reg::rax, Reg::r11, off);
}
static void store_rax_to_global(X64Emitter& e, int32_t off) {
    e.mov_reg_imm64_external(Reg::r11, AbsFixup::GlobalsBase);
    e.byte(0x49); e.byte(0x89); e.byte(0x83); e.imm32(off);
}
static void store_xmm0_to_rbp(X64Emitter& e, int32_t off, const Type* t) {
    if (t && t->prim == Prim::F64) e.movsd_mem_xmm(Reg::rbp, off, Xmm::xmm0);
    else e.movss_mem_xmm(Reg::rbp, off, Xmm::xmm0);
}
static void store_xmm0_to_global(X64Emitter& e, int32_t off, const Type* t) {
    e.mov_reg_imm64_external(Reg::r11, AbsFixup::GlobalsBase);
    if (t && t->prim == Prim::F64) e.movsd_mem_xmm(Reg::r11, off, Xmm::xmm0);
    else e.movss_mem_xmm(Reg::r11, off, Xmm::xmm0);
}

// ─── the emit context ───

struct EmitCtx {
    X64Emitter e;
    const ThinFunction& thf;
    const CodeGenCtx& ctx;

    // VReg -> storage info. A VReg may be frame-backed (frame_off != 0) and/or
    // currently live in rax (rax_vreg) / xmm0 (xmm0_vreg). The emit prefers the
    // frame slot for materialization (robust across clobbers); falls back to the
    // register if the VReg is the most-recently-produced value still there.
    struct VRegInfo {
        int32_t frame_off = 0;   // 0 = not frame-backed
        const Type* type = nullptr;
    };
    std::unordered_map<VReg, VRegInfo> vregs;
    VReg rax_vreg = 0;    // which VReg's int value is currently in rax (0 = unknown)
    VReg xmm0_vreg = 0;   // which VReg's float value is currently in xmm0

    // block labels (indexed by block id)
    std::vector<Label> block_labels;

    // pending native bindings (for CompiledNativeBinding + JIT-time ptr fill)
    struct PendingNative {
        CompiledNativeBinding binding;
        void* target = nullptr;
    };
    std::vector<PendingNative> pending_natives;

    std::string non_serializable_reason;

    EmitCtx(const ThinFunction& f, const CodeGenCtx& c) : thf(f), ctx(c) {}

    // ─── type / width helpers ───
    const StructLayoutTable* structs() const { return ctx.structs; }

    bool vreg_is_float(VReg v) const {
        auto it = vregs.find(v);
        return it != vregs.end() && it->second.type && it->second.type->is_float();
    }
    bool vreg_is_slice(VReg v) const {
        auto it = vregs.find(v);
        return it != vregs.end() && it->second.type && it->second.type->is_slice;
    }
    const Type* vreg_type(VReg v) const {
        auto it = vregs.find(v);
        return it != vregs.end() ? it->second.type : nullptr;
    }

    // ─── normalize rax to a type's int width (mirrors CG::normalize_rax) ───
    void normalize_rax(const Type* t) {
        if (!t || !t->is_int() || t->is_fn_handle || !t->struct_name.empty()) return;
        int bits = int_bits(t);
        if (bits == 64) return;
        if (bits == 32) {
            if (t->is_uint()) { e.byte(0x89); e.byte(0xC0); }       // mov eax,eax
            else { e.byte(0x48); e.byte(0x63); e.byte(0xC0); }      // movsxd rax,eax
            return;
        }
        // shl rax, 64-bits; then sar (signed) or shr (unsigned) 64-bits
        e.byte(0x48); e.byte(0xC1); e.byte(0xE0); e.byte(uint8_t(64 - bits));
        e.byte(0x48); e.byte(0xC1); e.byte(t->is_uint() ? 0xE8 : 0xF8); e.byte(uint8_t(64 - bits));
    }

    // ─── copy_bytes / store_rax_bytes (mirrors CG's helpers) ───
    void copy_bytes(Reg dst_base, int32_t dst_off, Reg src_base, int32_t src_off, int32_t total_bytes) {
        int32_t done = 0;
        while (done < total_bytes) {
            int32_t remaining = total_bytes - done;
            int chunk = remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
            e.load_elem_to_rax(src_base, src_off + done, chunk, false);
            e.store_rax_elem(dst_base, dst_off + done, chunk);
            done += chunk;
        }
    }
    void store_rax_bytes(Reg base, int32_t off, int32_t nbytes) {
        int32_t done = 0;
        while (done < nbytes) {
            int chunk = nbytes - done >= 4 ? 4 : nbytes - done >= 2 ? 2 : 1;
            e.store_rax_elem(base, off + done, chunk);
            done += chunk;
            if (done < nbytes) e.shr_reg_imm8(Reg::rax, uint8_t(chunk * 8));
        }
    }

    // ─── trap / safety guards (mirrors CG's emit_* helpers) ───
    void emit_trap(int reason_ord, const char* detail) {
        if (ctx.trap_stub) {
            if (non_serializable_reason.empty())
                non_serializable_reason = "trap stub/context/detail pointers require a host runtime binding";
            const int32_t call_frame = e.win64_call_frame_size(32);
            e.sub_reg_imm32(Reg::rsp, call_frame);
            if (ctx.use_context_reg) e.mov_reg_reg(Reg::rcx, Reg::r14);
            else                     e.mov_reg_imm64(Reg::rcx, int64_t(ctx.trap_ctx));
            e.mov_reg_imm64(Reg::rdx, int64_t(reason_ord));
            e.mov_reg_imm64(Reg::r8,  int64_t(reinterpret_cast<uintptr_t>(detail)));
            e.mov_reg_imm64(Reg::rax, int64_t(ctx.trap_stub));
            e.require_win64_call_alignment();
            e.call_reg(Reg::rax);
            e.add_reg_imm32(Reg::rsp, call_frame);
        } else {
            e.byte(0x0F); e.byte(0x0B); // ud2
        }
    }
    void emit_budget_check(int64_t body_cost, const char* detail) {
        if (!ctx.emit_budget_checks || body_cost <= 0) return;
        const int32_t encoded_cost = body_cost > std::numeric_limits<int32_t>::max()
            ? std::numeric_limits<int32_t>::max() : int32_t(body_cost);
        if (!ctx.use_context_reg && !ctx.budget_ptr) return;
        if (!ctx.use_context_reg && non_serializable_reason.empty())
            non_serializable_reason = "instruction-budget storage is process-local";
        if (ctx.use_context_reg) {
            e.byte(0x49); e.byte(0x81); e.byte(0xAE); e.imm32(context_offsets::budget()); e.imm32(encoded_cost);
        } else {
            e.mov_reg_imm64(Reg::rax, int64_t(ctx.budget_ptr));
            e.byte(0x48); e.byte(0x81); e.byte(0x28); e.imm32(encoded_cost);
        }
        Label cont = e.alloc_label();
        e.jcc(Cond::g, cont);
        emit_trap(int(TrapReason::BudgetExceeded), detail);
        e.bind(cont);
    }
    void emit_depth_check() {
        if (!ctx.emit_depth_checks) return;
        if (!ctx.use_context_reg && !ctx.depth_ptr) return;
        if (!ctx.use_context_reg && non_serializable_reason.empty())
            non_serializable_reason = "call-depth storage is process-local";
        if (ctx.use_context_reg) {
            e.byte(0x41); e.byte(0xFF); e.byte(0x86); e.imm32(context_offsets::depth());
            e.byte(0x41); e.byte(0x8B); e.byte(0x86); e.imm32(context_offsets::max_depth());
            e.byte(0x41); e.byte(0x39); e.byte(0x86); e.imm32(context_offsets::depth());
        } else {
            e.mov_reg_imm64(Reg::rax, int64_t(ctx.depth_ptr));
            e.byte(0xFF); e.byte(0x00);
            e.byte(0x81); e.byte(0x38); e.imm32(int32_t(ctx.max_call_depth));
        }
        Label ok = e.alloc_label();
        e.jcc(Cond::l, ok);
        emit_trap(int(TrapReason::StackOverflow), "stack overflow: call depth exceeded");
        e.bind(ok);
    }
    void emit_depth_leave() {
        if (!ctx.emit_depth_checks) return;
        if (!ctx.use_context_reg && !ctx.depth_ptr) return;
        if (ctx.use_context_reg) {
            e.byte(0x41); e.byte(0xFF); e.byte(0x8E); e.imm32(context_offsets::depth());
        } else {
            e.mov_reg_imm64(Reg::r10, int64_t(ctx.depth_ptr));
            e.byte(0x49); e.byte(0xFF); e.byte(0x0A);
        }
    }
    void emit_call_target_guard() {
        if (ctx.fn_slot_count <= 0 || ctx.fn_allowlist_base == 0) return;
        if (non_serializable_reason.empty())
            non_serializable_reason = "function-reference allowlist storage is process-local";
        Label trap = e.alloc_label();
        e.byte(0x48); e.byte(0x81); e.byte(0xF8); e.imm32(int32_t(ctx.fn_slot_count));
        e.jcc(Cond::ae, trap);
        e.mov_reg_imm64(Reg::r11, ctx.fn_allowlist_base);
        e.mov_reg_reg(Reg::rcx, Reg::rax);
        e.byte(0x48); e.byte(0xC1); e.byte(0xE9); e.byte(0x03);
        e.byte(0x49); e.byte(0x01); e.byte(0xCB);
        e.mov_reg_reg(Reg::rcx, Reg::rax);
        e.byte(0x48); e.byte(0x83); e.byte(0xE1); e.byte(0x07);
        e.byte(0x49); e.byte(0x0F); e.byte(0xAB); e.byte(0x0B);
        e.jcc(Cond::ae, trap);
        Label after = e.alloc_label();
        e.jmp(after);
        e.bind(trap);
        emit_trap(int(TrapReason::BadCallTarget),
                  "call-target provenance: handle is not a registered function");
        e.bind(after);
    }
    void emit_bounds_check_reg(Reg idx_reg, Reg len_reg) {
        e.cmp_reg_reg(idx_reg, len_reg);
        Label ok = e.alloc_label();
        e.jcc(Cond::b, ok);
        emit_trap(int(TrapReason::BoundsCheck), "bounds check: index out of range");
        e.bind(ok);
    }
    void emit_bounds_check_imm(Reg idx_reg, int64_t len) {
        e.cmp_reg_imm32(idx_reg, int32_t(len));
        Label ok = e.alloc_label();
        e.jcc(Cond::b, ok);
        emit_trap(int(TrapReason::BoundsCheck), "bounds check: index out of range");
        e.bind(ok);
    }
    void emit_integer_divmod(bool want_mod, bool is_unsigned) {
        Label nonzero = e.alloc_label();
        e.cmp_reg_imm32(Reg::rcx, 0); e.jcc(Cond::ne, nonzero);
        emit_trap(int(TrapReason::DivByZero), "integer division by zero"); e.bind(nonzero);
        if (!is_unsigned) {
            Label safe = e.alloc_label(), overflow = e.alloc_label();
            e.cmp_reg_imm32(Reg::rcx, -1); e.jcc(Cond::ne, safe);
            e.mov_reg_imm64(Reg::r10, INT64_MIN); e.cmp_reg_reg(Reg::rax, Reg::r10); e.jcc(Cond::e, overflow);
            e.jmp(safe); e.bind(overflow);
            emit_trap(int(TrapReason::DivByZero), "signed division overflow"); e.bind(safe);
            e.byte(0x48); e.byte(0x99);              // cqo
            e.byte(0x48); e.byte(0xF7); e.byte(0xF9); // idiv rcx
        } else {
            e.byte(0x48); e.byte(0x31); e.byte(0xD2); // xor rdx,rdx
            e.byte(0x48); e.byte(0xF7); e.byte(0xF1); // div rcx
        }
        if (want_mod) e.mov_reg_reg(Reg::rax, Reg::rdx);
    }

    // ─── VReg materialization ───
    // Load a scalar int VReg's value into rax. If the VReg is frame-backed,
    // load from its frame slot (with normalize). If it's the current rax_vreg,
    // it's already in rax. Otherwise assume rax (best-effort for register-flow
    // lowering where the VReg was just produced and no frame slot was assigned).
    void load_int_vreg(VReg v) {
        auto it = vregs.find(v);
        if (it != vregs.end() && it->second.frame_off != 0) {
            load_rbp_to_rax(e, it->second.frame_off);
            normalize_rax(it->second.type);
            rax_vreg = v;
        } else if (v != 0 && v == rax_vreg) {
            // already in rax
        } else if (v != 0) {
            // no frame slot + not rax_vreg: the lowering left it in rax but we
            // lost track (a clobbering instr ran in between). Best-effort: trust
            // rax. A well-formed lowering gives such VRegs frame slots.
        }
    }
    // Load a float VReg's value into xmm0.
    void load_float_vreg(VReg v) {
        auto it = vregs.find(v);
        if (it != vregs.end() && it->second.frame_off != 0) {
            if (it->second.type && it->second.type->prim == Prim::F32)
                e.movss_xmm_mem(Xmm::xmm0, Reg::rbp, it->second.frame_off);
            else
                e.movsd_xmm_mem(Xmm::xmm0, Reg::rbp, it->second.frame_off);
            xmm0_vreg = v;
        } else if (v != 0 && v == xmm0_vreg) {
            // already in xmm0
        }
        // else: best-effort trust xmm0
    }
    // Load a slice VReg's {ptr, len} into {rax, rdx}. The slice VReg v is the
    // ptr VReg; v+1 is the len VReg. If frame-backed, load from [rbp+off] and
    // [rbp+off+8].
    void load_slice_vreg(VReg v) {
        auto it = vregs.find(v);
        if (it != vregs.end() && it->second.frame_off != 0) {
            e.load_reg_mem(Reg::rax, Reg::rbp, it->second.frame_off);
            e.load_reg_mem(Reg::rdx, Reg::rbp, it->second.frame_off + 8);
        }
        // else: assume rax=ptr, rdx=len already
    }

    // Record a dst VReg's production. The result is in rax (int) / xmm0 (float)
    // / {rax,rdx} (slice). If meta.frame_off is set, store to the frame slot and
    // mark the VReg frame-backed. Records the VReg's type for future
    // materialization.
    void record_dst(VReg dst, const ThinMeta& meta) {
        if (dst == 0) return;  // struct-by-ptr calls have dst=0 (result via hidden ptr)
        VRegInfo info;
        info.type = meta.type;
        if (meta.frame_off != 0) {
            info.frame_off = meta.frame_off;
            // store the result to the frame slot
            if (meta.type && meta.type->is_slice) {
                e.store_reg_mem(Reg::rbp, meta.frame_off, Reg::rax);
                e.store_reg_mem(Reg::rbp, meta.frame_off + 8, Reg::rdx);
                // also record the len VReg's frame slot
                VRegInfo len_info;
                len_info.frame_off = meta.frame_off + 8;
                len_info.type = meta.type;  // slice type (the len's companion)
                vregs[dst + 1] = len_info;
            } else if (meta.type && meta.type->is_float()) {
                store_xmm0_to_rbp(e, meta.frame_off, meta.type);
                xmm0_vreg = dst;
            } else {
                normalize_rax(meta.type);
                store_rax_to_rbp(e, meta.frame_off);
                rax_vreg = dst;
            }
        } else {
            // no frame slot: result is in rax/xmm0 only
            if (meta.type && meta.type->is_float()) xmm0_vreg = dst;
            else rax_vreg = dst;
        }
        vregs[dst] = info;
    }
    // Record a dst without storing to frame (result left in rax/xmm0). Used by
    // ops where the lowering doesn't assign a frame slot to the dst (the value
    // flows to the next instr via rax).
    void record_dst_rax(VReg dst, const Type* ty) {
        if (dst == 0) return;
        VRegInfo info;
        info.type = ty;
        vregs[dst] = info;
        if (ty && ty->is_float()) xmm0_vreg = dst;
        else rax_vreg = dst;
    }

    // ─── prologue / epilogue ───
    void emit_prologue() {
        e.push(Reg::rbp);
        e.mov_reg_reg(Reg::rbp, Reg::rsp);
        e.sub_reg_imm32(Reg::rsp, thf.frame.frame_size);
        e.store_reg_mem(Reg::rbp, thf.frame.rbx_save_offset, Reg::rbx);
    }
    void emit_epilogue() {
        e.load_reg_mem(Reg::rbx, Reg::rbp, thf.frame.rbx_save_offset);
        e.mov_reg_reg(Reg::rsp, Reg::rbp);
        e.pop(Reg::rbp);
        e.ret();
    }

    // ─── param spill (mirrors compile_func's word-indexed spill_word) ───
    void emit_param_spills() {
        static const Reg int_arg_regs[4] = {Reg::rcx, Reg::rdx, Reg::r8, Reg::r9};
        static const Xmm flt_arg_regs[4] = {Xmm::xmm0, Xmm::xmm1, Xmm::xmm2, Xmm::xmm3};

        // total_words mirrors compile_func: 1 for the hidden return ptr (if
        // any) + the real params' word counts. The __struct_ret_ptr sentinel
        // in thf.frame.params (p.ty == nullptr) is NOT a real param — the hidden
        // ptr is already accounted for by the initial +1 and spilled separately
        // below — so it MUST be skipped here and in the spill loop.
        int32_t total_words = thf.frame.returns_struct_by_ptr ? 1 : 0;
        for (const auto& p : thf.frame.params)
            if (p.ty != nullptr) total_words += words_for_type(p.ty, structs());

        auto spill_word = [&](int32_t w, int32_t dst_off, const Type* float_ty) {
            if (w < 4) {
                if (float_ty) {
                    if (float_ty->prim == Prim::F64) e.movsd_mem_xmm(Reg::rbp, dst_off, flt_arg_regs[w]);
                    else e.movss_mem_xmm(Reg::rbp, dst_off, flt_arg_regs[w]);
                } else {
                    e.store_reg_mem(Reg::rbp, dst_off, int_arg_regs[w]);
                }
            } else {
                int32_t src_off = 48 + total_words * 8 + (w - 4) * 8;
                if (float_ty) {
                    if (float_ty->prim == Prim::F64) {
                        e.movsd_xmm_mem(Xmm::xmm4, Reg::rbp, src_off);
                        e.movsd_mem_xmm(Reg::rbp, dst_off, Xmm::xmm4);
                    } else {
                        e.movss_xmm_mem(Xmm::xmm4, Reg::rbp, src_off);
                        e.movss_mem_xmm(Reg::rbp, dst_off, Xmm::xmm4);
                    }
                } else {
                    e.load_reg_mem(Reg::rax, Reg::rbp, src_off);
                    e.store_reg_mem(Reg::rbp, dst_off, Reg::rax);
                }
            }
        };

        int32_t param_word = 0;
        // hidden return-pointer slot first (word 0) if returns_struct_by_ptr
        if (thf.frame.returns_struct_by_ptr && thf.frame.struct_ret_ptr_offset != 0) {
            spill_word(0, thf.frame.struct_ret_ptr_offset, nullptr);
            param_word = 1;
        }

        // Build the VReg->frame_off map from params. VReg numbering: 1-indexed,
        // scalar/float = 1 VReg, slice = 2 VRegs, struct = 0 VRegs.
        uint32_t next_vreg = 1;
        for (const auto& p : thf.frame.params) {
            const Type* pt = p.ty;
            if (pt == nullptr) continue;  // skip the __struct_ret_ptr sentinel
            bool is_struct = is_registered_struct(pt, structs());
            if (is_struct) {
                // struct param: spill ceil(size/8) words with last-word byte trimming
                auto sit = structs()->find(pt->struct_name);
                int32_t struct_bytes = (sit != structs()->end()) ? sit->second.size : words_for_type(pt, structs()) * 8;
                int32_t wcount = words_for_type(pt, structs());
                int32_t byte_pos = 0;
                for (int w = 0; w < wcount; ++w) {
                    int32_t word_bytes = std::min<int32_t>(8, struct_bytes - byte_pos);
                    int32_t w_global = param_word + w;
                    if (word_bytes >= 8) {
                        spill_word(w_global, p.off + byte_pos, nullptr);
                    } else {
                        if (w_global < 4) e.mov_reg_reg(Reg::rax, int_arg_regs[w_global]);
                        else {
                            int32_t src_off = 48 + total_words * 8 + (w_global - 4) * 8;
                            e.load_reg_mem(Reg::rax, Reg::rbp, src_off);
                        }
                        store_rax_bytes(Reg::rbp, p.off + byte_pos, word_bytes);
                    }
                    byte_pos += word_bytes;
                }
                // struct: no VReg assigned (structs are frame slots)
            } else if (pt && pt->is_slice) {
                spill_word(param_word, p.off, nullptr);
                spill_word(param_word + 1, p.off + 8, nullptr);
                // record the slice's two VRegs
                vregs[next_vreg] = {p.off, pt};
                vregs[next_vreg + 1] = {p.off + 8, pt};
                next_vreg += 2;
            } else if (pt && pt->is_float()) {
                spill_word(param_word, p.off, pt);
                vregs[next_vreg] = {p.off, pt};
                next_vreg += 1;
            } else {
                spill_word(param_word, p.off, nullptr);
                vregs[next_vreg] = {p.off, pt};
                next_vreg += 1;
            }
            param_word += words_for_type(pt, structs());
        }
    }

    // ─── call emission (shared arg-stash + ABI reg reload for all 4 call kinds) ───
    // Returns true if the call returns a struct by ptr (hidden word-0 ABI).
    bool call_returns_struct_by_ptr(const Type* ret_type) const {
        return is_registered_struct(ret_type, structs());
    }

    // Build the operand word layout from the instr's args[]/arg_types[]/arg_frame_offs[].
    // Returns the total word count; fills `slot0` per logical arg into `op_slots`.
    // When `ret_struct` is true, args[0] is the hidden return-dest encoding (not a
    // real operand) and is skipped — the caller decodes it separately.
    struct CallArg { VReg vreg; const Type* ty; int32_t slot0; int words; bool is_struct; int32_t struct_frame_off; };
    std::vector<CallArg> build_call_args(const ThinInstr& in, bool ret_struct) {
        std::vector<CallArg> ops;
        int32_t next_slot = 0;
        for (size_t i = ret_struct ? 1 : 0; i < in.args.size(); ++i) {
            VReg v = in.args[i];
            const Type* ty = i < in.arg_types.size() ? in.arg_types[i] : nullptr;
            int32_t afo = i < in.arg_frame_offs.size() ? in.arg_frame_offs[i] : -1;
            if (v == 0 && afo != -1) {
                // struct-by-value: vreg sentinel + frame offset
                int w = words_for_type(ty, structs());
                ops.push_back({v, ty, next_slot, w, true, afo});
                next_slot += w;
            } else if (ty && ty->is_slice) {
                // slice: 2 words (ptr at this vreg, len at next vreg)
                ops.push_back({v, ty, next_slot, 2, false, -1});
                next_slot += 2;
                ++i;  // consume the len VReg too
            } else {
                ops.push_back({v, ty, next_slot, 1, false, -1});
                next_slot += 1;
            }
        }
        return ops;
    }

    // Emit the arg stash + ABI reg reload for a call. For a struct-by-ptr
    // return call, `ret_struct` is true and the hidden dest address is encoded
    // in `hidden_dest_vreg` (a vreg holding the dest pointer, when != 0) or, if
    // that is 0, in `hidden_dest_off` (a frame slot the callee writes into, via
    // `lea rax, [rbp+hidden_dest_off]`). For regular calls both are 0 and
    // `ret_struct` is false. The handle word (for indirect calls) is at
    // [rsp+stash_size] if `is_indirect`.
    void emit_call_arg_stash(const ThinInstr& in, std::vector<CallArg>& ops,
                             bool ret_struct, VReg hidden_dest_vreg,
                             int32_t hidden_dest_off, bool is_indirect) {
        int n = 0;
        for (auto& op : ops) n += op.words;
        if (ret_struct) n += 1;  // word 0 = hidden dest ptr
        int32_t stash_size = n * 8;
        int32_t handle_word = is_indirect ? 8 : 0;
        int32_t outgoing = std::max(0, n - 4) * 8;
        int32_t total = round16(stash_size + handle_word + 32 + outgoing);

        // For indirect calls: the handle was already validated and is in rax
        // ( BEFORE this stash). Store it at [rsp+stash_size] right after sub rsp.
        e.sub_reg_imm32(Reg::rsp, total);
        if (is_indirect) {
            e.byte(0x48); e.byte(0x89); e.byte(0x84); e.byte(0x24); e.imm32(stash_size);
        }
        // hidden dest address at word 0 (if struct-by-ptr return)
        if (ret_struct) {
            if (hidden_dest_vreg != 0) {
                // dest ptr is in the vreg (forward-return: the loaded incoming
                // hidden ptr, or an IndexAddr/FieldAddr result)
                load_int_vreg(hidden_dest_vreg);
            } else {
                // dest is a frame slot (local/temp): lea rax, [rbp + hidden_dest_off]
                e.byte(0x48); e.byte(0x8D); e.byte(0x85); e.imm32(hidden_dest_off);
            }
            e.store_reg_mem(Reg::rsp, 0, Reg::rax);
            // rax now holds the dest address; clear rax_vreg so a later
            // load_int_vreg does not take the stale-rax shortcut.
            rax_vreg = 0;
        }
        // stash each operand
        int32_t word_offset = ret_struct ? 8 : 0;  // shift past hidden ptr
        for (auto& op : ops) {
            int32_t off = word_offset + op.slot0 * 8;
            if (op.is_struct) {
                // struct-by-value: copy bytes from the frame slot to [rsp+off]
                auto sit = structs()->find(op.ty->struct_name);
                int32_t sz = (sit != structs()->end()) ? sit->second.size : op.words * 8;
                copy_bytes(Reg::rsp, off, Reg::rbp, op.struct_frame_off, sz);
            } else if (op.words == 2) {
                // slice: materialize {rax=ptr, rdx=len}, store both
                load_slice_vreg(op.vreg);
                e.store_reg_mem(Reg::rsp, off, Reg::rax);
                e.store_reg_mem(Reg::rsp, off + 8, Reg::rdx);
            } else if (op.ty && op.ty->is_float()) {
                load_float_vreg(op.vreg);
                if (op.ty->prim == Prim::F64) e.movsd_mem_xmm(Reg::rsp, off, Xmm::xmm0);
                else e.movss_mem_xmm(Reg::rsp, off, Xmm::xmm0);
            } else {
                load_int_vreg(op.vreg);
                e.byte(0x48); e.byte(0x89); e.byte(0x84); e.byte(0x24); e.imm32(off);
            }
        }
        // per-word float width
        std::vector<bool> word_is_float(size_t(n), false), word_is_f64(size_t(n), false);
        int32_t widx = ret_struct ? 1 : 0;
        for (auto& op : ops) {
            if (op.words == 1 && !op.is_struct && op.ty && op.ty->is_float()) {
                word_is_float[size_t(widx)] = true;
                word_is_f64[size_t(widx)] = (op.ty->prim == Prim::F64);
            }
            widx += op.words;
        }
        // place words 0..3 into regs
        static const Reg int_regs[4] = {Reg::rcx, Reg::rdx, Reg::r8, Reg::r9};
        static const Xmm flt_regs[4] = {Xmm::xmm0, Xmm::xmm1, Xmm::xmm2, Xmm::xmm3};
        for (int w = 0; w < n && w < 4; ++w) {
            int32_t off = w * 8;
            if (word_is_float[size_t(w)]) {
                if (word_is_f64[size_t(w)]) e.movsd_xmm_mem(flt_regs[w], Reg::rsp, off);
                else e.movss_xmm_mem(flt_regs[w], Reg::rsp, off);
            } else {
                e.load_reg_mem(int_regs[w], Reg::rsp, off);
            }
        }
        // words 4+ -> outgoing stack args
        for (int w = 4; w < n; ++w) {
            int32_t src = w * 8;
            int32_t dst = stash_size + handle_word + 32 + (w - 4) * 8;
            e.load_reg_mem(Reg::rax, Reg::rsp, src);
            e.store_reg_mem(Reg::rsp, dst, Reg::rax);
        }
        // The caller does the actual call instruction, then: e.add_reg_imm32(Reg::rsp, total);
        // We stash `total` for the caller via a member.
        last_call_total = total;
        last_call_stash_size = stash_size;
    }
    int32_t last_call_total = 0;
    int32_t last_call_stash_size = 0;

    // Emit a native call: mov_reg_native + call + depth leave. The depth CHECK
    // is emitted as a separate DepthCheck ThinInstr before the call instr (by
    // lower_call); we only emit the depth LEAVE here to balance it. Emitting
    // depth_check again here would double-increment the depth counter (a leak
    // that overflows the call-depth limit on deep recursion like fib(15)).
    void emit_native_call(const ThinInstr& in) {
        // mov rax, native (relocatable); record pending native binding
        const std::string& name = in.meta.native_name;
        if (name.empty() || !in.ret_type || in.arg_types.empty()) {
            if (non_serializable_reason.empty())
                non_serializable_reason = "native call has no complete symbolic NativeSig binding";
            e.mov_reg_imm64(Reg::rax, int64_t(in.native_fn));
        } else {
            e.mov_reg_native(Reg::rax, name);
            PendingNative pn;
            pn.binding.code_offset = e.native_fixups().back().code_offset;
            pn.binding.name = name;
            pn.binding.ret = *in.ret_type;
            // arg_types is vector<const Type*>; CompiledNativeBinding.params
            // is vector<Type>. Copy the Type values.
            for (const Type* t : in.arg_types) pn.binding.params.push_back(*t);
            pn.target = in.native_fn;
            pending_natives.push_back(std::move(pn));
        }
        e.call_reg(Reg::rax);
        emit_depth_leave();
    }
    // Emit a script call: dispatch table + call + depth leave. (Depth check is
    // the separate DepthCheck ThinInstr; see emit_native_call for the rationale.)
    void emit_script_call(const ThinInstr& in) {
        e.mov_reg_imm64_external(Reg::r11, AbsFixup::DispatchTableBase);
        e.call_mem(Reg::r11, int32_t(in.meta.slot) * 8);
        emit_depth_leave();
    }
    // Emit a cross-module call: registry hop + call + depth leave. (Depth check
    // is the separate DepthCheck ThinInstr.)
    void emit_cross_module_call(const ThinInstr& in) {
        e.mov_reg_imm64_external(Reg::r11, AbsFixup::ModuleRegistryBase);
        e.load_reg_mem(Reg::r11, Reg::r11, int32_t(in.meta.mod_id) * 8);
        e.load_reg_mem(Reg::r11, Reg::r11, int32_t(in.meta.slot) * 8);
        e.call_reg(Reg::r11);
        emit_depth_leave();
    }
    // Emit an indirect call (CallIndirect): reload handle + lea + load + call +
    // depth leave. (Depth check is the separate DepthCheck ThinInstr.) The
    // handle was stashed at [rsp+stash_size] by emit_call_arg_stash.
    void emit_indirect_call() {
        e.load_reg_mem(Reg::rax, Reg::rsp, last_call_stash_size);  // reload handle
        e.mov_reg_imm64_external(Reg::r11, AbsFixup::DispatchTableBase);
        e.lea_reg_mem_sib(Reg::r11, Reg::r11, Reg::rax, 3);  // lea r11, [r11 + rax*8]
        e.load_reg_mem(Reg::r11, Reg::r11, 0);                // mov r11, [r11]
        e.call_reg(Reg::r11);
        emit_depth_leave();
    }

    // ─── the main emit: walk blocks, emit instrs + terms ───
    CompiledFn emit() {
        // allocate block labels
        block_labels.resize(thf.blocks.size());
        for (size_t i = 0; i < thf.blocks.size(); ++i)
            block_labels[i] = e.alloc_label();

        // prologue
        emit_prologue();
        // param spills + VReg map init
        emit_param_spills();
        // budget check at entry (coarse cost from instr count; budget is off by
        // default so this emits nothing in the common case)
        if (ctx.emit_budget_checks) {
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

        // resolve label fixups
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

        // build native_fixups + fill JIT-time ptrs
        std::vector<CompiledNativeBinding> native_bindings;
        for (const auto& pn : pending_natives) {
            native_bindings.push_back(pn.binding);
            if (pn.binding.code_offset + 8 > e.code.size()) continue;
            uint64_t v = reinterpret_cast<uintptr_t>(pn.target);
            for (int i = 0; i < 8; ++i)
                e.code[pn.binding.code_offset + i] = uint8_t(v >> (8 * i));
        }

        // peephole hook (identical to compile_func's peephole block)
        if (ctx.enable_peephole && !e.code.empty()) {
            PeepholeGuardedRegions guarded;
            for (const auto& af : e.abs_fixups()) guarded.imm64_offsets.insert(af.code_offset);
            for (const auto& nf : e.native_fixups()) guarded.imm64_offsets.insert(nf.code_offset);
            PeepholeCtx pctx{ e.code, e.resolved_labels_view(), std::move(guarded) };
            auto pipeline = make_stage1_pipeline();
            pipeline.run_all(pctx);
        }

        // assemble CompiledFn
        CompiledFn out;
        out.name = thf.name;
        out.abs_fixups = e.abs_fixups();
        out.native_fixups = std::move(native_bindings);
        out.rodata = thf.rodata;  // copy (ThinFunction keeps its rodata)
        if (non_serializable_reason.empty() && !thf.non_serializable_reason.empty())
            non_serializable_reason = thf.non_serializable_reason;
        out.non_serializable_reason = std::move(non_serializable_reason);
        out.bytes = std::move(e.code);
        return out;
    }

    // ─── term emission ───
    void emit_term(const ThinTerm& term) {
        switch (term.kind) {
        case TermKind::Jmp:
            e.jmp(block_labels[term.target]);
            break;
        case TermKind::Branch: {
            // test cond vreg; jcc to true/false targets
            load_int_vreg(term.cond);
            e.cmp_reg_imm32(Reg::rax, 0);
            // cond true -> target; cond false -> false_target
            e.jcc(Cond::ne, block_labels[term.target]);  // if true, jump to true-target
            e.jmp(block_labels[term.false_target]);       // else jump to false-target
            break;
        }
        case TermKind::Return:
            emit_return(term);
            break;
        case TermKind::Trap:
            emit_trap(int(term.trap_reason), "trap");
            break;
        case TermKind::None:
            // no terminator (shouldn't happen for a well-formed function)
            break;
        }
    }

    void emit_return(const ThinTerm& term) {
        // struct-by-ptr return: load hidden ptr into rax, epilogue (the struct
        // bytes were already copied through the hidden ptr by earlier instrs).
        if (thf.frame.returns_struct_by_ptr) {
            e.load_reg_mem(Reg::rax, Reg::rbp, thf.frame.struct_ret_ptr_offset);
            emit_epilogue();
            return;
        }
        const Type* rt = thf.ret_type;
        if (term.ret == 0 || rt == nullptr || rt->is_void()) {
            // void return
            emit_epilogue();
            return;
        }
        if (rt->is_float()) {
            load_float_vreg(term.ret);
            emit_epilogue();
            return;
        }
        if (rt->is_slice) {
            load_slice_vreg(term.ret);
            emit_epilogue();
            return;
        }
        // int/bool return: materialize into rax, normalize, epilogue
        load_int_vreg(term.ret);
        normalize_rax(rt);
        emit_epilogue();
    }

    // ─── instruction emission (the big switch) ───
    void emit_instr(const ThinInstr& in) {
        switch (in.op) {
        // ── constants ──
        case ThinOp::ConstInt:
            e.mov_reg_imm64(Reg::rax, in.imm.i);
            record_dst_rax(in.dst, in.meta.type);
            if (in.meta.frame_off != 0) {
                normalize_rax(in.meta.type);
                store_rax_to_rbp(e, in.meta.frame_off);
                if (in.dst != 0) { vregs[in.dst].frame_off = in.meta.frame_off; }
            }
            break;
        case ThinOp::ConstFloat: {
            if (in.meta.is_f32) {
                float fv = float(in.imm.f);
                uint32_t bits; std::memcpy(&bits, &fv, 4);
                e.mov_reg_imm64(Reg::rax, int64_t(int32_t(bits)));
                e.byte(0x66); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0); // movd xmm0,eax
            } else {
                uint64_t bits; std::memcpy(&bits, &in.imm.f, 8);
                e.mov_reg_imm64(Reg::rax, int64_t(bits));
                e.byte(0x66); e.byte(0x48); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0); // movq xmm0,rax
            }
            record_dst_rax(in.dst, in.meta.type);
            if (in.meta.frame_off != 0) {
                store_xmm0_to_rbp(e, in.meta.frame_off, in.meta.type);
                if (in.dst != 0) vregs[in.dst].frame_off = in.meta.frame_off;
            }
            break;
        }
        case ThinOp::ConstBool:
            e.mov_reg_imm64(Reg::rax, in.imm.i ? 1 : 0);
            record_dst_rax(in.dst, in.meta.type);
            if (in.meta.frame_off != 0) {
                store_rax_to_rbp(e, in.meta.frame_off);
                if (in.dst != 0) vregs[in.dst].frame_off = in.meta.frame_off;
            }
            break;
        case ThinOp::ConstStringRef: {
            // slice ABI: rax=ptr (rodata base + addend), rdx=len
            e.mov_reg_imm64_external(Reg::rax, AbsFixup::FunctionRodataBase, in.meta.addend);
            e.mov_reg_imm64(Reg::rdx, int64_t(in.meta.len));
            record_dst_rax(in.dst, in.meta.type);
            if (in.meta.frame_off != 0 && in.dst != 0) {
                e.store_reg_mem(Reg::rbp, in.meta.frame_off, Reg::rax);
                e.store_reg_mem(Reg::rbp, in.meta.frame_off + 8, Reg::rdx);
                vregs[in.dst].frame_off = in.meta.frame_off;
                vregs[in.dst + 1].frame_off = in.meta.frame_off + 8;
            }
            break;
        }
        case ThinOp::StringDecrypt: {
            // inline XOR decrypt into a temp frame slot, then yield {rax=ptr, rdx=len}
            const int32_t slot_off = in.meta.frame_off;
            const int64_t len = in.meta.len;
            const uint8_t key = uint8_t(in.imm.i);
            // r11 = enc source (rodata base + addend)
            e.mov_reg_imm64_external(Reg::r11, AbsFixup::FunctionRodataBase, in.meta.addend);
            // r10 = rbp - slot_off (frame slot address; slot_off is negative)
            e.mov_reg_reg(Reg::r10, Reg::rbp);
            e.sub_reg_imm32(Reg::r10, -slot_off);
            // inline byte XOR (unrolled for len <= 256, loop for longer)
            if (len <= 256) {
                for (int64_t i = 0; i < len; ++i) {
                    bool disp32 = i >= 128;
                    if (!disp32) {
                        e.byte(0x41); e.byte(0x8A); e.byte(0x43); e.byte(uint8_t(i));
                    } else {
                        e.byte(0x41); e.byte(0x8A); e.byte(0x83); e.imm32(int32_t(i));
                    }
                    e.byte(0x34); e.byte(key);
                    if (!disp32) {
                        e.byte(0x41); e.byte(0x88); e.byte(0x42); e.byte(uint8_t(i));
                    } else {
                        e.byte(0x41); e.byte(0x88); e.byte(0x82); e.imm32(int32_t(i));
                    }
                }
                e.mov_reg_reg(Reg::rax, Reg::r10);
            } else {
                e.mov_reg_imm64(Reg::rcx, len);
                Label loop = e.alloc_label();
                e.bind(loop);
                e.byte(0x41); e.byte(0x8A); e.byte(0x03);
                e.byte(0x34); e.byte(key);
                e.byte(0x41); e.byte(0x88); e.byte(0x02);
                e.byte(0x49); e.byte(0xFF); e.byte(0xC3);
                e.byte(0x49); e.byte(0xFF); e.byte(0xC2);
                e.byte(0x48); e.byte(0xFF); e.byte(0xC9);
                e.byte(0x48); e.byte(0x83); e.byte(0xF9); e.byte(0x00);
                Label done = e.alloc_label();
                e.jcc(Cond::e, done);
                e.jmp(loop);
                e.bind(done);
                // re-derive slot base from rbp (r10 was incremented past the slot)
                e.mov_reg_reg(Reg::rax, Reg::rbp);
                e.sub_reg_imm32(Reg::rax, -slot_off);
            }
            e.mov_reg_imm64(Reg::rdx, len);
            record_dst_rax(in.dst, in.meta.type);
            if (in.dst != 0) {
                vregs[in.dst].type = in.meta.type;
                // slice result is in {rax,rdx}; if frame-backed, store
                if (in.meta.frame_off != 0) {
                    e.store_reg_mem(Reg::rbp, in.meta.frame_off, Reg::rax);
                    e.store_reg_mem(Reg::rbp, in.meta.frame_off + 8, Reg::rdx);
                    vregs[in.dst].frame_off = in.meta.frame_off;
                    vregs[in.dst + 1].frame_off = in.meta.frame_off + 8;
                }
            }
            break;
        }

        // ── moves / memory ──
        case ThinOp::Move: {
            // dst = src1. Materialize src1 into rax/xmm0/{rax,rdx}, record dst.
            const Type* ty = in.meta.type ? in.meta.type : vreg_type(in.src1);
            if (ty && ty->is_float()) {
                load_float_vreg(in.src1);
                record_dst_rax(in.dst, ty);
                if (in.meta.frame_off != 0 && in.dst != 0) {
                    store_xmm0_to_rbp(e, in.meta.frame_off, ty);
                    vregs[in.dst].frame_off = in.meta.frame_off;
                }
            } else if (ty && ty->is_slice) {
                load_slice_vreg(in.src1);
                record_dst_rax(in.dst, ty);
                if (in.meta.frame_off != 0 && in.dst != 0) {
                    e.store_reg_mem(Reg::rbp, in.meta.frame_off, Reg::rax);
                    e.store_reg_mem(Reg::rbp, in.meta.frame_off + 8, Reg::rdx);
                    vregs[in.dst].frame_off = in.meta.frame_off;
                    vregs[in.dst + 1].frame_off = in.meta.frame_off + 8;
                }
            } else {
                load_int_vreg(in.src1);
                normalize_rax(ty);
                record_dst_rax(in.dst, ty);
                if (in.meta.frame_off != 0 && in.dst != 0) {
                    store_rax_to_rbp(e, in.meta.frame_off);
                    vregs[in.dst].frame_off = in.meta.frame_off;
                }
            }
            break;
        }
        case ThinOp::LoadFrame: {
            // dst = [base + meta.frame_off], where base is:
            //   - rbp (the usual frame load) when src1 == 0, OR
            //   - a computed address VReg (an IndexAddr/FieldAddr result) when
            //     src1 != 0 (the element/field load path: load from [addr + off]).
            const Type* ty = in.meta.type;
            Reg base_reg = Reg::rbp;
            if (in.src1 != 0) {
                // materialize the computed address into r11 (use r11, not rax, so
                // the load destination rax is independent of the base materialization)
                load_int_vreg(in.src1);
                e.mov_reg_reg(Reg::r11, Reg::rax);
                base_reg = Reg::r11;
            }
            if (ty && ty->is_float()) {
                // load from frame to xmm0
                if (base_reg == Reg::rbp) {
                    if (ty->prim == Prim::F32) e.movss_xmm_mem(Xmm::xmm0, Reg::rbp, in.meta.frame_off);
                    else e.movsd_xmm_mem(Xmm::xmm0, Reg::rbp, in.meta.frame_off);
                } else {
                    // load from [r11 + off] (computed address)
                    if (ty->prim == Prim::F32) { e.byte(0xF3); e.byte(0x41); e.byte(0x0F); e.byte(0x10); e.byte(0x43); e.imm32(in.meta.frame_off); }
                    else { e.byte(0xF2); e.byte(0x41); e.byte(0x0F); e.byte(0x10); e.byte(0x43); e.imm32(in.meta.frame_off); }
                }
                record_dst_rax(in.dst, ty);
            } else if (ty && ty->is_slice) {
                e.load_reg_mem(Reg::rax, base_reg, in.meta.frame_off);
                e.load_reg_mem(Reg::rdx, base_reg, in.meta.frame_off + 8);
                record_dst_rax(in.dst, ty);
            } else {
                if (base_reg == Reg::rbp) {
                    load_rbp_to_rax(e, in.meta.frame_off);
                } else {
                    // mov rax, [r11 + off]: REX.W 8B /r mod=10 reg=rax(0) rm=r11(3)-> 49 8B 83 <disp32>
                    e.byte(0x49); e.byte(0x8B); e.byte(0x83); e.imm32(in.meta.frame_off);
                }
                normalize_rax(ty);
                record_dst_rax(in.dst, ty);
            }
            // record the dst's frame slot ONLY for the rbp-base case (a load from
            // a computed address is NOT frame-backed at meta.frame_off — that
            // offset is within the slice/struct, not the function frame)
            if (in.dst != 0 && in.src1 == 0) {
                vregs[in.dst].frame_off = in.meta.frame_off;
                vregs[in.dst].type = ty;
                if (ty && ty->is_slice) {
                    vregs[in.dst + 1].frame_off = in.meta.frame_off + 8;
                    vregs[in.dst + 1].type = ty;
                }
            }
            break;
        }
        case ThinOp::StoreFrame: {
            // [rbp + meta.frame_off] = src1 (with width normalization)
            const Type* ty = in.meta.type ? in.meta.type : vreg_type(in.src1);
            if (ty && ty->is_float()) {
                load_float_vreg(in.src1);
                store_xmm0_to_rbp(e, in.meta.frame_off, ty);
            } else if (ty && ty->is_slice) {
                load_slice_vreg(in.src1);
                e.store_reg_mem(Reg::rbp, in.meta.frame_off, Reg::rax);
                e.store_reg_mem(Reg::rbp, in.meta.frame_off + 8, Reg::rdx);
            } else {
                load_int_vreg(in.src1);
                normalize_rax(ty);
                store_rax_to_rbp(e, in.meta.frame_off);
            }
            // record the src VReg as frame-backed at this offset
            if (in.src1 != 0) {
                vregs[in.src1].frame_off = in.meta.frame_off;
                vregs[in.src1].type = ty;
                if (ty && ty->is_slice) {
                    vregs[in.src1 + 1].frame_off = in.meta.frame_off + 8;
                    vregs[in.src1 + 1].type = ty;
                }
            }
            break;
        }
        case ThinOp::LoadGlobal: {
            // dst = [globals_base + meta.addend] (with type-based load)
            const Type* ty = in.meta.type;
            if (ty && ty->is_slice) {
                // slice global: load {ptr,len}, absolute-ize the ptr
                load_global_to_rax(e, int32_t(in.meta.addend));       // rax = relative ptr
                e.mov_reg_reg(Reg::rdx, Reg::rax);                      // rdx = ptr (saved)
                load_global_to_rax(e, int32_t(in.meta.addend) + 8);    // rax = len
                e.mov_reg_reg(Reg::rcx, Reg::rax);                      // rcx = len (saved)
                e.mov_reg_reg(Reg::rax, Reg::rdx);                      // rax = ptr
                e.mov_reg_imm64_external(Reg::r11, AbsFixup::GlobalsBase);
                e.add_reg_reg(Reg::rax, Reg::r11);                      // rax = absolute ptr
                e.mov_reg_reg(Reg::rdx, Reg::rcx);                      // rdx = len
                record_dst_rax(in.dst, ty);
            } else if (ty && ty->is_float()) {
                load_global_to_rax(e, int32_t(in.meta.addend));
                if (ty->prim == Prim::F64) {
                    e.byte(0x66); e.byte(0x48); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0); // movq xmm0,rax
                } else {
                    e.byte(0x66); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0); // movd xmm0,eax
                }
                record_dst_rax(in.dst, ty);
            } else {
                load_global_to_rax(e, int32_t(in.meta.addend));
                normalize_rax(ty);
                record_dst_rax(in.dst, ty);
            }
            break;
        }
        case ThinOp::StoreGlobal: {
            // [globals_base + meta.addend] = src1
            const Type* ty = in.meta.type ? in.meta.type : vreg_type(in.src1);
            if (ty && ty->is_float()) {
                load_float_vreg(in.src1);
                store_xmm0_to_global(e, int32_t(in.meta.addend), ty);
            } else if (ty && ty->is_slice) {
                load_slice_vreg(in.src1);
                // store ptr (relative) and len
                // The tree-walker stores the relative ptr for slice globals. But
                // the slice ABI has rax=absolute ptr. For a global slice store,
                // the ptr should be stored as a relative offset (globals_base-
                // relative). This mirrors the tree-walker's AssignExpr slice-
                // global store. However the tree-walker's global slice store is
                // complex; for value-equivalence we store the absolute ptr and
                // len. A round-trip through .em may need the relative form, but
                // Stage A focuses on JIT value-equivalence.
                e.mov_reg_imm64_external(Reg::r11, AbsFixup::GlobalsBase);
                e.store_reg_mem(Reg::r11, int32_t(in.meta.addend), Reg::rax);
                e.store_reg_mem(Reg::r11, int32_t(in.meta.addend) + 8, Reg::rdx);
            } else {
                load_int_vreg(in.src1);
                normalize_rax(ty);
                store_rax_to_global(e, int32_t(in.meta.addend));
            }
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
            //   otherwise both sides are rbp-relative (copy_frame_frame /
            //   copy_frame_vptr). copy_bytes() touches only rax, so r10 (globals
            //   base) and r11 (vreg dest ptr) are safe as base registers here.
            const int32_t len = in.meta.len;
            const bool dst_is_vreg  = (in.dst != 0);
            const bool global       = (in.meta.base_kind == AbsFixup::GlobalsBase);
            const bool src_is_global = global && (dst_is_vreg || in.src1 == 0);
            const bool dst_is_global = global && !dst_is_vreg && in.src1 != 0;

            Reg dst_base = Reg::rbp; int32_t dst_off = in.meta.frame_off;
            Reg src_base = Reg::rbp; int32_t src_off = in.meta.field_off;

            if (dst_is_vreg) {
                load_int_vreg(in.dst);                // hidden ptr -> rax
                e.mov_reg_reg(Reg::r11, Reg::rax);    // r11 = dest ptr
                dst_base = Reg::r11; dst_off = 0;
            } else if (dst_is_global) {
                e.mov_reg_imm64_external(Reg::r10, AbsFixup::GlobalsBase);
                dst_base = Reg::r10; dst_off = in.meta.frame_off;
            }
            if (src_is_global) {
                e.mov_reg_imm64_external(Reg::r10, AbsFixup::GlobalsBase);
                src_base = Reg::r10; src_off = in.meta.field_off;
            }
            copy_bytes(dst_base, dst_off, src_base, src_off, len);
            // copy_bytes clobbers rax; reset the register tracking.
            rax_vreg = 0;
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
            emit_int_divmod(in, false);
            break;
        case ThinOp::Mod:
            emit_int_divmod(in, true);
            break;
        case ThinOp::Neg: {
            load_int_vreg(in.src1);
            // neg rax: REX.W F7 /3 -> 48 F7 D8
            e.byte(0x48); e.byte(0xF7); e.byte(0xD8);
            normalize_rax(in.meta.type);
            record_dst_rax(in.dst, in.meta.type);
            if (in.meta.frame_off != 0 && in.dst != 0) {
                store_rax_to_rbp(e, in.meta.frame_off);
                vregs[in.dst].frame_off = in.meta.frame_off;
            }
            break;
        }
        case ThinOp::Not: {
            // logical not: rax = (rax == 0) ? 1 : 0
            load_int_vreg(in.src1);
            e.cmp_reg_imm32(Reg::rax, 0);
            e.byte(0x0F); e.byte(0x94); e.byte(0xC0); // sete al
            e.byte(0x48); e.byte(0x0F); e.byte(0xB6); e.byte(0xC0); // movzx rax,al
            record_dst_rax(in.dst, in.meta.type);
            if (in.meta.frame_off != 0 && in.dst != 0) {
                store_rax_to_rbp(e, in.meta.frame_off);
                vregs[in.dst].frame_off = in.meta.frame_off;
            }
            break;
        }
        case ThinOp::BitNot: {
            load_int_vreg(in.src1);
            // not rax: REX.W F7 /2 -> 48 F7 D0
            e.byte(0x48); e.byte(0xF7); e.byte(0xD0);
            normalize_rax(in.meta.type);
            record_dst_rax(in.dst, in.meta.type);
            if (in.meta.frame_off != 0 && in.dst != 0) {
                store_rax_to_rbp(e, in.meta.frame_off);
                vregs[in.dst].frame_off = in.meta.frame_off;
            }
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
            // lea rax, [rbp + meta.frame_off + meta.field_off]
            int32_t addr = in.meta.frame_off + in.meta.field_off;
            e.byte(0x48); e.byte(0x8D); e.byte(0x85); e.imm32(addr); // lea rax, [rbp+addr]
            record_dst_rax(in.dst, in.meta.type);
            if (in.meta.frame_off != 0 && in.dst != 0) {
                store_rax_to_rbp(e, in.meta.frame_off);
                vregs[in.dst].frame_off = in.meta.frame_off;
            }
            break;
        }
        case ThinOp::IndexAddr: {
            // rax = base + index * width  (emit convention: src1=base, src2=index)
            int32_t width = in.meta.width;
            // materialize index into rax, scale by width, then SAVE it across the
            // base load (the base load clobbers rax). rcx holds index*width.
            if (in.src2 == 0) {
                // immediate index
                e.mov_reg_imm64(Reg::rax, in.imm.i);
            } else {
                load_int_vreg(in.src2);
            }
            if (width > 1) {
                e.mov_reg_imm64(Reg::rcx, int64_t(width));
                e.imul_reg_reg(Reg::rax, Reg::rcx);
            }
            e.mov_reg_reg(Reg::rcx, Reg::rax);  // rcx = index*width (saved across base load)
            // add base
            if (in.src1 != 0 && vreg_is_slice(in.src1)) {
                // slice base: ptr is in the slice VReg's frame slot or rax
                load_slice_vreg(in.src1);  // rax = ptr, rdx = len
                e.mov_reg_reg(Reg::r11, Reg::rax);
            } else if (in.src1 != 0) {
                load_int_vreg(in.src1);  // rax = base address
                e.mov_reg_reg(Reg::r11, Reg::rax);
            } else if (in.meta.base_kind == AbsFixup::GlobalsBase) {
                // global fixed-array base: globals_base + addend
                e.mov_reg_imm64_external(Reg::r11, AbsFixup::GlobalsBase);
                if (in.meta.addend != 0) {
                    e.mov_reg_imm64(Reg::rax, int64_t(int32_t(in.meta.addend)));
                    e.add_reg_reg(Reg::r11, Reg::rax);
                }
            } else {
                // fixed array base at meta.frame_off (local)
                e.byte(0x48); e.byte(0x8D); e.byte(0x9D); e.imm32(in.meta.frame_off); // lea r11, [rbp+frame_off]
            }
            e.add_reg_reg(Reg::r11, Reg::rcx);  // r11 = base + index*width
            e.mov_reg_reg(Reg::rax, Reg::r11);  // rax = element address
            record_dst_rax(in.dst, in.meta.type);
            if (in.meta.frame_off != 0 && in.dst != 0) {
                store_rax_to_rbp(e, in.meta.frame_off);
                vregs[in.dst].frame_off = in.meta.frame_off;
            }
            break;
        }
        case ThinOp::BoundsCheck: {
            // materialize index into rcx; check against len (vreg or imm)
            if (in.src1 != 0) load_int_vreg(in.src1);
            e.mov_reg_reg(Reg::rcx, Reg::rax);
            if (in.src2 != 0) {
                load_int_vreg(in.src2);
                e.mov_reg_reg(Reg::r9, Reg::rax);  // r9 = len
                emit_bounds_check_reg(Reg::rcx, Reg::r9);
            } else {
                emit_bounds_check_imm(Reg::rcx, int64_t(in.meta.len));
            }
            break;
        }
        case ThinOp::DivOverflowCheck: {
            // the overflow check portion of emit_integer_divmod (standalone).
            // rax = dividend (src1), rcx = divisor (src2). Checks signed overflow
            // (rax == INT64_MIN && rcx == -1).
            int reason = in.meta.trap_reason ? int(in.meta.trap_reason) : int(TrapReason::DivByZero);
            if (in.src1 != 0) load_int_vreg(in.src1);
            if (in.src2 != 0) {
                e.push(Reg::rax);
                load_int_vreg(in.src2);
                e.mov_reg_reg(Reg::rcx, Reg::rax);
                e.pop(Reg::rax);
            }
            if (!in.meta.is_unsigned) {
                Label safe = e.alloc_label(), overflow = e.alloc_label();
                e.cmp_reg_imm32(Reg::rcx, -1); e.jcc(Cond::ne, safe);
                e.mov_reg_imm64(Reg::r10, INT64_MIN); e.cmp_reg_reg(Reg::rax, Reg::r10); e.jcc(Cond::e, overflow);
                e.jmp(safe); e.bind(overflow);
                emit_trap(reason, "signed division overflow"); e.bind(safe);
            }
            break;
        }
        case ThinOp::MakeSlice: {
            // materialize slice {rax=ptr, rdx=len} from a backing array.
            //   LOCAL fixed array: ptr = lea rax, [rbp + meta.frame_off]
            //   GLOBAL fixed array: ptr = globals_base + meta.addend
            //     (the lowerer sets meta.base_kind = GlobalsBase +
            //      meta.addend = global_offset for a global-array ViewExpr;
            //      without this check MakeSlice always used the frame-relative
            //      lea, so g[..] pointed at a stack slot instead of the global —
            //      the C3 defect. Mirrors the IndexAddr GlobalsBase path above.)
            if (in.meta.base_kind == AbsFixup::GlobalsBase) {
                e.mov_reg_imm64_external(Reg::r11, AbsFixup::GlobalsBase);
                if (in.meta.addend != 0) {
                    e.mov_reg_imm64(Reg::rax, int64_t(int32_t(in.meta.addend)));
                    e.add_reg_reg(Reg::r11, Reg::rax);
                }
                e.mov_reg_reg(Reg::rax, Reg::r11);
            } else {
                e.byte(0x48); e.byte(0x8D); e.byte(0x85); e.imm32(in.meta.frame_off); // lea rax, [rbp+frame_off]
            }
            e.mov_reg_imm64(Reg::rdx, int64_t(in.meta.len));
            record_dst_rax(in.dst, in.meta.type);
            if (in.dst != 0) {
                vregs[in.dst].type = in.meta.type;
                if (in.meta.frame_off != 0) {
                    // the slice's own slot (if assigned) is different from the backing;
                    // but if the lowering gave the dst a frame_off for the slice slot,
                    // store there. Here meta.frame_off is the BACKING, so we don't store.
                }
            }
            break;
        }
        case ThinOp::StructLitInit: {
            // store src1 (field value) at [rbp + meta.frame_off + meta.field_off]
            int32_t addr = in.meta.frame_off + in.meta.field_off;
            const Type* ft = in.meta.type;
            if (ft && ft->is_float()) {
                load_float_vreg(in.src1);
                store_xmm0_to_rbp(e, addr, ft);
            } else if (ft && ft->is_slice) {
                load_slice_vreg(in.src1);
                e.store_reg_mem(Reg::rbp, addr, Reg::rax);
                e.store_reg_mem(Reg::rbp, addr + 8, Reg::rdx);
            } else {
                load_int_vreg(in.src1);
                // store with the field's byte width
                int32_t w = value_bytes(ft, structs());
                if (w < 8) {
                    e.store_rax_elem(Reg::rbp, addr, w);
                } else {
                    store_rax_to_rbp(e, addr);
                }
            }
            break;
        }
        case ThinOp::ArrayLitInit: {
            // store src1 (element value) at [rbp + meta.frame_off + meta.field_off]
            int32_t addr = in.meta.frame_off + in.meta.field_off;
            const Type* et = in.meta.type;
            if (et && et->is_float()) {
                load_float_vreg(in.src1);
                store_xmm0_to_rbp(e, addr, et);
            } else if (et && et->is_slice) {
                load_slice_vreg(in.src1);
                e.store_reg_mem(Reg::rbp, addr, Reg::rax);
                e.store_reg_mem(Reg::rbp, addr + 8, Reg::rdx);
            } else {
                load_int_vreg(in.src1);
                int32_t w = value_bytes(et, structs());
                if (w < 8) {
                    e.store_rax_elem(Reg::rbp, addr, w);
                } else {
                    store_rax_to_rbp(e, addr);
                }
            }
            break;
        }

        // ── guards (safety) ──
        case ThinOp::DepthCheck:
            emit_depth_check();
            break;
        case ThinOp::BudgetCheck:
            emit_budget_check(int64_t(in.imm.i), "budget exceeded");
            break;
        case ThinOp::CallTargetGuard:
            emit_call_target_guard();
            break;
        }
    }

    // ─── integer binary op (push/pop across rhs, matching the tree-walker) ───
    void emit_int_binop(const ThinInstr& in) {
        const Type* ty = in.meta.type;
        bool is_unsigned = in.meta.is_unsigned != 0;
        // materialize src1 (lhs) into rax
        load_int_vreg(in.src1);
        if (in.src2 == 0) {
            // immediate form: op rax, imm.i
            int64_t imm = in.imm.i;
            switch (in.op) {
            case ThinOp::Add:
                if (imm >= INT32_MIN && imm <= INT32_MAX) {
                    // add rax, imm32: REX.W 81 /0 id (but we don't have add_reg_imm32;
                    // use the raw bytes)
                    e.byte(0x48); e.byte(0x81); e.byte(0xC0); e.imm32(int32_t(imm));
                } else {
                    e.mov_reg_imm64(Reg::rcx, imm);
                    e.add_reg_reg(Reg::rax, Reg::rcx);
                }
                break;
            case ThinOp::Sub:
                if (imm >= INT32_MIN && imm <= INT32_MAX) {
                    e.byte(0x48); e.byte(0x81); e.byte(0xE8); e.imm32(int32_t(imm)); // sub rax, imm32
                } else {
                    e.mov_reg_imm64(Reg::rcx, imm);
                    e.sub_reg_reg(Reg::rax, Reg::rcx);
                }
                break;
            case ThinOp::Mul:
                e.mov_reg_imm64(Reg::rcx, imm);
                e.imul_reg_reg(Reg::rax, Reg::rcx);
                break;
            case ThinOp::And:
                e.mov_reg_imm64(Reg::rcx, imm);
                e.byte(0x48); e.byte(0x21); e.byte(0xC8); // and rax,rcx
                break;
            case ThinOp::Or:
                e.mov_reg_imm64(Reg::rcx, imm);
                e.byte(0x48); e.byte(0x09); e.byte(0xC8); // or rax,rcx
                break;
            case ThinOp::Xor:
                e.mov_reg_imm64(Reg::rcx, imm);
                e.byte(0x48); e.byte(0x31); e.byte(0xC8); // xor rax,rcx
                break;
            case ThinOp::Shl:
                if (imm >= 1 && imm <= 63) {
                    // shl rax, imm8: REX.W C1 /4 ib (4 bytes — the short form)
                    e.byte(0x48); e.byte(0xC1); e.byte(0xE0); e.byte(uint8_t(imm));
                } else {
                    e.mov_reg_imm64(Reg::rcx, imm);
                    e.byte(0x48); e.byte(0xD3); e.byte(0xE0); // shl rax,cl
                }
                break;
            case ThinOp::Shr:
                if (imm >= 1 && imm <= 63) {
                    // shr/sar rax, imm8: REX.W C1 /5|/7 ib (4 bytes — the short form)
                    e.byte(0x48); e.byte(0xC1);
                    e.byte(is_unsigned ? 0xE8 : 0xF8);
                    e.byte(uint8_t(imm));
                } else {
                    e.mov_reg_imm64(Reg::rcx, imm);
                    e.byte(0x48); e.byte(0xD3); e.byte(is_unsigned ? 0xE8 : 0xF8); // shr/sar rax,cl
                }
                break;
            default: break;
            }
        } else {
            // VReg form: push rax (lhs); load src2 (rhs); mov rcx; pop rax; op
            e.push(Reg::rax);
            load_int_vreg(in.src2);
            e.mov_reg_reg(Reg::rcx, Reg::rax);  // rcx = rhs
            e.pop(Reg::rax);                     // rax = lhs
            switch (in.op) {
            case ThinOp::Add: e.add_reg_reg(Reg::rax, Reg::rcx); break;
            case ThinOp::Sub: e.sub_reg_reg(Reg::rax, Reg::rcx); break;
            case ThinOp::Mul: e.imul_reg_reg(Reg::rax, Reg::rcx); break;
            case ThinOp::And: e.byte(0x48); e.byte(0x21); e.byte(0xC8); break; // and rax,rcx
            case ThinOp::Or:  e.byte(0x48); e.byte(0x09); e.byte(0xC8); break; // or rax,rcx
            case ThinOp::Xor: e.byte(0x48); e.byte(0x31); e.byte(0xC8); break; // xor rax,rcx
            case ThinOp::Shl: e.byte(0x48); e.byte(0xD3); e.byte(0xE0); break; // shl rax,cl
            case ThinOp::Shr: e.byte(0x48); e.byte(0xD3); e.byte(is_unsigned ? 0xE8 : 0xF8); break;
            default: break;
            }
        }
        normalize_rax(ty);
        record_dst_rax(in.dst, ty);
        if (in.meta.frame_off != 0 && in.dst != 0) {
            store_rax_to_rbp(e, in.meta.frame_off);
            vregs[in.dst].frame_off = in.meta.frame_off;
        }
    }

    // ─── integer div/mod (emit_integer_divmod with src materialization) ───
    void emit_int_divmod(const ThinInstr& in, bool want_mod) {
        bool is_unsigned = in.meta.is_unsigned != 0;
        // rax = dividend (src1), rcx = divisor (src2 or imm)
        load_int_vreg(in.src1);
        if (in.src2 == 0) {
            e.mov_reg_imm64(Reg::rcx, in.imm.i);
        } else {
            // need to preserve rax (dividend) while loading src2 into rcx.
            // push rax; load src2; mov rcx, rax; pop rax.
            e.push(Reg::rax);
            load_int_vreg(in.src2);
            e.mov_reg_reg(Reg::rcx, Reg::rax);
            e.pop(Reg::rax);
        }
        emit_integer_divmod(want_mod, is_unsigned);
        normalize_rax(in.meta.type);
        record_dst_rax(in.dst, in.meta.type);
        if (in.meta.frame_off != 0 && in.dst != 0) {
            store_rax_to_rbp(e, in.meta.frame_off);
            vregs[in.dst].frame_off = in.meta.frame_off;
        }
    }

    // ─── float binary op (sub rsp,8 / movsd + SSE op, matching the tree-walker) ───
    void emit_float_binop(const ThinInstr& in) {
        bool f64 = !in.meta.is_f32;
        // eval lhs (src1) -> xmm0; sub rsp,8; save xmm0; eval rhs (src2) -> xmm0; reload lhs to xmm1
        load_float_vreg(in.src1);
        e.sub_reg_imm32(Reg::rsp, 8);
        if (f64) e.movsd_mem_xmm(Reg::rsp, 0, Xmm::xmm0); else e.movss_mem_xmm(Reg::rsp, 0, Xmm::xmm0);
        if (in.src2 == 0) {
            // immediate float
            if (f64) {
                uint64_t bits; std::memcpy(&bits, &in.imm.f, 8);
                e.mov_reg_imm64(Reg::rax, int64_t(bits));
                e.byte(0x66); e.byte(0x48); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0); // movq xmm0,rax
            } else {
                float fv = float(in.imm.f); uint32_t bits; std::memcpy(&bits, &fv, 4);
                e.mov_reg_imm64(Reg::rax, int64_t(int32_t(bits)));
                e.byte(0x66); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0); // movd xmm0,eax
            }
        } else {
            load_float_vreg(in.src2);
        }
        if (f64) e.movsd_xmm_mem(Xmm::xmm1, Reg::rsp, 0); else e.movss_xmm_mem(Xmm::xmm1, Reg::rsp, 0);
        e.add_reg_imm32(Reg::rsp, 8);
        switch (in.op) {
        case ThinOp::FAdd:
            if (f64) e.addsd_xmm(Xmm::xmm0, Xmm::xmm1); else e.addss_xmm(Xmm::xmm0, Xmm::xmm1);
            break;
        case ThinOp::FMul:
            if (f64) e.mulsd_xmm(Xmm::xmm0, Xmm::xmm1); else e.mulss_xmm(Xmm::xmm0, Xmm::xmm1);
            break;
        case ThinOp::FSub:
            if (f64) { e.subsd_xmm(Xmm::xmm1, Xmm::xmm0); e.movsd_xmm_xmm(Xmm::xmm0, Xmm::xmm1); }
            else     { e.subss_xmm(Xmm::xmm1, Xmm::xmm0); e.movss_xmm_xmm(Xmm::xmm0, Xmm::xmm1); }
            break;
        case ThinOp::FDiv:
            if (f64) { e.divsd_xmm(Xmm::xmm1, Xmm::xmm0); e.movsd_xmm_xmm(Xmm::xmm0, Xmm::xmm1); }
            else     { e.divss_xmm(Xmm::xmm1, Xmm::xmm0); e.movss_xmm_xmm(Xmm::xmm0, Xmm::xmm1); }
            break;
        case ThinOp::FMod:
            // float modulo: call a native fmod/fmodf. The tree-walker doesn't have
            // a direct FMod (it would be a native call). For Stage A, emit a trap
            // (the lowering should use CallNative for float mod). If reached, it's
            // a lowering issue.
            emit_trap(int(TrapReason::IllegalInstruction), "float mod not directly emittable (use CallNative)");
            break;
        default: break;
        }
        record_dst_rax(in.dst, in.meta.type);
        if (in.meta.frame_off != 0 && in.dst != 0) {
            store_xmm0_to_rbp(e, in.meta.frame_off, in.meta.type);
            vregs[in.dst].frame_off = in.meta.frame_off;
        }
    }

    // ─── compare (cmp + setcc + movzx) ───
    void emit_cmp(const ThinInstr& in) {
        // result is bool (0/1). predicate in meta.cmp (0=Eq..5=Ge).
        const Type* ty = in.meta.type;  // the operand type (for signed/unsigned)
        bool is_float = (in.meta.is_f32 != 0) || (ty && ty->is_float());
        bool is_unsigned = in.meta.is_unsigned != 0;
        uint8_t cmp_pred = in.meta.cmp;  // 0=Eq,1=Neq,2=Lt,3=Le,4=Gt,5=Ge

        if (is_float) {
            bool f64 = !in.meta.is_f32;
            load_float_vreg(in.src1);
            e.sub_reg_imm32(Reg::rsp, 8);
            if (f64) e.movsd_mem_xmm(Reg::rsp, 0, Xmm::xmm0); else e.movss_mem_xmm(Reg::rsp, 0, Xmm::xmm0);
            if (in.src2 == 0) {
                if (f64) {
                    uint64_t bits; std::memcpy(&bits, &in.imm.f, 8);
                    e.mov_reg_imm64(Reg::rax, int64_t(bits));
                    e.byte(0x66); e.byte(0x48); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0);
                } else {
                    float fv = float(in.imm.f); uint32_t bits; std::memcpy(&bits, &fv, 4);
                    e.mov_reg_imm64(Reg::rax, int64_t(int32_t(bits)));
                    e.byte(0x66); e.byte(0x0F); e.byte(0x6E); e.byte(0xC0);
                }
            } else {
                load_float_vreg(in.src2);
            }
            if (f64) e.movsd_xmm_mem(Xmm::xmm1, Reg::rsp, 0); else e.movss_xmm_mem(Xmm::xmm1, Reg::rsp, 0);
            e.add_reg_imm32(Reg::rsp, 8);
            // ucomi{sd,ss} xmm1(lhs), xmm0(rhs); setcc
            if (f64) e.ucomisd_xmm(Xmm::xmm1, Xmm::xmm0); else e.ucomiss_xmm(Xmm::xmm1, Xmm::xmm0);
            uint8_t cc = 0;
            switch (cmp_pred) {
            case 0: cc = 0x4; break; // Eq
            case 1: cc = 0x5; break; // Neq
            case 2: cc = 0x2; break; // Lt (setb)
            case 3: cc = 0x6; break; // Le (setbe)
            case 4: cc = 0x7; break; // Gt (seta)
            case 5: cc = 0x3; break; // Ge (setae)
            }
            e.byte(0x0F); e.byte(0x90 | cc); e.byte(0xC0);              // setcc al
            e.byte(0x48); e.byte(0x0F); e.byte(0xB6); e.byte(0xC0);     // movzx rax,al
        } else {
            // integer compare
            load_int_vreg(in.src1);
            if (in.src2 == 0) {
                // immediate
                int64_t imm = in.imm.i;
                if (imm >= INT32_MIN && imm <= INT32_MAX) {
                    e.cmp_reg_imm32(Reg::rax, int32_t(imm));
                } else {
                    // imm doesn't fit in int32: materialize into rcx, compare reg-reg
                    e.mov_reg_imm64(Reg::rcx, imm);
                    e.cmp_reg_reg(Reg::rax, Reg::rcx);  // cmp lhs(rax), rhs(rcx)
                }
            } else {
                e.push(Reg::rax);
                load_int_vreg(in.src2);
                e.mov_reg_reg(Reg::rcx, Reg::rax);
                e.pop(Reg::rax);
                e.cmp_reg_reg(Reg::rax, Reg::rcx);  // cmp lhs(rax), rhs(rcx)
            }
            uint8_t cc = 0;
            switch (cmp_pred) {
            case 0: cc = 0x4; break;  // Eq (sete)
            case 1: cc = 0x5; break;  // Neq (setne)
            case 2: cc = is_unsigned ? 0x2 : 0xC; break;  // Lt (setb/setl)
            case 3: cc = is_unsigned ? 0x6 : 0xE; break;  // Le (setbe/setle)
            case 4: cc = is_unsigned ? 0x7 : 0xF; break;  // Gt (seta/setg)
            case 5: cc = is_unsigned ? 0x3 : 0xD; break;  // Ge (setae/setge)
            }
            e.byte(0x0F); e.byte(0x90 | cc); e.byte(0xC0);              // setcc al
            e.byte(0x48); e.byte(0x0F); e.byte(0xB6); e.byte(0xC0);     // movzx rax,al
        }
        // result is bool (no normalize needed; setcc+movzx yields 0/1)
        record_dst_rax(in.dst, in.meta.type ? in.meta.type : &type_bool());
        if (in.dst != 0 && in.meta.type) vregs[in.dst].type = in.meta.type;
        if (in.meta.frame_off != 0 && in.dst != 0) {
            store_rax_to_rbp(e, in.meta.frame_off);
            vregs[in.dst].frame_off = in.meta.frame_off;
        }
    }

    // ─── short-circuit logical (LAnd / LOr) ───
    void emit_logical(const ThinInstr& in) {
        bool is_and = (in.op == ThinOp::LAnd);
        Label false_l = e.alloc_label(), end_l = e.alloc_label();
        load_int_vreg(in.src1);
        e.cmp_reg_imm32(Reg::rax, 0);
        if (is_and) e.jcc(Cond::e, false_l);   // LAnd: lhs false -> result false
        else        e.jcc(Cond::ne, end_l);    // LOr: lhs true -> result true
        load_int_vreg(in.src2);
        e.cmp_reg_imm32(Reg::rax, 0);
        e.jcc(Cond::ne, end_l);  // rhs true -> 1
        e.bind(false_l);
        e.mov_reg_imm64(Reg::rax, 0);
        e.bind(end_l);
        // normalize: rax = (rax != 0) ? 1 : 0
        e.cmp_reg_imm32(Reg::rax, 0);
        Label done = e.alloc_label();
        e.mov_reg_imm64(Reg::rax, 0); e.jcc(Cond::e, done);
        e.mov_reg_imm64(Reg::rax, 1); e.bind(done);
        record_dst_rax(in.dst, in.meta.type ? in.meta.type : &type_bool());
        if (in.dst != 0 && in.meta.type) vregs[in.dst].type = in.meta.type;
        if (in.meta.frame_off != 0 && in.dst != 0) {
            store_rax_to_rbp(e, in.meta.frame_off);
            vregs[in.dst].frame_off = in.meta.frame_off;
        }
    }

    // ─── cast (int<->int width, int<->float, f32<->f64) ───
    void emit_cast(const ThinInstr& in) {
        const Type* from = vreg_type(in.src1);
        const Type* to = in.meta.type;
        if (!from) from = in.meta.type;  // best-effort if src type unknown
        const bool plain_from_int = from && from->is_int() && !from->is_fn_handle && from->struct_name.empty();
        const bool plain_to_int = to && to->is_int() && !to->is_fn_handle && to->struct_name.empty();

        if (from && to && from->same(*to)) {
            // same-type: just move
            load_int_vreg(in.src1);
            record_dst_rax(in.dst, to);
            if (in.meta.frame_off != 0 && in.dst != 0) {
                store_rax_to_rbp(e, in.meta.frame_off);
                vregs[in.dst].frame_off = in.meta.frame_off;
            }
            return;
        }
        if (plain_from_int && plain_to_int) {
            load_int_vreg(in.src1);
            normalize_rax(to);
            record_dst_rax(in.dst, to);
            if (in.meta.frame_off != 0 && in.dst != 0) {
                store_rax_to_rbp(e, in.meta.frame_off);
                vregs[in.dst].frame_off = in.meta.frame_off;
            }
            return;
        }
        if (from && to && from->is_float() && to->is_float()) {
            load_float_vreg(in.src1);
            if (from->prim == Prim::F32 && to->prim == Prim::F64)
                { e.byte(0xF3); e.byte(0x0F); e.byte(0x5A); e.byte(0xC0); } // cvtss2sd
            else if (from->prim == Prim::F64 && to->prim == Prim::F32)
                { e.byte(0xF2); e.byte(0x0F); e.byte(0x5A); e.byte(0xC0); } // cvtsd2ss
            record_dst_rax(in.dst, to);
            if (in.meta.frame_off != 0 && in.dst != 0) {
                store_xmm0_to_rbp(e, in.meta.frame_off, to);
                vregs[in.dst].frame_off = in.meta.frame_off;
            }
            return;
        }
        if (plain_from_int && !from->is_uint() && to && to->is_float()) {
            load_int_vreg(in.src1);
            normalize_rax(from);
            e.byte(to->prim == Prim::F64 ? 0xF2 : 0xF3); e.byte(0x48); e.byte(0x0F); e.byte(0x2A); e.byte(0xC0); // cvtsi2sd/ss
            record_dst_rax(in.dst, to);
            if (in.meta.frame_off != 0 && in.dst != 0) {
                store_xmm0_to_rbp(e, in.meta.frame_off, to);
                vregs[in.dst].frame_off = in.meta.frame_off;
            }
            return;
        }
        if (from && from->is_float() && plain_to_int && !to->is_uint()) {
            load_float_vreg(in.src1);
            e.byte(from->prim == Prim::F64 ? 0xF2 : 0xF3); e.byte(0x48); e.byte(0x0F); e.byte(0x2C); e.byte(0xC0); // cvttsd2si/cvtss2si
            normalize_rax(to);
            record_dst_rax(in.dst, to);
            if (in.meta.frame_off != 0 && in.dst != 0) {
                store_rax_to_rbp(e, in.meta.frame_off);
                vregs[in.dst].frame_off = in.meta.frame_off;
            }
            return;
        }
        // unknown cast: trap (mirrors the tree-walker's assert)
        emit_trap(int(TrapReason::IllegalInstruction), "internal: invalid cast reached emit");
    }

    // ─── call emission (all 4 call kinds share the arg-stash) ───
    void emit_call(const ThinInstr& in) {
        bool is_indirect = (in.op == ThinOp::CallIndirect);
        bool ret_struct = call_returns_struct_by_ptr(in.ret_type);

        // Decode the hidden dest for a struct-by-ptr return call. lower_call
        // encodes it as args[0]: either a vreg (hidden_dest_vreg != 0, with
        // arg_frame_offs[0] == -1 — a forward-return forwarding the incoming
        // hidden ptr, or a computed dest address) or a sentinel (args[0] == 0,
        // with arg_frame_offs[0] = the dest frame slot offset). We must NOT use
        // thf.frame.struct_ret_ptr_offset here — that is THIS function's own
        // incoming hidden ptr, which is only the right dest for a forward-
        // returning call (and that case is already encoded in args[0] as the
        // loaded-ptr vreg).
        VReg hidden_dest_vreg = 0;
        int32_t hidden_dest_off = 0;
        if (ret_struct) {
            VReg a0 = in.args.empty() ? 0 : in.args[0];
            int32_t afo0 = in.arg_frame_offs.empty() ? -1 : in.arg_frame_offs[0];
            if (a0 != 0 && afo0 == -1) {
                hidden_dest_vreg = a0;  // dest ptr is in this vreg
            } else {
                hidden_dest_off = afo0;  // dest is a frame slot
            }
        }

        // For indirect calls: materialize the target handle into rax + guard BEFORE the stash.
        // src1 = the handle VReg for CallIndirect.
        if (is_indirect) {
            load_int_vreg(in.src1);
            emit_call_target_guard();
        }

        // Build operand layout (skip args[0] = hidden dest when ret_struct)
        std::vector<CallArg> ops = build_call_args(in, ret_struct);

        // Emit arg stash + ABI reg reload (handles sub rsp, stash, reg load, outgoing)
        emit_call_arg_stash(in, ops, ret_struct, hidden_dest_vreg, hidden_dest_off, is_indirect);

        // The actual call instruction
        switch (in.op) {
        case ThinOp::CallNative:
            emit_native_call(in);
            break;
        case ThinOp::CallScript:
            emit_script_call(in);
            break;
        case ThinOp::CallCrossModule:
            emit_cross_module_call(in);
            break;
        case ThinOp::CallIndirect:
            emit_indirect_call();
            break;
        default: break;
        }

        // add rsp, total (balance the sub rsp from the stash)
        e.add_reg_imm32(Reg::rsp, last_call_total);

        // Any call clobbers rax / xmm0 (Win64 ABI); reset the register tracking
        // so a subsequent load_int_vreg does not take the stale-rax shortcut.
        rax_vreg = 0;
        xmm0_vreg = 0;

        // Record the result
        if (ret_struct) {
            // struct-by-ptr: the callee wrote through the hidden ptr; no register result.
            // The dst is a frame slot (meta.frame_off); no VReg to record.
            // rax may hold the hidden ptr (for native struct returns) or garbage.
        } else if (in.dst != 0) {
            // normalize narrow integer returns at the call boundary
            if (in.ret_type && in.ret_type->is_int()) normalize_rax(in.ret_type);
            record_dst_rax(in.dst, in.ret_type);
            if (in.meta.frame_off != 0) {
                if (in.ret_type && in.ret_type->is_float()) {
                    store_xmm0_to_rbp(e, in.meta.frame_off, in.ret_type);
                } else if (in.ret_type && in.ret_type->is_slice) {
                    e.store_reg_mem(Reg::rbp, in.meta.frame_off, Reg::rax);
                    e.store_reg_mem(Reg::rbp, in.meta.frame_off + 8, Reg::rdx);
                    vregs[in.dst + 1].frame_off = in.meta.frame_off + 8;
                    vregs[in.dst + 1].type = in.ret_type;
                } else {
                    store_rax_to_rbp(e, in.meta.frame_off);
                }
                vregs[in.dst].frame_off = in.meta.frame_off;
            }
        }
    }
};

} // anon namespace

CompiledFn emit_x64(const ThinFunction& thf, const CodeGenCtx& ctx) {
    EmitCtx ec(thf, ctx);
    return ec.emit();
}

} // namespace ember
