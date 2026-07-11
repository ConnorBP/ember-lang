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
//      per entry. From v3 (F1 visibility, docs/spec/SPEC_AUDIT_2026-07-10.md F1)
//      the name directory IS the module's EXPORT TABLE: the caller populates
//      `mod.name_table` with only the `is_exported` (`pub fn`/bare `fn`)
//      entries, so a `priv fn` is serialized (its code/relocs are in section 2)
//      but is absent from the directory and therefore not callable cross-
//      module. v1/v2 directories listed every function.
//
// The writer always writes EM_VERSION / EM_MAGIC. No on-the-fly validation of
// those constants (Section 2.7: validation is the loader's job).
//
// F2 (docs/spec/SPEC_AUDIT_2026-07-10.md F2): `write_em_file` emits an UNSIGNED
// v3 module (no signature block — the dev/unsigned artifact). `write_em_file_signed`
// emits a v4 module: the same v3 content bytes (sections 1-4 above) followed by
// an additive Ed25519 signature block. The signature is computed over the v3
// content bytes (offset 0 .. end of name directory); the signature block itself
// is NOT signed (it is appended after the signed payload). The signing key stays
// OFF the host; the host loader gets only the verification public key.

#include "em_writer.hpp"
#include "em_type_codec.hpp"  // shared .em canonical-type codec (emit_type/emit_signature/validate_canonical_type/validate_signature)

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../thirdparty/ed25519/ed25519_ember.hpp"

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

// emit_type / emit_signature / validate_canonical_type / validate_signature
// live in em_type_codec.{hpp,cpp} (the shared .em canonical-type codec) and
// are called unqualified below; they resolve to ember::emit_type etc. via
// ordinary unqualified lookup because we are inside namespace ember.

bool count_fits_u32(size_t n, std::string* err, const char* ctx) {
    if (n > 0xFFFFFFFFu) {
        if (err) *err = std::string("em_writer: count exceeds u32 for ") + ctx +
                         " (n=" + std::to_string(n) + ")";
        return false;
    }
    return true;
}

} // namespace

// ---- shared pre-flight: bounds-check every disk-controlled count/size so we
// never write a truncated header or a corrupt record. Pure validation — no I/O.
bool preflight_em_module(const EmModule& mod, std::string* err, uint64_t& rodata_total_out) {
    if (!count_fits_u32(mod.functions.size(), err, "function_count")) return false;
    if (!count_fits_u32(mod.globals.size(),  err, "global_size"))      return false;
    if (!count_fits_u32(mod.name_table.size(), err, "name_table_count")) return false;

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
    rodata_total_out = rodata_total_acc;
    for (const auto& [nm, slot] : mod.name_table)
        if (!name_fits_u16(nm, err, "name-table entry")) return false;
    return true;
}

// Emit the v3/v4 CONTENT (header -> per-fn records -> globals -> name
// directory) into `ofs`. This byte range is the SIGNED PAYLOAD for a v4
// module (F2): a v4 file is exactly these bytes followed by the additive
// signature block. The version word written is `version` so the unsigned path
// writes v3 and the signed path writes v4 over otherwise-byte-identical content.
bool emit_em_content(std::ostream& ofs, const EmModule& mod, uint32_t version,
                     uint64_t rodata_total, std::string* err) {
    emit_u32_le(ofs, EM_MAGIC);
    emit_u32_le(ofs, version);
    emit_u32_le(ofs, /*flags=*/0u);
    emit_u32_le(ofs, static_cast<uint32_t>(mod.functions.size()));
    emit_u32_le(ofs, static_cast<uint32_t>(mod.globals.size()));
    emit_u32_le(ofs, static_cast<uint32_t>(rodata_total));
    emit_u32_le(ofs, mod.entry_slot);
    emit_u32_le(ofs, static_cast<uint32_t>(EM_BUILD_ID));
    emit_u32_le(ofs, static_cast<uint32_t>(EM_BUILD_ID >> 32));
    emit_u32_le(ofs, EM_TARGET_ABI_HASH);

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

    emit_bytes(ofs, mod.globals);

    emit_u32_le(ofs, static_cast<uint32_t>(mod.name_table.size()));
    for (const auto& [nm, slot] : mod.name_table) {
        if (!name_fits_u16(nm, err, "name-table entry")) { if (err) *err = "em_writer: name-table entry too long (>u16)"; return false; }
        emit_u16_le(ofs, static_cast<uint16_t>(nm.size()));
        emit_string(ofs, nm);
        emit_u32_le(ofs, slot);
    }
    return true;
}

bool write_em_file(const EmModule& mod, const char* path, std::string* err) {
    uint64_t rodata_total = 0;
    if (!preflight_em_module(mod, err, rodata_total)) return false;
    std::ofstream ofs(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!ofs) { if (err) *err = std::string("em_writer: could not open output file: ") + path; return false; }
    if (!emit_em_content(ofs, mod, EM_VERSION_V3, rodata_total, err)) { ofs.close(); return false; }
    ofs.flush();
    if (!ofs) { if (err) *err = "em_writer: I/O error during write/flush"; return false; }
    return true;
}

bool write_em_file_signed(const EmModule& mod, const char* path,
                          const std::array<uint8_t,32>& pub,
                          const std::array<uint8_t,64>& priv,
                          std::string* err) {
    uint64_t rodata_total = 0;
    if (!preflight_em_module(mod, err, rodata_total)) return false;
    // Emit the v4 CONTENT into an in-memory buffer so we can sign it. The signed
    // payload is exactly the bytes from offset 0 (magic) through the end of the
    // name directory (the v3 layout). The version word is EM_VERSION (v4) so the
    // loader routes to the signed path.
    std::stringstream content(std::ios::binary | std::ios::out | std::ios::in);
    if (!emit_em_content(content, mod, EM_VERSION, rodata_total, err)) return false;
    content.flush();
    std::string payload = content.str();
    if (payload.size() > MAX_FILE_SIZE - EM_SIG_BLOCK_SIZE) {
        if (err) *err = "em_writer: signed module content exceeds file size limit";
        return false;
    }
    const uint64_t payload_len = payload.size();

    ed25519::Sig signature{};
    ed25519::sign(reinterpret_cast<const uint8_t*>(payload.data()), payload_len,
                  pub, priv, signature);

    std::ofstream ofs(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!ofs) { if (err) *err = std::string("em_writer: could not open output file: ") + path; return false; }
    ofs.write(payload.data(), static_cast<std::streamsize>(payload_len));
    // Signature block (EM_SIG_BLOCK_SIZE = 104): sig_magic | payload_len | pubkey_id | signature
    emit_u32_le(ofs, EM_SIG_MAGIC);
    emit_u32_le(ofs, static_cast<uint32_t>(payload_len));
    emit_bytes(ofs, pub.data(), pub.size());
    emit_bytes(ofs, signature.data(), signature.size());
    ofs.flush();
    if (!ofs) { if (err) *err = "em_writer: I/O error during write/flush"; return false; }
    return true;
}

} // namespace ember
