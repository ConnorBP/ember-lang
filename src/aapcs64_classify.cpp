// src/aapcs64_classify.cpp — AAPCS64 argument/return classification impl.
// plan_MACOS_ARM64.md Phase 6c. See aapcs64_classify.hpp for the staging + rules.
#include "aapcs64_classify.hpp"
#include <stdexcept>

namespace ember {

namespace {

int32_t value_bytes_local(const Type* t, const StructLayoutTable* structs) {
    if (!t) return 8;
    if (t->is_slice || t->is_lambda) return 16;
    if (t->array_len > 0) return int32_t(t->array_len) * value_bytes_local(t->elem.get(), structs);
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

// Walk a composite's leaf members (recursing into nested structs/arrays) to
// test HFA-ness: ALL leaves must be the SAME f32 (or the same f64), count 1..4.
struct HfaProbe {
    bool pure = true;
    bool is_f32 = true;
    uint8_t count = 0;
    bool ok = false;  // true if at least one float seen + all same + 1..4
    void visit(const Type* t, const StructLayoutTable* structs) {
        if (!t || !pure) return;
        if (t->is_float()) {
            bool f32 = (t->prim == Prim::F32);
            if (count == 0) { is_f32 = f32; }
            else if (f32 != is_f32) { pure = false; return; }
            count++;
            return;
        }
        if (t->array_len > 0) {
            for (uint32_t i = 0; i < t->array_len; ++i) visit(t->elem.get(), structs);
            return;
        }
        if (!t->struct_name.empty() && structs) {
            auto it = structs->find(t->struct_name);
            if (it != structs->end()) {
                for (const auto& fn : it->second.field_names) {
                    auto fit = it->second.fields.find(fn);
                    if (fit != it->second.fields.end()) visit(fit->second.ty, structs);
                }
                return;
            }
        }
        // any non-float leaf (int/bool/ptr/slice) disqualifies HFA
        pure = false;
    }
    void finish() {
        ok = pure && count >= 1 && count <= 4;
    }
};

} // namespace

bool is_hfa(const Type* ty, const StructLayoutTable* structs,
            uint8_t& member_count, bool& is_f32) {
    if (!ty) return false;
    // HFA only applies to composites (struct or fixed array) of floats.
    if (ty->is_float()) { member_count = 1; is_f32 = (ty->prim == Prim::F32); return false; }
    if (ty->struct_name.empty() && ty->array_len == 0) return false;
    HfaProbe p;
    p.visit(ty, structs);
    p.finish();
    if (p.ok) { member_count = p.count; is_f32 = p.is_f32; return true; }
    return false;
}

Aapcs64ArgClass classify_aapcs64_arg(const Type* ty, const StructLayoutTable* structs,
                                     uint8_t gp_used, uint8_t fp_used) {
    Aapcs64ArgClass c;
    c.type = ty;
    c.byte_size = value_bytes_local(ty, structs);
    if (!ty || ty->is_void()) {
        return c;  // void arg: no slots
    }
    // scalar float -> 1 FP reg
    if (ty->is_float()) {
        if (fp_used > 7) throw std::runtime_error("aapcs64: float arg beyond v0-v7 (stack args not supported)");
        Aapcs64Slot s; s.kind = Aapcs64Slot::Kind::FpReg; s.reg_index = fp_used;
        s.is_f32 = (ty->prim == Prim::F32); s.width_bytes = s.is_f32 ? 4 : 8;
        c.slots.push_back(s);
        return c;
    }
    // slice / lambda -> 2 GP words (ptr, len)
    if (ty->is_slice || ty->is_lambda) {
        if (gp_used + 2 > 8) throw std::runtime_error(
            // slice/lambda needs two consecutive GP regs (x0-x7); a +1>8
            // check let gp_used==7 slip through and emit reg_index=8 (OOB).
            "aapcs64: slice arg beyond x0-x7 (stack args not supported)");
        Aapcs64Slot p; p.kind = Aapcs64Slot::Kind::GpReg; p.reg_index = gp_used;     p.width_bytes = 8;
        Aapcs64Slot l; l.kind = Aapcs64Slot::Kind::GpReg; l.reg_index = gp_used + 1; l.width_bytes = 8;
        c.slots.push_back(p); c.slots.push_back(l);
        return c;
    }
    // composite (struct or fixed array)
    if (!ty->struct_name.empty() || ty->array_len > 0) {
        if (c.byte_size > 16) {
            // indirect: the caller allocates + passes a pointer as the next GP arg.
            if (gp_used > 7) throw std::runtime_error("aapcs64: indirect arg ptr beyond x0-x7");
            c.indirect = true;
            Aapcs64Slot ptr; ptr.kind = Aapcs64Slot::Kind::GpReg; ptr.reg_index = gp_used; ptr.width_bytes = 8;
            c.slots.push_back(ptr);  // the pointer slot (the caller copies the bytes through it)
            return c;
        }
        // HFA? (1..4 identical floats) -> FP regs
        uint8_t mc = 0; bool f32 = false;
        if (is_hfa(ty, structs, mc, f32)) {
            if (fp_used + mc > 8) throw std::runtime_error("aapcs64: HFA arg beyond v0-v7");
            c.hfa_count = mc; c.is_f32_hfa = f32;
            for (uint8_t i = 0; i < mc; ++i) {
                Aapcs64Slot s; s.kind = Aapcs64Slot::Kind::FpReg; s.reg_index = fp_used + i;
                s.is_f32 = f32; s.width_bytes = f32 ? 4 : 8;
                c.slots.push_back(s);
            }
            return c;
        }
        // non-HFA composite <=16B -> GP words (ceil(size/8), max 2)
        int32_t words = (c.byte_size + 7) / 8;
        if (words < 1) words = 1;
        if (words > 2) throw std::runtime_error("aapcs64: composite >16B but not indirect (internal)");
        if (gp_used + words > 8) throw std::runtime_error("aapcs64: composite arg beyond x0-x7 (stack args not supported)");
        for (int32_t i = 0; i < words; ++i) {
            Aapcs64Slot s; s.kind = Aapcs64Slot::Kind::GpReg; s.reg_index = gp_used + i;
            s.width_bytes = (i + 1 == words && (c.byte_size % 8) != 0) ? (c.byte_size % 8) : 8;
            c.slots.push_back(s);
        }
        return c;
    }
    // scalar int/bool/ptr/handle -> 1 GP reg
    if (gp_used > 7) throw std::runtime_error("aapcs64: int arg beyond x0-x7 (stack args not supported)");
    {
        Aapcs64Slot s; s.kind = Aapcs64Slot::Kind::GpReg; s.reg_index = gp_used;
        switch (ty->prim) {
        case Prim::Bool: case Prim::I8: case Prim::U8: s.width_bytes = 1; break;
        case Prim::I16: case Prim::U16: s.width_bytes = 2; break;
        case Prim::I32: case Prim::U32: s.width_bytes = 4; break;
        default: s.width_bytes = 8; break;
        }
        c.slots.push_back(s);
    }
    return c;
}

Aapcs64ArgClass classify_aapcs64_return(const Type* ty, const StructLayoutTable* structs) {
    Aapcs64ArgClass c;
    c.type = ty;
    c.byte_size = value_bytes_local(ty, structs);
    if (!ty || ty->is_void()) return c;
    if (ty->is_float()) {
        Aapcs64Slot s; s.kind = Aapcs64Slot::Kind::FpReg; s.reg_index = 0;
        s.is_f32 = (ty->prim == Prim::F32); s.width_bytes = s.is_f32 ? 4 : 8;
        c.slots.push_back(s);
        return c;
    }
    if (ty->is_slice || ty->is_lambda) {
        // {ptr,len} returned in x0/x1
        Aapcs64Slot p; p.kind = Aapcs64Slot::Kind::GpReg; p.reg_index = 0; p.width_bytes = 8;
        Aapcs64Slot l; l.kind = Aapcs64Slot::Kind::GpReg; l.reg_index = 1; l.width_bytes = 8;
        c.slots.push_back(p); c.slots.push_back(l);
        return c;
    }
    if (!ty->struct_name.empty() || ty->array_len > 0) {
        if (c.byte_size > 16) {
            // indirect return: dest ptr in x8 (the caller allocates). No return
            // slot in x0; the function writes through x8.
            c.indirect = true;
            return c;
        }
        uint8_t mc = 0; bool f32 = false;
        if (is_hfa(ty, structs, mc, f32)) {
            c.hfa_count = mc; c.is_f32_hfa = f32;
            for (uint8_t i = 0; i < mc; ++i) {
                Aapcs64Slot s; s.kind = Aapcs64Slot::Kind::FpReg; s.reg_index = i;
                s.is_f32 = f32; s.width_bytes = f32 ? 4 : 8;
                c.slots.push_back(s);
            }
            return c;
        }
        int32_t words = (c.byte_size + 7) / 8;
        if (words < 1) words = 1;
        if (words > 2) { c.indirect = true; return c; }  // defensive
        for (int32_t i = 0; i < words; ++i) {
            Aapcs64Slot s; s.kind = Aapcs64Slot::Kind::GpReg; s.reg_index = i;
            s.width_bytes = (i + 1 == words && (c.byte_size % 8) != 0) ? (c.byte_size % 8) : 8;
            c.slots.push_back(s);
        }
        return c;
    }
    // scalar int/bool/ptr -> x0
    Aapcs64Slot s; s.kind = Aapcs64Slot::Kind::GpReg; s.reg_index = 0;
    switch (ty->prim) {
    case Prim::Bool: case Prim::I8: case Prim::U8: s.width_bytes = 1; break;
    case Prim::I16: case Prim::U16: s.width_bytes = 2; break;
    case Prim::I32: case Prim::U32: s.width_bytes = 4; break;
    default: s.width_bytes = 8; break;
    }
    c.slots.push_back(s);
    return c;
}

} // namespace ember
