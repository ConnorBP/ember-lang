// thin_ir.cpp — Stage A c1: the debug pretty-printer for the thin IR.
//
// dump(const ThinFunction&) renders the IR as human-readable text for tests
// and debug builds. It is NOT part of the serialization path (Stage B has its
// own binary encoder) and exercises NO lowering/emit logic — it only reads the
// data structures pinned in thin_ir.hpp.
//
// DEPENDENCY NOTE: this translation unit is linked into the `ember` core
// library (see CMakeLists.txt), the same library em_writer.cpp / em_loader.cpp
// live in. Like those, it touches `Type` ONLY through public data members
// (prim, struct_name, is_slice, array_len, is_fn_handle) and the `Prim` enum —
// it NEVER calls a Type method (to_string / is_int / byte_size / ...) because
// those are defined in types.cpp (ember_frontend), and ember must not depend
// on ember_frontend (the link direction is ember_frontend -> ember, one way).
// A local prim_name() helper maps Prim -> string so the dump is readable
// without pulling in the frontend. The struct test links only `ember` and so
// reaches this dump() symbol directly.

#include "thin_ir.hpp"

#include <string>

namespace ember {

namespace {

const char* op_name(ThinOp op) {
    switch (op) {
    case ThinOp::ConstInt:        return "ConstInt";
    case ThinOp::ConstFloat:      return "ConstFloat";
    case ThinOp::ConstBool:       return "ConstBool";
    case ThinOp::ConstStringRef:  return "ConstStringRef";
    case ThinOp::Move:            return "Move";
    case ThinOp::LoadFrame:       return "LoadFrame";
    case ThinOp::StoreFrame:      return "StoreFrame";
    case ThinOp::LoadGlobal:      return "LoadGlobal";
    case ThinOp::StoreGlobal:     return "StoreGlobal";
    case ThinOp::CopyBytes:       return "CopyBytes";
    case ThinOp::Add:             return "Add";
    case ThinOp::Sub:             return "Sub";
    case ThinOp::Mul:             return "Mul";
    case ThinOp::Div:             return "Div";
    case ThinOp::Mod:             return "Mod";
    case ThinOp::And:             return "And";
    case ThinOp::Or:              return "Or";
    case ThinOp::Xor:             return "Xor";
    case ThinOp::Shl:             return "Shl";
    case ThinOp::Shr:             return "Shr";
    case ThinOp::Neg:             return "Neg";
    case ThinOp::Not:             return "Not";
    case ThinOp::BitNot:          return "BitNot";
    case ThinOp::FAdd:            return "FAdd";
    case ThinOp::FSub:            return "FSub";
    case ThinOp::FMul:            return "FMul";
    case ThinOp::FDiv:            return "FDiv";
    case ThinOp::FMod:            return "FMod";
    case ThinOp::Cmp:             return "Cmp";
    case ThinOp::LAnd:            return "LAnd";
    case ThinOp::LOr:             return "LOr";
    case ThinOp::Cast:            return "Cast";
    case ThinOp::CallNative:      return "CallNative";
    case ThinOp::CallScript:      return "CallScript";
    case ThinOp::CallIndirect:    return "CallIndirect";
    case ThinOp::CallCrossModule: return "CallCrossModule";
    case ThinOp::FieldAddr:       return "FieldAddr";
    case ThinOp::IndexAddr:       return "IndexAddr";
    case ThinOp::BoundsCheck:     return "BoundsCheck";
    case ThinOp::DivOverflowCheck:return "DivOverflowCheck";
    case ThinOp::MakeSlice:       return "MakeSlice";
    case ThinOp::StructLitInit:   return "StructLitInit";
    case ThinOp::ArrayLitInit:    return "ArrayLitInit";
    case ThinOp::StringDecrypt:   return "StringDecrypt";
    case ThinOp::DepthCheck:      return "DepthCheck";
    case ThinOp::BudgetCheck:     return "BudgetCheck";
    case ThinOp::CallTargetGuard: return "CallTargetGuard";
    case ThinOp::StoreAddr:       return "StoreAddr";
    case ThinOp::TryCatch:        return "TryCatch";
    case ThinOp::CatchCleanup:    return "CatchCleanup";
    case ThinOp::CatchEntry:      return "CatchEntry";
    case ThinOp::Throw:           return "Throw";
    }
    return "?ThinOp";
}

const char* term_name(TermKind k) {
    switch (k) {
    case TermKind::None:    return "None";
    case TermKind::Jmp:     return "Jmp";
    case TermKind::Branch:  return "Branch";
    case TermKind::Return:  return "Return";
    case TermKind::Trap:    return "Trap";
    }
    return "?Term";
}

const char* base_kind_name(AbsFixup::Kind k) {
    switch (k) {
    case AbsFixup::DispatchTableBase:  return "DispatchTableBase";
    case AbsFixup::GlobalsBase:        return "GlobalsBase";
    case AbsFixup::ModuleRegistryBase: return "ModuleRegistryBase";
    case AbsFixup::FunctionRodataBase: return "FunctionRodataBase";
    }
    return "?BaseKind";
}

// Prim -> readable tag, using only the header enum (no Type::to_string() call,
// which lives in ember_frontend). struct/slice/array types add a suffix.
std::string type_tag(const Type* t) {
    if (!t) return "<null>";
    std::string s;
    switch (t->prim) {
    case Prim::Void: s = "void"; break;
    case Prim::Bool: s = "bool"; break;
    case Prim::I8:   s = "i8";  break;
    case Prim::I16:  s = "i16"; break;
    case Prim::I32:  s = "i32"; break;
    case Prim::I64:  s = "i64"; break;
    case Prim::U8:   s = "u8";  break;
    case Prim::U16:  s = "u16"; break;
    case Prim::U32:  s = "u32"; break;
    case Prim::U64:  s = "u64"; break;
    case Prim::F32:  s = "f32"; break;
    case Prim::F64:  s = "f64"; break;
    default:         s = "prim?"; break;
    }
    if (!t->struct_name.empty())  s = t->struct_name;
    if (t->is_fn_handle)          s += ":fnhandle";
    if (t->is_slice)              s += "[]";
    else if (t->array_len > 0)    s += "[" + std::to_string(t->array_len) + "]";
    return s;
}

const char* cmp_pred_name(uint8_t cmp) {
    // 0=Eq..5=Ge matching BinExpr::Op order (Eq,Neq,Lt,Le,Gt,Ge).
    static const char* names[6] = {"Eq", "Neq", "Lt", "Le", "Gt", "Ge"};
    if (cmp < 6) return names[cmp];
    return "Cmp?";
}

void append_vreg(std::string& out, VReg v) {
    if (v == 0) { out += "v0"; return; }   // 0 = invalid/none (printed, not hidden)
    out += "v";
    out += std::to_string(v);
}

} // namespace

std::string dump(const ThinFunction& f) {
    std::string out;
    out.reserve(256);
    out += "ThinFunction \"";
    out += f.name;
    out += "\"";
    if (f.non_serializable) {
        out += " [non_serializable";
        if (!f.non_serializable_reason.empty()) {
            out += ": ";
            out += f.non_serializable_reason;
        }
        out += "]";
    }
    out += "\n  ret: ";
    out += type_tag(f.ret_type);
    out += "  slot: ";
    out += std::to_string(f.slot);
    out += "\n  frame: size=";
    out += std::to_string(f.frame.frame_size);
    out += " rbx_save=";
    out += std::to_string(f.frame.rbx_save_offset);
    out += " struct_ret_ptr=";
    out += std::to_string(f.frame.struct_ret_ptr_offset);
    out += " arg_temps_base=";
    out += std::to_string(f.frame.arg_temps_base);
    out += " next_local_off=";
    out += std::to_string(f.frame.next_local_off);
    out += f.frame.returns_struct_by_ptr ? " ret_by_ptr" : "";
    out += " params=";
    out += std::to_string(f.frame.params.size());
    out += "\n";
    for (const auto& p : f.frame.params) {
        out += "    param \"";
        out += p.name;
        out += "\" : ";
        out += type_tag(p.ty);
        out += " @ off=";
        out += std::to_string(p.off);
        out += " word0=";
        out += std::to_string(p.word0);
        out += " nwords=";
        out += std::to_string(p.nwords);
        out += "\n";
    }
    if (!f.frame.native_fixup_names.empty()) {
        out += "  native_fixup_names:";
        for (const auto& n : f.frame.native_fixup_names) {
            out += " \"";
            out += n;
            out += "\"";
        }
        out += "\n";
    }
    out += "  blocks: ";
    out += std::to_string(f.blocks.size());
    out += "\n";
    for (const auto& blk : f.blocks) {
        out += "  bb";
        out += std::to_string(blk.id);
        out += ":\n";
        for (const auto& in : blk.instrs) {
            out += "    ";
            out += op_name(in.op);
            out += "  dst=";
            append_vreg(out, in.dst);
            out += " src1=";
            append_vreg(out, in.src1);
            out += " src2=";
            append_vreg(out, in.src2);
            // immediate (only printed when it looks set; harmless to always show)
            out += " imm(i=";
            out += std::to_string(in.imm.i);
            out += ",f=";
            out += std::to_string(in.imm.f);
            out += ")";
            // meta (compact one-liner of the fields that matter for this op)
            out += " meta{off=";
            out += std::to_string(in.meta.frame_off);
            out += ",w=";
            out += std::to_string(in.meta.width);
            out += ",len=";
            out += std::to_string(in.meta.len);
            out += ",slot=";
            out += std::to_string(in.meta.slot);
            out += ",mod=";
            out += std::to_string(in.meta.mod_id);
            out += ",foff=";
            out += std::to_string(in.meta.field_off);
            out += ",base=";
            out += base_kind_name(in.meta.base_kind);
            out += ",addend=";
            out += std::to_string(in.meta.addend);
            if (!in.meta.native_name.empty()) {
                out += ",native=\"";
                out += in.meta.native_name;
                out += "\"";
            }
            out += ",type=";
            out += type_tag(in.meta.type);
            out += ",cmp=";
            out += cmp_pred_name(in.meta.cmp);
            if (in.meta.is_unsigned) out += ",u";
            if (in.meta.is_f32)      out += ",f32";
            if (in.meta.trap_reason) {
                out += ",trap=";
                out += std::to_string(in.meta.trap_reason);
            }
            out += "}";
            if (!in.args.empty()) {
                out += " args=[";
                for (size_t i = 0; i < in.args.size(); ++i) {
                    if (i) out += ",";
                    // struct-by-value arg: vreg 0 sentinel + a frame offset
                    if (in.args[i] == 0 && i < in.arg_frame_offs.size() &&
                        in.arg_frame_offs[i] != -1) {
                        out += "frame@";
                        out += std::to_string(in.arg_frame_offs[i]);
                    } else {
                        append_vreg(out, in.args[i]);
                    }
                }
                out += "]";
            }
            if (in.ret_type) {
                out += " ret:";
                out += type_tag(in.ret_type);
            }
            if (in.is_tail_call) out += " [tail-call]";
            out += "  @line ";
            out += std::to_string(in.loc.line);
            out += ":";
            out += std::to_string(in.loc.col);
            out += "\n";
        }
        out += "    term: ";
        out += term_name(blk.term.kind);
        switch (blk.term.kind) {
        case TermKind::None: break;
        case TermKind::Jmp:
            out += " -> bb";
            out += std::to_string(blk.term.target);
            break;
        case TermKind::Branch:
            out += " cond=";
            append_vreg(out, blk.term.cond);
            out += " ? bb";
            out += std::to_string(blk.term.target);
            out += " : bb";
            out += std::to_string(blk.term.false_target);
            break;
        case TermKind::Return:
            out += " ret=";
            append_vreg(out, blk.term.ret);
            break;
        case TermKind::Trap:
            out += " reason=";
            out += std::to_string(blk.term.trap_reason);
            break;
        }
        out += "\n";
    }
    return out;
}

} // namespace ember
