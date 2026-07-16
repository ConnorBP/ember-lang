# Ember hot reload

Ember supports two reload families:

1. **Identity dispatch:** atomic replacement of one existing function with a
   stable slot.
2. **Keyed dispatch:** replacement of one logical function at its derived
   physical route, plus coherent whole-generation replacement.

Both use `HotReloadDomain` and `ExecutionGuard` to keep replaced executable
pages alive until pre-publication callers have left.

## 1. The guard rule

Every outer host-to-script invocation through reloadable state must enroll in
the associated domain **before** resolving/loading the entry and retain the
guard until JIT code returns:

```cpp
auto guard = domain.guard();
void* entry = table.get(slot);
return reinterpret_cast<Fn>(entry)(args...);
```

One outer guard covers nested script calls and recursion. Do not:

- load an entry before creating the guard;
- retain a raw entry after the guard ends;
- cache a physical keyed slot across keyed generations;
- destroy the domain while a guard exists.

Context state and reload-domain state are separate. A `context_t` does not
implicitly enroll in an epoch.

## 2. Publication and reclamation

`HotReloadDomain` serializes guard enrollment and publication with one mutex.
A successful publication:

1. validates/prepares retirement before changing visible state;
2. release-publishes the new entry or generation record;
3. advances the monotonic epoch once;
4. records replaced owned executable pages with their retirement epoch.

A guard records the epoch at entry. A retired page from publication epoch `E`
is reclaimable when no active guard has an entry epoch below `E`.

APIs:

- `guard()` — create a move-only `ExecutionGuard`;
- `reclaim()` — nonblocking; free currently eligible retired pages;
- `quiesce()` — wait for pages retired through the observed epoch and free them;
- `epoch()`, `retired_page_count()`, `reclaimed_page_count()` — instrumentation.

The domain owns **replaced** executable pages after successful publication. It
does not own entries still current. The host must disown a replaced
`CompiledFn::exec` handle to avoid double-free and must release current pages at
module teardown after guards drain.

## 3. Identity-dispatch single-function reload

The public helper is:

```cpp
ReloadResult reload_function(
    const std::string& replacement_source,
    Program& program,
    DispatchTable& table,
    HotReloadDomain& domain,
    const CodeGenCtx& codegen,
    const NativeTable& natives,
    const OpOverloadTable* overloads,
    const StructLayoutTable* structs);
```

The replacement source must contain exactly one complete function declaration.
Reload requires:

- the function already exists;
- the stable slot is assigned;
- arity is unchanged;
- every parameter type and ABI word shape is unchanged;
- return type and ABI word shape are unchanged.

The helper temporarily installs the replacement AST so whole-program sema can
resolve calls, compiles/finalizes a new page privately, and publishes only after
all steps succeed. Lex, parse, signature, sema, codegen, allocation, or
publication-preparation failure restores the old AST and leaves the table and
epoch unchanged.

`ReloadResult` returns:

- success/error;
- publication and retirement epochs;
- whether an old page was retired;
- the new current `CompiledFn`.

It does not return an owning old-page pointer.

### Slot alias rule

Ordinary domain publication scans the table before retiring the replaced page.
If another table slot still publishes the same page, that page remains current
and is not retired yet. Hosts that mutate dispatch storage directly must still
respect unique page ownership; prefer the domain publication APIs.

## 4. Keyed single-function reload

`reload_keyed_function` takes a stable logical slot plus a transient build
provider. Before publication it verifies that the replacement preserves the
existing logical route's:

- canonical ABI fingerprint;
- visibility;
- calling mode;
- dispatch-domain label and topology membership.

It derives the physical route with the module's existing strategy/version and
publishes that physical entry. It never treats the logical slot as a physical
index.

The provider route word is transient. Ember stores no expected key, key digest,
verifier, or machine fingerprint. A different provider may select a different
bounded destination within the compatible domain; metadata validation and
ownership safety still hold.

Only an actually replaced, owned JIT page transfers to the reclamation domain.
Static padding trap targets and non-owned entries are never retired as
executable allocations.

## 5. Keyed whole-generation replacement

`replace_keyed_generation` can publish a complete new keyed module topology
under an existing stable module ID. The request supplies:

- stable module name and optional expected dense ID;
- a new `ModuleLayoutPlan`;
- backing `RecordBuilderStorage`;
- a destination `ModuleDispatchRecord`;
- a callback providing real entries;
- the `ModuleRegistry` and `HotReloadDomain`.

The implementation builds and validates the new immutable record privately,
preflights registry/publication/retirement state, and performs one coherent
release publication. Readers acquire either the old generation or the new
generation, never a partially mutated record.

On success:

- the stable module ID is unchanged;
- old real executable pages transfer to the domain;
- old record metadata/backing remains the caller's responsibility until old
  guards drain;
- the caller must disown retired old `CompiledFn` allocations.

On failure, no generation record, legacy registry field, epoch, or ownership
state is partially changed.

`keyed_reload_preserves_topology` can be used to decide whether a replacement
is eligible for single-function keyed reload or requires generation
replacement.

## 6. Dispatch visibility and memory ordering

Ordinary `DispatchTable::set` is a release store and `get` is an acquire load.
Keyed generation records are also release-published/acquire-read through the
registry. Finalized code bytes and immutable metadata therefore happen-before a
reader that observes the new pointer.

A frame already executing an old body continues in that body. Calls made after
publication can observe the replacement. Recursive calls may enter the new body
while older recursive frames finish in the old body; this per-call version
identity is intentional.

## 7. What remains unsupported

The ordinary identity-dispatch path does not expose a general whole-module
transaction. It cannot atomically:

- add stable slots for new functions;
- publish unavailable stubs for removed functions;
- migrate globals layouts;
- re-resolve imports as one generation.

Keyed whole-generation replacement does exist, but it is tied to the immutable
keyed module-record model and does not silently provide these semantics for an
ordinary `DispatchTable`/`Program` pair.

Loaded `.em` files do not contain source by default. A host needs retained
source/`Program`/codegen state to compile a source replacement; there is no
“reload this `.em` from itself” API.

## 8. Shutdown checklist

1. Stop accepting new outer calls and reload requests.
2. Join script worker/tick threads.
3. Ensure all execution guards have left.
4. Call `domain.quiesce()`.
5. Release pages still current in identity tables or keyed records.
6. Release module registry, globals, dispatch/record backing, contexts, and
   extension/GC state in dependency order.
7. Destroy the domain last relative to its guards.

The domain destructor performs a final quiesce, but explicit shutdown ordering
is clearer and avoids relying on destruction side effects.

## 9. Migration from the legacy reload API

The pre-v1 API returned a caller-owned `old_entry` and had no persistent domain.
That API is gone. Migrate by:

- keeping one long-lived `HotReloadDomain` beside reloadable dispatch state;
- guarding every outer resolution/call;
- removing all manual old-page frees after successful publication;
- clearing replaced `CompiledFn` ownership in host bookkeeping;
- periodically reclaiming/quiescing;
- draining guards before module destruction.

A temporary domain created inside one reload call is unsafe because guards and
retired ownership must outlive the call.
