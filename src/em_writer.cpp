// ember `.em` serializer impl (docs/BUNDLING_AND_EM_MODULES.md Section 2.3)
//
// Writes the on-disk binary defined in em_file.hpp / Section 2.2 to `path` via a
// std::ofstream opened in binary mode (Windows target). All multi-byte
// fields are written little-endian by emitting bytes explicitly via shifts  - 
// the same style as the emitter's imm32/imm64 - so the output is correct
// regardless of host byte order (host is LE x86-64 anyway, but the explicit
// form is the documented convention and keeps the format self-describing on
// disk).
//
// Layout written, in order:
//   1. Header (40 bytes): magic, version, flags, function_count, global_size,
//      rodata_total, entry_slot, reserved[3].
//   2. Per-function record (function_count times): name_len, name, slot_index,
//      code_size, rodata_size, code, rodata, reloc_count, relocs.
//   3. Globals block (global_size bytes).
//   4. Name directory: name_table_count, then { name_len, name, slot_index }
//      per entry.
//
// The writer always writes EM_VERSION / EM_MAGIC. No on-the-fly validation of
// those constants (Section 2.7: validation is the loader's job).

#include "em_writer.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace ember {

namespace {

// ---- little-endian emitters (match the emitter's imm32/imm64 style) ----
// Write the low byte first; each shift picks the next byte up. `ofs` is the
// stream we append to; bytes go in order so the on-disk order is LE.

void emit_u16_le(std::ostream& ofs, uint16_t v) {
    uint8_t b[2] = {
        static_cast<uint8_t>(v & 0xFFu),
        static_cast<uint8_t>((v >> 8) & 0xFFu),
    };
    ofs.write(reinterpret_cast<const char*>(b), 2);
}

void emit_u32_le(std::ostream& ofs, uint32_t v) {
    uint8_t b[4] = {
        static_cast<uint8_t>(v & 0xFFu),
        static_cast<uint8_t>((v >> 8) & 0xFFu),
        static_cast<uint8_t>((v >> 16) & 0xFFu),
        static_cast<uint8_t>((v >> 24) & 0xFFu),
    };
    ofs.write(reinterpret_cast<const char*>(b), 4);
}

void emit_bytes(std::ostream& ofs, const uint8_t* p, size_t n) {
    if (n) ofs.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
}

void emit_bytes(std::ostream& ofs, const std::vector<uint8_t>& v) {
    if (!v.empty())
        ofs.write(reinterpret_cast<const char*>(v.data()),
                  static_cast<std::streamsize>(v.size()));
}

void emit_string(std::ostream& ofs, const std::string& s) {
    // name_len is u16; names are expected short. Caller is responsible for
    // not exceeding u16 range; we cap defensively below at the call sites.
    emit_bytes(ofs, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// ---- small validation helpers ----
//
// The format pins name_len (u16) and the various counts/sizes (u32). A
// well-formed EmModule from the compile pipeline will always fit, but a
// pathological input (e.g. a 70000-char name) would silently truncate if we
// just cast. We surface that as a loud error rather than write a corrupt file.

bool name_fits_u16(const std::string& s, std::string* err, const char* ctx) {
    if (s.size() > 0xFFFFu) {
        if (err) *err = std::string("em_writer: name too long (>u16) for ") + ctx +
                         " (len=" + std::to_string(s.size()) + ")";
        return false;
    }
    return true;
}

void emit_type(std::ostream& ofs, const Type& t) {
    uint8_t prim = static_cast<uint8_t>(t.prim);
    uint8_t flags = uint8_t((t.is_slice ? 1 : 0) | (t.array_len ? 2 : 0) |
                            (!t.struct_name.empty() ? 4 : 0) | (t.is_fn_handle ? 8 : 0) |
                            (t.has_recorded_sig ? 16 : 0));
    ofs.write(reinterpret_cast<const char*>(&prim), 1);
    ofs.write(reinterpret_cast<const char*>(&flags), 1);
    emit_u32_le(ofs, t.array_len);
    emit_u16_le(ofs, static_cast<uint16_t>(t.struct_name.size()));
    emit_string(ofs, t.struct_name);
    if ((t.is_slice || t.array_len) && t.elem) emit_type(ofs, *t.elem);
    if (t.is_fn_handle && t.has_recorded_sig) {
        emit_u32_le(ofs, static_cast<uint32_t>(t.recorded_params.size()));
        for (const auto& p : t.recorded_params) emit_type(ofs, p ? *p : Type{});
        emit_type(ofs, t.recorded_ret ? *t.recorded_ret : Type{});
    }
}

void emit_signature(std::ostream& ofs, const EmSignature& sig) {
    emit_type(ofs, sig.ret);
    emit_u32_le(ofs, static_cast<uint32_t>(sig.params.size()));
    for (const Type& p : sig.params) emit_type(ofs, p);
}

bool count_fits_u32(size_t n, std::string* err, const char* ctx) {
    if (n > 0xFFFFFFFFu) {
        if (err) *err = std::string("em_writer: count exceeds u32 for ") + ctx +
                         " (n=" + std::to_string(n) + ")";
        return false;
    }
    return true;
}

// Mirror of the loader's parse_type shape validation. A hand-built Type whose
// prim/flags-equivalent fields are internally inconsistent must be rejected at
// write time, not serialized for the loader to catch. This is the write-side
// half of the M-H14-1 canonical-type consistency gate; the loader enforces the
// same invariants against the on-disk flags/len fields before any allocation.
bool validate_canonical_type(const Type& t, std::string* err, unsigned depth = 0) {
    if (depth > 16) { if (err) *err = "em_writer: type nesting too deep"; return false; }
    // The writer derives flag bits from exactly these fields; a Type that does
    // not match the derivation would serialize a flag the loader would reject.
    const bool is_struct = !t.struct_name.empty();
    const bool is_array  = t.array_len != 0;
    // A slice/array is characterized by its element; its own prim must be Void.
    // A struct name does NOT constrain prim: a script struct has Prim::Void
    // while a host handle (`bind_handle`) is Prim::I64 with a struct-name tag.
    if ((t.is_slice || is_array) && t.prim != Prim::Void) { if (err) *err = "em_writer: inconsistent canonical type: slice/array must have Prim::Void"; return false; }
    if (t.is_slice && is_array) { if (err) *err = "em_writer: inconsistent canonical type: type is both slice and array"; return false; }
    if (t.is_slice && !t.elem) { if (err) *err = "em_writer: inconsistent canonical type: slice requires an element type"; return false; }
    if (is_array && !t.elem) { if (err) *err = "em_writer: inconsistent canonical type: fixed array requires an element type"; return false; }
    if (t.is_fn_handle && t.prim != Prim::I64) { if (err) *err = "em_writer: inconsistent canonical type: function handle requires Prim::I64"; return false; }
    if (t.has_recorded_sig && !t.is_fn_handle) { if (err) *err = "em_writer: inconsistent canonical type: recorded signature requires function handle"; return false; }
    if (t.elem && !validate_canonical_type(*t.elem, err, depth + 1)) return false;
    if (t.is_fn_handle && t.has_recorded_sig) {
        if (t.recorded_params.size() > 1024) { if (err) *err = "em_writer: inconsistent canonical type: function type parameter count"; return false; }
        for (const auto& p : t.recorded_params)
            if (!p || !validate_canonical_type(*p, err, depth + 1)) { if (err) *err = "em_writer: inconsistent canonical type: recorded parameter missing"; return false; }
        if (!t.recorded_ret || !validate_canonical_type(*t.recorded_ret, err, depth + 1)) { if (err) *err = "em_writer: inconsistent canonical type: recorded return type missing"; return false; }
    }
    return true;
}

bool validate_signature(const EmSignature& sig, std::string* err) {
    if (sig.params.size() > 1024) { if (err) *err = "em_writer: signature parameter count"; return false; }
    if (!validate_canonical_type(sig.ret, err)) return false;
    for (const Type& p : sig.params) if (!validate_canonical_type(p, err)) return false;
    return true;
}

} // namespace

bool write_em_file(const EmModule& mod, const char* path, std::string* err) {
    std::ofstream ofs(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!ofs) {
        if (err) *err = std::string("em_writer: could not open output file: ") + path;
        return false;
    }

    // ---- pre-flight size checks so we never write a truncated header ----
    if (!count_fits_u32(mod.functions.size(), err, "function_count")) return false;
    if (!count_fits_u32(mod.globals.size(),  err, "global_size"))      return false;
    if (!count_fits_u32(mod.name_table.size(), err, "name_table_count")) return false;

    // rodata_total = sum of per-fn rodata_size (informational, Section 2.2).
    uint64_t rodata_total_acc = 0;
    for (const auto& f : mod.functions) {
        if (!f.non_serializable_reason.empty()) {
            if (err) *err = "em_writer: function \"" + f.name + "\" is not portable: " + f.non_serializable_reason;
            return false;
        }
        if (!name_fits_u16(f.name, err, "function name")) return false;
        if (!count_fits_u32(f.code.size(),   err, "code_size"))   return false;
        if (!count_fits_u32(f.rodata.size(), err, "rodata_size")) return false;
        if (!count_fits_u32(f.relocs.size(),  err, "reloc_count")) return false;
        if (!count_fits_u32(f.native_bindings.size(), err, "native_binding_count")) return false;
        if (!count_fits_u32(f.signature.params.size(), err, "signature param_count")) return false;
        if (!validate_signature(f.signature, err)) { if (err) *err = "em_writer: function \"" + f.name + "\" has an inconsistent canonical type: " + *err; return false; }
        for (const auto& b : f.native_bindings) {
            if (b.name.empty()) { if (err) *err = "em_writer: native binding has no symbolic name"; return false; }
            if (!name_fits_u16(b.name, err, "native binding")) return false;
            if (uint64_t(b.offset) + 8 > f.code.size()) { if (err) *err = "em_writer: native binding offset out of range"; return false; }
            if (!validate_signature(b.signature, err)) { if (err) *err = "em_writer: native binding \"" + b.name + "\" has an inconsistent canonical type: " + *err; return false; }
        }
        rodata_total_acc += f.rodata.size();
    }
    if (rodata_total_acc > 0xFFFFFFFFull) {
        if (err) *err = "em_writer: rodata_total exceeds u32";
        return false;
    }

    // ---- 1. Header (40 bytes) ----
    emit_u32_le(ofs, EM_MAGIC);
    emit_u32_le(ofs, EM_VERSION);
    emit_u32_le(ofs, /*flags=*/0u);
    emit_u32_le(ofs, static_cast<uint32_t>(mod.functions.size()));
    emit_u32_le(ofs, static_cast<uint32_t>(mod.globals.size()));
    emit_u32_le(ofs, static_cast<uint32_t>(rodata_total_acc));
    emit_u32_le(ofs, mod.entry_slot);
    emit_u32_le(ofs, static_cast<uint32_t>(EM_BUILD_ID));
    emit_u32_le(ofs, static_cast<uint32_t>(EM_BUILD_ID >> 32));
    emit_u32_le(ofs, EM_TARGET_ABI_HASH);

    // ---- 2. Per-function records ----
    for (const auto& f : mod.functions) {
        emit_u16_le(ofs, static_cast<uint16_t>(f.name.size()));
        emit_string(ofs, f.name);
        emit_u32_le(ofs, f.slot_index);
        emit_u32_le(ofs, static_cast<uint32_t>(f.code.size()));
        emit_u32_le(ofs, static_cast<uint32_t>(f.rodata.size()));
        std::vector<uint8_t> portable_code = f.code;
        for (const auto& r : f.relocs) {
            if (uint64_t(r.offset) + 8 > portable_code.size()) { if (err) *err = "em_writer: relocation offset out of range"; return false; }
            std::fill(portable_code.begin() + r.offset, portable_code.begin() + r.offset + 8, 0);
        }
        for (const auto& b : f.native_bindings)
            std::fill(portable_code.begin() + b.offset, portable_code.begin() + b.offset + 8, 0);
        emit_bytes(ofs, portable_code);
        emit_bytes(ofs, f.rodata);
        emit_u32_le(ofs, static_cast<uint32_t>(f.relocs.size()));
        for (const auto& r : f.relocs) {
            emit_u32_le(ofs, r.offset);
            // kind is a single byte; EmReloc::Kind values are AbsFixup::Kind
            // values (0/1), already stored as uint8_t.
            uint8_t kb = r.kind;
            ofs.write(reinterpret_cast<const char*>(&kb), 1);
            emit_u32_le(ofs, r.addend);
        }
        emit_signature(ofs, f.signature);
        emit_u32_le(ofs, static_cast<uint32_t>(f.native_bindings.size()));
        for (const auto& b : f.native_bindings) {
            emit_u32_le(ofs, b.offset);
            emit_u16_le(ofs, static_cast<uint16_t>(b.name.size()));
            emit_string(ofs, b.name);
            emit_signature(ofs, b.signature);
        }
    }

    // ---- 3. Globals block ----
    emit_bytes(ofs, mod.globals);

    // ---- 4. Name directory ----
    emit_u32_le(ofs, static_cast<uint32_t>(mod.name_table.size()));
    for (const auto& [nm, slot] : mod.name_table) {
        if (!name_fits_u16(nm, err, "name-table entry")) {
            // We already started writing the file; signal failure so the
            // caller knows the output is incomplete.
            if (err) *err = "em_writer: name-table entry too long (>u16)";
            ofs.close();
            return false;
        }
        emit_u16_le(ofs, static_cast<uint16_t>(nm.size()));
        emit_string(ofs, nm);
        emit_u32_le(ofs, slot);
    }

    ofs.flush();
    if (!ofs) {
        if (err) *err = "em_writer: I/O error during write/flush";
        return false;
    }
    return true;
}

} // namespace ember
