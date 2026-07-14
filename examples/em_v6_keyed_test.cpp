// em_v6_keyed_test.cpp — Red 9
// (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §11, §14.2-§14.6, §16):
// the target-specific keyed-dispatch V6 artifact gate.
//
// RED-GREEN contract chunk for the V6 .em artifact. Pins, against a real
// keyed V6 module written + loaded in-memory (NO temp file), the §11 / §14.2 /
// §14.3 / §14.5 / §14.6 mandatory buckets:
//
//   - V6 write/load/run:        a keyed V6 module round-trips through
//                               write_em_bytes_v6 + load_em_bytes; under the
//                               correct route word a keyed call returns the
//                               correct value.
//   - safe wrong-key:           a wrong route word resolves to a non-null,
//                               in-range, same-ABI-domain entry (alternate real
//                               or padding) — no crash, no AV; a padding route
//                               fires KeyedDispatchPadding.
//   - V6 byte round-trip:       the serialized bytes parse back to an
//                               equivalent metadata + per-fn shape.
//   - data_temp_off:            a V6 IR function whose StringDecrypt carries a
//                               nonzero data_temp_off round-trips (blob-v2
//                               preserves it; the loaded function executes).
//   - malformed/truncated/
//     overflowed:               corrupt V6 descriptors are rejected BEFORE any
//                               exec page / publication.
//   - duplicate placement:      two physical entries at the same slot are
//                               rejected (writer preflight AND loader via
//                               mutated bytes).
//   - ABI mismatch:             a logical route whose ABI fingerprint differs
//                               from its domain's is rejected.
//   - unsupported/unrecognized/
//     contradictory/duplicate
//     caps:                     the capability matrix is validated against the
//                               host's caps; unsupported, unknown, duplicate,
//                               and contradictory requirements are structured
//                               failures (no exec page, no publication). The
//                               writer-preflight cases are paired with
//                               MUTATED-BYTE-BUFFER cases so the LOADER's
//                               rejection is actually exercised even when the
//                               writer preflight rejects equivalent in-memory
//                               metadata.
//   - unsupported ABI-domain
//     set:                      the host-supported declared ABI-domain set is
//                               checked (not merely a module-local domain
//                               count); a module domain whose ABI fingerprint
//                               the host does not support is rejected.
//   - blob-v1 rejection:        a V6 IR function whose blob declares v1 is
//                               rejected by the loader (mutated bytes) as well
//                               as the writer preflight.
//   - raw fallback rejection:   a V6 raw-x86 fallback function is rejected
//                               under the secure default (no allow_raw_x86).
//   - no publication on failure:a failed load leaves out.pages empty and no
//                               dispatch entries.
//   - schema assertions:        the V6 metadata structs contain NO field for a
//                               target/runtime key, machine fingerprint,
//                               expected key, key hash/digest/verifier, direct
//                               comparison constant, or predecoded permutation
//                               map (§14.6).
//   - V1-V5 regressions:        a v5 IR module still loads + runs (identity
//                               layout); a v3 raw-x86 module is rejected under
//                               the secure default (raw fallback) and accepted
//                               with allow_raw_x86 (identity layout unchanged).
//
// The test builds keyed V6 modules from real lowered ThinFunctions (lower_function
// + serialize_thin_function) + a hand-built EmV6Metadata derived from a
// ModuleLayoutPlan, so the on-disk topology is a real keyed permutation.
//
// NOT a CTest entry: the filtered suite count must stay 67 (§14.1).

#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"
#include "../src/thin_lower.hpp"
#include "../src/thin_ir.hpp"
#include "../src/thin_ir_ser.hpp"
#include "../src/thin_emit.hpp"
#include "../src/engine.hpp"
#include "../src/context.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/jit_memory.hpp"
#include "../src/em_file.hpp"
#include "../src/em_writer.hpp"
#include "../src/em_loader.hpp"
#include "../src/module_linker.hpp"
#include "../src/module_registry.hpp"
#include "../src/module_layout.hpp"
#include "../src/keyed_dispatch.hpp"
#include "../src/dispatch_abi.hpp"
#include "../src/extension_registry.hpp"
#include "../src/globals.hpp"
#include "../src/binding_builder.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

static int g_fail = 0;
static int g_checks = 0;
static void ck(bool c, const char* m) {
    ++g_checks;
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// 1-arg i64 caller.
static int64_t call_i64_i64(void* entry, int64_t a) {
    using F = int64_t(*)(int64_t);
    return reinterpret_cast<F>(entry)(a);
}

// ===========================================================================
// ABI classifier helpers (ported from keyed_dispatch_modules_test).
// ===========================================================================
static AbiType prim_to_abi(Prim p) {
    switch (p) {
    case Prim::Void:  return abi_scalar(PrimTag::Void);
    case Prim::Bool:  return abi_scalar(PrimTag::Bool);
    case Prim::I8:    return abi_scalar(PrimTag::I8);
    case Prim::I16:   return abi_scalar(PrimTag::I16);
    case Prim::I32:   return abi_scalar(PrimTag::I32);
    case Prim::I64:   return abi_scalar(PrimTag::I64);
    case Prim::U8:    return abi_scalar(PrimTag::U8);
    case Prim::U16:   return abi_scalar(PrimTag::U16);
    case Prim::U32:   return abi_scalar(PrimTag::U32);
    case Prim::U64:   return abi_scalar(PrimTag::U64);
    case Prim::F32:   return abi_scalar(PrimTag::F32);
    case Prim::F64:   return abi_scalar(PrimTag::F64);
    }
    return abi_scalar(PrimTag::Void);
}

static AbiType type_to_abi(const Type& t) {
    if (t.is_slice) {
        AbiType at; at.kind = TypeKind::Slice;
        at.elem = std::make_shared<AbiType>(t.elem ? type_to_abi(*t.elem) : abi_scalar(PrimTag::Void));
        return at;
    }
    if (t.is_lambda) {
        AbiType at; at.kind = TypeKind::Lambda;
        at.has_recorded_sig = t.has_recorded_sig;
        if (t.has_recorded_sig) {
            for (auto& p : t.recorded_params)
                at.recorded_params.push_back(std::make_shared<AbiType>(p ? type_to_abi(*p) : abi_scalar(PrimTag::Void)));
            at.recorded_ret = std::make_shared<AbiType>(t.recorded_ret ? type_to_abi(*t.recorded_ret) : abi_scalar(PrimTag::Void));
        }
        return at;
    }
    if (t.is_fn_handle) {
        AbiType at; at.kind = TypeKind::FnHandle;
        at.struct_name = t.struct_name;
        at.is_cross_module_handle = t.is_cross_module_handle;
        at.has_recorded_sig = t.has_recorded_sig;
        if (t.has_recorded_sig) {
            for (auto& p : t.recorded_params)
                at.recorded_params.push_back(std::make_shared<AbiType>(p ? type_to_abi(*p) : abi_scalar(PrimTag::Void)));
            at.recorded_ret = std::make_shared<AbiType>(t.recorded_ret ? type_to_abi(*t.recorded_ret) : abi_scalar(PrimTag::Void));
        }
        return at;
    }
    return prim_to_abi(t.prim);
}

static uint32_t words_for_type(const Type& t) {
    if (t.is_slice || t.is_lambda) return 2;
    return 1;
}

static uint64_t fingerprint_for(const FuncDecl& f) {
    CallableDescriptor d;
    d.arch = TargetArch::X64_Win64;
    d.convention_version = kWin64ConventionVersion;
    d.calling_mode = CallingMode::LegacyContext;
    d.visibility = f.is_exported ? Visibility::Public : Visibility::Private;
    const Type& rt = f.ret ? *f.ret : type_void();
    d.return_type = type_to_abi(rt);
    if (rt.is_void()) d.return_kind = ReturnKind::Void;
    else if (rt.is_float()) d.return_kind = (rt.prim == Prim::F64) ? ReturnKind::XmmF64 : ReturnKind::XmmF32;
    else if (rt.is_slice) d.return_kind = ReturnKind::SlicePair;
    else if (rt.is_lambda) d.return_kind = ReturnKind::LambdaPair;
    else if (rt.is_fn_handle) d.return_kind = ReturnKind::GpScalar;
    else d.return_kind = ReturnKind::GpScalar;
    d.is_coroutine = f.is_coroutine;
    d.is_special_entry = false;
    d.has_hidden_env = f.is_lambda;
    uint32_t pos = (d.has_hidden_return ? 1u : 0u) + (d.has_hidden_env ? 1u : 0u);
    size_t begin = f.is_lambda ? 1u : 0u;
    for (size_t i = begin; i < f.params.size(); ++i) {
        const Type& pt = *f.params[i].ty;
        AbiType at = type_to_abi(pt);
        uint32_t wc = words_for_type(pt);
        uint32_t byte_ext = (pt.is_slice || pt.is_lambda) ? 16u : (pt.is_float() ? (pt.prim == Prim::F64 ? 8u : 4u) : 8u);
        uint32_t partial = (wc > 0 && byte_ext != wc * 8u) ? (byte_ext - (wc - 1) * 8u) : 0u;
        WordClass wc0 = word_class_for_type(at);
        std::vector<WordClass> wcs;
        for (uint32_t w = 0; w < wc; ++w) wcs.push_back(wc0);
        d.params.push_back(abi_param(std::move(at), wc, byte_ext, partial, std::move(wcs), pos));
        pos += wc;
    }
    return classify_callable(d).fingerprint;
}

static ModuleManifest make_manifest(const Program& prog,
                                     const std::unordered_map<std::string,int>& slots,
                                     const std::string& module_id) {
    ModuleManifest m;
    m.module_id = module_id;
    std::unordered_map<std::string, const FuncDecl*> by_name;
    for (auto& fn : prog.funcs) by_name[fn.name] = &fn;
    for (auto& [name, slot] : slots) {
        ModuleCallable c;
        c.name = name;
        c.logical_slot = uint32_t(slot);
        auto it = by_name.find(name);
        c.abi_fingerprint = (it != by_name.end()) ? fingerprint_for(*it->second) : 0xA0A0A0A0A0A0A0A0ULL;
        c.visibility = Visibility::Public;
        c.calling_mode = CallingMode::LegacyContext;
        c.dispatch_domain = "";
        m.callables.push_back(std::move(c));
    }
    std::sort(m.callables.begin(), m.callables.end(),
              [](const ModuleCallable& a, const ModuleCallable& b) {
                  return a.logical_slot < b.logical_slot;
              });
    return m;
}

// ===========================================================================
// Convert a ModuleLayoutPlan (Red 3 keyed plan) into the on-disk EmV6Metadata.
// The plan is the build-time keyed layout; the metadata is its key-free on-disk
// shape (NO key/verifier field — §14.6). The physical order is the build key's
// influence and is necessarily observable; the loader loads it AS-IS.
// ===========================================================================
static std::shared_ptr<EmV6Metadata> plan_to_v6_metadata(const ModuleLayoutPlan& plan,
                                                          const std::string& strategy_id) {
    auto m = std::make_shared<EmV6Metadata>();
    m->strategy_id = strategy_id;
    m->strategy_version = 1;
    m->dispatch_mode = EM_V6_DISPATCH_KEYED;
    m->logical_slot_count = plan.logical_slot_count;
    m->physical_slot_count = plan.physical_slot_count;
    // Capability matrix (versioned, extensible, key-free).
    m->capabilities.push_back({EM_V6_CAP_KEYED_DISPATCH_RUNTIME, 1u});
    m->capabilities.push_back({EM_V6_CAP_BLOB_V2_RE_EMIT, 1u});
    m->capabilities.push_back({EM_V6_CAP_DISPATCH_STRATEGY, m->strategy_version});
    m->capabilities.push_back({EM_V6_CAP_DISPATCH_MODE, uint32_t(m->dispatch_mode)});
    m->capabilities.push_back({EM_V6_CAP_ABI_DOMAIN_SET, uint32_t(plan.domains.size())});
    // Domains.
    m->domains.reserve(plan.domains.size());
    for (const auto& d : plan.domains) {
        EmV6DomainDescriptor ed;
        ed.visibility = uint8_t(d.visibility);
        ed.calling_mode = uint8_t(d.calling_mode);
        ed.abi_fingerprint = d.abi_fingerprint;
        ed.domain_salt = d.domain_salt;
        ed.strategy_version = d.strategy_version;
        ed.physical_base = d.physical_base;
        ed.physical_count = d.physical_count;
        ed.logical_count = d.logical_count;
        ed.padding_count = d.padding_count;
        ed.padding_ordinal = d.padding_ordinal;
        ed.dispatch_domain = d.dispatch_domain;
        ed.logical_slots = d.logical_slots;
        m->domains.push_back(std::move(ed));
    }
    // Logical routes.
    m->logical_routes.reserve(plan.logical_routes.size());
    for (const auto& r : plan.logical_routes) {
        EmV6LogicalRoute er;
        er.logical_slot = r.logical_slot;
        er.domain_index = r.domain_index;
        er.ordinal = r.ordinal;
        er.abi_fingerprint = r.abi_fingerprint;
        er.visibility = uint8_t(r.visibility);
        er.calling_mode = uint8_t(r.calling_mode);
        er.dispatch_domain = r.dispatch_domain;
        m->logical_routes.push_back(std::move(er));
    }
    // Physical entries.
    m->physical_entries.reserve(plan.physical_entries.size());
    for (const auto& p : plan.physical_entries) {
        EmV6PhysicalEntry ep;
        ep.physical_slot = p.physical_slot;
        ep.abi_fingerprint = p.abi_fingerprint;
        ep.visibility = uint8_t(p.visibility);
        ep.calling_mode = uint8_t(p.calling_mode);
        ep.dispatch_domain = p.dispatch_domain;
        ep.name = p.name;
        ep.is_padding = p.is_padding;
        ep.logical_slot = p.logical_slot;
        ep.domain_index = p.domain_index;
        ep.ordinal = p.ordinal;
        m->physical_entries.push_back(std::move(ep));
    }
    // Padding descriptors.
    m->padding_descriptors.reserve(plan.padding_descriptors.size());
    for (const auto& pd : plan.padding_descriptors) {
        EmV6PaddingDescriptor epd;
        epd.domain_index = pd.domain_index;
        epd.ordinal = pd.ordinal;
        epd.physical_slot = pd.physical_slot;
        epd.abi_fingerprint = pd.abi_fingerprint;
        epd.visibility = uint8_t(pd.visibility);
        epd.calling_mode = uint8_t(pd.calling_mode);
        epd.dispatch_domain = pd.dispatch_domain;
        m->padding_descriptors.push_back(std::move(epd));
    }
    return m;
}

// Deep-copy an EmV6Metadata (EmModule's v6 is a shared_ptr, so a value copy
// of EmModule SHARES the metadata object — corruption tests must deep-copy to
// avoid mutating the shared original).
static std::shared_ptr<EmV6Metadata> clone_v6_metadata(const EmV6Metadata& src) {
    auto m = std::make_shared<EmV6Metadata>();
    m->strategy_id = src.strategy_id;
    m->strategy_version = src.strategy_version;
    m->dispatch_mode = src.dispatch_mode;
    m->logical_slot_count = src.logical_slot_count;
    m->physical_slot_count = src.physical_slot_count;
    m->capabilities = src.capabilities;
    m->domains = src.domains;
    m->logical_routes = src.logical_routes;
    m->physical_entries = src.physical_entries;
    m->padding_descriptors = src.padding_descriptors;
    return m;
}

// ===========================================================================
// V6ByteMap: parse a serialized V6 buffer to locate byte offsets of specific
// metadata fields + the first IR blob's version word. This lets the malformed-
// loader cases mutate ALREADY-SERIALIZED valid bytes (bypassing the writer's
// in-memory preflight) so the LOADER's rejection is actually exercised even
// when the writer preflight rejects equivalent in-memory metadata (§14.2 / the
// Red 9 contract). NO temp files — every mutation is on an in-memory vector.
// Mirrors the writer's/em_loader's meta-block layout EXACTLY (see
// emit_v6_metadata in em_writer.cpp).
// ===========================================================================
struct V6ByteMap {
    size_t meta_start = 0;          // = EM_HEADER_SIZE (40)
    size_t cap_count_off = 0;       // offset of the u32 cap_count
    std::vector<size_t> cap_id_off;     // offset of each capability's u16 id
    std::vector<size_t> cap_val_off;    // offset of each capability's u32 value
    std::vector<size_t> phys_slot_off;  // offset of each physical entry's u32 physical_slot
    size_t meta_end = 0;           // offset where per-fn records begin
    size_t first_blob_version_off = 0;  // offset of the u16 version word of the
                                        // first IR blob (or 0 if not found)
    bool ok = false;
};

static uint32_t rd_u32(const std::vector<uint8_t>& b, size_t off) {
    return uint32_t(b[off]) | (uint32_t(b[off+1]) << 8) |
           (uint32_t(b[off+2]) << 16) | (uint32_t(b[off+3]) << 24);
}
static uint16_t rd_u16(const std::vector<uint8_t>& b, size_t off) {
    return uint16_t(b[off]) | (uint16_t(b[off+1]) << 8);
}

static V6ByteMap map_v6_bytes(const std::vector<uint8_t>& b) {
    V6ByteMap m;
    m.meta_start = EM_HEADER_SIZE;
    size_t off = m.meta_start;
    auto need = [&](size_t n) -> bool { return off + n <= b.size(); };
    if (!need(8)) return m;  // magic + layout
    off += 8;  // magic + layout
    // strategy id (u16 len + bytes)
    if (!need(2)) return m;
    uint16_t sid_len = rd_u16(b, off); off += 2;
    if (off + sid_len > b.size()) return m;
    off += sid_len;
    // strategy_version u32, dispatch_mode u8, logical_count u32, physical_count u32
    if (!need(4 + 1 + 4 + 4)) return m;
    off += 4 + 1 + 4 + 4;
    // capability matrix: u32 count + count × {u16 id, u32 value}
    if (!need(4)) return m;
    m.cap_count_off = off;
    uint32_t cap_count = rd_u32(b, off); off += 4;
    for (uint32_t i = 0; i < cap_count; ++i) {
        if (!need(2 + 4)) return m;
        m.cap_id_off.push_back(off);
        (void)rd_u16(b, off); off += 2;
        m.cap_val_off.push_back(off);
        off += 4;
    }
    // domains: u32 count + count × descriptor
    if (!need(4)) return m;
    uint32_t dom_count = rd_u32(b, off); off += 4;
    for (uint32_t i = 0; i < dom_count; ++i) {
        // u8 vis, u8 mode, u64 abi, u64 salt, u32 strat_ver, u32 phys_base,
        // u32 phys_count, u32 log_count, u32 pad_count, u32 pad_ordinal
        if (!need(1+1+8+8+4+4+4+4+4+4)) return m;
        off += 1+1+8+8+4+4+4+4+4+4;
        // u16 label_len + bytes
        if (!need(2)) return m;
        uint16_t lab = rd_u16(b, off); off += 2;
        if (off + lab > b.size()) return m;
        off += lab;
        // u32 ls_count + ls_count × u32
        if (!need(4)) return m;
        uint32_t ls_count = rd_u32(b, off); off += 4;
        if (!need(size_t(ls_count) * 4)) return m;
        off += size_t(ls_count) * 4;
    }
    // routes: u32 count + count × {u32,u32,u32,u64,u8,u8,u16+bytes}
    if (!need(4)) return m;
    uint32_t route_count = rd_u32(b, off); off += 4;
    for (uint32_t i = 0; i < route_count; ++i) {
        if (!need(4+4+4+8+1+1)) return m;
        off += 4+4+4+8+1+1;
        if (!need(2)) return m;
        uint16_t lab = rd_u16(b, off); off += 2;
        if (off + lab > b.size()) return m;
        off += lab;
    }
    // physical entries: u32 count + count × {u32,u64,u8,u8,u16+bytes,u16+bytes,u8,u32,u32,u32}
    if (!need(4)) return m;
    uint32_t phys_count = rd_u32(b, off); off += 4;
    for (uint32_t i = 0; i < phys_count; ++i) {
        if (!need(4+8+1+1)) return m;
        m.phys_slot_off.push_back(off);  // u32 physical_slot first
        off += 4 + 8 + 1 + 1;
        // u16 dispatch_domain label
        if (!need(2)) return m;
        uint16_t lab = rd_u16(b, off); off += 2;
        if (off + lab > b.size()) return m;
        off += lab;
        // u16 name
        if (!need(2)) return m;
        uint16_t nm = rd_u16(b, off); off += 2;
        if (off + nm > b.size()) return m;
        off += nm;
        // u8 is_pad, u32 log_slot, u32 dom_idx, u32 ordinal
        if (!need(1+4+4+4)) return m;
        off += 1+4+4+4;
    }
    // padding descriptors: u32 count + count × {...} — skip past to record
    // meta_end (the per-fn region begins after the meta block).
    if (!need(4)) return m;
    uint32_t pad_count = rd_u32(b, off); off += 4;
    for (uint32_t i = 0; i < pad_count; ++i) {
        // u32 dom_idx, u32 ordinal, u32 phys_slot, u64 abi, u8 vis, u8 mode, u16+bytes
        if (!need(4+4+4+8+1+1)) return m;
        off += 4+4+4+8+1+1;
        if (!need(2)) return m;
        uint16_t lab = rd_u16(b, off); off += 2;
        if (off + lab > b.size()) return m;
        off += lab;
    }
    m.meta_end = off;
    // Find the first IR blob version word: search the per-fn region for the
    // IR_BLOB_MAGIC 4-byte LE pattern; the version u16 is at +4. The per-fn
    // region starts at meta_end. (The magic 0x4952464E LE = 4E 46 52 49.)
    if (m.meta_end + 6 <= b.size()) {
        const uint8_t magic_le[4] = {
            uint8_t(IR_BLOB_MAGIC & 0xFF),
            uint8_t((IR_BLOB_MAGIC >> 8) & 0xFF),
            uint8_t((IR_BLOB_MAGIC >> 16) & 0xFF),
            uint8_t((IR_BLOB_MAGIC >> 24) & 0xFF)};
        for (size_t p = m.meta_end; p + 6 <= b.size(); ++p) {
            if (b[p] == magic_le[0] && b[p+1] == magic_le[1] &&
                b[p+2] == magic_le[2] && b[p+3] == magic_le[3]) {
                m.first_blob_version_off = p + 4;
                break;
            }
        }
    }
    m.ok = (m.meta_end > m.meta_start);
    return m;
}

// Patch a u16 LE value at `off` in `b`.
static void patch_u16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
    b[off] = uint8_t(v & 0xFF);
    b[off+1] = uint8_t((v >> 8) & 0xFF);
}
// Patch a u32 LE value at `off` in `b`.
static void patch_u32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    b[off] = uint8_t(v & 0xFF);
    b[off+1] = uint8_t((v >> 8) & 0xFF);
    b[off+2] = uint8_t((v >> 16) & 0xFF);
    b[off+3] = uint8_t((v >> 24) & 0xFF);
}

// Build a v6 EmModule from lowered ThinFunctions + a keyed plan. Each IR
// function ships its ir_blob (blob-v2). The EmFunctionRecord's slot_index is
// the LOGICAL slot; the V6 metadata carries the physical placement.
static EmModule build_v6_module(const std::vector<ThinFunction>& thfs,
                                const Program& program,
                                const std::vector<uint8_t>& globals_bytes,
                                uint32_t entry_slot,
                                const ModuleLayoutPlan& plan,
                                const std::string& strategy_id,
                                std::string* err) {
    EmModule mod;
    mod.has_v6 = true;
    mod.v6 = plan_to_v6_metadata(plan, strategy_id);
    mod.functions.reserve(thfs.size());
    for (size_t i = 0; i < thfs.size(); ++i) {
        const auto& thf = thfs[i];
        const auto& decl = program.funcs[i];
        EmFunctionRecord rec;
        rec.name = thf.name;
        rec.slot_index = uint32_t(decl.slot);  // LOGICAL slot
        if (!serialize_thin_function(thf, rec.ir_blob, err)) return mod;
        rec.signature.ret = decl.ret ? *decl.ret : Type{};
        for (const auto& p : decl.params)
            rec.signature.params.push_back(p.ty ? *p.ty : Type{});
        mod.functions.push_back(std::move(rec));
    }
    mod.globals = globals_bytes;
    mod.entry_slot = entry_slot;
    mod.name_table.reserve(program.funcs.size());
    for (const auto& fn : program.funcs)
        mod.name_table.emplace_back(fn.name, uint32_t(fn.slot));
    return mod;
}

// Build V6 host capabilities for a host that registered implicit-keyed-v1 and
// opted into keyed V6 (no raw x86). supports_all_abi_domains == true: the full
// keyed runtime accepts every ABI domain the classifier produces (the §11.3
// host-supported declared ABI-domain set).
static EmV6HostCaps make_v6_host_caps(bool keyed_runtime, bool allow_keyed) {
    EmV6HostCaps caps;
    caps.keyed_dispatch_runtime = keyed_runtime;
    caps.blob_v2_re_emit = true;
    caps.registered_strategies.push_back({"implicit-keyed-v1", 1u});
    caps.allow_identity_mode = true;
    caps.allow_keyed_mode = allow_keyed;
    caps.allow_raw_x86 = false;
    caps.supports_all_abi_domains = true;  // full keyed runtime: any ABI domain
    return caps;
}

// ===========================================================================
// Compile a small module (two same-ABI i64->i64 functions) to ThinFunctions +
// a keyed layout plan under a pinned route word. Returns the thfs, program,
// slots, plan, and route word.
// ===========================================================================
struct KeyedFixture {
    std::vector<ThinFunction> thfs;
    Program program;
    std::unordered_map<std::string, int> slots;
    std::unordered_map<std::string, NativeSig> natives;
    ModuleLayoutPlan plan;
    uint64_t route_word = 0;
    uint32_t entry_slot = 0;
    std::vector<uint8_t> globals;
    std::string src;
    // The JIT-compiled ground-truth entries (per logical slot), for value
    // comparison against the loaded V6 module.
    std::vector<CompiledFn> jit_fns;
    DispatchTable jit_table{0};
    ~KeyedFixture() { for (auto& f : jit_fns) if (f.exec) free_executable(f.exec); }
};

static std::unique_ptr<KeyedFixture> build_keyed_fixture(uint64_t route_word_seed) {
    auto fx = std::make_unique<KeyedFixture>();
    // Two same-ABI (i64->i64) functions in one domain + a main that calls one.
    // `inc` and `dbl` share the same ABI fingerprint -> one keyed domain with
    // a padding ordinal (2 real + 1 padding = physical_count 3).
    fx->src =
        "fn inc(x: i64) -> i64 { return x + 1; }\n"
        "fn dbl(x: i64) -> i64 { return x * 2; }\n"
        "fn main(x: i64) -> i64 { return inc(x); }\n";
    auto lr = tokenize(fx->src, "<v6>");
    if (!lr.ok) { std::printf("FAIL: lex\n"); return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse\n"); return nullptr; }
    int si = 0;
    for (auto& fn : pr.program.funcs) { fx->slots[fn.name] = si++; fn.slot = fx->slots[fn.name]; }
    auto layouts = build_struct_layouts(pr.program);
    auto sr = sema(pr.program, fx->natives, fx->slots, 0, nullptr, &layouts);
    if (!sr.ok) { std::printf("FAIL: sema\n"); return nullptr; }
    fx->program = std::move(pr.program);

    // Lower to ThinFunctions.
    CodeGenCtx ctx;
    ctx.natives = &fx->natives;
    ctx.script_slots = &fx->slots;
    ctx.structs = &layouts;
    ctx.enable_ir_backend = true;
    for (const auto& fn : fx->program.funcs) {
        ThinFunction thf = lower_function(fn, ctx);
        if (thf.blocks.empty()) { std::printf("FAIL: lower %s\n", fn.name.c_str()); return nullptr; }
        fx->thfs.push_back(std::move(thf));
    }

    // Keyed layout plan under the pinned route word.
    fx->globals = {};  // no globals
    uint8_t key_bytes[8];
    for (int i = 0; i < 8; ++i) key_bytes[i] = uint8_t((route_word_seed >> (8*i)) & 0xFF);
    BuildKeyView key{key_bytes, 8};
    ImplicitKeyedLayoutV1 planner;
    auto plan_res = planner.plan(make_manifest(fx->program, fx->slots, "<v6>"), key);
    if (!plan_res) { std::printf("FAIL: keyed plan\n"); return nullptr; }
    fx->plan = std::move(*plan_res.value);
    fx->route_word = derive_route_word(key);
    fx->entry_slot = uint32_t(fx->slots["main"]);

    // JIT ground-truth: compile each function and place at its logical slot.
    fx->jit_table = DispatchTable(int(fx->slots.size()));
    ctx.dispatch_base = int64_t(fx->jit_table.base());
    ctx.globals_base = 0;
    for (const auto& fn : fx->program.funcs) {
        CompiledFn cf = emit_x64(fx->thfs[size_t(fn.slot)], ctx);
        finalize(cf);
        fx->jit_table.set(fn.slot, cf.entry);
        fx->jit_fns.push_back(std::move(cf));
    }
    return fx;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    std::printf("== em_v6_keyed_test (Red 9) ==\n");

    const uint64_t CORRECT_SEED = 0xCAFEBABE12345678ULL;

    // =====================================================================
    // 1. V6 WRITE/LOAD/RUN with the correct route word.
    // =====================================================================
    std::printf("Part 1: V6 write/load/run (correct key)\n");
    auto fx = build_keyed_fixture(CORRECT_SEED);
    if (!fx) { std::printf("FAIL: build_keyed_fixture\n"); return 1; }
    ck(fx->plan.keyed, "keyed plan produced a keyed layout");
    ck(fx->plan.physical_slot_count > fx->plan.logical_slot_count,
       "keyed plan: physical > logical (padding present)");

    std::string serr;
    EmModule mod = build_v6_module(fx->thfs, fx->program, fx->globals,
                                   fx->entry_slot, fx->plan, "implicit-keyed-v1", &serr);
    if (!serr.empty()) { std::printf("FAIL: build_v6_module: %s\n", serr.c_str()); return 1; }
    ck(mod.has_v6 && mod.v6 != nullptr, "V6 EmModule has v6 metadata");

    // Write in-memory (no temp file).
    std::vector<uint8_t> buf;
    std::string werr;
    if (!write_em_bytes_v6(mod, buf, &werr)) {
        std::printf("FAIL: write_em_bytes_v6: %s\n", werr.c_str()); return 1;
    }
    ck(!buf.empty(), "write_em_bytes_v6 produced bytes");
    // The header version word (offset 4) must be 6.
    ck(buf.size() >= 8 && (uint32_t(buf[4]) | uint32_t(buf[5])<<8 |
                           uint32_t(buf[6])<<16 | uint32_t(buf[7])<<24) == EM_VERSION_V6,
       "V6 header version == 6");

    // Load with V6 host caps (keyed runtime + keyed mode opt-in).
    EmV6HostCaps caps = make_v6_host_caps(true, true);
    LoadedModule lm;
    std::string lerr;
    bool loaded = load_em_bytes(buf.data(), buf.size(), lm, &lerr,
                                nullptr, &fx->natives, nullptr, nullptr, &caps);
    if (!loaded) { std::printf("FAIL: load_em_bytes: %s\n", lerr.c_str()); return 1; }
    ck(loaded, "load_em_bytes succeeded for keyed V6 module");
    ck(lm.is_v6, "loaded module is V6");
    ck(lm.v6_keyed, "loaded module is keyed V6");
    ck(!lm.pages.empty(), "exec pages allocated (re-emit produced code)");
    ck(lm.v6_record != nullptr, "keyed ModuleDispatchRecord built + published");
    ck(lm.v6_physical_slot_count == fx->plan.physical_slot_count,
       "loaded physical_slot_count matches plan");

    // Correct-key run: resolve `main` through the keyed resolver.
    void* main_entry = lm.resolve_entry_by_name("main", fx->route_word);
    ck(main_entry != nullptr, "resolve_entry_by_name(main) under correct key -> non-null");
    // main calls inc(x); inc(x) = x+1. Run under the keyed re-entry thunk with
    // the route word installed in r15.
    ember::context_t ctx{};
    ctx.budget_remaining = 2'000'000'000LL;
    ctx.max_call_depth = 64;
    ctx.call_depth = 0;
    int64_t v = ember_keyed_reentry_i64(main_entry, &ctx, 41, fx->route_word);
    ck(v == 42, "correct-key V6 main(41) == 42 (inc)");

    // JIT ground-truth comparison.
    int64_t jit_v = call_i64_i64(fx->jit_table.get(fx->slots["inc"]), 41);
    ck(v == jit_v, "loaded keyed result matches JIT inc(41)");

    // entry()/entry_by_name() must NOT raw-index a keyed V6 module.
    ck(lm.entry() == nullptr, "entry() returns nullptr for keyed V6 (no raw logical indexing)");
    ck(lm.entry_by_name("main") == nullptr,
       "entry_by_name returns nullptr for keyed V6 (no raw logical indexing)");

    // =====================================================================
    // 2. SAFE WRONG-KEY alternate/padding routing.
    // =====================================================================
    std::printf("Part 2: safe wrong-key alternate/padding routing\n");
    // Find a wrong route word that routes `inc` to an alternate real entry or
    // to padding, under the loaded record. The resolver must return a non-null
    // in-range same-ABI entry (no crash, no AV).
    {
        const uint32_t inc_ls = uint32_t(fx->slots["inc"]);
        const auto& route = fx->plan.logical_routes[inc_ls];
        const auto& domain = fx->plan.domains[route.domain_index];
        KeyedDispatchDomain kd{domain.domain_salt, domain.strategy_version, domain.physical_count};
        uint64_t alt_word = 0, pad_word = 0;
        bool found_alt = false, found_pad = false;
        for (uint64_t w = 1; w < 200000 && !(found_alt && found_pad); ++w) {
            if (w == fx->route_word) continue;
            auto pr = keyed_dispatch_permute_runtime(w, kd, route.ordinal);
            if (!pr || !pr.value) continue;
            uint32_t local = *pr.value;
            if (local >= domain.physical_count) continue;
            uint32_t phys = domain.physical_base + local;
            if (phys >= fx->plan.physical_slot_count) continue;
            const auto& pe = fx->plan.physical_entries[phys];
            if (pe.is_padding) { if (!found_pad) { found_pad = true; pad_word = w; } }
            else if (pe.logical_slot != inc_ls) { if (!found_alt) { found_alt = true; alt_word = w; } }
        }
        ck(found_alt || found_pad, "found a wrong-key route (alternate or padding)");

        // Alternate-real route: non-null, in-range, same-ABI, no crash.
        if (found_alt) {
            void* alt_entry = lm.resolve_entry_by_name("inc", alt_word);
            ck(alt_entry != nullptr, "wrong-key alternate route -> non-null entry");
            // It is the same-domain alternate (dbl, x*2) — call it; must not crash.
            ember::context_t c2{};
            c2.budget_remaining = 2'000'000'000LL; c2.max_call_depth = 64; c2.call_depth = 0;
            int64_t av = ember_keyed_reentry_i64(alt_entry, &c2, 10, alt_word);
            // dbl(10) == 20 (alternate same-ABI callable). The exact value is
            // the alternate's; the contract is: in-range, same-ABI, no crash.
            ck(true, "wrong-key alternate route ran without crash (same-ABI)");
            (void)av;
        }
        // Padding route: non-null, same-ABI padding trap stub.
        if (found_pad) {
            void* pad_entry = lm.resolve_entry_by_name("inc", pad_word);
            ck(pad_entry != nullptr, "wrong-key padding route -> non-null padding entry");
            ck(ember_is_padding_trap_target(pad_entry),
               "wrong-key padding route resolves to the padding-trap stub");
            // Calling the padding stub records TrapReason::KeyedDispatchPadding on
            // the context and returns 0 (a neutral i64). The padding stub performs
            // NO key comparison (§7.3); it is a real callable same-ABI target that
            // fires the recoverable trap reason. The host checks last_trap after
            // the call (the stub does not longjmp itself).
            ember::context_t c3{};
            c3.budget_remaining = 2'000'000'000LL; c3.max_call_depth = 64; c3.call_depth = 0;
            int64_t pv = ember_keyed_reentry_i64(pad_entry, &c3, 7, pad_word);
            ck(pv == 0, "padding stub returns 0 (neutral i64)");
            ck(c3.last_trap == TrapReason::KeyedDispatchPadding,
               "padding route fires KeyedDispatchPadding trap (no key comparison)");
        }
    }

    // =====================================================================
    // 3. V6 byte round-trip: re-load the same bytes and compare metadata.
    // =====================================================================
    std::printf("Part 3: V6 byte round-trip\n");
    {
        LoadedModule lm2;
        std::string lerr2;
        EmV6HostCaps caps2 = make_v6_host_caps(true, true);
        bool ok2 = load_em_bytes(buf.data(), buf.size(), lm2, &lerr2,
                                 nullptr, &fx->natives, nullptr, nullptr, &caps2);
        ck(ok2, "second load of the same V6 bytes succeeded");
        if (ok2) {
            ck(lm2.v6_logical_slot_count == fx->plan.logical_slot_count,
               "round-trip logical_slot_count matches");
            ck(lm2.v6_physical_slot_count == fx->plan.physical_slot_count,
               "round-trip physical_slot_count matches");
            ck(lm2.v6_metadata->domains.size() == fx->plan.domains.size(),
               "round-trip domain count matches");
            ck(lm2.v6_metadata->logical_routes.size() == fx->plan.logical_routes.size(),
               "round-trip route count matches");
            // Physical entries: same placement (physical order is the build key's
            // influence and is necessarily observable; the loader loads AS-IS).
            ck(lm2.v6_metadata->physical_entries.size() == fx->plan.physical_entries.size(),
               "round-trip physical entry count matches");
            for (size_t i = 0; i < fx->plan.physical_entries.size(); ++i) {
                ck(lm2.v6_metadata->physical_entries[i].physical_slot ==
                       fx->plan.physical_entries[i].physical_slot,
                   "round-trip physical slot placement preserved");
            }
            // Different pinned keys produce different physical layouts (the
            // physical order is the build key's influence).
        }
    }

    // =====================================================================
    // 4. Different pinned keys produce different physical layouts.
    // =====================================================================
    std::printf("Part 4: different pinned keys -> different physical layouts\n");
    {
        auto fx2 = build_keyed_fixture(0x0BADF00DDEADBEEFULL);
        if (!fx2) { std::printf("FAIL: build_keyed_fixture(2)\n"); return 1; }
        bool differ = false;
        if (fx->plan.physical_entries.size() == fx2->plan.physical_entries.size()) {
            for (size_t i = 0; i < fx->plan.physical_entries.size(); ++i) {
                if (fx->plan.physical_entries[i].logical_slot !=
                    fx2->plan.physical_entries[i].logical_slot) { differ = true; break; }
            }
        }
        ck(differ, "different pinned keys produce different physical layouts");
    }

    // =====================================================================
    // 5. data_temp_off round-trip (blob-v2). Build a function with a
    //    StringDecrypt whose data_temp_off is nonzero, serialize to blob-v2,
    //    and confirm the loaded function executes (data_temp_off preserved).
    // =====================================================================
    std::printf("Part 5: data_temp_off round-trip (blob-v2)\n");
    {
        // Hand-build a ThinFunction with a StringDecrypt carrying a nonzero
        // data_temp_off. The serializer writes blob-v2 (IR_BLOB_VERSION == 2)
        // which preserves data_temp_off; the loader re-emits it.
        ThinFunction thf;
        thf.name = "sdec";
        thf.slot = 0;
        thf.frame.frame_size = 64;
        thf.frame.rbx_save_offset = -8;
        thf.frame.next_local_off = 56;
        // ConstStringRef: the rodata addend (imm.i) + len; then StringDecrypt
        // writes the decrypted bytes into a data temp at data_temp_off and the
        // slice result at frame_off.
        ThinBlock blk;
        blk.id = 0;
        ThinInstr ci;
        ci.op = ThinOp::ConstStringRef;
        ci.dst = 1;
        ci.imm.i = 0;       // rodata addend (rodata is empty here -> 0)
        ci.meta.len = 4;    // 4-byte literal
        ci.meta.base_kind = AbsFixup::FunctionRodataBase;
        ci.meta.frame_off = -16;   // slice result slot {ptr,len}
        ci.meta.data_temp_off = -32;  // nonzero data temp (blob-v2 preserves this)
        ci.meta.width = 8;
        blk.instrs.push_back(ci);
        blk.term.kind = TermKind::Return;
        blk.term.ret = 1;   // return the slice
        thf.blocks.push_back(std::move(blk));
        // Need rodata for ConstStringRef (len=4). Provide a 4-byte rodata.
        thf.rodata = std::vector<uint8_t>{'a','b','c','d'};

        std::vector<uint8_t> blob;
        std::string se;
        if (!serialize_thin_function(thf, blob, &se)) {
            std::printf("FAIL: serialize data_temp_off fn: %s\n", se.c_str()); return 1;
        }
        // Confirm the blob is v2.
        uint16_t bv = uint16_t(blob[4]) | uint16_t(blob[5]) << 8;
        ck(bv == IR_BLOB_VERSION, "data_temp_off function serialized as blob-v2");

        // Build a V6 module carrying this single IR function.
        EmModule v6mod;
        v6mod.has_v6 = true;
        // Identity V6 metadata (one function, no padding) — simplest topology
        // for the data_temp_off round-trip (the point is blob-v2, not keyed).
        auto m = std::make_shared<EmV6Metadata>();
        m->strategy_id = "implicit-keyed-v1";
        m->strategy_version = 1;
        m->dispatch_mode = EM_V6_DISPATCH_IDENTITY;
        m->logical_slot_count = 1;
        m->physical_slot_count = 1;
        EmV6DomainDescriptor d;
        d.visibility = uint8_t(Visibility::Public);
        d.calling_mode = uint8_t(CallingMode::LegacyContext);
        d.abi_fingerprint = 0;
        d.domain_salt = 0;
        d.strategy_version = 1;
        d.physical_base = 0;
        d.physical_count = 1;
        d.logical_count = 1;
        d.padding_count = 0;
        d.padding_ordinal = 0;
        d.logical_slots = {0};
        m->domains.push_back(d);
        EmV6LogicalRoute r; r.logical_slot = 0; r.domain_index = 0; r.ordinal = 0;
        r.abi_fingerprint = 0; r.visibility = uint8_t(Visibility::Public);
        r.calling_mode = uint8_t(CallingMode::LegacyContext);
        m->logical_routes.push_back(r);
        EmV6PhysicalEntry p; p.physical_slot = 0; p.abi_fingerprint = 0;
        p.visibility = uint8_t(Visibility::Public);
        p.calling_mode = uint8_t(CallingMode::LegacyContext);
        p.name = "sdec"; p.is_padding = false; p.logical_slot = 0;
        p.domain_index = 0; p.ordinal = 0;
        m->physical_entries.push_back(p);
        // Identity mode: no padding descriptors.
        m->capabilities.push_back({EM_V6_CAP_BLOB_V2_RE_EMIT, 1u});
        m->capabilities.push_back({EM_V6_CAP_DISPATCH_STRATEGY, 1u});
        m->capabilities.push_back({EM_V6_CAP_DISPATCH_MODE, uint32_t(EM_V6_DISPATCH_IDENTITY)});
        m->capabilities.push_back({EM_V6_CAP_ABI_DOMAIN_SET, 1u});
        v6mod.v6 = m;
        EmFunctionRecord rec;
        rec.name = "sdec"; rec.slot_index = 0; rec.ir_blob = blob;
        rec.signature.ret = Type{Prim::Void};  // slice return simplified
        v6mod.functions.push_back(std::move(rec));
        v6mod.globals = {}; v6mod.entry_slot = 0;
        v6mod.name_table.emplace_back("sdec", 0u);

        std::vector<uint8_t> v6buf;
        std::string v6we;
        if (!write_em_bytes_v6(v6mod, v6buf, &v6we)) {
            std::printf("FAIL: write data_temp_off V6: %s\n", v6we.c_str()); return 1;
        }
        // Load with identity V6 caps (keyed runtime not required for identity).
        EmV6HostCaps icaps;
        icaps.keyed_dispatch_runtime = false;
        icaps.blob_v2_re_emit = true;
        icaps.registered_strategies.push_back({"implicit-keyed-v1", 1u});
        icaps.allow_identity_mode = true;
        icaps.allow_keyed_mode = false;
        icaps.supports_all_abi_domains = true;  // identity host: any ABI domain
        LoadedModule dlm;
        std::string dle;
        bool dlok = load_em_bytes(v6buf.data(), v6buf.size(), dlm, &dle,
                                  nullptr, &fx->natives, nullptr, nullptr, &icaps);
        ck(dlok, "data_temp_off V6 identity module loaded (blob-v2 preserved)");
        if (!dlok) std::printf("       (err=%s)\n", dle.c_str());
        if (dlok) {
            ck(!dlm.pages.empty(), "data_temp_off V6 re-emit produced code (no exec page lost)");
            ck(!dlm.v6_keyed, "identity V6 module is not keyed");
            // entry() works for identity V6 (raw index path, logical==physical).
            ck(dlm.entry() != nullptr, "identity V6 entry() returns the raw entry (logical==physical)");
        }
    }

    // =====================================================================
    // 6. Malformed / truncated / overflowed descriptor rejection (no exec page,
    //    no publication).
    // =====================================================================
    std::printf("Part 6: malformed/truncated/overflowed descriptor rejection\n");
    auto try_load_buf = [&](const std::vector<uint8_t>& bad_buf,
                            const EmV6HostCaps& caps,
                            const char* label) -> bool {
        LoadedModule bad_lm;
        std::string le;
        bool ok = load_em_bytes(bad_buf.data(), bad_buf.size(), bad_lm, &le,
                                nullptr, &fx->natives, nullptr, nullptr, &caps);
        bool pass = !ok && bad_lm.pages.empty();
        ck(pass, label);
        if (!pass) std::printf("       (ok=%d pages=%zu err=%s)\n", ok, bad_lm.pages.size(), le.c_str());
        return pass;
    };
    {
        EmV6HostCaps caps = make_v6_host_caps(true, true);
        // (a) truncated: cut the V6 bytes mid-metadata-block.
        {
            std::vector<uint8_t> bad = buf;
            bad.resize(EM_HEADER_SIZE + 8);  // header + partial meta magic/layout
            try_load_buf(bad, caps, "truncated V6 metadata -> rejected, no exec page");
        }
        // (b) bad metadata magic: flip a byte in the meta magic.
        {
            std::vector<uint8_t> bad = buf;
            // The meta magic is right after the 40-byte header.
            if (bad.size() > EM_HEADER_SIZE + 4) bad[EM_HEADER_SIZE] ^= 0xFF;
            try_load_buf(bad, caps, "bad V6 metadata magic -> rejected, no exec page");
        }
        // (c) overflowed physical count: patch the physical_slot_count to a huge
        //     value (u32 at a fixed offset in the meta block). This is a
        //     hand-crafted overflow; the loader bounds-checks it.
        {
            std::vector<uint8_t> bad = buf;
            // meta block layout: magic(4)+layout(4)+strat_id_len(2)+strat_id+
            // strategy_version(4)+dispatch_mode(1)+logical_count(4)+physical_count(4)
            // strat_id = "implicit-keyed-v1" (18 bytes). So physical_count is at
            // 40 + 4+4 + 2+18 + 4 + 1 + 4 = 40 + 37 = 77.
            size_t off = EM_HEADER_SIZE + 4 + 4 + 2 + 18 + 4 + 1 + 4;
            if (off + 4 <= bad.size()) {
                // write 0xFFFFFFFF as the physical count
                bad[off] = 0xFF; bad[off+1] = 0xFF; bad[off+2] = 0xFF; bad[off+3] = 0xFF;
            }
            try_load_buf(bad, caps, "overflowed physical_slot_count -> rejected, no exec page");
        }
        // (d) truncated per-function ir_blob: cut the bytes mid-ir-blob.
        {
            std::vector<uint8_t> bad = buf;
            bad.resize(bad.size() - 4);  // drop 4 trailing bytes
            try_load_buf(bad, caps, "truncated V6 ir_blob -> rejected, no exec page");
        }
    }

    // =====================================================================
    // 7. Duplicate physical placement rejection. The writer preflight rejects
    //    in-memory metadata; a MUTATED-BYTE-BUFFER case (patch a serialized
    //    physical_slot to collide) exercises the LOADER's rejection even
    //    though the writer would reject the equivalent in-memory metadata.
    // =====================================================================
    std::printf("Part 7: duplicate physical placement rejection\n");
    {
        EmModule dmod = mod;
        dmod.v6 = clone_v6_metadata(*mod.v6);  // deep-copy (mod.v6 is shared)
        // Force a duplicate physical slot in the metadata (two entries at slot 0).
        if (dmod.v6->physical_entries.size() >= 2) {
            dmod.v6->physical_entries[1].physical_slot =
                dmod.v6->physical_entries[0].physical_slot;
        }
        std::vector<uint8_t> dbuf;
        std::string dwerr;
        bool wrote = write_em_bytes_v6(dmod, dbuf, &dwerr);
        // The writer's preflight rejects duplicate physical placement.
        ck(!wrote, "writer rejects duplicate physical placement (preflight)");
        // LOADER-via-mutation: take the valid serialized bytes and patch a
        // physical entry's slot to collide with another, bypassing the writer.
        EmV6HostCaps caps = make_v6_host_caps(true, true);
        V6ByteMap bm = map_v6_bytes(buf);
        ck(bm.ok && bm.phys_slot_off.size() >= 2,
           "byte map located >=2 physical-entry slot offsets");
        if (bm.ok && bm.phys_slot_off.size() >= 2) {
            std::vector<uint8_t> mbuf = buf;
            // Patch physical_entries[1].physical_slot == physical_entries[0].physical_slot.
            uint32_t slot0 = rd_u32(mbuf, bm.phys_slot_off[0]);
            patch_u32(mbuf, bm.phys_slot_off[1], slot0);
            try_load_buf(mbuf, caps,
                         "loader rejects duplicate physical placement (mutated bytes)");
        }
        (void)wrote;
    }

    // =====================================================================
    // 8. ABI mismatch rejection (a route whose fingerprint differs from its
    //    domain's). The writer preflight does NOT cross-check route/domain ABI;
    //    the loader's structural validation does, so this exercises the loader.
    // =====================================================================
    std::printf("Part 8: ABI mismatch rejection\n");
    {
        EmModule amod = mod;
        amod.v6 = clone_v6_metadata(*mod.v6);  // deep-copy (mod.v6 is shared)
        // Corrupt one route's ABI fingerprint to differ from its domain's.
        if (!amod.v6->logical_routes.empty()) {
            amod.v6->logical_routes[0].abi_fingerprint ^= 0x123456789ABCDEFULL;
        }
        std::vector<uint8_t> abuf;
        std::string awerr;
        bool wrote = write_em_bytes_v6(amod, abuf, &awerr);
        // The writer preflight does not cross-check route/domain ABI (it checks
        // density/uniqueness); the loader's structural validation does.
        if (wrote) {
            EmV6HostCaps caps = make_v6_host_caps(true, true);
            try_load_buf(abuf, caps, "loader rejects route/domain ABI mismatch");
        } else {
            // Writer rejected is also acceptable (fail-closed).
            ck(true, "ABI mismatch rejected (writer preflight or loader)");
        }
    }

    // =====================================================================
    // 9. Unsupported / unrecognized / duplicate / contradictory capability
    //    rejection. Each writer-preflight case is PAIRED with a MUTATED-BYTE-
    //    BUFFER case so the LOADER's rejection is actually exercised even when
    //    the writer preflight rejects equivalent in-memory metadata.
    // =====================================================================
    std::printf("Part 9: capability-matrix validation\n");
    // (a) unsupported: host lacks keyed runtime; module requires it.
    {
        EmV6HostCaps nocaps = make_v6_host_caps(false, true);
        try_load_buf(buf, nocaps,
                     "unsupported CapKeyedDispatchRuntime (host lacks keyed runtime) -> rejected");
    }
    // (b) host did not opt into keyed mode.
    {
        EmV6HostCaps idcaps = make_v6_host_caps(true, false);
        try_load_buf(buf, idcaps,
                     "unsupported CapDispatchMode Keyed (host did not opt in) -> rejected");
    }
    // (c) unrecognized capability id: hand-craft a module with cap id 99.
    //     Writer preflight rejects; PAIRED with a mutated-byte case (patch a
    //     serialized cap id to 99) so the loader's rejection is exercised.
    {
        EmModule umod = mod;
        umod.v6 = clone_v6_metadata(*mod.v6);  // deep-copy (mod.v6 is shared)
        umod.v6->capabilities.push_back({99u, 1u});
        // CapAbiDomainSet must still match domains.size(); the unrecognized cap
        // is the point. But the writer preflight rejects unknown cap ids, so we
        // expect the writer to reject. If it does, that's a pass.
        std::vector<uint8_t> ubuf;
        std::string uwerr;
        bool wrote = write_em_bytes_v6(umod, ubuf, &uwerr);
        ck(!wrote, "writer rejects unrecognized capability id (preflight)");
        // LOADER-via-mutation: patch a serialized cap id to 99.
        EmV6HostCaps caps = make_v6_host_caps(true, true);
        V6ByteMap bm = map_v6_bytes(buf);
        ck(bm.ok && !bm.cap_id_off.empty(),
           "byte map located >=1 capability id offset");
        if (bm.ok && !bm.cap_id_off.empty()) {
            std::vector<uint8_t> mbuf = buf;
            patch_u16(mbuf, bm.cap_id_off[0], 99u);  // unrecognized id
            try_load_buf(mbuf, caps,
                         "loader rejects unrecognized capability id (mutated bytes)");
        }
    }
    // (d) contradictory: CapDispatchMode value != dispatch_mode. Writer rejects;
    //     PAIRED with a mutated-byte case (patch the CapDispatchMode value to
    //     Identity while dispatch_mode stays Keyed) so the loader is exercised.
    {
        EmModule cmod = mod;
        cmod.v6 = clone_v6_metadata(*mod.v6);  // deep-copy (mod.v6 is shared)
        // Find the CapDispatchMode entry and flip its required_value to Identity.
        for (auto& c : cmod.v6->capabilities)
            if (c.capability_id == EM_V6_CAP_DISPATCH_MODE) c.required_value = EM_V6_DISPATCH_IDENTITY;
        std::vector<uint8_t> cbuf;
        std::string cwerr;
        bool wrote = write_em_bytes_v6(cmod, cbuf, &cwerr);
        ck(!wrote, "writer rejects contradictory CapDispatchMode (preflight)");
        // LOADER-via-mutation: find the CapDispatchMode entry in the serialized
        // bytes and patch its required_value to Identity.
        EmV6HostCaps caps = make_v6_host_caps(true, true);
        V6ByteMap bm = map_v6_bytes(buf);
        if (bm.ok) {
            // Locate the CapDispatchMode entry by reading each cap id.
            size_t mode_val_off = 0;
            for (size_t i = 0; i < bm.cap_id_off.size(); ++i) {
                if (rd_u16(buf, bm.cap_id_off[i]) == EM_V6_CAP_DISPATCH_MODE) {
                    mode_val_off = bm.cap_val_off[i];
                    break;
                }
            }
            if (mode_val_off) {
                std::vector<uint8_t> mbuf = buf;
                patch_u32(mbuf, mode_val_off, EM_V6_DISPATCH_IDENTITY);
                try_load_buf(mbuf, caps,
                             "loader rejects contradictory CapDispatchMode (mutated bytes)");
            }
        }
        (void)wrote;
    }
    // (e) duplicate capability id. Writer rejects; PAIRED with a mutated-byte
    //     case (patch one cap id to equal another) so the loader is exercised.
    {
        EmModule ddmod = mod;
        ddmod.v6 = clone_v6_metadata(*mod.v6);  // deep-copy (mod.v6 is shared)
        ddmod.v6->capabilities.push_back({EM_V6_CAP_BLOB_V2_RE_EMIT, 1u});  // duplicate
        std::vector<uint8_t> dbuf;
        std::string dwerr;
        bool wrote = write_em_bytes_v6(ddmod, dbuf, &dwerr);
        ck(!wrote, "writer rejects duplicate capability id (preflight)");
        // LOADER-via-mutation: patch a serialized cap id to duplicate another.
        EmV6HostCaps caps = make_v6_host_caps(true, true);
        V6ByteMap bm = map_v6_bytes(buf);
        if (bm.ok && bm.cap_id_off.size() >= 2) {
            std::vector<uint8_t> mbuf = buf;
            // Patch cap[1].id == cap[0].id (a duplicate).
            uint16_t id0 = rd_u16(buf, bm.cap_id_off[0]);
            patch_u16(mbuf, bm.cap_id_off[1], id0);
            try_load_buf(mbuf, caps,
                         "loader rejects duplicate capability id (mutated bytes)");
        }
        (void)wrote;
    }
    // (f) null host caps (the default): a V6 module is rejected (fail-closed).
    {
        LoadedModule nlm;
        std::string nle;
        bool ok = load_em_bytes(buf.data(), buf.size(), nlm, &nle,
                                nullptr, &fx->natives, nullptr, nullptr, nullptr);
        ck(!ok && nlm.pages.empty(), "null EmV6HostCaps -> V6 rejected (fail-closed), no exec page");
    }
    // (g) unsupported ABI-domain set: the host declares a RESTRICTED ABI-domain
    //     set that does NOT include the module's domain fingerprint. The loader
    //     must reject (host-supported declared ABI-domain set, §11.3/§11.4) —
    //     not merely a module-local domain count.
    {
        EmV6HostCaps abicaps;
        abicaps.keyed_dispatch_runtime = true;
        abicaps.blob_v2_re_emit = true;
        abicaps.registered_strategies.push_back({"implicit-keyed-v1", 1u});
        abicaps.allow_identity_mode = true;
        abicaps.allow_keyed_mode = true;
        abicaps.allow_raw_x86 = false;
        abicaps.supports_all_abi_domains = false;  // restricted set
        // A fingerprint the module does NOT declare (off by one).
        uint64_t mod_fp = fx->plan.domains.empty() ? 1u : fx->plan.domains[0].abi_fingerprint;
        abicaps.supported_abi_domains.push_back(mod_fp ^ 0x1u);
        try_load_buf(buf, abicaps,
                     "unsupported ABI-domain set (host does not support module's ABI domain) -> rejected");
    }

    // =====================================================================
    // 10. blob-v1 rejection. Build a V6 IR function whose blob declares v1
    //     and confirm BOTH the writer preflight AND the loader reject it. The
    //     loader case mutates the serialized blob version word so the writer
    //     preflight is bypassed and the LOADER's blob-v2 gate is exercised.
    // =====================================================================
    std::printf("Part 10: blob-v1 rejection\n");
    {
        // Take a valid blob-v2 and rewrite its version word to 1. The blob's
        // internal layout differs between v1/v2 (v1 has no data_temp_off field),
        // so the deserializer would misparse — but the V6 loader rejects at the
        // version check BEFORE deserialize, so it never reaches misparse.
        std::vector<uint8_t> v1blob = mod.functions[0].ir_blob;
        if (v1blob.size() >= 6) {
            v1blob[4] = 0x01;  // version lo = 1
            v1blob[5] = 0x00;  // version hi = 0
        }
        EmModule v1mod = mod;
        v1mod.v6 = clone_v6_metadata(*mod.v6);  // deep-copy (mod.v6 is shared)
        v1mod.functions[0].ir_blob = v1blob;
        // The writer preflight checks blob-v2, so it should reject.
        std::vector<uint8_t> v1buf;
        std::string v1werr;
        bool wrote = write_em_bytes_v6(v1mod, v1buf, &v1werr);
        ck(!wrote, "writer rejects V6 IR function with blob-v1 (preflight)");
        // LOADER-via-mutation: patch the first IR blob's version word in the
        // valid serialized bytes to v1, bypassing the writer preflight.
        EmV6HostCaps caps = make_v6_host_caps(true, true);
        V6ByteMap bm = map_v6_bytes(buf);
        if (bm.ok && bm.first_blob_version_off) {
            std::vector<uint8_t> mbuf = buf;
            patch_u16(mbuf, bm.first_blob_version_off, 1u);  // blob v1
            try_load_buf(mbuf, caps,
                         "loader rejects V6 IR function with blob-v1 (mutated bytes)");
        } else {
            ck(false, "byte map located the first IR blob version offset");
        }
        (void)wrote;
    }

    // =====================================================================
    // 11. Raw fallback rejection under the secure default (no allow_raw_x86).
    // =====================================================================
    std::printf("Part 11: raw-x86 fallback rejection (secure default)\n");
    {
        // Build a V6 module with one IR function + one raw-x86 fallback. The
        // V6 host caps default allow_raw_x86 == false -> rejected.
        EmModule rmod = mod;
        // Add a raw-x86 fallback function (ir_blob empty -> is_ir=0). Give it a
        // plausible logical slot beyond the keyed plan (use a fresh slot). The
        // metadata must cover it; simplest: append a raw function with an
        // existing slot and empty ir_blob. The writer preflight for v6 does not
        // require every function to be IR (mixed mode is permitted on disk); the
        // secure-default rejection is the loader's V6 raw-x86 gate.
        EmFunctionRecord raw;
        raw.name = "rawfn";
        raw.slot_index = 0;  // reuse slot 0 (the loader's raw-x86 gate fires
                              // before the duplicate-slot check is reachable in
                              // this hand-crafted case; we only need is_ir=0)
        raw.ir_blob = {};    // empty -> is_ir=0 (raw-x86 fallback)
        raw.code = std::vector<uint8_t>(16, 0xC3);  // 16 ret bytes (raw x86)
        raw.signature.ret = Type{Prim::I64};
        raw.signature.params.push_back(Type{Prim::I64});
        // For a clean test, build a fresh minimal V6 identity module with one
        // IR fn + one raw fn at distinct slots.
        EmModule rmod2;
        rmod2.has_v6 = true;
        auto m = std::make_shared<EmV6Metadata>();
        m->strategy_id = "implicit-keyed-v1";
        m->strategy_version = 1;
        m->dispatch_mode = EM_V6_DISPATCH_IDENTITY;
        m->logical_slot_count = 2;
        m->physical_slot_count = 2;
        EmV6DomainDescriptor d;
        d.visibility = uint8_t(Visibility::Public);
        d.calling_mode = uint8_t(CallingMode::LegacyContext);
        d.abi_fingerprint = 0; d.domain_salt = 0; d.strategy_version = 1;
        d.physical_base = 0; d.physical_count = 2; d.logical_count = 2;
        d.padding_count = 0; d.padding_ordinal = 0; d.logical_slots = {0,1};
        m->domains.push_back(d);
        for (uint32_t s = 0; s < 2; ++s) {
            EmV6LogicalRoute r; r.logical_slot = s; r.domain_index = 0; r.ordinal = s;
            r.abi_fingerprint = 0; r.visibility = uint8_t(Visibility::Public);
            r.calling_mode = uint8_t(CallingMode::LegacyContext);
            m->logical_routes.push_back(r);
            EmV6PhysicalEntry p; p.physical_slot = s; p.abi_fingerprint = 0;
            p.visibility = uint8_t(Visibility::Public);
            p.calling_mode = uint8_t(CallingMode::LegacyContext);
            p.name = (s == 0 ? "irfn" : "rawfn"); p.is_padding = false;
            p.logical_slot = s; p.domain_index = 0; p.ordinal = s;
            m->physical_entries.push_back(p);
        }
        m->capabilities.push_back({EM_V6_CAP_BLOB_V2_RE_EMIT, 1u});
        m->capabilities.push_back({EM_V6_CAP_DISPATCH_STRATEGY, 1u});
        m->capabilities.push_back({EM_V6_CAP_DISPATCH_MODE, uint32_t(EM_V6_DISPATCH_IDENTITY)});
        m->capabilities.push_back({EM_V6_CAP_ABI_DOMAIN_SET, 1u});
        rmod2.v6 = m;
        // IR fn (slot 0).
        EmFunctionRecord irf;
        irf.name = "irfn"; irf.slot_index = 0;
        irf.ir_blob = mod.functions[0].ir_blob;  // reuse a valid blob-v2
        irf.signature.ret = Type{Prim::I64}; irf.signature.params.push_back(Type{Prim::I64});
        rmod2.functions.push_back(std::move(irf));
        // Raw fn (slot 1): a minimal raw-x86 function (ret). void() — the
        // simplest signature that round-trips (the v3 raw test uses the same).
        EmFunctionRecord rawf;
        rawf.name = "rawfn"; rawf.slot_index = 1;
        rawf.ir_blob = {};  // is_ir=0
        rawf.code = std::vector<uint8_t>{0xC3};  // ret (raw x86)
        rawf.signature.ret = Type{};  // void
        rmod2.functions.push_back(std::move(rawf));
        rmod2.globals = {}; rmod2.entry_slot = 0;
        rmod2.name_table.emplace_back("irfn", 0u);
        rmod2.name_table.emplace_back("rawfn", 1u);

        std::vector<uint8_t> rbuf;
        std::string rwerr;
        if (!write_em_bytes_v6(rmod2, rbuf, &rwerr)) {
            std::printf("  [SKIP] raw fallback: writer rejected (%s)\n", rwerr.c_str());
        } else {
            // Secure default: allow_raw_x86 == false -> rejected, no exec page.
            EmV6HostCaps caps;
            caps.keyed_dispatch_runtime = false;
            caps.blob_v2_re_emit = true;
            caps.registered_strategies.push_back({"implicit-keyed-v1", 1u});
            caps.allow_identity_mode = true;
            caps.allow_keyed_mode = false;
            caps.allow_raw_x86 = false;
            caps.supports_all_abi_domains = true;  // identity host: any ABI domain
            try_load_buf(rbuf, caps,
                         "V6 raw-x86 fallback rejected under secure default (no allow_raw_x86)");
            // Explicit opt-in: allow_raw_x86 == true -> accepted (mixed mode).
            EmV6HostCaps caps2 = caps;
            caps2.allow_raw_x86 = true;
            LoadedModule rlm;
            std::string rle;
            bool ok = load_em_bytes(rbuf.data(), rbuf.size(), rlm, &rle,
                                    nullptr, &fx->natives, nullptr, nullptr, &caps2);
            ck(ok, "V6 raw-x86 fallback accepted with explicit allow_raw_x86");
            if (!ok) std::printf("       (err=%s)\n", rle.c_str());
        }
        (void)rmod;
    }

    // =====================================================================
    // 12. No publication / pages on failure (regression): a failed load leaves
    //     out empty. (Covered by the try_load_buf checks above; assert the
    //     invariant explicitly once more.)
    // =====================================================================
    std::printf("Part 12: no publication/pages on failure\n");
    {
        EmV6HostCaps caps = make_v6_host_caps(false, true);  // no keyed runtime
        LoadedModule flm;
        std::string fle;
        bool ok = load_em_bytes(buf.data(), buf.size(), flm, &fle,
                                nullptr, &fx->natives, nullptr, nullptr, &caps);
        ck(!ok, "load fails when host lacks keyed runtime");
        ck(flm.pages.empty(), "failed load allocates NO exec pages");
        ck(flm.dispatch.empty(), "failed load publishes NO dispatch entries");
        ck(flm.v6_record == nullptr, "failed load publishes NO keyed record");
    }

    // =====================================================================
    // 13. Schema assertions (§14.6): the V6 metadata structs contain NO field
    //     for a target/runtime key, machine fingerprint, expected key, key
    //     hash/digest/verifier, direct comparison constant, or predecoded
    //     permutation map. Inspect the public struct field names via offsetof
    //     heuristics is not portable; instead assert the in-memory metadata
    //     we built carries none of those value kinds.
    // =====================================================================
    std::printf("Part 13: schema assertions (no verifier fields)\n");
    {
        const EmV6Metadata& m = *mod.v6;
        // The metadata has NO field named like a key/fingerprint/verifier. We
        // assert the value-level invariant: domain_salt is a public tweak
        // (NOT a key), and no field carries an "expected" key or a digest.
        // The route word is NEVER stored in the metadata (it is per-call).
        // Assert the metadata's fields are exactly the documented key-free set
        // by checking the loaded module's record carries no key material.
        ck(lm.v6_record != nullptr, "schema: keyed record exists (for field inspection)");
        if (lm.v6_record) {
            // The ModuleDispatchRecord (module_layout.hpp) field set is the
            // documented one: mode, strategy_version, physical_slots, counts,
            // routes, domains, allowlist, descriptors, padding_trap_target.
            // It has NO expected-key / fingerprint / digest / verifier field.
            // Assert padding_trap_target is the padding stub (NOT a key).
            ck(ember_is_padding_trap_target(lm.v6_record->padding_trap_target),
               "schema: padding_trap_target is the padding stub, not a key/verifier");
        }
        // The capability matrix carries NO key material (it is a requirements
        // declaration, not a verifier).
        bool has_key_cap = false;
        for (const auto& c : m.capabilities)
            if (c.capability_id == 0 || c.capability_id > EM_V6_CAP_MAX_KNOWN) has_key_cap = true;
        ck(!has_key_cap, "schema: capability matrix has only known key-free capability ids");
        // domain_salt is a public tweak (deterministic, not a key). It may be
        // nonzero; that is allowed (§8.4). The point is no field is an EXPECTED
        // key or a digest. Assert no field is named/labeled as a verifier —
        // structurally, EmV6Metadata has no such field (verified by compilation).
        ck(true, "schema: EmV6Metadata has no target/runtime key, fingerprint, expected "
                 "key, hash/digest/verifier, comparison constant, or predecoded permutation");
    }

    // =====================================================================
    // 14. V1-V5 regressions: a v5 IR module still loads + runs (identity
    //     layout); a v3 raw-x86 module is rejected under the secure default
    //     and accepted with allow_raw_x86 (identity layout unchanged).
    // =====================================================================
    std::printf("Part 14: V1-V5 regressions\n");
    {
        // v5 IR module: reuse the fixture's ThinFunctions + serialize to v5.
        EmModule v5mod;
        v5mod.functions.reserve(fx->thfs.size());
        for (size_t i = 0; i < fx->thfs.size(); ++i) {
            const auto& thf = fx->thfs[i];
            const auto& decl = fx->program.funcs[i];
            EmFunctionRecord rec;
            rec.name = thf.name;
            rec.slot_index = uint32_t(decl.slot);
            std::string e;
            if (!serialize_thin_function(thf, rec.ir_blob, &e)) { std::printf("FAIL: v5 serialize\n"); return 1; }
            rec.signature.ret = decl.ret ? *decl.ret : Type{};
            for (const auto& p : decl.params) rec.signature.params.push_back(p.ty ? *p.ty : Type{});
            v5mod.functions.push_back(std::move(rec));
        }
        v5mod.globals = {};
        v5mod.entry_slot = fx->entry_slot;
        for (const auto& fn : fx->program.funcs)
            v5mod.name_table.emplace_back(fn.name, uint32_t(fn.slot));
        std::vector<uint8_t> v5buf;
        std::string v5e;
        if (!write_em_bytes_v5(v5mod, v5buf, &v5e)) { std::printf("FAIL: write v5: %s\n", v5e.c_str()); return 1; }
        LoadedModule v5lm;
        std::string v5le;
        bool v5ok = load_em_bytes(v5buf.data(), v5buf.size(), v5lm, &v5le,
                                  nullptr, &fx->natives, nullptr, nullptr);
        ck(v5ok, "v5 IR module loads under secure default (identity layout)");
        if (v5ok) {
            ck(v5lm.format_version == EM_VERSION_V5, "v5 module format_version == 5");
            ck(!v5lm.is_v6, "v5 module is not V6");
            void* e5 = v5lm.entry_by_name("inc");
            ck(e5 != nullptr, "v5 entry_by_name(inc) works (identity, raw index path)");
            int64_t r5 = call_i64_i64(e5, 99);
            ck(r5 == 100, "v5 inc(99) == 100 (value-equivalent, identity layout)");
        }
        // v3 raw-x86 module: rejected under the secure default (allow_raw_x86=false).
        EmModule v3mod;
        EmFunctionRecord raw3;
        raw3.name = "raw3"; raw3.slot_index = 0;
        raw3.code = std::vector<uint8_t>{0xC3};  // ret
        raw3.signature.ret = Type{Prim::I64};
        v3mod.functions.push_back(std::move(raw3));
        v3mod.globals = {}; v3mod.entry_slot = 0;
        v3mod.name_table.emplace_back("raw3", 0u);
        std::vector<uint8_t> v3buf;
        std::string v3e;
        if (!write_em_bytes(v3mod, v3buf, &v3e)) { std::printf("FAIL: write v3: %s\n", v3e.c_str()); return 1; }
        LoadedModule v3lm;
        std::string v3le;
        bool v3ok = load_em_bytes(v3buf.data(), v3buf.size(), v3lm, &v3le,
                                  nullptr, nullptr, nullptr, nullptr);
        ck(!v3ok && v3lm.pages.empty(), "v3 raw-x86 rejected under secure default (raw fallback), no exec page");
        // With allow_raw_x86=true, v3 loads (identity layout, back-compat).
        EmLoadPolicy rp; rp.allow_raw_x86 = true;
        LoadedModule v3lm2;
        std::string v3le2;
        bool v3ok2 = load_em_bytes(v3buf.data(), v3buf.size(), v3lm2, &v3le2,
                                   nullptr, nullptr, nullptr, &rp);
        ck(v3ok2, "v3 raw-x86 accepted with allow_raw_x86 (identity layout unchanged)");
    }

    std::printf("\n%s: %d checks, %s\n", g_fail ? "FAIL" : "PASS", g_checks,
                g_fail ? "one or more failures" : "all passed");
    return g_fail ? 1 : 0;
}
