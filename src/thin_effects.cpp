// thin_effects.cpp — implementation of the public exhaustive effect model.
// See thin_effects.hpp for the design + the classification contract.

#include "thin_effects.hpp"

#include <cstdint>

namespace ember {

namespace {

// Is `ty` a two-word pair type (slice or lambda)? Both occupy 16 bytes and
// use the {ptr, len} / {fn_slot, env_ptr} two-vreg ABI.
bool is_pair_type(const Type* ty) {
    return ty && (ty->is_slice || ty->is_lambda);
}

// The byte span of a producing op's dst spill at [rbp + frame_off], derived
// from the exact emit behavior in src/thin_emit.cpp (mirrors dst_spill_span in
// thin_ir_ser.cpp). Slice/lambda = 16, F32 = 4, F64 = 8, int/bool = 8.
int64_t producer_spill_span(const Type* ty, uint8_t is_f32) {
    if (is_pair_type(ty)) return 16;
    if (ty && ty->is_float()) return (is_f32 != 0) ? 4 : 8;
    return 8;
}

EffectFlags flag(ThinEffectFlag f) { return EffectFlags{f}; }

ByteInterval frame_iv(int64_t begin, uint64_t size) {
    ByteInterval iv; iv.space = MemorySpace::Frame; iv.begin = begin; iv.size = size; return iv;
}
ByteInterval global_iv(int64_t begin, uint64_t size) {
    ByteInterval iv; iv.space = MemorySpace::Global; iv.begin = begin; iv.size = size; return iv;
}
ByteInterval rodata_iv(int64_t begin, uint64_t size) {
    ByteInterval iv; iv.space = MemorySpace::Rodata; iv.begin = begin; iv.size = size; return iv;
}
ByteInterval unknown_iv() {
    ByteInterval iv; iv.space = MemorySpace::Unknown; iv.unknown = true; return iv;
}

} // namespace

ThinEffectDescriptor classify_thin_effects(const ThinInstr& in) {
    ThinEffectDescriptor d;

    // Calls: conservatively effectful (call barrier). A struct-by-value call
    // arg also reads its source frame slot; the hidden struct-return dest is a
    // frame write. We record frame reads for arg_frame_offs entries != -1/0
    // (excluding the hidden return-dest word0 when ret_type is a struct).
    // A call with dst != 0 && frame_off != 0 (not struct-by-ptr) spills its
    // result to frame_off — an implicit frame-home WRITE the callee's
    // result pin performs (emit_call: pin_int_dst / store_xmm0_to_rbp /
    // slice two-word store). This is the effect DCE/const-prop must honor.
    switch (in.op) {
    case ThinOp::CallNative:
    case ThinOp::CallScript:
    case ThinOp::CallIndirect:
    case ThinOp::CallCrossModule: {
        d.flags |= flag(ThinEffectFlag::CallsUnknown);
        d.aliases_unknown_memory = true;  // a callee may touch any memory
        const bool ret_struct = (in.ret_type && !in.ret_type->struct_name.empty());
        const size_t first_arg = ret_struct ? 1 : 0;
        for (size_t i = first_arg; i < in.args.size(); ++i) {
            const int32_t afo = i < in.arg_frame_offs.size() ? in.arg_frame_offs[i] : -1;
            if (afo == -1 || afo == 0) continue;
            const Type* aty = (i < in.arg_types.size()) ? in.arg_types[i] : nullptr;
            uint64_t span = is_pair_type(aty) ? 16 : 8;
            d.flags |= flag(ThinEffectFlag::ReadsFrame);
            d.reads.push_back(frame_iv(int64_t(afo), span));
        }
        // The hidden struct-return dest (args[0]==0 && arg_frame_offs[0] != -1/0
        // when ret_type is a struct) is a frame WRITE the callee performs.
        if (ret_struct && !in.args.empty() &&
            in.args[0] == 0 && !in.arg_frame_offs.empty() &&
            in.arg_frame_offs[0] != -1 && in.arg_frame_offs[0] != 0) {
            d.flags |= flag(ThinEffectFlag::WritesFrame);
            d.writes.push_back(frame_iv(int64_t(in.arg_frame_offs[0]), 8));
        }
        // Implicit result frame-home write: a non-struct-by-ptr call with
        // dst != 0 && frame_off != 0 pins its result to that slot (the
        // emitter's pin_int_dst / store_xmm0_to_rbp / slice two-word store).
        // Slice results span 16 bytes (dst + dst+1).
        if (!ret_struct && in.dst != 0 && in.meta.frame_off != 0) {
            const Type* rt = in.ret_type;
            int64_t sp = producer_spill_span(rt, in.meta.is_f32);
            d.flags |= flag(ThinEffectFlag::WritesFrame);
            d.flags |= flag(ThinEffectFlag::ImplicitSpillWrite);
            d.writes.push_back(frame_iv(int64_t(in.meta.frame_off), uint64_t(sp)));
        }
        return d;
    }
    default:
        break;
    }

    // Trapping guards: may trap, never removable.
    switch (in.op) {
    case ThinOp::BoundsCheck:
    case ThinOp::DivOverflowCheck:
    case ThinOp::DepthCheck:
    case ThinOp::BudgetCheck:
    case ThinOp::CallTargetGuard:
        d.flags |= flag(ThinEffectFlag::MayTrap);
        // BoundsCheck/DivOverflowCheck do not touch memory themselves; they
        // guard a subsequent access. No frame/global intervals.
        return d;
    default:
        break;
    }

    // Trapping arithmetic: integer Div/Mod emit a div-by-zero / signed-
    // overflow trap (emit_integer_divmod); FMod directly emits a trap. These
    // are NOT pure — a dead-result rule must not remove them even if dst is
    // unused, because the trap is an observable effect.
    switch (in.op) {
    case ThinOp::Div:
    case ThinOp::Mod:
    case ThinOp::FMod: {
        d.flags |= flag(ThinEffectFlag::MayTrap);
        // A producer with a frame_off still pins its result to that slot.
        if (in.dst != 0 && in.meta.frame_off != 0) {
            const Type* rt = in.meta.type;
            int64_t sp = producer_spill_span(rt, in.meta.is_f32);
            d.flags |= flag(ThinEffectFlag::WritesFrame);
            d.flags |= flag(ThinEffectFlag::ImplicitSpillWrite);
            d.writes.push_back(frame_iv(int64_t(in.meta.frame_off), uint64_t(sp)));
        }
        return d;
    }
    default:
        break;
    }

    switch (in.op) {
    // ── pure value producers (no memory effect unless frame_off spill) ──
    case ThinOp::ConstInt:
    case ThinOp::ConstFloat:
    case ThinOp::ConstBool:
    case ThinOp::Move:
    case ThinOp::Add: case ThinOp::Sub: case ThinOp::Mul:
    case ThinOp::And: case ThinOp::Or:  case ThinOp::Xor:
    case ThinOp::Shl: case ThinOp::Shr:
    case ThinOp::Neg: case ThinOp::Not: case ThinOp::BitNot:
    case ThinOp::FAdd: case ThinOp::FSub: case ThinOp::FMul:
    case ThinOp::FDiv:
    case ThinOp::Cmp:
    case ThinOp::LAnd: case ThinOp::LOr:
    case ThinOp::Cast: {
        // A producer with a frame_off pins its result to that slot: an
        // implicit frame WRITE (the spill). This is the effect DCE/const-prop
        // must honor (a seemingly dead def whose slot is read later is live).
        if (in.dst != 0 && in.meta.frame_off != 0) {
            const Type* rt = in.meta.type;
            int64_t sp = producer_spill_span(rt, in.meta.is_f32);
            d.flags |= flag(ThinEffectFlag::WritesFrame);
            d.flags |= flag(ThinEffectFlag::ImplicitSpillWrite);
            d.writes.push_back(frame_iv(int64_t(in.meta.frame_off), uint64_t(sp)));
        }
        return d;
    }

    // ── explicit frame load/store ──
    case ThinOp::LoadFrame: {
        // Direct load (src1 == 0): reads [rbp + frame_off] — a Frame read.
        // Computed load (src1 != 0): reads [src1 + field_off] — a computed
        // address through a vreg pointer, so it is ReadsIndirect /
        // aliases_unknown_memory, NOT a frame read from frame_off. The
        // field_off is the displacement through the computed pointer, not an
        // rbp-relative offset.
        if (in.src1 != 0) {
            // Computed-address load: indirect read through src1.
            d.flags |= flag(ThinEffectFlag::ReadsIndirect);
            d.aliases_unknown_memory = true;
            d.reads.push_back(unknown_iv());
        } else if (in.meta.frame_off != 0) {
            // Direct rbp-relative load: reads frame_off.
            const Type* rt = in.meta.type;
            int64_t sp = producer_spill_span(rt, in.meta.is_f32);
            d.flags |= flag(ThinEffectFlag::ReadsFrame);
            d.reads.push_back(frame_iv(int64_t(in.meta.frame_off), uint64_t(sp)));
        }
        // A computed-address load (src1 != 0) also produces a result that may
        // be spilled to frame_off (implicit write).
        if (in.dst != 0 && in.src1 != 0 && in.meta.frame_off != 0) {
            const Type* rt = in.meta.type;
            int64_t sp = producer_spill_span(rt, in.meta.is_f32);
            d.flags |= flag(ThinEffectFlag::WritesFrame);
            d.flags |= flag(ThinEffectFlag::ImplicitSpillWrite);
            d.writes.push_back(frame_iv(int64_t(in.meta.frame_off), uint64_t(sp)));
        }
        return d;
    }
    case ThinOp::StoreFrame: {
        if (in.src2 != 0) {
            // [src2(computed) + frame_off]: an indirect/computed store, NOT an
            // rbp-relative local slot write. Aliases unknown memory.
            d.flags |= flag(ThinEffectFlag::WritesIndirect);
            d.aliases_unknown_memory = true;
            d.writes.push_back(unknown_iv());
        } else if (in.meta.frame_off != 0) {
            // Exact local slot write. Span: slice/lambda 16, F32 4, F64 8,
            // narrow packed-field `width`, else 8.
            const Type* rt = in.meta.type;
            int64_t sp;
            if (is_pair_type(rt)) sp = 16;
            else if (rt && rt->is_float()) sp = (in.meta.is_f32 != 0) ? 4 : 8;
            else if (in.meta.field_off != 0 && in.meta.width > 0 && in.meta.width <= 8)
                sp = in.meta.width;
            else sp = 8;
            d.flags |= flag(ThinEffectFlag::WritesFrame);
            d.writes.push_back(frame_iv(int64_t(in.meta.frame_off), uint64_t(sp)));
        }
        return d;
    }

    // ── globals ──
    case ThinOp::LoadGlobal: {
        d.flags |= flag(ThinEffectFlag::ReadsGlobal);
        d.reads.push_back(global_iv(int64_t(in.meta.addend), 8));
        // The result may be spilled to frame_off (implicit write).
        if (in.dst != 0 && in.meta.frame_off != 0) {
            const Type* rt = in.meta.type;
            int64_t sp = producer_spill_span(rt, in.meta.is_f32);
            d.flags |= flag(ThinEffectFlag::WritesFrame);
            d.flags |= flag(ThinEffectFlag::ImplicitSpillWrite);
            d.writes.push_back(frame_iv(int64_t(in.meta.frame_off), uint64_t(sp)));
        }
        return d;
    }
    case ThinOp::StoreGlobal: {
        d.flags |= flag(ThinEffectFlag::WritesGlobal);
        d.writes.push_back(global_iv(int64_t(in.meta.addend), 8));
        return d;
    }

    // ── indirect / computed-address store ──
    case ThinOp::StoreAddr: {
        // [src2(computed) + frame_off] = src1. Computed address -> unknown.
        d.flags |= flag(ThinEffectFlag::WritesIndirect);
        d.aliases_unknown_memory = true;
        d.writes.push_back(unknown_iv());
        return d;
    }

    // ── byte copies ──
    case ThinOp::CopyBytes: {
        // Source range: field_off (rbp-relative) when not globals-backed.
        // Dest range: frame_off (rbp-relative) when dst==0 and not globals.
        const bool global = (in.meta.base_kind == AbsFixup::GlobalsBase);
        const bool dst_is_vreg = (in.dst != 0);
        const bool dst_is_global = global && !dst_is_vreg && in.src1 != 0;
        const bool src_is_global = global && (dst_is_vreg || in.src1 == 0);
        int64_t len = int64_t(in.meta.len);
        if (len < 0) len = 0;
        // Source.
        if (src_is_global) {
            d.flags |= flag(ThinEffectFlag::ReadsGlobal);
            d.reads.push_back(global_iv(int64_t(in.meta.field_off), uint64_t(len)));
        } else if (in.meta.field_off != 0) {
            d.flags |= flag(ThinEffectFlag::ReadsFrame);
            d.reads.push_back(frame_iv(int64_t(in.meta.field_off), uint64_t(len)));
        } else if (in.src1 != 0) {
            // source through a vreg pointer -> unknown
            d.flags |= flag(ThinEffectFlag::ReadsIndirect);
            d.aliases_unknown_memory = true;
            d.reads.push_back(unknown_iv());
        }
        // Dest.
        if (dst_is_vreg) {
            // dst is a dest-pointer VReg that is READ (a pointer use), not a
            // produced scalar. No frame write; the dest is [dst+0].
            d.flags |= flag(ThinEffectFlag::WritesIndirect);
            d.aliases_unknown_memory = true;
            d.writes.push_back(unknown_iv());
        } else if (dst_is_global) {
            d.flags |= flag(ThinEffectFlag::WritesGlobal);
            d.writes.push_back(global_iv(int64_t(in.meta.frame_off), uint64_t(len)));
        } else if (in.meta.frame_off != 0) {
            d.flags |= flag(ThinEffectFlag::WritesFrame);
            d.writes.push_back(frame_iv(int64_t(in.meta.frame_off), uint64_t(len)));
        }
        return d;
    }

    // ── addresses / aggregates ──
    case ThinOp::FieldAddr:
    case ThinOp::IndexAddr: {
        // Compute an address (lea). No memory access. The result address may
        // escape to a call / indirect store, so flag EscapesAddress. The
        // result may be spilled to frame_off (implicit write).
        d.flags |= flag(ThinEffectFlag::EscapesAddress);
        if (in.dst != 0 && in.meta.frame_off != 0) {
            d.flags |= flag(ThinEffectFlag::WritesFrame);
            d.flags |= flag(ThinEffectFlag::ImplicitSpillWrite);
            d.writes.push_back(frame_iv(int64_t(in.meta.frame_off), 8));
        }
        return d;
    }
    case ThinOp::MakeSlice: {
        // Produces a two-word pair {ptr, len} from a backing array. The
        // emitter does NOT spill to meta.frame_off — frame_off is the BACKING
        // ARRAY ADDRESS (the lea base: `lea rax, [rbp+frame_off]`), not a
        // result spill slot. The slice result is kept in registers (rax/rdx)
        // and recorded via record_dst_rax. So there is NO implicit frame
        // write to frame_off. The ptr word may escape -> EscapesAddress.
        d.flags |= flag(ThinEffectFlag::EscapesAddress);  // the ptr word may escape
        return d;
    }
    case ThinOp::BoundsCheck:
    case ThinOp::DivOverflowCheck:
        // Handled above in the trap group; unreachable here but kept for
        // -Wswitch completeness. No memory effect.
        return d;
    case ThinOp::StructLitInit:
    case ThinOp::ArrayLitInit: {
        // Write at [rbp + frame_off + field_off], span = elem width (or 16 for
        // a slice field, 4/8 for float).
        const Type* mt = in.meta.type;
        int64_t addr = int64_t(in.meta.frame_off) + int64_t(in.meta.field_off);
        int64_t sp;
        if (is_pair_type(mt)) sp = 16;
        else if (mt && mt->is_float()) sp = (in.meta.is_f32 != 0) ? 4 : 8;
        else sp = (in.meta.width > 0 && in.meta.width <= 8) ? in.meta.width : 8;
        if (addr != 0) {
            d.flags |= flag(ThinEffectFlag::WritesFrame);
            d.writes.push_back(frame_iv(addr, uint64_t(sp)));
        }
        return d;
    }
    case ThinOp::StringDecrypt: {
        // Reads encrypted rodata [addend, addend+len); writes the decrypted-
        // data temp buffer at data_temp_off (span len); writes the slice
        // result slot at frame_off (span 16).
        int64_t len = int64_t(in.meta.len);
        if (len < 0) len = 0;
        d.flags |= flag(ThinEffectFlag::ReadsGlobal);  // rodata via a rodata-base reloc
        d.reads.push_back(rodata_iv(int64_t(in.meta.addend), uint64_t(len)));
        d.flags |= flag(ThinEffectFlag::WritesTemp);
        int64_t data_off = (in.meta.data_temp_off != 0)
                           ? int64_t(in.meta.data_temp_off) : int64_t(in.meta.frame_off);
        if (data_off != 0) {
            d.flags |= flag(ThinEffectFlag::WritesFrame);
            d.writes.push_back(frame_iv(data_off, uint64_t(len)));
        }
        if (in.meta.data_temp_off != 0 && in.meta.frame_off != 0) {
            d.flags |= flag(ThinEffectFlag::WritesFrame);
            d.writes.push_back(frame_iv(int64_t(in.meta.frame_off), 16));
        }
        return d;
    }
    case ThinOp::ConstStringRef: {
        // Produces a slice {ptr, len} from rodata. Reads rodata [addend,
        // addend+len); result spilled to frame_off (16-byte implicit write).
        int64_t len = int64_t(in.meta.len);
        if (len < 0) len = 0;
        d.flags |= flag(ThinEffectFlag::ReadsGlobal);
        d.reads.push_back(rodata_iv(int64_t(in.meta.addend), uint64_t(len)));
        if (in.dst != 0 && in.meta.frame_off != 0) {
            d.flags |= flag(ThinEffectFlag::WritesFrame);
            d.flags |= flag(ThinEffectFlag::ImplicitSpillWrite);
            d.writes.push_back(frame_iv(int64_t(in.meta.frame_off), 16));
        }
        return d;
    }
    default:
        // Any future op: conservatively effectful + unknown alias.
        d.flags |= flag(ThinEffectFlag::WritesIndirect);
        d.aliases_unknown_memory = true;
        return d;
    }
}

bool removable_if_result_dead(const ThinInstr& in, const ThinEffectDescriptor& desc) {
    // No dst -> never removable by a dead-RESULT rule (StoreFrame, guards,
    // CopyBytes with dst==0 are handled by dead-store / barrier analyses).
    if (in.dst == 0) return false;
    // Any side effect -> not removable.
    if (desc.flags.any()) return false;
    if (desc.aliases_unknown_memory) return false;
    if (!desc.reads.empty() || !desc.writes.empty()) return false;
    // Conservative cross-check: a handful of ops are never removable even
    // with a dst and no recorded effect (they have implicit behavior the
    // descriptor would have caught, but double-check so a future classifier
    // gap cannot silently authorize a removal).
    switch (in.op) {
    case ThinOp::StoreFrame:
    case ThinOp::StoreGlobal:
    case ThinOp::StoreAddr:
    case ThinOp::CopyBytes:
    case ThinOp::StructLitInit:
    case ThinOp::ArrayLitInit:
    case ThinOp::StringDecrypt:
    case ThinOp::BoundsCheck:
    case ThinOp::DivOverflowCheck:
    case ThinOp::DepthCheck:
    case ThinOp::BudgetCheck:
    case ThinOp::CallTargetGuard:
    case ThinOp::Div:
    case ThinOp::Mod:
    case ThinOp::FMod:
    case ThinOp::CallNative:
    case ThinOp::CallScript:
    case ThinOp::CallIndirect:
    case ThinOp::CallCrossModule:
    case ThinOp::LoadFrame:
    case ThinOp::LoadGlobal:
    case ThinOp::FieldAddr:
    case ThinOp::IndexAddr:
    case ThinOp::MakeSlice:
    case ThinOp::ConstStringRef:
        return false;
    default:
        break;
    }
    // Pure producing op with no spill and no memory effect: removable.
    return true;
}

} // namespace ember
