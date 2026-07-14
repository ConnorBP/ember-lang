// dispatch_abi.cpp — Red 2 GREEN: stable canonical ABI fingerprints.
//
// Implementation of the pinned byte encoding + FNV-1a 64-bit fingerprint
// documented in dispatch_abi.hpp. Uses ONLY implementation-independent
// integer operations. No std::hash, no Type::to_string, no native struct
// bytes, no pointer values, no unordered-container iteration order.
//
// Design ref: docs/planning/plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §7.

#include "dispatch_abi.hpp"

#include <cstdint>
#include <cstring>

namespace ember {

namespace {

// FNV-1a 64-bit offset basis and prime (pinned constants).
inline constexpr uint64_t kFnv1aOffset = 0xcbf29ce484222325ULL;
inline constexpr uint64_t kFnv1aPrime  = 0x100000001b3ULL;

// The fixed 20-byte domain header (ASCII "Ember.DispatchAbi.v1", no NUL).
// kDispatchAbiDomain is declared in the header; its length is spelled out
// here as a compile-time check.
static_assert(kDispatchAbiDomainLen == 20, "domain header length mismatch");

// FNV-1a 64-bit over a byte range. h starts at the offset basis; per byte
// h = (h ^ byte) * prime, all mod 2^64 (unsigned overflow is well-defined).
inline uint64_t fnv1a64(const uint8_t* data, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= kFnv1aPrime;
    }
    return h;
}

// Append a single byte to the buffer.
inline void put_u8(std::string& out, uint8_t v) {
    out.push_back(static_cast<char>(v));
}

// Append a uint32_t little-endian to the buffer.
inline void put_u32_le(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>(
            static_cast<uint8_t>((v >> (8 * i)) & 0xFF)));
}

// Append a length-prefixed string (u32_le length + raw bytes) to the buffer.
// std::string::size() is a byte count (no NUL); the string is not iterated
// through any unordered container.
inline void put_lpref_str(std::string& out, const std::string& s) {
    put_u32_le(out, static_cast<uint32_t>(s.size()));
    out.append(s.data(), s.size());
}

// Recursive canonical type encoding. See dispatch_abi.hpp for the grammar.
void encode_type(std::string& out, const AbiType& t) {
    put_u8(out, static_cast<uint8_t>(t.kind));
    switch (t.kind) {
    case TypeKind::Scalar:
        put_u8(out, static_cast<uint8_t>(t.prim));
        break;
    case TypeKind::OpaqueHandle:
        // A nominal opaque handle: carried as an i64 GP word but tagged with
        // a nominal identity + byte size so it is distinct from Scalar(I64).
        put_lpref_str(out, t.struct_name);
        put_u32_le(out, t.handle_byte_size);
        break;
    case TypeKind::Struct:
        put_lpref_str(out, t.struct_name);
        put_u32_le(out, t.struct_size);
        put_u32_le(out, t.struct_alignment);
        put_u32_le(out, static_cast<uint32_t>(t.fields.size()));
        for (const auto& f : t.fields) {
            put_lpref_str(out, f.name);
            put_u32_le(out, f.offset);
            // A null field type encodes as Scalar(Void) — a defensive default
            // that keeps the encoding total even if a builder forgot to set it.
            encode_type(out, f.type ? *f.type : abi_scalar(PrimTag::Void));
        }
        break;
    case TypeKind::Slice:
        encode_type(out, t.elem ? *t.elem : abi_scalar(PrimTag::Void));
        break;
    case TypeKind::Lambda:
        put_u8(out, t.has_recorded_sig ? 1 : 0);
        put_u32_le(out, static_cast<uint32_t>(t.recorded_params.size()));
        for (const auto& p : t.recorded_params)
            encode_type(out, p ? *p : abi_scalar(PrimTag::Void));
        encode_type(out, t.recorded_ret ? *t.recorded_ret : abi_scalar(PrimTag::Void));
        break;
    case TypeKind::FnHandle:
        put_lpref_str(out, t.struct_name);
        put_u8(out, t.is_cross_module_handle ? 1 : 0);
        put_u8(out, t.has_recorded_sig ? 1 : 0);
        put_u32_le(out, static_cast<uint32_t>(t.recorded_params.size()));
        for (const auto& p : t.recorded_params)
            encode_type(out, p ? *p : abi_scalar(PrimTag::Void));
        encode_type(out, t.recorded_ret ? *t.recorded_ret : abi_scalar(PrimTag::Void));
        break;
    case TypeKind::Array:
        put_u32_le(out, t.array_len);
        encode_type(out, t.elem ? *t.elem : abi_scalar(PrimTag::Void));
        break;
    }
}

} // namespace

WordClass word_class_for_type(const AbiType& t) {
    if (t.kind == TypeKind::Scalar) {
        if (t.prim == PrimTag::F32) return WordClass::XmmF32;
        if (t.prim == PrimTag::F64) return WordClass::XmmF64;
        return WordClass::GP;
    }
    if (t.kind == TypeKind::OpaqueHandle) return WordClass::GP;
    // Slices, lambdas, structs, fn handles, arrays: all words are GP.
    return WordClass::GP;
}

std::string encode_callable(const CallableDescriptor& d) {
    std::string out;
    out.reserve(128);
    // Fixed domain header (no length prefix — it is a constant).
    out.append(kDispatchAbiDomain, kDispatchAbiDomainLen);
    // Fixed descriptor fields.
    put_u8(out, static_cast<uint8_t>(d.arch));
    put_u32_le(out, d.convention_version);
    put_u8(out, static_cast<uint8_t>(d.calling_mode));
    put_u8(out, static_cast<uint8_t>(d.visibility));
    put_u8(out, static_cast<uint8_t>(d.return_kind));
    // Full recursive return type.
    encode_type(out, d.return_type);
    // Hidden words + special entry flags.
    put_u8(out, d.has_hidden_return ? 1 : 0);
    put_u8(out, d.has_hidden_env ? 1 : 0);
    put_u8(out, d.is_coroutine ? 1 : 0);
    put_u8(out, d.is_special_entry ? 1 : 0);
    // Parameters in order.
    put_u32_le(out, static_cast<uint32_t>(d.params.size()));
    for (const auto& p : d.params) {
        encode_type(out, p.type);
        put_u32_le(out, p.word_count);
        put_u32_le(out, p.byte_extent);
        put_u32_le(out, p.partial_final_word_bytes);
        put_u32_le(out, static_cast<uint32_t>(p.word_classes.size()));
        for (WordClass wc : p.word_classes)
            put_u8(out, static_cast<uint8_t>(wc));
        put_u32_le(out, p.start_position);
    }
    return out;
}

uint64_t abi_fingerprint(const CallableDescriptor& d) {
    const std::string bytes = encode_callable(d);
    return fnv1a64(reinterpret_cast<const uint8_t*>(bytes.data()),
                   bytes.size(), kFnv1aOffset);
}

AbiClassification classify_callable(const CallableDescriptor& d) {
    AbiClassification r;
    r.bytes = encode_callable(d);
    r.fingerprint = fnv1a64(reinterpret_cast<const uint8_t*>(r.bytes.data()),
                            r.bytes.size(), kFnv1aOffset);
    return r;
}

} // namespace ember
