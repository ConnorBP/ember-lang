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
only, no bytecode interpreter fallback" (`planning/DESIGN.md` non-goal),
globals-block-by-absolute-address and string-literals-as-function-
local-rodata (`spec/MEMORY_AND_GC.md` Section 4/Section 6) - and re-uses the existing
pipeline rather than adding a second execution path.

---

## Part 1 - `include` (bundle mode) vs `import` (live mode)

### 1.1 The cut, and why it's the right one

> **Shipped keyword mapping.** This part frames the two modes as `include`
> (bundle) vs `import` (live). What shipped uses different keywords for the
> same two subsystems: the **bundle/textual** mode shipped as **`import "path";`**
> (textual inclusion before lexing, resolved by `src/import.{hpp,cpp}` — see
> §1.2's shipped-status note), and the **live** mode shipped as the **`link "mod.em" as m;`**
> directive + `ModuleRegistry` (see §1.5). So where this part says `include`, read
> the shipped `import "path";`; where it says `import` (live), read the shipped
> `link` directive. The two-subsystem split itself is unchanged.

The two modes are not two flavors of one mechanism. They are two
*different* subsystems, and bundling is the cheap one.

| | `include "x.ember"` (bundle) | `import "x.ember"` (live) |
|---|---|---|
| When it happens | parse time, before sema | load time, after each unit compiles |
| Output | **one** module: one dispatch table, one globals block, one slot assignment | **N** modules: one dispatch table + globals block each |
| Cross-unit call | ordinary `CallScript` through the single shared dispatch table (same as any intra-file call today) | new cross-module indirection: module registry → foreign `DispatchTable` → slot |
| New runtime machinery | **none**  -  merged `Program` flows through the existing pipeline | module registry, cross-module call IR shape, linker stage |
| Dedup | parse-time include-graph walk, set of canonicalized paths | not needed (separate units, separately compiled) |
| Matches existing | exactly the current single-file compile, just fed a merged AST | `ROADMAP.md` Tier 6 "Modules / `import`" (the live `link` surface shipped since — see §1.5 + `MODULES.md`) |

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
`MODULES.md` (originally as future work; the live `link` directive +
`ModuleRegistry` + cross-module call subsequently **shipped** — see §1.5
below + `MODULES.md`). The two
keywords are named to make the semantics obvious and to match the
user's own phrasing ("one of them forcing non bundle so it imports it
live"): `include` is the C `#include` analog (merged before
compilation, one output), `import` is the Rust/Python `import` analog
(separate unit, linked at load).

### 1.2 `include` - design

> **Shipped status (supersedes the `include`/`include_resolver` design below).**
> The textual-inclusion feature shipped as the **`import "path";`** directive
> resolved by **`src/import.{hpp,cpp}`** (`resolve_imports`), NOT as an
> `include` keyword and NOT via a `src/include_resolver.hpp/.cpp` file (that
> file does not exist). The semantics below (parse-time textual inclusion,
> canonical-path dedup via a `seen` set, cycle termination for free, flat
> one-module merge) are exactly what `src/import.cpp` implements — only the
> keyword (`import`, not `include`) and the source file (`import.cpp`, not
> `include_resolver.cpp`) differ from this design sketch. The CLI uses
> `resolve_imports` before lexing (see `README.md` "`import "path";` is
> textual inclusion before lexing"). Accordingly, every `include "path"` /
> `src/include_resolver.*` reference in the remainder of this section is the
> pre-implementation design naming; read it as `import "path"` /
> `src/import.{hpp,cpp}`. (Part 3 item 2 records the shipped status.)

**Grammar** (added to `spec/COMPILER_PIPELINE.md` Section 2, top-level):

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

**Driver include resolver** (shipped as `src/import.{hpp,cpp}`, `resolve_imports`; the design below named it `src/include_resolver.hpp/.cpp` — see the shipped-status note above):

- `struct IncludeResolver { std::unordered_set<std::string> seen; }`
  keyed by **canonicalized absolute path** (case-correct, symlinks
  resolved via `std::filesystem::canonical`/`weakly_canonical`).
- On `include "path"` (shipped as `import "path";`):
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

**`SourceLoc.file`**: the `SourceLoc` struct (`spec/COMPILER_PIPELINE.md`
Section 7) already carries `const char* file`. The resolver must ensure each
parsed file's tokens carry that file's own name so diagnostics point
at the right file. This is a property of the lexer being told which
file it's lexing, not new machinery - just "don't share one filename
across the whole merge."

### 1.3 What `include` does *not* need

- No namespace / module-name concept. All included names land in one
  flat module scope (matches `spec/COMPILER_PIPELINE.md` Section 4's existing
  flat module scope; namespaces are `ROADMAP.md` Tier 6).
- No visibility / `pub` / `export` on textual `import` - everything included
  is visible (matches C `#include`; a private-include model is a language
  extension with no v1 use case). **This is specifically about textual
  `import` (which inlines to one flat module scope, like C `#include`). The
  BUNDLED `.em`/`link` surface DOES now have pub/priv visibility — see Section 4
  + `MODULES.md` §6 (F1, implemented 2026-07-10): `pub fn`/bare `fn` are
  exported; `priv fn` is hidden from the `.em` name directory and a cross-module
  `mod::priv_fn()` call is a sema error. In-module name scoping (namespaces,
  `ROADMAP.md` Tier 6) remains a language concern; the exported surface of a
  bundled module is a bundling/linking concern — the two are siblings.**
- No include guards / `#pragma once` - the `seen` set is the guard.
- No conditional includes - `include` is unconditional; conditional
  composition is `engine.define` + `const`/`constexpr` territory
  (`planning/DESIGN.md` Section 8, `ROADMAP.md` hard non-goal "no preprocessor").

### 1.4 Interaction with hot reload

The shipped `reload_function` helper takes one complete replacement function
and recompiles it by name. With textual inclusion, the function belongs to the
merged `Program`, but reload remains per-function:

- **Single-function reload** (`reload_function`): **changed in v1.0**
  (breaking source-API bump). The helper now takes a `HotReloadDomain&` as its
  fourth argument and `ReloadResult` no longer returns a caller-owned
  `old_entry` pointer; the domain owns the replaced page from publication and
  frees it after a guarded quiescent point. See `HOT_RELOAD.md` Section 0
  for the migration recipe (persistent domain beside the table; guard before
  every outer call; disown the old `CompiledFn`; `reclaim()`/`quiesce()`; drain
  guards then quiesce before freeing current pages). The host still hands the
  new body of *one* function; includes don't participate (a function's body
  doesn't contain includes - includes are top-level only, Section 1.2). The host
  is responsible for re-resolving includes itself if the reloaded function now
  calls a symbol that came from an included file - but that symbol is already in
  the module's slot table from the initial compile, so the reload just
  resolves the call against the existing slots. **No include
  machinery runs on reload.**
- **Whole-module reload:** deferred. A future implementation would need to
  re-resolve the root and apply a transactional batch; v1 does not claim this.

### 1.5 `import` (live) - design, now shipped

For completeness, the Tier 6 design lives in `MODULES.md`
(written since this section was first sketched). The load-bearing
design point, recorded here so it isn't lost:

- A **module registry**: per-process map `module_id → DispatchTable*`.
  Loaded `.em` files (Part 2) and JIT-compiled modules both register.
- A cross-module `CallScript` IR op carries `(module_id, slot_index)`;
  codegen emits one extra indirection vs intra-module calls: load
  registry base → load `registry[module_id].slots` → load
  `slots[slot_index]` → `call r11`. That is one more `mov` than the
  intra-module form (`spec/CODEGEN_SPEC.md` Section 7), the cost of true runtime
  linking.
- **Slot stability still holds within each module**; cross-module
  calls go through the registry, so a foreign module's slot table can
  be reloaded independently (its slots are stable *for its own
  lifetime*; a foreign caller cached a `(module_id, slot_index)` pair,
  and `module_id` resolves to the current `DispatchTable*` of that
  module, so a reload of the foreign module is transparent to the
  caller - same property intra-module reload already has, lifted one
  indirection up).

This **shipped** as the live `link` directive + `ModuleRegistry` +
cross-module call path (`src/module_registry.{hpp,cpp}` +
`src/module_linker.hpp` + `em_loader.cpp`'s `link_em_file` + the
`link "file.em" as alias;` grammar + `CallCrossModule` / `emit_cross_module_call`).
See `MODULES.md` (the design reference) + Part 3 item 3. What remains
genuinely future is the re-entry-trigger-gated work (`MODULES.md` §6:
whole-module reload, late `relink_imports`, the removed-function trap stub).
The re-entry trigger for *those* is a real runtime mod-loading use case
(one mod loads another's pre-compiled `.em` at runtime) beyond what the
shipped `link` surface covers.

---

## Part 2 - `.em` pre-compile (Option B: serialized native code)

### 2.1 Weighing the options

The user asked to weigh "dense intermediary" vs "final bytecode"
for the `.em` format. There are three real options:

**Option A - serialized IR (dense intermediary).** Serialize the
`IrFunction` (blocks, instrs, vreg types, params) + dispatch slot
table + globals layout. The loader re-runs `run_linear_scan` +
`emit_x64` (`spec/COMPILER_PIPELINE.md` Section 8) to produce machine code at load
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
  The `planning/DESIGN.md` non-goal is "no bytecode interpreter fallback,"
  meaning *one* execution path, and that path is native. Serialized
  native code is not a second path; it's the same path with the
  regalloc+emit step moved earlier. This is the MCJIT-vs-ORC
  distinction: pre-compiled object code with relocations is not
  "bytecode." Fully consistent with the spec's stance.
- *Pro:* the file is target-specific (Win64 x86-64), but ember is
  Windows-first x86-64-only (`spec/CODEGEN_SPEC.md` Section 1), so cross-platform
  `.em` is not a real concern. Option A's only theoretical advantage
  (portable IR) doesn't apply.
- *Con:* larger file than IR (raw x64 is less dense). Acceptable  - 
  `.em` files ship with a game, not downloaded per-call.
- *Con:* every process-dependent absolute immediate needs explicit metadata.
  The implemented format records dispatch, globals, module-registry, and
  function-rodata base relocations plus separate symbolic native binding
  slots. The emitter captures these while generating instructions; neither
  writer nor loader scans x64 bytes.

**Option C - a bytecode interpreter.** Explicitly rejected. This is
the `planning/DESIGN.md` non-goal: "JIT only, no bytecode interpreter fallback  - 
one execution path." A bytecode `.em` would create a second execution
path (the interpreter) alongside the JIT, which is exactly what the
spec refuses. It would also reintroduce the "5-50× interpreter
overhead" `planning/GAP_ANALYSIS.md` Section 4 deliberately avoids. Not considered
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
  version         : u32   = 2             (loader also accepts historical v1)
  flags           : u32   = 0            (bit 0 reserved: "contains source"
                                           for future embed-source reload)
  function_count  : u32
  global_size     : u32                  (bytes of the globals block)
  rodata_total    : u32                  (bytes of all per-function rodata,
                                           sum of per-fn rodata_size)
  entry_slot      : u32                  (slot of @entry fn, or 0xFFFFFFFF)
  v2_build_id     : u64                  (stable compiler/format identity)
  v2_target_abi   : u32                  (target/calling-convention hash)

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
    v2 reloc:
      offset      : u32                   (byte offset within `code` of imm64)
      kind        : u8
        0 = dispatch_table_base
        1 = globals_base
        2 = module_registry_base
        3 = function_rodata_base          (loaded code end + addend)
      addend      : u32                   (nonzero only for kind 3)
    v1 reloc:
      offset      : u32
      kind        : u8                    (kinds 0..2 only; no addend)
  export_signature (v2):
    return_type   : canonical Type
    param_count   : u32
    params        : canonical Type[param_count]
  native_binding_count : u32              (v2)
  native_bindings:
    code_offset   : u32                   (8-byte zeroed imm64 slot)
    name_len/name : u16 + UTF-8 bytes     (exact host allowlist key)
    signature     : return Type + ordered parameter Types

Canonical Type encoding (recursive, based directly on `ast.hpp::Type`):
  prim            : u8                    (`Prim` enum ordinal)
  flags           : u8                    (slice, array, struct, fn handle,
                                           recorded fn signature)
  array_len       : u32
  struct_name     : u16 + UTF-8 bytes
  elem            : canonical Type        (slice/fixed array only)
  recorded fn signature                    (typed function handles only)

Globals block:
  bytes[global_size]                     (initial values, copied verbatim
                                          into the allocated globals block)

Function-name directory (for ember_call by name, HOT_RELOAD.md Section 7):
  name_table_count : u32
  entries: [ { name_len: u16, name: bytes, slot_index: u32 } ... ]
```

**Why rodata is per-function, not a shared section.** String bytes are copied
from sema-owned `Program::rodata_store` into `CompiledFn::rodata`. Codegen emits
an explicit `FunctionRodataBase` imm64 relocation with the byte offset as its
addend; the loader patches it to `loaded_page + code_size + addend`. Thus no
`StringLit::baked_ptr` survives serialization. Keeping rodata next to its code
also gives one allocation and one W^X transition per function.

**Why no cross-function `rel32` reloc.** `spec/CODEGEN_SPEC.md` Section 4
explicitly forbids cross-function `rel32` references - script-to-
script calls go through the dispatch table, never same-image direct
calls. Branch labels are resolved before serialization; absolute external
bases, function-rodata addresses, and native targets use the explicit v2
records above. The serialized bytes are otherwise position-independent.

### 2.3 Serializer (compile-time, after `emit_x64`)

New `src/em_writer.hpp/.cpp`. Runs after the full pipeline produces
the `module_t` (or the v0.1 equivalent: the list of `CompiledFn` +
the dispatch table + the globals block). For each function:

1. Take the final `code` vector (post-`resolve_fixups`) and its
   trailing `rodata` blob.
2. Emit the per-function record: name, `slot_index`, `code_size`,
   `rodata_size`, the code bytes, the rodata bytes.
3. Emit every explicit base/rodata relocation (including v2 addends), the
   canonical signature copied from the real `FuncDecl`, and each symbolic
   native binding copied from sema/codegen metadata. Relocation/native imm64
   bytes are zeroed in the file; the writer never scans machine bytes.
4. Reject exact generated functions that contain unsupported process state
   (trap stub/context/detail, baked budget/depth storage, or function-reference
   allowlist storage), with the codegen feature-specific diagnostic.
5. After all functions: the already-evaluated globals bytes and name→slot
   directory.

The serializer needs the slot table (name→index) and the globals
block address/size, both of which the compile pipeline already
produces. No new compile-time state.

### 2.4 Relocation and native-binding capture

`X64Emitter::mov_reg_imm64_external` emits an eight-byte placeholder and an
`AbsFixup`; `mov_reg_native` emits a separate symbolic native placeholder.
Codegen preserves the exact name selected by sema, including
`OpOverload::fn_name`, so aliases and overload-only targets never require
ambiguous pointer reverse lookup.

The base relocation shape is:

```cpp
struct AbsFixup {
    uint32_t code_offset = 0;   // offset of the imm64 within `code`
    enum Kind : uint8_t {
        DispatchTableBase = 0,
        GlobalsBase = 1,
        ModuleRegistryBase = 2,
        FunctionRodataBase = 3,
    } kind;
    uint32_t addend = 0;
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

Raw `mov_reg_imm64` remains for genuine numeric constants. Process pointers
must use one of these explicit records or make the generated function
non-serializable.

### 2.5 Loader (`load_em_file`)

`src/em_loader.hpp/.cpp` exposes `load_em_file(path, out, error, registry,
native_allowlist)`. The final allowlist argument is additive/default-null for
source compatibility. It stages
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
   - Apply dispatch/globals/registry/function-rodata relocations.
   - Resolve every v2 native name only through the supplied
     `unordered_map<string, NativeSig>` and require canonical signature equality;
     missing names, null targets, and mismatches are load errors.
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

### 2.5.2 Version 5 — IR `.em` (Stage B, IL-`.em`)

**Status: shipped (Stage A + Stage B, 2026-07-10).** The thin three-address IR
backend (Stage A, `COMPILER_PIPELINE.md` §5's Stage-A note + `CODEGEN_SPEC.md`
§5's Stage-A note) produces a serializable `ThinFunction` per script function.
Stage B ships the v5 `.em` format that carries that IR on disk instead of raw
x86. The on-disk contract is `src/em_file.hpp` (the `EM_VERSION_V5` constant +
the v5 per-function record + the SECURITY MODEL block) and `src/thin_ir_ser.hpp`
(the `ir_blob` codec — `serialize_thin_function` / `deserialize_thin_function` /
`validate_thin_function`).

**What v5 changes (additive, per-function only).** The 40-byte header, the
globals block, and the name directory (export table) are **byte-identical** to
v3/v4; only the per-function record is redesigned. Each per-function record
now begins with an **`is_ir` byte** that selects between two body shapes:

```
v5 per-function record (header version=5):
  name_len        : u16
  name            : bytes[name_len]   (UTF-8, no NUL)
  slot_index      : u32
  is_ir           : u8   (1 = IR function; 0 = raw-x86 fallback)
  signature       : canonical export signature (SAME encoding as v2+:
                    emit_type(ret) + u32 param_count + emit_type per param)
  if is_ir == 1:  // IR function — carries IR, NOT machine code
    ir_blob_len   : u32
    ir_blob       : bytes[ir_blob_len]  (OPAQUE to the .em container =
                    serialize_thin_function output; parsed ONLY by the IR
                    deserializer, never by the .em loader). NO
                    code/rodata/relocs/native_bindings fields follow.
  if is_ir == 0:  // raw-x86 fallback (byte-identical to the v4 per-fn body
                  // AFTER the is_ir byte; the signature was hoisted above
                  // the branch in v5):
    code_size     : u32, rodata_size : u32, code : bytes, rodata : bytes,
    reloc_count   : u32, relocs[reloc_count] { offset:u32, kind:u8, addend:u32 },
    native_binding_count : u32, native_bindings (SAME as v2+)
```

A v5 module may **mix** IR and raw-x86 functions per-function ("mixed mode"):
IR-serializable functions ship IR; non-serializable functions — aggregate,
string, struct, or defer gaps flagged by `ThinFunction::non_serializable` —
ship raw x86. On disk the `is_ir` byte is derived as `!ir_blob.empty()`. The
additive design means a v4 reader that ignored trailing bytes would reject a
v5 file as corrupt (the `is_ir` byte and the hoisted signature shift the
record) rather than misread it — no silent misparse.

**The `ir_blob` record.** The blob is **opaque to the `.em` container**: `em_writer.cpp`
writes it as raw bytes (`ir_blob_len` + `ir_blob`) and `em_loader.cpp` reads it
as raw bytes, then hands it to the IR deserializer. The `.em` layer never parses
inside the blob. The blob's own format (little-endian, self-contained,
counts-up-front) is pinned in `src/thin_ir_ser.hpp`:

```
ir_blob format:
  Header:
    ir_magic     : u32 = 0x4952464E ("IRFN") — reject garbage immediately
    ir_version   : u16 = 1           — IR serialization format version
    slot         : i32               — dispatch slot
    max_vreg     : u32               — highest VReg+1; all VReg refs < this
    num_blocks   : u16 (<= 65535)
    has_ret_type : u8                — 0=nullptr, 1=type follows
    [ret_type    : canonical type via emit_type/parse_type if has_ret_type]
  Frame plan:
    frame_size, rbx_save_offset, struct_ret_ptr_offset,
      arg_temps_base, next_local_off  : 5×i32
    returns_struct_by_ptr : u8
    num_params            : u16 (<= 1024)
    per param: name_len:u16, name, has_type:u8, [type], off:i32, word0:i32, nwords:i32
    num_native_fixup_names : u16 (<= 1024)
    per name: name_len:u16, name
  Rodata:
    rodata_len  : u32 (checked <= remaining blob)
    rodata      : bytes[rodata_len]
  Blocks (repeated num_blocks times):
    block_id    : u32
    num_instrs  : u16 (<= 65535)
    per instr:
      op        : u16 (validated against the ThinOp enum range — STABLE,
                  serialized VERBATIM; new ops append at the END only)
      dst,src1,src2 : 3×u32 (< max_vreg or 0)
      imm_i     : i64
      imm_f     : f64 (raw 8 bytes)
      meta: frame_off/width/len/slot/mod_id/field_off : 6×i32,
            base_kind:u8 (validated against AbsFixup::Kind range),
            addend:u32, native_name_len:u16 + native_name,
            has_type:u8 + [type], cmp/is_unsigned/is_f32/trap_reason : 4×u8
      num_args  : u8 (<= 255), args : u32[num_args]
      num_arg_frame_offs : u8, arg_frame_offs : i32[num_arg_frame_offs]
      num_arg_types      : u8, arg_types      : per has_type:u8 + [type]
      has_ret_type       : u8, [ret_type]
      loc_line : u32, loc_col : u32
    terminator:
      term_kind : u8 (validated against TermKind enum range)
      cond, target, false_target, ret : 4×u32
      trap_reason : u8
```

The serialization boundary (`thin_ir.hpp` pins this): `ThinOp` is a stable
`uint16_t` enum serialized verbatim. The only raw-pointer fields in the IR are
**not** serialized as pointers — `ThinInstr::native_fn` is dropped (the symbolic
binding is carried by `meta.native_name`; the loader rebinds `native_fn` at load
time by looking up the name in the host native table, never by reverse-mapping a
pointer), and every `const Type*` field is encoded via `em_type_codec`'s
`emit_type`/`parse_type` (canonical `Prim` + `struct_name` + `is_slice` +
`array_len` + `elem` chain), each prefixed by a `has_type:u8`. `abs_fixups` is
**not** serialized — it is populated by `emit_x64` at load time (the re-emit
produces the same `AbsFixup` list the tree-walker would). `rodata` IS serialized
(function-local string-literal bytes).

**Re-emit-at-load security model.** This is the core v5 security property and
the reason v5 exists. A v5 `.em` carries **IR, not machine code**. The load
path for an `is_ir == 1` function is:

1. **Deserialize** the `ir_blob` via `deserialize_thin_function` (structural
   validation only: magic `"IRFN"`, `ir_version`, count bounds, cursor bounds,
   `ThinOp` range, `TermKind` range, `base_kind` range, canonical-type shape).
   Every count is checked against a hard maximum **before** the corresponding
   resize/reserve; all cursor arithmetic uses `uint64_t` for `avail = end - cur`
   with `n > avail` (no `uint32_t` addition that can wrap); no exceptions escape
   the deserializer — it returns `false` + error on any structural failure.
2. **Rebind natives by name.** The loader looks up each `meta.native_name` in
   the host native table and sets `native_fn`. An unknown name is a **load
   error** (the core v5 security gate — a tampered blob naming a native the
   host did not register is rejected here, before any x64 exists).
3. **Semantically validate** via `validate_thin_function` (block-id bounds,
   block 0 is entry, every block has a terminator, block-target bounds,
   terminator shape consistency, VReg bounds, rodata bounds, `CallScript` slot
   < dispatch size, `CallCrossModule` mod_id < registry size, cmp predicate in
   `[0,5]`, `CallNative` has a non-empty name, frame-plan sanity).
4. **Re-emit x64** via `emit_x64` (`src/thin_emit.hpp`) — the loader IS a
   codegen for the IR path, producing the same x64 the tree-walker would.
5. **Only then** `alloc_executable_rw` + patch relocations (the `abs_fixups`
   `emit_x64` just produced) + `seal_executable` (RW→RX W^X).

The load-bearing invariant: **deserialize → validate → re-emit happens BEFORE
`alloc_executable_rw`**, so a tampered or malformed v5 `.em` is **rejected at IR
validation with no executable page allocated**. This is the defense-in-depth the
F2 audit's "secondary" option named: a malicious input must survive structural +
semantic validation before any x64 is emitted, and the emitted x64 is produced
by the host's own `emit_x64` from validated IR (not `memcpy`'d from the file).
The reduction is "malicious x64 bytes → malicious IR that must survive type /
structure validation before becoming x64," not "malicious input → no native
execution" — a valid-but-malicious IR that passes validation still lowers to x64
that runs with host privileges. The raw-x86 fallback (`is_ir == 0`) is the v4
per-function body and does NOT get this re-emit protection.

**Signing and v5.** v5 is **unsigned** for Stage B (no Ed25519 signature block)
— the v3 "trailing bytes == 0" rule still holds. A v5 module is not routed
through the v4 Ed25519 verification path (the loader checks `version == 4` for
the signed path; v5 falls into the unsigned branch). In signed-only mode
(non-empty keyring) the entire v5 module is rejected; in dev mode the raw-x86
fallback is accepted without signature verification (same as v3). A **v5-signed
variant** (v5 layout + the additive Ed25519 block) is explicitly **future work**
that would give the raw-x86 fallback v4-level content authentication; do not
implement it in Stage B. `EM_VERSION` stays 4 (the default writer version); v5
is off-by-default and inert until a v5 writer/loader code path is selected.

### 2.6 Host API + CLI

- Source compilation is assembled from the public lexer/parser/sema/codegen
  APIs (there is no shipped `ember_compile` facade).
### 2.5.1 Format v2 portability and v1 compatibility — and v4 content authentication (F2)

The writer emits version 4 (signed) via `write_em_file_signed`, or version 3
(unsigned) via `write_em_file`. The loader explicitly accepts versions 1, 2, 3,
and 4. Version 1 retains its historical ABI-trusted `unknown_sig` behavior.
Version 2 rejects compiler/build or target-ABI identity mismatches before
executable allocation, stores canonical `Type`-based export signatures, and
resolves each native immediate by symbolic name and signature through a
host-supplied `unordered_map<string, NativeSig>` allowlist. Version 3 (F1,
docs/spec/SPEC_AUDIT_2026-07-10.md F1) keeps the v2 per-function record layout
byte-identical and repurposes the name directory as the module's EXPORT TABLE
(only `pub fn`/bare-`fn` entries). Function string bytes live in the function
rodata payload and use an explicit code-plus-rodata relocation; no
`Program::rodata_store` pointer is serialized. Generated trap-stub/context/detail
and function-reference allowlist state has no portable binding yet, so the
writer rejects those exact functions with a feature-specific error rather than
baking a process pointer.

**Version 4 — signed raw-x86 `.em` (F2, docs/spec/SPEC_AUDIT_2026-07-10.md F2).**
v1/v2/v3 carry a build/abi IDENTITY hash (FNV1a of compiler/ABI string literals)
+ a TYPE signature (sema arg-checking) — NOT content authentication. A malicious
`.em` from the same compiler/ABI with valid identity+type-sigs still injected
arbitrary x86 (the raw-x86 code-injection risk the audit names). v4 closes that
gap: the v3 layout byte-identical + an additive Ed25519 signature block (104
bytes: `sig_magic` "EMSG" | `payload_len` | `pubkey_id`[32] | `signature`[64]).
The signed payload is the v3 content bytes (header → name directory); the
loader cross-checks `payload_len` == end of name dir, then **verifies the Ed25519
signature over the content BEFORE `alloc_executable_rw`** and rejects on mismatch
(no exec page published — a tampered `.em` is rejected, not executed). The crypto
is a vendored orlp/ed25519 (public domain, `thirdparty/ed25519/`); standard
PureEd25519 (RFC 8032; the scheme's internal hash is SHA-512 — the audit's "over
SHA-256" is read as "cryptographic content authentication via Ed25519 over the
.em content", not a separate SHA-256 prehash). The build_id/abi_hash stay the
COMPATIBILITY check; the v4 signature is the CONTENT authentication the identity
hash is NOT.

**Key management — secure-boot-style opt-in (the honest minimal v1).** The signing
key stays OFF the host (the build tool that emits `.em` signs it); the host gets
only verification public keys. `load_em_file` takes an `EmVerifyPolicy{ vector<array<uint8_t,32>> trusted_keys; }`:
- **empty keyring (or null) = DEV MODE** — the loader accepts unsigned v1/v2/v3
  modules (the development convenience the audit names) and rejects a v4 module
  with a clear error ("v4 module requires a verification key; host provided none"
  — a v4 module IS signed, so running it unverified is worse than honest
  unsigned dev code). The existing .em round-trip tests + demos use this path.
- **non-empty keyring = SIGNED-ONLY** — the loader rejects unsigned v1/v2/v3
  modules ("host mandates signed modules") and accepts a v4 module ONLY if its
  signature verifies against one of the trusted keys. A v4 module whose
  `pubkey_id` is not in the keyring is rejected with "signed by an untrusted key"
  (so a host can tell "wrong keychain" from "tampered content").
The verification policy is a **host/library API**, not a CLI flag: the
standalone `ember` CLI (`examples/ember_cli.cpp`) exposes no `--verify-em-key`
option and loads `.em` modules in dev mode (empty keyring). A host that wants
signed-only mode constructs an `EmVerifyPolicy` with its trusted pubkeys and
passes it to `load_em_file` / `link_em_file` (the `verify` parameter, below) —
this is the embedding-side seam, mirroring secure-boot: keys present ==
signed-only; keys absent (null `verify`) == unsigned dev OK.

- `load_em_file(path, LoadedModule&, error, registry, native_allowlist, verify)`
  is the implemented host loader (the `verify` policy is additive, default null =
  dev mode); `link_em_file` additionally registers the loaded dispatch table and
  threads the same `verify` policy. v1 remains ABI/process trusted and exposes
  unknown export signatures. v2/v3 verify stable build/target identities before
  executable allocation and use explicit symbolic native and function-rodata
  bindings. v4 verifies the Ed25519 signature before `alloc_executable_rw`.
- **Reloading code originally loaded from `.em`:** `.em` carries no source.
  The host may compile a source replacement and use the existing
  single-function machinery only if it also retains the corresponding Program,
  signatures, codegen context, and safe retirement policy. There is no direct
  `.em` reload facade or whole-module reload API in v1.
- **CLI** (`examples/ember_cli.cpp`):
  - `ember_cli emit-em input.ember output.em`, or
    `ember_cli run input.ember --emit-em output.em`, runs the full pipeline,
    serializes initialized globals, and does not execute.
  - `ember_cli run --load-em file.em [--fn name]` registers the same standard
    native and overload-symbol allowlist, loads in a fresh process, and invokes
    the selected export. Source `link "file.em" as alias;` uses the same loader.
    v2 exports are type-checked from slot-indexed canonical signatures; only v1
    exports remain ABI-trusted unknown.

### 2.7 Versioning and forward-compat

- The writer emits version 4 (signed, via `write_em_file_signed`) or version 3
  (unsigned, via `write_em_file`). The loader accepts exactly v1, v2, v3, v4,
  and v5 and rejects other versions outright (no guessing). Bumping the
  version on any format change is mandatory - a `.em` is a
  de-facto ABI, and silent misreads are worse than loud rejects.
- v4 (F2, docs/spec/SPEC_AUDIT_2026-07-10.md F2) is v3 layout + an additive
  Ed25519 signature block after the name directory; v1/v2/v3 have no signature
  block (their "trailing bytes == 0" check is unchanged). The signature block
  is self-describing (a `sig_magic` "EMSG" sentinel + a `payload_len` the loader
  cross-checks against the name-directory end), so a v3 reader that ignored
  trailing bytes would still reject a v4 file as corrupt rather than misread it.
- v5 (Stage B, IL-`.em`, §2.5.2 above) redesigns only the per-function record
  (the `is_ir` byte + the hoisted signature + the `ir_blob` / raw-x86 branch);
  the 40-byte header, globals block, and name directory are byte-identical to
  v3/v4. v5 is unsigned for Stage B (no Ed25519 block; the "trailing bytes == 0"
  rule holds). The `is_ir` byte + the hoisted signature shift the per-fn record,
  so a v4 reader that ignored trailing bytes would reject a v5 file as corrupt
  rather than misread it. See `src/em_file.hpp` (`EM_VERSION_V5` + the v5
  per-function record + the SECURITY MODEL block) and `src/thin_ir_ser.hpp`
  (the `ir_blob` codec).
- The `flags` field reserves bit 0 for "embeds source" (future
  reload-from-`.em`) and leaves the rest zero. Unused bits are not
  repurposed without a version bump.
- v2 relocation kinds 0..3 and their addend semantics are pinned above;
  adding a kind or changing a record requires a version bump. The format is meant
  to be regenerated by the toolchain, not hand-edited.

---

## Part 3 - Implemented status

1. **`.em` v2 serializer + loader:** implemented by `em_writer` / `em_loader`,
   including v1 compatibility, W^X staging, identities, signatures, symbolic
   natives, rodata, globals, and hardened limits.
2. **Textual imports:** implemented by `src/import.cpp` and used by the CLI.
3. **Live modules:** implemented by `ModuleRegistry`, kind-2 relocations,
   `module_linker.hpp`, and the `link` grammar. JIT and loaded modules share the
   same export and dispatch model.
4. **v5 IR `.em` (Stage B, IL-`.em`):** the thin three-address IR backend
   (Stage A, `src/thin_ir.{hpp,cpp}` / `src/thin_lower.{hpp,cpp}` /
   `src/thin_emit.{hpp,cpp}`) produces a serializable `ThinFunction`; the v5
   `.em` format (`src/em_file.hpp` `EM_VERSION_V5` + the v5 per-function record)
   carries that IR on disk via the `is_ir` byte + the `ir_blob` record; the
   `ir_blob` codec is `src/thin_ir_ser.{hpp,cpp}`
   (`serialize_thin_function` / `deserialize_thin_function` /
   `validate_thin_function`). The loader deserializes + validates + re-emits the
   IR to x64 via `emit_x64` BEFORE `alloc_executable_rw` (§2.5.2, the re-emit-
   at-load security model). `EM_VERSION` stays 4 (the default writer version);
   v5 is the Stage-B contract.

---

## Part 4 - What this plan deliberately does *not* do

- **No bytecode interpreter.** Option C rejected outright (`planning/DESIGN.md`
  non-goal, `planning/GAP_ANALYSIS.md` Section 4).
- **No new execution path.** `.em` loads native x64; the runtime
  executes it exactly as it executes JIT'd code. One path.
- **No source embedded in `.em` v2.** Reload from source still requires the
  source to be supplied separately; header flag bit 0 remains reserved.
- **No host-configured textual-import search path.** Imports resolve relative
  to the current file.
- **No namespace/visibility on `include`.** All included names land in
  one flat module scope (`spec/COMPILER_PIPELINE.md` Section 4). ~~`pub`/`priv` is a
  language extension, not a bundling concern.~~ **CORRECTED (F1, implemented
  2026-07-10):** textual `include` remains flat (no visibility, like C
  `#include`) — that IS a language/scope concern. But the BUNDLED `.em`/`link`
  module surface now HAS pub/priv visibility as a BUNDLING concern: a `pub fn` /
  bare `fn` is published to the `.em` name directory (the export table) and is
  callable cross-module; a `priv fn` is serialized (its code occupies a dispatch
  slot for intra-module calls) but is absent from the name directory, so other
  modules cannot resolve it and a `mod::priv_fn()` call is a sema error. The `.em`
  format is v3 (v1/v2 still load — backward compat). In-module name scoping
  (namespaces) stays a language concern; the exported surface of a bundled
  module is a bundling concern. See `spec/SPEC_AUDIT_2026-07-10.md` F1 +
  `MODULES.md` §6 + `pub_priv_test` (ctest).

---

## Part 5 - Open questions to resolve at implementation time

1. **`@entry` discovery in `.em`.** The CLI writer stores the annotated entry
   slot, falling back to `main`, or `0xFFFFFFFF` if neither exists. The load/run
   action accepts `--fn` and otherwise uses the stored entry.
2. **Reloading `.em`-loaded functions (deferred).** `HOT_RELOAD.md` Section 5
   ships guarded epoch reclamation for pages replaced through the source-level
   `reload_function` path. A `.em`-loaded module that never reloads never
   retires pages, but replacing loaded functions is not yet exposed as a
   dedicated loader transaction. Verify a future loaded-module reload path
   transfers its page records into the same domain without double ownership.
3. **`mov_r_imm64_external` vs raw `mov_r_imm64` in v0.1 tests.** The
   five `Section 12` acceptance tests bake real addresses via raw
   `mov_r_imm64`. Switching to the external form means the JIT driver
   fills the placeholder - the tests' behavior is unchanged, but the
   encoder's byte output now has zero placeholders until the driver
   fills them. The byte-exact test #6 (`spec/COMPILER_PIPELINE.md` Section 8)
   will need its expected bytes updated for the dispatch-table-base
   `mov r11, imm64` lines. This is a test-only change, not a
   behavior change.
