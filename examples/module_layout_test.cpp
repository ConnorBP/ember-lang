// module_layout_test — Red 3 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
// §5, §9.2, §14.3): logical/physical module layout planning.
//
// RED-GREEN contract chunk for the module layout planner. Unlike a synthetic
// fixture, this test drives the REAL Ember front end: it tokenizes, parses,
// assigns stable logical slots, builds struct layouts, and runs sema over an
// actual Ember source module. Each ModuleCallable is then DERIVED from the
// semantically resolved function ABI through the canonical Red 2 classifier
// (dispatch_abi.hpp classify_callable) — the abi_fingerprint is never a
// hand-written arbitrary hash. The fixture module contains several same-ABI
// functions plus singleton domains produced by differing ABI, public/private
// visibility, and an explicit @dispatch_domain("...") label.
//
// Against an INDEPENDENT oracle (a from-scratch reimplementation of the
// salt/route-word derivation spec written here in the test), this pins:
//   - canonical classifier:    fingerprints come from classify_callable.
//   - domain grouping:          group by (module id, visibility, calling mode,
//                             exact ABI fingerprint, optional label); same-ABI
//                             functions share a domain; singleton domains arise
//                             from differing ABI / visibility / label.
//   - golden placements:        under two pinned target route words, hard-coded
//                             golden logical-to-physical placements (cross-
//                             checked against the oracle).
//   - deterministic builds:    same target key reproduces the physical layout.
//   - different keyed layouts: the two pinned keys produce different layouts.
//   - dense logical identity:  logical routes cover [0,N) exactly once.
//   - isolation:               exact ABI/visibility/calling-mode/domain-label
//                             isolation across domains; domain module ids match
//                             the plan module id; route metadata matches its
//                             domain (validated).
//   - padding:                exactly one ABI-compatible padding entry per keyed
//                             domain; a complete PaddingDescriptor per domain;
//                             padding metadata matches its domain.
//   - singleton physical 2:   singleton real-function domains have physical
//                             count two (1 real + 1 padding).
//   - wrong-key safety:        for many wrong route words, every recomputed
//                             physical destination stays inside its domain and
//                             selects metadata with the same ABI fingerprint,
//                             visibility, calling mode, and explicit label —
//                             per-domain, NOT only a global contiguous-range
//                             assertion.
//   - negatives:               duplicate/non-dense logical slots, missing routes,
//                             overlapping/OOB placements, missing/duplicate
//                             padding, inconsistent counts, cross-ABI/
//                             cross-visibility membership, oversized domains,
//                             and empty modules (keyed plan REJECTS an empty
//                             module — strict negative).
//   - disabled/unkeyed:        identity layout — slot i -> physical i, counts
//                             match, no keyed padding, existing DispatchTable
//                             behavior untouched; indexes callables by
//                             logical_slot (a dense-but-shuffled manifest
//                             preserves correct callable metadata).
//   - registry:               configured KeyedDispatchRegistry for
//                             implicit-keyed-v1: reject empty names, null
//                             factories, duplicates without replacement; return
//                             fresh configured instances; structured
//                             diagnostics; deterministic name listing.
//
// Scope (§14.3 Red 3): layout metadata/planning only. No JIT call lowering,
// V6, hot reload, threads, or the Red 4 resolver.
//
// Links ember (keyed_dispatch.* — Red 1 helper) + ember_frontend (lexer/parser/
// sema — the real front end; dispatch_abi.* — Red 2 classifier; module_layout.*
// — Red 3 planner).

#include "../src/module_layout.hpp"
#include "../src/keyed_dispatch.hpp"
#include "../src/dispatch_abi.hpp"
#include "../src/extension_registry.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

// ===========================================================================
// Test harness
// ===========================================================================
static int g_fail = 0;
static int g_checks = 0;
static void ck(bool c, const char* m) {
    ++g_checks;
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// ===========================================================================
// Independent oracle — reimplements the salt/route-word derivation spec.
//
// Spec (must match src/module_layout.* exactly):
//   derive_route_word(key):   FNV-1a-64 over the key bytes (offset basis).
//   derive_domain_salt(module_id, visibility, calling_mode, abi_fingerprint,
//                      dispatch_domain, strategy_version):
//     B = u32_le(len(module_id)) || module_id bytes
//         || u8(visibility) || u8(calling_mode)
//         || u64_le(abi_fingerprint)
//         || u32_le(len(dispatch_domain)) || dispatch_domain bytes
//         || u32_le(strategy_version)
//     h = FNV-1a-64 over B (offset basis)
// NO std::hash, NO native struct bytes, NO unordered iteration. The oracle
// delegates placement to the already (Red 1) independently-verified production
// keyed_dispatch_permute.
// ===========================================================================
static uint64_t oracle_fnv1a(const uint8_t* data, size_t n, uint64_t h) {
    const uint64_t prime = 0x100000001b3ULL;
    for (size_t i = 0; i < n; ++i) { h ^= static_cast<uint64_t>(data[i]); h *= prime; }
    return h;
}
static uint64_t oracle_route_word(const uint8_t* key, size_t sz) {
    return oracle_fnv1a(key, sz, 0xcbf29ce484222325ULL);
}
static void oracle_put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}
static void oracle_put_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}
static uint64_t oracle_domain_salt(const std::string& module_id, Visibility vis,
                                    CallingMode mode, uint64_t fp,
                                    const std::string& label, uint32_t ver) {
    std::vector<uint8_t> b;
    oracle_put_u32(b, uint32_t(module_id.size()));
    for (char c : module_id) b.push_back(uint8_t(c));
    b.push_back(uint8_t(vis));
    b.push_back(uint8_t(mode));
    oracle_put_u64(b, fp);
    oracle_put_u32(b, uint32_t(label.size()));
    for (char c : label) b.push_back(uint8_t(c));
    oracle_put_u32(b, ver);
    return oracle_fnv1a(b.data(), b.size(), 0xcbf29ce484222325ULL);
}
static uint32_t oracle_place(uint64_t route_word, uint64_t salt, uint32_t ver,
                             uint32_t n, uint32_t ordinal) {
    KeyedDispatchDomain d{salt, ver, n};
    auto r = keyed_dispatch_permute(route_word, d, ordinal);
    return r ? *r.value : 0xFFFFFFFFu;
}

// ===========================================================================
// Ember Type -> canonical AbiType (the semantic bridge).
//
// Converts a sema-resolved ember::Type into the self-contained AbiType the Red
// 2 classifier consumes, using the StructLayoutTable for struct field layout.
// Recursive; no Type::to_string, no native struct bytes, no pointer values.
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

static AbiType type_to_abi(const Type& t, const StructLayoutTable* structs);

static AbiType struct_to_abi(const std::string& name, const StructLayoutTable* structs) {
    AbiType at;
    at.kind = TypeKind::Struct;
    at.struct_name = name;
    if (structs) {
        auto it = structs->find(name);
        if (it != structs->end()) {
            at.struct_size = uint32_t(it->second.size);
            at.struct_alignment = it->second.alignment;
            for (const std::string& fn : it->second.field_names) {
                auto fit = it->second.fields.find(fn);
                AbiStructField af;
                af.name = fn;
                af.offset = uint32_t(fit != it->second.fields.end() ? fit->second.offset : 0);
                af.type = std::make_shared<AbiType>(
                    fit != it->second.fields.end()
                        ? type_to_abi(*fit->second.ty, structs)
                        : abi_scalar(PrimTag::Void));
                at.fields.push_back(std::move(af));
            }
            return at;
        }
    }
    at.struct_size = 0;
    at.struct_alignment = 1;
    return at;
}

static AbiType type_to_abi(const Type& t, const StructLayoutTable* structs) {
    if (t.is_slice) {
        AbiType at;
        at.kind = TypeKind::Slice;
        at.elem = std::make_shared<AbiType>(t.elem ? type_to_abi(*t.elem, structs)
                                                   : abi_scalar(PrimTag::Void));
        return at;
    }
    if (t.is_lambda) {
        AbiType at;
        at.kind = TypeKind::Lambda;
        at.has_recorded_sig = t.has_recorded_sig;
        if (t.has_recorded_sig) {
            for (auto& p : t.recorded_params)
                at.recorded_params.push_back(std::make_shared<AbiType>(
                    p ? type_to_abi(*p, structs) : abi_scalar(PrimTag::Void)));
            at.recorded_ret = std::make_shared<AbiType>(
                t.recorded_ret ? type_to_abi(*t.recorded_ret, structs)
                               : abi_scalar(PrimTag::Void));
        }
        return at;
    }
    if (t.is_fn_handle) {
        AbiType at;
        at.kind = TypeKind::FnHandle;
        at.struct_name = t.struct_name; // nominal handle identity (may be empty)
        at.is_cross_module_handle = t.is_cross_module_handle;
        at.has_recorded_sig = t.has_recorded_sig;
        if (t.has_recorded_sig) {
            for (auto& p : t.recorded_params)
                at.recorded_params.push_back(std::make_shared<AbiType>(
                    p ? type_to_abi(*p, structs) : abi_scalar(PrimTag::Void)));
            at.recorded_ret = std::make_shared<AbiType>(
                t.recorded_ret ? type_to_abi(*t.recorded_ret, structs)
                               : abi_scalar(PrimTag::Void));
        }
        return at;
    }
    if (t.array_len > 0) {
        AbiType at;
        at.kind = TypeKind::Array;
        at.array_len = t.array_len;
        at.elem = std::make_shared<AbiType>(t.elem ? type_to_abi(*t.elem, structs)
                                                   : abi_scalar(PrimTag::Void));
        return at;
    }
    if (!t.struct_name.empty()) {
        return struct_to_abi(t.struct_name, structs);
    }
    return prim_to_abi(t.prim);
}

// words_for_type — matches codegen's Win64 word count: 1 for scalar/float/
// handle, 2 for slice/lambda, ceil(size/8) for a registered struct.
static uint32_t words_for_type(const Type& t, const StructLayoutTable* structs) {
    if (t.is_slice || t.is_lambda) return 2;
    if (!t.struct_name.empty() && structs) {
        auto it = structs->find(t.struct_name);
        if (it != structs->end()) return uint32_t((it->second.size + 7) / 8);
    }
    return 1;
}

static bool type_is_struct_return(const Type& t, const StructLayoutTable* structs) {
    return !t.struct_name.empty() && structs && structs->count(t.struct_name) != 0;
}

// Build a CallableDescriptor from a sema-resolved FuncDecl, computing the
// per-param Win64 word placement (word_count, byte_extent,
// partial_final_word_bytes, per-word classes, start_position), the return
// kind, and the hidden-word flags — exactly the ABI shape codegen lowers.
static CallableDescriptor func_to_descriptor(const FuncDecl& f,
                                              const StructLayoutTable* structs) {
    CallableDescriptor d;
    d.arch = TargetArch::X64_Win64;
    d.convention_version = kWin64ConventionVersion;
    d.calling_mode = CallingMode::LegacyContext; // these are legacy-context entries
    d.visibility = f.is_exported ? Visibility::Public : Visibility::Private;

    // Return kind + hidden return pointer.
    const Type& rt = f.ret ? *f.ret : type_void();
    d.return_type = type_to_abi(rt, structs);
    if (rt.is_void()) {
        d.return_kind = ReturnKind::Void;
    } else if (type_is_struct_return(rt, structs)) {
        d.return_kind = ReturnKind::HiddenAggregate;
        d.has_hidden_return = true;
    } else if (rt.is_float()) {
        d.return_kind = (rt.prim == Prim::F64) ? ReturnKind::XmmF64 : ReturnKind::XmmF32;
    } else if (rt.is_slice) {
        d.return_kind = ReturnKind::SlicePair;
    } else if (rt.is_lambda) {
        d.return_kind = ReturnKind::LambdaPair;
    } else if (rt.is_fn_handle) {
        d.return_kind = ReturnKind::GpScalar;
    } else {
        d.return_kind = ReturnKind::GpScalar; // i*/u*/bool
    }

    d.is_coroutine = f.is_coroutine;
    d.is_special_entry = false;
    // A lambda fn has a hidden __env pointer param (params[0]); codegen spills
    // it as the first word. Flag it so the declared params' start_positions
    // account for the hidden env word.
    d.has_hidden_env = f.is_lambda;

    // Per-param Win64 word placement.
    uint32_t pos = (d.has_hidden_return ? 1u : 0u) + (d.has_hidden_env ? 1u : 0u);
    // For a lambda, params[0] is the hidden __env; skip it (it is NOT a
    // declared/script-visible param and is represented by has_hidden_env).
    size_t begin = f.is_lambda ? 1u : 0u;
    for (size_t i = begin; i < f.params.size(); ++i) {
        const Type& pt = *f.params[i].ty;
        AbiType at = type_to_abi(pt, structs);
        uint32_t wc = words_for_type(pt, structs);
        // byte_extent = the param's true byte footprint.
        uint32_t byte_ext;
        if (pt.is_slice || pt.is_lambda) byte_ext = 16;
        else if (!pt.struct_name.empty() && structs) {
            auto it = structs->find(pt.struct_name);
            byte_ext = (it != structs->end()) ? uint32_t(it->second.size) : wc * 8u;
        } else if (pt.is_float()) byte_ext = (pt.prim == Prim::F64) ? 8u : 4u;
        else if (pt.prim == Prim::Bool) byte_ext = 1u;
        else if (pt.prim == Prim::I8 || pt.prim == Prim::U8) byte_ext = 1u;
        else if (pt.prim == Prim::I16 || pt.prim == Prim::U16) byte_ext = 2u;
        else if (pt.prim == Prim::I32 || pt.prim == Prim::U32) byte_ext = 4u;
        else byte_ext = 8u; // i64/u64/f64/handle
        // partial_final_word_bytes: bytes used in the final word (0 if full).
        uint32_t partial = 0;
        if (wc > 0 && byte_ext != wc * 8u) {
            partial = byte_ext - (wc - 1) * 8u;
        }
        // Per-word classes: every word a struct/slice/lambda occupies is GP;
        // a scalar float word is XmmF32/XmmF64; everything else GP.
        std::vector<WordClass> wcs;
        WordClass wc0 = word_class_for_type(at);
        for (uint32_t w = 0; w < wc; ++w) wcs.push_back(wc0);
        AbiParam ap = abi_param(std::move(at), wc, byte_ext, partial, std::move(wcs), pos);
        d.params.push_back(std::move(ap));
        pos += wc;
    }
    return d;
}

// ===========================================================================
// Fixture: a real Ember source module.
//
// Module "test.mod" has 6 callables across 4 dispatch domains:
//   D0: add1/add2/add3 — public,  LegacyContext, i64->i64, ""               (3 real, 4 phys)
//   D1: secret         — private, LegacyContext, i64->i64, ""               (1 real, 2 phys) singleton (visibility)
//   D2: fma            — public,  LegacyContext, (f32,f32)->f32, ""         (1 real, 2 phys) singleton (ABI)
//   D3: transform      — public,  LegacyContext, i64->i64, "critical-math"  (1 real, 2 phys) singleton (label; same ABI as D0)
//
// logical_slot_count = 6, physical_slot_count = 4+2+2+2 = 10.
// ===========================================================================
static const char* kFixtureSrc =
    "fn add1(a: i64) -> i64 { return a + 1; }\n"
    "fn add2(a: i64) -> i64 { return a + 2; }\n"
    "fn add3(a: i64) -> i64 { return a + 3; }\n"
    "priv fn secret(a: i64) -> i64 { return a; }\n"
    "fn fma(a: f32, b: f32) -> f32 { return a + b; }\n"
    "@dispatch_domain(\"critical-math\")\n"
    "fn transform(a: i64) -> i64 { return a * 2; }\n";

struct ParsedModule {
    Program program;
    std::unordered_map<std::string, int> slots;
    StructLayoutTable struct_layouts;
    std::vector<std::string> fn_names_in_slot_order; // names[i] = fn at logical slot i
};

static ParsedModule* parse_fixture(const char* src) {
    auto* pm = new ParsedModule{};
    auto lr = tokenize(src, "<module_layout_test>");
    if (!lr.ok) { std::printf("FAIL: lex: %s\n", lr.error.c_str()); delete pm; return nullptr; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("FAIL: parse: %s\n", pr.error.c_str()); delete pm; return nullptr; }
    pm->program = std::move(pr.program);

    // Stable logical slot assignment: slots in source order (0,1,2,...).
    int si = 0;
    for (auto& fn : pm->program.funcs) {
        pm->slots[fn.name] = si++;
        fn.slot = pm->slots[fn.name];
    }
    pm->fn_names_in_slot_order.reserve(pm->program.funcs.size());
    for (auto& fn : pm->program.funcs)
        pm->fn_names_in_slot_order.push_back(fn.name);

    pm->struct_layouts = build_struct_layouts(pm->program);
    pm->program.string_xor_key = 0;
    std::unordered_map<std::string, NativeSig> empty_natives;
    auto sr = sema(pm->program, empty_natives, pm->slots, 0, nullptr, &pm->struct_layouts, nullptr);
    if (!sr.ok) {
        std::printf("FAIL: sema (%zu errors):\n", sr.errors.size());
        for (auto& e : sr.errors) std::printf("  line %u: %s\n", e.line, e.msg.c_str());
        delete pm; return nullptr;
    }
    return pm;
}

// Derive the ModuleManifest from the parsed+sema'd module via the canonical
// classifier. Each ModuleCallable's abi_fingerprint is classify_callable's
// output over the sema-resolved function ABI — never a hand-written hash.
static ModuleManifest build_manifest(const ParsedModule& pm, const std::string& module_id) {
    ModuleManifest m;
    m.module_id = module_id;
    m.callables.reserve(pm.program.funcs.size());
    for (const auto& fn : pm.program.funcs) {
        CallableDescriptor d = func_to_descriptor(fn, &pm.struct_layouts);
        AbiClassification c = classify_callable(d);
        ModuleCallable mc;
        mc.name = fn.name;
        mc.logical_slot = uint32_t(fn.slot);
        mc.abi_fingerprint = c.fingerprint;
        mc.visibility = fn.is_exported ? Visibility::Public : Visibility::Private;
        mc.calling_mode = CallingMode::LegacyContext;
        // Optional explicit dispatch-domain label from @dispatch_domain("...").
        mc.dispatch_domain.clear();
        for (const auto& ann : fn.annotations) {
            if (ann.name == "dispatch_domain" && !ann.args.empty())
                mc.dispatch_domain = ann.args[0];
        }
        m.callables.push_back(std::move(mc));
    }
    // Sort callables into logical-slot order so the manifest is dense and
    // ordered (the planner requires dense {0..N-1}; order is a courtesy).
    std::sort(m.callables.begin(), m.callables.end(),
              [](const ModuleCallable& a, const ModuleCallable& b){ return a.logical_slot < b.logical_slot; });
    return m;
}

// Two pinned target route words, derived from pinned BuildKeyView byte contents.
static const uint8_t KEY1_BYTES[] = "target-key-alpha-001";
static const size_t  KEY1_SIZE = 19;
static const uint8_t KEY2_BYTES[] = "target-key-beta__0022";
static const size_t  KEY2_SIZE = 19;
static BuildKeyView key1_view() { return {KEY1_BYTES, KEY1_SIZE}; }
static BuildKeyView key2_view() { return {KEY2_BYTES, KEY2_SIZE}; }

int main() {
    std::printf("== module_layout_test (Red 3) ==\n");

    // --- Parse + sema + slot assignment on the REAL fixture module. ---
    std::unique_ptr<ParsedModule> pm(parse_fixture(kFixtureSrc));
    ck(pm != nullptr, "fixture module tokenizes, parses, assigns slots, and passes sema");
    if (!pm) { std::printf("== %d checks, FAILED ==\n", g_checks); return g_fail; }

    const std::string kModId = "test.mod";
    ModuleManifest manifest = build_manifest(*pm, kModId);

    // The fixture has 6 functions in 4 domains.
    ck(manifest.callables.size() == 6, "fixture produces 6 ModuleCallable records");
    ck(manifest.callables[0].name == "add1", "callable 0 is add1");
    ck(manifest.callables[5].name == "transform", "callable 5 is transform");
    ck(manifest.callables[3].visibility == Visibility::Private, "secret is private (is_exported=false via priv fn)");
    ck(manifest.callables[5].dispatch_domain == "critical-math", "transform carries the @dispatch_domain label");

    // =======================================================================
    // 0. CANONICAL CLASSIFIER — fingerprints come from classify_callable over
    //    the sema-resolved ABI, not hand-written hashes. Pin same-ABI equality
    //    and the singleton-producing inequalities.
    // =======================================================================
    uint64_t fp_pub_legacy_i64 = 0; // used in section 13 (oversized-domain fixture)
    {
        // Re-derive each descriptor from the parsed FuncDecls and classify.
        auto classify_named = [&](const char* name) -> uint64_t {
            for (const auto& fn : pm->program.funcs) if (fn.name == name)
                return classify_callable(func_to_descriptor(fn, &pm->struct_layouts)).fingerprint;
            return 0;
        };
        uint64_t fp_add1 = classify_named("add1");
        uint64_t fp_add2 = classify_named("add2");
        uint64_t fp_add3 = classify_named("add3");
        uint64_t fp_secret = classify_named("secret");
        uint64_t fp_fma = classify_named("fma");
        uint64_t fp_transform = classify_named("transform");

        fp_pub_legacy_i64 = fp_add1;

        ck(fp_add1 == fp_add2 && fp_add2 == fp_add3, "same-ABI group (add1/add2/add3) -> identical fingerprints");
        ck(fp_add1 == fp_transform, "add1 and transform share ABI (i64->i64, public, legacy) -> identical fingerprint (label is NOT in the fingerprint)");
        ck(fp_add1 != fp_secret, "public i64->i64 vs private i64->i64 -> distinct fingerprints (visibility in fingerprint)");
        ck(fp_add1 != fp_fma, "i64->i64 vs (f32,f32)->f32 -> distinct fingerprints (ABI isolation)");
        ck(fp_secret != fp_fma, "private i64 vs public f32 -> distinct fingerprints");

        // The manifest's callables carry exactly these classifier fingerprints.
        ck(manifest.callables[0].abi_fingerprint == fp_add1, "manifest add1 carries classifier fingerprint");
        ck(manifest.callables[3].abi_fingerprint == fp_secret, "manifest secret carries classifier fingerprint");
        ck(manifest.callables[4].abi_fingerprint == fp_fma, "manifest fma carries classifier fingerprint");
        ck(manifest.callables[5].abi_fingerprint == fp_transform, "manifest transform carries classifier fingerprint");
        ck(manifest.callables[5].abi_fingerprint == manifest.callables[0].abi_fingerprint, "transform and add1 share fingerprint in the manifest");
    }

    ImplicitKeyedLayoutV1 keyed_planner;

    // =======================================================================
    // 1. DOMAIN GROUPING — correct domain count, membership, singleton sizes.
    // =======================================================================
    ExtensionResult<ModuleLayoutPlan> plan1r = keyed_planner.plan(manifest, key1_view());
    ck(bool(plan1r) && plan1r.value.has_value(), "keyed plan() succeeds for the fixture under K1");
    ModuleLayoutPlan plan1 = *plan1r.value;

    {
        ck(plan1.keyed, "keyed plan is flagged keyed");
        ck(plan1.module_id == kModId, "plan carries module id");
        ck(plan1.logical_slot_count == 6, "logical_slot_count == 6");
        ck(plan1.physical_slot_count == 10, "physical_slot_count == 10 (4+2+2+2)");
        ck(plan1.domains.size() == 4, "exactly 4 dispatch domains");
        ck(plan1.logical_routes.size() == 6, "logical_routes size == logical_slot_count");
        ck(plan1.physical_entries.size() == 10, "physical_entries size == physical_slot_count");
        ck(plan1.build_physical_placement.size() == 6, "build_physical_placement size == logical_slot_count");
        ck(plan1.padding_descriptors.size() == 4, "padding_descriptors size == domain count (one per keyed domain)");

        // Every domain's module_id matches the plan's module_id.
        bool mod_ok = true;
        for (const auto& d : plan1.domains) if (d.module_id != plan1.module_id) mod_ok = false;
        ck(mod_ok, "every domain's module_id matches the plan's module_id");

        // D0: 3 real + 1 padding = 4 physical.
        ck(plan1.domains[0].logical_count == 3, "D0 logical_count == 3 (same-ABI group)");
        ck(plan1.domains[0].padding_count == 1, "D0 padding_count == 1");
        ck(plan1.domains[0].physical_count == 4, "D0 physical_count == 4 (3+1)");
        ck(plan1.domains[0].physical_base == 0, "D0 physical_base == 0 (first domain)");
        ck(plan1.domains[0].logical_slots.size() == 3, "D0 has 3 logical slots");
        ck(plan1.domains[0].logical_slots[0] == 0 &&
           plan1.domains[0].logical_slots[1] == 1 &&
           plan1.domains[0].logical_slots[2] == 2, "D0 ordinals map to slots 0,1,2 in order");
        ck(plan1.domains[0].padding_ordinal == 3, "D0 padding_ordinal == logical_count (3)");

        // Singleton domains (1,2,3): physical_count == 2.
        for (uint32_t di = 1; di <= 3; ++di) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "D%u singleton real-function domain has logical_count == 1", di);
            ck(plan1.domains[di].logical_count == 1, msg);
            ck(plan1.domains[di].padding_count == 1, "singleton domain padding_count == 1");
            ck(plan1.domains[di].physical_count == 2, "singleton domain physical_count == 2 (1 real + 1 padding)");
        }
        // Bases are dense and contiguous: 0,4,6,8.
        ck(plan1.domains[0].physical_base == 0, "D0 base 0");
        ck(plan1.domains[1].physical_base == 4, "D1 base 4");
        ck(plan1.domains[2].physical_base == 6, "D2 base 6");
        ck(plan1.domains[3].physical_base == 8, "D3 base 8");

        // Domain identity isolation.
        ck(plan1.domains[0].visibility == Visibility::Public &&
           plan1.domains[0].calling_mode == CallingMode::LegacyContext &&
           plan1.domains[0].dispatch_domain == "", "D0 identity: public/legacy/empty-label");
        ck(plan1.domains[1].visibility == Visibility::Private, "D1 identity: private (visibility isolation)");
        ck(plan1.domains[2].abi_fingerprint != plan1.domains[0].abi_fingerprint, "D2 fingerprint differs from D0 (ABI isolation)");
        ck(plan1.domains[3].abi_fingerprint == plan1.domains[0].abi_fingerprint, "D3 fingerprint EQUALS D0 (same ABI, diff label)");
        ck(plan1.domains[3].dispatch_domain == "critical-math", "D3 carries explicit dispatch-domain label");
    }

    // =======================================================================
    // 2. DENSE LOGICAL IDENTITY PRESERVATION — routes cover [0,N) exactly once,
    //    and each route's metadata matches its domain (validated).
    // =======================================================================
    {
        std::vector<bool> seen(plan1.logical_slot_count, false);
        bool dense = true, no_oob = true;
        for (uint32_t i = 0; i < plan1.logical_routes.size(); ++i) {
            const auto& r = plan1.logical_routes[i];
            if (r.logical_slot != i) dense = false;
            if (r.logical_slot >= plan1.logical_slot_count) no_oob = false;
            else if (seen[r.logical_slot]) dense = false;
            else seen[r.logical_slot] = true;
        }
        ck(dense, "logical routes are dense: route[i].logical_slot == i, no dup");
        ck(no_oob, "no logical route slot out of bounds");
        bool all_seen = true;
        for (bool b : seen) if (!b) all_seen = false;
        ck(all_seen, "every logical slot [0,N) is covered exactly once");

        // Each route's metadata matches its domain.
        bool meta_ok = true;
        for (const auto& r : plan1.logical_routes) {
            const auto& d = plan1.domains[r.domain_index];
            if (r.abi_fingerprint != d.abi_fingerprint ||
                r.visibility != d.visibility ||
                r.calling_mode != d.calling_mode ||
                r.dispatch_domain != d.dispatch_domain) meta_ok = false;
        }
        ck(meta_ok, "every logical route's metadata matches its domain identity");
    }

    // =======================================================================
    // 3. PADDING — exactly one ABI-compatible padding entry per keyed domain,
    //    with a complete PaddingDescriptor per domain.
    // =======================================================================
    {
        bool one_padding_each = true, padding_meta_ok = true, padding_placed = true;
        bool pad_desc_ok = true;
        uint32_t padding_total = 0;
        for (uint32_t di = 0; di < plan1.domains.size(); ++di) {
            const auto& d = plan1.domains[di];
            if (d.padding_count != 1) one_padding_each = false;
            const PhysicalEntry* pad = nullptr;
            for (const auto& e : plan1.physical_entries) {
                if (e.domain_index == di && e.is_padding) {
                    if (pad) one_padding_each = false; // duplicate padding
                    pad = &e;
                }
            }
            if (!pad) { one_padding_each = false; continue; }
            ++padding_total;
            if (pad->abi_fingerprint != d.abi_fingerprint ||
                pad->visibility != d.visibility ||
                pad->calling_mode != d.calling_mode ||
                pad->dispatch_domain != d.dispatch_domain) padding_meta_ok = false;
            if (pad->physical_slot < d.physical_base ||
                pad->physical_slot >= d.physical_base + d.physical_count) padding_placed = false;
            if (pad->ordinal != d.logical_count) padding_placed = false;
            // The complete PaddingDescriptor for this domain.
            if (di >= plan1.padding_descriptors.size()) { pad_desc_ok = false; continue; }
            const auto& pd = plan1.padding_descriptors[di];
            if (pd.domain_index != di || pd.ordinal != d.padding_ordinal ||
                pd.physical_slot != pad->physical_slot ||
                pd.abi_fingerprint != d.abi_fingerprint ||
                pd.visibility != d.visibility ||
                pd.calling_mode != d.calling_mode ||
                pd.dispatch_domain != d.dispatch_domain) pad_desc_ok = false;
        }
        ck(one_padding_each, "every keyed domain has exactly one padding entry");
        ck(padding_meta_ok, "padding entry metadata is ABI-compatible with its domain");
        ck(padding_placed, "padding entry is placed inside its domain at the padding ordinal");
        ck(padding_total == plan1.domains.size(), "total padding entries == domain count (one per domain)");
        ck(pad_desc_ok, "every keyed domain has a complete, matching PaddingDescriptor");
    }

    // =======================================================================
    // 4. EXACT PHYSICAL COVERAGE — every physical slot covered exactly once.
    // =======================================================================
    {
        std::vector<bool> covered(plan1.physical_slot_count, false);
        bool exact = true;
        for (const auto& e : plan1.physical_entries) {
            if (e.physical_slot >= plan1.physical_slot_count) { exact = false; break; }
            if (covered[e.physical_slot]) { exact = false; break; }
            covered[e.physical_slot] = true;
        }
        ck(exact, "physical entries are exactly-once, no OOB, no duplicate slot");
        bool all_cov = true;
        for (bool b : covered) if (!b) all_cov = false;
        ck(all_cov, "every physical slot [0, physical_slot_count) covered exactly once");

        bool idx_ok = true;
        for (uint32_t i = 0; i < plan1.physical_entries.size(); ++i)
            if (plan1.physical_entries[i].physical_slot != i) idx_ok = false;
        ck(idx_ok, "physical_entries[i].physical_slot == i (indexed by slot)");

        bool in_domain = true;
        for (const auto& e : plan1.physical_entries) {
            const auto& d = plan1.domains[e.domain_index];
            if (e.physical_slot < d.physical_base ||
                e.physical_slot >= d.physical_base + d.physical_count) in_domain = false;
            if (e.abi_fingerprint != d.abi_fingerprint ||
                e.visibility != d.visibility ||
                e.calling_mode != d.calling_mode ||
                e.dispatch_domain != d.dispatch_domain) in_domain = false;
        }
        ck(in_domain, "every physical entry is inside its domain and matches domain metadata");
    }

    // =======================================================================
    // 5. GOLDEN PLACEMENTS under K1 and K2 (cross-checked against the oracle).
    // =======================================================================
    uint64_t rw1 = oracle_route_word(KEY1_BYTES, KEY1_SIZE);
    uint64_t rw2 = oracle_route_word(KEY2_BYTES, KEY2_SIZE);
    ck(derive_route_word(key1_view()) == rw1, "production derive_route_word matches oracle (K1)");
    ck(derive_route_word(key2_view()) == rw2, "production derive_route_word matches oracle (K2)");
    ck(rw1 != rw2, "K1 and K2 produce distinct route words");

    {
        bool salt_ok = true;
        for (const auto& d : plan1.domains) {
            uint64_t os = oracle_domain_salt(d.module_id, d.visibility, d.calling_mode,
                                            d.abi_fingerprint, d.dispatch_domain, d.strategy_version);
            if (d.domain_salt != os) salt_ok = false;
            if (d.domain_salt == 0) salt_ok = false;
        }
        ck(salt_ok, "production domain salts match oracle derivation (non-zero, deterministic)");
    }

    auto compute_expected = [&](uint64_t rw, const ModuleLayoutPlan& p) {
        std::vector<std::vector<uint32_t>> exp(p.domains.size());
        for (uint32_t di = 0; di < p.domains.size(); ++di) {
            const auto& d = p.domains[di];
            exp[di].resize(d.physical_count);
            for (uint32_t o = 0; o < d.physical_count; ++o)
                exp[di][o] = d.physical_base + oracle_place(rw, d.domain_salt, d.strategy_version, d.physical_count, o);
        }
        return exp;
    };
    auto exp1 = compute_expected(rw1, plan1);

    std::printf("-- oracle placements K1 --\n");
    for (uint32_t di = 0; di < plan1.domains.size(); ++di) {
        const auto& d = plan1.domains[di];
        for (uint32_t o = 0; o < d.physical_count; ++o)
            std::printf("D%u ord%u -> phys %u\n", di, o, exp1[di][o]);
    }

    {
        bool real_match = true;
        for (uint32_t s = 0; s < plan1.logical_slot_count; ++s) {
            const auto& r = plan1.logical_routes[s];
            uint32_t expected = exp1[r.domain_index][r.ordinal];
            if (plan1.build_physical_placement[s] != expected) real_match = false;
            const auto& e = plan1.physical_entries[expected];
            if (e.is_padding || e.logical_slot != s || e.ordinal != r.ordinal) real_match = false;
        }
        ck(real_match, "golden K1: real-callable placements match oracle (base + Red1 permute)");
    }
    {
        bool pad_match = true;
        for (uint32_t di = 0; di < plan1.domains.size(); ++di) {
            const auto& d = plan1.domains[di];
            uint32_t expected = exp1[di][d.padding_ordinal];
            const PhysicalEntry* pad = nullptr;
            for (const auto& e : plan1.physical_entries)
                if (e.domain_index == di && e.is_padding) pad = &e;
            if (!pad || pad->physical_slot != expected) pad_match = false;
        }
        ck(pad_match, "golden K1: padding placements match oracle");
    }

    // =======================================================================
    // 6. DETERMINISTIC REPEAT BUILDS — same key reproduces the layout.
    // =======================================================================
    {
        auto p_again = keyed_planner.plan(manifest, key1_view());
        ck(bool(p_again), "repeat plan() succeeds");
        bool same = (p_again.value->logical_slot_count == plan1.logical_slot_count &&
                     p_again.value->physical_slot_count == plan1.physical_slot_count &&
                     p_again.value->build_physical_placement == plan1.build_physical_placement &&
                     p_again.value->domains.size() == plan1.domains.size() &&
                     p_again.value->padding_descriptors == plan1.padding_descriptors);
        for (uint32_t di = 0; same && di < plan1.domains.size(); ++di) {
            if (p_again.value->domains[di].domain_salt != plan1.domains[di].domain_salt) same = false;
            if (p_again.value->domains[di].physical_base != plan1.domains[di].physical_base) same = false;
        }
        ck(same, "deterministic: same key -> identical plan (salts, bases, placements, padding descriptors)");
    }

    // =======================================================================
    // 7. DIFFERENT KEYED LAYOUTS — K2 produces a different layout from K1.
    // =======================================================================
    ExtensionResult<ModuleLayoutPlan> plan2r = keyed_planner.plan(manifest, key2_view());
    ck(bool(plan2r), "keyed plan() succeeds under K2");
    ModuleLayoutPlan plan2 = *plan2r.value;
    {
        bool struct_same = (plan2.domains.size() == plan1.domains.size() &&
                            plan2.physical_slot_count == plan1.physical_slot_count);
        for (uint32_t di = 0; struct_same && di < plan1.domains.size(); ++di) {
            if (plan2.domains[di].physical_base != plan1.domains[di].physical_base) struct_same = false;
            if (plan2.domains[di].domain_salt != plan1.domains[di].domain_salt) struct_same = false;
        }
        ck(struct_same, "domain structure (bases/salts) is key-independent");

        bool any_diff = false;
        for (uint32_t s = 0; s < plan1.logical_slot_count; ++s)
            if (plan2.build_physical_placement[s] != plan1.build_physical_placement[s]) any_diff = true;
        ck(any_diff, "different pinned keys produce different physical layouts");
    }
    auto exp2 = compute_expected(rw2, plan2);
    std::printf("-- oracle placements K2 --\n");
    for (uint32_t di = 0; di < plan2.domains.size(); ++di) {
        const auto& d = plan2.domains[di];
        for (uint32_t o = 0; o < d.physical_count; ++o)
            std::printf("D%u ord%u -> phys %u\n", di, o, exp2[di][o]);
    }
    {
        bool real_match = true;
        for (uint32_t s = 0; s < plan2.logical_slot_count; ++s) {
            const auto& r = plan2.logical_routes[s];
            if (plan2.build_physical_placement[s] != exp2[r.domain_index][r.ordinal]) real_match = false;
        }
        ck(real_match, "golden K2: real-callable placements match oracle");
    }

    // =======================================================================
    // 8. GOLDEN HARDCODED LITERAL PLACEMENTS — cross-checked against the oracle
    //    AND production.
    // =======================================================================
    static const std::vector<std::vector<uint32_t>> golden1 = {
        {1, 0, 3, 2},   // D0 (3 real + 1 padding): K1
        {5, 4},         // D1 singleton: K1
        {7, 6},         // D2 singleton: K1
        {9, 8},         // D3 singleton: K1
    };
    static const std::vector<std::vector<uint32_t>> golden2 = {
        {0, 1, 2, 3},   // D0 (K2): identity permutation for this key/salt/size
        {5, 4},         // D1 singleton (K2)
        {7, 6},         // D2 singleton (K2)
        {8, 9},         // D3 singleton (K2)
    };
    {
        bool lit_oracle_k1 = (golden1.size() == exp1.size());
        for (uint32_t di = 0; lit_oracle_k1 && di < exp1.size(); ++di) {
            if (golden1[di].size() != exp1[di].size()) lit_oracle_k1 = false;
            for (uint32_t o = 0; lit_oracle_k1 && o < exp1[di].size(); ++o)
                if (golden1[di][o] != exp1[di][o]) lit_oracle_k1 = false;
        }
        ck(lit_oracle_k1, "golden K1 literals match oracle (no shared typo)");

        bool lit_prod_k1 = true;
        for (uint32_t s = 0; lit_prod_k1 && s < plan1.logical_slot_count; ++s) {
            const auto& r = plan1.logical_routes[s];
            if (plan1.build_physical_placement[s] != golden1[r.domain_index][r.ordinal]) lit_prod_k1 = false;
        }
        ck(lit_prod_k1, "golden K1 literals match production placements");

        bool lit_oracle_k2 = (golden2.size() == exp2.size());
        for (uint32_t di = 0; lit_oracle_k2 && di < exp2.size(); ++di) {
            if (golden2[di].size() != exp2[di].size()) lit_oracle_k2 = false;
            for (uint32_t o = 0; lit_oracle_k2 && o < exp2[di].size(); ++o)
                if (golden2[di][o] != exp2[di][o]) lit_oracle_k2 = false;
        }
        ck(lit_oracle_k2, "golden K2 literals match oracle (no shared typo)");
        bool lit_prod_k2 = true;
        for (uint32_t s = 0; lit_prod_k2 && s < plan2.logical_slot_count; ++s) {
            const auto& r = plan2.logical_routes[s];
            if (plan2.build_physical_placement[s] != golden2[r.domain_index][r.ordinal]) lit_prod_k2 = false;
        }
        ck(lit_prod_k2, "golden K2 literals match production placements");
    }

    // =======================================================================
    // 9. WRONG-KEY SAFETY — per-domain in-range + same-ABI/visibility/label.
    // =======================================================================
    {
        std::vector<uint64_t> wrong;
        for (uint32_t i = 0; i < 256; ++i) {
            uint64_t w = rw1 ^ (0x9E3779B97F4A7C15ULL * (uint64_t(i) + 7));
            w += uint64_t(i) * 0x100000001b3ULL;
            if (w == rw1 || w == rw2) w ^= 0x0123456789ABCDEFULL;
            wrong.push_back(w);
        }
        uint32_t per_domain_checks = 0, per_domain_fail = 0;
        for (uint64_t w : wrong) {
            for (uint32_t di = 0; di < plan1.domains.size(); ++di) {
                const auto& d = plan1.domains[di];
                for (uint32_t o = 0; o < d.physical_count; ++o) {
                    uint32_t phys = oracle_place(w, d.domain_salt, d.strategy_version, d.physical_count, o);
                    if (phys >= d.physical_count) { ++per_domain_fail; continue; }
                    uint32_t slot = d.physical_base + phys;
                    if (slot < d.physical_base || slot >= d.physical_base + d.physical_count) { ++per_domain_fail; continue; }
                    const auto& e = plan1.physical_entries[slot];
                    if (e.abi_fingerprint != d.abi_fingerprint ||
                        e.visibility != d.visibility ||
                        e.calling_mode != d.calling_mode ||
                        e.dispatch_domain != d.dispatch_domain) { ++per_domain_fail; continue; }
                    ++per_domain_checks;
                }
            }
        }
        ck(per_domain_fail == 0, "wrong keys: every recomputed destination in-domain + same ABI/visibility/mode/label metadata");
        ck(per_domain_checks == 256u * 10u /*sum of physical_counts*/,
           "wrong keys: per-domain membership checked for every domain/ordinal/key (not global range only)");
    }

    // =======================================================================
    // 10. VALIDATE GOOD PLANS — validate() accepts the freshly-built plans.
    // =======================================================================
    {
        ExtensionStatus v1 = keyed_planner.validate(plan1);
        ExtensionStatus v2 = keyed_planner.validate(plan2);
        ck(bool(v1), "validate() accepts the good K1 plan");
        ck(bool(v2), "validate() accepts the good K2 plan");
    }

    // =======================================================================
    // 11. NEGATIVE — duplicate logical slots in manifest -> plan() rejects.
    // =======================================================================
    {
        ModuleManifest m = build_manifest(*pm, kModId);
        m.callables[2].logical_slot = 0; // duplicate slot 0
        auto r = keyed_planner.plan(m, key1_view());
        ck(!r && r.error.has_value(), "duplicate logical slot in manifest -> plan() rejects (structured error)");
        ck(r.error->registry.size() && r.error->message.size(), "duplicate-slot rejection carries structured diagnostic");
    }

    // =======================================================================
    // 12. NEGATIVE — non-dense logical slots in manifest -> plan() rejects.
    // =======================================================================
    {
        ModuleManifest m = build_manifest(*pm, kModId);
        m.callables[5].logical_slot = 99; // gap: slots 0..4 then 99 (missing 5, OOB range)
        auto r = keyed_planner.plan(m, key1_view());
        ck(!r && r.error.has_value(), "non-dense logical slots in manifest -> plan() rejects");
    }
    {
        ModuleManifest m = build_manifest(*pm, kModId);
        m.callables[0].logical_slot = 1; // doesn't start at 0
        auto r = keyed_planner.plan(m, key1_view());
        ck(!r && r.error.has_value(), "logical slots not starting at 0 -> plan() rejects");
    }

    // =======================================================================
    // 13. NEGATIVE — oversized domain (256 same-ABI callables -> phys 257 > MAX).
    // =======================================================================
    {
        ModuleManifest m;
        m.module_id = "huge.mod";
        uint64_t fp = fp_pub_legacy_i64;
        for (uint32_t i = 0; i < 256; ++i) {
            ModuleCallable c;
            c.name = "f" + std::to_string(i);
            c.logical_slot = i;
            c.abi_fingerprint = fp;
            c.visibility = Visibility::Public;
            c.calling_mode = CallingMode::LegacyContext;
            c.dispatch_domain = "";
            m.callables.push_back(std::move(c));
        }
        auto r = keyed_planner.plan(m, key1_view());
        ck(!r && r.error.has_value(), "oversized domain (256 same-ABI -> phys 257 > MAX) -> plan() rejects");

        m.callables.pop_back(); // 255 -> phys 256 == MAX
        auto r2 = keyed_planner.plan(m, key1_view());
        ck(bool(r2), "domain at the boundary (255 real -> phys 256 == MAX) accepted");
        ck(r2.value->domains[0].physical_count == KEYED_DISPATCH_MAX_DOMAIN_SIZE,
           "boundary domain physical_count == MAX (256)");
    }

    // =======================================================================
    // 14. NEGATIVE — empty module: the keyed planner REJECTS an empty module
    //     (strict negative — an empty keyed layout has no dispatch domains and
    //     is not a valid keyed plan).
    // =======================================================================
    {
        ModuleManifest empty;
        empty.module_id = "empty.mod";
        auto r = keyed_planner.plan(empty, key1_view());
        ck(!r && r.error.has_value(), "empty module -> keyed plan() REJECTS (strict negative)");
        ck(r.error->registry.size() && r.error->message.size(), "empty-module rejection carries structured diagnostic");
        // The identity planner, by contrast, accepts an empty module (an empty
        // identity dispatch table is valid — preserves existing DispatchTable
        // behavior). See section 16.
    }
    // A padding-only domain (logical_count 0 but a padding entry) is a malformed
    // plan; validate() rejects it.
    {
        ModuleLayoutPlan p;
        p.module_id = "bad.mod";
        p.keyed = true;
        p.logical_slot_count = 0;
        p.physical_slot_count = 1;
        DispatchDomain d;
        d.module_id = "bad.mod";
        d.visibility = Visibility::Public;
        d.calling_mode = CallingMode::LegacyContext;
        d.abi_fingerprint = 0x1234;
        d.dispatch_domain = "";
        d.domain_salt = 0x5;
        d.strategy_version = 1;
        d.physical_base = 0;
        d.physical_count = 1;
        d.logical_count = 0;
        d.padding_count = 1;
        d.padding_ordinal = 0;
        p.domains.push_back(d);
        PhysicalEntry e;
        e.physical_slot = 0;
        e.abi_fingerprint = 0x1234;
        e.visibility = Visibility::Public;
        e.calling_mode = CallingMode::LegacyContext;
        e.dispatch_domain = "";
        e.is_padding = true;
        e.logical_slot = 0xFFFFFFFFu;
        e.domain_index = 0;
        e.ordinal = 0;
        p.physical_entries.push_back(e);
        ExtensionStatus v = keyed_planner.validate(p);
        ck(!v && v.error.has_value(), "padding-only domain (logical_count 0) -> validate rejects");
    }

    // =======================================================================
    // 15. NEGATIVE PLAN MUTATIONS — feed malformed plans to validate().
    // =======================================================================
    auto base_good = [&]() { return *keyed_planner.plan(build_manifest(*pm, kModId), key1_view()).value; };

    // 15a. missing route: drop one logical route.
    { ModuleLayoutPlan bad = base_good(); bad.logical_routes.pop_back();
      ck(!keyed_planner.validate(bad), "missing route (route count < logical_slot_count) -> validate rejects"); }
    // 15b. duplicate route slot.
    { ModuleLayoutPlan bad = base_good(); bad.logical_routes[3].logical_slot = 0;
      ck(!keyed_planner.validate(bad), "duplicate logical route slot -> validate rejects"); }
    // 15c. overlapping physical placements: shift D1's base to overlap D0.
    {
        ModuleLayoutPlan bad = base_good();
        bad.domains[1].physical_base = 3; // overlaps D0's [0,4)
        for (auto& e : bad.physical_entries) if (e.domain_index == 1) e.physical_slot = bad.domains[1].physical_base + (e.physical_slot - 4);
        ck(!keyed_planner.validate(bad), "overlapping physical domain ranges -> validate rejects");
    }
    // 15d. OOB placement.
    { ModuleLayoutPlan bad = base_good(); bad.build_physical_placement[0] = bad.physical_slot_count + 100;
      ck(!keyed_planner.validate(bad), "OOB build placement (>= physical_slot_count) -> validate rejects"); }
    // 15e. missing padding.
    { ModuleLayoutPlan bad = base_good(); bad.domains[0].padding_count = 0; bad.domains[0].physical_count = bad.domains[0].logical_count;
      ck(!keyed_planner.validate(bad), "missing padding (padding_count 0) -> validate rejects"); }
    // 15f. duplicate padding.
    { ModuleLayoutPlan bad = base_good(); bad.domains[0].padding_count = 2; bad.domains[0].physical_count = bad.domains[0].logical_count + 2;
      ck(!keyed_planner.validate(bad), "duplicate padding (padding_count 2) -> validate rejects"); }
    // 15g. inconsistent counts.
    { ModuleLayoutPlan bad = base_good(); bad.domains[0].physical_count = 99;
      ck(!keyed_planner.validate(bad), "inconsistent counts (physical != logical + padding) -> validate rejects"); }
    // 15h. cross-ABI membership: an entry in D0 with a different fingerprint.
    { ModuleLayoutPlan bad = base_good(); bad.physical_entries[0].abi_fingerprint = 0xDEAD;
      ck(!keyed_planner.validate(bad), "cross-ABI membership (entry fp != domain fp) -> validate rejects"); }
    // 15i. cross-visibility membership.
    { ModuleLayoutPlan bad = base_good(); for (auto& e : bad.physical_entries) if (e.domain_index == 1) { e.visibility = Visibility::Public; break; }
      ck(!keyed_planner.validate(bad), "cross-visibility membership -> validate rejects"); }
    // 15j. oversized domain in a plan.
    { ModuleLayoutPlan bad = base_good(); bad.domains[0].physical_count = KEYED_DISPATCH_MAX_DOMAIN_SIZE + 1;
      ck(!keyed_planner.validate(bad), "oversized domain (physical_count > MAX) -> validate rejects"); }
    // 15k. physical coverage gap.
    { ModuleLayoutPlan bad = base_good(); bad.physical_slot_count = 99;
      ck(!keyed_planner.validate(bad), "physical coverage gap (slot_count > covered) -> validate rejects"); }
    // 15l. domain logical_count mismatch.
    { ModuleLayoutPlan bad = base_good(); bad.domains[0].logical_slots.push_back(42);
      ck(!keyed_planner.validate(bad), "domain logical_slots size != logical_count -> validate rejects"); }
    // 15m. route metadata mismatch: mutate a route's ABI fingerprint to differ
    //      from its domain.
    { ModuleLayoutPlan bad = base_good(); bad.logical_routes[0].abi_fingerprint = 0xBEEF;
      ck(!keyed_planner.validate(bad), "route ABI fingerprint != domain ABI fingerprint -> validate rejects"); }
    // 15n. route metadata mismatch: mutate a route's visibility.
    { ModuleLayoutPlan bad = base_good(); bad.logical_routes[0].visibility = Visibility::Private;
      ck(!keyed_planner.validate(bad), "route visibility != domain visibility -> validate rejects"); }
    // 15o. route metadata mismatch: mutate a route's calling mode.
    { ModuleLayoutPlan bad = base_good(); bad.logical_routes[0].calling_mode = CallingMode::KeyedR15;
      ck(!keyed_planner.validate(bad), "route calling_mode != domain calling_mode -> validate rejects"); }
    // 15p. route metadata mismatch: mutate a route's dispatch-domain label.
    { ModuleLayoutPlan bad = base_good(); bad.logical_routes[0].dispatch_domain = "tampered";
      ck(!keyed_planner.validate(bad), "route dispatch_domain != domain dispatch_domain -> validate rejects"); }
    // 15q. domain module_id mismatch: a domain whose module_id != plan module_id.
    { ModuleLayoutPlan bad = base_good(); bad.domains[0].module_id = "other.mod";
      ck(!keyed_planner.validate(bad), "domain module_id != plan module_id -> validate rejects"); }
    // 15r. padding descriptor mismatch: corrupt a PaddingDescriptor's fingerprint.
    { ModuleLayoutPlan bad = base_good(); bad.padding_descriptors[0].abi_fingerprint = 0xDEAD;
      ck(!keyed_planner.validate(bad), "padding descriptor ABI fingerprint != domain -> validate rejects"); }
    // 15s. padding descriptor physical_slot mismatch.
    { ModuleLayoutPlan bad = base_good(); bad.padding_descriptors[0].physical_slot += 1;
      ck(!keyed_planner.validate(bad), "padding descriptor physical_slot != placed entry -> validate rejects"); }

    // =======================================================================
    // 15t–15y. STRENGTHENED VALIDATION — keyed flag, route/entry ordinal vs
    //   DispatchDomain::logical_slots consistency, padding ordinal/sentinel,
    //   and BuildKeyView{nullptr, nonzero_size} structured rejection. These
    //   pin the Red 3 audit defects: a malformed plan that previously slipped
    //   past validate() is now rejected, and a null-byte/nonzero-size key view
    //   is rejected through a structured ExtensionError BEFORE derive_route_word
    //   dereferences it.
    // =======================================================================

    // 15t. keyed == false handed to the KEYED validator -> reject (an identity
    //      plan must not be accepted by the keyed concept's validate).
    {
        IdentityLayout id_planner;
        auto idr = id_planner.plan(manifest, key1_view());
        ck(bool(idr), "identity plan builds for the keyed-flag negative");
        ModuleLayoutPlan idp = *idr.value;
        ExtensionStatus v = keyed_planner.validate(idp);
        ck(!v && v.error.has_value(), "keyed validate rejects an identity plan (keyed == false)");
        ck(v.error->registry.size() && v.error->message.size(), "keyed-flag rejection carries structured diagnostic");
    }
    // 15u. route ordinal / DispatchDomain::logical_slots mismatch: swap two
    //      logical_slots entries within a domain so the route's ordinal no
    //      longer maps to its logical slot.
    {
        ModuleLayoutPlan bad = base_good();
        // D0 has logical_slots {0,1,2}; routes 0,1,2 carry ordinal 0,1,2.
        // Swap the domain's logical_slots so slot 1 and 2 trade ordinals:
        // route[1].ordinal==1 but d.logical_slots[1]==2 now.
        std::swap(bad.domains[0].logical_slots[1], bad.domains[0].logical_slots[2]);
        ck(!keyed_planner.validate(bad), "route ordinal/logical_slots mismatch -> validate rejects");
    }
    // 15v. route ordinal out of range of DispatchDomain::logical_slots (shrink
    //      logical_slots without changing logical_count is caught earlier, so
    //      instead corrupt a route's ordinal to point past the domain's reals).
    {
        ModuleLayoutPlan bad = base_good();
        // route[0] is in D0 (logical_count 3, padding_ordinal 3). Point its
        // ordinal at the padding ordinal: an invalid real-ordinal mapping.
        bad.logical_routes[0].ordinal = bad.domains[0].padding_ordinal;
        ck(!keyed_planner.validate(bad), "route ordinal >= domain logical_count -> validate rejects (ordinal/logical_slots guard)");
    }
    // 15w. real physical entry ordinal / logical_slots mismatch: corrupt a
    //      real entry's logical_slot so it disagrees with its domain's
    //      logical_slots[ordinal].
    {
        ModuleLayoutPlan bad = base_good();
        // Find D0's first real physical entry and mis-state its logical_slot.
        uint32_t hit = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < bad.physical_entries.size(); ++i)
            if (bad.physical_entries[i].domain_index == 0 && !bad.physical_entries[i].is_padding) { hit = i; break; }
        ck(hit != 0xFFFFFFFFu, "located a D0 real entry for the entry-ordinal negative");
        bad.physical_entries[hit].logical_slot = (bad.physical_entries[hit].logical_slot == 0) ? 1u : 0u;
        ck(!keyed_planner.validate(bad), "real entry ordinal/logical_slots mismatch -> validate rejects");
    }
    // 15x. padding entry ordinal != domain padding_ordinal: mark a real entry
    //      as padding but give it a non-padding ordinal, OR mark a padding
    //      entry with the wrong ordinal. Flip a padding entry's ordinal.
    {
        ModuleLayoutPlan bad = base_good();
        uint32_t hit = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < bad.physical_entries.size(); ++i)
            if (bad.physical_entries[i].domain_index == 0 && bad.physical_entries[i].is_padding) { hit = i; break; }
        ck(hit != 0xFFFFFFFFu, "located a D0 padding entry for the padding-ordinal negative");
        bad.physical_entries[hit].ordinal = 0; // padding must sit at padding_ordinal (== logical_count), not 0
        ck(!keyed_planner.validate(bad), "padding entry ordinal != domain padding_ordinal -> validate rejects");
    }
    // 15y. padding entry logical_slot != padding sentinel: a padding entry that
    //      claims to serve a real logical slot is malformed.
    {
        ModuleLayoutPlan bad = base_good();
        uint32_t hit = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < bad.physical_entries.size(); ++i)
            if (bad.physical_entries[i].domain_index == 0 && bad.physical_entries[i].is_padding) { hit = i; break; }
        ck(hit != 0xFFFFFFFFu, "located a D0 padding entry for the padding-sentinel negative");
        bad.physical_entries[hit].logical_slot = 0; // must be 0xFFFFFFFFu
        ck(!keyed_planner.validate(bad), "padding entry logical_slot != padding sentinel -> validate rejects");
    }
    // 15z. BuildKeyView{nullptr, nonzero_size} -> keyed plan() rejects through a
    //      structured ExtensionError BEFORE derive_route_word dereferences it.
    {
        BuildKeyView bad_key{nullptr, 7};
        auto r = keyed_planner.plan(manifest, bad_key);
        ck(!r && r.error.has_value(), "BuildKeyView{nullptr, nonzero_size} -> keyed plan() REJECTS (structured error)");
        ck(r.error->registry.size() && r.error->message.size(), "null-byte/nonzero-size key rejection carries structured diagnostic");
        // Defensive: derive_route_word on a null base never dereferences null
        // (treats it as empty -> FNV-1a offset basis, a non-zero route word).
        uint64_t rw_null_empty = derive_route_word({nullptr, 0});
        uint64_t rw_null_nonzero = derive_route_word({nullptr, 7});
        ck(rw_null_empty == rw_null_nonzero, "derive_route_word defensive: null base never dereferences (empty-fold)");
        ck(rw_null_empty != 0, "empty key folds to a non-zero route word (FNV-1a offset basis)");
    }
    // 15z2. BuildKeyView{nullptr, 0} (degenerate empty key) is ACCEPTED by the
    //      keyed planner (folds to a valid non-zero route word -> a fully-
    //      determined layout). Only null + NON-zero size is rejected.
    {
        BuildKeyView empty_key{nullptr, 0};
        auto r = keyed_planner.plan(manifest, empty_key);
        ck(bool(r) && r.value->keyed && r.value->physical_slot_count == 10,
           "BuildKeyView{nullptr, 0} (degenerate empty key) is accepted (non-zero route word)");
        ck(bool(keyed_planner.validate(*r.value)), "empty-key keyed plan validates");
    }

    // =======================================================================
    // 16. DISABLED / UNKEYED IDENTITY LAYOUT.
    // =======================================================================
    {
        IdentityLayout id_planner;
        auto r = id_planner.plan(manifest, key1_view()); // key ignored
        ck(bool(r), "identity layout plan() succeeds");
        ModuleLayoutPlan p = *r.value;
        ck(!p.keyed, "identity plan is NOT flagged keyed");
        ck(p.logical_slot_count == 6, "identity plan logical_slot_count == 6");
        ck(p.physical_slot_count == 6, "identity plan physical_slot_count == logical_slot_count (no padding)");
        ck(p.build_physical_placement.size() == 6, "identity plan placement vector size == 6");
        bool identity = true;
        for (uint32_t i = 0; i < p.logical_slot_count; ++i)
            if (p.build_physical_placement[i] != i) identity = false;
        ck(identity, "identity layout: logical slot i -> physical slot i");
        ck(p.domains.empty(), "identity plan has no keyed domains");
        bool no_pad = true;
        for (const auto& e : p.physical_entries) if (e.is_padding) no_pad = false;
        ck(no_pad, "identity plan adds no keyed padding entries");
        ck(p.physical_entries.size() == 6, "identity plan physical_entries == logical count (no padding)");
        ck(p.padding_descriptors.empty(), "identity plan has no padding descriptors");
        ck(p.physical_slot_count == p.logical_slot_count, "identity: physical and logical counts match");
        // Each physical entry serves its logical slot with the correct callable metadata.
        bool meta_ok = true;
        for (uint32_t i = 0; i < p.physical_slot_count; ++i) {
            const auto& e = p.physical_entries[i];
            const auto& mc = manifest.callables[i];
            if (e.logical_slot != i || e.is_padding) meta_ok = false;
            if (e.name != mc.name) meta_ok = false;
        }
        ck(meta_ok, "identity plan: each physical entry serves its slot with the correct callable metadata");
        // Free-function form agrees.
        auto r2 = plan_identity_layout(manifest);
        ck(bool(r2) && r2.value->physical_slot_count == p.physical_slot_count &&
           r2.value->build_physical_placement == p.build_physical_placement,
           "plan_identity_layout free function agrees with IdentityLayout planner");
        ExtensionStatus v = id_planner.validate(p);
        ck(bool(v), "validate() accepts the identity plan");
        ck(p.physical_slot_count == p.logical_slot_count &&
           std::all_of(p.build_physical_placement.begin(), p.build_physical_placement.end(),
                       [&](uint32_t s){ return s < p.physical_slot_count; }),
           "identity layout preserves existing DispatchTable invariants (untouched behavior)");
    }

    // =======================================================================
    // 16b. IDENTITY LAYOUT indexes callables by logical_slot — a dense but
    //      SHUFFLED manifest preserves the correct callable metadata (the
    //      planner does NOT assume callables[i].logical_slot == i).
    // =======================================================================
    {
        ModuleManifest shuffled = build_manifest(*pm, kModId);
        // Shuffle the manifest vector order while keeping logical slots dense.
        // Rotate: move callables[0] (add1, slot 0) to the end.
        std::rotate(shuffled.callables.begin(), shuffled.callables.begin() + 1,
                    shuffled.callables.end());
        // callables vector is now [add2(slot1), add3(slot2), secret(slot3),
        // fma(slot4), transform(slot5), add1(slot0)] — dense but NOT in slot order.
        ck(shuffled.callables[0].logical_slot == 1, "shuffled manifest: vector[0] has logical_slot 1 (not 0)");
        ck(shuffled.callables[5].logical_slot == 0, "shuffled manifest: vector[5] has logical_slot 0 (not 5)");

        IdentityLayout id_planner;
        auto r = id_planner.plan(shuffled, key1_view());
        ck(bool(r), "identity plan() succeeds for a dense-but-shuffled manifest");
        ModuleLayoutPlan p = *r.value;
        // slot i -> physical i, counts match, no padding.
        ck(p.physical_slot_count == p.logical_slot_count, "shuffled identity: physical == logical count");
        bool identity = true;
        for (uint32_t i = 0; i < p.logical_slot_count; ++i)
            if (p.build_physical_placement[i] != i) identity = false;
        ck(identity, "shuffled identity: slot i -> physical i");
        // The CORRECT callable metadata is preserved per logical slot, even though
        // the manifest vector was shuffled. entry[0] serves add1 (slot 0), not
        // add2 (which is now vector[0]).
        bool meta_ok = true;
        for (uint32_t i = 0; i < p.physical_slot_count; ++i) {
            // Find the manifest callable whose logical_slot is i.
            const ModuleCallable* mc = nullptr;
            for (const auto& c : shuffled.callables) if (c.logical_slot == i) { mc = &c; break; }
            const auto& e = p.physical_entries[i];
            if (!mc || e.name != mc->name || e.logical_slot != i) { meta_ok = false; break; }
        }
        ck(meta_ok, "shuffled identity: each physical slot serves the CORRECT callable (indexed by logical_slot, not vector position)");
        // Spot-check: slot 0 -> add1, slot 4 -> fma.
        ck(p.physical_entries[0].name == "add1", "shuffled identity: physical slot 0 serves add1 (logical slot 0)");
        ck(p.physical_entries[4].name == "fma", "shuffled identity: physical slot 4 serves fma (logical slot 4)");

        // The KEYED planner also handles a shuffled manifest (it groups by
        // logical slot, not vector position).
        auto rk = keyed_planner.plan(shuffled, key1_view());
        ck(bool(rk), "keyed plan() succeeds for a dense-but-shuffled manifest");
        ck(rk.value->physical_slot_count == 10, "shuffled keyed: physical_slot_count == 10 (same structure as ordered)");
        // Its placements match the ordered manifest's (the permutation depends
        // on domain/ordinal/salt, not vector order).
        ck(rk.value->build_physical_placement == plan1.build_physical_placement,
           "shuffled keyed: placements identical to ordered manifest (vector order is irrelevant)");
        // The shuffled keyed plan passes the STRENGTHENED validate (which now
        // checks route/entry ordinal <-> DispatchDomain::logical_slots),
        // proving that indexing callables by logical_slot — not vector
        // position — keeps the ordinal/logical-slot mapping consistent.
        ck(bool(keyed_planner.validate(*rk.value)), "shuffled keyed plan passes strengthened validate (ordinal<->logical_slots consistent)");
        // Spot-check the ordinal<->logical_slot invariant directly: for every
        // real entry, its domain's logical_slots[entry.ordinal] must equal the
        // entry's logical slot, regardless of manifest vector order.
        bool ord_slot_ok = true;
        for (const auto& e : rk.value->physical_entries) {
            if (e.is_padding) continue;
            if (e.domain_index >= rk.value->domains.size()) { ord_slot_ok = false; continue; }
            const auto& d = rk.value->domains[e.domain_index];
            if (e.ordinal >= d.logical_slots.size() || d.logical_slots[e.ordinal] != e.logical_slot)
                ord_slot_ok = false;
        }
        ck(ord_slot_ok, "shuffled keyed: every real entry's ordinal maps via its domain's logical_slots to its logical slot");
    }

    // =======================================================================
    // 16c. IDENTITY LAYOUT accepts an empty module (untouched DispatchTable
    //      behavior — an empty identity table is valid).
    // =======================================================================
    {
        ModuleManifest empty; empty.module_id = "empty.mod";
        IdentityLayout id_planner;
        auto r = id_planner.plan(empty, key1_view());
        ck(bool(r), "identity plan() accepts an empty module (empty identity table is valid)");
        ck(r.value->logical_slot_count == 0 && r.value->physical_slot_count == 0, "empty identity plan has zero counts");
        ck(r.value->domains.empty() && r.value->physical_entries.empty() && r.value->padding_descriptors.empty(),
           "empty identity plan has no domains/entries/padding");
        ck(bool(id_planner.validate(*r.value)), "validate() accepts the empty identity plan");
    }

    // =======================================================================
    // 17. KEYED DISPATCH REGISTRY (implicit-keyed-v1 package).
    // =======================================================================
    {
        KeyedDispatchRegistry reg;
        ck(reg.list_names().empty(), "registry starts empty");
        ck(!reg.has("implicit-keyed-v1"), "registry does not have implicit-keyed-v1 before registration");

        ExtensionStatus s = register_implicit_keyed_v1(reg);
        ck(bool(s), "register_implicit_keyed_v1 succeeds");
        ck(reg.has("implicit-keyed-v1"), "registry has implicit-keyed-v1 after registration");

        auto names = reg.list_names();
        ck(names.size() == 1 && names[0] == "implicit-keyed-v1", "list_names returns [implicit-keyed-v1] deterministically");

        auto a = reg.create("implicit-keyed-v1");
        auto b = reg.create("implicit-keyed-v1");
        ck(bool(a) && a.value.has_value(), "create returns a configured instance");
        ck(bool(b) && b.value.has_value(), "create returns a second configured instance");
        ck((*a.value).get() != (*b.value).get(), "create returns FRESH instances (distinct objects)");
        ck((*a.value)->name() == "implicit-keyed-v1", "instance reports its strategy name");

        auto planr = (*a.value)->plan(manifest, key1_view());
        ck(bool(planr) && planr.value->keyed && planr.value->physical_slot_count == 10,
           "registry-produced instance plans the fixture correctly");

        auto unk = reg.create("does-not-exist");
        ck(!unk && unk.error.has_value(), "create unknown name -> structured error");
        ck(unk.error->registry.size() && unk.error->message.size(), "unknown-name error carries diagnostic");

        // --- registry rejection rules ---
        KeyedDispatchRegistry reg2;
        auto se = reg2.add_factory("", []{ return std::unique_ptr<ModuleLayoutConcept>(std::make_unique<IdentityLayout>()); });
        ck(!se && se.error.has_value(), "add_factory rejects empty name");

        auto sn = reg2.add_factory("x", KeyedDispatchRegistry::StrategyFactory{});
        ck(!sn && sn.error.has_value(), "add_factory rejects null factory");

        KeyedDispatchRegistry reg3;
        auto s1 = reg3.add_factory("strat", []{ return std::make_unique<IdentityLayout>(); });
        ck(bool(s1), "first add_factory succeeds");
        auto s2 = reg3.add_factory("strat", []{ return std::make_unique<IdentityLayout>(); });
        ck(!s2 && s2.error.has_value(), "duplicate add_factory rejected WITHOUT replacement");
        auto still = reg3.create("strat");
        ck(bool(still), "after rejected duplicate, original factory remains usable");

        KeyedDispatchRegistry reg4;
        reg4.add_factory("zeta",   []{ return std::make_unique<IdentityLayout>(); });
        reg4.add_factory("alpha",  []{ return std::make_unique<IdentityLayout>(); });
        reg4.add_factory("middle", []{ return std::make_unique<IdentityLayout>(); });
        auto nm = reg4.list_names();
        ck(nm.size() == 3 && nm[0] == "alpha" && nm[1] == "middle" && nm[2] == "zeta",
           "list_names returns entries sorted (deterministic) — not insertion/unordered order");
    }

    // =======================================================================
    // 18. ABI/visibility/domain-label isolation across ALL domains.
    // =======================================================================
    {
        bool no_dup_identity = true;
        for (uint32_t i = 0; i < plan1.domains.size(); ++i)
            for (uint32_t j = i + 1; j < plan1.domains.size(); ++j) {
                const auto& a = plan1.domains[i];
                const auto& b = plan1.domains[j];
                if (a.module_id == b.module_id &&
                    a.visibility == b.visibility &&
                    a.calling_mode == b.calling_mode &&
                    a.abi_fingerprint == b.abi_fingerprint &&
                    a.dispatch_domain == b.dispatch_domain) no_dup_identity = false;
            }
        ck(no_dup_identity, "no two domains share the full identity tuple (exact isolation)");

        const auto& d0 = plan1.domains[0];
        const auto& d3 = plan1.domains[3];
        ck(d0.abi_fingerprint == d3.abi_fingerprint &&
           d0.visibility == d3.visibility &&
           d0.calling_mode == d3.calling_mode &&
           d0.dispatch_domain != d3.dispatch_domain,
           "D0/D3: same ABI+visibility+mode, differ only by explicit label -> distinct domains");
    }

    // =======================================================================
    // Summary
    // =======================================================================
    std::printf("== %d checks, %s ==\n", g_checks, g_fail ? "FAILED" : "ALL PASSED");
    return g_fail;
}
