# Bundling and `.em` modules

This document covers two shipped features:

- public precompiled `.em` modules, whose on-disk magic is **EMBL**;
- standalone executables made by appending an EMBL module to the Ember runtime
  stub.

For source composition and registry-level linking, also read
[`MODULES.md`](MODULES.md). For the self-hosted compiler's different **EMBM**
in-memory image format, read
[`../self_hosted/MODULE_IMAGE_FORMAT.md`](../self_hosted/MODULE_IMAGE_FORMAT.md).

## 1. Terminology

| Term | Meaning |
|---|---|
| `import "x.ember";` | textual inclusion before lexing; produces one module |
| `link "x.em" as x;` | live link to a separately compiled EMBL module |
| EMBL / `.em` | public file/byte-buffer module format loaded by `load_em_file` or `load_em_bytes` |
| EMBM | self-hosted compiler's in-memory bootstrap image loaded by `load_executable_module` |
| standalone bundle | runtime stub + embedded EMBL bytes + `EMBD` footer |

The old design names “include” for textual composition and “import” for live
linking did not become the language syntax. The shipped keywords are `import`
for textual composition and `link` for separately compiled modules.

## 2. CLI usage

In these examples, `ember` means `buildt/ember_cli.exe`.

```bash
# Compile source to the standard unsigned EMBL artifact.
ember emit-em app.ember app.em

# Equivalent run-mode spelling; compiles and writes without executing.
ember run app.ember --emit-em app.em

# Load and execute a compiled module.
ember run --load-em app.em --fn main

# Create one standalone executable.
ember bundle app.ember app.exe

# The dedicated frontend uses the same bundler implementation.
ember_bundle app.ember app.exe
```

Bundler options:

```text
--stub PATH                         choose ember_stub_main executable
--fn NAME                           choose entry function (default main)
--permissions none|ffi             source/module permissions (default none)
--output-permissions stub|preserve use stub mode bits or preserve an existing output
```

The bundler compiles source, serializes an unsigned EMBL v3 module in memory,
copies the runtime stub to a same-directory temporary file, appends the module
and footer, applies the selected filesystem permissions, and atomically
replaces the destination. A failure before the final rename leaves an existing
output untouched.

## 3. Standalone executable layout

```text
| ember_stub_main executable | EMBL bytes | footer |
```

The 12-byte little-endian footer is:

```text
magic     u32 = 0x454D4244  ("EMBD")
em_length u64
```

At startup the stub:

1. locates and reads its own executable;
2. validates the footer and embedded length before allocating;
3. extracts the EMBL byte range;
4. registers the same standard native surface used by the bundler;
5. loads through `load_em_bytes` with trusted raw-x86 and FFI permission (the
   current stub is a trusted host; the bundler's `--permissions` option controls
   which gated calls source is allowed to compile, not a serialized runtime
   permission bit in the footer);
6. invokes the stored entry slot;
7. releases module and extension state.

No parser, sema pass, or source compiler runs in the bundled executable. The
runtime still performs EMBL format validation, relocation/native binding, W^X
page publication, and runtime safety checks.

A bundle is target- and ABI-specific. Distribute the matching runtime stub and
module together; do not append a module produced by an incompatible toolchain
to an arbitrary stub.

## 4. EMBL fixed header

All integer fields are little-endian. Every version begins with the same
40-byte header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `0x454D424C` (`EMBL`) |
| 4 | 4 | format version |
| 8 | 4 | flags (currently zero) |
| 12 | 4 | function count |
| 16 | 4 | globals-block byte size |
| 20 | 4 | total per-function rodata bytes |
| 24 | 4 | entry slot or `0xFFFFFFFF` |
| 28 | 4 | versioned identity/reserved word 0 |
| 32 | 4 | versioned identity/reserved word 1 |
| 36 | 4 | versioned identity/reserved word 2 |

For v2+, the last three words carry the compiler/build identity and target ABI
hash. The loader bounds every disk-controlled count and size before allocation.
Current caps include a 256 MiB file maximum, 16k functions, 64k dispatch slots,
64 MiB globals, 16 MiB code/rodata per function, and bounded names/relocations.

## 5. EMBL version history

### v1 — historical raw x86

The per-function record contains name, slot, code, rodata, and compact
relocations. It has no canonical signature or symbolic native-binding metadata.
Exports therefore have `unknown_sig` and are ABI/process trusted.

v1 is compatibility input, not a portable or secure interchange format.

### v2 — canonical ABI and symbolic bindings

v2 adds:

- stable build/compiler and target-ABI identity;
- canonical return/parameter type encodings;
- symbolic native binding records with exact signatures;
- relocation addends and `FunctionRodataBase`;
- load-time native allowlist and permission checks.

A v2 function record contains:

```text
name_len:u16, name, slot:u32
code_size:u32, rodata_size:u32, code, rodata
reloc_count:u32, reloc records
canonical export signature
native_binding_count:u32, native binding records
```

Each v2 relocation is `offset:u32`, `kind:u8`, `addend:u32`.

| Kind | Value patched into the 8-byte code placeholder |
|---|---|
| 0 `DispatchTableBase` | this module's dispatch storage |
| 1 `GlobalsBase` | this module's globals block |
| 2 `ModuleRegistryBase` | host registry backing for cross-module calls |
| 3 `FunctionRodataBase` | loaded function page + code size + addend |

Native pointers are never serialized. The writer zeros their immediate slots
and writes `(offset, name, canonical signature)`. The loader resolves names
only through the host-supplied `NativeSig` table and verifies signature and
permission before publication.

### v3 — export visibility

v3 keeps the v2 function body layout and defines the trailing name directory as
the export table:

- bare `fn` and `pub fn` names are present;
- `priv fn` names are absent, while their code/dispatch entries remain available
  for intra-module calls.

The ordinary `write_em_file` and `write_em_bytes` APIs emit unsigned v3.
The CLI and standalone bundler use this version.

### v4 — signed raw x86

v4 is v3 content plus a 104-byte Ed25519 signature block after the export
directory:

```text
sig_magic  u32 = 0x454D5347 ("EMSG")
payload_len u32
pubkey_id   32 bytes
signature   64 bytes
```

`write_em_file_signed` produces v4. The loader verifies the signature before
allocating executable memory.

`EmVerifyPolicy` behavior:

- empty/no keyring is development mode for unsigned v1-v3, but rejects v4
  because accepting a signed file without verification is misleading;
- a non-empty keyring is signed-only mode: unsigned versions are rejected and
  v4 must verify under a trusted key.

Signing authenticates bytes; build/ABI identity still performs compatibility
checking. These are separate properties.

### v5 — ThinIR on disk

v5 redesigns each function record:

```text
name_len:u16, name, slot:u32
is_ir:u8
canonical signature
if is_ir:
    ir_blob_len:u32, ir_blob
else:
    v4-style raw code/rodata/relocations/native bindings
```

A v5 file may be all-IR, all-raw, or mixed. `write_em_file_v5` and
`write_em_bytes_v5` are the dedicated writers.

For an IR function, the loader:

1. structurally deserializes the versioned `IRFN` blob;
2. rebinds native names through the host table;
3. validates CFG targets, VRegs, frame plans and exact rbp-relative spans,
   dispatch/module slot ranges, types, rodata, and call metadata;
4. re-emits x86-64 using the host's ThinIR emitter;
5. allocates RW memory, applies generated relocations, and seals RX.

Deserialization, semantic validation, and emission occur before executable page
allocation. The current ThinIR blob version is separately versioned from EMBL.
Do not infer its version from the EMBL version number.

v5 has no Ed25519 variant. Under the secure default, every function must be IR.
Any raw fallback requires explicit `allow_raw_x86=true`.

### v6 — keyed-dispatch artifact

v6 retains the 40-byte EMBL header, inserts a versioned keyed-dispatch metadata
block before the v5-style function records, and uses the v5 body alternatives.
Dedicated APIs are `write_em_file_v6` and `write_em_bytes_v6`.

The metadata includes:

- metadata magic/layout version;
- dispatch strategy ID/version and identity/keyed mode;
- logical and physical slot counts;
- a required capability matrix;
- ABI/visibility/calling-mode/dispatch-domain descriptors;
- logical route descriptors;
- physical real/padding entries and padding descriptors.

It deliberately contains no expected runtime key, key digest, verifier, or
machine fingerprint. The serialized physical topology reflects build-time
placement; the loader validates and publishes that topology rather than deriving
and comparing a local secret.

`EmV6HostCaps` must explicitly advertise compatible support, including the
strategy/version, IR re-emission, dispatch mode, raw-code policy, and ABI
domains. The loader rejects contradictory metadata, unsupported capabilities,
out-of-range topology, null physical entries, and malformed records before
publication. Keyed modules expose logical lookup through the immutable dispatch
record; callers must not index physical storage as though it were identity
layout.

## 6. Canonical type and relocation rules

Canonical type records encode primitive kind and recursive modifiers for:

- slices and fixed arrays;
- named structs and typed enums;
- function handles and recorded signatures;
- lambdas and managed pointer shapes where supported by the codec.

The implementation in `src/em_type_codec.*` is authoritative. A producer must
not invent a parallel encoding.

Cross-function script calls do not require code-address relocations: ordinary
calls go through dispatch identity. Branches local to a function are resolved
before serialization. Every process-dependent absolute value must be represented
by an explicit relocation or symbolic binding; unsupported baked process state
makes the function non-serializable for the selected writer.

## 7. Writer APIs

```cpp
bool write_em_file(const EmModule&, const char* path, std::string* err);       // unsigned v3
bool write_em_bytes(const EmModule&, std::vector<uint8_t>&, std::string* err); // unsigned v3
bool write_em_file_signed(const EmModule&, const char* path, ...);              // signed v4
bool write_em_file_v5(const EmModule&, const char* path, std::string* err);      // v5
bool write_em_bytes_v5(const EmModule&, std::vector<uint8_t>&, std::string* err);
bool write_em_file_v6(const EmModule&, const char* path, std::string* err);      // v6
bool write_em_bytes_v6(const EmModule&, std::vector<uint8_t>&, std::string* err);
```

The writers preflight:

- function/name/slot/count limits;
- signature and canonical-type validity;
- relocation offsets, addends, and allowed kinds;
- native-binding offsets and names;
- version-specific metadata consistency;
- IR blob/version requirements for v5/v6.

## 8. Loader APIs and policy

```cpp
bool load_em_file(
    const char* path,
    LoadedModule& out,
    std::string* err,
    ModuleRegistry* registry = nullptr,
    const std::unordered_map<std::string, NativeSig>* native_bindings = nullptr,
    const EmVerifyPolicy* verify = nullptr,
    const EmLoadPolicy* load_policy = nullptr,
    const EmV6HostCaps* v6_caps = nullptr);

bool load_em_bytes(/* same policy arguments */);
```

### `EmLoadPolicy`

```cpp
struct EmLoadPolicy {
    uint32_t module_permissions = 0;
    bool allow_raw_x86 = false;
};
```

The null/default policy is fail-closed:

- no permission-gated native is bound without its permission bit;
- EMBL v1-v4 raw machine code is rejected;
- mixed/raw v5 is rejected;
- all-IR v5 remains eligible;
- v6 additionally requires explicit matching `EmV6HostCaps`.

Compatibility tools and the standalone stub pass an explicit trusted raw-x86
policy because the ordinary writer emits v3. Production hosts should choose a
policy based on the module's trust boundary.

### Transactional loading

The loader parses and validates privately, pre-sizes dispatch/globals storage,
stages pages, applies relocations, and seals pages before moving ownership into
`LoadedModule`. On failure:

- `out` remains unchanged;
- no partial dispatch publication survives;
- staged executable pages are released;
- errors are returned through `false` plus a categorized message.

`LoadedModule` owns dispatch storage, globals, pages, export names, signatures,
and v6 metadata/record backing. Moving a loaded module is supported; copying is
not.

## 9. Live linking

`link_em_file` loads an EMBL artifact and registers its dispatch state under a
stable module ID. The registry publishes the real logical slot count, which the
IR validator and runtime call-target guard use for cross-module bounds checks.

v6 keyed targets publish an immutable `ModuleDispatchRecord`; identity targets
publish ordinary dispatch storage. Cross-module callers inspect the target mode
and use the matching resolver. See [`MODULES.md`](MODULES.md).

## 10. Hot reload interaction

A `.em` file does not embed source by default. Loading it therefore does not by
itself provide source-level recompilation.

- A host that retains the corresponding `Program`, signatures, codegen state,
  and native tables may use the source-level single-function reload API.
- Ordinary identity reload preserves the existing slot and exact call ABI.
- Keyed single-function reload preserves logical identity and publishes the
  derived physical route.
- Keyed whole-generation replacement can atomically publish a new topology.
- There is no general ordinary whole-module `.em` reload transaction yet.

## 11. Security guidance

- Treat raw-x86 EMBL files as native code. Signing authenticates origin/content;
  it does not sandbox the instructions.
- Prefer all-IR v5 for untrusted module input, while recognizing that validated
  IR still becomes native code with the capabilities granted by the host.
- Register only the natives the module needs and grant permissions explicitly.
- Keep trusted signing private keys outside the runtime host.
- Do not use the CLI's development/raw-code convenience policy as a production
  default.
- Retain W^X: writable staging, relocation, then RX publication.
- Bound file sizes, counts, frame spans, dispatch slots, and metadata before
  allocation or indexing.

## 12. Current limitations

- The ordinary CLI/bundler emits v3, not all-IR v5 or keyed v6.
- v5/v6 are unsigned; v4 signing applies to the raw v3 body model.
- Public EMBL execution and self-hosted EMBM execution are Win64 x86-64
  specific.
- Public `.em` files do not carry source for automatic source reload.
- Ordinary identity modules do not yet have a whole-generation reload API.
