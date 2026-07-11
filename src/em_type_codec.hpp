// ember `.em` canonical-type codec — the SINGLE shared implementation of the
// on-disk Type/EmSignature encoding.
//
// This header is the one place that knows how a `Type` (ast.hpp) and an
// `EmSignature` (em_file.hpp) are serialized to / parsed from the `.em` binary
// format. Previously this encoding was DUPLICATED: the writer
// (em_writer.cpp) carried emit_type / emit_signature / validate_canonical_type
// / validate_signature in its anonymous namespace, and the loader (em_loader.cpp)
// carried parse_type / canonical_type_same / parse_signature in its anonymous
// namespace. Both encoded the SAME shape (see the em_file.hpp format comment
// and ast.hpp struct Type); this codec deduplicates them so the writer, the
// loader, and the (later) IR serializer all share one implementation.
//
// On-disk canonical-type layout (little-endian, see em_file.hpp):
//   prim        : u8   (Prim enum value)
//   flags       : u8   (bit0=is_slice, bit1=array, bit2=struct,
//                       bit3=is_fn_handle, bit4=has_recorded_sig)
//   array_len   : u32  (0 = not a fixed array; >0 = T[N])
//   name_len    : u16  (struct_name length)
//   name        : bytes[name_len]
//   if (is_slice || array_len): recursive element type
//   if (is_fn_handle && has_recorded_sig):
//       param_count : u32  (<= 1024)
//       param_count recursive param types
//       one recursive return type
//
// Validation invariants enforced identically on both sides (the writer derives
// flag bits from Type fields; the loader enforces that on-disk flags agree with
// the accompanying fields):
//   - struct flag  iff struct_name nonempty (name_len > 0)
//   - array flag   iff array_len != 0
//   - slice (bit0) and array (bit1) mutually exclusive
//   - slice/array prim must be Void (the value is elem[] / elem[N])
//   - is_fn_handle requires Prim::I64
//   - has_recorded_sig requires is_fn_handle
//   - nesting depth <= 16
//   - recorded param count <= 1024
//
// Pure codec: no I/O, no allocation policy, no version routing. The writer and
// loader keep their own I/O / page-allocation / version logic and call into
// here for the type/signature encode+validate and decode+validate pieces.

#pragma once

#include "ast.hpp"      // Type, Prim
#include "em_file.hpp"   // EmSignature

#include <cstdint>
#include <ostream>
#include <string>

namespace ember {

// ---- writer side ----

// Serialize `t` to `ofs` in the canonical on-disk form. Byte order is
// little-endian and identical to the historical em_writer implementation.
void emit_type(std::ostream& ofs, const Type& t);

// Serialize `sig` (ret type + u32 param count + each param type).
void emit_signature(std::ostream& ofs, const EmSignature& sig);

// Validate that `t`'s fields are internally consistent with the flag bits the
// writer would derive from them, so a hand-built Type is rejected at write time
// rather than serialized for the loader to catch. `depth` is the current
// nesting depth (top-level callers pass 0). Returns false and sets `*err` on
// the first inconsistency. Byte-identical decisions to the historical
// em_writer validate_canonical_type.
bool validate_canonical_type(const Type& t, std::string* err, unsigned depth = 0);

// Validate a whole signature: param count <= 1024, then validate_canonical_type
// on ret and every param.
bool validate_signature(const EmSignature& sig, std::string* err);

// ---- reader side ----

// Parse a canonical type from the byte range [cur, end). On success advances
// `cur` past the consumed bytes, fills `out`, and returns true. On failure
// leaves `cur` at an unspecified position, sets `*err`, and returns false.
// `depth` is the current nesting depth (top-level callers pass 0; the limit is
// 16). Enforces every on-disk consistency invariant before assigning any `out`
// field or allocating an element. Byte-identical decisions to the historical
// em_loader parse_type.
bool parse_type(const uint8_t*& cur, const uint8_t* end, Type& out,
                unsigned depth, std::string* err);

// Structural equality of two canonical Types (the on-disk-signature comparison
// used by the loader's native-binding signature check). Compares every
// dimension that the codec serializes, recursing into elem / recorded_params /
// recorded_ret. Byte-identical decisions to the historical em_loader
// canonical_type_same.
bool canonical_type_same(const Type& a, const Type& b);

// Parse a signature (ret type + u32 param count <= 1024 + each param type)
// from [cur, end). Advances `cur` on success.
bool parse_signature(const uint8_t*& cur, const uint8_t* end, EmSignature& out,
                     std::string* err);

} // namespace ember
