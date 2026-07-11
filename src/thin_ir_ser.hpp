// thin_ir_ser.hpp — Stage B c1b: the ThinFunction serializer/deserializer.
//
// This is the IR-blob codec: it turns a ThinFunction (src/thin_ir.hpp) into a
// self-contained byte blob (the `ir_blob` a v5 .em per-function record carries)
// and back. The blob is OPAQUE to the .em container — em_writer.cpp writes it
// as raw bytes (ir_blob_len + ir_blob) and em_loader.cpp reads it as raw bytes,
// then calls deserialize_thin_function to interpret it. The .em layer never
// parses inside the blob.
//
// ─── SERIALIZATION BOUNDARY (the contract thin_ir.hpp pins) ───
//
// ThinOp is a STABLE uint16_t enum — serialized VERBATIM (never renumber;
// append new ops at the END only). The numeric IDs are the on-disk identity.
//
// The ONLY raw pointer fields in the IR are NOT serialized as pointers:
//   1. ThinInstr::native_fn  — DROPPED. The symbolic binding is carried by
//      ThinMeta::native_name; the loader rebinds native_fn at load time by
//      looking up native_name in the host native table (never by
//      reverse-mapping a ptr).
//   2. const Type* fields (ThinMeta::type, ThinInstr::ret_type,
//      ThinInstr::arg_types[], ThinFramePlan::ParamSpill::ty,
//      ThinFunction::ret_type) — encoded via em_type_codec's emit_type /
//      parse_type (the canonical Prim + struct_name + is_slice + array_len +
//      elem chain). Each is prefixed by a has_type:u8 (0 = nullptr, 1 = type
//      follows) so the null case is explicit. Deserialized Type objects live
//      in ThinFunction::owned_types so the const Type* pointers remain valid
//      for the ThinFunction's lifetime.
//
// abs_fixups is NOT serialized — it is populated by emit_x64 at load time
// (the re-emit produces the same AbsFixup list the tree-walker would). The IR
// is serialized BEFORE emit; abs_fixups is empty at serialization time.
//
// rodata IS serialized (function-local string-literal bytes).
//
// ─── ir_blob FORMAT (little-endian, self-contained, counts-up-front) ───
//
//   Header:
//     ir_magic     : u32 = 0x4952464E ("IRFN") — reject garbage immediately
//     ir_version   : u16 = 1                  — IR serialization format version
//     slot         : i32                      — dispatch slot
//     max_vreg     : u32                      — highest VReg+1; all VReg refs < this
//     num_blocks   : u16 (<= 65535)
//     has_ret_type : u8                       — 0=nullptr, 1=type follows
//     [ret_type    : canonical type via emit_type/parse_type if has_ret_type]
//
//   Frame plan:
//     frame_size, rbx_save_offset, struct_ret_ptr_offset,
//       arg_temps_base, next_local_off  : 5×i32
//     returns_struct_by_ptr : u8
//     num_params            : u16 (<= 1024)
//     per param:
//       name_len  : u16 (<= MAX_NAME_SIZE)
//       name      : bytes[name_len]
//       has_type  : u8, [type via emit_type/parse_type if has_type]
//       off       : i32
//       word0     : i32
//       nwords    : i32
//     num_native_fixup_names : u16 (<= 1024)
//     per name:
//       name_len  : u16 (<= MAX_NAME_SIZE)
//       name      : bytes[name_len]
//
//   Rodata:
//     rodata_len  : u32 (checked <= remaining blob)
//     rodata      : bytes[rodata_len]
//
//   Blocks (repeated num_blocks times):
//     block_id    : u32
//     num_instrs  : u16 (<= 65535)
//     per instr:
//       op        : u16 (validated against ThinOp enum range)
//       dst,src1,src2 : 3×u32 (< max_vreg or 0)
//       imm_i     : i64
//       imm_f     : f64 (raw 8 bytes)
//       meta:
//         frame_off, width, len, slot, mod_id, field_off : 6×i32
//         base_kind : u8 (validated against AbsFixup::Kind range)
//         addend    : u32
//         native_name_len : u16 (<= MAX_NAME_SIZE), native_name
//         has_type  : u8, [type via emit_type/parse_type if has_type]
//         cmp, is_unsigned, is_f32, trap_reason : 4×u8
//       num_args  : u8 (<= 255)
//       args      : u32[num_args]
//       num_arg_frame_offs : u8 (== num_args or 0)
//       arg_frame_offs     : i32[num_arg_frame_offs]
//       num_arg_types      : u8 (== num_args or 0)
//       arg_types          : per: has_type:u8, [type]
//       has_ret_type       : u8, [ret_type]
//       loc_line : u32, loc_col : u32
//     terminator:
//       term_kind     : u8 (validated against TermKind enum range)
//       cond          : u32 (< max_vreg or 0)
//       target        : u32 (< num_blocks or 0)
//       false_target  : u32 (< num_blocks or 0)
//       ret           : u32 (< max_vreg or 0)
//       trap_reason   : u8
//
// ─── SAFETY-BY-CONSTRUCTION ───
//
// Every count is checked against a hard maximum BEFORE the corresponding
// resize/reserve. All cursor arithmetic uses uint64_t for avail = end - cur
// and n > avail (no uint32_t addition that can wrap). The ir_magic check
// rejects garbage before any allocation. max_vreg in the header lets every
// VReg reference be checked in O(1) without a pre-scan. No exceptions escape
// the deserializer — it returns false + error on any structural failure.

#pragma once

#include "thin_ir.hpp"        // ThinFunction, ThinOp, ThinBlock, etc.
#include "em_type_codec.hpp"  // emit_type / parse_type (const Type* codec)

#include <cstdint>
#include <string>
#include <vector>

namespace ember {

// ir_blob magic + version.
constexpr uint32_t IR_BLOB_MAGIC    = 0x4952464Eu;  // "IRFN" (LE: 'N','F','R','I')
constexpr uint16_t IR_BLOB_VERSION  = 1u;

// Hard maxima (bounded counts — checked before any resize/reserve).
constexpr uint32_t IR_MAX_BLOCKS   = 65535u;
constexpr uint32_t IR_MAX_INSTRS   = 65535u;
constexpr uint32_t IR_MAX_ARGS     = 255u;
constexpr uint32_t IR_MAX_PARAMS   = 1024u;
constexpr uint32_t IR_MAX_NFIXUPS  = 1024u;
constexpr uint32_t IR_MAX_VREGS    = (1u << 24);  // 16M — generous; checked against blob size

// Serialize a ThinFunction into `out`. The function's name is NOT written
// (it's carried by the .em per-function record; the deserializer takes name as
// a parameter). Returns true on success; false + *err on failure (e.g. a count
// exceeds a hard maximum). Does NOT serialize abs_fixups (empty at serialize
// time — emit_x64 populates them at load time) or native_fn (rebound by name).
bool serialize_thin_function(const ThinFunction& thf, std::vector<uint8_t>& out,
                             std::string* err);

// Deserialize a ThinFunction from the byte range [cur, end). On success
// advances `cur` past the consumed bytes, fills `out`, and returns true. On
// failure leaves `cur` at an unspecified position, sets `*err`, and returns
// false — no partial `out` is published (the caller discards it on false).
//
// `name` and `slot` are supplied by the .em container (they live in the
// per-function record, not the blob). Reconstructed Type objects are pushed
// into `out.owned_types` so the const Type* pointers (ret_type, arg_types,
// meta.type, frame.params[].ty) remain valid for `out`'s lifetime.
//
// This function performs STRUCTURAL validation only (checks 1–10 from the
// Stage B design: magic, version, count bounds, cursor bounds, ThinOp range,
// TermKind range, base_kind range, width range, canonical-type shape via
// em_type_codec). SEMANTIC validation (VReg bounds, block-target bounds,
// native-name rebinding, rodata bounds, slot consistency) is the loader's
// responsibility — see validate_thin_function — because it needs the host
// native table.
bool deserialize_thin_function(const uint8_t*& cur, const uint8_t* end,
                               const std::string& name, int32_t slot,
                               ThinFunction& out, std::string* err);

// Semantic validation of a deserialized ThinFunction, performed by the loader
// AFTER deserialize_thin_function succeeds and BEFORE emit_x64 (so no
// executable page is allocated for a malformed/invalid IR function). Checks:
//   - VReg bounds: every dst/src1/src2/args[i]/term.cond/term.ret is 0 or
//     < max_vreg (computed from the function's VReg usage).
//   - Block-target bounds: every term.target/false_target < num_blocks.
//   - Block 0 is entry (blocks[0].id == 0).
//   - Every block has a terminator (term.kind != None).
//   - Terminator shape consistency (Jmp/Branch/Return/Trap fields).
//   - Rodata bounds: every ConstStringRef addend+len <= rodata.size().
//   - ThinOp ordinal in range (re-checked; the deserializer already checks).
//
// `max_vreg` is an out-parameter: the loader passes it to emit_x64 callers
// if needed; it's the highest VReg+1 seen across the function.
//
// NOTE: native-name rebinding (looking up meta.native_name in the host table
// and setting native_fn) is a SEPARATE step the loader performs between
// deserialize and validate, because it needs the host natives table and
// produces a reject on unknown names (the core v5 security gate).
bool validate_thin_function(const ThinFunction& thf, std::string* err);

} // namespace ember
