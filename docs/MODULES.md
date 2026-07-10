# ember - modules / live `import` (Tier 6 design sketch)

**Status: DESIGN ONLY.** This is a future-work design sketch for
`ROADMAP.md` Tier 6 ("Modules / `import`"). Nothing here is in the v1
grammar, nothing here is implemented, nothing here changes the v1
pipeline. It exists so the deferral is a tracked decision with the
load-bearing design points written down, not a forgotten gap - the
same reason every other `ROADMAP.md` entry has a re-entry trigger and
a dependency rather than a one-line "later."

The split this doc rests on is already decided in
`BUNDLING_AND_EM_MODULES.md` Section 1.1: `include` (parse-time bundle, one
output module) ships now; `import` (live, runtime multi-module
linking) is Tier 6 and lives here. Read Section 1.5 of that doc first - it
records the trigger, the registry shape, and the slot-stability
argument in compressed form; this doc expands them into a design.

---

## 1. Purpose & re-entry trigger

**Why `import` exists:** to let one ember module call into another
module that was compiled separately and loaded at runtime  - 
specifically, a mod loading another mod's pre-compiled `.em`
(`BUNDLING_AND_EM_MODULES.md` Part 2) and invoking its functions.
Each module keeps its own dispatch table, globals block, and slot
assignment; the cross-module call goes through a per-process
registry that resolves "which module" at call time.

**Re-entry trigger (from `ROADMAP.md` Tier 6):** "a real runtime
mod-loading use case (one mod loads another's pre-compiled `.em` at
runtime)." This is not "a script got too big for one file" - that is
source-level file splitting, which `include` covers
(`BUNDLING_AND_EM_MODULES.md` Section 1.1) by merging files into one module
at parse time with zero new runtime machinery. The two needs look
similar from the script author's chair and are completely different
subsystems from the runtime's chair: `include` produces one module;
`import` produces N modules that link at load time.

**Why `include` does not cover this.** `include` is a parse-time
merge into a single `Program` that flows through the existing
pipeline as if it were one file (`BUNDLING_AND_EM_MODULES.md` Section 1.2)  - 
one dispatch table, one globals block, one slot assignment, no new IR
shape, no new runtime state. By the time it runs there *is* no
separately compiled module - everything has been fused. A mod that
wants to call into another mod's already-shipped `.em` needs the
target to remain a separate module with its own stable identity,
looked up at load time. That is `import`, and `include` is
structurally incapable of it without ceasing to be `include`.

The trigger has not fired. Until a real game ships a mod that loads
another mod's `.em` at runtime, this is YAGNI - spec'd, not built.

---

## 2. Module registry

A per-process map from `module_id` to the current `DispatchTable*`
of that module. Both flavors of module register here: JIT-compiled
modules (`ember_compile`, `DESIGN.md` Section 8) and `.em`-loaded modules
(`ember_load_em`, `BUNDLING_AND_EM_MODULES.md` Section 2.5). They share the
same `module_t` shape by construction (Section 2.6 of the bundling doc: "the
runtime cannot tell a loaded module from a JIT'd one"), so the
registry does not branch on provenance.

```cpp
struct ModuleRegistry {
    // Stable id assigned once at registration; baked into cross-module
    // call sites (Section 3). Never reused (Section 8 non-goal: no entry removal).
    uint32_t register_module(const std::string& name,
                             DispatchTable* table,
                             void* globals_block);

    // O(1), used by the cross-module call sequence (Section 3). Returns the
    // *current* DispatchTable*  -  may have been swapped on reload (Section 4).
    DispatchTable* resolve(uint32_t module_id) const;

    // Used by the linker stage (Section 5), not the hot call path.
    uint32_t find_by_name(const std::string& name) const;

  private:
    struct Entry {
        std::string    name;             // for linker name resolution (Section 5)
        DispatchTable* table;            // current; swapped on reload
        void*          globals_block;    // current; swapped on reload
    };
    std::vector<Entry>             entries_;   // module_id = index
    std::unordered_map<std::string,
                       uint32_t>   by_name_;
};
```

- **`module_id` is a small dense integer**, not a pointer or string.
  It is baked into call sites as a literal (`Section 3`), so it must be
  cheap to embed and stable for the caller's lifetime. Assigned once,
  never reused.
- **`table` is a pointer, not a copy.** The registry holds the
  *current* `DispatchTable*`; on reload, the entry's pointer is
  swapped to the new table (Section 4). Call sites never cache the table
  pointer - they cache `module_id` and re-resolve on every call. This
  is the one indirection that buys cross-module reload transparency.
- **Both `table` and `globals_block` are in the entry** because a
  cross-module call may also touch the callee's globals
  (`MEMORY_AND_GC.md` Section 4 - globals are per-module). The cross-module
  globals access path mirrors the call path: `registry[module_id]
  .globals_block` → offset. Carrying both in one entry avoids a
  second lookup; the shape has to carry both.

---

## 3. Cross-module call IR

A new IR op `CallScriptExternal` (or an augmentation of `CallScript`
with an optional `module_id` field - code-organization detail, not a
design decision; this doc writes them as separate ops for clarity).
It carries `(module_id, slot_index)` instead of intra-module
`(slot_index)` alone.

**Cross-module form (one extra indirection):**

```asm
mov  r11, [registry_base + module_id*8]   ; registry_base = absolute imm64
                                          ; baked at compile time
                                          ; (AbsFixup kind 2  -  below)
                                          ; load the Entry's DispatchTable*
mov  r11, [r11 + slot_index*8]            ; load slots[slot_index]  -  the
                                          ; function's current entry
call r11                                  ; same final step as intra-module
```

**Intra-module form (`CODEGEN_SPEC.md` Section 7, for contrast):**

```asm
mov  r11, [dispatch_table_base + slot*8] ; this module's own table base,
                                         ; baked as imm64; no registry hop
call r11                                 ; (spec Section 7 folds the load into the
                                         ; call's addressing mode; the
                                         ; indirection count is the same  - 
                                         ; one absolute load, one indirect
                                         ; call)
```

The cross-module form is **one extra `mov`**: the registry hop. The
intra-module caller knows its own dispatch table at compile time and
skips the lookup; the cross-module caller knows only the *foreign
module's identity* (`module_id`), not its current table address, so
it pays one indirection to turn identity into table. That is the
entire runtime cost of true dynamic linking in this design - one load
per cross-module call, on top of the intra-module baseline. Call it
the price of the registry's reason for existing.

**Relocation kind.** `registry_base` is a new absolute-imm64 the
codegen bakes into cross-module call sites. It fits the `AbsFixup`/
reloc-table mechanism introduced for the `.em` loader
(`BUNDLING_AND_EM_MODULES.md` Section 2.4) as a **new kind 2,
"ModuleRegistryBase"**:

```cpp
struct AbsFixup {
    uint32_t code_offset = 0;
    enum Kind : uint8_t {
        DispatchTableBase   = 0,   // existing  -  bundling doc Section 2.4
        GlobalsBase         = 1,   // existing  -  bundling doc Section 2.4
        ModuleRegistryBase  = 2,   // NEW: per-process registry base addr
    } kind;
};
```

The `module_id` itself is a compile-time literal baked into the
`[registry_base + module_id*8]` displacement (constant at the call
site post-link - Section 5), so it needs no reloc of its own; only
`registry_base` is position-dependent and gets patched when a `.em`
containing a cross-module call site loads into a fresh process whose
registry lives at a different address.

---

## 4. Slot stability across modules

`HOT_RELOAD.md` Section 1 establishes slot stability within a single
module: "slot indices never change for the lifetime of the
`module_t`," which makes intra-module reload safe - callers bake
`slot*8` into their compiled code, so a slot's *content* (function
address) can be swapped without touching any caller's bytes.
Cross-module calls lift this invariant one indirection up.

- **Within each module, slots are still stable for that module's
  own lifetime.** A foreign module's slot indices do not change
  while it is loaded; a reload repoints slot *contents*, never
  renumbers slots (`HOT_RELOAD.md` Section 3).
- **A foreign caller caches `(module_id, slot_index)`** - identity
  of the target module, and a slot inside it. It does *not* cache the
  target's `DispatchTable*` and does *not* cache a function pointer;
  both go stale on reload.
- **At call time, `module_id` resolves to the *current*
  `DispatchTable*`** via the registry (`Section 2 resolve()`), which may
  have been swapped if the foreign module was reloaded since the
  caller was compiled. The caller then indexes `slots[slot_index]`
  on that current table.
- **A reload of the foreign module is transparent to the caller.**
  The caller's bytes (`mov r11, [registry_base + module_id*8]`) are
  unchanged - the registry entry's `table` pointer changed, the
  caller's code did not. The caller picks up new function addresses
  on its next call with no recompilation of itself.

This is the same property intra-module reload already has
(`HOT_RELOAD.md` Section 3: "the swap only affects *future* `call [slot]`
instructions, which read the slot fresh each time"), lifted one
indirection up: the registry slot plays for modules the role the
dispatch slot plays for functions. The cost is the one extra `mov`
from Section 3; the benefit is that cross-module reload needs no
cross-module caller recompilation - exactly the goal that motivated
the dispatch table in the first place (`CODEGEN_SPEC.md` Section 7: "hot-
reloading any function ... never requires touching the caller's code
bytes at all").

One asymmetry, inherited from `HOT_RELOAD.md` Section 4: a cross-module
call already past the registry load and into the foreign function's
body completes in the *old* version of that function, exactly as an
intra-module in-flight call does. Reload affects *future* calls, not
frames already inside the callee. No new machinery; the property
composes.

---

## 5. Linker stage

`import "foo"` is resolved at **load time**, not parse time (the
defining difference from `include`, `BUNDLING_AND_EM_MODULES.md`
Section 1.1). The flow:

1. **Compile records unresolved externals.** A `modname::fn`
   reference (Section 6) where `modname` is an imported (not-included) module
   becomes an unresolved-symbol record: `(target_module_name, fn_name,
   expected signature, call site location)`. The call site is emitted
   with a **trap stub** as its operands initially (same pattern as
   `HOT_RELOAD.md` Section 2 / `CODEGEN_SPEC.md` Section 7 edge case: "slot points
   at a shared trap stub rather than a null/garbage pointer"), so the
   module is *runnable* before linking but any cross-module call
   traps.
2. **Linker walks the unresolved list.** For each record:
   - Look up `target_module_name` via `find_by_name` (Section 2). If not
     registered → unmet import; the linker leaves the trap stub in
     place and reports it (the module runs, calls to the missing
     module trap - see "unresolved at call time" below). Whether an
     unmet import is a hard error or a deferred trap is a host policy
     decision; this design makes it a deferred trap, matching
     `CODEGEN_SPEC.md` Section 7's "slots are never literally null during
     normal operation" stance (a missing module is the cross-module
     analog of a not-yet-compiled function).
   - If registered → retrieve the target's `DispatchTable*` and its
     name→slot directory (`HOT_RELOAD.md` Section 7: the directory
     `ember_call` uses for host→script lookup; the linker reuses it
     for cross-module name lookup - same data, no duplicate).
   - Look up `fn_name` in the target's directory. Absent → unmet
     import, trap stub stays.
   - **Signature handling:** JIT-module and v2 `.em` exports carry canonical
     `Type` signatures and sema verifies arity, ordered parameter types, and
     return type. Historical v1 `.em` exports remain `unknown_sig`; those
     imports are ABI-trusted and skip signature verification.
   - On success → **bake `(target_module_id, target_slot_index)`
     into the call site**, rewriting the `module_id` and `slot_index`
     displacements in the Section 3 sequence from trap-stub placeholders to
     the resolved values. The `registry_base` reloc
     (`AbsFixup::ModuleRegistryBase`) is untouched - it was already
     correct; only the two displacements change.
3. **Unresolved at call time.** The current source linker marks unresolved
   calls for the existing runtime trap lowering; hosts should treat unresolved
   links as load/compile failures rather than rely on a removed-function or
   uncompiled-function slot stub (those stubs are not shipped APIs).
4. **Late re-linking is deferred.** No `relink_imports(module_t*)` API ships.
   A host resolves/registers dependencies before compiling the importer or
   recompiles after the dependency becomes available.

---

## 6. Shipped link grammar

The v0.5 live-module trigger fired and the parser accepts:

```
link_stmt     := 'link' STRING_LIT 'as' IDENT ';'
call_external := IDENT '::' IDENT '(' arg_list? ')'     // modname::fn
```

- `link "file.em" as alias;` loads/registers a file relative to the source
  file. `link "registered-name" as alias;` is the host-driven form for a
  module already in the registry. The alias is required by the grammar.
- `modname::fn(args)` - a call into `fn` of the imported module. The
  `::` is the cross-module selector; an unqualified `fn(args)` is the
  existing intra-module call (`CODEGEN_SPEC.md` Section 7). The `::` makes
  the distinction visible at the call site, which matters for reading
  code and makes the IR op choice (`CallScript` vs
  `CallScriptExternal`, Section 3) a trivial parse-time decision rather than
  a name-resolution guess.
- **No `pub`/`export`.** Visibility remains deferred; every
  function in a registered module is callable cross-module, matching
  C `#include`'s "everything included is visible" stance
  (`BUNDLING_AND_EM_MODULES.md` Section 1.3) and the existing flat module
  scope (`COMPILER_PIPELINE.md` Section 4).

---

## 7. Why this does not ship with `include`

The two features share a keyword family and a user mental model ("pull
in code from elsewhere") and almost nothing else.

| | textual `import` | live `link` |
|---|---|---|
| Stage | parse time, before sema | load time, after each unit compiles |
| Output | one module | N modules |
| New runtime state | none  -  merged `Program` flows through existing pipeline (`BUNDLING_AND_EM_MODULES.md` Section 1.2) | module registry (`Section 2`), cross-module call IR (`Section 3`), linker stage (`Section 5`) |
| New IR op | none  -  ordinary `CallScript` through the shared table | `CallScriptExternal` carrying `(module_id, slot_index)` |
| New encoder reloc | none | `AbsFixup::ModuleRegistryBase` (`Section 3`) |
| Signature verification | none — same module, sema checked it | JIT exports: yes; v1 `.em`: no metadata, ABI-trusted `unknown_sig` (`Section 5`) |
| Status | shipped source merge | shipped v0.5; v1 `.em` signatures remain ABI-trusted |

Textual `import` is a parse-time merge with no new runtime machinery. Live
`link` is the shipped registry/kind-2-relocation path. The remaining gap is
metadata: v1 `.em` exports do not carry canonical signatures, so link-time type
verification applies only to JIT exports.

---

## 8. Dependencies & non-goals

**Hard dependency:**

- **The `.em` loader (`BUNDLING_AND_EM_MODULES.md` Part 2).** The Section 1
  trigger is literally "one mod loads another mod's pre-compiled
  `.em` at runtime." `load_em_file`/`link_em_file` produce and register the
  `LoadedModule`; callers must retain it while the registry points at its
  dispatch storage.

**Companion, not a hard dep:**

- **Namespaces (`ROADMAP.md` Tier 6).** Roadmap lists namespaces as a
  separate Tier 6 entry ("name scoping within a module; trigger:
  module size makes flat scope crowded; dep: modules, or usable
  standalone"). Namespaces and modules are siblings, not
  parent/child: this doc's registry and call mechanism work without
  namespaces (every function in a registered module is callable
  cross-module, Section 6), and namespaces can ship without cross-module
  calls (scoping *within* one module needs no registry). They compose
  when both exist; neither blocks the other. The Section 6 sketch's "no
  `pub`/`export`" note is the seam where namespaces plug in later,
  not a prerequisite.

**Hard non-goals (things this design deliberately does not do):**

- **No cross-process modules.** The registry is per-process (`Section 2`).
  A mod in process A cannot call a mod in process B through this
  mechanism; that is an IPC problem, not a linking problem, and ember
  is an in-process embedded scripting language (`DESIGN.md` goals:
  "for embedding in game engines"). Out of scope forever, not just
  for v1 of this feature.
- **No dynamic unloading / module-registry-entry removal.**
  `HOT_RELOAD.md` Section 5's guarded epoch retirement covers replaced *code
  pages* only; it does not remove a `module_t` from the registry once
  registered. A `module_id` baked
  into a call site (`Section 3`) must remain valid for the caller's lifetime
  - unregistering a module would orphan every cross-module call site
  that cached its id, turning them into registry reads of a stale
  index. The Section 2 design assigns ids once and never reuses them
  precisely to avoid this; entry *removal* is the symmetric operation
  and is not provided. If a host needs to "drop" a module, the
  supported path is to leave its registry entry in place and repoint
  retain the entry and arrange its own unavailable-target policy; the proposed
  removed-function trap stub is deferred with whole-module reload
  (`HOT_RELOAD.md` Section 2). This is the "slot indices are never
  recycled in v1" stance lifted one level up: module ids are never
  recycled either.
- **No version negotiation.** JIT exports can be signature-checked. v1 `.em`
  exports are `unknown_sig` and therefore ABI-trusted; the linker cannot
  verify them. The format has no semver/build identity or canonical signature
  records. A future version must add those records before claiming portable,
  verified `.em` linking.

---

This doc is a design sketch, not a spec amendment. It adds no
grammar, no IR op, no encoder change, no source file. The Section 1 trigger
("a real runtime mod-loading use case") is still the gate, and
`ROADMAP.md`'s re-evaluation cadence ("after the v0.5 benchmark
milestone") still governs when the trigger is even *checked* against
reality. Writing this doc is documentation, not a commitment to build
 -  recorded so the next person who hits the trigger does not start
from a blank page. That is the entire purpose of a deferred-design
doc; it is the same purpose `ROADMAP.md` serves at the higher level of
"which features at all."
