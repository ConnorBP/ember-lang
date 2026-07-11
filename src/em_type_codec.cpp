// ember `.em` canonical-type codec — shared implementation.
// See em_type_codec.hpp for the format + invariant documentation. This TU is
// the single source of truth for the Type/EmSignature on-disk encoding; the
// writer (em_writer.cpp) and loader (em_loader.cpp) both call into here.
//
// The writer-side helpers (emit_type / emit_signature / validate_canonical_type
// / validate_signature) are lifted verbatim from the historical em_writer
// anonymous namespace. The reader-side helpers (parse_type /
// canonical_type_same / parse_signature) are lifted verbatim from the
// historical em_loader anonymous namespace, with the only structural change
// being that parse_type / parse_signature take a raw (cur, end) cursor pair
// instead of the loader-private Reader struct — the loader adapts its Reader
// calls at the ~3 call sites (see em_loader.cpp). On-disk byte order,
// flag-bit layout, validation rule order, and all decisions are byte-identical
// to the pre-refactor implementations.

#include "em_type_codec.hpp"

#include <cstdint>
#include <ostream>
#include <string>

namespace ember {

namespace {

// ---- little-endian emitters (match the emitter's imm32/imm64 style) ----
// Write the low byte first; each shift picks the next byte up.

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

void emit_string(std::ostream& ofs, const std::string& s) {
    // name_len is u16; names are expected short. Caller is responsible for
    // not exceeding u16 range; the writer caps defensively at the call sites.
    if (!s.empty())
        ofs.write(reinterpret_cast<const char*>(s.data()),
                  static_cast<std::streamsize>(s.size()));
}

// ---- cursor helpers (reader side) ----
//
// The loader's Reader struct wraps a vector + position; here we work directly
// off a (cur, end) pointer pair so the codec is self-contained and the loader
// can adapt its Reader calls trivially.

// Read n bytes; on success advance cur and set *out to the read pointer.
bool take(const uint8_t*& cur, const uint8_t* end, uint64_t n,
          const uint8_t*& out) {
    // n + cur must not overflow / exceed end. cur <= end by contract.
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
    v = static_cast<uint16_t>(p[0]) |
        static_cast<uint16_t>(p[1]) << 8;
    return true;
}

bool read_u32(const uint8_t*& cur, const uint8_t* end, uint32_t& v) {
    const uint8_t* p = nullptr;
    if (!take(cur, end, 4, p)) return false;
    v = static_cast<uint32_t>(p[0]) |
        static_cast<uint32_t>(p[1]) << 8 |
        static_cast<uint32_t>(p[2]) << 16 |
        static_cast<uint32_t>(p[3]) << 24;
    return true;
}

} // namespace

// ---- writer side ----

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

// Mirror of the loader's parse_type shape validation. A hand-built Type whose
// prim/flags-equivalent fields are internally inconsistent must be rejected at
// write time, not serialized for the loader to catch. This is the write-side
// half of the M-H14-1 canonical-type consistency gate; the loader enforces the
// same invariants against the on-disk flags/len fields before any allocation.
bool validate_canonical_type(const Type& t, std::string* err, unsigned depth) {
    if (depth > 16) { if (err) *err = "em_type_codec: type nesting too deep"; return false; }
    // The writer derives flag bits from exactly these fields; a Type that does
    // not match the derivation would serialize a flag the loader would reject.
    const bool is_array  = t.array_len != 0;
    // A slice/array is characterized by its element; its own prim must be Void.
    // A struct name does NOT constrain prim: a script struct has Prim::Void
    // while a host handle (`bind_handle`) is Prim::I64 with a struct-name tag.
    if ((t.is_slice || is_array) && t.prim != Prim::Void) { if (err) *err = "em_type_codec: inconsistent canonical type: slice/array must have Prim::Void"; return false; }
    if (t.is_slice && is_array) { if (err) *err = "em_type_codec: inconsistent canonical type: type is both slice and array"; return false; }
    if (t.is_slice && !t.elem) { if (err) *err = "em_type_codec: inconsistent canonical type: slice requires an element type"; return false; }
    if (is_array && !t.elem) { if (err) *err = "em_type_codec: inconsistent canonical type: fixed array requires an element type"; return false; }
    if (t.is_fn_handle && t.prim != Prim::I64) { if (err) *err = "em_type_codec: inconsistent canonical type: function handle requires Prim::I64"; return false; }
    if (t.has_recorded_sig && !t.is_fn_handle) { if (err) *err = "em_type_codec: inconsistent canonical type: recorded signature requires function handle"; return false; }
    if (t.elem && !validate_canonical_type(*t.elem, err, depth + 1)) return false;
    if (t.is_fn_handle && t.has_recorded_sig) {
        if (t.recorded_params.size() > 1024) { if (err) *err = "em_type_codec: inconsistent canonical type: function type parameter count"; return false; }
        for (const auto& p : t.recorded_params)
            if (!p || !validate_canonical_type(*p, err, depth + 1)) { if (err) *err = "em_type_codec: inconsistent canonical type: recorded parameter missing"; return false; }
        if (!t.recorded_ret || !validate_canonical_type(*t.recorded_ret, err, depth + 1)) { if (err) *err = "em_type_codec: inconsistent canonical type: recorded return type missing"; return false; }
    }
    return true;
}

bool validate_signature(const EmSignature& sig, std::string* err) {
    if (sig.params.size() > 1024) { if (err) *err = "em_type_codec: signature parameter count"; return false; }
    if (!validate_canonical_type(sig.ret, err)) return false;
    for (const Type& p : sig.params) if (!validate_canonical_type(p, err)) return false;
    return true;
}

// ---- reader side ----

// Validate the on-disk canonical-type shape BEFORE assigning any field or
// allocating an element. A hand-built Type whose prim/flags/array_len/name are
// internally inconsistent must be rejected here, not published as an exec
// page. The writer derives every flag bit from the Type's fields; the loader
// does not consume bits 1 (array) or 2 (struct) for its own Type construction,
// but it enforces that they agree with array_len/name_len so a malformed v2
// artifact cannot smuggle an inconsistent Type past the signature check.
bool parse_type(const uint8_t*& cur, const uint8_t* end, Type& t,
                unsigned depth, std::string* err) {
    if (depth > 16) { if (err) *err = "em_type_codec: limit: type nesting too deep"; return false; }
    uint8_t prim=0, flags=0; uint32_t array_len=0; uint16_t name_len=0;
    if(!read_u8(cur,end,prim)||!read_u8(cur,end,flags)||!read_u32(cur,end,array_len)||!read_u16(cur,end,name_len)) { if (err) *err = "em_type_codec: format: truncated canonical type"; return false; }
    if(prim>static_cast<uint8_t>(Prim::F64)||(flags&~uint8_t(31))||name_len>MAX_NAME_SIZE) { if (err) *err = "em_type_codec: format: invalid canonical type"; return false; }
    const uint8_t* name=nullptr; if(!take(cur,end,name_len,name)) { if (err) *err = "em_type_codec: format: truncated type name"; return false; }

    const bool is_slice  = (flags & 1) != 0;
    const bool is_array  = (flags & 2) != 0;
    const bool is_struct = (flags & 4) != 0;
    const bool is_fn     = (flags & 8) != 0;
    const bool has_sig   = (flags & 16) != 0;

    // (a) struct flag iff name nonempty.
    if (is_struct != (name_len > 0)) { if (err) *err = "em_type_codec: format: inconsistent canonical type: struct flag does not match name"; return false; }
    // (e) array flag iff nonzero length; slice (bit 0) and array (bit 1) are
    // mutually exclusive — a slice carries array_len==0, an array carries
    // is_slice==false and array_len>0.
    if (is_array != (array_len != 0)) { if (err) *err = "em_type_codec: format: inconsistent canonical type: array flag does not match array_len"; return false; }
    if (is_slice && array_len) { if (err) *err = "em_type_codec: format: type is both slice and array"; return false; }
    // (b) a function handle is an i64 slot; is_fn_handle requires prim==I64.
    if (is_fn && prim != static_cast<uint8_t>(Prim::I64)) { if (err) *err = "em_type_codec: format: inconsistent canonical type: function handle requires Prim::I64"; return false; }
    // (c)+(d) a recorded signature is only valid on a function handle.
    if (has_sig && !is_fn) { if (err) *err = "em_type_codec: format: inconsistent canonical type: recorded signature requires function handle"; return false; }
    // (g) a slice/array is characterized by its element, not its own prim;
    // its prim must be Void (the value is `elem[]` / `elem[N]`). A struct
    // name does NOT constrain prim: a script struct has Prim::Void while a
    // host handle (`bind_handle`) is Prim::I64 with a struct-name tag, so
    // both are valid.
    if ((is_slice || array_len) && prim != static_cast<uint8_t>(Prim::Void)) { if (err) *err = "em_type_codec: format: inconsistent canonical type: slice/array must have Prim::Void"; return false; }

    t.prim=static_cast<Prim>(prim); t.is_slice=is_slice; t.array_len=array_len;
    t.struct_name.assign(reinterpret_cast<const char*>(name),name_len); t.is_fn_handle=is_fn; t.has_recorded_sig=has_sig;
    if(t.is_slice||t.array_len) { t.elem=std::make_shared<Type>(); if(!parse_type(cur,end,*t.elem,depth+1,err))return false; }
    if(t.is_fn_handle&&t.has_recorded_sig) { uint32_t n=0; if(!read_u32(cur,end,n)||n>1024){if(err)*err="em_type_codec: limit: function type parameter count";return false;} for(uint32_t i=0;i<n;++i){auto p=std::make_shared<Type>();if(!parse_type(cur,end,*p,depth+1,err))return false;t.recorded_params.push_back(std::move(p));}t.recorded_ret=std::make_shared<Type>();if(!parse_type(cur,end,*t.recorded_ret,depth+1,err))return false; }
    return true;
}

bool canonical_type_same(const Type& a,const Type& b) {
    if(a.prim!=b.prim||a.struct_name!=b.struct_name||a.is_slice!=b.is_slice||a.array_len!=b.array_len||a.is_fn_handle!=b.is_fn_handle||a.has_recorded_sig!=b.has_recorded_sig)return false;
    if(bool(a.elem)!=bool(b.elem)||(a.elem&&!canonical_type_same(*a.elem,*b.elem)))return false;
    if(a.recorded_params.size()!=b.recorded_params.size()||bool(a.recorded_ret)!=bool(b.recorded_ret))return false;
    for(size_t i=0;i<a.recorded_params.size();++i)if(bool(a.recorded_params[i])!=bool(b.recorded_params[i])||(a.recorded_params[i]&&!canonical_type_same(*a.recorded_params[i],*b.recorded_params[i])))return false;
    return !a.recorded_ret||canonical_type_same(*a.recorded_ret,*b.recorded_ret);
}

bool parse_signature(const uint8_t*& cur, const uint8_t* end, EmSignature& sig,
                     std::string* err) {
    if(!parse_type(cur,end,sig.ret,0,err))return false;
    uint32_t n=0;
    if(!read_u32(cur,end,n)||n>1024){if(err)*err="em_type_codec: limit: signature parameter count";return false;}
    sig.params.resize(n); for(Type& t:sig.params)if(!parse_type(cur,end,t,0,err))return false; return true;
}

} // namespace ember
