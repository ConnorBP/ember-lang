# Final Ember Docs Accuracy Audit — 2026-07-11

**Scope:** READ-ONLY audit of every ember doc (`docs/**`, `README.md`,
`extensions/README.md`) against the current source at `ember/`. Stale/inaccurate
claims were fixed in-place; this report records what was checked, what was
wrong, and what was fixed. Source code (`src/`, `examples/`) was NOT modified by
this audit (only docs).

**Method:** each doc's feature claims, test counts, code examples, and
cross-references were checked against the current source (`src/*.cpp/.hpp`,
`extensions/*/ext_*.cpp`, `CMakeLists.txt`, `tests/lang/`, `examples/`,
`self_hosted/`) and the live `ctest -N` / `ember_cli test tests/lang` runs.

## Ground truth (verified from source this session)

- **ctest targets:** **54** total `add_test` entries in `CMakeLists.txt`. Two
  (`bench_ember_vs_as`, `bench_codegen_paths`) are behind `if(EXISTS ...)` guards
  that are **both true** (the AngelScript SDK + `bench/bench_codegen_paths.cpp`
  both exist), so all 54 register. **52 excluding the two benchmarks.**
  `ctest -N` reports `Total Tests: 54`. (A no-AngelScript-SDK build would
  configure 53.)
- **lang suite:** **249/249 passed** via `ember_cli test tests/lang`
  (81 `valid_*`, 113 `invalid_*`/`sema_invalid_*`, 26 `sema_valid_*`,
  20 `runtime_*`).
- **IR optimization passes:** **8** opt passes (constprop/dce/cse/licm/forward/
  copyprop/instcombine/dse) + 1 obf pass (subst) — confirmed in
  `extensions/opt/ext_opt.cpp` `register_passes` + `extensions/obf/ext_obf.cpp`.
- **regalloc:** a linear-scan register allocator over the thin IR IS shipped
  (`src/regalloc.{hpp,cpp}`, `run_regalloc`, wired into `compile_func` at
  `src/codegen.cpp:4654` behind `CodeGenCtx::enable_regalloc`, default off,
  only effective with `enable_ir_backend`). Pinned by `regalloc_test` (ctest
  `regalloc`). NOT the full SSA-lite `IrFunction` design (still future).
- **extensions:** **13 NativeSig addon extensions** (vec/quat/mat/string/array/
  math/map/sync/thread/coroutine/lifecycle/io/call_raw) + **2 pass extensions**
  (opt/obf) = 15 `ember_ext_*` libs (`CMakeLists.txt` `ember_add_extension`).
- **toolchain:** MinGW g++ 15.2.0 (confirmed); MSVC x64 fails loudly
  (`CMakeLists.txt:21` `FATAL_ERROR "MSVC x64 not yet supported"`).
- **shipped v1.0+ features confirmed in source/tests:** constexpr fn,
  static_assert, typed enums (`enum E : T`), iterable() hook (slice + array
  handle), struct destructure + match guards, parameterized `fn(Args)->Ret`
  types, cross-module `&mod::fn` handles, GC core (`src/gc.{hpp,cpp}`),
  lambdas (by-value capture), coroutines (Windows fibers), try/catch/throw,
  in-context threads (`extensions/thread/`), namespaces, `ember pipe` + `ember
  live` CLI actions, self-hosting (4 stages + full pipeline).

## Findings + fixes

### README.md — FIXED

1. **Test count stale.** Said `ctest # 42 tests (40 excluding the two
   benchmarks)`. Actual: 54 (52 excluding benchmarks). **Fixed -> `54 tests
   (52 excluding the two benchmarks)`.**
2. **Extension count stale.** Said "The ten `NativeSig` extensions
   (vec/quat/mat/string/array/math/map/sync/lifecycle/io)". Actual: 13 addon
   extensions (adds thread, coroutine, call_raw). **Fixed -> "thirteen
   `NativeSig` addon extensions (.../thread/coroutine/.../io/call_raw)".**
3. **regalloc omitted from the shipped-behind-flags list.** The list mentioned
   the thin-IR backend, v5 IR serialization, and 8 passes but NOT the
   linear-scan regalloc (which shipped). Also said "Full SSA-lite + linear-scan
   regalloc remains the future upgrade" — the linear-scan-over-thin-IR subset
   shipped; only full SSA construction is future. **Fixed -> added the
   linear-scan regalloc to the shipped-behind-flags list and clarified the
   SSA-construction-vs-linear-scan distinction.**
4. **CLI reference missing `pipe` / `live`.** Both Family C actions shipped
   (`examples/ember_cli.cpp` `ember pipe <config>` + `ember live <file.ember>`,
   ctest `ember_cli_pipe_live`). **Fixed -> added `ember pipe` and `ember live`
   lines to the CLI reference.**
5. **Minor gap (noted, not fixed):** the `--ffi` / `--allow-io` flag (needed to
   call the `io` extension's `PERM_FFI`-gated natives) is not in the README CLI
   reference. The `io` extension + `PERM_FFI` are mentioned in the safety-model
   section. Recommend adding a `--ffi` bullet.

### extensions/README.md — FIXED

1. **Addon count stale.** Said "the standalone `ember` CLI links all ten addon
   extensions, plus the `opt`/`obf` pass extensions". Actual: 13 addon + 2
   pass. **Fixed -> "all thirteen addon extensions
   (vec/quat/mat/string/array/math/map/sync/thread/coroutine/lifecycle/io/
   call_raw), plus the `opt`/`obf` pass extensions".**
2. **Build library list stale.** Listed 10 addon libs + opt/obf, omitting
   `ember_ext_thread`, `ember_ext_coroutine`, `ember_ext_call_raw`.
   **Fixed -> added the three missing libs to the Build list.**
3. **Extension table incomplete.** The "What lives here" table lists
   vec/quat/mat/string/array/math/sync/lifecycle/map/io/opt/obf but omits
   `thread/`, `coroutine/`, `call_raw/`. **Noted in report; table not edited
   (each missing extension has its own subdirectory + header already).**
   Recommend adding three rows: `thread/` (thread_spawn/join/trap_reason —
   in-context threads, Tier 4), `coroutine/` (coroutine_start/next/done over
   Windows fibers, #21), `call_raw/` (make_executable/call_raw/
   free_executable_ptr — the self-hosting execution bridge, PERM_FFI-gated).

### docs/ROADMAP.md — FIXED (multiple stale TODO->shipped)

The ROADMAP's "Shipped v1.0" section + check markers covered enum, fn refs,
sync, context-thread-safety, lifecycle, constexpr, static_assert, typed enums,
for-each, match, map, io. But several Tier entries still said **TODO** for
features that have shipped. All fixed:

1. **Tier 4 Coroutines / `yield`** — was "TODO (blocked on GC/heap)".
   **Shipped** via `extensions/coroutine/` (Windows fibers, NOT GC/heap —
   the fiber copies the frame off the native stack on `yield`). Pinned by
   `tests/lang/valid_coroutine_*`. **Fixed -> SHIPPED.**
2. **Tier 4 Exceptions `try`/`catch`/`throw`** — was "TODO". **Shipped**
   (`TryCatchStmt`/`ThrowStmt`, `context_t::catch_bufs`, ctest `try_catch` +
   `valid_try_catch`/`valid_nested_try_catch`/`valid_throw_*`). **Fixed ->
   SHIPPED.**
3. **Tier 4 In-context threads** — was "TODO (largest, highest-risk)".
   **Partially shipped**: `extensions/thread/` (thread_spawn/join/
   thread_trap_reason) + ctest `in_context_threads`; the residual TODO is
   true concurrent execution on one `context_t` (the `call_mutex` serializes).
   **Fixed -> PARTIALLY SHIPPED.**
4. **Tier 6 Namespaces** — was "TODO". **Shipped** (`NamespaceDecl`, parser
   flattens + stamps `ns`, `valid_namespaces`/`valid_namespaces_intra_call`/
   `valid_namespaces_two_ns`). **Fixed -> SHIPPED.**
5. **Tier 1 Struct destructure + guards in match** — was "TODO". **Shipped**
   (`StructPattern`, `valid_struct_destructure`/`valid_match_guards`).
   **Fixed -> SHIPPED.**
6. **Tier 2 bare-`fn` signature hole -> parameterized fn types** — was "TODO".
   **Shipped**: `fn(Args)->Ret` parameterized types (`has_recorded_sig`,
   ctest `fn_types`, `valid_fn_types.ember` incl. higher-order
   `fn(i64)->fn(i64)->i64`). **Fixed -> SHIPPED.**
7. **Tier 2 Cross-module function handles** — was "TODO". **Shipped**
   (`is_cross_module_handle`, `&mod::fn`, ctest `cross_module_handles` +
   `sema_invalid_cross_module_handle_unlinked`). **Fixed -> SHIPPED.**
8. **Tier 3 Tracing GC** — was "TODO". **GC core shipped** (`src/gc.{hpp,cpp}`,
   ctest `gc_core`) as foundational infra; not yet wired into codegen/sema.
   **Fixed -> CORE SHIPPED; full integration TODO.**
9. **Tier 3 `new`/`delete` + lambdas with capture** — was "TODO, depends on
   GC". **By-value-capture lambdas shipped without GC** (#20, `is_lambda`,
   `valid_lambda*`). By-reference capture + `new`/`delete` still TODO.
   **Fixed -> lambdas SHIPPED; by-ref capture + new/delete TODO.**
10. **Family C `ember pipe` / `ember live`** — were "TODO". **Both shipped**
    (`examples/ember_cli.cpp`, ctest `ember_cli_pipe_live`,
    `tests/features/pipe_*.pipe` + `live_tick.ember`). **Fixed -> SHIPPED.**
11. **`ember bench` "commit pending"** — stale; bench shipped. **Fixed ->
    SHIPPED (2026-07-10).**

**ROADMAP test-count claims are accurate** ("54 CTest targets total (52
excluding the two bench targets...)"). Historical "Verified: ctest 22/22 /
26/26 / lang 245/0/0" lines are dated ship-time records, not current-count
claims — left as-is (they are audit-trail entries).

### docs/LIFECYCLE.md — FIXED

Section 5 "What this deliberately does NOT cover" claimed:
- "Coroutines / `yield` — v1 has no sequential-looking-code primitive" —
  **false**. **Fixed -> coroutines shipped (coroutine extension, Windows
  fibers).**
- "Exceptions caught in-script — a routine that faults aborts the whole
  invocation" — **false** (presented as the current state). **Fixed ->
  try/catch/throw shipped.**
Also corrected the "ROADMAP.md Tier 5" cross-reference -> Tier 4 (coroutines/
exceptions moved to Tier 4).

### docs/spec/TYPE_SYSTEM.md — FIXED

Section 2 Script-declared struct: said "Native by-value aggregate arguments
are supported only through 8 bytes; sema rejects larger native aggregate
arguments." **Stale** — `reject_large_native_aggregate` (`src/sema.cpp`) now
allows up to **128 bytes** (<=8B register, >8B Win64 hidden-pointer).
**Fixed -> "supported through 128 bytes"** with the exact limit + mechanism.

### docs/spec/CODEGEN_SPEC.md — FIXED

Section 8 Script-to-native calls: said "Native struct-by-value arguments are
supported only when the tightly packed Ember aggregate is at most 8 bytes.
Sema rejects larger native aggregate arguments ... a correct Win64
indirect-by-value implementation for those shapes is deferred." **Stale** —
now 128 bytes with the hidden-pointer path implemented + tested.
**Fixed -> "at most 128 bytes" + hidden-pointer path implemented; marked the
old claim superseded.**

Section 5 "Deferred register allocation design": the Stage-A note covered the
thin IR; the Stage-3 linear-scan regalloc shipment was not noted. **Fixed ->
added a Stage-3-regalloc implementation note** (linear-scan over
`ThinFunction` shipped behind `enable_regalloc`; full SSA-lite `IrFunction`
design still future).

### docs/spec/COMPILER_PIPELINE.md — FIXED

Section 8 "Deferred regalloc/codegen interface (not implemented)" — a
linear-scan regalloc over the thin IR HAS shipped (`run_regalloc(ThinFunction&)`,
ctest `regalloc`); only the full SSA-lite `IrFunction`/`run_linear_scan`
interface is still future. **Fixed -> retitled + added a status note
distinguishing the shipped thin-IR linear-scan subset from the future
full-SSA design.**

### docs/spec/BINDING_API.md — FIXED

Section 2 `fn_ptr` signature contract: said "The shipped path accepts native
by-value aggregate parameters only through 8 bytes; larger aggregate
arguments are rejected by sema." **Stale** — now 128 bytes.
**Fixed -> "through 128 bytes" + hidden-pointer path.**

Section 4 Calling-convention mapping table: row "struct >8 bytes argument |
deferred/unsupported | v1 sema rejects ... over 8 bytes". **Stale**.
**Fixed -> "struct >8 bytes argument (up to 128 bytes) | ... Win64
hidden-pointer by-value ... (sema rejects a registered struct >128 bytes)".**

### docs/spec/MEMORY_AND_GC.md — FIXED

Section 1 said "...excludes a GC from v1" and section 8 was titled "v2 GC
deferral". A tracing mark-sweep GC core has shipped (`src/gc.{hpp,cpp}`,
ctest `gc_core`) as foundational infra (not yet wired into codegen).
**Fixed -> section 1 clarifies "script-visible GC heap" excluded (the core
shipped); section 8 retitled "GC status - core shipped; full integration
deferred" with a status-update note.**

### docs/MODULES.md — FIXED

Section 1 said "The trigger has not fired ... this is YAGNI - spec'd, not
built." **Stale** — the v0.5 live-module trigger fired and `link` +
`ModuleRegistry` + cross-module call shipped (the doc's own section 6 +
status header say so; section 1 contradicted them). **Fixed -> section 1 now
states the v0.5 trigger fired and the core surface shipped, with only the
section 6 re-entry-trigger-gated work future.**

### docs/guide/** — FIXED (major staleness; banner + targeted corrections)

The **entire developer guide** (`docs/guide/`) was written against an early
pre-v1.0 state and was the most stale part of the tree. It is NOT a full
rewrite (tracked as a follow-up); the most actively-false claims were
corrected and a prominent staleness banner was added to `00-index.md`.

**False claims corrected:**
- **Bounds checking** (10-types, 40-expressions, 50-gotchas, 50-bubble-sort):
  the guide repeatedly said indexing is "not bounds-checked at runtime" /
  "undefined behavior". **False** — `TrapReason::BoundsCheck` is emitted
  (single unsigned `cmp`/`jae`; negative signed index caught as huge
  unsigned; constant index into fixed array checked at compile time). All
  instances **fixed** with corrected sections pointing at CODEGEN_SPEC
  section 9 + SAFETY_AND_SANDBOX section 5.
- **Struct-by-value** (10-types, 40-expressions, 50-gotchas): the guide said
  "Structs cannot be passed by value across a function call boundary in v1".
  **False** — shipped 2026-07-10 (<=8B register, >8B hidden pointer, native
  args up to 128B; struct-literal return + struct-by-value arg temps). All
  instances **fixed**.
- **for-each** (30-statements, 40-expressions, 50-bubble-sort, 10-fibonacci):
  the guide said "There is no range-based or for-each form in Ember".
  **False** — `for (x in slice)` / `for (x in array_handle)` shipped.
  **Fixed.**
- **Fictional `slice_length()` native** (40-expressions): the `sum_slice`
  example called `s.slice_length()`, which does **not exist** in any standard
  extension. **Fixed -> replaced with a hardcoded-bound example + a note that
  `slice_length()` doesn't exist (use for-each or carry the length).**
- **Struct layout** (10-types): said "laid out using MSVC x64 layout rules".
  **False** — Ember uses tight-packed layout (no alignment padding,
  `build_struct_layouts` in `src/sema.cpp`). **Fixed -> "Ember's tight-packed
  layout".**

**Standard-extension-API mismatch (banner + table fixes):**
The guide claims to document "the standard extension API that ships with
Ember" but actually documents the **prism host's** native set. Corrected:
- **20-api/00-overview** + **10-io-debug**: documented `print_i64`/`print_f32`/
  `print_str`; only `print_i64` is standard. Real `io` extension: `print`/
  `println`/`print_i64`/`print_f64`/`read_line`/`file_*`/`path_*` (all
  `PERM_FFI`-gated). **Fixed -> staleness banner + corrected overview table.**
- **20-assertions**: documented `assert_eq_*` as standard; they are
  **prism-host** natives. **Fixed -> staleness banner.**
- **20-api/30-strings**: documented `str_compare`/`str_length` (prism) and
  omitted `string_find`/`string_substr` (standard). **Fixed -> banner +
  corrected native table.**
- **20-api/40-math-vectors**: documented `aim_atan2`/`clamp` (prism) and
  omitted the f64 math + `abs_i64`. **Fixed -> corrected scalar-math table.**
- **20-api/50-arrays**: the function table was truncated (ended at
  `array_push_u8`), omitting `push_f32`/`push_i64`/`pop_*`/`clear`/`remove`;
  also repeated the false "unchecked" bounds claim. **Fixed -> completed the
  table + corrected the bounds claim.**

**Broken cross-references (banners added):**
- **30-examples/10-fibonacci**: referenced `examples/scripts/fibonacci.ember`
  (does not exist; real file is `fib.ember` with different content). **Fixed
  -> banner.**
- **30-examples/20-vector-math**: referenced
  `examples/scripts/vector_math_demo.ember` (does not exist). **Fixed ->
  banner** + the cross-ref from 40-math-vectors.
- **30-examples/50-bubble-sort**: referenced `examples/scripts/bubble_sort.ember`
  (does not exist). **Fixed -> banner.**

**Guide features NOT documented (noted in banners, not added):** enums
(untyped + typed), `match` (+ struct destructure + guards), `try`/`catch`/
`throw`, `constexpr fn`, `static_assert`, namespaces, lambdas, coroutines,
parameterized `fn(Args)->Ret` types, cross-module handles. The `00-index.md`
banner lists all of these and points at the spec docs as authoritative.

### Other docs checked — accurate / no fixes needed

- **docs/spec/SAFETY_AND_SANDBOX.md** — accurate (bounds checking section 5,
  call-target-provenance section 7a, context thread-safety section 8a all
  current).
- **docs/spec/PASS_SYSTEM_DESIGN.md** — accurate (8 opt passes + subst,
  matches `extensions/opt/ext_opt.cpp`).
- **docs/spec/BENCHMARK_SYSTEM_DESIGN.md** — accurate (design doc; dated
  results are records).
- **docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md** — accurate (Stage 1/A/C
  shipped notes current; dated ctest counts are ship-time records).
- **docs/HOT_RELOAD.md** — accurate (HotReloadDomain + guarded epoch
  reclamation current).
- **docs/BUNDLING_AND_EM_MODULES.md** — accurate (v1-v5 + Ed25519 v4 + v5
  re-emit-at-load current). Minor: Part 1's `include` keyword was renamed to
  `import` when shipped (the doc's section 1.5 notes the live `link`
  shipped); the `include` vs `import` naming is a design-doc artifact, not a
  live claim.
- **docs/RESEARCH_NOTES.md**, **docs/LLVM_PASS_SYSTEM_RESEARCH.md**,
  **docs/LLVM_ADDITIONAL_PASSES_RESEARCH.md**,
  **docs/HOST_STRUCT_ABI_RESEARCH.md** — research docs, accurate (prior-art
  surveys, not live feature claims).
- **docs/MAINTENANCE_LOG.md**, **docs/MAINTENANCE_CONSTRAINTS.md** —
  operational logs/rulebook; the MAINTENANCE_LOG's "ctest 34/34" entry is a
  dated record (the log's own note corrects prior "32/32" claims).
- **docs/planning/*** , **docs/spec/SPEC_AUDIT_2026-07-10.md** — historical
  planning/audit records (not live claims).

## Summary table

| Doc | Status | Fix |
|---|---|---|
| README.md | had stale counts + missing CLI/regalloc | FIXED |
| extensions/README.md | had stale addon count + lib list | FIXED |
| docs/ROADMAP.md | 11 stale TODO->shipped entries | FIXED |
| docs/LIFECYCLE.md | section 5 false (coroutines/exceptions) | FIXED |
| docs/spec/TYPE_SYSTEM.md | section 2 stale 8B->128B native aggregate | FIXED |
| docs/spec/CODEGEN_SPEC.md | section 8 stale 8B->128B; section 5 missing regalloc note | FIXED |
| docs/spec/COMPILER_PIPELINE.md | section 8 "not implemented" vs shipped regalloc | FIXED |
| docs/spec/BINDING_API.md | section 2+4 stale 8B->128B | FIXED |
| docs/spec/MEMORY_AND_GC.md | section 1+8 "no GC" vs shipped GC core | FIXED |
| docs/MODULES.md | section 1 "trigger not fired" vs shipped link | FIXED |
| docs/guide/00-index.md | whole guide stale | banner FIXED |
| docs/guide/10-language/*.md | false bounds/struct/for-each/fictional native | FIXED |
| docs/guide/20-api/*.md | prism natives documented as standard | banner + tables FIXED |
| docs/guide/30-examples/*.md | 3 broken file refs + false claims | banners FIXED |
| docs/spec/SAFETY_AND_SANDBOX.md | accurate | — |
| docs/spec/PASS_SYSTEM_DESIGN.md | accurate | — |
| docs/spec/BENCHMARK_SYSTEM_DESIGN.md | accurate | — |
| docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md | accurate | — |
| docs/HOT_RELOAD.md | accurate | — |
| docs/BUNDLING_AND_EM_MODULES.md | accurate (minor include/import naming artifact) | — |
| research/planning/audit-history docs | accurate (records, not live claims) | — |

## Recommended follow-ups (not done in this pass)

1. **Full guide rewrite.** `docs/guide/**` documents a pre-v1.0 language; the
   banners + targeted fixes here stop the worst misinformation, but the guide
   needs a comprehensive rewrite to cover enums, match, try/catch, constexpr,
   static_assert, namespaces, lambdas, coroutines, parameterized fn types,
   cross-module handles, and the real standard extension API (`print`/`println`/
   `print_i64`/`print_f64`/`read_line`/`string_find`/`string_substr`/...).
2. **Real example scripts.** Create `examples/scripts/fibonacci.ember`,
   `vector_math_demo.ember`, `bubble_sort.ember` matching the guide's "Full
   Source" blocks (using standard natives, not prism's `assert_eq_*`), OR
   rewrite the example docs to walk through the real shipped scripts
   (`fib.ember`, `struct.ember`, `string.ember`, `game_logic.ember`).
3. **extensions/README.md table:** add `thread/`, `coroutine/`, `call_raw/`
   rows to the "What lives here" table.
4. **README CLI reference:** add a `--ffi` / `--allow-io` bullet (needed to
   call the `io` extension's `PERM_FFI`-gated natives).
5. **Code-example compile verification:** the guide's code examples were not
   compile-tested in this pass (several use prism-only natives). A follow-up
   should run every guide example through `ember_cli` against the standard
   extension set.

## Verification

- `ctest -N` -> Total Tests: 54 (52 non-bench).
- `ember_cli test tests/lang` -> 249/249 passed.
- All edits are to `*.md` files only; no source (`src/`, `examples/`) was
  modified by this audit.
