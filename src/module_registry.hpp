// ember module registry - per-process module_id -> DispatchTable* (docs/MODULES.md Section 2).
// The runtime half of live `import` (docs/MODULES.md Section 2/Section 3). A single per-process
// registry maps a small dense `module_id` (assigned once at registration) to
// the *current* DispatchTable base of that module. Both JIT-compiled modules
// (the test's hand-built/hand-parsed functions) and `.em`-loaded modules
// (LoadedModule::dispatch) register here; the registry does not branch on
// provenance - exactly the Section 2.6 "the runtime cannot tell a loaded module from
// a JIT'd one" property.
//
// Cross-module call sequence (Section 3), baked into a caller's bytes:
//   mov  r11, [registry_base + module_id*8]   ; <- ModuleRegistryBase reloc
//                                              ;   (AbsFixup kind 2, patched
//                                              ;   with registry->base() at
//                                              ;   JIT fill / .em load)
//   mov  r11, [r11 + slot*8]                  ; load callee's slots[slot]
//   call r11
// `module_id*8` and `slot*8` are compile-time displacements (constants post-
// link); only `registry_base` is position-dependent and gets the reloc.
//
// SLOT STABILITY ACROSS MODULES (Section 4): a foreign caller caches
// (module_id, slot_index) - never the DispatchTable* and never a function
// pointer. At call time `module_id` resolves to the *current* table via the
// registry, which may have been swapped on reload; the caller's bytes are
// unchanged. Registering the same name again (reload) updates the table pointer
// for that id - the id does not change, so callers that cached (module_id,
// slot) pick up the new table on the next call.
//
// REGISTRY-BASE STABILITY INVARIANT (critical, mirrors em_loader.cpp's
// PRE-SIZING INVARIANT):
//   `entries_` is the backing storage whose `.data()` address is baked into
//   JIT'd/.em code as an imm64 (the ModuleRegistryBase reloc, AbsFixup kind 2).
//   If `entries_` reallocates after a base address is baked, `.data()` moves,
//   every baked absolute address dangles, and the next cross-module call jumps
//   to freed/relocated memory. To guarantee stability we:
//     - size `entries_` to `capacity` at construction (capacity is the
//       per-process upper bound on registered modules, set once at registry
//       creation);
//     - fill `entries_` by INDEX (`entries_[id] = table`), never push_back;
//     - never append to `entries_` past the reserved capacity (registering
//       beyond capacity is a hard error, not a silent grow).
//   This is the only correct ordering; violating it is a silent corruption.
//   Host contract: construct the registry with a capacity >= the number of
//   modules that will ever register in this process, and bake `base()` into
//   call sites only after at least the importing module is registered (so the
//   base is already stable - it is stable from construction regardless, since
//   we size up front, but registering the importer before JIT'ing its call
//   sites is the natural order anyway).
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ember {

// v1.0 Tier 2 cross-module function handles (`&mod::fn`): the call-target
// guard validates a cross-module handle against the TARGET module's dispatch
// table + registered-fn allowlist + slot_count, not this module's. The guard
// bakes the base of this per-module records table (handle_records_base) and
// indexes it by module_id at runtime to fetch the target's (dispatch_base,
// allowlist_base, slot_count). One record per registered module, laid out as
// a fixed 24-byte struct (8+8+8, no padding on 64-bit) so the guard can
// compute `record = base + module_id*24` (via imul) and load the three fields
// at fixed offsets.
//
// REGISTRY-BASE STABILITY: handle_records_ is sized to `capacity` at
// construction (like entries_) and filled by INDEX in register_module, so
// handle_records_base() (== handle_records_.data()) never moves after
// construction — the same invariant as entries_/base(). A module that does
// not opt into cross-module handles registers with allowlist_base=nullptr +
// slot_count=0, so a cross-module handle into it fails the slot_count range
// check and traps (correct: it published no callable surface for handles).
struct ModuleHandleRecord {
    void*    dispatch_base;   // [0]  target module's DispatchTable base
    void*    allowlist_base;  // [8]  target module's fn-allowlist byte array
    int64_t  slot_count;      // [16] target module's registered slot count
};

// A per-process module registry (docs/MODULES.md Section 2). One per process; module_id is
// a small dense integer assigned once at registration. The registry's
// `base()` is the address baked into cross-module call sites as the
// ModuleRegistryBase reloc (AbsFixup kind 2 / EmReloc kind 2).
//
// `entries_` is sized to `capacity` at construction and filled by index only;
// see the REGISTRY-BASE STABILITY INVARIANT above. `base()` returns
// `entries_.data()`, which is stable for the registry's lifetime because the
// vector never reallocates (size-at-construction + fill-by-index + no-grow-
//   past-capacity).
class ModuleRegistry {
public:
    // Construct with a fixed upper bound on the number of modules that may
    // register in this process. `capacity` is sized into `entries_`
    // immediately so `base()` is stable from here on. A capacity of 0 is
    // degenerate (no module can register); we allow it for tests that
    // construct a registry they never use, but `register_module` will reject.
    explicit ModuleRegistry(uint32_t capacity);

    // Register a module's dispatch-table base under a name. Returns the
    // stable `module_id` (the index into `entries_`). The dispatch-table
    // pointer is stored by value (Section 2: "the registry holds the *current*
    // DispatchTable*"); the caller owns the storage it points at
    // (`std::vector<void*>` for a JIT'd module, `LoadedModule::dispatch` for
    // a `.em`-loaded module) and must keep it alive for the registry's
    // lifetime.
    //
    // Registering the same name again (reload, Section 4) updates the table pointer
    // for the *existing* id - the id does not change, callers that cached
    // (module_id, slot) pick up the new table on the next call. This is the
    // slot-stability-lifted-one-indirection-up property (Section 4).
    //
    // v1.0 Tier 2 cross-module handles: `allowlist_base` + `slot_count` (both
    // defaulting to nullptr/0) populate the per-module handle-records entry so
    // a `&this_module::fn` handle from ANOTHER module validates against this
    // module's allowlist before dispatch. A module that does not opt into
    // being a handle target passes the defaults (nullptr/0) — a cross-module
    // handle into it then fails the slot_count range check and traps. Existing
    // callers pass 2-3 args (no allowlist) so behavior is unchanged.
    //
    // Returns `UINT32_MAX` and sets *err on failure (capacity exhausted or
    // capacity==0). The id space is dense in [0, count()); an id is never
    // reused (Section 8 non-goal: no entry removal).
    uint32_t register_module(const std::string& name, void* dispatch_table_base,
                             std::string* err = nullptr,
                             void* allowlist_base = nullptr,
                             int64_t slot_count = 0);

    // O(1) lookup of the current dispatch-table base for a module_id (Section 2
    // `resolve`). Used by the cross-module call sequence (Section 3) - but note the
    // hot path does NOT call this C++ function; the baked code reads
    // `[base() + module_id*8]` directly. This method is for the host/linker
    // (Section 5) and tests. Returns nullptr for an out-of-range id (a stale id from
    // a corrupt caller - Section 8 says ids are never recycled, so an out-of-range
    // id is a host bug, not a registry state; we return nullptr rather than
    // throw so a host can surface it as a runtime error per SAFETY_AND_SANDBOX
    // Section 7, matching docs/spec/CODEGEN_SPEC.md Section 7's "slots are never literally null"
    // stance lifted to the registry).
    void* resolve(uint32_t module_id) const;

    // The base address of the `entries_` array - baked into cross-module
    // call sites as the ModuleRegistryBase reloc (AbsFixup kind 2). Stable
    // for the registry's lifetime per the REGISTRY-BASE STABILITY INVARIANT
    // (size-at-construction + fill-by-index + no-grow-past-capacity).
    void* base() const;  // == entries_.data()

    // Number of modules currently registered (dense id space [0, count())).
    uint32_t count() const;

    // The reserved capacity (upper bound on count()). Set once at
    // construction.
    uint32_t capacity() const { return capacity_; }

    // v1.0 Tier 2 cross-module handles: the base address of the per-module
    // handle-records array, baked into a cross-module handle's guard/dispatch
    // as a raw imm64 (process-local, like the fn-allowlist base — NOT a
    // kind-2 reloc; functions using cross-module handles are non-serializable
    // to .em). Stable for the registry's lifetime (handle_records_ is sized
    // at construction + filled by index, same invariant as entries_).
    void* handle_records_base() const;  // == handle_records_.data()
    // The number of modules currently registered (== count()); baked as the
    // module_id range bound for the cross-module guard (mod_id >= this ->
    // out-of-range forged handle -> trap). Equals count() so an id in
    // [count(), capacity()) is rejected (an unregistered slot — handle_records_
    // is zero-filled there, but the range check rejects it first).
    uint32_t handle_records_count() const;  // == count()
    // Read a module's handle record (host/linker + tests). Returns a zero
    // record (null dispatch / 0 slot_count) for an out-of-range id.
    ModuleHandleRecord handle_record(uint32_t module_id) const;

    // Find a module_id by name (Section 2 `find_by_name`, used by the linker stage
    // Section 5, not the hot call path). Returns UINT32_MAX if not registered.
    uint32_t find_by_name(const std::string& name) const;

    // The name a module_id was registered under (for diagnostics / the
    // linker). Returns "" for an out-of-range id.
    const std::string& name_of(uint32_t module_id) const;

private:
    uint32_t capacity_ = 0;                          // reserved upper bound
    std::vector<void*>     entries_;                 // entries_[id] = DispatchTable base
    std::vector<std::string> names_;                 // names_[id] = module name
    std::vector<ModuleHandleRecord> handle_records_; // handle_records_[id] = (dispatch, allowlist, slot_count)
    std::unordered_map<std::string, uint32_t> by_name_;  // name -> id (reload keeps id)
    uint32_t next_id_ = 0;                           // == count() while dense
};

} // namespace ember
