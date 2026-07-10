# ember - bundling + `.em` pre-compile plan

Detail plan for two related additions the user asked for:

1. **Bundling as an option** - split file composition into two modes:
   one that forces a *live* (separately-compiled, runtime-linked) import,
   one that allows *bundling* the imported script into the same output.
   Multiple files pulling in the same file bundle it exactly once.
2. **Pre-compile to a `.em` module** instead of always JIT - either a
   dense intermediary or final bytecode. This doc weighs the options
   and picks one.

This is a **plan**, not a spec amendment. It respects every existing
invariant - dispatch-table slot stability (`HOT_RELOAD.md` Section 1), "JIT
only, no bytecode interpreter fallback" (`DESIGN.md` non-goal),
globals-block-by-absolute-address and string-literals-as-function-
local-rodata (`MEMORY_AND_GC.md` Section 4/Section 6) - and re-uses the existing
pipeline rather than adding a second execution path.

---

## Part 1 - `include` (bundle mode) vs `import` (live mode)

### 1.1 The cut, and why it's the right one

The two modes are not two flavors of one mechanism. They are two
*different* subsystems, and bundling is the cheap one.

| | `include "x.ember"` (bundle) | `import "x.ember"` (live) |
|---|---|---|
| When it happens | parse time, before sema | load time, after each unit compiles |
| Output | **one** module: one dispatch table, one globals block, one slot assignment | **N** modules: one dispatch table + globals block each |
| Cross-unit call | ordinary `CallScript` through the single shared dispatch table (same as any intra-file call today) | new cross-module indirection: module registry → foreign `DispatchTable` → slot |
| New runtime machinery | **none**  -  merged `Program` flows through the existing pipeline | module registry, cross-module call IR shape, linker stage |
| Dedup | parse-time include-graph walk, set of canonicalized paths | not needed (separate units, separately compiled) |
| Matches existing | exactly the current single-file compile, just fed a merged AST | `ROADMAP.md` Tier 6 "Modules / `import`" deferral |

This split is the YAGNI-correct reading of the request. "Bundle"
means "compile a set of source files as one module" - the dedup is a
parse-time concern, slot assignment happens once for the merged AST,
and *nothing downstream changes*. "Live import" is the actual
multi-module feature (`ROADMAP.md` Tier 6): cross-module dispatch
indirection, link-by-name at load, a linker stage. The user's
re-entry trigger for Tier 6 is "a mod is big enough that one file is
unmaintainable" - but that need is satisfied by *source-level file
splitting*, which `include` provides. True runtime module separation
(a mod loads another mod's already-compiled `.em` at runtime) is a
larger subsystem with no demonstrated need yet.

**Decision:** ship `include` (bundle) now. Spec `import` (live) in
`docs/MODULES.md` as future work, do not implement it. The two
keywords are named to make the semantics obvious and to match the
user's own phrasing ("one of them forcing non bundle so it imports it
live"): `include` is the C `#include` analog (merged before
compilation, one output), `import` is the Rust/Python `import` analog
(separate unit, linked at load).

### 1.2 `include` - design

**Grammar** (added to `COMPILER_PIPELINE.md` Section 2, top-level):

```
program      := (include_stmt | annotation* func_decl | struct_decl | global_decl)*
include_stmt := 'include' STRING_LIT ';'
```

`include` is a **parse-time directive**, not an AST node that
survives to sema. The driver expands it before sema runs. This is the
same layering as C's `#include` (textual, pre-compilation) and is
deliberately *not* a statement that can appear inside a function
body - only at module top level - so there is no "include inside a
loop" or "include inside an `if`" question to answer. (YAGNI: nested
includes inside blocks add a whole scoping question with no use case;
top-level-only is the C-family default and is enough.)

**Driver include resolver** (new `src/include_resolver.hpp/.cpp`):

- `struct IncludeResolver { std::unordered_set<std::string> seen; }`
  keyed by **canonicalized absolute path** (case-correct, symlinks
  resolved via `std::filesystem::canonical`/`weakly_canonical`).
- On `include "path"`:
  1. Resolve `path` relative to the **current file's directory**
     (not a global include root - matches C `#include "..."` local-
     first semantics; a host-configured include root is a v2 addition,
     not needed for v1 game-scripting where files ship together).
  2. Canonicalize. If in `seen` → **skip** (this is the dedup: a file
     is parsed and merged at most once, no matter how many includers
     pull it in). If absent → insert, recursively parse that file,
     merge its top-level declarations into the accumulating `Program`.
- **Circular includes terminate for free**: `A includes B includes A`
  → A is parsed and inserted into `seen` before B is parsed; when B
  includes A, A is already in `seen`, the include is a no-op. No
  cycle-detection pass needed; the `seen` set *is* the cycle break.
- **Merge semantics**: the parsed file's `Program` (structs, globals,
  fns) is appended into the parent's `Program`. Declaration order
  across files follows include order (depth-first, like C). Duplicate
  *declarations* across included files (two files both declare
  `fn foo`) → **sema error**, identical to a duplicate within one
  file today - the resolver does not deduplicate declarations, only
  *files*. This is correct: dedup is "parse this file once," not
  "merge identical declarations" (the latter would silently paper
  over real conflicts).

**Sema and everything downstream: unchanged.** The merged `Program`
flows through the existing sema passes and tree-walking x64 emitter as if it
had been one file: one dispatch table, one globals block, one module. The
shipped single-function reload can replace an existing function from that
merged program; whole-module include re-resolution is deferred.

**`SourceLoc.file`**: the `SourceLoc` struct (`COMPILER_PIPELINE.md`
Section 7) already carries `const char* file`. The resolver must ensure each
parsed file's tokens carry that file's own name so diagnostics point
at the right file. This is a property of the lexer being told which
file it's lexing, not new machinery - just "don't share one filename
across the whole merge."

### 1.3 What `include` does *not* need

- No namespace / module-name concept. All included names land in one
  flat module scope (matches `COMPILER_PIPELINE.md` Section 4's existing
  flat module scope; namespaces are `ROADMAP.md` Tier 6).
- No visibility / `pub` / `export` - everything included is visible
  (matches C `#include`; a private-include model is a language
  extension with no v1 use case).
- No include guards / `#pragma once` - the `seen` set is the guard.
- No conditional includes - `include` is unconditional; conditional
  composition is `engine.define` + `const`/`constexpr` territory
  (`DESIGN.md` Section 8, `ROADMAP.md` hard non-goal "no preprocessor").

### 1.4 Interaction with hot reload

The shipped `reload_function` helper takes one complete replacement function
and recompiles it by name. With textual inclusion, the function belongs to the
merged `Program`, but reload remains per-function:

- **Single-function reload** (`reload_function`): unchanged.
  The host hands the new body of *one* function; includes don't
  participate (a function's body doesn't contain includes - includes
  are top-level only, Section 1.2). The host is responsible for re-resolving
  includes itself if the reloaded function now calls a symbol that
  came from an included file - but that symbol is already in the
  module's slot table from the initial compile, so the reload just
  resolves the call against the existing slots. **No include
  machinery runs on reload.**
- **Whole-module reload:** deferred. A future implementation would need to
  re-resolve the root and apply a transactional batch; v1 does not claim this.

### 1.5 `import` (live) - design sketch only, not implemented

For completeness, the Tier 6 design lives in a new `docs/MODULES.md`
(not written in this pass - it's future work). The load-bearing
design point, recorded here so it isn't lost:

- A **module registry**: per-process map `module_id → DispatchTable*`.
  Loaded `.em` files (Part 2) and JIT-compiled modules both register.
- A cross-module `CallScript` IR op carries `(module_id, slot_index)`;
  codegen emits one extra indirection vs intra-module calls: load
  registry base → load `registry[module_id].slots` → load
  `slots[slot_index]` → `call r11`. That is one more `mov` than the
  intra-module form (`CODEGEN_SPEC.md` Section 7), the cost of true runtime
  linking.
- **Slot stability still holds within each module**; cross-module
  calls go through the registry, so a foreign module's slot table can
  be reloaded independently (its slots are stable *for its own
  lifetime*; a foreign caller cached a `(module_id, slot_index)` pair,
  and `module_id` resolves to the current `DispatchTable*` of that
  module, so a reload of the foreign module is transparent to the
  caller - same property intra-module reload already has, lifted one
  indirection up).

This does **not** ship now. The re-entry trigger is a real runtime
mod-loading use case (one mod loads another's pre-compiled `.em` at
runtime), which bundling does not cover and which does not exist yet.

---

## Part 2 - `.em` pre-compile (Option B: serialized native code)

### 2.1 Weighing the options

The user asked to weigh "dense intermediary" vs "final bytecode"
for the `.em` format. There are three real options:

**Option A - serialized IR (dense intermediary).** Serialize the
`IrFunction` (blocks, instrs, vreg types, params) + dispatch slot
table + globals layout. The loader re-runs `run_linear_scan` +
`emit_x64` (`COMPILER_PIPELINE.md` Section 8) to produce machine code at load
time.

- *Pro:* smaller file than raw machine code (IR is denser than x64);
  load is "fast regalloc+emit" which the spec already claims is
  sub-ms-per-function.
- *Con:* **the loader must ship the entire regalloc+emit backend.**
  This partially defeats the point of pre-compiling: the work moved
  from compile-time to load-time is only the parse+sema+lower stages,
  not the backend. The host still links the whole codegen. The only
  real win is "no parser/sema on load."
- *Con:* regalloc and emit are still in flux (v0.1 is a proof; the
  future SSA-lite/linear-scan design and encoder may change). An IR-format `.em` baked today
  breaks the moment the IR shape changes - a serialized IR is a
  de-facto ABI you must version and migrate.

**Option B - serialized native code + relocations.** Serialize the
already-emitted x64 bytes per function + the dispatch slot table + the
globals block + a small relocation table. The loader `mmap`s/`memcpy`s
code pages and fixes up absolute addresses.

- *Pro:* **load is `memcpy` + a couple pointer fixups per function.**
  No regalloc, no emit, no parser, no sema on the load path. The
  loader is tiny (a header reader + a fixup applier). This is the
  maximal pre-compile win.
- *Pro:* **exactly one execution path** - native x64, same as JIT.
  The `DESIGN.md` non-goal is "no bytecode interpreter fallback,"
  meaning *one* execution path, and that path is native. Serialized
  native code is not a second path; it's the same path with the
  regalloc+emit step moved earlier. This is the MCJIT-vs-ORC
  distinction: pre-compiled object code with relocations is not
  "bytecode." Fully consistent with the spec's stance.
- *Pro:* the file is target-specific (Win64 x86-64), but ember is
  Windows-first x86-64-only (`CODEGEN_SPEC.md` Section 1), so cross-platform
  `.em` is not a real concern. Option A's only theoretical advantage
  (portable IR) doesn't apply.
- *Con:* larger file than IR (raw x64 is less dense). Acceptable  - 
  `.em` files ship with a game, not downloaded per-call.
- *Con:* needs a relocation table for every absolute-imm64 the
  codegen bakes in. Today that is two kinds: the dispatch-table base
  (`mov r11, <table base imm64>`, `CODEGEN_SPEC.md` Section 7) and the
  globals block base (`mov reg, [globals_base + offset]`,
  `MEMORY_AND_GC.md` Section 4). Both are baked as raw `imm64` with **no
  fixup entry today** (verified in `label_patch.hpp`: only branch
  `rel32` and rodata `RipFixup` disp32 exist; absolute-imm64 has no
  capture path). So part of the work is *adding* that capture path
  (Section 2.4). This is a small, contained change to the encoder.

**Option C - a bytecode interpreter.** Explicitly rejected. This is
the `DESIGN.md` non-goal: "JIT only, no bytecode interpreter fallback  - 
one execution path." A bytecode `.em` would create a second execution
path (the interpreter) alongside the JIT, which is exactly what the
spec refuses. It would also reintroduce the "5-50× interpreter
overhead" `GAP_ANALYSIS.md` Section 4 deliberately avoids. Not considered
further.

**Decision: Option B.** It maximizes the pre-compile win, keeps one
execution path, and is consistent with the spec's anti-bytecode
stance. The cost is the relocation-capture work (Section 2.4), which is small
and is needed anyway once absolute addresses are baked (which they
already are for the dispatch table and globals).

### 2.2 `.em` file format (Option B)

A self-describing container. Little-endian throughout (matches x86-64
and the existing encoder's `emit32`/`emit64`).

```
Header (40 bytes):
  magic           : u32   = 0x454D424C   ("EMBL"  -  ember bundle, loadable)
  version         : u32   = 1
  flags           : u32   = 0            (bit 0 reserved: "contains source"
                                           for future embed-source reload)
  function_count  : u32
  global_size     : u32                  (bytes of the globals block)
  rodata_total    : u32                  (bytes of all per-function rodata,
                                           sum of per-fn rodata_size)
  entry_slot      : u32                  (slot of @entry fn, or 0xFFFFFFFF)
  reserved        : u32[3] = 0

Per-function (repeated function_count times):
  name_len        : u16
  name            : bytes[name_len]       (UTF-8, no NUL)
  slot_index      : u32
  code_size       : u32                   (x64 bytes)
  rodata_size     : u32                   (trailing rodata for this fn's
                                          string/const literals; 0 if none)
  code            : bytes[code_size]      (raw x64, position-independent
                                          EXCEPT for the reloc'd imm64s)
  rodata          : bytes[rodata_size]    (appended right after code,
                                          RIP-relative from code, same page)
  reloc_count     : u32
  relocs          : reloc[reloc_count]
    reloc:
      offset      : u32                   (byte offset within `code` of the
                                          imm64 to patch)
      kind        : u8
        0 = dispatch_table_base   (patch imm64 -> &this module's DispatchTable)
        1 = globals_base          (patch imm64 -> &this module's globals block)
        2 = slot_index_load       (patch imm64 -> &DispatchTable itself, then
                                   the [r11 + slot*8] load is already correct
                                   since slot*8 is a compile-time const disp32;
                                   this reloc exists only if codegen emitted
                                   a raw slot-pointer load rather than the
                                   base+disp form  -  v1 uses base+disp, so
                                   kind 2 is reserved/unused in v1)

Globals block:
  bytes[global_size]                     (initial values, copied verbatim
                                          into the allocated globals block)

Function-name directory (for ember_call by name, HOT_RELOAD.md Section 7):
  name_table_count : u32
  entries: [ { name_len: u16, name: bytes, slot_index: u32 } ... ]
```

**Why rodata is per-function, not a shared section.** `MEMORY_AND_GC.md`
Section 6 specifies string literals as *function-local rodata in the code
page*, with `RipFixup` disp32 already resolved intra-page by
`LabelPatch::resolve_fixups`. Keeping rodata per-function in the
`.em` (appended right after each function's code, same page) means
the existing RIP-relative disp32 values are **already correct** after
a straight `memcpy` - no rodata relocation needed. This is the design
choice that minimizes relocation kinds to just the two absolute
imm64s (dispatch base, globals base). A shared rodata section would
require a third reloc kind and break the "string literals live in
their function's page" invariant for no benefit.

**Why no cross-function `rel32` reloc.** `CODEGEN_SPEC.md` Section 4
explicitly forbids cross-function `rel32` references - script-to-
script calls go through the dispatch table, never same-image direct
calls. So the only fixups are within a function (branches + rodata,
already resolved by `resolve_fixups` before serialization) and the
two absolute imm64s (new reloc table). The serialized bytes are
otherwise position-independent.

### 2.3 Serializer (compile-time, after `emit_x64`)

New `src/em_writer.hpp/.cpp`. Runs after the full pipeline produces
the `module_t` (or the v0.1 equivalent: the list of `CompiledFn` +
the dispatch table + the globals block). For each function:

1. Take the final `code` vector (post-`resolve_fixups`) and its
   trailing `rodata` blob.
2. Emit the per-function record: name, `slot_index`, `code_size`,
   `rodata_size`, the code bytes, the rodata bytes.
3. Emit the reloc list: every absolute-imm64 the codegen baked,
   captured during emit (Section 2.4). For v1, that is the dispatch-table
   base load(s) and the globals-base load(s) per function.
4. After all functions: the globals block (raw initial-value bytes)
   and the name→slot directory.

The serializer needs the slot table (name→index) and the globals
block address/size, both of which the compile pipeline already
produces. No new compile-time state.

### 2.4 Relocation capture - the one encoder change

Today (`encoder.hpp` / `label_patch.hpp`) there is **no fixup path for
absolute-imm64**. `mov_r_imm64` (`48 B8+r <imm64>`) writes the 8
bytes raw. The dispatch-table base and globals base are baked as raw
`imm64` in `main.cpp`'s `build_fib` (`enc.mov_r_imm64(Gp::r11,
reinterpret_cast<uintptr_t>(dispatch_table_base))`). There is no way
for a serializer to find these after the fact without scanning bytes
(brittle).

**Fix: add a third fixup kind to `LabelPatch`.**

```cpp
struct AbsFixup {
    uint32_t code_offset = 0;   // offset of the imm64 within `code`
    enum Kind : uint8_t {
        DispatchTableBase = 0,  // patch with &module_dispatch_table
        GlobalsBase       = 1,  // patch with &module_globals_block
    } kind;
};
std::vector<AbsFixup> abs_fixups_;
void add_abs_fixup(AbsFixup f) { abs_fixups_.push_back(f); }
```

Then a new encoder entry point for "load an external base address
that will need relocating in a `.em`":

```cpp
void mov_r_imm64_external(Gp dst, AbsFixup::Kind kind);
   // emits 48 B8+r <8 zero bytes> and records an AbsFixup at the
   // imm64 offset with the given kind. The 8 bytes are placeholders;
   // at JIT time the driver fills them with the real address (same
   // as today's raw imm64), at .em-write time the serializer records
   // the reloc, at .em-load time the loader patches them.
```

`build_fib` and any globals-base load switch from `mov_r_imm64(r, real_addr)`
to `mov_r_imm64_external(r, AbsFixup::DispatchTableBase)`, with the
JIT driver performing the same address fill it does today. This is a
mechanical change localized to the call-site and globals-access
emission. Branches and rodata keep using the existing `rel32` /
`RipFixup` paths unchanged.

This is the **only** change to existing code the `.em` feature
requires, and it is additive (a new encoder method + a new fixup
vector; the existing `mov_r_imm64` with a real address still works
for genuinely-constant pointers if any exist).

### 2.5 Loader (`load_em_file`)

`src/em_loader.hpp/.cpp` exposes `bool load_em_file(const char* path,
LoadedModule& out, std::string* error, ModuleRegistry* registry)`. It stages
and validates the complete file before publication:

1. Read + validate header (magic, version).
2. Allocate the `DispatchTable` (slots array, `function_count`
   entries, zero-initialized).
3. Allocate the globals block (`global_size` bytes), `memcpy` the
   file's globals block into it.
4. For each function record:
   - Allocate one exec page of `code_size + rodata_size` (the
     `ExecArena` already does one allocation per fn, `main.cpp`'s
     `CompiledFn`).
   - `memcpy` code bytes, then rodata bytes right after.
   - Apply the relocs: for each `reloc` in the record, read `kind`:
     - `DispatchTableBase` → write `&dispatch_table` as imm64 at
       `code_offset`.
     - `GlobalsBase` → write `&globals_block` as imm64 at
       `code_offset`.
   - Make the page executable (`ExecArena::publish` already does
     `mmap`/`VirtualProtect` RX).
   - Stamp `dispatch_table.slots[slot_index] = page_base` (the
     function's entry, which is the page base since code is first).
5. Build the name→slot directory from the trailing name table.
6. On success move the staged dispatch/globals/name table/page ownership into
   `LoadedModule`; on failure leave the caller's object unchanged.

**No regalloc, no emit, no parser, no sema.** Load is header-read +
`memcpy` + two pointer writes per function. This is the design's
core payoff.

### 2.6 Host API + CLI

- Source compilation is assembled from the public lexer/parser/sema/codegen
  APIs (there is no shipped `ember_compile` facade).
- `load_em_file(path, LoadedModule&, error, registry)` is the implemented
  host loader; `link_em_file` additionally registers the loaded dispatch table.
  v1 `.em` is ABI/process trusted: it contains native code and may contain
  embedded process-local pointers. It is not promised portable across a new
  process/build. Symbolic native/trap/string/allowlist relocations plus build
  identity require a versioned format redesign (H12).
- **Reloading code originally loaded from `.em`:** `.em` carries no source.
  The host may compile a source replacement and use the existing
  single-function machinery only if it also retains the corresponding Program,
  signatures, codegen context, and safe retirement policy. There is no direct
  `.em` reload facade or whole-module reload API in v1.
- **CLI** (`examples/ember_cli.cpp`):
  - `ember_cli emit-em input.ember output.em`, or
    `ember_cli run input.ember --emit-em output.em`, runs the full pipeline,
    serializes initialized globals, and does not execute.
  - No `--load-em` action ships. Loading is via the host APIs above or a
    source `link "file.em" as alias;` directive. The latter exposes v1 exports
    as ABI-trusted unknown signatures because v1 serializes no signatures
    (H14); exact arity/types/return ABI are the host/caller's responsibility.

### 2.7 Versioning and forward-compat

- The `version` field is 1. A loader checks `magic` + `version` and
  rejects unknown versions outright (no guessing). Bumping the
  version on any format change is mandatory - a `.em` is a
  de-facto ABI, and silent misreads are worse than loud rejects.
- The `flags` field reserves bit 0 for "embeds source" (future
  reload-from-`.em`) and leaves the rest zero. Unused bits are not
  repurposed without a version bump.
- The reloc `kind` space is small (2 used, 1 reserved) and a new
  kind requires a version bump. This is fine - the format is meant
  to be regenerated by the toolchain, not hand-edited.

---

## Part 3 - Implementation order (gated on milestones)

The two features have different dependencies on the existing
milestone plan (`DESIGN.md` Section 9):

1. **`.em` serializer + loader** - **buildable now against v0.1.**
   The v0.1 codegen already produces everything the format needs
   (`CompiledFn` with raw bytes + `ExecArena`, the dispatch table,
   the globals block). The only prerequisite is the relocation-
   capture encoder change (Section 2.4), which is self-contained. This can
   ship as a v0.1.1 / v0.2 companion to the existing `ember_v01.exe`
   proof and *proves the pre-compile path end to end* before the
   parser even exists.
2. **`include` resolver + AST merge** - **blocked on v0.2's parser.**
   `include` is a parse-time directive that merges `Program`s; v0.1
   has no parser (hand-built IR, `COMPILER_PIPELINE.md` Section 12.1). The
   resolver is cheap once the parser exists (an `unordered_set` +
   a recursive parse-and-merge walk), so it lands with or just
   after v0.2.
3. **`import` / modules** - **future, `ROADMAP.md` Tier 6.** Design
   only (`docs/MODULES.md`); no grammar, no implementation. The
   re-entry trigger is a real runtime mod-loading use case.

**Recommended sequence:** `.em` first (unblocked, proves the format),
`include` with v0.2 (unblocks real multi-file scripts), `import`
whenever Tier 6's trigger fires.

---

## Part 4 - What this plan deliberately does *not* do

- **No bytecode interpreter.** Option C rejected outright (`DESIGN.md`
  non-goal, `GAP_ANALYSIS.md` Section 4).
- **No new execution path.** `.em` loads native x64; the runtime
  executes it exactly as it executes JIT'd code. One path.
- **No cross-module linking in v1.** `import` is spec'd, not built
  (Tier 6). Bundling (`include`) covers the "split a big script across
  files" need without runtime modules.
- **No embedded-source `.em` in v1.** Reload of a loaded `.em`
  requires separate source (Section 2.6). Embedding source is a `flags` bit
  reserved for later.
- **No include root / search path in v1.** `include` resolves relative
  to the current file only (Section 1.2). A host-configured include root is
  a v2 addition with no current use case.
- **No namespace/visibility on `include`.** All included names land in
  one flat module scope (`COMPILER_PIPELINE.md` Section 4). `pub`/`priv` is a
  language extension, not a bundling concern.

---

## Part 5 - Open questions to resolve at implementation time

1. **`@entry` discovery in `.em`.** The writer stores the annotated entry
   slot, falling back to `main`, or `0xFFFFFFFF` if neither exists. The v1 CLI
   emits this metadata but has no direct load/run action; hosts decide how to
   invoke a loaded slot.
2. **Concurrent reclamation of loaded pages (deferred).** `HOT_RELOAD.md`
   Section 5 ships caller-owned retirement only. A `.em`-loaded module that
   never reloads never retires pages. A concurrent host that reloads must keep
   old pages until it establishes host-level quiescence; no epoch scheme ships
   in v1. Verify the `module_t` carries the page list for both JIT'd and loaded
   modules.
3. **`mov_r_imm64_external` vs raw `mov_r_imm64` in v0.1 tests.** The
   five `Section 12` acceptance tests bake real addresses via raw
   `mov_r_imm64`. Switching to the external form means the JIT driver
   fills the placeholder - the tests' behavior is unchanged, but the
   encoder's byte output now has zero placeholders until the driver
   fills them. The byte-exact test #6 (`COMPILER_PIPELINE.md` Section 8)
   will need its expected bytes updated for the dispatch-table-base
   `mov r11, imm64` lines. This is a test-only change, not a
   behavior change.
