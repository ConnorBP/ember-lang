# ember + prism restructure plan

A refactor/restructure plan, not a spec. Goal: separate the ember
*language* from the prism *cheat host* by concern, so the language
carries no cheat references and is reusable, while the cheat-specific
surface stays in prism. This plan is grounded in the verified state
of both trees as of this writing - see "Ground truth" at the end.

This plan **fired** - it superseded the former `ember/`-vs.-vendored-
`src/ember/`-in-prism split described in `../../../prism/docs/PRISM_DESIGN.md`
("ember lives under prism's `src/ember/`... promote to a standalone lib
later if a second consumer appears"). A second consumer became the
stated goal (reusability), the YAGNI deferral lifted, and the promotion
executed: ember is now the canonical language home at
`hyper_workspace/ember/`, consumed by prism via
`add_subdirectory(../ember)`. The prose below is the plan as written;
where it says "promote" it describes the move that has since happened.

---

## 1. The decision: PROMOTE

**ember becomes the canonical language home at `hyper_workspace/ember/`.**
prism's former in-tree vendored copy (the `src/ember/` that lived under
`prism/`) was the source of truth for the *implementation* (it had the
parser/sema/codegen/engine the standalone tree never grew); the standalone
tree was the source of truth for the *spec* (`ember/docs/*.md`) and the
`.em` pre-compile feature. The promotion reconciled both into one
canonical ember at `hyper_workspace/ember/` (src/ + docs/ + extensions/ +
examples/).

Why promote rather than factor-in-place: the user's goal "language
extensions ... in an extensions folder of ember itself for future
reuse" only makes sense if ember is the reusable root a second
consumer would link against. Factoring in place leaves two ember
trees and no reusable home. Promotion is the goal-shaped move.

What is **not** in scope for this plan (YAGNI):
- Building new language extensions / addons. `ember/extensions/`
  is created and *populated with existing non-cheat extensions
  relocated out of prism*; no new addons are authored speculatively.
- Reconciling the two `Type` systems by merging the standalone's
  trivial `Type` into prism's rich `Type`. The promotion **adopts
  prism's rich `Type` as canonical** (it's the superset - structs,
  slices, arrays). The standalone's trivial `Type` is abandoned with
  the standalone's hand-built IR, which is superseded by prism's
  real parser/sema/codegen. Only the `.em` feature is ported across
  (Section 5); the standalone's v0.1 hand-built IR `main.cpp` is retired.
- A second actual consumer. prism remains the only consumer; the
  promotion makes a *possible* second consumer cheap, it does not
  require one.

---

## 2. Target layout

```
hyper_workspace/
├── ember/                         # canonical language home
│   ├── CMakeLists.txt             # builds ember_core (static lib) + ember_cli + tests
│   ├── docs/                      # the language spec (unchanged home)
│   │   ├── ... (existing 13 docs)
│   │   ├── ../BUNDLING_AND_EM_MODULES.md   # already here
│   │   ├── ../MODULES.md                   # already here
│   │   └── RESTRUCTURE_PLAN.md          # this doc
│   ├── src/                       # pure language, ZERO cheat refs
│   │   ├── ast.hpp                 # at ember/src/ (relocated from prism) - string_xor_key REMOVED (Section 4)
│   │   ├── lexer.{hpp,cpp}         # at ember/src/ (relocated from prism)
│   │   ├── parser.{hpp,cpp}       # at ember/src/ (relocated from prism)
│   │   ├── sema.{hpp,cpp}          # at ember/src/ (relocated from prism) - string-encryption XOR REMOVED (Section 4)
│   │   ├── codegen.{hpp,cpp}       # at ember/src/ (relocated from prism) - obf helpers / CPUID gate / MBA REMOVED (Section 4)
│   │   ├── x64_emitter.hpp         # at ember/src/ (relocated from prism)
│   │   ├── jit_memory.{hpp,cpp}    # at ember/src/ (relocated from prism)
│   │   ├── dispatch_table.hpp      # at ember/src/ (relocated from prism)
│   │   ├── types.{hpp,cpp}         # at ember/src/ (relocated from prism)
│   │   ├── engine.{hpp,cpp}        # at ember/src/ (relocated from prism)
│   │   ├── import.{hpp,cpp}        # at ember/src/ (relocated from prism) - renamed to include_resolver
│   │   │                           #   (it's textual inclusion = `include`/bundle, Section 3)
│   │   ├── em_file.hpp             # PORTED from standalone (Section 5)
│   │   ├── em_writer.{hpp,cpp}     # PORTED from standalone (Section 5)
│   │   └── em_loader.{hpp,cpp}    # PORTED from standalone (Section 5)
│   ├── extensions/                # non-cheat-specific language extensions, reusable
│   │   ├── README.md              # what lives here, how to add one (Section 6)
│   │   └── (relocated non-cheat addons from prism, if any exist - Section 6 audit)
│   ├── examples/                  # language-only examples (from standalone + prism)
│   └── tests/                     # language tests (lexer/parser/sema/codegen/.em)
│
└── prism/                         # the cheat host; consumes ember
    ├── CMakeLists.txt             # add_subdirectory(../ember) or find_package(ember)
    ├── src/
    │   ├── prism/                 # unchanged: cheat natives, backends, overlay, GUI
    │   │   ├── prism_script_host.*       # bridge: compiles+runs ember with prism natives
    │   │   ├── proc_api.*, render_api.*, memory_*, overlay_*, prism_gui_*, ...
    │   │   └── (all cheat-specific code stays here)
    │   └── physmap/                # unchanged
    └── docs/

# (No `prism/src/ember_obf/` - the obfuscation features stay in ember; Section 4.)
```

The one structural rule: **nothing under `ember/` references specific
cheat products, hosts, or research objects by name** - FACEIT,
`unc_faceit`, discord, physmem, hyperv, binprotect, streamproof, EAC,
Vanguard, or `vgk.sys` - in code, comments, or docs. A grep for that
vocabulary against `ember/` returns zero hits. That is the
language-purity goal.

**What is NOT a violation:** generic obfuscation features. String
encryption (encrypted rodata + `__str_decrypt` native), MBA identities,
and CPUID-keyed gates are *language-level* facilities any host can
use; they are not cheat-specific. They STAY in ember (Section 4). Only
*named-product* references get cleaned. This is the distinction: an
`ObfuscationOptions` struct with a `str_decrypt_fn` field is fine;
a comment saying "mirrors binprotect/src/licensing/licensing.cpp" is
not, because it names a specific research object. The feature stays,
the name-dropping goes.

---

## 3. The `import` keyword and filename: DO NOT rename

`ember/src/import.{hpp,cpp}` is a **textual-inclusion** resolver
(inlines file contents before lexing, `seen`-set cycle detection). Per
`../BUNDLING_AND_EM_MODULES.md` Section 1, that *mechanism* is what the bundling
plan called `include` (bundle: parse-time merge), not live `import`
(runtime multi-module linking).

**But the existing scripts already use `import "path";` as the
keyword** (`prism/scripts/bomb_timer.ember` et al. all say
`import "lib/cs2.ember";`), and the resolver's regex matches
`^\s*import\s+"..."\s*;`. Renaming the keyword or the function
(`resolve_imports`) would break every existing script and the
include graph for no semantic gain.

Decision: **keep the `import` keyword, the `resolve_imports`
function name, and the `import.{hpp,cpp}` filename.** The header
comment was cleaned (the `unc_faceit` reference is gone; it now
accurately cites `../BUNDLING_AND_EM_MODULES.md` Section 1.2). The naming
tension between this textual `import` and a future *live* `import`
(../MODULES.md Tier 6) is resolved by noting that a live mechanism - if
it ever ships - uses a *different* surface keyword (e.g. `link` or a
qualified `import live "..."`), since `import "path";` is already
taken for textual inclusion. This is a ../MODULES.md doc note, not a
code change.

This means the user's "make the parser" and "make include" are
**both already implemented** in prism's ember (the parser exists; the
textual-inclusion resolver exists and is wired to `prism_script_host`).
The restructure relocates them to the canonical home; it does not
rename them.

---

## 4. Keep the obfuscation features; clean the named-product references

The obfuscation features STAY in the language - string encryption
(encrypted rodata + `__str_decrypt` native), MBA identities, and the
CPUID-keyed entry gate are generic language-level facilities, not
cheat-specific. The restructure's job here is narrow: **remove
references to specific cheats/products/research-objects by name**
from comments and docs, and re-point any code that name-drops a
sibling project at the generic facility instead. No code moves to
`prism/src/ember_obf/`; that directory is not created.

| Site | Current home | Action | How |
|---|---|---|---|
| `Program::string_xor_key` | `ast.hpp` | KEEP | Stays - language-level per-compile XOR key. Comment is fine (generic). |
| `StringLit::baked_key`/`encrypted` | `ast.hpp` | KEEP | Stays - encrypted-rodata fields. Rewrite the comment to drop "OLLVM-style string obfuscation" (OLLVM is a named project) → describe it generically as "encrypted rodata; codegen emits a `__str_decrypt` call so raw strings don't appear in the JIT'd exec memory." |
| String XOR in `sema.cpp` | `sema.cpp` | KEEP | Stays. Rewrite the "OLLVM-style" comment to a neutral description. |
| `ObfuscationOptions` + `str_decrypt_fn` in `codegen.hpp` | `codegen.hpp` | KEEP | Stays - the `ObfuscationOptions` struct and `str_decrypt_fn` field are generic. Rewrite the header comment to drop "binprotect integration" / "reuse binprotect's algorithms" → describe them as language codegen options any host can set. |
| CPUID-keyed entry gate, MBA identities in `codegen.cpp` | `codegen.cpp` | KEEP | Stays - generic obfuscation transforms. Rewrite comments to drop "mirrors binprotect/src/licensing/licensing.cpp" and "binprotect-inspired" → cite `../spec/CODEGEN_SPEC.md`'s obfuscation section generically. |
| `unc_faceit` ref in `import.hpp` | `import.hpp` | REWRITE | Strip "the only multi-file mechanism unc_faceit's `#include` actually uses" → neutral reference to `../BUNDLING_AND_EM_MODULES.md` Section 1.2. |

**The principle, restated:** the user's bar is "no cheat-related
references in comments and docs." A reference to a *named product*
(FACEIT, discord, binprotect, unc_faceit, physmem, hyperv) is a
cheat-related reference. A generic obfuscation feature (string
encryption, MBA, CPUID gate) is not - it's a language feature. So the
features stay, the name-drops go. If a comment says "OLLVM-style" or
"mirrors binprotect/...", rewrite it to describe the mechanism; do
not remove the mechanism.

---

## 5. Port the `.em` feature to canonical ember

The `.em` pre-compile work (AbsFixup relocations, em_writer, em_loader,
em_file, round-trip test) was built against the standalone tree's
trivial `Type` and hand-built IR. It ports to prism's (now canonical)
ember mechanically - the same change, made once already, made again
against a different emitter:

1. **`AbsFixup` + `abs_fixups()` on the canonical `LabelPatch`-equivalent.**
   Prism's ember has no `LabelPatch`; it has `X64Emitter` with raw
   `mov_reg_imm64(Reg, int64_t)`. Add an `AbsFixup{offset, kind}` vector
   + `mov_reg_imm64_external(Reg, AbsFixup::Kind)` to `X64Emitter`
   (mirrors the standalone change). Kinds: `DispatchTableBase=0`,
   `GlobalsBase=1`, and - because cross-module is on the table  - 
   `ModuleRegistryBase=2` (reserved; used only if Section 8 lands).
2. **Switch the two dispatch-base bakes** (`codegen.cpp:529`, `:1412`,
   `e.mov_reg_imm64(Reg::r11, ctx.dispatch_base)`) to the external
   form + a JIT-fill loop writing `ctx.dispatch_base` into the
   placeholders after emit (byte-identical output, same as standalone).
3. **`em_file.hpp`** moves across verbatim (it's Type-agnostic - just
   `EmModule`/`EmFunctionRecord`/`EmReloc` + constants).
4. **`em_writer`** moves across; the serializer reads the per-function
   emitted bytes + rodata + `AbsFixup`s from the canonical codegen's
   output. The canonical codegen emits into `jit_memory`'s
   `alloc_executable`; the serializer must capture bytes *before*
   publish (same `BuiltFn` pattern as standalone's test #7).
5. **`em_loader`** moves across; uses `alloc_executable` instead of
   standalone's `ExecArena::publish` (same shape: memcpy code+rodata,
   apply relocs, stamp dispatch slots).
6. **Round-trip test** ports to the canonical test harness: JIT a
   real parsed function (the parser now exists!), serialize, load,
   call, assert. This is *better* than the standalone test (which
   hand-built fib) because it exercises the real parser → sema →
   codegen → .em → load path end to end.

The standalone `main.cpp` v0.1 proof and its `encoder.{hpp,cpp}` /
`label_patch.{hpp,cpp}` / `exec_mem.{hpp,cpp}` / `ir.hpp` are
**retired** (superseded by prism's real parser/sema/codegen). They
stay in git history; the canonical `ember/src/` is prism's set + the
ported `.em` files. Do not carry the dead standalone encoder/IR into
the canonical tree.

---

## 6. `ember/extensions/` - what goes there

Created with a `../../extensions/README.md` explaining: an extension is a set of
`NativeFn` registrations + `OpOverloadTable` entries + any host C++
backing them, **not** a language grammar/type-system change (those
are `../ROADMAP.md` Tier 1+ language features, not extensions).
Extensions are how a host ships reusable non-cheat-specific addons.

**Audit prism for existing non-cheat extensions to relocate.** The
ROADMAP Tier 0 standard addon set (`array<T>`, `map<K,V>`, `string`,
`math`, `vec2/3/4`/`quat`/`mat4`) is the candidate list. Concretely:
grep `prism/src/prism/` for `NativeSig` registrations and classify
each as cheat-specific (`proc.*`, `render_*`, `gui_*`, memory, overlay
 -  stay in prism) vs general-purpose (`math_*`, `vec_*`, `array_*`,
`string_*` - relocate to `ember/extensions/<name>/`). Only relocate
ones that *already exist* and are *provably non-cheat*. Do **not**
author new addons (YAGNI - the user asked for an extensions *folder*
for future reuse, not a built addon library). If the audit finds
nothing general-purpose in prism today, `extensions/` ships with
just the README - that's a correct empty-by-YAGNI state.

---

## 7. Sequenced moves (highest value first, lowest risk order)

The order is chosen so prism's build never breaks for long: each move
ends with a compiling, tested state.

1. **Port `.em` to prism's ember IN PLACE** (before any moving).
   The AbsFixup + em_writer + em_loader changes were made directly in
   prism's then-in-tree `src/ember/` (add `em_*.{hpp,cpp}`, switch the
   two `mov_reg_imm64` call sites, add a round-trip test using the real
   parser), then build prism + run tests. This proved the `.em` feature
   against the real parser/sema/codegen *before* the restructure
   complicated blame. Lowest risk because it's additive to a working
   tree; highest value because it's the feature that would otherwise
   be re-done during the move.

2. **Clean named-product references in prism's ember IN PLACE** (Section 4).
   Rewrite comments in `ast.hpp`, `sema.cpp`, `codegen.{hpp,cpp}` that
   name specific products/research-objects (OLLVM, binprotect,
   "mirrors binprotect/src/licensing/...", "binprotect-inspired") to
   describe the mechanisms generically. **No code moves to
   `ember_obf/`; no `ember_obf/` is created.** String encryption,
   MBA, and the CPUID gate stay in the language as-is. Build prism,
   run tests. The grep-purity test (Section 2 - named products only, not
   "obfuscation" the word) then passed against prism's in-tree `src/ember/`.
   This is low risk because it's comment/doc edits, not semantics.

3. **Rename `import` → `include_resolver` in place** (Section 3), strip the
   `unc_faceit` comment. Build, test. Small, mechanical, clears the
   last language-purity hit.

4. **PROMOTE: move `prism/src/ember/` → `hyper_workspace/ember/src/`**
   (the whole directory, now pure - features intact, name-drops gone).
   Point prism's `CMakeLists.txt` at the new location via
   `add_subdirectory(../ember)` (or a relative path). Build prism
   against the relocated ember. Run all prism tests. This is the
   actual promotion; steps 1-3 made the moved tree pure and complete
   first, so the move is a pure file relocation + build-graph change.

5. **Create `ember/extensions/` + relocate non-cheat addons** (Section 6).
   Audit prism's `NativeSig` registrations; move general-purpose
   ones to `ember/extensions/<name>/`; leave cheat ones in prism. Wire
   prism to register the relocated extensions from their new home.
   Build, test. Last because it's the most optional (YAGNI-empty is
   acceptable) and touches prism's native registration.

Each move ends green. If any move breaks prism's build, the previous
move is the known-good rollback point.

---

## 8. Out of scope / explicit non-goals

- **Removing or relocating the obfuscation features.** String
  encryption, MBA, and the CPUID gate STAY in ember (Section 4). They are
  generic language-level facilities, not cheat-specific. Only
  *named-product* references in comments/docs get cleaned. Do not
  touch the mechanisms.
- **New language extensions / addons.** `extensions/` is a home for
  existing non-cheat extensions; no new addons authored (Section 6, YAGNI).
- **Reconciling the abandoned standalone `Type`/IR.** The standalone
  v0.1 hand-built IR (`ir.hpp`, `encoder.*`, `label_patch.*`,
  `exec_mem.*`, `main.cpp`) is retired, not merged. Only `.em` is
  ported (Section 5). The standalone's trivial `Type` is discarded with it.
- **A second actual ember consumer.** Promotion makes one possible;
  none is built or required.
- **Changing the native-binding API** (`NativeSig`, `OpOverloadTable`).
  It's already clean; it's the seam the restructure respects, not a
  thing being refactored.
- **Hot-reload semantics, dispatch-table invariants, `.em` format.**
  Unchanged by this restructure (the `.em` port preserves the format;
  `../HOT_RELOAD.md`/`../BUNDLING_AND_EM_MODULES.md` stand as-is).

## 8a. Post-restructure fix: prism_script_host / prism_gui_lib decoupling

While executing step 4 it emerged that `prism_script_host` (linked by the
CLI and test targets) referenced three symbols defined only in
`prism_gui_lib` - `GetPanelRegistry()`, `PanelRegistry::clear()`,
`build_panel_natives()` - so `ember_cli`, `hot_reload_test`, and
`loadscript_regression_test` failed to link unless they also pulled in the
ImGui/D3D-heavy `prism_gui_lib`. This was a pre-existing prism-internal
coupling, not caused by the restructure, but it blocked the CLI and the
hot-reload test, so it was fixed as a follow-up (concern separation, same
spirit as the restructure):

- Added two host-installed hooks to `prism_script_host`:
  `SetPanelNativeBuilder` and `SetPanelClear` (mirroring the existing
  `SetHostPrintSink` pattern). `prism_script_host` calls them if installed
  and no-ops if null.
- `prism_gui_lib` installs real impls at startup; the CLI/test targets
  leave them null (they don't exercise GUI panels) and link cleanly without
  `prism_gui_lib`.

Result: `ember_cli`, `hot_reload_test`, and `loadscript_regression_test`
now build and run. `hot_reload_test` PASSes all 8 hot-reload edge cases
(unloaded-script safety, `@on_tick`/`@on_unload` lifecycle, **load →
unload → reload globals re-initialized**, repeated-load safety). Hot reload
works; the prior failure was a link-time coupling, not a hot-reload bug.

---

## 9. Ground truth (verified firsthand, the basis for this plan)

- `hyper_workspace/ember/`: 2737 LOC across `src/`, 13 spec docs in
  `docs/`, no parser, trivial `Type{Prim}`. Holds the `.em` feature
  (AbsFixup in `label_patch.hpp`, `em_file.hpp`, `em_writer.*`,
  `em_loader.*`, round-trip test #7 in `main.cpp`, all passing under
  g++ and CMake+Ninja).
- `hyper_workspace/prism/src/ember/`: 5567 LOC, full
  lexer/parser/sema/codegen/engine/dispatch_table/jit_memory/
  x64_emitter/types/import. Rich `Type` (Prim + struct_name + is_slice
  + array_len + shared_ptr elem). The ember prism actually uses.
- Contamination (5 files): `ast.hpp` (`string_xor_key`, StringLit
  `baked_key`/`encrypted`/OLLVM), `sema.cpp` (string XOR), `codegen.cpp`
  (binprotect/CPUID-gate/MBA), `codegen.hpp` (ObfuscationOptions/
  `str_decrypt_fn`), `import.hpp` (`unc_faceit`). Binding surface
  (`NativeSig`, `OpOverloadTable`) clean.
- `prism/src/ember/import.{hpp,cpp}`: textual-inclusion resolver, =
  the `include`/bundle mechanism, already built.
- Prism couples via `NativeSig`/`OpOverloadTable`; cheat natives
  (`proc.*`, `render_*`, `gui_*`, memory, overlay) live in
  `prism/src/prism/` (clean); bridge is `prism_script_host`.
- `.em` port is mechanical: prism `X64Emitter::mov_reg_imm64(Reg,
  int64_t)` raw-bakes at `codegen.cpp:529`/`:1412` (dispatch_base);
  `jit_memory` has `alloc_executable`/`free_executable`.
- `../../../prism/docs/PRISM_DESIGN.md` documents the vendor-in-prism decision
  and its YAGNI deferral of promotion; this plan fires that deferral.

---

## 10. Open questions to resolve at execution time

1. **Does the parser drive `resolve_includes`, or does the engine?**
   `import.hpp` inlines file contents *before lexing* (textual). The
   `include "path";` token therefore never reaches the parser as a
   statement - it's resolved pre-lex. Confirm the lexer/engine wiring
   so `include` is resolved at the right stage; if the current
   `import.cpp` inlines unconditionally, gate it on the `include`
   keyword (not `import`, which is reserved for live modules).
2. **String-encryption: confirm it stays; clean only name-drops.**
   Section 4 settles this - encryption stays in the language, comments get
   re-described generically. The only residual check: ensure the
   `__str_decrypt` native registration is host-side (prism) and the
   language codegen only *emits the call*, so the language doesn't
   depend on prism registering it. Confirm during step 2.
3. **`add_subdirectory` vs vendored copy for prism→ember.**
   `add_subdirectory(../ember)` keeps one source of truth but couples
   the build graphs; a vendored copy decouples them but reintroduces
   divergence. Default to `add_subdirectory`; vendor only if prism's
   build can't tolerate the coupling.
4. **What exactly is in prism's non-cheat extension surface?** The
   Section 6 audit decides what moves to `ember/extensions/`. If the audit
   is empty, `extensions/` is README-only (acceptable).
