// ember `.em` loader impl (docs/BUNDLING_AND_EM_MODULES.md Section 2.5)
//
// v1 `.em` contains native x86-64. In addition to the three relocations the
// format records, generated code may contain process-local native/trap/
// allowlist/string pointers. Loading is therefore an ABI/process-trusted
// operation, not validation of an untrusted or portable code container.

#include "em_loader.hpp"

#include <algorithm>
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
};

struct ParsedModule {
    std::vector<ParsedFn> functions;
    std::vector<uint8_t> globals;
    std::vector<std::pair<std::string, uint32_t>> names;
    uint32_t entry_slot = EM_NO_ENTRY;
    size_t dispatch_size = 0;
    uint32_t version = EM_VERSION_V1;
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

// Validate the on-disk canonical-type shape BEFORE assigning any field or
// allocating an element. A hand-built Type whose prim/flags/array_len/name are
// internally inconsistent must be rejected here, not published as an exec
// page. The writer derives every flag bit from the Type's fields; the loader
// does not consume bits 1 (array) or 2 (struct) for its own Type construction,
// but it enforces that they agree with array_len/name_len so a malformed v2
// artifact cannot smuggle an inconsistent Type past the signature check.
bool parse_type(Reader& rd, Type& t, unsigned depth, std::string* err) {
    if (depth > 16) { set_error(err, "em_loader: limit: type nesting too deep"); return false; }
    uint8_t prim=0, flags=0; uint32_t array_len=0; uint16_t name_len=0;
    if(!rd.u8(prim)||!rd.u8(flags)||!rd.u32(array_len)||!rd.u16(name_len)) { set_error(err,"em_loader: format: truncated canonical type"); return false; }
    if(prim>static_cast<uint8_t>(Prim::F64)||(flags&~uint8_t(31))||name_len>MAX_NAME_SIZE) { set_error(err,"em_loader: format: invalid canonical type"); return false; }
    const uint8_t* name=nullptr; if(!rd.take(name_len,name)) { set_error(err,"em_loader: format: truncated type name"); return false; }

    const bool is_slice  = (flags & 1) != 0;
    const bool is_array  = (flags & 2) != 0;
    const bool is_struct = (flags & 4) != 0;
    const bool is_fn     = (flags & 8) != 0;
    const bool has_sig   = (flags & 16) != 0;

    // (a) struct flag iff name nonempty.
    if (is_struct != (name_len > 0)) { set_error(err,"em_loader: format: inconsistent canonical type: struct flag does not match name"); return false; }
    // (e) array flag iff nonzero length; slice (bit 0) and array (bit 1) are
    // mutually exclusive — a slice carries array_len==0, an array carries
    // is_slice==false and array_len>0.
    if (is_array != (array_len != 0)) { set_error(err,"em_loader: format: inconsistent canonical type: array flag does not match array_len"); return false; }
    if (is_slice && array_len) { set_error(err,"em_loader: format: type is both slice and array"); return false; }
    // (b) a function handle is an i64 slot; is_fn_handle requires prim==I64.
    if (is_fn && prim != static_cast<uint8_t>(Prim::I64)) { set_error(err,"em_loader: format: inconsistent canonical type: function handle requires Prim::I64"); return false; }
    // (c)+(d) a recorded signature is only valid on a function handle.
    if (has_sig && !is_fn) { set_error(err,"em_loader: format: inconsistent canonical type: recorded signature requires function handle"); return false; }
    // (g) a slice/array is characterized by its element, not its own prim;
    // its prim must be Void (the value is `elem[]` / `elem[N]`). A struct
    // name does NOT constrain prim: a script struct has Prim::Void while a
    // host handle (`bind_handle`) is Prim::I64 with a struct-name tag, so
    // both are valid.
    if ((is_slice || array_len) && prim != static_cast<uint8_t>(Prim::Void)) { set_error(err,"em_loader: format: inconsistent canonical type: slice/array must have Prim::Void"); return false; }

    t.prim=static_cast<Prim>(prim); t.is_slice=is_slice; t.array_len=array_len;
    t.struct_name.assign(reinterpret_cast<const char*>(name),name_len); t.is_fn_handle=is_fn; t.has_recorded_sig=has_sig;
    if(t.is_slice||t.array_len) { t.elem=std::make_shared<Type>(); if(!parse_type(rd,*t.elem,depth+1,err))return false; }
    if(t.is_fn_handle&&t.has_recorded_sig) { uint32_t n=0; if(!rd.u32(n)||n>1024){set_error(err,"em_loader: limit: function type parameter count");return false;} for(uint32_t i=0;i<n;++i){auto p=std::make_shared<Type>();if(!parse_type(rd,*p,depth+1,err))return false;t.recorded_params.push_back(std::move(p));}t.recorded_ret=std::make_shared<Type>();if(!parse_type(rd,*t.recorded_ret,depth+1,err))return false; }
    return true;
}

bool canonical_type_same(const Type& a,const Type& b) {
    if(a.prim!=b.prim||a.struct_name!=b.struct_name||a.is_slice!=b.is_slice||a.array_len!=b.array_len||a.is_fn_handle!=b.is_fn_handle||a.has_recorded_sig!=b.has_recorded_sig)return false;
    if(bool(a.elem)!=bool(b.elem)||(a.elem&&!canonical_type_same(*a.elem,*b.elem)))return false;
    if(a.recorded_params.size()!=b.recorded_params.size()||bool(a.recorded_ret)!=bool(b.recorded_ret))return false;
    for(size_t i=0;i<a.recorded_params.size();++i)if(bool(a.recorded_params[i])!=bool(b.recorded_params[i])||(a.recorded_params[i]&&!canonical_type_same(*a.recorded_params[i],*b.recorded_params[i])))return false;
    return !a.recorded_ret||canonical_type_same(*a.recorded_ret,*b.recorded_ret);
}

bool parse_signature(Reader& rd, EmSignature& sig, std::string* err) {
    if(!parse_type(rd,sig.ret,0,err))return false;
    uint32_t n=0;
    if(!rd.u32(n)||n>1024){set_error(err,"em_loader: limit: signature parameter count");return false;}
    sig.params.resize(n); for(Type& t:sig.params)if(!parse_type(rd,t,0,err))return false; return true;
}

bool parse_file(const std::vector<uint8_t>& file, ParsedModule& mod,
                ModuleRegistry* registry,
                const std::unordered_map<std::string, NativeSig>* natives,
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
    if (version != EM_VERSION_V1 && version != EM_VERSION) {
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
        if (!rd.u32(f.slot_index) || !rd.u32(code_size) || !rd.u32(rodata_size)) {
            set_error(err, "em_loader: format: truncated function metadata");
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
            if (!rd.u32(r.offset) || !rd.u8(r.kind) || (version == EM_VERSION && !rd.u32(r.addend))) {
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
        if(version==EM_VERSION) {
            if(!parse_signature(rd,f.signature,err))return false;
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

    if (rd.remaining() != 0) {
        set_error(err, "em_loader: format: trailing bytes after name directory");
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

bool load_em_file_impl(const char* path, LoadedModule& out, std::string* err,
                       ModuleRegistry* registry,
                       const std::unordered_map<std::string, NativeSig>* natives) {
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

    ParsedModule parsed;
    if (!parse_file(file, parsed, registry, natives, err)) return false;

    // All structure, all functions, and all relocations are validated above.
    // Only now establish the stable dispatch/globals backing stores and begin
    // executable allocation. They never resize after an address is patched.
    StagedModule staged;
    staged.dispatch.assign(parsed.dispatch_size, nullptr);
    staged.globals = std::move(parsed.globals);
    staged.names = std::move(parsed.names);
    staged.entry_slot = parsed.entry_slot;
    staged.version = parsed.version;
    if(parsed.version==EM_VERSION) {
        staged.signatures_by_slot.resize(parsed.dispatch_size);
        for(const auto& f:parsed.functions)
            staged.signatures_by_slot[f.slot_index]=f.signature;
    }
    staged.pages.reserve(parsed.functions.size());
    std::vector<void*> entries(parsed.functions.size(), nullptr);

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
                  const std::unordered_map<std::string, NativeSig>* native_bindings) {
    // Complete public no-throw boundary: malformed input and allocation/library
    // failures are always reported as false plus a categorized error.
    try {
        return load_em_file_impl(path, out, err, registry, native_bindings);
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
