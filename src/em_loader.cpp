// ember `.em` loader impl (BUNDLING_AND_EM_MODULES.md Section 2.5)
//
// Reads the on-disk binary defined in em_file.hpp / Section 2.2 - byte-for-byte the
// same layout em_writer.cpp writes - and produces a `LoadedModule` whose
// exec pages have their baked absolute-imm64 relocations repointed at the
// live dispatch-table and globals-block addresses.
//
// On-disk order (mirror of em_writer.cpp, do not deviate):
//   1. Header (40 bytes): magic, version, flags, function_count, global_size,
//      rodata_total, entry_slot, reserved[3].
//   2. Per-function record (function_count times): name_len(u16), name,
//      slot_index(u32), code_size(u32), rodata_size(u32), code, rodata,
//      reloc_count(u32), relocs { offset(u32), kind(u8) }.
//   3. Globals block (global_size bytes).
//   4. Name directory: name_table_count(u32), then { name_len(u16), name,
//      slot_index(u32) } per entry.
//
// All multi-byte fields are read little-endian by reconstructing via shifts
// (low byte first), mirroring the writer's emit_u16_le / emit_u32_le style
// so the format is self-describing regardless of host byte order (host is LE
// x86-64 anyway).
//
// No regalloc, no emit, no parser, no sema. The per-function work is:
//   alloc_executable(code ++ rodata)  -> RWX page, entry == base
//   patch each reloc's imm64  -> DispatchTableBase: &out.dispatch.data()
//                                GlobalsBase:       &out.globals.data()
//                                ModuleRegistryBase (kind 2): the supplied
//                                ModuleRegistry::base() (MODULES.md Section 3  - 
//                                cross-module call site's registry hop)
//   stamp dispatch[slot]      -> page base  (== fn entry)
//
// PRE-SIZING INVARIANT (critical):
//   `out.dispatch` and `out.globals` are the backing storage whose `.data()`
//   addresses are baked into exec pages as imm64s during reloc patching. If
//   either vector reallocates after a page is patched, its `.data()` moves,
//   every previously-baked absolute address dangles, and the next call into
//   any such function jumps to freed/relocated memory. To guarantee
//   stability we:
//     - reserve `out.globals` to `global_size` BEFORE the per-function loop
//       (global_size is known from the header);
//     - scan the per-function records for `max_slot` and reserve
//       `out.dispatch` to `max_slot + 1` (clamped to function_count as a
//       floor) BEFORE any page is published + patched;
//     - fill `dispatch` by INDEX (`dispatch[slot] = ...`), never push_back;
//     - never append to `globals` after the initial reserve + read.
//   This is the only correct ordering; violating it is a silent corruption.
//
// The records are buffered in a first pass (we need max_slot anyway), then
// the pages are published + patched in a second pass over the buffer. This
// keeps the two concerns - record parsing and page publishing - separate
// and makes the pre-sizing invariant a local, auditable block.

#include "em_loader.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace ember {

namespace {

// ---- little-endian readers (mirror em_writer.cpp's emit_*_le) ----
// Reconstruct the value from low-byte-first bytes via shifts. `ifs` is the
// stream; on short read these return false and leave `v` untouched.

bool read_u8(std::istream& ifs, uint8_t& v) {
    return static_cast<bool>(ifs.read(reinterpret_cast<char*>(&v), 1));
}

bool read_u16(std::istream& ifs, uint16_t& v) {
    uint8_t b[2];
    if (!ifs.read(reinterpret_cast<char*>(b), 2)) return false;
    v = static_cast<uint16_t>(b[0]) |
        static_cast<uint16_t>(b[1]) << 8;
    return true;
}

bool read_u32(std::istream& ifs, uint32_t& v) {
    uint8_t b[4];
    if (!ifs.read(reinterpret_cast<char*>(b), 4)) return false;
    v = static_cast<uint32_t>(b[0]) |
        static_cast<uint32_t>(b[1]) << 8 |
        static_cast<uint32_t>(b[2]) << 16 |
        static_cast<uint32_t>(b[3]) << 24;
    return true;
}

// Read `n` raw bytes into `out`. Returns false on short read.
bool read_bytes(std::istream& ifs, std::vector<uint8_t>& out, size_t n) {
    out.resize(n);
    if (n == 0) return true;
    return static_cast<bool>(
        ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n)));
}

// Write a uint64 little-endian into an 8-byte imm64 slot at `p`. Mirrors the
// writer's LE convention and the emitter's mov_reg_imm64 byte order.
void write_u64_le(uint8_t* p, uint64_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    p[4] = static_cast<uint8_t>((v >> 32) & 0xFFu);
    p[5] = static_cast<uint8_t>((v >> 40) & 0xFFu);
    p[6] = static_cast<uint8_t>((v >> 48) & 0xFFu);
    p[7] = static_cast<uint8_t>((v >> 56) & 0xFFu);
}

// Buffered per-function record (parsed in pass 1, consumed in pass 2). Kept
// POD-ish; we move the vectors through to the publish step.
struct ParsedFn {
    std::string            name;
    uint32_t               slot_index  = 0;
    std::vector<uint8_t>   code;
    std::vector<uint8_t>   rodata;
    std::vector<EmReloc>   relocs;
};

// Parse one per-function record from the stream into `f`. Returns false on
// any short read; `*err` is set with the offending field for diagnostics.
bool parse_fn(std::istream& ifs, ParsedFn& f, std::string* err) {
    uint16_t name_len = 0;
    if (!read_u16(ifs, name_len)) { if (err) *err = "em_loader: truncated read (name_len)"; return false; }
    f.name.resize(name_len);
    if (name_len) {
        if (!ifs.read(reinterpret_cast<char*>(f.name.data()), name_len)) {
            if (err) *err = "em_loader: truncated read (name)";
            return false;
        }
    }

    if (!read_u32(ifs, f.slot_index)) { if (err) *err = "em_loader: truncated read (slot_index)"; return false; }

    uint32_t code_size = 0, rodata_size = 0;
    if (!read_u32(ifs, code_size))   { if (err) *err = "em_loader: truncated read (code_size)";   return false; }
    if (!read_u32(ifs, rodata_size)) { if (err) *err = "em_loader: truncated read (rodata_size)"; return false; }

    if (!read_bytes(ifs, f.code,   code_size))   { if (err) *err = "em_loader: truncated read (code)";   return false; }
    if (!read_bytes(ifs, f.rodata, rodata_size)) { if (err) *err = "em_loader: truncated read (rodata)"; return false; }

    uint32_t reloc_count = 0;
    if (!read_u32(ifs, reloc_count)) { if (err) *err = "em_loader: truncated read (reloc_count)"; return false; }
    f.relocs.resize(reloc_count);
    for (uint32_t i = 0; i < reloc_count; ++i) {
        EmReloc& r = f.relocs[i];
        if (!read_u32(ifs, r.offset)) { if (err) *err = "em_loader: truncated read (reloc.offset)"; return false; }
        if (!read_u8(ifs, r.kind))     { if (err) *err = "em_loader: truncated read (reloc.kind)";   return false; }
    }
    return true;
}

} // namespace

LoadedModule::~LoadedModule() {
    for (void* p : pages) free_executable(p);
    pages.clear();
}

void* LoadedModule::entry_by_name(const char* name) const {
    if (!name) return nullptr;
    for (const auto& [nm, slot] : name_table) {
        if (nm == name) {
            // Bounds-check the slot against the dispatch table; a corrupt
            // name_table with an out-of-range slot returns nullptr rather
            // than reading past the end of `dispatch`.
            if (slot >= dispatch.size()) return nullptr;
            return dispatch[slot];
        }
    }
    return nullptr;
}

void* LoadedModule::entry() const {
    if (entry_slot == EM_NO_ENTRY) return nullptr;
    if (entry_slot >= dispatch.size()) return nullptr; // corrupt module
    return dispatch[entry_slot];
}

bool load_em_file(const char* path, LoadedModule& out, std::string* err,
                  ModuleRegistry* registry) {
    if (!path) {
        if (err) *err = "em_loader: null path";
        return false;
    }

    std::ifstream ifs(path, std::ios::binary | std::ios::in);
    if (!ifs) {
        if (err) *err = std::string("em_loader: could not open input file: ") + path;
        return false;
    }

    // ---- 1. Header (40 bytes) ----
    uint32_t magic = 0, version = 0, flags = 0, function_count = 0;
    uint32_t global_size = 0, rodata_total = 0, entry_slot = 0;
    uint32_t reserved[3] = {0, 0, 0};

    if (!read_u32(ifs, magic))          { if (err) *err = "em_loader: truncated read (magic)";          return false; }
    if (!read_u32(ifs, version))        { if (err) *err = "em_loader: truncated read (version)";        return false; }
    if (!read_u32(ifs, flags))          { if (err) *err = "em_loader: truncated read (flags)";          return false; }
    if (!read_u32(ifs, function_count)) { if (err) *err = "em_loader: truncated read (function_count)"; return false; }
    if (!read_u32(ifs, global_size))    { if (err) *err = "em_loader: truncated read (global_size)";    return false; }
    if (!read_u32(ifs, rodata_total))   { if (err) *err = "em_loader: truncated read (rodata_total)";   return false; }
    if (!read_u32(ifs, entry_slot))      { if (err) *err = "em_loader: truncated read (entry_slot)";      return false; }
    if (!read_u32(ifs, reserved[0]))    { if (err) *err = "em_loader: truncated read (reserved[0])";    return false; }
    if (!read_u32(ifs, reserved[1]))    { if (err) *err = "em_loader: truncated read (reserved[1])";    return false; }
    if (!read_u32(ifs, reserved[2]))    { if (err) *err = "em_loader: truncated read (reserved[2])";    return false; }

    if (magic != EM_MAGIC) {
        if (err) *err = "em_loader: bad magic";
        return false;
    }
    if (version != EM_VERSION) {
        if (err) *err = "em_loader: unsupported version (got " +
                        std::to_string(version) + ", want " +
                        std::to_string(EM_VERSION) + ")";
        return false;
    }
    // flags is reserved 0 in v1; we do not reject non-zero flags here - the
    // spec reserves bit 0 for "embeds source" (Section 2.7 forward-compat), which a
    // v1 loader is permitted to ignore. A future loader stashes the source.
    (void)flags;

    // ---- PRE-SIZING INVARIANT: reserve globals now (header-known size) ----
    // out.globals is reserved to global_size and filled exactly once (the
    // block read below); no later append. Its .data() is baked into exec
    // pages via GlobalsBase relocs, so it must not move after patching.
    out.globals.resize(global_size); // value-initialized to 0; overwritten by read

    // ---- 2. Per-function records: PASS 1 - parse + find max_slot ----
    // Buffer the records: we need max_slot to reserve dispatch before any
    // page is published + patched, and we need the bytes again in pass 2 to
    // actually publish. Buffering is O(total code bytes) and clean.
    std::vector<ParsedFn> fns;
    fns.reserve(function_count);

    uint32_t max_slot = 0;
    bool any_slot_seen = false;
    for (uint32_t i = 0; i < function_count; ++i) {
        ParsedFn f;
        if (!parse_fn(ifs, f, err)) return false;
        if (f.slot_index > max_slot || !any_slot_seen) {
            max_slot = f.slot_index;
            any_slot_seen = true;
        }
        // Sanity: a function with neither code nor rodata cannot be
        // published (alloc_executable on empty bytes yields a null/empty
        // page). Surface now rather than mid-pass-2.
        if (f.code.empty() && f.rodata.empty()) {
            if (err) *err = "em_loader: function \"" + f.name +
                            "\" has no code and no rodata (cannot publish empty page)";
            return false;
        }
        fns.push_back(std::move(f));
    }

    // ---- PRE-SIZING INVARIANT: reserve dispatch now ----
    // dispatch must hold every slot_index referenced by the records AND by
    // the name directory AND by entry_slot. The records are the upper bound
    // (max_slot+1); we floor at function_count so a module with all-empty
    // slots still has a non-degenerate table. After this reserve we fill by
    // index only - never push_back - so .data() never moves.
    size_t dispatch_size = static_cast<size_t>(max_slot) + 1;
    if (dispatch_size < function_count) dispatch_size = function_count;
    out.dispatch.assign(dispatch_size, nullptr); // zero-init; filled by index in pass 2

    // ---- 3. Globals block (global_size bytes, read AFTER per-fn records) ----
    // File order is header, per-fn records, globals, name dir (em_writer.cpp
    // step 3). We already reserved out.globals to global_size above; now
    // read the bytes over it. If global_size==0 the block is empty and we
    // skip the read (the resize above already left it empty).
    if (global_size > 0) {
        if (!ifs.read(reinterpret_cast<char*>(out.globals.data()),
                      static_cast<std::streamsize>(global_size))) {
            if (err) *err = "em_loader: truncated read (globals block)";
            return false;
        }
    }

    // ---- 4. Per-function: PASS 2 - publish page + patch relocs + stamp slot ----
    out.pages.reserve(function_count);
    for (uint32_t i = 0; i < function_count; ++i) {
        ParsedFn& f = fns[i];

        // Combined page = code ++ rodata. Code is first, so the page base
        // IS the function entry. Rodata follows in the same allocation so
        // RIP-relative disp32s baked against (rodata_base - rip) are already
        // correct on the published page: the emitter computed disp32 =
        // (data_base + data_offset) - (rip+4) with data_base == code.size(),
        // and on the combined page code occupies [0, code_size) and rodata
        // occupies [code_size, code_size+rodata_size) - identical layout.
        // (For emitters like prism's that bake string-literal pointers as
        // raw imm64s rather than in-page rodata, rodata is empty and this
        // is just the code bytes.)
        std::vector<uint8_t> combined;
        combined.reserve(f.code.size() + f.rodata.size());
        combined.insert(combined.end(), f.code.begin(),   f.code.end());
        combined.insert(combined.end(), f.rodata.begin(), f.rodata.end());

        void* page_base = alloc_executable(combined); // RWX page, memcpy in
        if (!page_base) {
            if (err) *err = "em_loader: alloc_executable failed for \"" + f.name + "\"";
            return false;
        }
        out.pages.push_back(page_base); // track for free_executable on unload

        // Apply relocations: patch the 8-byte imm64 at `reloc.offset` within
        // the page. reloc.offset is a byte offset within `code` (em_file.hpp
        // Section 2.2: "offset is the byte offset within the function's code of the
        // 8-byte imm64 placeholder"). On the combined page code is first, so
        // the offset into the page EQUALS the offset into code - verify:
        // page = [code | rodata], code spans [0, code_size), reloc.offset <
        // code_size (it points at an instruction in code), so page_base +
        // reloc.offset lands inside the code region. Correct.
        uint8_t* page_bytes = static_cast<uint8_t*>(page_base);
        for (const auto& r : f.relocs) {
            // Bounds-check: the imm64 slot must be fully within code (reloc
            // offsets are within code, per the format contract).
            if (r.offset + 8 > f.code.size()) {
                if (err) *err = "em_loader: reloc offset out of range (offset=" +
                                std::to_string(r.offset) + ", code_size=" +
                                std::to_string(f.code.size()) + ") in \"" + f.name + "\"";
                return false;
            }
            uint8_t* patch = page_bytes + r.offset;
            switch (r.kind) {
                case EmReloc::DispatchTableBase: {
                    // Bake &out.dispatch.data() - the slots array's address.
                    // Stable per the PRE-SIZING INVARIANT (reserved above,
                    // filled by index below, never reallocates).
                    uint64_t addr = reinterpret_cast<uintptr_t>(out.dispatch.data());
                    write_u64_le(patch, addr);
                    break;
                }
                case EmReloc::GlobalsBase: {
                    // Bake &out.globals.data(). Stable per the PRE-SIZING
                    // INVARIANT (reserved before the loop, filled once at
                    // the globals-block read, never reallocates).
                    uint64_t addr = reinterpret_cast<uintptr_t>(out.globals.data());
                    write_u64_le(patch, addr);
                    break;
                }
                case EmReloc::ModuleRegistryBase: {
                    // MODULES.md Section 3 - the cross-module call site's registry
                    // hop. The imm64 is repointed at the per-process
                    // ModuleRegistry::base() (the entries_ array address,
                    // stable per the registry's REGISTRY-BASE STABILITY
                    // INVARIANT - size-at-construction + fill-by-index).
                    // A kind-2 reloc with no supplied registry is a hard
                    // error: the module has a cross-module call site but no
                    // registry to bind to, which would execute a wild jump
                    // on the first call. Loud load-time reject over silent
                    // corruption.
                    if (!registry) {
                        if (err) *err = "em_loader: kind-2 (ModuleRegistryBase) reloc "
                                        "in \"" + f.name + "\" but no ModuleRegistry was "
                                        "supplied to load_em_file (cross-module call "
                                        "site has no registry to bind)";
                        return false;
                    }
                    uint64_t addr = reinterpret_cast<uintptr_t>(registry->base());
                    write_u64_le(patch, addr);
                    break;
                }
                default:
                    if (err) *err = "em_loader: unknown reloc kind (" +
                                    std::to_string(static_cast<unsigned>(r.kind)) +
                                    ") in \"" + f.name + "\"";
                    return false;
            }
        }

        // Stamp the dispatch slot with the function's entry (page base,
        // == start of code). Bounds-check against the reserved size; a
        // corrupt slot_index beyond max_slot is impossible here (max_slot
        // was derived from the records, so dispatch holds it), but guard
        // anyway in case of a logic regression.
        if (f.slot_index >= out.dispatch.size()) {
            if (err) *err = "em_loader: slot_index out of range (" +
                            std::to_string(f.slot_index) + ") in \"" + f.name + "\"";
            return false;
        }
        out.dispatch[f.slot_index] = page_base;
    }

    // fns no longer needed; drop it to free the buffered code/rodata bytes.
    fns.clear();
    fns.shrink_to_fit();

    // ---- 5. Name directory ----
    // File order: name_table_count(u32), then { name_len(u16), name,
    // slot_index(u32) } per entry (em_writer.cpp step 4). We do NOT bounds-
    // check slot_index against dispatch here - a corrupt entry is surfaced
    // lazily by entry_by_name() returning nullptr rather than failing the
    // whole load (the name directory is advisory, not load-critical).
    uint32_t name_table_count = 0;
    if (!read_u32(ifs, name_table_count)) {
        if (err) *err = "em_loader: truncated read (name_table_count)";
        return false;
    }
    out.name_table.clear();
    out.name_table.reserve(name_table_count);
    for (uint32_t i = 0; i < name_table_count; ++i) {
        uint16_t name_len = 0;
        if (!read_u16(ifs, name_len)) {
            if (err) *err = "em_loader: truncated read (name_table name_len)";
            return false;
        }
        std::string nm(name_len, '\0');
        if (name_len) {
            if (!ifs.read(reinterpret_cast<char*>(nm.data()), name_len)) {
                if (err) *err = "em_loader: truncated read (name_table name)";
                return false;
            }
        }
        uint32_t slot = 0;
        if (!read_u32(ifs, slot)) {
            if (err) *err = "em_loader: truncated read (name_table slot_index)";
            return false;
        }
        out.name_table.emplace_back(std::move(nm), slot);
    }

    // ---- 6. entry_slot ----
    out.entry_slot = entry_slot;

    return true;
}

} // namespace ember
