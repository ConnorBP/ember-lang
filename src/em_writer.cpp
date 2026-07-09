// ember `.em` serializer impl (BUNDLING_AND_EM_MODULES.md Section 2.3)
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

bool count_fits_u32(size_t n, std::string* err, const char* ctx) {
    if (n > 0xFFFFFFFFu) {
        if (err) *err = std::string("em_writer: count exceeds u32 for ") + ctx +
                         " (n=" + std::to_string(n) + ")";
        return false;
    }
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
        if (!name_fits_u16(f.name, err, "function name")) return false;
        if (!count_fits_u32(f.code.size(),   err, "code_size"))   return false;
        if (!count_fits_u32(f.rodata.size(), err, "rodata_size")) return false;
        if (!count_fits_u32(f.relocs.size(),  err, "reloc_count")) return false;
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
    emit_u32_le(ofs, /*reserved[0]=*/0u);
    emit_u32_le(ofs, /*reserved[1]=*/0u);
    emit_u32_le(ofs, /*reserved[2]=*/0u);

    // ---- 2. Per-function records ----
    for (const auto& f : mod.functions) {
        emit_u16_le(ofs, static_cast<uint16_t>(f.name.size()));
        emit_string(ofs, f.name);
        emit_u32_le(ofs, f.slot_index);
        emit_u32_le(ofs, static_cast<uint32_t>(f.code.size()));
        emit_u32_le(ofs, static_cast<uint32_t>(f.rodata.size()));
        emit_bytes(ofs, f.code);
        emit_bytes(ofs, f.rodata);
        emit_u32_le(ofs, static_cast<uint32_t>(f.relocs.size()));
        for (const auto& r : f.relocs) {
            emit_u32_le(ofs, r.offset);
            // kind is a single byte; EmReloc::Kind values are AbsFixup::Kind
            // values (0/1), already stored as uint8_t.
            uint8_t kb = r.kind;
            ofs.write(reinterpret_cast<const char*>(&kb), 1);
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
