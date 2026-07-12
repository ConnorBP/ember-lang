// ember `.em` loader impl (docs/BUNDLING_AND_EM_MODULES.md Section 2.5)
//
// v1 `.em` contains native x86-64. In addition to the three relocations the
// format records, generated code may contain process-local native/trap/
// allowlist/string pointers. Loading is therefore an ABI/process-trusted
// operation, not validation of an untrusted or portable code container.

#include "em_loader.hpp"
#include "em_type_codec.hpp"  // shared .em canonical-type codec (parse_type/canonical_type_same/parse_signature)
#include "binding_builder.hpp"  // PERM_FFI constant (Finding B: load-side permission gate)
#include "thin_ir_ser.hpp"  // Stage B: deserialize_thin_function / validate_thin_function
#include "thin_emit.hpp"    // Stage B: emit_x64 (re-emit deserialized IR -> x64)
#include "codegen.hpp"      // Stage B: CodeGenCtx (the load-time re-emit context)
#include "dispatch_table.hpp" // Stage B: DispatchTable (for the load-time dispatch base)

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../thirdparty/ed25519/ed25519_ember.hpp"

namespace ember {
namespace {

void set_error(std::string* err, const std::string& message) noexcept {
    if (!err) return;
    try { *err = message; } catch (...) {}
}

// Exact-match overload used from catch handlers: constructing a temporary
// std::string while already handling allocation failure could itself throw.
void set_error(std::string* err, const char* message) noexcept {
    if (!err) return;
    try { *err = message; } catch (...) {}
}

bool checked_add(uint64_t a, uint64_t b, uint64_t& out) {
    if (a > std::numeric_limits<uint64_t>::max() - b) return false;
    out = a + b;
    return true;
}

bool checked_mul(uint64_t a, uint64_t b, uint64_t& out) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) return false;
    out = a * b;
    return true;
}

struct Reader {
    const std::vector<uint8_t>& bytes;
    uint64_t pos = 0;

    uint64_t remaining() const { return uint64_t(bytes.size()) - pos; }

    bool take(uint64_t n, const uint8_t*& p) {
        uint64_t end = 0;
        if (!checked_add(pos, n, end) || end > bytes.size()) return false;
        p = bytes.data() + static_cast<size_t>(pos);
        pos = end;
        return true;
    }

    bool u8(uint8_t& v) {
        const uint8_t* p = nullptr;
        if (!take(1, p)) return false;
        v = p[0];
        return true;
    }

    bool u16(uint16_t& v) {
        const uint8_t* p = nullptr;
        if (!take(2, p)) return false;
        v = static_cast<uint16_t>(p[0]) |
            static_cast<uint16_t>(p[1]) << 8;
        return true;
    }

    bool u32(uint32_t& v) {
        const uint8_t* p = nullptr;
        if (!take(4, p)) return false;
        v = static_cast<uint32_t>(p[0]) |
            static_cast<uint32_t>(p[1]) << 8 |
            static_cast<uint32_t>(p[2]) << 16 |
            static_cast<uint32_t>(p[3]) << 24;
        return true;
    }
};

void write_u64_le(uint8_t* p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xffu);
}

struct ParsedFn {
    std::string name;
    uint32_t slot_index = 0;
    EmSignature signature;
    std::vector<uint8_t> code;
    std::vector<uint8_t> rodata;
    std::vector<EmReloc> relocs;
    std::vector<EmNativeBinding> native_bindings;
    // v5 (Stage B): the explicit per-function IR/raw marker, read from the
    // on-disk `is_ir` byte. `is_ir == true` -> this is an IR function: ir_blob
    // is the output of serialize_thin_function (opaque to parse_file;
    // deserialized + validated + re-emitted in load_em_bytes_impl) and
    // code/rodata/relocs/native_bindings stay empty. `is_ir == false` -> this
    // is a raw-x86 fallback: code/rodata/relocs/native_bindings are populated
    // as in v3/v4 and ir_blob stays empty. This is the SECURE-DEFAULT
    // discriminator: load_em_bytes_impl rejects any v5 function with
    // is_ir == false under the secure default (allow_raw_x86 == false) before
    // any executable allocation — a raw-x86 function is an arbitrary-code-
    // execution surface by construction (FIX 3 extended from v1-v4 to the v5
    // per-function level). `ir_blob.empty()` tracks is_ir for v5 (an is_ir=1
    // function must carry a non-empty blob, enforced in parse_file as the P6
    // fix), but the explicit `is_ir` marker is the load-side gate so the
    // secure-default rejection does not lean on an empty-blob side effect.
    bool is_ir = false;
    std::vector<uint8_t> ir_blob;
};

struct ParsedModule {
    std::vector<ParsedFn> functions;
    std::vector<uint8_t> globals;
    std::vector<std::pair<std::string, uint32_t>> names;
    uint32_t entry_slot = EM_NO_ENTRY;
    size_t dispatch_size = 0;
    uint32_t version = EM_VERSION_V1;
    // F2 v4 signature block (parsed but NOT yet verified; verification runs in
    // load_em_file_impl BEFORE alloc_executable_rw). `sig_payload_len` is the
    // byte length of the SIGNED payload (offset 0 .. sig block start); it is
    // cross-checked against the position the parser reached at the end of the
    // name directory, so a lying/truncated block is rejected here. `signature`
    // and `pubkey_id` are the block's 64- and 32-byte fields. All three stay
    // zero/default for v1/v2/v3 (unsigned modules).
    uint64_t sig_payload_len = 0;
    std::array<uint8_t, EM_SIG_PUBKEY_SIZE>  pubkey_id{};
    std::array<uint8_t, EM_SIG_SIGNATURE_SIZE> signature{};
    bool has_signature_block = false;
};

// Owns pages until the complete module has been sealed and committed to `out`.
// No dispatch entry or page is published through LoadedModule on failure.
struct StagedModule {
    std::vector<void*> dispatch;
    std::vector<uint8_t> globals;
    std::vector<void*> pages;
    std::vector<std::pair<std::string, uint32_t>> names;
    uint32_t entry_slot = EM_NO_ENTRY;
    uint32_t version = EM_VERSION_V1;
    std::vector<EmSignature> signatures_by_slot;

    ~StagedModule() {
        for (void* page : pages) free_executable(page);
    }

    void commit(LoadedModule& out) {
        dispatch.swap(out.dispatch);
        globals.swap(out.globals);
        pages.swap(out.pages);
        names.swap(out.name_table);
        signatures_by_slot.swap(out.signatures_by_slot);
        std::swap(entry_slot, out.entry_slot);
        std::swap(version, out.format_version);
        // This object now owns the old contents of `out`; its destructor frees
        // any old pages after the new module has been atomically assembled.
    }
};

bool parse_name(Reader& rd, std::string& out, const char* field,
                std::string* err) {
    uint16_t len = 0;
    if (!rd.u16(len)) {
        set_error(err, std::string("em_loader: format: truncated ") + field + " length");
        return false;
    }
    if (len > MAX_NAME_SIZE) {
        set_error(err, std::string("em_loader: limit: ") + field + " exceeds MAX_NAME_SIZE");
        return false;
    }
    const uint8_t* p = nullptr;
    if (!rd.take(len, p)) {
        set_error(err, std::string("em_loader: format: truncated ") + field);
        return false;
    }
    out.assign(reinterpret_cast<const char*>(p), len);
    return true;
}

// parse_type / canonical_type_same / parse_signature live in em_type_codec
// (the shared .em canonical-type codec) and operate on a raw (cur, end) cursor
// pair. The loader keeps a Reader-based adapter here so its call sites in
// parse_file stay unchanged; the adapter bridges Reader <-> cursor and then
// delegates to ember::parse_signature. canonical_type_same is a pure
// Type-vs-Type comparison with no cursor, so parse_file calls it directly
// (unqualified lookup resolves to ember::canonical_type_same).
bool parse_signature(Reader& rd, EmSignature& sig, std::string* err) {
    const uint8_t* cur = rd.bytes.data() + rd.pos;
    const uint8_t* end = rd.bytes.data() + rd.bytes.size();
    if (!::ember::parse_signature(cur, end, sig, err)) {
        // Advance pos to wherever the cursor stopped so a follow-on diagnostic
        // or a caller inspecting position sees a best-effort offset. The parse
        // has already failed; the exact pos does not affect the reject path.
        rd.pos = static_cast<uint64_t>(cur - rd.bytes.data());
        return false;
    }
    rd.pos = static_cast<uint64_t>(cur - rd.bytes.data());
    return true;
}

bool parse_file(const std::vector<uint8_t>& file, ParsedModule& mod,
                ModuleRegistry* registry,
                const std::unordered_map<std::string, NativeSig>* natives,
                uint32_t module_permissions,
                std::string* err) {
    Reader rd{file};
    uint32_t magic = 0, version = 0, flags = 0, function_count = 0;
    uint32_t global_size = 0, rodata_total = 0;
    uint32_t reserved[3] = {};

    if (!rd.u32(magic) || !rd.u32(version) || !rd.u32(flags) ||
        !rd.u32(function_count) || !rd.u32(global_size) ||
        !rd.u32(rodata_total) || !rd.u32(mod.entry_slot) ||
        !rd.u32(reserved[0]) || !rd.u32(reserved[1]) || !rd.u32(reserved[2])) {
        set_error(err, "em_loader: format: truncated header");
        return false;
    }
    if (magic != EM_MAGIC) {
        set_error(err, "em_loader: format: bad magic");
        return false;
    }
    if (version != EM_VERSION_V1 && version != EM_VERSION_V2 &&
        version != EM_VERSION_V3 && version != EM_VERSION &&
        version != EM_VERSION_V5) {
        set_error(err, "em_loader: format: unsupported version " + std::to_string(version)); return false;
    }
    mod.version = version;
    if(flags!=0){set_error(err,"em_loader: format: unsupported flags");return false;}
    if(version==EM_VERSION_V1) {
        if(reserved[0]||reserved[1]||reserved[2]){set_error(err,"em_loader: format: unsupported v1 reserved fields");return false;}
    } else {
        uint64_t build=uint64_t(reserved[0])|(uint64_t(reserved[1])<<32);
        if(build!=EM_BUILD_ID||reserved[2]!=EM_TARGET_ABI_HASH){set_error(err,"em_loader: compatibility: compiler/build ID or target ABI hash mismatch");return false;}
    }
    if (function_count > MAX_FUNCTIONS) {
        set_error(err, "em_loader: limit: function_count exceeds MAX_FUNCTIONS");
        return false;
    }
    if (global_size > MAX_GLOBALS) {
        set_error(err, "em_loader: limit: global_size exceeds MAX_GLOBALS");
        return false;
    }
    if (rodata_total > MAX_FILE_SIZE) {
        set_error(err, "em_loader: limit: rodata_total exceeds file limit");
        return false;
    }

    // Even zero-length records need 18 fixed bytes. Check multiplication and
    // remaining bytes before reserve or per-record work.
    uint64_t min_functions_bytes = 0;
    if (!checked_mul(function_count, 18u, min_functions_bytes) ||
        min_functions_bytes > rd.remaining()) {
        set_error(err, "em_loader: format: function records cannot fit in file");
        return false;
    }
    mod.functions.reserve(function_count);
    std::vector<uint8_t> slot_seen(MAX_SLOTS, 0);
    uint32_t max_slot = 0;
    uint64_t actual_rodata = 0;

    for (uint32_t i = 0; i < function_count; ++i) {
        ParsedFn f;
        if (!parse_name(rd, f.name, "function name", err)) return false;

        uint32_t code_size = 0, rodata_size = 0;
        if (!rd.u32(f.slot_index)) {
            set_error(err, "em_loader: format: truncated function metadata (slot_index)");
            return false;
        }
        if (f.slot_index >= MAX_SLOTS) {
            set_error(err, "em_loader: limit: slot_index exceeds MAX_SLOTS");
            return false;
        }
        if (slot_seen[f.slot_index]) {
            set_error(err, "em_loader: format: duplicate function slot " +
                           std::to_string(f.slot_index));
            return false;
        }
        slot_seen[f.slot_index] = 1;
        max_slot = std::max(max_slot, f.slot_index);

        // v5 per-function record: is_ir byte + hoisted signature, then either
        // the IR blob (is_ir=1) or the v4 raw-x86 body (is_ir=0).
        if (version == EM_VERSION_V5) {
            uint8_t is_ir = 0;
            if (!rd.u8(is_ir)) {
                set_error(err, "em_loader: format: truncated v5 is_ir byte");
                return false;
            }
            if (is_ir != 0 && is_ir != 1) {
                set_error(err, "em_loader: format: invalid v5 is_ir byte (must be 0 or 1)");
                return false;
            }
            // Record the explicit per-function IR/raw marker. This is the
            // SECURE-DEFAULT discriminator load_em_bytes_impl gates on (a
            // raw-x86 v5 function, is_ir=0, is rejected under the secure
            // default before any executable allocation).
            f.is_ir = (is_ir != 0);
            if (!parse_signature(rd, f.signature, err)) return false;
            if (is_ir) {
                // IR function: read the opaque ir_blob. parse_file does NOT
                // interpret it — deserialization + validation + re-emit happen
                // in load_em_file_impl BEFORE alloc_executable_rw.
                uint32_t ir_blob_len = 0;
                if (!rd.u32(ir_blob_len)) {
                    set_error(err, "em_loader: format: truncated v5 ir_blob_len");
                    return false;
                }
                if (ir_blob_len > MAX_FILE_SIZE) {
                    set_error(err, "em_loader: limit: v5 ir_blob_len exceeds file limit");
                    return false;
                }
                if (ir_blob_len > rd.remaining()) {
                    set_error(err, "em_loader: format: v5 ir_blob cannot fit in file");
                    return false;
                }
                const uint8_t* blob = nullptr;
                if (!rd.take(ir_blob_len, blob)) {
                    set_error(err, "em_loader: format: truncated v5 ir_blob");
                    return false;
                }
                // P6 fix: is_ir=1 with ir_blob_len=0 is malformed (an IR
                // function must carry a non-empty blob).
                if (ir_blob_len == 0) {
                    set_error(err, "em_loader: format: v5 is_ir=1 with empty ir_blob");
                    return false;
                }
                f.ir_blob.assign(blob, blob + ir_blob_len);
                mod.functions.push_back(std::move(f));
                continue;  // NO code/rodata/relocs/native_bindings follow.
            }
            // is_ir == 0: fall through to the raw-x86 body (code_size +
            // rodata_size follow, same as v3/v4 after the hoisted signature).
        }

        if (!rd.u32(code_size) || !rd.u32(rodata_size)) {
            set_error(err, "em_loader: format: truncated function metadata (code/rodata size)");
            return false;
        }

        if (code_size == 0 || code_size > MAX_CODE_PER_FN) {
            set_error(err, "em_loader: limit: invalid/excessive code_size in \"" + f.name + "\"");
            return false;
        }
        if (rodata_size > MAX_RODATA_PER_FN) {
            set_error(err, "em_loader: limit: rodata_size exceeds MAX_RODATA_PER_FN");
            return false;
        }
        uint64_t payload_size = 0;
        if (!checked_add(code_size, rodata_size, payload_size) ||
            payload_size > rd.remaining()) {
            set_error(err, "em_loader: format: function code/rodata cannot fit in file");
            return false;
        }
        const uint8_t* code = nullptr;
        const uint8_t* rodata = nullptr;
        if (!rd.take(code_size, code) || !rd.take(rodata_size, rodata)) {
            set_error(err, "em_loader: format: truncated function payload");
            return false;
        }
        f.code.assign(code, code + code_size);
        f.rodata.assign(rodata, rodata + rodata_size);
        if (!checked_add(actual_rodata, rodata_size, actual_rodata)) {
            set_error(err, "em_loader: format: rodata_total overflow");
            return false;
        }

        uint32_t reloc_count = 0;
        if (!rd.u32(reloc_count)) {
            set_error(err, "em_loader: format: truncated reloc_count");
            return false;
        }
        if (reloc_count > MAX_RELOCS_PER_FN) {
            set_error(err, "em_loader: limit: reloc_count exceeds MAX_RELOCS_PER_FN");
            return false;
        }
        uint64_t reloc_bytes = 0;
        const uint64_t reloc_record_size = version == EM_VERSION_V1 ? 5u : 9u;
        if (!checked_mul(reloc_count, reloc_record_size, reloc_bytes) || reloc_bytes > rd.remaining()) {
            set_error(err, "em_loader: format: relocation records cannot fit in file");
            return false;
        }
        f.relocs.resize(reloc_count);
        for (EmReloc& r : f.relocs) {
            if (!rd.u32(r.offset) || !rd.u8(r.kind) || (version != EM_VERSION_V1 && !rd.u32(r.addend))) {
                set_error(err, "em_loader: format: truncated relocation");
                return false;
            }
            // Use subtraction in 64-bit space: r.offset + 8 must never wrap.
            if (!(f.code.size() >= 8 &&
                  uint64_t(r.offset) <= uint64_t(f.code.size()) - 8)) {
                set_error(err, "em_loader: format: relocation offset out of range in \"" +
                               f.name + "\"");
                return false;
            }
            const uint8_t max_kind = version == EM_VERSION_V1 ? EmReloc::ModuleRegistryBase : EmReloc::FunctionRodataBase;
            if (r.kind > max_kind) {
                set_error(err, "em_loader: format: invalid relocation kind " +
                               std::to_string(static_cast<unsigned>(r.kind)));
                return false;
            }
            if(r.kind!=EmReloc::FunctionRodataBase&&r.addend!=0){set_error(err,"em_loader: format: relocation addend only valid for function rodata");return false;}
            if(r.kind==EmReloc::FunctionRodataBase&&r.addend>f.rodata.size()){set_error(err,"em_loader: format: rodata relocation addend out of range");return false;}
            if (r.kind == EmReloc::ModuleRegistryBase && !registry) {
                set_error(err, "em_loader: binding: ModuleRegistryBase relocation requires a registry");
                return false;
            }
        }
        // v2+ records carry a canonical export signature + symbolic native
        // bindings (v3 keeps the v2 per-function layout byte-identical; only
        // the name directory's contents differ - F1 visibility). v5 hoisted
        // the signature above the is_ir branch, so it is NOT re-read here.
        if(version >= EM_VERSION_V2) {
            if (version != EM_VERSION_V5) {
                if(!parse_signature(rd,f.signature,err))return false;
            }
            uint32_t binding_count=0;if(!rd.u32(binding_count)){set_error(err,"em_loader: format: truncated native binding count");return false;}
            if(binding_count>MAX_RELOCS_PER_FN){set_error(err,"em_loader: limit: native binding count");return false;}
            f.native_bindings.resize(binding_count);
            for(auto& b:f.native_bindings){
                if(!rd.u32(b.offset)||!parse_name(rd,b.name,"native binding name",err)||!parse_signature(rd,b.signature,err))return false;
                if(uint64_t(b.offset)+8>f.code.size()){set_error(err,"em_loader: format: native binding offset out of range");return false;}
                if(!natives){set_error(err,"em_loader: binding: v2 module requires a host native allowlist");return false;}
                auto it=natives->find(b.name);if(it==natives->end()||!it->second.fn_ptr){set_error(err,"em_loader: binding: missing native \""+b.name+"\"");return false;}
                if(!canonical_type_same(it->second.ret,b.signature.ret)||it->second.params.size()!=b.signature.params.size()){set_error(err,"em_loader: binding: signature mismatch for native \""+b.name+"\"");return false;}
                for(size_t pi=0;pi<b.signature.params.size();++pi)if(!canonical_type_same(it->second.params[pi],b.signature.params[pi])){set_error(err,"em_loader: binding: signature mismatch for native \""+b.name+"\"");return false;}
                // Finding B (EM_FORMAT_RED_TEAM 2026-07-11): PERM_FFI load-side
                // enforcement. A hand-crafted .em bypasses sema's compile-time
                // PERM_FFI gate, so the loader must check NativeSig::permission
                // here. A native flagged PERM_FFI is rejected if the loading
                // module's permissions lack the FFI bit. This mirrors sema's
                // compile-time check (sema.cpp:1954) at the load boundary.
                if((it->second.permission&PERM_FFI)&&!(module_permissions&PERM_FFI)){set_error(err,"em_loader: binding: native \""+b.name+"\" requires PERM_FFI permission (module lacks it)");return false;}
            }
        }
        mod.functions.push_back(std::move(f));
    }

    if (actual_rodata != rodata_total) {
        set_error(err, "em_loader: format: rodata_total does not match function records");
        return false;
    }

    const uint8_t* globals = nullptr;
    if (!rd.take(global_size, globals)) {
        set_error(err, "em_loader: format: truncated globals block");
        return false;
    }
    // Preserve serialized initialized bytes exactly; do not substitute a new
    // zero-filled block on this path.
    mod.globals.assign(globals, globals + global_size);

    uint32_t name_count = 0;
    if (!rd.u32(name_count)) {
        set_error(err, "em_loader: format: missing name directory");
        return false;
    }
    if (name_count > MAX_NAMES) {
        set_error(err, "em_loader: limit: name_table_count exceeds MAX_NAMES");
        return false;
    }
    uint64_t min_name_bytes = 0;
    if (!checked_mul(name_count, 6u, min_name_bytes) || min_name_bytes > rd.remaining()) {
        set_error(err, "em_loader: format: name directory cannot fit in file");
        return false;
    }
    mod.names.reserve(name_count);
    for (uint32_t i = 0; i < name_count; ++i) {
        std::string name;
        if (!parse_name(rd, name, "directory name", err)) return false;
        uint32_t slot = 0;
        if (!rd.u32(slot)) {
            set_error(err, "em_loader: format: truncated name-directory slot");
            return false;
        }
        if (slot >= MAX_SLOTS || !slot_seen[slot]) {
            set_error(err, "em_loader: format: name directory references an impossible slot");
            return false;
        }
        mod.names.emplace_back(std::move(name), slot);
    }

    // F2 (docs/spec/SPEC_AUDIT_2026-07-10.md F2): the byte position reached here
    // is the END of the v3 content (header -> name directory) — i.e. the start
    // of the trailing bytes, which is exactly the signed PAYLOAD length for a
    // v4 module. Capture it before handling the trailing bytes.
    const uint64_t payload_end = rd.pos;

    // Trailing bytes: v1/v2/v3 carry NO signature block -> the file MUST end
    // here (the historical "trailing bytes == 0" check). v4 carries exactly
    // one additive Ed25519 signature block (em_file.hpp EM_SIG_BLOCK_SIZE =
    // 104 bytes) starting with the EMSG sentinel; parse it here. The
    // signature is VERIFIED later in load_em_file_impl, BEFORE
    // alloc_executable_rw — parse_file only captures the block.
    if (version == EM_VERSION) {
        if (rd.remaining() != EM_SIG_BLOCK_SIZE) {
            set_error(err, "em_loader: format: v4 module has a malformed signature block (wrong trailing size)");
            return false;
        }
        uint32_t sig_magic = 0;
        uint32_t sig_payload_len = 0;
        if (!rd.u32(sig_magic) || !rd.u32(sig_payload_len)) {
            set_error(err, "em_loader: format: truncated signature block header");
            return false;
        }
        if (sig_magic != EM_SIG_MAGIC) {
            set_error(err, "em_loader: format: bad signature-block sentinel");
            return false;
        }
        // The block's payload_len MUST equal the position the parser reached at
        // the end of the name directory (otherwise the signed range is a lie —
        // a truncated or padded file would verify against a different byte range
        // than the loader actually parsed). This is a structural cross-check;
        // cryptographic verification happens in load_em_file_impl.
        if (sig_payload_len != payload_end) {
            set_error(err, "em_loader: format: signature payload length does not match content length");
            return false;
        }
        const uint8_t* pk = nullptr;
        const uint8_t* sg = nullptr;
        if (!rd.take(EM_SIG_PUBKEY_SIZE, pk) || !rd.take(EM_SIG_SIGNATURE_SIZE, sg)) {
            set_error(err, "em_loader: format: truncated signature block payload");
            return false;
        }
        std::memcpy(mod.pubkey_id.data(), pk, EM_SIG_PUBKEY_SIZE);
        std::memcpy(mod.signature.data(), sg, EM_SIG_SIGNATURE_SIZE);
        mod.sig_payload_len = sig_payload_len;
        mod.has_signature_block = true;
    } else {
        if (rd.remaining() != 0) {
            set_error(err, "em_loader: format: trailing bytes after name directory");
            return false;
        }
    }
    if (rd.remaining() != 0) {
        set_error(err, "em_loader: format: trailing bytes after signature block");
        return false;
    }
    if (mod.entry_slot != EM_NO_ENTRY &&
        (mod.entry_slot >= MAX_SLOTS || !slot_seen[mod.entry_slot])) {
        set_error(err, "em_loader: format: entry_slot does not name a function");
        return false;
    }

    mod.dispatch_size = function_count == 0
        ? 0
        : std::max<size_t>(static_cast<size_t>(max_slot) + 1, function_count);
    return true;
}

// The post-file-read tail of `load_em_file_impl`: parse the in-memory buffer,
// verify the signature (v4), stage + publish the exec pages. Shared by the
// file-path entry (`load_em_file_impl` reads the file then calls this) and the
// byte-buffer entry (`load_em_bytes_impl` takes the buffer directly). The
// `file` vector is the full .em content (the signed-payload bytes are its
// leading `parsed.sig_payload_len` bytes, which parse_file cross-checked).
bool load_em_bytes_impl(const std::vector<uint8_t>& file, LoadedModule& out,
                        std::string* err, ModuleRegistry* registry,
                        const std::unordered_map<std::string, NativeSig>* natives,
                        const EmVerifyPolicy* verify,
                        const EmLoadPolicy* load_policy) {
    // EmLoadPolicy: null == the SECURE DEFAULT (module_permissions = 0,
    // allow_raw_x86 = false). Resolve the effective values once so every
    // check below uses the same policy.
    const uint32_t module_permissions = load_policy ? load_policy->module_permissions : 0u;
    const bool allow_raw_x86 = load_policy ? load_policy->allow_raw_x86 : false;

    ParsedModule parsed;
    if (!parse_file(file, parsed, registry, natives, module_permissions, err)) return false;

    // FIX 3 (EM_FORMAT_RED_TEAM 2026-07-11): reject raw-x86 formats (v1-v4)
    // by default. v1-v4 store raw x86 machine code — an arbitrary-code-
    // execution surface by construction (the loader maps the bytes executable
    // with no validation). Only v5 (IR, re-emitted through emit_x64 + the
    // structural validator) is accepted by default. A host that needs to load
    // existing v1-v4 artifacts passes EmLoadPolicy{allow_raw_x86=true} for
    // back-compat. This check runs BEFORE the signature/dev-mode policy so a
    // raw-x86 module is rejected regardless of its signature status.
    if (parsed.version != EM_VERSION_V5 && !allow_raw_x86) {
        set_error(err, "em_loader: format: raw x86 format v" +
                       std::to_string(parsed.version) +
                       " rejected by default (only v5 IR accepted); " +
                       "pass EmLoadPolicy{allow_raw_x86=true} for back-compat");
        return false;
    }

    // v5 mixed-mode secure default (EM_FORMAT_RED_TEAM 2026-07-11, D8
    // follow-up / MAINTENANCE_LOG 2026-07-12 candidate #1): a v5 module may
    // MIX IR and raw-x86 functions per-function (the on-disk is_ir byte). The
    // IR functions (is_ir=1) are re-emitted from validated IR (safe). The
    // raw-x86 fallback functions (is_ir=0) are an arbitrary-code-execution
    // surface by construction — exactly the surface FIX 3 rejects for v1-v4.
    // The secure default therefore rejects a v5 module that contains ANY
    // raw-x86 function, BEFORE any executable allocation and BEFORE the
    // signature/dev-mode policy, so a raw-x86-bearing v5 module is rejected
    // regardless of its (unsigned, for Stage B) signature status. Only an
    // all-IR v5 module is accepted by the secure default. A host that needs
    // to load mixed/raw v5 artifacts passes EmLoadPolicy{allow_raw_x86=true}
    // for back-compat (the explicit opt-in; under it the raw-x86 functions
    // load as raw x86 and the IR functions re-emit as usual). The explicit
    // ParsedFn::is_ir marker is the discriminator (not ir_blob.empty()) so
    // the gate does not lean on an empty-blob side effect.
    if (parsed.version == EM_VERSION_V5 && !allow_raw_x86) {
        for (const auto& pf : parsed.functions) {
            if (!pf.is_ir) {
                set_error(err, "em_loader: format: v5 function \"" + pf.name +
                               "\" ships raw x86 (is_ir=0), rejected by default " +
                               "(only v5 IR functions accepted); pass " +
                               "EmLoadPolicy{allow_raw_x86=true} for back-compat");
                return false;
            }
        }
    }

    // F2 (docs/spec/SPEC_AUDIT_2026-07-10.md F2): verify the .em CONTENT
    // authentication BEFORE any executable page is allocated. This is the
    // load-bearing security fix — a maliciously-modified `.em` is rejected
    // here, not executed. parse_file already cross-checked the v4 payload
    // length against the name-directory end; here we do the cryptographic
    // verify against the host's trusted keyring and the dev/signed-only policy.
    const bool signed_only = verify && verify->signed_only();
    if (parsed.version == EM_VERSION) {
        // v4: signed module. Requires a verification key.
        if (!signed_only) {
            set_error(err, "em_loader: signature: v4 module requires a verification key; host provided none (dev mode cannot run signed code unverified)");
            return false;
        }
        // Find the trusted key matching the module's pubkey_id. A v4 module
        // with an untrusted key is rejected with a clear error rather than a
        // generic verify-failed (so a host can tell "wrong keychain" from
        // "tampered content").
        const ed25519::PubKey* trusted = nullptr;
        for (const auto& k : verify->trusted_keys) {
            if (std::memcmp(k.data(), parsed.pubkey_id.data(), EM_SIG_PUBKEY_SIZE) == 0) {
                trusted = &k;
                break;
            }
        }
        if (!trusted) {
            set_error(err, "em_loader: signature: module signed by a key not in the host's trusted keyring");
            return false;
        }
        // Verify the Ed25519 signature over the content bytes (0 .. payload_len).
        // `file` holds the full file; the signed payload is the leading
        // `sig_payload_len` bytes (header -> name directory), which parse_file
        // cross-checked against the parser position.
        if (!ed25519::verify(parsed.signature, file.data(), parsed.sig_payload_len, *trusted)) {
            set_error(err, "em_loader: signature: Ed25519 verification FAILED (module content does not match its signature; possible tampering)");
            return false;
        }
    } else {
        // v1/v2/v3: unsigned module. In signed-only mode the host mandated
        // signed modules, so reject. In dev mode (no keys) accept (the
        // development convenience the audit names).
        if (signed_only) {
            set_error(err, "em_loader: signature: host mandates signed modules; this is an unsigned v" +
                           std::to_string(parsed.version) + " module");
            return false;
        }
    }

    // All structure, all functions, and all relocations are validated above.
    // Only now establish the stable dispatch/globals backing stores and begin
    // executable allocation. They never resize after an address is patched.
    StagedModule staged;
    staged.dispatch.assign(parsed.dispatch_size, nullptr);
    staged.globals = std::move(parsed.globals);
    staged.names = std::move(parsed.names);
    staged.entry_slot = parsed.entry_slot;
    staged.version = parsed.version;
    if(parsed.version >= EM_VERSION_V2) {
        staged.signatures_by_slot.resize(parsed.dispatch_size);
        for(const auto& f:parsed.functions)
            staged.signatures_by_slot[f.slot_index]=f.signature;
    }
    staged.pages.reserve(parsed.functions.size());
    std::vector<void*> entries(parsed.functions.size(), nullptr);

    // v5 (Stage B): for IR functions (ir_blob non-empty), deserialize +
    // validate + re-emit to x64 HERE, BEFORE any alloc_executable_rw. This is
    // the v5 SECURITY MODEL: a tampered/malformed v5 .em is REJECTED at IR
    // validation with NO executable page allocated. The re-emitted code/rodata/
    // relocs/native_bindings replace the ParsedFn's empty fields so the
    // existing exec-page loop below handles them uniformly.
    if (parsed.version == EM_VERSION_V5) {
        // Build the load-time CodeGenCtx for re-emit. The dispatch + globals
        // bases are the staged backing stores (stable — never resize after
        // an address is patched). natives is the host table. script_slots is
        // built from the parsed functions. structs is empty (the passing IR
        // cases — scalar/control-flow/calls — don't need struct layouts; the
        // known-gap cases ship raw-x86 fallback via is_ir=0).
        std::unordered_map<std::string, int> slot_map;
        for (const auto& pf : parsed.functions)
            slot_map[pf.name] = int(pf.slot_index);
        StructLayoutTable empty_structs;
        CodeGenCtx ictx;
        ictx.dispatch_base = reinterpret_cast<int64_t>(staged.dispatch.data());
        ictx.globals_base  = reinterpret_cast<int64_t>(staged.globals.data());
        ictx.natives = natives;
        ictx.script_slots = &slot_map;
        ictx.structs = &empty_structs;
        ictx.enable_ir_backend = true;  // emit_x64 is the IR-path emitter
        // Never strip serialized budget/depth guard instructions while
        // re-emitting v5 IR. Hosts that provide runtime guard storage get the
        // same checks as the original JIT compilation; absent storage remains
        // the documented no-op rather than silently changing the IR policy.
        ictx.safe_defaults();

        for (auto& pf : parsed.functions) {
            if (!pf.is_ir) continue;  // raw-x86 fallback — skip (re-emit IR only)
            ThinFunction thf;
            const uint8_t* cur = pf.ir_blob.data();
            const uint8_t* end = pf.ir_blob.data() + pf.ir_blob.size();
            std::string derr;
            if (!deserialize_thin_function(cur, end, pf.name,
                                           int32_t(pf.slot_index), thf, &derr)) {
                set_error(err, "em_loader: v5 IR: deserialization failed for \"" +
                               pf.name + "\": " + derr);
                return false;
            }
            // Efficiency: the blob is consumed by deserialize; clear it now to
            // free memory before the re-emit (the blob is never read again).
            pf.ir_blob.clear();
            pf.ir_blob.shrink_to_fit();
            // Native rebind: for every CallNative instr, look up
            // meta.native_name in the host table and set native_fn. An IR
            // module referencing a native the host didn't register is
            // REJECTED — the core v5 security gate (no exec page allocated).
            // Finding B (EM_FORMAT_RED_TEAM 2026-07-11): also enforce
            // PERM_FFI at rebind — a native flagged PERM_FFI is rejected if
            // the loading module's permissions lack the FFI bit. This mirrors
            // the v2-v4 check in parse_file and sema's compile-time gate.
            if (natives) {
                for (auto& blk : thf.blocks) {
                    for (auto& in : blk.instrs) {
                        if (in.op == ThinOp::CallNative && !in.meta.native_name.empty()) {
                            auto it = natives->find(in.meta.native_name);
                            if (it == natives->end() || !it->second.fn_ptr) {
                                set_error(err, "em_loader: v5 IR: unknown native \"" +
                                               in.meta.native_name + "\" in \"" +
                                               pf.name + "\"");
                                return false;
                            }
                            if ((it->second.permission & PERM_FFI) && !(module_permissions & PERM_FFI)) {
                                set_error(err, "em_loader: v5 IR: native \"" +
                                               in.meta.native_name + "\" in \"" +
                                               pf.name + "\" requires PERM_FFI permission (module lacks it)");
                                return false;
                            }
                            in.native_fn = it->second.fn_ptr;
                        }
                    }
                }
            } else {
                // No host native table provided. An IR function with any
                // CallNative instr is REJECTED — the native_fn would stay
                // null and the generated x64 would call through null. This
                // scan uses thf.blocks (NOT pf.ir_blob, which was cleared
                // above for efficiency) so the guard is NOT dead code.
                bool has_native_call = false;
                for (const auto& blk : thf.blocks)
                    for (const auto& in : blk.instrs)
                        if (in.op == ThinOp::CallNative) { has_native_call = true; break; }
                if (has_native_call) {
                    set_error(err, "em_loader: v5 IR: function \"" + pf.name +
                                   "\" calls a native but host provided no native table");
                    return false;
                }
            }
            // Semantic validation (block id bounds, VReg bounds, block-target
            // bounds, rodata bounds, CallScript/CallCrossModule slot range,
            // Cmp predicate range, CallNative non-empty name, frame plan sanity).
            // Pass dispatch_size and registry_size so CallScript/CallCrossModule
            // slots can be range-checked against the host's tables.
            std::string verr;
            if (!validate_thin_function(thf, &verr, uint32_t(staged.dispatch.size()),
                                        registry ? registry->count() : 0u)) {
                set_error(err, "em_loader: v5 IR: validation failed for \"" +
                               pf.name + "\": " + verr);
                return false;
            }
            // Re-emit the deserialized IR to x64.
            CompiledFn cf = emit_x64(thf, ictx);
            if (cf.bytes.empty()) {
                set_error(err, "em_loader: v5 IR: re-emit produced empty code for \"" +
                               pf.name + "\"");
                return false;
            }
            // Replace the ParsedFn's fields with the re-emitted code so the
            // existing exec-page loop handles it uniformly.
            pf.code = std::move(cf.bytes);
            pf.rodata = std::move(cf.rodata);
            pf.relocs.clear();
            pf.relocs.reserve(cf.abs_fixups.size());
            for (const auto& af : cf.abs_fixups) {
                EmReloc r;
                r.offset = af.code_offset;
                r.kind = static_cast<uint8_t>(af.kind);
                r.addend = af.addend;
                pf.relocs.push_back(r);
            }
            pf.native_bindings.clear();
            pf.native_bindings.reserve(cf.native_fixups.size());
            for (const auto& nf : cf.native_fixups) {
                EmNativeBinding b;
                b.offset = nf.code_offset;
                b.name = nf.name;
                b.signature.ret = nf.ret;
                b.signature.params = nf.params;
                pf.native_bindings.push_back(std::move(b));
            }
            // pf.ir_blob was already cleared after deserialize (efficiency);
            // no second clear needed.
        }
    }

    for (size_t i = 0; i < parsed.functions.size(); ++i) {
        const ParsedFn& f = parsed.functions[i];
        uint64_t combined_size = 0;
        if (!checked_add(f.code.size(), f.rodata.size(), combined_size)) {
            set_error(err, "em_loader: format: combined function size overflow");
            return false;
        }
        std::vector<uint8_t> combined;
        combined.reserve(static_cast<size_t>(combined_size));
        combined.insert(combined.end(), f.code.begin(), f.code.end());
        combined.insert(combined.end(), f.rodata.begin(), f.rodata.end());

        void* page = alloc_executable_rw(combined); // RW, never RWX
        if (!page) {
            set_error(err, "em_loader: allocation: executable page allocation failed");
            return false;
        }
        staged.pages.push_back(page);
        uint8_t* bytes = static_cast<uint8_t*>(page);
        for (const EmReloc& r : f.relocs) {
            uint64_t address = 0;
            switch (r.kind) {
                case EmReloc::DispatchTableBase:
                    address = reinterpret_cast<uintptr_t>(staged.dispatch.data());
                    break;
                case EmReloc::GlobalsBase:
                    address = reinterpret_cast<uintptr_t>(staged.globals.data());
                    break;
                case EmReloc::ModuleRegistryBase:
                    address = reinterpret_cast<uintptr_t>(registry->base());
                    break;
                case EmReloc::FunctionRodataBase:
                    address = reinterpret_cast<uintptr_t>(bytes + f.code.size() + r.addend);
                    break;
                default: // validated before allocation
                    set_error(err, "em_loader: internal: unvalidated relocation kind");
                    return false;
            }
            write_u64_le(bytes + r.offset, address);
        }
        for(const auto& b:f.native_bindings) {
            const auto it=natives->find(b.name); // validated before allocation
            write_u64_le(bytes+b.offset,reinterpret_cast<uintptr_t>(it->second.fn_ptr));
        }
        if (!seal_executable(page, static_cast<size_t>(combined_size))) {
            set_error(err, "em_loader: allocation: executable page sealing failed");
            return false;
        }
        entries[i] = page;
    }

    // Publish only after every page is patched and RX-sealed.
    for (size_t i = 0; i < parsed.functions.size(); ++i)
        staged.dispatch[parsed.functions[i].slot_index] = entries[i];
    staged.commit(out);
    return true;
}

} // namespace

// ---- internal: file-path entry (reads the file, then delegates to the
// byte-buffer impl above) ----
bool load_em_file_impl(const char* path, LoadedModule& out, std::string* err,
                       ModuleRegistry* registry,
                       const std::unordered_map<std::string, NativeSig>* natives,
                       const EmVerifyPolicy* verify,
                       const EmLoadPolicy* load_policy) {
    if (!path) {
        set_error(err, "em_loader: argument: null path");
        return false;
    }

    std::ifstream ifs(path, std::ios::binary | std::ios::in | std::ios::ate);
    if (!ifs) {
        set_error(err, std::string("em_loader: io: could not open input file: ") + path);
        return false;
    }
    const std::streampos end = ifs.tellg();
    if (end < 0) {
        set_error(err, "em_loader: io: could not determine file length");
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(end);
    if (file_size < EM_HEADER_SIZE) {
        set_error(err, "em_loader: format: file shorter than header");
        return false;
    }
    if (file_size > MAX_FILE_SIZE) {
        set_error(err, "em_loader: limit: file exceeds MAX_FILE_SIZE");
        return false;
    }
    ifs.seekg(0, std::ios::beg);
    if (!ifs) {
        set_error(err, "em_loader: io: seek failed");
        return false;
    }
    std::vector<uint8_t> file(static_cast<size_t>(file_size));
    if (!ifs.read(reinterpret_cast<char*>(file.data()),
                  static_cast<std::streamsize>(file.size()))) {
        set_error(err, "em_loader: io: short file read");
        return false;
    }
    return load_em_bytes_impl(file, out, err, registry, natives, verify, load_policy);
}

LoadedModule::~LoadedModule() {
    for (void* p : pages) free_executable(p);
}

LoadedModule::LoadedModule(LoadedModule&& other) noexcept
    : dispatch(std::move(other.dispatch)), globals(std::move(other.globals)),
      pages(std::move(other.pages)), name_table(std::move(other.name_table)),
      entry_slot(other.entry_slot), format_version(other.format_version),
      signatures_by_slot(std::move(other.signatures_by_slot)) {
    other.pages.clear();
    other.entry_slot = EM_NO_ENTRY;
}

LoadedModule& LoadedModule::operator=(LoadedModule&& other) noexcept {
    if (this == &other) return *this;
    for (void* p : pages) free_executable(p);
    dispatch = std::move(other.dispatch);
    globals = std::move(other.globals);
    pages = std::move(other.pages);
    name_table = std::move(other.name_table);
    entry_slot = other.entry_slot;
    format_version = other.format_version;
    signatures_by_slot = std::move(other.signatures_by_slot);
    other.pages.clear();
    other.entry_slot = EM_NO_ENTRY;
    return *this;
}

void* LoadedModule::entry_by_name(const char* name) const {
    if (!name) return nullptr;
    for (const auto& item : name_table) {
        if (item.first == name)
            return item.second < dispatch.size() ? dispatch[item.second] : nullptr;
    }
    return nullptr;
}

void* LoadedModule::entry() const {
    if (entry_slot == EM_NO_ENTRY || entry_slot >= dispatch.size()) return nullptr;
    return dispatch[entry_slot];
}

bool load_em_file(const char* path, LoadedModule& out, std::string* err,
                  ModuleRegistry* registry,
                  const std::unordered_map<std::string, NativeSig>* native_bindings,
                  const EmVerifyPolicy* verify,
                  const EmLoadPolicy* load_policy) {
    // Complete public no-throw boundary: malformed input and allocation/library
    // failures are always reported as false plus a categorized error.
    try {
        return load_em_file_impl(path, out, err, registry, native_bindings, verify, load_policy);
    } catch (const std::bad_alloc&) {
        set_error(err, "em_loader: allocation: std::bad_alloc");
    } catch (const std::length_error&) {
        set_error(err, "em_loader: allocation: std::length_error");
    } catch (const std::exception&) {
        set_error(err, "em_loader: exception: std::exception");
    } catch (...) {
        set_error(err, "em_loader: exception: unknown failure");
    }
    return false;
}

bool load_em_bytes(const uint8_t* data, size_t len, LoadedModule& out,
                   std::string* err,
                   ModuleRegistry* registry,
                   const std::unordered_map<std::string, NativeSig>* native_bindings,
                   const EmVerifyPolicy* verify,
                   const EmLoadPolicy* load_policy) {
    // Complete public no-throw boundary: malformed input and allocation/library
    // failures are always reported as false plus a categorized error.
    try {
        if (!data) {
            set_error(err, "em_loader: argument: null data");
            return false;
        }
        if (len < EM_HEADER_SIZE) {
            set_error(err, "em_loader: format: buffer shorter than header");
            return false;
        }
        if (len > MAX_FILE_SIZE) {
            set_error(err, "em_loader: limit: buffer exceeds MAX_FILE_SIZE");
            return false;
        }
        std::vector<uint8_t> file(data, data + len);
        return load_em_bytes_impl(file, out, err, registry, native_bindings, verify, load_policy);
    } catch (const std::bad_alloc&) {
        set_error(err, "em_loader: allocation: std::bad_alloc");
    } catch (const std::length_error&) {
        set_error(err, "em_loader: allocation: std::length_error");
    } catch (const std::exception&) {
        set_error(err, "em_loader: exception: std::exception");
    } catch (...) {
        set_error(err, "em_loader: exception: unknown failure");
    }
    return false;
}

} // namespace ember
