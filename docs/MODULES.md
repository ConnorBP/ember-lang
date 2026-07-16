# Ember modules and module formats

Ember has three related but distinct composition/serialization mechanisms:

1. **Textual source import** merges `.ember` files before lexing.
2. **Live `.em` linking** keeps separately compiled `EMBL` modules and routes
   calls through a module registry.
3. **Self-hosted EMBM images** are in-memory executable images emitted by the
   Ember-written compiler during bootstrap.

Do not use `.em`, EMBL, and EMBM interchangeably. EMBL and EMBM have different
magics, layouts, loaders, and version histories.

## 1. Textual source import

```rs
import "math/stats.ember";
```

`src/import.cpp` resolves imports before lexing:

- paths are relative to the importing file;
- canonical paths are deduplicated;
- recursive import cycles terminate through the same seen-path set;
- imported source is merged into one `Program`;
- the result has one globals block, one dispatch space, and ordinary
  intra-module calls.

An import is therefore source composition, not runtime linking. It creates no
module registry entry and no cross-module dispatch operation.

## 2. Live compiled-module linking

```rs
link "filters.em" as filters;

fn process(x: i64) -> i64 {
    return filters::lowpass(x);
}
```

The `link` declaration keeps the target as a separate module. Its functions
and globals retain their own storage. The importer records the target module ID
and logical slot, and generated code resolves the call through
`ModuleRegistry`.

The host may also pre-register a module and link it by the configured name.
File-based linking uses `link_em_file` from `src/module_linker.hpp`.

### Export visibility

- A bare `fn` or `pub fn` is exported.
- A `priv fn` remains callable inside its module but is omitted from the export
  directory.
- Cross-module sema checks the target's canonical signature for v2+ EMBL
  modules.
- Historical EMBL v1 has no canonical signature metadata and remains
  ABI-trusted compatibility input.

## 3. Module registry and dispatch

`ModuleRegistry` assigns a stable dense `module_id`. A module publishes:

- its dispatch storage;
- its logical slot count;
- its cross-module call allowlist/provenance data;
- for keyed modules, an immutable `ModuleDispatchRecord`.

An ordinary cross-module call uses a logical `(module_id, slot)` pair. The
registry supplies the target dispatch state at call time, so callers do not
cache a raw code pointer. Logical IDs and slots are not recycled while callers
can still refer to them.

### Identity dispatch

Identity modules map logical slot `N` to dispatch entry `N`. Ordinary
single-function reload release-stores a new entry into that stable slot.

### Keyed dispatch

A keyed module groups compatible callables into dispatch domains and maps each
logical route to a physical slot through the versioned keyed permutation. The
immutable dispatch record carries:

- dispatch mode and strategy version;
- logical and physical counts;
- ABI/visibility/calling-mode/domain descriptors;
- logical routes;
- physical real and padding entries;
- allowlist data.

Runtime resolution receives a transient route word from the host/provider. The
module does not store or compare an expected environmental key, key digest, or
machine fingerprint. Invalid metadata and padding routes fail closed.

## 4. Public `.em` files: EMBL

Public precompiled `.em` files start with the little-endian magic `EMBL`
(`0x454D424C`). The fixed header is 40 bytes. Versions are additive or
explicitly branched by the loader; unknown versions are rejected.

| EMBL version | Main contract |
|---|---|
| v1 | historical raw x86 and basic relocations; unknown export signatures |
| v2 | canonical signatures, symbolic native bindings, rodata relocations, build/ABI identity |
| v3 | v2 body model plus export-directory `pub`/`priv` visibility |
| v4 | v3 plus a 104-byte Ed25519 signature block |
| v5 | per-function ThinIR or raw-x86 fallback records |
| v6 | v5 body model plus versioned keyed-dispatch metadata and capabilities |

The complete binary layouts and loader policies are documented in
[`BUNDLING_AND_EM_MODULES.md`](BUNDLING_AND_EM_MODULES.md).

### Loader policy

The loader separates authenticity policy from executable-content policy:

- `EmVerifyPolicy` supplies trusted Ed25519 public keys for signed v4 input.
- `EmLoadPolicy::module_permissions` controls permission-gated native binding.
- `EmLoadPolicy::allow_raw_x86` controls compatibility loading of raw machine
  code.
- `EmV6HostCaps` declares supported v6 dispatch strategy, mode, re-emission,
  and ABI-domain capabilities.

The secure default refuses historical raw-x86 EMBL input and mixed/raw v5
functions. All-IR v5 is deserialized, validated, rebound to allowed natives,
and re-emitted before executable allocation. v6 additionally requires an
explicit compatible host capability set.

The CLI and bundler deliberately opt into trusted/development raw-x86 loading
for the artifacts they create. Embedders loading untrusted modules should use
the secure default or an explicit signed policy, not copy the CLI's convenience
policy blindly.

## 5. Self-hosted module images: EMBM v1 and v2

The Ember-written compiler emits an **in-memory** module image with magic
`EMBM`, loaded by the `call_raw` extension's `load_executable_module` native.
This is not the public `.em`/EMBL loader.

### EMBM v1

Contains:

- an 80-byte header;
- code, rodata, mutable data, symbol-name, and relocation sections;
- ABS64 rodata/data/native relocations;
- native ABI fingerprints and permission checks.

### EMBM v2

Extends the header to 96 bytes and inserts a function table. The table maps
logical function slots to code offsets. The loader creates a stable dispatch
vector and supports the `DISPATCH` relocation, enabling self-hosted function
handles, lambdas, and coroutines to use logical slots.

The full format, limits, and loader lifecycle are documented in
[`../self_hosted/MODULE_IMAGE_FORMAT.md`](../self_hosted/MODULE_IMAGE_FORMAT.md).

## 6. Slot and lifetime rules

- A caller caches logical identity, never an unguarded raw executable pointer.
- Dispatch and globals backing storage must not move after addresses are baked
  or published.
- A loaded module must outlive registry entries and callers that reference it.
- Raw `LoadedModule::dispatch` indexes physical storage for v6 keyed modules;
  use `resolve_entry_by_name`/the keyed resolver rather than assuming logical
  slot equals vector index.
- Current executable pages are owned by their module/host. Replaced pages
  transfer to the associated `HotReloadDomain` and are reclaimed only after
  pre-publication guards drain.

## 7. Reload behavior

### Ordinary identity modules

`reload_function` replaces exactly one existing function. It requires the same
name, slot, arity, parameter types/word shapes, and return type/word shape.
Added/removed functions and global-layout changes are rejected because the
ordinary whole-module transaction is not yet exposed.

### Keyed modules

`reload_keyed_function` preserves logical identity and verifies the replacement
against the existing ABI fingerprint, visibility, calling mode, and dispatch
domain. It publishes the derived physical slot, not the raw logical index.

`replace_keyed_generation` can publish a complete new keyed topology under the
same stable module ID. The new immutable record is built and validated
privately, then coherently release-published. Old executable pages remain alive
under epoch guards.

See [`HOT_RELOAD.md`](HOT_RELOAD.md).

## 8. Host integration checklist

1. Resolve textual imports before lexing when compiling source.
2. Construct and retain one `ModuleRegistry` for mutually linked modules.
3. Register dependencies before compiling/linking callers when possible.
4. Supply the real native signature table and explicit permissions to EMBL or
   EMBM loaders.
5. Choose the EMBL verification/raw-code policy deliberately.
6. For v6, advertise only capabilities actually implemented by the host.
7. Keep loaded-module storage and keyed record backing alive for all callers.
8. Guard reloadable outer calls and quiesce reclamation domains before teardown.

## 9. Remaining work

- Ordinary identity-dispatch whole-module transactions
- Added/removed-function publication policy for ordinary modules
- Global-layout migration hooks
- Late re-linking without recompilation
- Platform-neutral module backends beyond the current Win64 x86-64 execution
  formats
