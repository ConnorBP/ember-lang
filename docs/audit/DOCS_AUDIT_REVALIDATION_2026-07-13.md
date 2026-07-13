# Documentation Audit — Revalidation 2026-07-13

**Scope:** read-only audit of active claims in `README.md`, `docs/ROADMAP.md`,
`docs/BUNDLING_AND_EM_MODULES.md`, `docs/MODULES.md`, `docs/HOT_RELOAD.md`,
`docs/LIFECYCLE.md`, `extensions/README.md`, `docs/guide/**`, and CLI/build
documentation against `CMakeLists.txt`, `examples/ember_cli.cpp` help/behavior,
pass registries, extension registrations, current tests, and current
format-loader behavior.

**Method:** every claim below was revalidated against the live tree, not
repeated from historical log claims. The success gates were actually executed
(see §0). No source/spec/thirdparty files were modified; this is a report only.
`docs/spec/` design content and `thirdparty/` were not touched. `G:` was not
accessed.

---

## 0. Success gates — executed and recorded

These were run, not enumerated.

| Gate | Command | Result |
|---|---|---|
| Full CTest suite | `cd buildt && ctest --timeout 120` | **70/70 passed, 0 failed (100%)**, 135.81 s, exit 0. No tests excluded/skipped. |
| Validation-177 | `cd buildt && ./ember_cli.exe run ../tests/lang/optimization_validation.ember --fn main --passes constprop,forward,copyprop,instcombine,dce,licm,dse` | **exit 177** (expected). |
| Native lang runner | `cd buildt && ./ember_cli.exe test ../tests/lang` | **274/274 passed, 0 failed**, exit 0. |

**CTest inventory (70 total):** the two bench targets `bench_ember_vs_as`
(#47, TIMEOUT 300, configured only when the AngelScript SDK is present — it is
present in this tree) and `bench_codegen_paths` (#48, TIMEOUT 600), plus
`vst3_soak` (#11, `LABELS soak`), are the 3 non-core tests; 70 − 3 = 67 core.
This matches the README ("70 tests (67 excluding benchmarks + soak)") and the
ROADMAP count exactly. No `DISABLED` tests; no `EXCLUDE_FROM_ALL` on any test
target (`add_subdirectory(thirdparty/vst3sdk EXCLUDE_FROM_ALL)` excludes the
SDK from the default build target set, not from ctest).

**Build used:** `buildt/ember_cli.exe` (MinGW g++ 15.2.0 Rev8 / Ninja,
`CMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe` — matches the README's
"MinGW g++ 15.2.0 + Ninja" claim). `ctest --timeout 120` was used instead of
the per-test TIMEOUTs only to bound wall time; every test ran to completion.

**Verdict:** all three gates pass. No blocker. (The prior chunk's
DIRTY-READ-ONLY deferral was not a real external blocker for read-only test
execution; running ctest/validation writes only into ignored build-output
dirs and does not touch tracked source.)

---

## 1. Verified accurate (no change needed)

Recorded so the revalidation is explicit, not assumed:

- **Pass counts:** `extensions/opt/ext_opt.cpp::register_passes` registers
  **16** optimization passes; `extensions/obf/ext_obf.cpp::register_passes`
  registers **7** obfuscation passes; **23 total**. Matches README, ROADMAP,
  and `extensions/README.md` exactly. CLI registers both via
  `ext_opt::register_passes(pass_reg); ext_obf::register_passes(pass_reg);`
  (`examples/ember_cli.cpp:566-567`).
- **Extension count:** `register_standard_bindings`
  (`examples/ember_cli.cpp:141`) registers **15** addon extensions
  (vec/quat/mat/string/array/math/map/sync/lifecycle/io/coroutine/call_raw/
  audio/thread/gc) + the 2 pass extensions. Matches the README "fifteen
  `NativeSig` addon extensions" list and the `extensions/README.md` Build
  section. All 17 extension dirs exist under `extensions/`.
- **Platforms:** `CMakeLists.txt:21-22` `if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "MSVC x64 not yet supported; use MinGW")` — README's
  "MSVC x64 is unsupported — CMake fails loudly" is accurate. Windows x64 only,
  as ROADMAP states.
- **CLI exit semantics:** run path `exit_code = int(uint64_t(entry_ret) &
  0x7fffffff)` (`ember_cli.cpp:749`); `--load-em` `return is_void ? 0 :
  int(result)` (no 31-bit mask, `:1645`); pipe `return int(uint64_t(sum) &
  0x7fffffff)` (`:1310`); trap exit = 70. Matches README exactly.
- **Self-hosting demo:** `ember_cli.exe run self_hosted/full_pipeline.ember
  --fn run_demo --ffi` → **exit 7** (verified). Matches README/ROADMAP.
- **README cited paths:** all checked and exist —
  `examples/scripts/game_logic.ember`, `examples/game_host.cpp`,
  `examples/vst3_wrapper/gain_vst.ember`, `examples/vst_dsp_harness.cpp`,
  `examples/ember_stub_main.cpp`, `examples/ember_bundle.cpp`,
  `scripts/package_release.sh`, `examples/custom_pass/README.md`,
  `tests/features/pipe_{2,3}stage.pipe`, `tests/features/live_tick.ember`,
  `self_hosted/{full_pipeline,emberc}.ember`.
- **Bounds-checking corrections already landed** in `docs/guide/10-language/
  10-types.md` (fixed arrays + slices: "is bounds-checked at runtime"),
  `docs/guide/10-language/50-gotchas.md` ("Indexing Is Bounds-Checked At
  Runtime", CORRECTED 2026-07-11), `docs/guide/20-api/50-arrays.md`
  ("arr[i] on a slice or fixed array is bounds-checked at runtime"). The
  `00-index.md` STALENESS NOTICE correctly flags that other guide pages still
  carry the false claim.
- **`extensions/README.md` pass-extension text** (16 opt + 7 obf, names) is
  accurate against the registries.

---

## 2. Exact inaccuracies with proposed edits

### Finding 1 — `docs/guide/10-language/40-expressions-operators.md:190` (CRITICAL, actively false)

**Claim (line 190):**
> **WARNING:** Indexing is **not** bounds-checked, at compile time or at runtime.
> An out-of-range index is undefined behavior: a small stray index silently
> reads or writes whatever memory happens to sit next to the array on the
> stack, and a large enough one raises a hardware access violation. There is
> no compile-time check either, even when both the array's size and the index
> are constants known at compile time. Double-check index arithmetic yourself;
> the language will not catch a mistake here for you.

**Claim (line 195, code example):**
> `let bad: i32 = arr[N];   // compiles without error, despite N == arr's own length - undefined behavior at runtime`

**Revalidation (live):**
- Runtime: `let arr: i32[4] = [10,20,30,40]; let i: i64 = 10; let v: i32 = arr[i];`
  → `ember: RUNTIME TRAP: bounds check: index out of range (bounds check)`,
  **exit 70**. Emitted by `src/codegen.cpp:330` `emit_bounds_check_reg` /
  `emit_bounds_check_imm` (`cmp idx,len; jae .oob_trap` →
  `TrapReason::BoundsCheck`) and mirrored in `src/thin_emit.cpp:351`. NOT
  undefined behavior.
- Compile time (literal constant index): `arr[10]` against `i32[4]` →
  `ember: sema errors (1): line N: array index 10 out of bounds for array of
  size 4`, **exit 2** (`src/sema.cpp:2600`). So a literal constant index IS
  checked at compile time. (A `const`-named index like `const N: u64 = 4;
  arr[N]` is not folded by sema and instead traps at runtime — exit 70 — still
  not UB.)
- The `50-gotchas.md` and `10-types.md` pages already state the correct
  behavior; this page was not updated.

**Proposed edit:** replace the WARNING block + the `arr[N]` example with the
corrected text already used in `10-types.md` / `50-gotchas.md`, e.g.:

> Indexing **is bounds-checked**. `arr[i]` compiles to a single unsigned
> `cmp idx, len; jae .oob_trap` before the address computation, and an
> out-of-range index (including a negative signed index, which appears as a
> huge unsigned value under the unsigned compare) traps via
> `TrapReason::BoundsCheck` — a recoverable non-local unwind to the host, not
> undefined behavior. A compile-time-constant *literal* index into a fixed
> array of known size is checked at **compile time** instead (a sema error,
> zero runtime cost). Keeping indices in range is still good practice (the
> trap aborts the call). See `docs/spec/CODEGEN_SPEC.md` §9 +
> `docs/spec/SAFETY_AND_SANDBOX.md` §5.

and change the example comment to:
> `let bad: i32 = arr[N];   // N is a const, not folded -> traps at runtime with TrapReason::BoundsCheck (not UB)`

### Finding 2 — `docs/guide/10-language/30-statements.md:206` (CRITICAL, actively false)

**Claim (last sentence of the `defer` WARNING, line 206):**
> Note that an out-of-bounds array or slice index is not one of the cases that
> traps at all, indexing is not bounds-checked, so it neither traps nor runs
> pending `defer`s; it simply reads or writes past the end of the array.

**Revalidation (live):** an OOB index **does** trap (exit 70,
`TrapReason::BoundsCheck` — see Finding 1). The `defer`-does-not-run-on-trap
point itself is correct (defer is skipped on a trap-unwind), but the rationale
("indexing is not bounds-checked, so it neither traps") is false: an OOB index
*does* trap, and precisely because it traps, pending `defer`s do not run — the
same as any other trap.

**Proposed edit:** replace the trailing sentence with:
> Note that an out-of-bounds array or slice index **does** trap
> (`TrapReason::BoundsCheck`), so it is one of the cases where pending `defer`s
> do not run — the trap unwinds without running deferred cleanup, exactly like
> a divide-by-zero or a failed assertion. Indexing is bounds-checked at
> runtime (and at compile time for literal constant indices); see
> [40-expressions-operators.md](40-expressions-operators.md) §Indexing.

### Finding 3 — `docs/guide/10-language/20-declarations.md:33-34` (actively false, no per-page notice)

**Claim (lines 33-34, under Functions):**
> There is no by-value struct parameter: structs cannot be passed by value
> across a function call boundary in v1, so a function that logically operates
> on a struct's fields takes those fields as separate scalar parameters
> instead.

plus the example `// NOT fn area(s: Shape) -> f32 { ... } - takes the fields
directly instead.`

**Revalidation (live + code):** structs **can** be passed and returned by
value (shipped 2026-07-10). `10-types.md`, `50-gotchas.md`, and
`40-expressions-operators.md` all carry CORRECTED notes; `binding_abi_test`
probes [2c]/[2d]/[2e] (`V3{1,2,3}`, `v3_dot(v3_up(),v3_up())`) are ctest-green.
This page was not corrected and has no staleness notice.

**Proposed edit:** replace with the corrected text used in `10-types.md`:
> Structs **can** be passed by value across a function call boundary (shipped
> 2026-07-10). A struct ≤8 bytes is passed in one register; a struct >8 bytes
> uses the Win64 hidden-pointer by-value path. A native by-value arg is
> rejected only if its registered struct size exceeds 128 bytes. A fn can
> `return V3 { ... };` directly and pass a struct literal / struct-returning
> call as a by-value arg. See `docs/spec/TYPE_SYSTEM.md` §12 +
> `docs/spec/CODEGEN_SPEC.md` §16.

and replace the `// NOT fn area(s: Shape) ...` example with a working
`fn rect_area(r: Rect) -> f32 { return r.width * r.height; }` form.

### Finding 4 — `docs/guide/10-language/20-declarations.md:89` (actively false, contradicts `10-types.md`)

**Claim (line 89, under Structs):**
> Fields are separated by semicolons, not commas. Struct layout follows MSVC
> x64 rules.

**Revalidation (code):** `src/sema.cpp:66` `build_struct_layouts` lays fields
out **tight-packed** — `off = 0; … off += sz;` with **no alignment padding and
no trailing padding**. `10-types.md:62-67` correctly states "Ember's
tight-packed layout … no alignment padding … This is **not** MSVC/C layout."
This page directly contradicts the types page.

**Proposed edit:** replace "Struct layout follows MSVC x64 rules." with:
> Struct layout follows **Ember's tight-packed layout**: fields are placed in
> declaration order at consecutive offsets with no alignment padding and no
> trailing padding (the offset of each field is the sum of the previous
> fields' Ember sizes; `build_struct_layouts` in `src/sema.cpp`). This is
> **not** MSVC/C layout; a host that needs a C-layout struct uses an explicit
> host-mapped struct (`docs/spec/BINDING_API.md`).

### Finding 5 — `README.md` + `examples/ember_cli.cpp` `usage()`: `--gc-env` and `--allow-io` are parsed but undocumented (feedback item)

**Revalidation (code):** `examples/ember_cli.cpp` parses:
- `--gc-env` (line 1533) → sets `gc_env` → `opts.gc_env = gc_env` (line 1665)
  → `ctx.use_gc_env = opts.gc_env` (line 562): allocates lambda closure envs
  on the tracing-GC heap (#20) instead of stack-frame temps.
- `--allow-io` (line 1564) as an **alias** for `--ffi`
  (`else if (a == "--ffi" || a == "--allow-io")`).

Neither is in `usage()` (lines 170-251). `usage()` documents `--ffi` but not
the `--allow-io` alias and not `--gc-env`. The README's CLI reference block
documents neither `--gc-env` nor `--allow-io`.

(The prior audit's "Finding 14" wrongly said all listed flags are documented
by `usage()`; that is incorrect — `--gc-env` and `--allow-io` are the two
exceptions.)

**Proposed edits:**
- **CLI-help omission** — add to `usage()` in `examples/ember_cli.cpp`, in the
  per-flag section:
  > `  --ffi / --allow-io  grant FFI permission: enable I/O natives (print, file, path)`
  > `  --gc-env            allocate lambda closure envs on the tracing-GC heap (off by default)`
- **README omission** — add `--gc-env` and the `--allow-io` alias to the
  README CLI reference block (the line currently shows only the core flags;
  note `--allow-io` is already mentioned in `docs/ROADMAP.md` Family B and in
  the `io-debug`/`assertions` guide staleness notices, so the README is the
  outlier).

### Finding 6 — `README.md` CLI reference: `ember bundle` omits `--permissions` / `--output-permissions`

**Claim (README CLI reference):**
> `ember bundle <file.ember> <output.exe> [--stub <stub.exe>] [--fn NAME]`

**Revalidation (code):** `ember bundle` dispatches to
`ember_bundle::command` (`ember_cli.cpp:1500`), which parses `--stub`,
`--fn`, `--permissions` (none|ffi), and `--output-permissions`
(stub|preserve) (`examples/ember_bundle.cpp:447-459`). The CLI's own `usage()`
(ember_cli.cpp:184-187) shows all four. The README omits the two
permissions flags.

**Proposed edit:** extend the README bundle line to:
> `ember bundle <file.ember> <output.exe> [--stub <stub.exe>] [--fn NAME] [--permissions none|ffi] [--output-permissions stub|preserve]`

### Finding 7 — `README.md` line 41: version "v1.2" vs `CMakeLists.txt` project version 1.0.0

**Claim (README:41):** `Current version: **v1.2**.`

**Revalidation:** `CMakeLists.txt:11` `project(ember VERSION 1.0.0
LANGUAGES CXX)`. A repo-wide grep finds **no** other `v1.2` / `1.2.0`
reference anywhere (no `EMBER_VERSION` define, no header constant). The
documented "v1.2" does not correspond to any build version constant; the only
machine-readable version is the CMake project version `1.0.0`.

**Proposed edit:** reconcile — either bump `CMakeLists.txt` to
`project(ember VERSION 1.2.0 …)` to match the README, or change the README to
`v1.0` to match CMake. (Assumption: the README "v1.2" is the intended release
label and the CMake `1.0.0` is stale; confirm with the owner before editing
either, since this is a release-versioning decision, not a pure doc fix.)

### Finding 8 — `docs/ROADMAP.md:1138` header label stale ("TODO" vs shipped body)

**Claim (line 1138):** `### Standalone exe bundler — TODO`
**Body (line 1140):** `- **Standalone exe bundler** — **✓ SHIPPED (v1.0,
\`ember bundle\` CLI in v1.1).**`

**Revalidation:** `ember bundle` + `ember_bundle.exe` + `ember_stub_main.exe`
ship and are ctest-covered (`bundler_test`, `em_cli_emit`, etc., all in the
70/70). The section header label "— TODO" contradicts the shipped body.

**Proposed edit:** change the header to `### Standalone exe bundler — SHIPPED`
(or fold the section into the Family-C shipped area).

### Finding 9 — math extension docs are stale: ~20 registered natives undocumented; `atan2`/`clamp` mislabeled as prism-host-only

**Revalidation (code + live):** `extensions/math/ext_math.cpp::register_natives`
registers a much larger set than any doc lists. Verified callable from the
standalone CLI (`--ffi`, exit 0): `atan2_f64`, `clamp_i64`, `min_i64`, plus
`abs`/`atan`/`atan2`/`atan_f64`/`ceil`/`clamp_f64`/`exp`/`exp_f64`/`floor`/
`fmod_f64`/`log`/`log_f64`/`log2_f64`/`log10_f64`/`max_i64`/`max_f64`/
`min_f64`/`round`/`round_f64`/`trunc_f64` (and the f32 mirrors).

Three docs under-document or mislabel this:
- `docs/guide/20-api/40-math-vectors.md` Scalar Math table lists only
  `sqrt/sin/cos/tan` + the f64 set + `abs_i64`, and explicitly says
  "`aim_atan2` and `clamp` … are **prism-host** natives … not the standard
  `math` extension." That is **false for `atan2` and `clamp`**: the standard
  extension ships `atan2` (f32), `atan2_f64`, `clamp_f64`, `clamp_i64`.
  (`aim_atan2` specifically is the prism-named variant and is not standard —
  that part is correct.)
- `README.md` Tier-0 math line: "✓ limited v1 API — … broader math still
  deferred." "Broader math" is **no longer deferred** — it shipped.
- `extensions/README.md` math row lists only the original subset.
- `docs/guide/20-api/00-overview.md` math line repeats the limited subset.

**Proposed edits:**
- `40-math-vectors.md`: expand the Scalar Math table to the full registered
  set; correct the `atan2`/`clamp` note to "`aim_atan2` is the prism-named
  variant (not standard); the standard extension ships `atan2`/`atan2_f64` and
  `clamp_f64`/`clamp_i64`."
- `README.md` / `extensions/README.md` / `00-overview.md`: update the math
  description from "limited v1 API … broader math still deferred" to the
  actual shipped set (or at minimum drop "broader math still deferred").

### Finding 10 — `extensions/README.md` audit table omits 5 of the 15 addon extensions

**Revalidation:** the top audit table lists 10 NativeSig addons
(vec/quat/mat/string/array/math/sync/lifecycle/map/io) + the 2 pass
extensions, but omits **thread, coroutine, call_raw, audio, gc** (5 addon
extensions that the Build section below it and the README both list). All 5
dirs exist (`extensions/{thread,coroutine,call_raw,audio,gc}/`) and are
registered in `register_standard_bindings`. The table is internally
inconsistent with its own Build section.

**Proposed edit:** add 5 rows to the audit table for `thread/`
(`ember_ext_thread`), `coroutine/` (`ember_ext_coroutine`), `call_raw/`
(`ember_ext_call_raw`), `audio/` (`ember_ext_audio`), `gc/`
(`ember_ext_gc`), matching the Build section's list and the registration
order in `register_standard_bindings`.

### Finding 11 — `docs/guide/20-api/30-strings.md` staleness notice is itself partly stale

**Claim (STALENESS NOTICE):** "the table below **omits `string_find` and
`string_substr`** (both shipped)."

**Revalidation:** the native table immediately below **does** include
`string_find` and `string_substr` rows. So the notice's "omits" sentence is
now false (the table was updated but the notice was not). The body also still
contains a full `### str_compare and str_length` section and a `str_compare`
WARNING that document prism-host natives as if standard, despite the notice.

**Proposed edit:** delete the "omits `string_find` and `string_substr`"
sentence from the notice (they are present); then either remove the
`str_compare`/`str_length` table rows + section + WARNING, or clearly mark
them "prism-host, not registered by `ext_string`" inline (the notice already
says this, but the body still presents them as reference entries). The
`10-types.md` cross-reference ("the `string_*` functions, `str_compare`,
`str_length`") should drop `str_compare`/`str_length` for the same reason.

---

## 3. Marked TODO (large documentation/design work — not a one-line edit)

These are real but require a page rewrite or a design decision; flagged as
TODO per the task instructions, not proposed as inline edits here.

- **TODO — `docs/guide/01-getting-started.md`:** the "Hello, World" example
  uses `print_str` and the text presents `print_f32`/`print_str` as built-in.
  Revalidated: `print_str` is **not** registered by the standalone CLI
  (`ember: sema errors: unknown function 'print_str'`, exit 2 even with
  `--ffi`); the standard io natives are `print`/`println`/`print_i64`/
  `print_f64`. The page has no staleness notice. Full rewrite needed to use
  `print`/`println`/`print_i64`/`print_f64` and to note `--ffi` is required
  for any io native.

- **TODO — `docs/guide/20-api/10-io-debug.md`:** has a STALENESS notice + a
  corrected overview table, but the body sections `## print_f32` and
  `## print_str` still document prism-host natives as standard. The notice
  itself says "A full rewrite of this page is tracked as a follow-up." Keep
  the notice; rewrite the body to the 11 standard io natives only.

- **TODO — `docs/guide/20-api/20-assertions.md`:** has a STALENESS notice
  flagging `assert_eq_*` as prism-host (not registered by the standalone CLI —
  verified: not in `ext_io`/any standard extension; a script calling
  `assert_eq_i64` gets "unknown function"). The body still says
  "`assert_eq_*` … always available" and "There is no separate test runner,"
  both false against the standalone CLI (which ships `ember test` and does
  not register `assert_eq_*`). The notice flags this; full rewrite needed
  (point users at `// expect: N` + `ember test`).

- **TODO — `docs/guide/30-examples/{10-fibonacci,20-vector-math,50-bubble-sort}.md`:**
  each has a STALENESS notice correctly flagging that the named
  `examples/scripts/*.ember` files do not exist (verified: `fibonacci.ember`,
  `vector_math_demo.ember`, `bubble_sort.ember` are all MISSING; the real
  files are `fib.ember`, etc.) and that the "Full Source" blocks are
  illustrative and use prism-host `assert_eq_*`. The notices are accurate;
  the pages need a rewrite against the real shipped scripts.

- **TODO — `docs/guide/10-language/20-declarations.md` annotation example
  (lines ~253-260):** uses `ref_process`/`ru64`/`print_u64`/`print_string`,
  all prism-host/cheat natives not registered by the standalone CLI. Lower
  priority than Findings 3-4 on the same page (the struct claims), but the
  page should either mark these as host-specific illustrative names or swap
  them for standard natives.

- **TODO — `docs/guide/20-api/40-math-vectors.md` examples:** several examples
  use `assert_eq_f32` (prism-host) and one uses `print_f32` (prism-host). The
  page's Scalar Math NOTE is otherwise well-corrected; the example natives
  need the same treatment as the assertions/io pages.

---

## 4. Notes / assumptions

- No files were edited; this is a report only. `docs/spec/` design content and
  `thirdparty/` were not modified. `G:` was not accessed.
- The validation-177 pass list
  (`constprop,forward,copyprop,instcombine,dce,licm,dse`) matches
  `scripts/prepare_release.sh` exactly; exit 177 confirmed.
- The "v1.2" vs CMake "1.0.0" version gap (Finding 7) is the only finding that
  touches a release-versioning decision rather than a pure doc correction;
  owner confirmation recommended before changing either side.
- The math-extension growth (Finding 9) is the largest single
  shipped-vs-documented drift: ~20 natives shipped, docs still describe the
  "limited v1 API" and actively mislabel `atan2`/`clamp` as non-standard.
- All guide pages that were already corrected (`10-types.md`, `50-gotchas.md`,
  `50-arrays.md`, `00-overview.md`) and the `00-index.md` STALENESS NOTICE are
  accurate; the residual actively-false guide claims are the four in Findings
  1-4 (statements/expressions-operators/declarations) plus the TODO rewrites
  above.
