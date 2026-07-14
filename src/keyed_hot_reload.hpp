// keyed_hot_reload.hpp — Red 10 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
// §12, §10, §14.2, §14.3 Red 10): keyed single-function hot reload +
// whole-module keyed-generation replacement.
//
// This is the GREEN side of the Red 10 contract. It implements two
// publication paths on top of the immutable ModuleDispatchRecord (Red 4/7) +
// the HotReloadDomain reclamation class (Red 8):
//
//   - reload_keyed_function:        replace ONE logical callable inside a keyed
//                                   module. Retains the stable logical identity
//                                   (§12.1); validates the replacement's exact
//                                   canonical ABI fingerprint, visibility,
//                                   calling mode, and dispatch-domain membership
//                                   against the existing route's domain identity
//                                   (§12.2); transiently derives the build route
//                                   word through the supplied provider/adapter;
//                                   uses the existing strategy/version and the
//                                   checked domain permutation to select the
//                                   PHYSICAL slot (P(K, domain, ordinal), NOT the
//                                   raw logical index); stages parse, sema, code
//                                   generation, finalization, and validation
//                                   BEFORE publication; compiles with a valid
//                                   keyed generation record so nested direct/
//                                   indirect calls continue to resolve correctly;
//                                   release-publishes ONLY the selected atomic
//                                   physical entry; and transfers ownership of
//                                   an ACTUALLY replaced executable page to the
//                                   HotReloadDomain without ever attempting to
//                                   free the static padding target or another
//                                   non-owned entry. Provider failure, malformed
//                                   metadata, incompatible signatures/domains, or
//                                   publication-preparation failure leave the AST,
//                                   the record, the storage, the epoch, and the
//                                   ownership UNCHANGED. A wrong build provider is
//                                   never compared with an expected key: it may
//                                   replace a different destination, but the
//                                   selected index remains bounded and inside the
//                                   same exact ABI/visibility/calling-mode/domain
//                                   class, and reclamation stays safe. Rejects
//                                   topology/domain changes (directing them to
//                                   replace_keyed_generation).
//
//   - replace_keyed_generation:     replace an ENTIRE keyed module generation
//                                   under the same stable module ID. Privately
//                                   builds and validates a fresh immutable record
//                                   and all backing storage/pages, then publishes
//                                   the record coherently under the existing
//                                   stable module ID using release/acquire
//                                   semantics; NEVER mutates metadata of a
//                                   previously published record. Preflights all
//                                   state/ownership and performs ONE coherent
//                                   publication with no prior observable
//                                   mutations. Old record backing + executable
//                                   pages remain alive while an old guard is
//                                   active (the domain retires them only after
//                                   guards drain).
//
//   - keyed_reload_preserves_topology: the §12.3 regression gate. Returns true
//                                   iff a single-function reload of
//                                   `logical_slot` under `replacement_manifest`
//                                   preserves the existing route's exact ABI
//                                   fingerprint, visibility, calling mode, and
//                                   dispatch-domain label (so a single-slot
//                                   reload is safe). Returns false (with a
//                                   diagnostic in `reason`) if the replacement
//                                   would change domain membership or topology,
//                                   directing the caller to whole-generation
//                                   replacement.
//
// Constraints (§3.3, §6.4, §10.2, §10.4, §12, §14.6): NO expected key, machine
// fingerprint, key digest, or verifier is stored or compared. The route word is
// a TRANSIENT value derived from the supplied provider for the duration of one
// reload and discarded; it only participates in the routing arithmetic (§3.3).
// Publication + retirement are serialized atomically with guard enrollment
// (§10.2, §12.4): a guard is either recorded at the old epoch before the
// release-store, or can enter only after the store completed and records the
// new epoch — there is no window where a guard can enter between retirement and
// publication and execute an old page without pinning it. Only an OWNED
// executable page (a real JIT-compiled page that the reload actually replaced)
// is transferred to the domain; the static padding trap target and non-owned
// entries are NEVER retired or freed. A failed reload leaves the AST, the
// record, the storage, the epoch, and the ownership byte-for-byte unchanged.
//
// Dependency direction: this header pulls codegen.hpp / sema.hpp / parser
// (via ast.hpp) / module_layout.hpp / key_provider.hpp / hot_reload_domain.hpp,
// so the implementation (keyed_hot_reload.cpp) links into ember_frontend (a
// compile-time/reload concern). It depends on keyed_dispatch.* + key_provider.*
// (core ember, reached via ember_frontend -> ember) and module_registry.*
// (core ember). No cycle: the core lib never depends on ember_frontend.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "ast.hpp"                // Program, FuncDecl (for reload_keyed_function)
#include "codegen.hpp"            // CodeGenCtx, CompiledFn
#include "engine.hpp"             // finalize, free_executable (publication/reclamation)
#include "sema.hpp"               // NativeSig, OpOverloadTable, StructLayoutTable
#include "hot_reload_domain.hpp"  // HotReloadDomain (reclamation + guard)
#include "key_provider.hpp"       // DerivedMaterialProvider (transient route)
#include "keyed_dispatch.hpp"     // keyed_dispatch_permute (physical slot selection)
#include "module_layout.hpp"      // ModuleLayoutPlan, ModuleDispatchRecord, RecordBuilderStorage, ModuleManifest, DispatchMode
#include "module_registry.hpp"    // ModuleRegistry (whole-generation publication)

namespace ember {

// ─── A request to reload ONE logical callable inside a keyed module ──────
// (§12.2). The transient build-provider route word is derived from
// `build_provider` (the same domain-separated derivation the build used); the
// physical slot is P(K, domain, ordinal) — the keyed domain slot, NOT the raw
// logical index. `logical_slot` identifies the stable logical callable to
// replace; the replacement source must preserve the exact ABI fingerprint,
// visibility, calling mode, and dispatch-domain membership of that callable.
struct KeyedReloadRequest {
    std::string new_fn_source;     // the COMPLETE replacement declaration
    uint32_t logical_slot = 0;     // the stable logical callable to replace
    std::shared_ptr<const DerivedMaterialProvider> build_provider;  // transient route
};

// ─── The result of a keyed single-function reload ────────────────────────
// On success, the replacement was published at the keyed domain physical slot
// (reported in `physical_slot_published`), the epoch advanced, and the old
// page was transferred to the domain for reclamation. On failure, `ok` is
// false, `error` carries a structured diagnostic, and NOTHING was published and
// the epoch did NOT advance. `physical_slot_published` is the keyed domain slot
// (P(K, domain, ordinal)), which must differ from the raw logical index under a
// topology that permutes (a HARD, testable invariant).
struct KeyedReloadResult {
    bool ok = false;
    std::string error;
    uint64_t publication_epoch = 0;
    uint64_t retirement_epoch = 0;
    bool old_page_retired = false;
    uint32_t physical_slot_published = 0;   // the keyed domain slot (not logical)
    CompiledFn new_fn{};
};

// ─── A request to replace an ENTIRE keyed module generation ──────────────
// (§12.3). The new plan may carry a changed keyed topology (different domain
// layout, different padding, different physical count). The new record is a
// fresh immutable ModuleDispatchRecord published atomically through the
// registry; readers acquire-load one coherent generation (old or new, never a
// mix). Old record backing storage + executable pages remain alive while an old
// guard is active.
struct KeyedGenerationReplacementRequest {
    std::string stable_module_id;            // unchanged across the swap (the name)
    uint32_t expected_module_id = UINT32_MAX;  // optional: the stable dense id the
                                               // caller expects find_by_name(name)
                                               // to return. UINT32_MAX = unspecified
                                               // (look it up by name, no cross-check).
                                               // When specified, the function verifies
                                               // find_by_name(name) == expected_module_id
                                               // BEFORE any publication (stable-ID
                                               // validation); a mismatch is a structured
                                               // failure with no observable mutation.
    const ModuleLayoutPlan* new_plan = nullptr;
    RecordBuilderStorage* new_storage = nullptr;    // host-owned backing for new record
    ModuleDispatchRecord* new_record = nullptr;     // filled from the plan
    std::function<void*(uint32_t physical_slot)> real_entry;  // per real slot
    ModuleRegistry* registry = nullptr;
    HotReloadDomain* domain = nullptr;
};

struct KeyedGenerationReplacementResult {
    bool ok = false;
    std::string error;
    uint64_t publication_epoch = 0;
    uint32_t module_id = UINT32_MAX;         // the stable id (unchanged)
};

// ─── Red 10 §12.2: keyed single-function reload ──────────────────────────
// Publishes at the keyed domain slot (P(K, domain, ordinal)) under the
// transient build-provider route word, NOT the raw logical index. Verifies the
// replacement source preserves the exact ABI fingerprint, visibility, calling
// mode, and dispatch-domain membership. Provider/compile/validation failures
// publish NOTHING and do NOT advance the epoch. Rejects topology/domain changes
// (directing them to replace_keyed_generation).
//
// `prog` is the live program whose function at `req.logical_slot` is replaced
// in-place for the sema pass and restored on failure (the same install/restore
// pattern reload_function uses). `record_storage` + `record` are the module's
// CURRENT immutable keyed dispatch record + its backing storage; the reload
// publish-publishes ONLY the selected physical slot in `record_storage.storage`
// (the record's `physical_slots` points at it). `plan` is the module's layout
// plan (the key-independent domain/route metadata + the build placement the
// physical slot is derived from). `domain` owns replaced executable pages.
// `ctx` is the keyed CodeGenCtx (module_record = &record) the replacement
// compiles under so nested direct/indirect calls resolve correctly. `natives` /
// `overloads` / `structs` are the sema/codegen tables.
KeyedReloadResult reload_keyed_function(
    const KeyedReloadRequest& req,
    Program& prog,
    RecordBuilderStorage& record_storage,
    ModuleDispatchRecord& record,
    const ModuleLayoutPlan& plan,
    HotReloadDomain& domain,
    const CodeGenCtx& ctx,
    const std::unordered_map<std::string, NativeSig>& natives,
    const OpOverloadTable* overloads,
    const StructLayoutTable* structs);

// ─── Red 10 §12.3: whole-generation replacement ──────────────────────────
// Atomically publishes a fresh immutable ModuleDispatchRecord under the same
// stable module ID, possibly with a changed keyed topology. Readers see only
// coherent old or new records. The OLD generation's real executable pages are
// retired into the domain (production-owned lifetime): the domain owns them and
// frees them only after guards drain, so an old reader that holds an old guard
// keeps executing live code. Old record METADATA backing (storage/routes/
// domains/descriptors) remains the caller's responsibility to keep alive while
// an old guard is active (the same contract reload_function has for the
// DispatchTable: the domain owns replaced PAGES, the caller owns the table /
// record backing). The old pages are NOT freed by the caller after a successful
// swap — ownership transfers to the domain (the caller must disown its old
// CompiledFn exec handles for the retired pages to avoid a double-free).
//
// Preflights ALL state/ownership and performs ONE coherent publication with no
// prior observable mutations: it validates the stable name/id, privately builds
// + validates the fresh record + backing storage, then atomically (under the
// domain lock, serialized with guard enrollment) retires the old generation's
// real pages, advances the epoch, and release-publishes the new record. It
// NEVER mutates metadata of a previously published record and NEVER mutates
// legacy registry base/count fields before the record publication. If old-page
// retirement fails (cap exceeded even after a reclaim), NOTHING is published,
// the epoch does NOT advance, and no false success is reported.
KeyedGenerationReplacementResult replace_keyed_generation(
    const KeyedGenerationReplacementRequest& req);

// ─── Red 10 §12.3 regression: topology-preservation check ────────────────
// Returns true iff a single-function reload of `logical_slot` under
// `replacement_manifest` preserves the existing route's exact ABI fingerprint,
// visibility, calling mode, and dispatch-domain label (so a single-slot reload
// is safe). Returns false (with a diagnostic in `reason`) if the replacement
// would change domain membership or topology, directing the caller to
// whole-generation replacement.
bool keyed_reload_preserves_topology(
    const ModuleDispatchRecord& record,
    uint32_t logical_slot,
    const ModuleManifest& replacement_manifest,
    std::string& reason);

} // namespace ember
