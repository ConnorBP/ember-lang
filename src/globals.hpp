// ember global initializer evaluation (the load-time gap the v1.0 integration
// found). ember does NOT run script code at load — so a global's `= <expr>`
// initializer is parsed + sema'd but, before this helper, never evaluated into
// the globals block (the block was zero-initialized; a host or @entry had to
// seed globals). This helper evaluates CONST initializers (literals + constexpr-
// foldable exprs: 10, 2.5f, 1+2, -x where x is a literal, etc.) at load and
// writes them into the globals block, so `global g : f32 = 10.0f;` starts at 10.
//
// Non-const initializers (global g = some_fn(); or g = other_global;) are NOT
// evaluable at load (the JIT'd fns don't exist yet, and other globals' values
// aren't known) — those stay zero-initialized (the host or @entry seeds them,
// same as before). Only what try_eval_const_* folds is baked here.
//
// Chunk c3: aggregate globals (struct / fixed-array / slice) are folded here
// too. A struct global's StructLit initializer is const-folded field-by-field
// into the typed block at the global's per-global OFFSET (each field written at
// offset + field_offset, where field_offset comes from the StructLayoutTable);
// non-const field initializers stay zero (same rule as a scalar non-const init).
// A fixed-array global's ArrayLit initializer is const-folded element-by-
// element (each at offset + i*elem_size). A slice global's ArrayLit initializer
// folds its elements into a BACKING region (at the slice's recorded backing
// offset within the block); the slice's 16-byte {ptr,len} slot at its primary
// offset is seeded with {ptr = backing_offset (RELATIVE), len = count} — the
// ptr is a 0-based offset within the block so the baked bytes round-trip
// through .em without loader fixup (codegen's global-slice-load adds
// globals_base at runtime). A non-const element/field stays zero (left as-is).
//
// The host calls this AFTER sema + after sizing the globals block, BEFORE the
// first script call. Idempotent + safe to call even with no globals.
#pragma once
#include "ast.hpp"          // Program, GlobalDecl, Type, Prim, StructLit, ArrayLit
#include "sema.hpp"          // try_eval_const_i64/f32/bool, StructLayoutTable
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <unordered_map>

namespace ember {

// The globals block layout a host uses (mirrors codegen.hpp's GlobalsBlock,
// kept here as a standalone {index, offsets, sizes, backing, types, structs}
// set). `bytes` is the TYPED globals block (zero-initialized by the host);
// this helper writes folded initializer values into it by per-global OFFSET.
// `index` is the legacy flat slot index (kept for backward compat — a host
// that only seeds scalar globals can leave offsets/backing empty and the
// helper falls back to index*8, but typed offsets are required for aggregate
// globals to land at their non-8-byte slots).
struct GlobalInitCtx {
    std::vector<uint8_t>& bytes;                               // the typed globals block
    const std::unordered_map<std::string, uint32_t>& index;    // name -> legacy slot index
    const std::unordered_map<std::string, const Type*>& types; // name -> Type
    // c3 typed layout: per-global byte offset + size + (for slice globals) the
    // backing-region byte offset within the block. When offsets is empty the
    // helper falls back to index*8 (the legacy scalar-only layout).
    const std::unordered_map<std::string, uint32_t>* offsets = nullptr;
    const std::unordered_map<std::string, uint32_t>* sizes = nullptr;
    const std::unordered_map<std::string, uint32_t>* backing_offsets = nullptr; // slice globals only
    const StructLayoutTable* structs = nullptr;
    // Optional host hook to materialize a `string`-typed global's string-literal
    // initializer into an opaque i64 handle at load (the string extension owns
    // the handle store; this helper stays free of that dependency). Receives the
    // literal's raw bytes + length and returns a 1-based handle (0 = null).
    // When null, string-typed globals are left zero (the pre-fix behavior — no
    // host that doesn't wire this regresses).
    int64_t (*string_alloc_fn)(const char* bytes, int64_t len) = nullptr;
};

// Const-fold a single scalar field/element initializer into a byte buffer at
// `dst`. Writes 1/2/4/8 bytes per the type's width; leaves the slot UNCHANGED
// (zero from the host's init) when the expr is non-const (same rule as a
// scalar non-const global init — the JIT'd fn doesn't exist at load). Returns
// true iff a const value was folded + written.
inline bool fold_scalar_init(const Expr& init, const Type* ty, uint8_t* dst) {
    if (!ty) return false;
    if (ty->is_int()) {
        int64_t v;
        if (try_eval_const_i64(init, v)) {
            // write the type's width bytes (i8..i64), little-endian, so a u8
            // field of a struct folds into 1 byte not 8.
            size_t w = ty->byte_size();
            if (w == 0) w = 8;
            std::memcpy(dst, &v, w);
            return true;
        }
    } else if (ty->is_float()) {
        float v;
        if (try_eval_const_f32(init, v)) {
            size_t w = ty->byte_size();
            if (w == 0) w = (ty->prim == Prim::F64) ? 8 : 4;
            if (ty->prim == Prim::F64) {
                double dv = double(v);
                std::memcpy(dst, &dv, 8);
            } else {
                std::memcpy(dst, &v, 4);
            }
            return true;
        }
    } else if (ty->is_bool()) {
        bool v;
        if (try_eval_const_bool(init, v)) {
            int64_t iv = v ? 1 : 0;
            size_t w = ty->byte_size();
            if (w == 0) w = 1;
            std::memcpy(dst, &iv, w);
            return true;
        }
    }
    return false;
}

// Recursively const-fold an aggregate initializer (StructLit / ArrayLit) into
// `base + offset` for a typed global. Struct fields fold at offset +
// field_offset (from the StructLayoutTable); array elements fold at offset +
// i*elem_size. Non-const field/element initializers stay zero (the slot is
// already zero from the host's init — a non-const global init is not evaluable
// at load, the same rule as a scalar non-const init). Recurses into nested
// struct/array field initializers (a struct field that is itself a struct
// literal folds recursively; a struct field that is a fixed-array literal
// folds element-by-element).
inline void write_aggregate_init(const Expr& init, const Type* ty,
                                 uint8_t* base, size_t offset,
                                 const StructLayoutTable& structs) {
    if (!ty) return;
    if (auto* sl = dynamic_cast<const StructLit*>(&init)) {
        if (!structs.count(sl->type_name)) return;
        const StructLayout& layout = structs.at(sl->type_name);
        for (auto& kv : sl->fields) {
            auto fit = layout.fields.find(kv.first);
            if (fit == layout.fields.end()) continue;
            const Type* ft = fit->second.ty;
            size_t foff = offset + size_t(fit->second.offset);
            if (auto* nested_sl = dynamic_cast<const StructLit*>(kv.second.get())) {
                // nested struct literal field
                write_aggregate_init(*nested_sl, ft, base, foff, structs);
            } else if (auto* nested_al = dynamic_cast<const ArrayLit*>(kv.second.get())) {
                // nested array literal field (a fixed-array struct field)
                write_aggregate_init(*nested_al, ft, base, foff, structs);
            } else {
                fold_scalar_init(*kv.second, ft, base + foff);
            }
        }
    } else if (auto* al = dynamic_cast<const ArrayLit*>(&init)) {
        if (!ty->elem) return;
        const Type* elem_ty = ty->elem.get();
        size_t elem_sz = elem_ty->byte_size();
        if (elem_sz == 0) elem_sz = 8;
        for (size_t i = 0; i < al->elements.size(); ++i) {
            size_t eoff = offset + i * elem_sz;
            if (auto* nested_sl = dynamic_cast<const StructLit*>(al->elements[i].get())) {
                write_aggregate_init(*nested_sl, elem_ty, base, eoff, structs);
            } else if (auto* nested_al = dynamic_cast<const ArrayLit*>(al->elements[i].get())) {
                write_aggregate_init(*nested_al, elem_ty, base, eoff, structs);
            } else {
                fold_scalar_init(*al->elements[i], elem_ty, base + eoff);
            }
        }
    }
}

// Evaluate every global's const initializer and write it into `gic.bytes`.
// Returns the count of globals whose initializer was folded + baked. A global
// with no init, or a non-const init, is left as-is (zero from the host's init).
inline size_t eval_global_initializers(const Program& prog, GlobalInitCtx gic) {
    size_t baked = 0;
    for (const auto& g : prog.globals) {
        if (!g.init) continue;                       // no initializer -> leave zero
        // resolve the per-global byte offset. Typed offsets (c3) for aggregate
        // globals; fall back to the legacy flat index*8 when the host hasn't
        // wired offsets (scalar-only hosts keep working — offset == index*8).
        size_t offset = 0;
        bool have_typed = false;
        if (gic.offsets) {
            auto oit = gic.offsets->find(g.name);
            if (oit != gic.offsets->end()) { offset = oit->second; have_typed = true; }
        }
        if (!have_typed) {
            auto iit = gic.index.find(g.name);
            if (iit == gic.index.end()) continue;    // host didn't register this global
            offset = size_t(iit->second) * 8;
        }
        uint8_t* slot = gic.bytes.data() + offset;

        const Type* t = nullptr;
        auto tit = gic.types.find(g.name);
        if (tit != gic.types.end()) t = tit->second;

        if (t && (t->is_int())) {
            int64_t v;
            if (try_eval_const_i64(*g.init, v)) {
                size_t w = t->byte_size(); if (w == 0) w = 8;
                std::memcpy(slot, &v, w);
                ++baked;
            }
        } else if (t && t->is_float()) {
            float v;
            if (try_eval_const_f32(*g.init, v)) {
                if (t->prim == Prim::F64) {
                    double dv = double(v); std::memcpy(slot, &dv, 8);
                } else {
                    std::memcpy(slot, &v, 4);
                }
                ++baked;
            }
        } else if (t && t->is_bool()) {
            bool v;
            if (try_eval_const_bool(*g.init, v)) {
                int64_t iv = v ? 1 : 0;
                size_t w = t->byte_size(); if (w == 0) w = 1;
                std::memcpy(slot, &iv, w);
                ++baked;
            }
        } else if (t && t->prim == Prim::I64 && t->struct_name == "string" && gic.string_alloc_fn) {
            // A `string`-typed global (opaque i64 handle into the string
            // extension's host store). Only a bare string-literal initializer is
            // materializable at load — the literal's bytes go straight to the
            // host hook, which returns the handle; we bake that handle (8 bytes)
            // into the global slot so the first script read sees a live string.
            // (Globals with non-literal string initializers, e.g. `global g =
            //  make_str();`, stay zero — the JIT'd fn doesn't exist at load, same
            // as the int/float/bool non-const case above.)
            if (auto* lit = dynamic_cast<const StringLit*>(g.init.get())) {
                int64_t h = gic.string_alloc_fn(lit->s.c_str(), int64_t(lit->s.size()));
                std::memcpy(slot, &h, 8);
                ++baked;
            }
        } else if (t && (t->is_slice || t->array_len > 0 ||
                         (gic.structs && !t->struct_name.empty() && gic.structs->count(t->struct_name)))) {
            // Aggregate global initializer (chunk c3): fold a StructLit /
            // ArrayLit into the typed block. A struct global folds its fields
            // at offset + field_offset; a fixed-array global folds its elements
            // at offset + i*elem_size. A slice global folds its elements into
            // a BACKING region (at the slice's recorded backing offset) and
            // seeds its 16-byte {ptr,len} slot with {ptr = backing_offset
            // (RELATIVE within the block), len = count} — codegen's global-
            // slice-load adds globals_base at runtime, so the baked bytes
            // round-trip through .em without loader fixup. A non-const field/
            // element initializer stays zero (same rule as a scalar non-const).
            if (gic.structs) {
                if (t->is_slice && gic.backing_offsets) {
                    // slice global: fold the backing elements into the backing
                    // region, then seed {ptr = backing_offset (RELATIVE), len}.
                    auto bit = gic.backing_offsets->find(g.name);
                    if (bit != gic.backing_offsets->end()) {
                        size_t back_off = bit->second;
                        if (auto* al = dynamic_cast<const ArrayLit*>(g.init.get())) {
                            write_aggregate_init(*al, t, gic.bytes.data(), back_off, *gic.structs);
                            // seed the {ptr,len} slot: ptr = backing_off (relative),
                            // len = element count.
                            uint64_t rel_ptr = back_off;
                            uint64_t len = uint64_t(al->elements.size());
                            std::memcpy(slot, &rel_ptr, 8);
                            std::memcpy(slot + 8, &len, 8);
                            ++baked;
                        }
                    }
                } else {
                    // struct or fixed-array global: fold directly into the
                    // slot at its offset.
                    write_aggregate_init(*g.init, t, gic.bytes.data(), offset, *gic.structs);
                    ++baked;
                }
            }
        }
        // else: an aggregate global without a structs table, or an unknown
        // shape — left zero (the host owns that state via natives today).
    }
    return baked;
}

} // namespace ember
