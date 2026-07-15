// thin_ir_ser.cpp — Stage B c1b: the ThinFunction serializer/deserializer.
// See thin_ir_ser.hpp for the ir_blob format + the serialization boundary
// contract + the safety-by-construction principles.

#include "thin_ir_ser.hpp"

#include "thin_ir_mutation.hpp"   // compute_central_max_vreg (single source of truth)

#include <cstdint>  // int64_t for overflow-safe span arithmetic
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

// ThinOp is append-only on disk; keep the deserializer ceiling at the actual
// final enumerator. StoreAddr was appended after CallTargetGuard; the Tier 4
// try/catch ops (TryCatch/CatchCleanup/CatchEntry/Throw) were appended after
// StoreAddr.
constexpr uint16_t THIN_OP_LAST = static_cast<uint16_t>(ThinOp::Throw);
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

// Compute max_vreg via the CENTRAL enumeration (compute_central_max_vreg in
// thin_ir_mutation.cpp), which includes implicit dst+1 slice/lambda pair
// results, call args, and terminator cond/ret. The serializer declares this
// up front so the deserializer can check every VReg reference in O(1) without
// a pre-scan. The central enumeration is the single source of truth shared
// with ThinIRMutation (fresh-VReg allocation) — the serializer no longer
// carries a private explicit-field-only scanner.
// (Forward-declared via thin_ir_mutation.hpp; defined here as a thin alias.)

} // namespace

// ─── serialize ───

bool serialize_thin_function(const ThinFunction& thf, std::vector<uint8_t>& out,
                             std::string* err) {
    out.clear();

    // Header
    put_u32(out, IR_BLOB_MAGIC);
    put_u16(out, IR_BLOB_VERSION);
    put_i32(out, thf.slot);
    uint32_t max_vreg = compute_central_max_vreg(thf);
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

    // v3: precise-GC frame-plan fields — the GC-pointer frame-slot offsets
    // (gc_ptr_frame_offs) + the 24-byte GcFrameRecord region offsets. Stable
    // frame-layout data; the deserializer reconstructs them and the JIT re-
    // derives the runtime GcFrameMap at load time.
    if (thf.frame.gc_ptr_frame_offs.size() > IR_MAX_GC_OFFS) {
        if (err) *err = "thin_ir_ser: limit: gc_ptr_frame_offs count exceeds IR_MAX_GC_OFFS";
        return false;
    }
    put_u32(out, static_cast<uint32_t>(thf.frame.gc_ptr_frame_offs.size()));
    for (int32_t o : thf.frame.gc_ptr_frame_offs) put_i32(out, o);
    put_i32(out, thf.frame.gc_rec_off);
    put_i32(out, thf.frame.gc_rec_base_off);
    put_i32(out, thf.frame.gc_rec_map_off);

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
            put_i32(out, in.meta.data_temp_off);  // v2: StringDecrypt data buffer
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
    // Accept v1 (legacy: no data_temp_off), v2 (data_temp_off; no precise-GC
    // frame-plan fields), and v3 (current: precise-GC frame-plan fields after
    // native_fixup_names). Unknown future versions are rejected (fail-closed).
    if (version != 1 && version != 2 && version != IR_BLOB_VERSION) {
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

    // v3: precise-GC frame-plan fields. v1/v2 blobs do NOT carry these — they
    // default to empty/0 (the pre-v3 behavior: no precise GC root scanning).
    if (version >= 3) {
        uint32_t num_gc_offs = 0;
        if (!read_u32(cur, end, num_gc_offs)) { if (err) *err = "thin_ir_ser: truncated num_gc_ptr_frame_offs"; return false; }
        if (num_gc_offs > IR_MAX_GC_OFFS) { if (err) *err = "thin_ir_ser: limit: num_gc_ptr_frame_offs exceeds IR_MAX_GC_OFFS"; return false; }
        out.frame.gc_ptr_frame_offs.clear();
        out.frame.gc_ptr_frame_offs.reserve(num_gc_offs);
        for (uint32_t i = 0; i < num_gc_offs; ++i) {
            int32_t o;
            if (!read_i32(cur, end, o)) { if (err) *err = "thin_ir_ser: truncated gc_ptr_frame_off"; return false; }
            out.frame.gc_ptr_frame_offs.push_back(o);
        }
        if (!read_i32(cur, end, out.frame.gc_rec_off)) { if (err) *err = "thin_ir_ser: truncated gc_rec_off"; return false; }
        if (!read_i32(cur, end, out.frame.gc_rec_base_off)) { if (err) *err = "thin_ir_ser: truncated gc_rec_base_off"; return false; }
        if (!read_i32(cur, end, out.frame.gc_rec_map_off)) { if (err) *err = "thin_ir_ser: truncated gc_rec_map_off"; return false; }
    } else {
        out.frame.gc_ptr_frame_offs.clear();
        out.frame.gc_rec_off = 0;
        out.frame.gc_rec_base_off = 0;
        out.frame.gc_rec_map_off = 0;
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
            // v2 adds meta.data_temp_off at this fixed versioned location
            // (the StringDecrypt decrypted-data buffer offset). v1 blobs do
            // NOT carry this field — it defaults to 0 (the pre-v2 behavior).
            if (version >= 2) {
                if (!read_i32(cur, end, in.meta.data_temp_off)) { if (err) *err = "thin_ir_ser: truncated meta.data_temp_off"; return false; }
            } else {
                in.meta.data_temp_off = 0;  // v1: no data_temp_off field
            }
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

    // ─── v1 compatibility safety rule ───
    // A v1 blob does not carry meta.data_temp_off; it defaults to 0. The
    // emit fallback (data_temp_off != 0 ? data_temp_off : frame_off) then uses
    // frame_off as the decrypted-data buffer, which OVERLAPS the slice result
    // slot (also at frame_off). The XOR loop writes len bytes to frame_off,
    // then the slice result {ptr,len} overwrites the first 16 bytes of that
    // buffer — the decrypted data is corrupted and the slice ptr points into
    // the corrupted region. This is ALWAYS unsafe for a v1 StringDecrypt
    // (the data and slice regions can never be proven nonoverlapping when
    // data_temp_off defaults to 0). Reject such blobs with a format
    // diagnostic rather than allowing them through to emit (plan §7.7: "a
    // v1 StringDecrypt whose buffer/result layout cannot be proven
    // nonoverlapping is rejected with a format diagnostic rather than emitted").
    if (version == 1) {
        for (const auto& blk : out.blocks) {
            for (const auto& in : blk.instrs) {
                if (in.op == ThinOp::StringDecrypt) {
                    if (err) *err = "thin_ir_ser: incompatible v1 blob: StringDecrypt "
                                   "requires data_temp_off (added in v2) for a safe "
                                   "nonoverlapping data/slice layout; v1 default "
                                   "data_temp_off=0 overlaps the slice result slot";
                    return false;
                }
            }
        }
    }

    return true;
}

// ─── frame-plan full-span validation (Finding C, EM_FORMAT_RED_TEAM 2026-07-11) ───
//
// emit_x64's prologue + param-spill write to [rbp + off] using three frame-plan
// offsets the deserialized ThinFunction carries verbatim:
//   - frame.rbx_save_offset       (prologue: store rbx; an 8-byte spill)
//   - frame.struct_ret_ptr_offset  (param-spill: store the hidden return pointer
//                                   rcx; an 8-byte spill, gated on
//                                   returns_struct_by_ptr && off != 0)
//   - frame.params[].off           (param-spill: store the caller-supplied
//                                   argument registers rcx/rdx/r8/r9; a
//                                   multi-word spill spanning [off, off+nwords*8))
// The Finding A fix range-checked instr.meta.frame_off but NOT these frame-plan
// offsets. A hand-crafted v5 IR module (is_ir=1, accepted in dev mode) can set
// params[0].off = +8 so emit_param_spills writes rcx (the caller-controlled
// first argument) to [rbp+8] = the return address — the same return-address-
// overwrite primitive as Finding A, via a sibling unvalidated offset. This
// helper closes it by requiring the FULL write span to stay within the stack
// frame and strictly below rbp.
//
// The frame occupies [rbp - frame_size, rbp); in offset terms [-frame_size, 0).
// [rbp+0] = saved rbp, [rbp+8] = the return address (the prologue always does
// `push rbp; mov rbp, rsp` regardless of frame_size, so those slots always
// exist). Any byte of the write span reaching offset >= 0 overwrites saved
// rbp / the return address. The span [off, off+span) must therefore satisfy:
//   - off < 0            (a non-negative off is the Finding C exploit: a
//                         positive off spills an argument register into the
//                         saved-rbp / return-address slots)
//   - off + span <= 0    (the FULL multi-word span stays strictly below rbp — a
//                         base-offset-only check would miss a short-negative
//                         base whose multi-word extent reaches the return
//                         address, e.g. a 2-word slice spill at off=-8 spans
//                         [-8, +8) and overwrites [rbp+0] and [rbp+8])
//   - off >= -frame_size (the span low end is within the allocated frame — a
//                         write below the frame is a below-rsp write into
//                         unallocated stack, a DoS. Applied only when
//                         frame_size > 0: frame_size == 0 is the hand-crafted
//                         leaf-with-no-frame case pinned by thin_ir_ser_test
//                         case A3, where there is no allocated frame to bound
//                         the low end and only the below-rbp check applies.)
// All arithmetic is done in int64_t so an attacker-controlled offset/span
// cannot overflow int32 and bypass the bounds (the MAINTENANCE_LOG's
// "overflow-safe offset/span helpers" requirement).
static bool frame_plan_span_ok(int64_t off, int64_t span, int32_t frame_size) {
    if (off >= 0) return false;            // off must be rbp-negative
    int64_t end = off + span;              // overflow-safe in int64
    if (end > 0) return false;             // full span must stay below rbp
    if (frame_size > 0 && off < -int64_t(frame_size)) return false;  // within frame
    return true;
}

// ─── instr-level rbp-relative full-span validation (Finding C residual) ───
//
// Finding A range-checked instr.meta.frame_off's BASE offset (off in
// [-frame_size, 0)) but NOT the actual read/write SPAN emit_x64 performs at
// [rbp + off]. A producing op with frame_off = -1 writes 8 bytes to
// [rbp-1, rbp+7) and overwrites saved rbp / the return address even though
// the base offset -1 is in range. The same short-negative-base primitive
// applies to every rbp-relative access: slice stores (16 bytes), F32 stores
// (4 bytes), CopyBytes (len bytes), StringDecrypt's data buffer (len bytes)
// and slice-result slot (16 bytes), and the struct-by-value call-arg /
// struct-return-dest frame offsets in ThinInstr::arg_frame_offs.
//
// This helper is the instr-level analogue of frame_plan_span_ok with ONE
// difference: the low-end bound (off >= -frame_size) is ALWAYS applied, even
// when frame_size == 0. A non-zero instr frame_off with no allocated frame
// (frame_size == 0) is always malformed — unlike the frame-plan offsets
// (rbx_save=-8 etc., which every leaf function carries and which case A3
// pins as legitimate), an instr frame_off is a per-instruction slot that a
// leaf-with-no-frame must not have. So when frame_size == 0 the low-end bound
// becomes `off >= 0`, which (combined with `off < 0`) rejects every non-zero
// off — exactly Finding A's cases A1 (+8) and A2 (-8).
//
// Rule: the FULL span [off, off+span) must lie within [-frame_size, 0):
//   - off < 0            (a non-negative off overwrites saved rbp / return addr)
//   - off + span <= 0    (the full multi-byte span stays strictly below rbp —
//                         catches a short-negative base whose extent reaches
//                         [rbp+0]/[rbp+8], e.g. an 8-byte store at off=-1
//                         spans [-1, +7); a 16-byte slice store at off=-8
//                         spans [-8, +8))
//   - off >= -frame_size (the low end is within the allocated frame — a write
//                         below the frame is a below-rsp write, DoS. ALWAYS
//                         applied; when frame_size == 0 this rejects every
//                         non-zero off.)
// All arithmetic in int64_t (no int32 overflow on attacker-controlled spans).
static bool instr_frame_span_ok(int64_t off, int64_t span, int32_t frame_size) {
    if (off >= 0) return false;            // off must be rbp-negative
    int64_t end = off + span;              // overflow-safe in int64
    if (end > 0) return false;             // full span must stay below rbp
    if (off < -int64_t(frame_size)) return false;  // within frame (always)
    return true;
}

// The byte span of a producing op's dst spill / a StoreFrame's stored value /
// a LoadFrame's loaded value at [rbp + frame_off], derived from the exact
// emit behavior in src/thin_emit.cpp:
//   - slice type (ConstStringRef / StringDecrypt result / Move / StoreFrame /
//     LoadFrame / call slice result): store_reg_mem(rbp, off, rax) +
//     store_reg_mem(rbp, off+8, rdx) = 16 bytes ({ptr, len}).
//   - float F32 (is_f32): movss = 4 bytes.
//   - float F64: movsd = 8 bytes.
//   - int/bool: store_rax_to_rbp / pin_int_dst = mov [rbp+off], rax = 8 bytes
//     (a full qword store; narrow int values are normalized then stored as a
//     full qword, EXCEPT StoreFrame's packed-field path which stores `width`
//     bytes — handled by the caller via the narrow_field flag).
// `narrow_field` selects the StoreFrame packed-field span (meta.width bytes)
// instead of the default 8-byte int store.
static int64_t dst_spill_span(const Type* ty, uint8_t is_f32, bool narrow_field,
                              int32_t width) {
    if (ty && ty->is_slice) return 16;
    if (ty && ty->is_float()) return (is_f32 != 0) ? 4 : 8;
    if (narrow_field) return (width > 0 && width <= 8) ? int64_t(width) : 8;
    return 8;  // int/bool default: full qword store
}

// ─── semantic validation ───

bool validate_thin_function(const ThinFunction& thf, std::string* err,
                            uint32_t dispatch_size,
                            uint32_t registry_size,
                            const int64_t* cross_module_slot_counts,
                            bool skip_cross_module_range) {
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
    // Frame plan sanity (P7) + full-span validation (Finding C,
    // EM_FORMAT_RED_TEAM_2026-07-11). frame_size in a reasonable range; every
    // rbp-relative frame-plan write offset's FULL span stays within the frame
    // and strictly below rbp, so no rbx-save / struct-ret-ptr / param spill
    // can reach the saved rbp at [rbp+0] or the return address at [rbp+8].
    // See frame_plan_span_ok above for the threat model + the rule.
    if (thf.frame.frame_size < 0 || thf.frame.frame_size > int32_t(1u << 20)) {
        if (err) *err = "thin_ir_ser: validate: frame_size out of range";
        return false;
    }
    // rbx_save_offset: the prologue unconditionally spills rbx to [rbp+off]
    // (store_reg_mem rbp, off, rbx) — an 8-byte rbp-relative write. Range-check
    // its full 8-byte span (subsumes the prior sign-only check) plus the 1MB cap.
    if (!frame_plan_span_ok(int64_t(thf.frame.rbx_save_offset), 8,
                            thf.frame.frame_size) ||
        thf.frame.rbx_save_offset < -(1 << 20)) {
        if (err) *err = "thin_ir_ser: validate: rbx_save_offset out of range";
        return false;
    }
    // struct_ret_ptr_offset: emit_param_spills spills the caller-supplied hidden
    // return pointer (rcx) to [rbp+off] when returns_struct_by_ptr && off != 0
    // (the spill is gated on off != 0 in the emit, so off == 0 means "no spill"
    // and is not validated here). A positive off overwrites the return address
    // with a caller-controlled buffer address — the Finding C primitive.
    if (thf.frame.returns_struct_by_ptr && thf.frame.struct_ret_ptr_offset != 0) {
        if (!frame_plan_span_ok(int64_t(thf.frame.struct_ret_ptr_offset), 8,
                                thf.frame.frame_size)) {
            if (err) *err = "thin_ir_ser: validate: struct_ret_ptr_offset out of range";
            return false;
        }
    }
    // params[].off: emit_param_spills spills the caller-supplied argument
    // registers to [rbp + p.off + byte_pos] across the param's full word span.
    // A scalar/float param spills 1 word (8 bytes); a slice param spills 2
    // words (16 bytes, [off, off+16)); a struct param's word count is
    // words_for_type — BUT struct-by-value params are non_serializable ->
    // is_ir=0 raw-x86 fallback and never reach this validator at load time, and
    // the load-time re-emit uses an empty StructLayoutTable so a struct-typed
    // param in an is_ir=1 function is treated as a 1-word scalar spill. The
    // live load-time span is therefore 8 bytes unless p.ty is a slice (16).
    // Validate the FULL span = max(type-derived load-time span, p.nwords*8) —
    // exact for the load-time attack surface, conservative (defense-in-depth)
    // against an inflated p.nwords. Skip the __struct_ret_ptr sentinel
    // (p.ty == nullptr), which is spilled via struct_ret_ptr_offset above.
    for (size_t i = 0; i < thf.frame.params.size(); ++i) {
        const auto& p = thf.frame.params[i];
        if (p.ty == nullptr) continue;  // __struct_ret_ptr sentinel
        int64_t type_span = (p.ty->is_slice) ? int64_t(16) : int64_t(8);
        int64_t nwords_span = int64_t(p.nwords) * 8;  // overflow-safe (int32*8)
        int64_t span = (nwords_span > type_span) ? nwords_span : type_span;
        if (!frame_plan_span_ok(int64_t(p.off), span, thf.frame.frame_size)) {
            if (err) *err = "thin_ir_ser: validate: param " + std::to_string(i) +
                           " offset out of range (off=" + std::to_string(p.off) +
                           ", span=" + std::to_string(span) + ")";
            return false;
        }
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
                        : compute_central_max_vreg(thf);

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
            // Implicit dst+1 for slice/lambda pair results: the second word
            // (len / env_ptr) occupies dst+1 even when no explicit instruction
            // references it. The central enumeration (compute_central_max_vreg)
            // counts this; the validator must check it too so a malformed
            // declared_max_vreg that is too small for the implicit pair word
            // is rejected (e.g. a slice with dst = N-1 has implicit dst+1 = N
            // which is NOT < N). This consumes the same central enumeration /
            // implicit-use logic the serializer uses for the ir_blob header.
            if (in.dst != 0 && in.meta.type &&
                (in.meta.type->is_slice || in.meta.type->is_lambda)) {
                if (!check_vreg(in.dst + 1, "implicit slice/lambda dst+1"))
                    return false;
            }
            // A call returning a slice also occupies dst+1 implicitly (the
            // emitter stores the len word at frame_off+8 and records dst+1).
            if (in.dst != 0 && in.ret_type &&
                (in.ret_type->is_slice || in.ret_type->is_lambda)) {
                if (!check_vreg(in.dst + 1, "implicit call slice/lambda dst+1"))
                    return false;
            }
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
            // Item 12a fix (extended) + Finding A fix + Finding C residual
            // (EM_FORMAT_RED_TEAM 2026-07-11): every rbp-relative frame access
            // instr.meta.frame_off makes must keep its FULL read/write span
            // within [-frame_size, 0). The original Finding A check validated
            // only the BASE offset (off in [-frame_size, 0)), so a short-
            // negative base whose multi-byte extent reaches [rbp+0]/[rbp+8]
            // (e.g. an 8-byte store at off=-1 spans [-1,+7); a 16-byte slice
            // store at off=-8 spans [-8,+8)) overwrote saved rbp / the return
            // address. The full-span check (instr_frame_span_ok) closes that.
            //
            // Spans are derived from the EXACT emit behavior in thin_emit.cpp:
            //   - producing ops (ConstInt/Bool, ConstFloat, ConstStringRef,
            //     Move, Add..BitNot, FAdd..FMod, Cmp, LAnd, LOr, Cast, calls):
            //     the dst spill via record_dst/pin_int_dst/store_xmm0_to_rbp —
            //     16 (slice), 4 (F32), 8 (F64/int). See dst_spill_span.
            //   - StoreFrame (src2==0): the stored value — 16 (slice), 4/8
            //     (float), `width` (int packed-field when field_off!=0), 8 (int
            //     default). StoreFrame with src2!=0 writes [src2(computed)+
            //     frame_off] — frame_off is a computed displacement, NOT rbp-
            //     relative, so it is EXCLUDED (the computed-address
            //     distinction the task requires).
            //   - LoadFrame: frame_off is the rbp-relative read source
            //     (src1==0) or the spill slot (src1!=0); either way rbp-
            //     relative, span 16/4/8. When src1!=0 the LOAD displacement is
            //     meta.field_off (computed) — NOT validated as rbp-relative.
            //   - CopyBytes: frame_off is the rbp-relative DEST (span = len)
            //     when dst==0 && base_kind != GlobalsBase; field_off is the
            //     rbp-relative SOURCE (span = len) when the source side is not
            //     the globals block. Globals-sided offsets are NOT rbp-relative.
            //   - StringDecrypt: meta.data_temp_off is the decrypted-data
            //     buffer (span = len, rbp-relative write); frame_off is the
            //     slice RESULT slot (span = 16, rbp-relative write).
            //   - StructLitInit/ArrayLitInit: write at [rbp + frame_off +
            //     field_off], span = the field element width.
            //   - FieldAddr: frame_off is the spill of the computed address
            //     (8 bytes); frame_off+field_off is an address COMPUTATION
            //     (lea), not an access.
            //   - StoreAddr, IndexAddr, MakeSlice: frame_off is a COMPUTED
            //     address base/displacement (lea or [src2+frame_off]), NOT an
            //     rbp-relative access — EXCLUDED.
            // arg_frame_offs (struct-by-value call args + struct-return hidden
            // dest) and data_temp_off are validated separately below.
            //
            // frame_off == 0 means "not frame-backed" and is skipped (no
            // rbp-relative access). When frame_size == 0, instr_frame_span_ok
            // rejects every non-zero off (Finding A cases A1/A2): a leaf with
            // no allocated frame must not carry an instr frame slot.
            {
                int64_t fo = int64_t(in.meta.frame_off);
                int32_t fsz = thf.frame.frame_size;
                bool fo_ok = true;
                const Type* mt = in.meta.type;
                switch (in.op) {
                case ThinOp::StoreAddr:
                    // [src2(computed) + frame_off]: computed, NOT rbp-relative.
                    break;
                case ThinOp::IndexAddr:
                case ThinOp::MakeSlice:
                    // frame_off is a lea address base — no memory access.
                    break;
                case ThinOp::LoadFrame: {
                    // frame_off is rbp-relative (read src1==0 / spill src1!=0).
                    // field_off is the computed displacement when src1!=0 — not
                    // validated here. Span: slice 16 / float 4|8 / int 8.
                    if (fo != 0) {
                        int64_t sp = dst_spill_span(mt, in.meta.is_f32, false, 0);
                        fo_ok = instr_frame_span_ok(fo, sp, fsz);
                    }
                    break;
                }
                case ThinOp::StoreFrame: {
                    if (in.src2 != 0) break;  // computed store — NOT rbp-relative
                    if (fo != 0) {
                        // int packed-field store (field_off != 0): width bytes;
                        // else the default dst_spill_span.
                        bool narrow = (mt == nullptr || (!mt->is_slice && !mt->is_float()))
                                      && in.meta.field_off != 0;
                        int64_t sp = dst_spill_span(mt, in.meta.is_f32, narrow,
                                                    in.meta.width);
                        fo_ok = instr_frame_span_ok(fo, sp, fsz);
                    }
                    break;
                }
                case ThinOp::CopyBytes: {
                    // frame_off = DEST (span = len) when dst==0 && not globals-dest.
                    // field_off = SOURCE (span = len) when not globals-src.
                    bool global = (in.meta.base_kind == AbsFixup::GlobalsBase);
                    bool dst_is_vreg = (in.dst != 0);
                    bool dst_is_global = global && !dst_is_vreg && in.src1 != 0;
                    bool src_is_global = global && (dst_is_vreg || in.src1 == 0);
                    int64_t len = int64_t(in.meta.len);
                    if (len < 0) len = 0;  // P2/Item 11 reject negative len elsewhere
                    if (!dst_is_vreg && !dst_is_global && fo != 0) {
                        if (!instr_frame_span_ok(fo, len, fsz)) fo_ok = false;
                    }
                    if (!src_is_global && in.meta.field_off != 0) {
                        if (!instr_frame_span_ok(int64_t(in.meta.field_off), len, fsz))
                            fo_ok = false;
                    }
                    break;
                }
                case ThinOp::StringDecrypt: {
                    // data_temp_off: decrypted-data buffer (span = len).
                    // frame_off: slice result slot (span = 16).
                    int64_t len = int64_t(in.meta.len);
                    if (len < 0) len = 0;
                    int64_t data_off = (in.meta.data_temp_off != 0)
                                       ? int64_t(in.meta.data_temp_off) : fo;
                    if (data_off != 0) {
                        if (!instr_frame_span_ok(data_off, len, fsz)) fo_ok = false;
                    }
                    if (fo != 0 && in.meta.data_temp_off != 0) {
                        // slice result slot distinct from the data buffer
                        if (!instr_frame_span_ok(fo, 16, fsz)) fo_ok = false;
                    }
                    break;
                }
                case ThinOp::StructLitInit:
                case ThinOp::ArrayLitInit: {
                    // write at [rbp + frame_off + field_off], span = elem width.
                    int64_t addr = int64_t(in.meta.frame_off) + int64_t(in.meta.field_off);
                    if (addr != 0) {
                        int64_t sp = dst_spill_span(mt, in.meta.is_f32, true,
                                                    in.meta.width);
                        // slice field is 16 (dst_spill_span covers it); the
                        // narrow_field path only applies to non-slice/non-float.
                        if (mt && mt->is_slice) sp = 16;
                        else if (mt && mt->is_float()) sp = (in.meta.is_f32 ? 4 : 8);
                        fo_ok = instr_frame_span_ok(addr, sp, fsz);
                    }
                    break;
                }
                case ThinOp::FieldAddr:
                    // frame_off is the 8-byte spill of the computed address.
                    if (fo != 0) fo_ok = instr_frame_span_ok(fo, 8, fsz);
                    break;
                default:
                    // producing ops: ConstInt/Bool/Float/StringRef, Move,
                    // Add..BitNot, FAdd..FMod, Cmp, LAnd, LOr, Cast, calls.
                    if (fo != 0) {
                        const Type* rt = mt;
                        if (in.op == ThinOp::CallNative || in.op == ThinOp::CallScript ||
                            in.op == ThinOp::CallIndirect || in.op == ThinOp::CallCrossModule)
                            rt = in.ret_type;  // call result type drives the spill span
                        int64_t sp = dst_spill_span(rt, in.meta.is_f32, false, 0);
                        fo_ok = instr_frame_span_ok(fo, sp, fsz);
                    }
                    break;
                }
                if (!fo_ok) {
                    if (err) *err = "thin_ir_ser: validate: frame_off out of frame bounds";
                    return false;
                }
            }
            // StringDecrypt data_temp_off: validated above in the StringDecrypt
            // case (span = len). For other ops data_temp_off is unused (0).
            //
            // ThinInstr::arg_frame_offs (Finding C residual): each entry != -1
            // is an rbp-relative frame access. emit_call_arg_stash READS a
            // struct-by-value arg from [rbp + afo] (copy_bytes src = rbp + afo,
            // span = words_for_type*8 at load time = 8, or 16 for a slice-
            // tagged entry). For a struct-return call (ret_type is a registered
            // struct), arg_frame_offs[0] with args[0]==0 is the hidden DEST —
            // the callee writes the returned struct to [rbp + afo0] (span = 8
            // at load time). A positive afo reads/writes saved rbp / the return
            // address (info-leak for the read; return-address overwrite for the
            // dest). Validate the full span within [-frame_size, 0). -1 entries
            // (plain vreg args / slice two-vreg args) are skipped.
            for (size_t ai = 0; ai < in.arg_frame_offs.size(); ++ai) {
                int32_t afo = in.arg_frame_offs[ai];
                if (afo == -1 || afo == 0) continue;  // -1 = not a frame arg; 0 = unused
                const Type* aty = (ai < in.arg_types.size()) ? in.arg_types[ai] : nullptr;
                int64_t sp = (aty && aty->is_slice) ? 16 : 8;
                if (!instr_frame_span_ok(int64_t(afo), sp, thf.frame.frame_size)) {
                    if (err) *err = "thin_ir_ser: validate: arg_frame_off " +
                                   std::to_string(ai) + " out of frame bounds";
                    return false;
                }
            }
            // P3 fix: CallScript slot < dispatch_size.
            if (in.op == ThinOp::CallScript && dispatch_size > 0 &&
                uint32_t(in.meta.slot) >= dispatch_size) {
                if (err) *err = "thin_ir_ser: validate: CallScript slot out of range";
                return false;
            }
            // A cross-module call indexes the TARGET module's dispatch table
            // directly in thin_emit (emit_cross_module_call: `mov r11,[r11+
            // slot*8]` after the registry hop). Validate both attacker-
            // controlled indices against the REAL host tables before any
            // executable page is emitted.
            //
            // X1 redesign (SANDBOX_REVALIDATION_2026-07-12_ROUND2 /
            // EM_FORMAT_RED_TEAM_2026-07-11): `meta.slot` is an int32 read
            // straight from the attacker-controlled v5 ir_blob. The prior check
            // range-checked it only against an arbitrary 10000 ceiling, so a
            // registry whose target module published a one-slot dispatch table
            // still accepted slots 1..9999 -> emit_cross_module_call read
            // dispatch[slot] out of bounds -> wild call. The real bound is the
            // TARGET module's actual dispatch-table slot count, which the loader
            // threads in via cross_module_slot_counts (length registry_size;
            // cross_module_slot_counts[mod_id] = the target's dispatch size).
            // Fail closed: negative slot never valid; with no per-module counts
            // the slot cannot be range-checked against the real target, so a
            // non-negative slot is rejected (the 10000 ceiling was not
            // conservative — it was the bug); a target that published a 0
            // dispatch size rejects every slot (it opted out of being a
            // cross-module dispatch target).
            if (in.op == ThinOp::CallCrossModule) {
                if (skip_cross_module_range) {
                    // Red 7: the codegen verifier (verify_thin_function_for_codegen)
                    // has no registry view (registry_size == 0), so it SKIPS the
                    // CallCrossModule mod_id/slot range check — the JIT emit
                    // handles a bad mod_id/slot at runtime (the keyed resolver
                    // returns null -> trap; the legacy registry-hop reads a null
                    // table base -> the host's trap stub fires). The LOAD-time
                    // validator (em_loader) passes skip_cross_module_range=false
                    // so it fails closed on an out-of-range mod_id/slot (V5 X1).
                    // This does NOT weaken V5: the load path is unchanged.
                } else if (registry_size == 0 || in.meta.mod_id < 0 ||
                    uint32_t(in.meta.mod_id) >= registry_size) {
                    if (err) *err = "thin_ir_ser: validate: CallCrossModule mod_id out of range";
                    return false;
                } else if (in.meta.slot < 0) {
                    if (err) *err = "thin_ir_ser: validate: CallCrossModule slot out of range";
                    return false;
                } else if (cross_module_slot_counts == nullptr) {
                    if (err) *err = "thin_ir_ser: validate: CallCrossModule slot out of range "
                                   "(no per-module dispatch size to validate against)";
                    return false;
                } else {
                    const int64_t target_slot_count =
                        cross_module_slot_counts[uint32_t(in.meta.mod_id)];
                    if (in.meta.slot >= target_slot_count) {
                        if (err) *err = "thin_ir_ser: validate: CallCrossModule slot out of range "
                                       "(>= target module dispatch size)";
                        return false;
                    }
                }
            }
            // P4 fix: Cmp predicate in [0,5] (Eq..Ge).
            if (in.op == ThinOp::Cmp && in.meta.cmp > 5) {
                if (err) *err = "thin_ir_ser: validate: Cmp predicate out of range";
                return false;
            }
            // Tier 4: TryCatch's meta.slot is a block-id reference (the catch
            // block whose entry rip the inline setjmp saves into
            // context_t::catch_bufs). It MUST be a valid, in-range block id —
            // a stale slot (a pass that renumbered blocks without remapping
            // it) would make the Throw longjmp land at the wrong block (a
            // silent miscompile / infinite loop). canonicalize_block_ids
            // remaps it; this validator is the defense-in-depth gate that
            // rejects a corrupt or hand-rolled blob.
            if (in.op == ThinOp::TryCatch) {
                if (in.meta.slot < 0 ||
                    uint32_t(in.meta.slot) >= num_blocks) {
                    if (err) *err = "thin_ir_ser: validate: TryCatch catch-block slot out of bounds";
                    return false;
                }
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

// ─── Red 5: the in-memory, codegen-facing verifier ───
//
// Delegates the shared CFG/frame/rodata/VReg/op-shape invariants to
// validate_thin_function (dispatch=registry=0, so the host-table-only slot
// checks are skipped — those are load-time concerns, not pass-output ones),
// then adds a codegen-specific frame-plan consistency check (next_local_off
// must stay within the allocated frame) that the disk validator does not make.
// It does NOT validate the regalloc result: `thf.ra` is a JIT-time annotation
// computed by run_regalloc, not an IR invariant a pass produces. A stale/bogus
// `ra` is rejected/cleared by compile_func_checked before the single regalloc/
// emit stage, not by this verifier (see thin_ir_ser.hpp +
// docs/PASS_AUTHORING.md).
bool verify_thin_function_for_codegen(const ThinFunction& thf, std::string* err) {
    std::string v;
    std::string* ep = err ? err : &v;
    if (!validate_thin_function(thf, ep, /*dispatch_size=*/0,
                                /*registry_size=*/0, /*cross_module_slot_counts=*/nullptr,
                                /*skip_cross_module_range=*/true)) {
        if (err && !err->empty() && err->find("codegen") == std::string::npos)
            *err = "codegen verify: " + *err;
        return false;
    }
    // Codegen-specific strengthening: the next-free local offset must stay
    // within the allocated frame (a pass that bumped next_local_off past
    // frame_size would let emit write outside the frame). next_local_off is a
    // magnitude (e.g. 8 => the next free slot is [rbp-8]); frame_size is the
    // rounded total. Permit frame_size == 0 only when nothing is allocated.
    if (thf.frame.frame_size < thf.frame.next_local_off) {
        if (err) *err = "codegen verify: frame.next_local_off exceeds frame_size";
        return false;
    }
    return true;
}

} // namespace ember
