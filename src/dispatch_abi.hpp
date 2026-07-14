// dispatch_abi.hpp — Red 2: stable canonical ABI fingerprints for keyed dispatch.
//
// A stable, fully-specified byte encoding of a callable's calling-sequence
// shape plus a pinned 64-bit fingerprint over those bytes. The fingerprint
// partitions callables into exact ABI domains (docs/planning/
// plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §7). Build and runtime use
// the same encoding so a domain identity is stable across the build/runtime
// boundary. The fingerprint is NOT std::hash and NOT Type::to_string; it is a
// pinned FNV-1a 64-bit over an explicit, length-prefixed, little-endian byte
// stream with a fixed domain header and deterministic field order.
//
// ─── Why a separate encoding (not Type::to_string) ───
//
// Type::to_string is a diagnostic pretty-printer. It does not carry word
// counts, per-word placement classes, byte extents, partial final words,
// hidden words, calling mode, or coroutine flags — every property that
// actually affects the calling sequence. Two callables with the same
// to_string spelling can have different ABIs (a legacy-context entry vs a
// keyed-r15 entry; an ordinary fn vs a coroutine). The classifier encoding
// captures every ABI-affecting property explicitly.
//
// ─── Canonical byte encoding ───
//
//   DOMAIN = "Ember.DispatchAbi.v1"   (20 ASCII bytes, no length prefix,
//                                       no NUL — it is a fixed constant)
//
//   B := DOMAIN
//        || u8(arch)                         // TargetArch
//        || u32_le(convention_version)       // Win64 ABI version (1)
//        || u8(calling_mode)                 // CallingMode
//        || u8(visibility)                   // Visibility
//        || u8(return_kind)                  // ReturnKind
//        || encode_type(return_type)         // full recursive return type
//        || u8(has_hidden_return)            // hidden aggregate return ptr
//        || u8(has_hidden_env)               // hidden lambda env ptr
//        || u8(is_coroutine)                 // coroutine entry
//        || u8(is_special_entry)             // thread/lifecycle/special entry
//        || u32_le(param_count)
//        || [for each param in order:]
//           || encode_type(param.type)             // recursive canonical type
//           || u32_le(param.word_count)            // 8-byte words this param occupies
//           || u32_le(param.byte_extent)           // actual byte size the param occupies
//           || u32_le(param.partial_final_word_bytes) // bytes used in the final word
//                                                      // (0 = full final word; for a
//                                                      // single full word, byte_extent==8
//                                                      // and this is 0)
//           || u32_le(word_classes.size())         // must equal word_count
//           || [for each occupied word in order:] u8(word_class)
//                                                  // GP / XmmF32 / XmmF64 / Stack / HiddenPtr
//           || u32_le(param.start_position)       // 0-based word index of the first word
//
//   encode_type(t) :=
//        || u8(t.kind)                       // TypeKind
//        [kind == Scalar:]
//           || u8(t.prim)                    // PrimTag
//        [kind == OpaqueHandle:]
//           // A nominal opaque handle (vec3-style) — passed/returned as an i64
//           // in a GP register, but carrying an explicit nominal identity and
//           // byte size so it is NOT indistinguishable from an ordinary i64.
//           || u32_le(len(struct_name)) || struct_name bytes
//           || u32_le(byte_size)            // handle storage size in bytes (e.g. 8)
//        [kind == Struct:]
//           || u32_le(len(struct_name)) || struct_name bytes
//           || u32_le(struct_size)
//           || u32_le(struct_alignment)
//           || u32_le(field_count)
//           || [for each field in declaration order:]
//              || u32_le(len(field.name)) || field.name bytes
//              || u32_le(field.offset)
//              || encode_type(field.type)
//        [kind == Slice:]
//           || encode_type(t.elem)
//        [kind == Lambda:]
//           || u8(has_recorded_sig)
//           || u32_le(recorded_param_count)
//           || [for each recorded param:] encode_type(param)
//           || encode_type(recorded_ret)
//        [kind == FnHandle:]
//           || u32_le(len(struct_name)) || struct_name bytes   // nominal id
//           || u8(is_cross_module_handle)
//           || u8(has_recorded_sig)
//           || u32_le(recorded_param_count)
//           || [for each recorded param:] encode_type(param)
//           || encode_type(recorded_ret)
//        [kind == Array:]
//           || u32_le(array_len)
//           || encode_type(t.elem)
//
//   fingerprint = FNV-1a 64-bit over B
//                 h0 = 0xcbf29ce484222325
//                 h  = (h ^ byte) * 0x100000001b3  (per byte, mod 2^64)
//
// String lengths are uint32_t counts of raw UTF-8 bytes (no NUL terminator).
// Integer fields are fixed-width little-endian. Nothing about a native C++
// struct layout, std::hash, a pointer value, or unordered-container iteration
// order participates in the encoding.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ember {

// ─── Target architecture ───
enum class TargetArch : uint8_t {
    X64_Win64 = 0,   // Windows x86-64 calling convention
};

// ─── Calling mode (affects register reservation + entry sequence) ───
enum class CallingMode : uint8_t {
    LegacyContext = 0,   // r14 = context_t*, r15 free (legacy/unkeyed)
    KeyedR15      = 1,   // r15 reserved for transient route word (keyed mode)
};

// ─── Visibility class (domain isolation per §7.2) ───
enum class Visibility : uint8_t {
    Private = 0,
    Public  = 1,
};

// ─── Return representation kind ───
enum class ReturnKind : uint8_t {
    Void            = 0,   // no return value
    GpScalar        = 1,   // GP register (rax): i*/u*/bool/ptr/handle
    XmmF32          = 2,   // XMM register (xmm0): f32
    XmmF64          = 3,   // XMM register (xmm0): f64
    SlicePair       = 4,   // {ptr, len} in rax/rdx
    LambdaPair      = 5,   // {fn_slot, env_ptr} in rax/rdx
    HiddenAggregate = 6,   // >8B struct via hidden pointer (rcx = dest)
    OpaqueHandle    = 7,   // nominal opaque handle returned as an i64 in rax,
                           //   distinct from an ordinary GpScalar i64 return
};

// ─── Word class for a single calling-sequence word ───
enum class WordClass : uint8_t {
    GP        = 0,   // general-purpose register (rcx/rdx/r8/r9)
    XmmF32    = 1,   // XMM register, f32
    XmmF64    = 2,   // XMM register, f64
    HiddenPtr = 3,   // hidden pointer (return dest or env)
    Stack     = 4,   // outgoing stack word (past the 4-register / shadow area)
};

// ─── Canonical type kind ───
enum class TypeKind : uint8_t {
    Scalar       = 0,   // primitive (PrimTag carries which)
    Struct       = 1,   // nominal aggregate (name + size + alignment + fields)
    Slice        = 2,   // {ptr, len} — 2 words
    Lambda       = 3,   // {fn_slot, env_ptr} — 2 words, recorded sig
    FnHandle     = 4,   // i64 function handle, optional recorded sig
    Array        = 5,   // fixed array
    OpaqueHandle = 6,  // nominal opaque handle (vec3-style): carried as an i64
                       //   in a GP word but carrying a nominal identity so it is
                       //   distinguishable from an ordinary i64 scalar.
};

// ─── Primitive tag (stable, explicit) ───
enum class PrimTag : uint8_t {
    Void = 0,
    Bool = 1,
    I8  = 2, I16 = 3, I32 = 4, I64 = 5,
    U8  = 6, U16 = 7, U32 = 8, U64 = 9,
    F32 = 10, F64 = 11,
};

// Forward declaration: AbiType and AbiStructField are mutually recursive.
struct AbiType;

// A struct field in canonical layout order.
struct AbiStructField {
    std::string name;
    uint32_t offset = 0;
    std::shared_ptr<AbiType> type;   // shared_ptr breaks the mutual recursion
};

// A canonical recursive type description. Self-contained — no raw Type
// pointers, no Type::to_string, no native struct bytes. Captures every
// property that affects the calling sequence.
struct AbiType {
    TypeKind kind = TypeKind::Scalar;
    PrimTag prim = PrimTag::Void;               // kind == Scalar

    std::string struct_name;                      // kind == Struct || FnHandle || OpaqueHandle

    // Struct layout (kind == Struct)
    uint32_t struct_size = 0;
    uint32_t struct_alignment = 1;
    std::vector<AbiStructField> fields;

    // Opaque handle storage size in bytes (kind == OpaqueHandle)
    uint32_t handle_byte_size = 8;

    // Fn handle / lambda (kind == FnHandle || Lambda)
    bool is_cross_module_handle = false;
    bool has_recorded_sig = false;
    std::vector<std::shared_ptr<AbiType>> recorded_params;
    std::shared_ptr<AbiType> recorded_ret;

    // Slice / array element (kind == Slice || Array)
    std::shared_ptr<AbiType> elem;
    uint32_t array_len = 0;                       // kind == Array
};

// A single parameter's ABI placement. The word_classes vector carries an
// EXPLICIT class for every occupied word (not a single representative class),
// so a parameter that spans register words and a stack word records each
// word's class individually. byte_extent + partial_final_word_bytes record
// the true byte footprint so a partial final word (e.g. a 12-byte struct
// occupying 2 words, only 4 of the final 8 bytes used) is distinguishable from
// a struct that fills both words.
struct AbiParam {
    AbiType type;
    uint32_t word_count = 1;       // 8-byte words this param occupies
    uint32_t byte_extent = 8;      // actual byte size the param occupies
    uint32_t partial_final_word_bytes = 0;  // bytes used in the final word
                                            // (0 = full final word, or word_count==1 with byte_extent==8)
    std::vector<WordClass> word_classes;    // one class per occupied word (size == word_count)
    uint32_t start_position = 0;   // 0-based word index of the first word
};

// A concrete callable descriptor — the input to the classifier. Self-contained
// and constructable directly by tests (no dependency on ember::Type or
// NativeSig). Every field that affects the calling sequence is explicit.
struct CallableDescriptor {
    TargetArch arch = TargetArch::X64_Win64;
    uint32_t convention_version = 1;
    CallingMode calling_mode = CallingMode::LegacyContext;
    Visibility visibility = Visibility::Public;
    ReturnKind return_kind = ReturnKind::Void;
    AbiType return_type;
    bool has_hidden_return = false;   // hidden aggregate return pointer (word 0)
    bool has_hidden_env = false;      // hidden lambda env pointer (word 0 or 1)
    bool is_coroutine = false;
    bool is_special_entry = false;
    std::vector<AbiParam> params;
};

// ─── Convenience builders (keep test code readable) ───

inline AbiType abi_scalar(PrimTag p) {
    AbiType t;
    t.kind = TypeKind::Scalar;
    t.prim = p;
    return t;
}

// A nominal opaque handle (vec3-style). Carried as an i64 in a GP word but
// carrying an explicit nominal identity + byte size so it is distinguishable
// from an ordinary i64 scalar in the fingerprint.
inline AbiType abi_opaque_handle(std::string name, uint32_t byte_size = 8) {
    AbiType t;
    t.kind = TypeKind::OpaqueHandle;
    t.struct_name = std::move(name);
    t.handle_byte_size = byte_size;
    return t;
}

inline AbiType abi_struct(std::string name, uint32_t size, uint32_t align,
                          std::vector<AbiStructField> fields = {}) {
    AbiType t;
    t.kind = TypeKind::Struct;
    t.struct_name = std::move(name);
    t.struct_size = size;
    t.struct_alignment = align;
    t.fields = std::move(fields);
    return t;
}

inline AbiType abi_slice(AbiType elem) {
    AbiType t;
    t.kind = TypeKind::Slice;
    t.elem = std::make_shared<AbiType>(std::move(elem));
    return t;
}

inline AbiType abi_lambda(std::vector<AbiType> params, AbiType ret) {
    AbiType t;
    t.kind = TypeKind::Lambda;
    t.has_recorded_sig = true;
    t.recorded_ret = std::make_shared<AbiType>(std::move(ret));
    t.recorded_params.reserve(params.size());
    for (auto& p : params)
        t.recorded_params.push_back(std::make_shared<AbiType>(std::move(p)));
    return t;
}

inline AbiType abi_fn_handle(std::string name, bool cross_module,
                             bool has_sig, std::vector<AbiType> params, AbiType ret) {
    AbiType t;
    t.kind = TypeKind::FnHandle;
    t.struct_name = std::move(name);
    t.is_cross_module_handle = cross_module;
    t.has_recorded_sig = has_sig;
    if (has_sig) {
        t.recorded_ret = std::make_shared<AbiType>(std::move(ret));
        t.recorded_params.reserve(params.size());
        for (auto& p : params)
            t.recorded_params.push_back(std::make_shared<AbiType>(std::move(p)));
    }
    return t;
}

inline AbiType abi_array(AbiType elem, uint32_t len) {
    AbiType t;
    t.kind = TypeKind::Array;
    t.elem = std::make_shared<AbiType>(std::move(elem));
    t.array_len = len;
    return t;
}

// Build an AbiParam from an explicit per-word class list. word_classes.size()
// MUST equal word_count. byte_extent is the true byte footprint;
// partial_final_word_bytes is the bytes used in the final word (0 when the
// final word is full, i.e. byte_extent == word_count*8).
inline AbiParam abi_param(AbiType type, uint32_t word_count, uint32_t byte_extent,
                          uint32_t partial_final_word_bytes,
                          std::vector<WordClass> word_classes,
                          uint32_t start_position) {
    AbiParam p;
    p.type = std::move(type);
    p.word_count = word_count;
    p.byte_extent = byte_extent;
    p.partial_final_word_bytes = partial_final_word_bytes;
    p.word_classes = std::move(word_classes);
    p.start_position = start_position;
    return p;
}

// Convenience: a single full GP word (1 word, 8 bytes, no partial word).
inline AbiParam abi_gp(AbiType type, uint32_t start_position) {
    return abi_param(std::move(type), 1, 8, 0, {WordClass::GP}, start_position);
}

// Convenience: a single full XMM f32 word.
inline AbiParam abi_f32(AbiType type, uint32_t start_position) {
    return abi_param(std::move(type), 1, 4, 4, {WordClass::XmmF32}, start_position);
}

// Determine the Win64 word class for a canonical type (f32 → XmmF32,
// f64 → XmmF64, everything else → GP). Used by builders that want the
// codegen-accurate class without spelling it out.
WordClass word_class_for_type(const AbiType& t);

// ─── Classifier API ───

// The fixed 20-byte domain header (ASCII "Ember.DispatchAbi.v1", no NUL).
inline constexpr const char* kDispatchAbiDomain = "Ember.DispatchAbi.v1";
inline constexpr size_t kDispatchAbiDomainLen = 20;

// The current Win64 convention version. Bump when the calling convention
// changes — this invalidates all old fingerprints (correctly).
inline constexpr uint32_t kWin64ConventionVersion = 1;

// Encode a callable descriptor to its canonical byte stream.
std::string encode_callable(const CallableDescriptor& d);

// Compute the 64-bit fingerprint (pinned FNV-1a, NOT std::hash) over the
// canonical byte encoding.
uint64_t abi_fingerprint(const CallableDescriptor& d);

// Convenience: encode + fingerprint in one call.
struct AbiClassification {
    std::string bytes;
    uint64_t fingerprint;
};
AbiClassification classify_callable(const CallableDescriptor& d);

} // namespace ember
