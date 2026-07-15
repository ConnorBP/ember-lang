// module_abi_fingerprint.hpp - shared native ABI fingerprint for EMBM v1
// module images (self_hosted/MODULE_IMAGE_FORMAT.md §8).
//
// The self-hosted codegen (Phase 1b, codegen.ember) and the C++ host loader
// (extensions/call_raw/ext_call_raw.cpp, load_executable_module) MUST agree on
// (a) the Type -> 6-bit code table and (b) the u64 fingerprint packing. This
// header is the single source of truth for the C++ side; the .ember codegen
// mirrors the exact constants below (the codegen cannot #include C++ headers,
// so it hardcodes the same numbers — keep them stable).
//
// === TYPE -> 6-BIT CODE TABLE (mirror exactly in .ember) ===
//
//   0  void
//   1  i64
//   2  f64
//   3  f32
//   4  bool
//   5  string / slice (any T[] — the {ptr,len} pair; a string is slice<u8>)
//   6  ptr / opaque (managed pointer, fn handle, lambda closure, or an opaque
//      i64 host-handle with a struct_name tag, i.e. the bind_handle convention)
//   7  i32
//   8  u8
//   9  u16
//  10  u32
//  11  u64
//  12  i8
//  13  i16
//  14  aggregate (by-value named struct / fixed array T[N] / fallback)
//
// 15..63 are reserved (unused in v1; the codegen must not emit them).
//
// === FINGERPRINT PACKING (u64, little-endian, fields from LSB) ===
//
//   bits  0..3   param_count   0..15 (the FULL declared arity; saturated to 15)
//   bits  4..27  param types   6 bits each, up to 4 params
//                        (param i occupies bits 4+6i .. 9+6i; params beyond the
//                         4th are NOT encoded — only their count is, via
//                         param_count. Phase 1 codegen only emits <=4 register
//                         params; arity is still fully validated by param_count.)
//   bits 28..33  return type   6 bits
//   bits 34..37  permission    low 4 bits of NativeSig::permission
//                        (0=none, 1=FFI/PERM_FFI; matches binding_builder.hpp)
//   bits 38..63  reserved      MUST be zero (loader rejects a nonzero field)
//
// The loader computes abi_fingerprint(registered_sig) and compares it to the
// ABS64_NATIVE reloc's addend (the codegen-baked fingerprint). Mismatch (wrong
// arity / param type / return type / permission, or nonzero reserved bits) ->
// reject the whole image.
#pragma once
#include "ast.hpp"    // Type, Prim
#include "sema.hpp"   // NativeSig
#include <cstdint>

namespace ember {

// Named 6-bit type codes (the .ember codegen mirrors these exact values).
// Keep stable: changing a value is an ABI break between codegen and loader.
inline constexpr uint8_t ABI_VOID = 0;
inline constexpr uint8_t ABI_I64  = 1;
inline constexpr uint8_t ABI_F64  = 2;
inline constexpr uint8_t ABI_F32  = 3;
inline constexpr uint8_t ABI_BOOL = 4;
inline constexpr uint8_t ABI_SLICE = 5;   // string / slice<T>
inline constexpr uint8_t ABI_PTR  = 6;    // ptr / opaque
inline constexpr uint8_t ABI_I32  = 7;
inline constexpr uint8_t ABI_U8   = 8;
inline constexpr uint8_t ABI_U16  = 9;
inline constexpr uint8_t ABI_U32  = 10;
inline constexpr uint8_t ABI_U64  = 11;
inline constexpr uint8_t ABI_I8   = 12;
inline constexpr uint8_t ABI_I16  = 13;
inline constexpr uint8_t ABI_AGGREGATE = 14;  // struct / fixed array / fallback
// 15..63 reserved.

// Map an ember Type to its 6-bit ABI code. See the table above.
inline uint8_t type_code(const Type& t) {
    // Tagged shapes override the primitive: check them first.
    if (t.is_slice)     return ABI_SLICE;    // string / any slice {ptr,len}
    if (t.is_managed_ptr) return ABI_PTR;   // GC pointer (opaque one-word)
    if (t.is_fn_handle) return ABI_PTR;     // function handle (opaque i64)
    if (t.is_lambda)    return ABI_PTR;     // closure {slot,env} (opaque pair)
    if (!t.struct_name.empty()) {
        // bind_handle convention: an opaque i64 host handle tagged with a
        // struct_name (prim I64 + struct_name) -> ptr/opaque. A by-value named
        // struct (prim Void + struct_name, e.g. bind_struct / script struct) is
        // an aggregate.
        if (t.prim == Prim::I64) return ABI_PTR;
        return ABI_AGGREGATE;
    }
    if (t.array_len > 0) return ABI_AGGREGATE;  // fixed array T[N]
    switch (t.prim) {
    case Prim::Void: return ABI_VOID;
    case Prim::I64:  return ABI_I64;
    case Prim::F64:  return ABI_F64;
    case Prim::F32:  return ABI_F32;
    case Prim::Bool: return ABI_BOOL;
    case Prim::I32:  return ABI_I32;
    case Prim::U8:   return ABI_U8;
    case Prim::U16:  return ABI_U16;
    case Prim::U32:  return ABI_U32;
    case Prim::U64:  return ABI_U64;
    case Prim::I8:   return ABI_I8;
    case Prim::I16:  return ABI_I16;
    }
    return ABI_AGGREGATE;  // fallback (unknown shape -> aggregate)
}

// Compute the u64 ABI fingerprint for a registered NativeSig. The loader
// compares this to an ABS64_NATIVE reloc's addend (§8). param_count is
// saturated to 15 (4-bit field); only the first 4 param types are encoded
// (24 bits), but the full arity is captured by param_count so a wrong arity
// still mismatches. Reserved bits 38..63 are left zero.
inline uint64_t abi_fingerprint(const NativeSig& s) {
    uint64_t fp = 0;
    uint64_t pc = s.params.size();
    if (pc > 15) pc = 15;                 // param_count is a 4-bit field
    fp |= (pc & 0xFull);                  // bits 0..3
    for (uint64_t i = 0; i < pc && i < 4; ++i) {
        uint64_t tc = uint64_t(type_code(s.params[size_t(i)])) & 0x3Full;
        fp |= (tc << (4 + 6 * i));        // bits 4+6i .. 9+6i
    }
    uint64_t rc = uint64_t(type_code(s.ret)) & 0x3Full;
    fp |= (rc << 28);                     // bits 28..33
    uint64_t perm = uint64_t(s.permission) & 0xFull;
    fp |= (perm << 34);                   // bits 34..37
    // bits 38..63 reserved zero (already zero)
    return fp;
}

} // namespace ember
