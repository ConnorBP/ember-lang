// thin_ir_ser.cpp — Stage B c1b: the ThinFunction serializer/deserializer.
// See thin_ir_ser.hpp for the ir_blob format + the serialization boundary
// contract + the safety-by-construction principles.

#include "thin_ir_ser.hpp"

#include <cstring>   // memcpy for f64 raw bytes
#include <limits>
#include <sstream>   // put_type bridges emit_type(ostream&) -> bytes
#include <string>
#include <unordered_set>
#include <vector>

namespace ember {

namespace {

// ─── writer-side little-endian emitters (into a vector<uint8_t>) ───

void put_u8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
void put_u16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(static_cast<uint8_t>(v & 0xFFu));
    o.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}
void put_u32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v & 0xFFu));
    o.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    o.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    o.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}
void put_i32(std::vector<uint8_t>& o, int32_t v) { put_u32(o, static_cast<uint32_t>(v)); }
void put_u64(std::vector<uint8_t>& o, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        o.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}
void put_i64(std::vector<uint8_t>& o, int64_t v) { put_u64(o, static_cast<uint64_t>(v)); }
void put_f64(std::vector<uint8_t>& o, double v) {
    uint64_t raw;
    std::memcpy(&raw, &v, sizeof(raw));
    put_u64(o, raw);
}
void put_str(std::vector<uint8_t>& o, const std::string& s) {
    // Caller ensures s.size() <= MAX (checked at the call site).
    o.insert(o.end(), s.begin(), s.end());
}

// A sub-stream for the canonical-type encoding: emit_type writes to an
// ostream, so we bridge through a std::string-backed stringstream, then copy
// the bytes. (em_type_codec's emit_type takes std::ostream&.) This keeps the
// codec the single source of truth for the Type encoding.
void put_type(std::vector<uint8_t>& o, const Type* t) {
    // has_type byte (0 = nullptr, 1 = type follows) is written by the caller.
    if (!t) return;
    std::ostringstream ss;
    emit_type(ss, *t);
    std::string s = ss.str();
    o.insert(o.end(), s.begin(), s.end());
}

// ─── reader-side cursor helpers (match em_type_codec.cpp's shape) ───

bool take(const uint8_t*& cur, const uint8_t* end, uint64_t n,
          const uint8_t*& out) {
    uint64_t avail = static_cast<uint64_t>(end - cur);
    if (n > avail) return false;
    out = cur;
    cur += n;
    return true;
}
bool read_u8(const uint8_t*& cur, const uint8_t* end, uint8_t& v) {
    const uint8_t* p = nullptr;
    if (!take(cur, end, 1, p)) return false;
    v = p[0];
    return true;
}
bool read_u16(const uint8_t*& cur, const uint8_t* end, uint16_t& v) {
    const uint8_t* p = nullptr;
    if (!take(cur, end, 2, p)) return false;
    v = static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1]) << 8;
    return true;
}
bool read_u32(const uint8_t*& cur, const uint8_t* end, uint32_t& v) {
    const uint8_t* p = nullptr;
    if (!take(cur, end, 4, p)) return false;
    v = static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
        static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
    return true;
}
bool read_i32(const uint8_t*& cur, const uint8_t* end, int32_t& v) {
    uint32_t u = 0;
    if (!read_u32(cur, end, u)) return false;
    v = static_cast<int32_t>(u);
    return true;
}
bool read_u64(const uint8_t*& cur, const uint8_t* end, uint64_t& v) {
    const uint8_t* p = nullptr;
    if (!take(cur, end, 8, p)) return false;
    v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return true;
}
bool read_i64(const uint8_t*& cur, const uint8_t* end, int64_t& v) {
    uint64_t u = 0;
    if (!read_u64(cur, end, u)) return false;
    v = static_cast<int64_t>(u);
    return true;
}
bool read_f64(const uint8_t*& cur, const uint8_t* end, double& v) {
    uint64_t raw = 0;
    if (!read_u64(cur, end, raw)) return false;
    std::memcpy(&v, &raw, sizeof(v));
    return true;
}

// Read a name: u16 length (checked <= MAX_NAME_SIZE) + bytes. The cursor-based
// parse_type from em_type_codec reads the name internally; this is for the
// non-type names (param names, native_name, native_fixup_names).
bool read_name(const uint8_t*& cur, const uint8_t* end, std::string& out,
               uint32_t max_len, std::string* err, const char* field) {
    uint16_t len = 0;
    if (!read_u16(cur, end, len)) {
        if (err) *err = std::string("thin_ir_ser: truncated ") + field + " length";
        return false;
    }
    if (len > max_len) {
        if (err) *err = std::string("thin_ir_ser: limit: ") + field + " exceeds max length";
        return false;
    }
    const uint8_t* p = nullptr;
    if (!take(cur, end, len, p)) {
        if (err) *err = std::string("thin_ir_ser: truncated ") + field;
        return false;
    }
    out.assign(reinterpret_cast<const char*>(p), len);
    return true;
}

// Read a type: has_type:u8, then parse_type (via em_type_codec) if has_type.
// On success, pushes the reconstructed Type into `owned` and sets `*out_ptr`
// to point at it. Returns false on any parse failure (parse_type already
// validates the canonical-type shape + nesting depth).
bool read_type(const uint8_t*& cur, const uint8_t* end,
               std::vector<std::shared_ptr<Type>>& owned,
               const Type*& out_ptr, std::string* err, const char* field) {
    uint8_t has = 0;
    if (!read_u8(cur, end, has)) {
        if (err) *err = std::string("thin_ir_ser: truncated ") + field + " has_type";
        return false;
    }
    if (has == 0) { out_ptr = nullptr; return true; }
    if (has != 1) {
        if (err) *err = std::string("thin_ir_ser: invalid ") + field + " has_type (must be 0 or 1)";
        return false;
    }
    auto sp = std::make_shared<Type>();
    if (!parse_type(cur, end, *sp, 0, err)) {
        if (err) *err = std::string("thin_ir_ser: ") + *err;
        return false;
    }
    out_ptr = sp.get();
    owned.push_back(std::move(sp));
    return true;
}

// ─── enum range helpers ───

constexpr uint16_t THIN_OP_LAST = static_cast<uint16_t>(ThinOp::CallTargetGuard);
constexpr uint8_t  TERM_KIND_LAST = static_cast<uint8_t>(TermKind::Trap);
constexpr uint8_t  BASE_KIND_LAST = static_cast<uint8_t>(AbsFixup::FunctionRodataBase);

bool valid_width(int32_t w) {
    return w == 0 || w == 1 || w == 2 || w == 4 || w == 8;
}

// ─── serialize helpers ───

bool ser_name(std::vector<uint8_t>& o, const std::string& s,
              std::string* err, const char* field) {
    if (s.size() > MAX_NAME_SIZE) {
        if (err) *err = std::string("thin_ir_ser: limit: ") + field + " exceeds MAX_NAME_SIZE";
        return false;
    }
    put_u16(o, static_cast<uint16_t>(s.size()));
    put_str(o, s);
    return true;
}

void ser_type(std::vector<uint8_t>& o, const Type* t) {
    put_u8(o, t ? 1u : 0u);
    put_type(o, t);
}

// Compute max_vreg by scanning the function (the highest VReg+1 referenced).
// The serializer declares this up front so the deserializer can check every
// VReg reference in O(1) without a pre-scan.
uint32_t compute_max_vreg(const ThinFunction& thf) {
    uint32_t max = 1;  // VRegs are 1-indexed; 0 = invalid/none
    auto bump = [&](uint32_t v) { if (v >= max) max = v + 1; };
    for (const auto& blk : thf.blocks) {
        for (const auto& in : blk.instrs) {
            bump(in.dst); bump(in.src1); bump(in.src2);
            for (uint32_t a : in.args) bump(a);
        }
        bump(blk.term.cond);
        bump(blk.term.ret);
    }
    return max;
}

} // namespace

// ─── serialize ───

bool serialize_thin_function(const ThinFunction& thf, std::vector<uint8_t>& out,
                             std::string* err) {
    out.clear();

    // Header
    put_u32(out, IR_BLOB_MAGIC);
    put_u16(out, IR_BLOB_VERSION);
    put_i32(out, thf.slot);
    uint32_t max_vreg = compute_max_vreg(thf);
    if (max_vreg > IR_MAX_VREGS) {
        if (err) *err = "thin_ir_ser: limit: max_vreg exceeds IR_MAX_VREGS";
        return false;
    }
    put_u32(out, max_vreg);
    if (thf.blocks.size() > IR_MAX_BLOCKS) {
        if (err) *err = "thin_ir_ser: limit: block count exceeds IR_MAX_BLOCKS";
        return false;
    }
    put_u16(out, static_cast<uint16_t>(thf.blocks.size()));
    // ret_type
    ser_type(out, thf.ret_type);

    // Frame plan
    put_i32(out, thf.frame.frame_size);
    put_i32(out, thf.frame.rbx_save_offset);
    put_i32(out, thf.frame.struct_ret_ptr_offset);
    put_i32(out, thf.frame.arg_temps_base);
    put_i32(out, thf.frame.next_local_off);
    put_u8(out, thf.frame.returns_struct_by_ptr ? 1u : 0u);
    if (thf.frame.params.size() > IR_MAX_PARAMS) {
        if (err) *err = "thin_ir_ser: limit: param count exceeds IR_MAX_PARAMS";
        return false;
    }
    put_u16(out, static_cast<uint16_t>(thf.frame.params.size()));
    for (const auto& p : thf.frame.params) {
        if (!ser_name(out, p.name, err, "param name")) return false;
        ser_type(out, p.ty);
        put_i32(out, p.off);
        put_i32(out, p.word0);
        put_i32(out, p.nwords);
    }
    if (thf.frame.native_fixup_names.size() > IR_MAX_NFIXUPS) {
        if (err) *err = "thin_ir_ser: limit: native_fixup_names count exceeds IR_MAX_NFIXUPS";
        return false;
    }
    put_u16(out, static_cast<uint16_t>(thf.frame.native_fixup_names.size()));
    for (const auto& nm : thf.frame.native_fixup_names)
        if (!ser_name(out, nm, err, "native_fixup_name")) return false;

    // Rodata
    put_u32(out, static_cast<uint32_t>(thf.rodata.size()));
    out.insert(out.end(), thf.rodata.begin(), thf.rodata.end());

    // Blocks
    for (const auto& blk : thf.blocks) {
        put_u32(out, blk.id);
        if (blk.instrs.size() > IR_MAX_INSTRS) {
            if (err) *err = "thin_ir_ser: limit: instr count exceeds IR_MAX_INSTRS";
            return false;
        }
        put_u16(out, static_cast<uint16_t>(blk.instrs.size()));
        for (const auto& in : blk.instrs) {
            put_u16(out, static_cast<uint16_t>(in.op));
            put_u32(out, in.dst);
            put_u32(out, in.src1);
            put_u32(out, in.src2);
            put_i64(out, in.imm.i);
            put_f64(out, in.imm.f);
            // meta
            put_i32(out, in.meta.frame_off);
            put_i32(out, in.meta.width);
            put_i32(out, in.meta.len);
            put_i32(out, in.meta.slot);
            put_i32(out, in.meta.mod_id);
            put_i32(out, in.meta.field_off);
            put_u8(out, static_cast<uint8_t>(in.meta.base_kind));
            put_u32(out, in.meta.addend);
            if (!ser_name(out, in.meta.native_name, err, "native_name")) return false;
            ser_type(out, in.meta.type);
            put_u8(out, in.meta.cmp);
            put_u8(out, in.meta.is_unsigned);
            put_u8(out, in.meta.is_f32);
            put_u8(out, in.meta.trap_reason);
            // args
            if (in.args.size() > IR_MAX_ARGS) {
                if (err) *err = "thin_ir_ser: limit: args count exceeds IR_MAX_ARGS";
                return false;
            }
            put_u8(out, static_cast<uint8_t>(in.args.size()));
            for (uint32_t a : in.args) put_u32(out, a);
            // arg_frame_offs
            if (in.arg_frame_offs.size() > IR_MAX_ARGS) {
                if (err) *err = "thin_ir_ser: limit: arg_frame_offs count exceeds IR_MAX_ARGS";
                return false;
            }
            put_u8(out, static_cast<uint8_t>(in.arg_frame_offs.size()));
            for (int32_t o : in.arg_frame_offs) put_i32(out, o);
            // arg_types
            if (in.arg_types.size() > IR_MAX_ARGS) {
                if (err) *err = "thin_ir_ser: limit: arg_types count exceeds IR_MAX_ARGS";
                return false;
            }
            put_u8(out, static_cast<uint8_t>(in.arg_types.size()));
            for (const Type* at : in.arg_types) ser_type(out, at);
            // ret_type
            ser_type(out, in.ret_type);
            // loc
            put_u32(out, in.loc.line);
            put_u32(out, in.loc.col);
        }
        // terminator
        put_u8(out, static_cast<uint8_t>(blk.term.kind));
        put_u32(out, blk.term.cond);
        put_u32(out, blk.term.target);
        put_u32(out, blk.term.false_target);
        put_u32(out, blk.term.ret);
        put_u8(out, blk.term.trap_reason);
    }
    return true;
}

// ─── deserialize ───

bool deserialize_thin_function(const uint8_t*& cur, const uint8_t* end,
                               const std::string& name, int32_t slot,
                               ThinFunction& out, std::string* err) {
    // Header
    uint32_t magic = 0;
    if (!read_u32(cur, end, magic)) {
        if (err) *err = "thin_ir_ser: truncated header (magic)";
        return false;
    }
    if (magic != IR_BLOB_MAGIC) {
        if (err) *err = "thin_ir_ser: bad IR blob magic";
        return false;
    }
    uint16_t version = 0;
    if (!read_u16(cur, end, version)) {
        if (err) *err = "thin_ir_ser: truncated header (version)";
        return false;
    }
    if (version != IR_BLOB_VERSION) {
        if (err) *err = "thin_ir_ser: unsupported IR blob version " + std::to_string(version);
        return false;
    }
    int32_t ser_slot = 0;
    if (!read_i32(cur, end, ser_slot)) {
        if (err) *err = "thin_ir_ser: truncated header (slot)";
        return false;
    }
    // The .em container supplies the authoritative slot; the blob's slot is
    // informational. (Semantic validation can cross-check them.)
    uint32_t max_vreg = 0;
    if (!read_u32(cur, end, max_vreg)) {
        if (err) *err = "thin_ir_ser: truncated header (max_vreg)";
        return false;
    }
    if (max_vreg > IR_MAX_VREGS) {
        if (err) *err = "thin_ir_ser: limit: max_vreg exceeds IR_MAX_VREGS";
        return false;
    }
    out.declared_max_vreg = max_vreg;  // stored for validate_thin_function
    uint16_t num_blocks = 0;
    if (!read_u16(cur, end, num_blocks)) {
        if (err) *err = "thin_ir_ser: truncated header (num_blocks)";
        return false;
    }
    if (num_blocks > IR_MAX_BLOCKS) {
        if (err) *err = "thin_ir_ser: limit: num_blocks exceeds IR_MAX_BLOCKS";
        return false;
    }
    if (num_blocks == 0) {
        if (err) *err = "thin_ir_ser: invalid: zero blocks (entry block required)";
        return false;
    }

    out.name = name;
    out.slot = slot;  // authoritative from the .em container
    out.blocks.clear();
    out.blocks.reserve(num_blocks);
    out.owned_types.clear();

    // ret_type
    if (!read_type(cur, end, out.owned_types, out.ret_type, err, "function ret_type"))
        return false;

    // Frame plan
    if (!read_i32(cur, end, out.frame.frame_size)) { if (err) *err = "thin_ir_ser: truncated frame_size"; return false; }
    if (!read_i32(cur, end, out.frame.rbx_save_offset)) { if (err) *err = "thin_ir_ser: truncated rbx_save_offset"; return false; }
    if (!read_i32(cur, end, out.frame.struct_ret_ptr_offset)) { if (err) *err = "thin_ir_ser: truncated struct_ret_ptr_offset"; return false; }
    if (!read_i32(cur, end, out.frame.arg_temps_base)) { if (err) *err = "thin_ir_ser: truncated arg_temps_base"; return false; }
    if (!read_i32(cur, end, out.frame.next_local_off)) { if (err) *err = "thin_ir_ser: truncated next_local_off"; return false; }
    uint8_t ret_struct_ptr = 0;
    if (!read_u8(cur, end, ret_struct_ptr)) { if (err) *err = "thin_ir_ser: truncated returns_struct_by_ptr"; return false; }
    out.frame.returns_struct_by_ptr = ret_struct_ptr != 0;
    uint16_t num_params = 0;
    if (!read_u16(cur, end, num_params)) { if (err) *err = "thin_ir_ser: truncated num_params"; return false; }
    if (num_params > IR_MAX_PARAMS) { if (err) *err = "thin_ir_ser: limit: num_params exceeds IR_MAX_PARAMS"; return false; }
    out.frame.params.clear();
    out.frame.params.reserve(num_params);
    for (uint16_t i = 0; i < num_params; ++i) {
        ThinFramePlan::ParamSpill p;
        if (!read_name(cur, end, p.name, MAX_NAME_SIZE, err, "param name")) return false;
        if (!read_type(cur, end, out.owned_types, p.ty, err, "param type")) return false;
        if (!read_i32(cur, end, p.off)) { if (err) *err = "thin_ir_ser: truncated param off"; return false; }
        if (!read_i32(cur, end, p.word0)) { if (err) *err = "thin_ir_ser: truncated param word0"; return false; }
        if (!read_i32(cur, end, p.nwords)) { if (err) *err = "thin_ir_ser: truncated param nwords"; return false; }
        out.frame.params.push_back(std::move(p));
    }
    uint16_t num_nfixups = 0;
    if (!read_u16(cur, end, num_nfixups)) { if (err) *err = "thin_ir_ser: truncated num_native_fixup_names"; return false; }
    if (num_nfixups > IR_MAX_NFIXUPS) { if (err) *err = "thin_ir_ser: limit: num_native_fixup_names exceeds IR_MAX_NFIXUPS"; return false; }
    out.frame.native_fixup_names.clear();
    out.frame.native_fixup_names.reserve(num_nfixups);
    for (uint16_t i = 0; i < num_nfixups; ++i) {
        std::string nm;
        if (!read_name(cur, end, nm, MAX_NAME_SIZE, err, "native_fixup_name")) return false;
        out.frame.native_fixup_names.push_back(std::move(nm));
    }

    // Rodata
    uint32_t rodata_len = 0;
    if (!read_u32(cur, end, rodata_len)) { if (err) *err = "thin_ir_ser: truncated rodata_len"; return false; }
    {
        const uint8_t* p = nullptr;
        if (!take(cur, end, rodata_len, p)) {
            if (err) *err = "thin_ir_ser: truncated rodata (rodata_len exceeds remaining blob)";
            return false;
        }
        out.rodata.assign(p, p + rodata_len);
    }

    // Blocks
    for (uint16_t bi = 0; bi < num_blocks; ++bi) {
        ThinBlock blk;
        if (!read_u32(cur, end, blk.id)) { if (err) *err = "thin_ir_ser: truncated block id"; return false; }
        uint16_t num_instrs = 0;
        if (!read_u16(cur, end, num_instrs)) { if (err) *err = "thin_ir_ser: truncated num_instrs"; return false; }
        if (num_instrs > IR_MAX_INSTRS) { if (err) *err = "thin_ir_ser: limit: num_instrs exceeds IR_MAX_INSTRS"; return false; }
        blk.instrs.clear();
        blk.instrs.reserve(num_instrs);
        for (uint16_t ii = 0; ii < num_instrs; ++ii) {
            ThinInstr in;
            uint16_t op_raw = 0;
            if (!read_u16(cur, end, op_raw)) { if (err) *err = "thin_ir_ser: truncated instr op"; return false; }
            if (op_raw > THIN_OP_LAST) {
                if (err) *err = "thin_ir_ser: invalid ThinOp ordinal " + std::to_string(op_raw);
                return false;
            }
            in.op = static_cast<ThinOp>(op_raw);
            if (!read_u32(cur, end, in.dst)) { if (err) *err = "thin_ir_ser: truncated instr dst"; return false; }
            if (!read_u32(cur, end, in.src1)) { if (err) *err = "thin_ir_ser: truncated instr src1"; return false; }
            if (!read_u32(cur, end, in.src2)) { if (err) *err = "thin_ir_ser: truncated instr src2"; return false; }
            if (!read_i64(cur, end, in.imm.i)) { if (err) *err = "thin_ir_ser: truncated imm_i"; return false; }
            if (!read_f64(cur, end, in.imm.f)) { if (err) *err = "thin_ir_ser: truncated imm_f"; return false; }
            // meta
            if (!read_i32(cur, end, in.meta.frame_off)) { if (err) *err = "thin_ir_ser: truncated meta.frame_off"; return false; }
            if (!read_i32(cur, end, in.meta.width)) { if (err) *err = "thin_ir_ser: truncated meta.width"; return false; }
            if (!valid_width(in.meta.width)) { if (err) *err = "thin_ir_ser: invalid meta.width (must be 0/1/2/4/8)"; return false; }
            if (!read_i32(cur, end, in.meta.len)) { if (err) *err = "thin_ir_ser: truncated meta.len"; return false; }
            if (!read_i32(cur, end, in.meta.slot)) { if (err) *err = "thin_ir_ser: truncated meta.slot"; return false; }
            if (!read_i32(cur, end, in.meta.mod_id)) { if (err) *err = "thin_ir_ser: truncated meta.mod_id"; return false; }
            if (!read_i32(cur, end, in.meta.field_off)) { if (err) *err = "thin_ir_ser: truncated meta.field_off"; return false; }
            uint8_t base_kind_raw = 0;
            if (!read_u8(cur, end, base_kind_raw)) { if (err) *err = "thin_ir_ser: truncated meta.base_kind"; return false; }
            if (base_kind_raw > BASE_KIND_LAST) { if (err) *err = "thin_ir_ser: invalid meta.base_kind ordinal"; return false; }
            in.meta.base_kind = static_cast<AbsFixup::Kind>(base_kind_raw);
            if (!read_u32(cur, end, in.meta.addend)) { if (err) *err = "thin_ir_ser: truncated meta.addend"; return false; }
            if (!read_name(cur, end, in.meta.native_name, MAX_NAME_SIZE, err, "native_name")) return false;
            if (!read_type(cur, end, out.owned_types, in.meta.type, err, "meta.type")) return false;
            if (!read_u8(cur, end, in.meta.cmp)) { if (err) *err = "thin_ir_ser: truncated meta.cmp"; return false; }
            if (!read_u8(cur, end, in.meta.is_unsigned)) { if (err) *err = "thin_ir_ser: truncated meta.is_unsigned"; return false; }
            if (!read_u8(cur, end, in.meta.is_f32)) { if (err) *err = "thin_ir_ser: truncated meta.is_f32"; return false; }
            if (!read_u8(cur, end, in.meta.trap_reason)) { if (err) *err = "thin_ir_ser: truncated meta.trap_reason"; return false; }
            // args
            uint8_t num_args = 0;
            if (!read_u8(cur, end, num_args)) { if (err) *err = "thin_ir_ser: truncated num_args"; return false; }
            in.args.clear(); in.args.reserve(num_args);
            for (uint8_t ai = 0; ai < num_args; ++ai) {
                uint32_t a = 0;
                if (!read_u32(cur, end, a)) { if (err) *err = "thin_ir_ser: truncated args entry"; return false; }
                in.args.push_back(a);
            }
            // arg_frame_offs
            uint8_t num_afo = 0;
            if (!read_u8(cur, end, num_afo)) { if (err) *err = "thin_ir_ser: truncated num_arg_frame_offs"; return false; }
            in.arg_frame_offs.clear(); in.arg_frame_offs.reserve(num_afo);
            for (uint8_t ai = 0; ai < num_afo; ++ai) {
                int32_t o = 0;
                if (!read_i32(cur, end, o)) { if (err) *err = "thin_ir_ser: truncated arg_frame_offs entry"; return false; }
                in.arg_frame_offs.push_back(o);
            }
            // arg_types
            uint8_t num_at = 0;
            if (!read_u8(cur, end, num_at)) { if (err) *err = "thin_ir_ser: truncated num_arg_types"; return false; }
            in.arg_types.clear(); in.arg_types.reserve(num_at);
            for (uint8_t ai = 0; ai < num_at; ++ai) {
                const Type* at = nullptr;
                if (!read_type(cur, end, out.owned_types, at, err, "arg_type")) return false;
                in.arg_types.push_back(at);
            }
            // ret_type
            if (!read_type(cur, end, out.owned_types, in.ret_type, err, "instr ret_type")) return false;
            // loc
            if (!read_u32(cur, end, in.loc.line)) { if (err) *err = "thin_ir_ser: truncated loc.line"; return false; }
            if (!read_u32(cur, end, in.loc.col)) { if (err) *err = "thin_ir_ser: truncated loc.col"; return false; }
            blk.instrs.push_back(std::move(in));
        }
        // terminator
        uint8_t term_kind_raw = 0;
        if (!read_u8(cur, end, term_kind_raw)) { if (err) *err = "thin_ir_ser: truncated term_kind"; return false; }
        if (term_kind_raw > TERM_KIND_LAST) { if (err) *err = "thin_ir_ser: invalid TermKind ordinal"; return false; }
        blk.term.kind = static_cast<TermKind>(term_kind_raw);
        if (!read_u32(cur, end, blk.term.cond)) { if (err) *err = "thin_ir_ser: truncated term.cond"; return false; }
        if (!read_u32(cur, end, blk.term.target)) { if (err) *err = "thin_ir_ser: truncated term.target"; return false; }
        if (!read_u32(cur, end, blk.term.false_target)) { if (err) *err = "thin_ir_ser: truncated term.false_target"; return false; }
        if (!read_u32(cur, end, blk.term.ret)) { if (err) *err = "thin_ir_ser: truncated term.ret"; return false; }
        if (!read_u8(cur, end, blk.term.trap_reason)) { if (err) *err = "thin_ir_ser: truncated term.trap_reason"; return false; }
        out.blocks.push_back(std::move(blk));
    }

    // non_serializable stays false (the serializer is only called on
    // serializable functions; non-serializable fns ship raw-x86 via is_ir=0).
    out.non_serializable = false;
    out.non_serializable_reason.clear();
    out.abs_fixups.clear();  // populated by emit_x64 at load time

    return true;
}

// ─── semantic validation ───

bool validate_thin_function(const ThinFunction& thf, std::string* err,
                            uint32_t dispatch_size,
                            uint32_t registry_size) {
    const uint32_t num_blocks = static_cast<uint32_t>(thf.blocks.size());
    if (num_blocks == 0) {
        if (err) *err = "thin_ir_ser: validate: zero blocks";
        return false;
    }
    // Block 0 is entry.
    if (thf.blocks[0].id != 0) {
        if (err) *err = "thin_ir_ser: validate: entry block id != 0";
        return false;
    }
    // Frame plan sanity (P7): frame_size in a reasonable range, rbx_save negative.
    if (thf.frame.frame_size < 0 || thf.frame.frame_size > int32_t(1u << 20)) {
        if (err) *err = "thin_ir_ser: validate: frame_size out of range";
        return false;
    }
    if (thf.frame.rbx_save_offset >= 0 || thf.frame.rbx_save_offset < -(1 << 20)) {
        if (err) *err = "thin_ir_ser: validate: rbx_save_offset out of range";
        return false;
    }

    // P1 fix: use the DECLARED max_vreg from the blob header (stored in
    // thf.declared_max_vreg by the deserializer) rather than recomputing it.
    // Recomputing from the function's own VRefs is tautological (every VReg is
    // < recomputed_max by construction). The declared bound is an independent
    // ceiling the attacker cannot inflate — if a VReg reference exceeds it,
    // the blob is malformed. For JIT-lowered functions (declared_max_vreg==0),
    // fall back to the recomputed value.
    uint32_t max_vreg = thf.declared_max_vreg != 0
                        ? thf.declared_max_vreg
                        : compute_max_vreg(thf);

    auto check_vreg = [&](uint32_t v, const char* field) -> bool {
        if (v != 0 && v >= max_vreg) {
            if (err) *err = std::string("thin_ir_ser: validate: VReg out of range in ") + field;
            return false;
        }
        return true;
    };

    std::unordered_set<uint32_t> seen_ids;
    for (uint32_t bi = 0; bi < num_blocks; ++bi) {
        const auto& blk = thf.blocks[bi];
        // C1 fix: block.id must be < num_blocks (emit_x64 uses it as a vector
        // index into block_labels). An out-of-range id is a heap OOB.
        if (blk.id >= num_blocks) {
            if (err) *err = "thin_ir_ser: validate: block " + std::to_string(bi) +
                           " id out of range (" + std::to_string(blk.id) + " >= " +
                           std::to_string(num_blocks) + ")";
            return false;
        }
        // Sec-6 fix: reject duplicate block IDs. Two blocks sharing an id cause
        // block_labels[id] to be bound twice (second wins), so a Jmp/Branch to
        // that id lands in the wrong block.
        if (!seen_ids.insert(blk.id).second) {
            if (err) *err = "thin_ir_ser: validate: duplicate block id " +
                           std::to_string(blk.id) + " (block " + std::to_string(bi) + ")";
            return false;
        }
        // Every block has a terminator.
        if (blk.term.kind == TermKind::None) {
            if (err) *err = "thin_ir_ser: validate: block " + std::to_string(bi) + " has no terminator";
            return false;
        }
        for (const auto& in : blk.instrs) {
            if (!check_vreg(in.dst, "instr dst")) return false;
            if (!check_vreg(in.src1, "instr src1")) return false;
            if (!check_vreg(in.src2, "instr src2")) return false;
            for (uint32_t a : in.args)
                if (!check_vreg(a, "instr args")) return false;
            // C2 fix: CallNative must have a non-empty native_name (the rebind
            // gate relies on this; an empty name bypasses the rebind and
            // produces a null call target).
            if (in.op == ThinOp::CallNative && in.meta.native_name.empty()) {
                if (err) *err = "thin_ir_ser: validate: CallNative with empty native_name";
                return false;
            }
            // Item 11 fix: len must be >= 0 for ConstStringRef/StringDecrypt
            // (a negative len sign-extends to a huge uint64 in the bounds
            // check below, which can wrap and bypass it).
            if ((in.op == ThinOp::ConstStringRef || in.op == ThinOp::StringDecrypt) &&
                in.meta.len < 0) {
                if (err) *err = "thin_ir_ser: validate: negative len for rodata instr";
                return false;
            }
            // P2 fix: ConstStringRef AND StringDecrypt rodata bounds.
            if (in.op == ThinOp::ConstStringRef || in.op == ThinOp::StringDecrypt) {
                uint64_t end_off = uint64_t(in.meta.addend) + uint64_t(in.meta.len);
                if (end_off > thf.rodata.size()) {
                    if (err) *err = "thin_ir_ser: validate: rodata reference out of bounds";
                    return false;
                }
            }
            // Item 12h fix: BoundsCheck len must be >= 0 (a negative len
            // disables the bounds check — the emitted `cmp rcx, len; jae
            // trap` with a negative len (huge unsigned) never traps).
            if (in.op == ThinOp::BoundsCheck && in.meta.len < 0) {
                if (err) *err = "thin_ir_ser: validate: negative len for BoundsCheck";
                return false;
            }
            // Item 12a fix (extended) + Finding A fix (EM_FORMAT_RED_TEAM
            // 2026-07-11): frame_off must be within the function's frame for
            // ANY instr that uses it as an rbp-relative displacement. The
            // original check only covered 7 specific ops, but emit_x64 writes
            // to [rbp + frame_off] for ~20 producing ops (ConstInt, Add, etc.
            // via record_dst/store_rax_to_rbp). An attacker-controlled
            // frame_off on ANY of those can write outside the stack frame
            // (stack smash / return-address overwrite). frame_off must be
            // negative (stack grows down) and >= -frame_size. frame_off == 0
            // means "not frame-backed" and is safe to skip.
            //
            // FINDING A: the prior version gated this check on
            // `frame_size > 0`, so a hand-crafted IR with frame_size == 0
            // skipped the check entirely — a producing op with frame_off = +8
            // overwrote the return address (the prologue always does
            // `push rbp; mov rbp, rsp` regardless of frame_size, so
            // [rbp+0] = saved rbp, [rbp+8] = return addr). The fix: ALWAYS
            // check frame_off regardless of frame_size. When frame_size == 0,
            // -frame_size == 0, so any non-zero frame_off (positive or
            // negative) fails the range check and is rejected.
            if (in.meta.frame_off != 0) {
                if (in.meta.frame_off >= 0 || in.meta.frame_off < -thf.frame.frame_size) {
                    if (err) *err = "thin_ir_ser: validate: frame_off out of frame bounds";
                    return false;
                }
            }
            // P3 fix: CallScript slot < dispatch_size.
            if (in.op == ThinOp::CallScript && dispatch_size > 0 &&
                uint32_t(in.meta.slot) >= dispatch_size) {
                if (err) *err = "thin_ir_ser: validate: CallScript slot out of range";
                return false;
            }
            // P3 fix: CallCrossModule mod_id < registry_size.
            if (in.op == ThinOp::CallCrossModule && registry_size > 0 &&
                uint32_t(in.meta.mod_id) >= registry_size) {
                if (err) *err = "thin_ir_ser: validate: CallCrossModule mod_id out of range";
                return false;
            }
            // P4 fix: Cmp predicate in [0,5] (Eq..Ge).
            if (in.op == ThinOp::Cmp && in.meta.cmp > 5) {
                if (err) *err = "thin_ir_ser: validate: Cmp predicate out of range";
                return false;
            }
        }
        // Block-target bounds (Jmp/Branch).
        if (blk.term.kind == TermKind::Jmp) {
            if (blk.term.target >= num_blocks) {
                if (err) *err = "thin_ir_ser: validate: Jmp target out of bounds";
                return false;
            }
        } else if (blk.term.kind == TermKind::Branch) {
            if (blk.term.target >= num_blocks || blk.term.false_target >= num_blocks) {
                if (err) *err = "thin_ir_ser: validate: Branch target out of bounds";
                return false;
            }
            if (!check_vreg(blk.term.cond, "term.cond")) return false;
        } else if (blk.term.kind == TermKind::Return) {
            if (!check_vreg(blk.term.ret, "term.ret")) return false;
        }
        // Trap: no successor checks (dead end); trap_reason is a u8, no range
        // check needed (it's a TrapReason ordinal used only if the host
        // installs a trap stub).
    }
    return true;
}

} // namespace ember
