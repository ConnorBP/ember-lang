# ember - design plan

C-style scripting language that JIT-compiles straight to native
x86-64, for embedding in game engines. Goal: AngelScript's binding
ergonomics, an optimizing native-JIT language's speed.

**This document is the index.** Each subsystem is fully specified in
its own detail doc under `docs/`. Read this for the shape and
cross-references; read the detail docs for the byte-level / rule-level
answers. See `RESEARCH_NOTES.md` for the prior-art survey (binprotect,
a prior native-JIT scripting language, AngelScript, Mun) that grounds every decision below.

## Architecture in one page

```
source text
  │
  ▼
lexer ─► parser (recursive descent) ─► AST
  │
  ▼
sema: struct layout pass ─► fn-signature/slot pass ─► per-fn body check
  │
  ▼
codegen: tree-walk typed AST ─► stack-spilling x86-64 emitter
  │
  ▼
machine bytes ─► W^X executable pages

(deferred: SSA-lite IR ─► linear-scan allocation)
  │
  ▼
dispatch table slot[slot_index] = entry address   (HOT_RELOAD.md)
  │
  ▼
host calls ember_call(context, fn_name, args) ─► indirect call through slot
```

One calling convention everywhere (Win64 fastcall, Windows-first).
Script-to-script and host-to-script calls both go through the
dispatch table by slot index - that single rule is what makes hot
reload, recursion, and forward references all work without special
cases. JIT only, no bytecode interpreter fallback.

## Detail docs (read in this order for a top-down picture)

| Doc | What it nails down |
|---|---|
| `RESEARCH_NOTES.md` | What was pulled from each surveyed codebase and why. Read first  -  every later doc references it. |
| `TYPE_SYSTEM.md` | Primitives, struct layout (host-mapped vs script-declared), fixed arrays, slices, the no-raw-pointer rule, implicit/explicit conversion matrix, operator result types, the unsigned-index rule that makes bounds-checking a runtime-only (not signedness) concern, `void` return-path enforcement, `auto`'s narrow scope, annotation argument grammar. |
| `COMPILER_PIPELINE.md` | Lexer token set, grammar, AST/sema, shipped tree-walking lowering, error model, and clearly deferred SSA-lite/regalloc design. |
| `CODEGEN_SPEC.md` | Win64 calling convention, frame/code emission, exact encoder subset, fixups, calls, bounds/div traps, and the deferred linear-scan design clearly labeled. |
| `BINDING_API.md` | Shipped `BindingBuilder`/`NativeSig`, script-type→Win64 mapping and slice convention; fluent builders/descriptors are labeled deferred. |
| `SAFETY_AND_SANDBOX.md` | Threat/trust model stated explicitly. Instruction budget mechanism (where counters live, what trap-on-exhaustion does), stack depth guard, bounds checking policy, `PERM_FFI` compile-time gating, the single non-local-unwind primitive that all traps funnel through, what is explicitly *not* checked (documented gaps, not oversights). |
| `HOT_RELOAD.md` | Shipped atomic single-function replacement, host execution guards, and epoch/quiescence reclamation; whole-module reload and removed-function traps are deferred. |
| `MEMORY_AND_GC.md` | Ownership taxonomy (frame-local / host-owned / module-global / arena), the `arr[..]`-slice escape check that's the one place ember can introduce a dangling slice, module-global storage layout, string representation (rodata in the function's own code page), arena allocator design (reserved, not built in v1), explicit v2-GC deferral rationale. |
| `LIFECYCLE.md` | The native-JIT-language-equivalent of `main()` + `register_routine(cast(fn))` expressed ember's way: `@entry`/`@on_tick`/`@event(...)` annotation-based discovery + host name/slot lookup. (Script-side first-class function references — `&fn` / `handle(args)` / the `fn` type — shipped in v1.0; see `ROADMAP.md` Tier 2 and `SAFETY_AND_SANDBOX.md` §7a.) |
| `GAP_ANALYSIS.md` | Systematic completeness audit: every original-request requirement → where satisfied; every feature of the surveyed native-JIT language → v1 has / deferred-with-plan / out-of-scope. Includes the honest performance caveat (ember v1 is baseline JIT, won't match an optimizing native-JIT language's speed, but beats AngelScript's bytecode interpreter). |
| `ROADMAP.md` | Every v2+ deferral with a **re-entry trigger** (the signal that says "now build this") and a **dependency** (what else must exist first). Tiered: Tier 0 standard addon set (ships v1.0), Tier 1 small lang extensions (`enum` ✓ shipped v1.0), Tier 2 function refs (✓ shipped v1.0, two open items), Tier 3 OOP, Tier 4 GC, Tier 5 concurrency/exceptions (sync queues + context thread-safety ✓ shipped v1.0), Tier 6 ecosystem, plus hard non-goals. |

## Goals / non-goals (restated; full reasoning in detail docs)

Goals:
- Near-native execution speed for hot game-logic scripts.
- Fast compile: JIT in microseconds-to-low-ms per function. No LLVM,
  no temp files, no dlopen.
- Safe-ish sandboxing: instruction budgets, bounds-checked access,
  no raw pointer arithmetic exposed to script, `PERM_FFI` gating.
- Simple binding API: descriptor structs, not declaration-string
  parsing.
- Hot-reload of individual script functions without invalidating
  call sites.
- JIT only.

Non-goals for v1:
- Generics / templates. Closures. A tracing GC. Multithreaded
  execution inside one context. Arbitrary pointer casts. Self-hosting.
  (Script-side `enum` and first-class function references **are now in v1.0**
  — see the v1.0 milestone below and `ROADMAP.md`; `switch` landed in v0.2.)

## Milestones

- **v0.1** - hand-built IR for `fn add(i64,i64)->i64`, direct x64
  codegen, exec-memory allocator, the IR data model (`ir.hpp`), and
  the `.em` binary bundling/serialization format
  (`em_file`/`em_writer`/`em_loader`). Passes the original 5
  acceptance criteria in `CODEGEN_SPEC.md` Section 14 (int path, float path,
  forward-label fixup, spill path, recursive-via-dispatch-table) plus
  the expanded criteria in Section 15 (full Section 3 ISA byte-exact encoder
  coverage, `.em` serialize→load→run round-trip). This is the "prove
  it actually compiles to real machine code AND can be
  bundled/reloaded" gate.
- **v0.2** ✓ shipped - the full frontend landed: lexer, recursive-descent
  parser (full v1 grammar - control flow, structs, slices, switch, defer,
  ternary, compound assign, annotations), the 4-pass sema, and a
  tree-walking AST→x64 codegen. Script-to-script calls go through the
  dispatch table by slot. The `.em` bundling format (serialize→load→run
  round-trip) and cross-module textual `import "path";` inclusion both
  work. A standard addon set ships as native extensions in `extensions/`
  (vec/quat/mat/string/array/math). The canonical tree is standalone-
  buildable and standalone-testable: a prism-decoupled `ember` CLI runs
  `.ember` files, and a language regression suite (`tests/lang/` +
  `ember_check`/`sema_check` exes) runs under `ctest`.
  Honest divergence from the spec above: codegen is currently a
  correctness-first **tree-walking stack-spilling emitter**, not the
  SSA-lite IR + linear-scan regalloc specified in `COMPILER_PIPELINE.md`
  Section 5. That refactor is deliberately deferred until the benchmark
  harness exists to prove it matters (per Section 9 below - no
  speculative optimization before the bench harness exists).
- **v0.3** - the binding-correctness milestone. Three pieces:
  (a) a binding-ABI test suite that pins the Win64 script→native call
  ABI (struct-by-value arg/return, >4-arg spill, `f32` in xmm
  slot-parallel, slice as `ptr+len` two words — the mapping table in
  `BINDING_API.md` Section 4, now test-proven not just spec-asserted);
  (b) `BindingBuilder` (`src/binding_builder.hpp`), the ergonomic
  registration helper that dedupes the I/H/add boilerplate the six
  standard extensions each redefined — the "bindings like AngelScript"
  floor (`b.add("name", ret, {params...}, &fn)`, one call per native);
  (c) the `BINDING_API.md` truth-fix annotating that the fluent
  `TypeBuilder`/`StructBuilder`/`engine_t` surface is the v1.0 target,
  not current code, while `BindingBuilder` + the `NativeSig` map is the
  working API the extensions ship on. `PERM_FFI` is defined as a
  constant (stored on `NativeSig`) but **permission gating in codegen
  is deferred to v0.4** (v0.3 records the bit, does not yet refuse to
  marshal). `TypeBuilder`/`StructBuilder`/`engine_t` deferred to **v1.0**
  — re-entry trigger: a real host wants script-visible C++ struct types
  (the extensions use opaque `i64` handles + `StructLayoutTable`, which
  is sufficient until that trigger fires). (Note: struct
  decl/literal/field-access and the annotation machinery already landed
  in the v0.2 overshot; v0.3 is the binding-API correctness and
  host↔script calling-convention validation milestone.)
- **v0.4** ✓ shipped (first hardening + safe execution) - two commits.
  First: W^X JIT memory (`VirtualAlloc RW → memcpy → VirtualProtect RX`,
  the red-team V5 latent shellcode backstop), per-frame byte budget (V6-DoS
  stack-exhaustion fix), int32 struct-sizing overflow rejection (V6-overflow).
  Second: the non-local abort primitive + quantity budgets from
  `SAFETY_AND_SANDBOX.md` §2-§4 — `context_t` (setjmp/`__builtin_longjmp`
  checkpoint), instruction budget (sub+jg at loop back-edges only),
  stack-depth guard (inc/cmp/jcc at script-to-script calls), PERM_FFI
  sema gating, and trap-surface unification (all traps route through a
  host trap-stub → longjmp, fixing red-team V7 `@obf_keyed` forced
  SIGILL). Performance: budget+depth are compile-flag gated — zero
  overhead when off, one sub+jg per loop iteration / one inc+cmp+jcc
  per call when on. A red-team writeup (`EMBER_REDSHELL_WRITEUP.md` at
  the workspace root) documents the full attack-surface study + 8
  prioritized mitigations.
  **NOT shipped** (the original v0.4 prose listed these; they slipped to
  later work): lifecycle annotation *runtime effect* (`@on_tick`/`@event`
  are parsed but do nothing — only `@obf`/`@obf_keyed` have codegen
  effect) and single-function hot reload (the slot-stability *machinery*
  exists + whole-module load works, but no atomic-slot-swap + page-retire
  + concurrent reclamation path). Both have design docs (`LIFECYCLE.md`,
  `HOT_RELOAD.md`). This historical v0.4 status was closed later: guarded
  epoch/quiescence reclamation now ships; whole-module reload remains open.
- **v0.5** ✓ shipped - live modules (the Tier 6 ROADMAP item, pulled
  forward): bidirectional script↔`.em` cross-module linking through the
  real grammar. `link "foo.em" as foo;` + `foo::bar(args)`; a
  pre-compiled `.em` callee is callable from a JIT'd script and vice
  versa via the provenance-agnostic `ModuleRegistry` (MODULES.md §2.6).
  Built on the pre-existing runtime half (registry + kind-2 reloc); this
  added the source half (grammar, sema, codegen, linker/loader, an
  `--emit-em` CLI pre-compile mode). `--tick` lifecycle mode (a tick
  thread + TUI unload keybind, shared between the terminal CLI and the
  prism runtime) is a suggested follow-on once `@on_tick` has runtime
  effect.
- **v0.6** - benchmark harness vs AngelScript; tune regalloc/peephole
  only if the bench shows a need. This is the gate the SSA-lite IR +
  linear-scan regalloc refactor (COMPILER_PIPELINE.md §5) is deferred to
  — no speculative optimization before the bench proves it matters.
- **v1.0** - stable native binding API (the fluent `TypeBuilder`/
  `StructBuilder`/`engine_t` surface — see v0.3's deferred-binding
  analysis `docs/v0.3_DEFERRED_BINDING_ANALYSIS.md`), docs, example
  game-engine integration (event hooks via annotations).

  **Concurrency + Tier 2 batch shipped v1.0 (commit e5d1814 + follow-ons):**
  four features, each verified in source/tests; the current tree has 20 CTest
  tests with AngelScript and 19 without —
  - **Context thread-safety (Option D + B1)** — `context_t` restructured
    to a POD prefix; `CodeGenCtx::use_context_reg` makes the budget/depth/
    trap emits read `context_t` fields through `r14` (the per-call context
    register, callee-saved) so **one compiled body serves N per-thread
    contexts**; `ember_call_void`/`ember_call_i64` (`src/engine.{hpp,cpp}`)
    set `r14 = ctx` at entry. Keystone proof: `examples/thread_safety_test.cpp`
    (per-thread context, no cross-thread longjmp). `g_globals_for_codegen`
    superseded by `ctx.globals_index`/`ctx.globals_types` (parallel-compile-
    safe; legacy pointer kept as a null fallback). See `SAFETY_AND_SANDBOX.md`
    §8a, `docs/plan_CONTEXT_THREADSAFETY.md`.
  - **Enums (Tier 1)** — `enum E { A, B, C }` top-level decl + `E::A`
    qualified access; sema rewrites `EnumAccessExpr` → `IntLit` in place
    (no codegen change). Pinned by `tests/lang/{valid_enums,sema_valid_enums,
    sema_invalid_enum_unknown_enum,sema_invalid_enum_unknown_variant}.ember`.
    See `docs/plan_ENUMS.md`.
  - **Sync queue primitives (`extensions/sync/`)** — atomics
    (`aint8/16/32/64`), swap buffer (double-buffer), SPSC/MPSC/MPMC queues
    behind i64 handles, internally synchronized host storage; script side
    single-threaded per context (U2 contract). 16 tests in
    `examples/ext_sync_test.cpp` (multi-thread stress incl. 10k SPSC, MPMC
    contention). See `docs/plan_SYNC_QUEUES.md`.
  - **First-class function refs (Tier 2)** — `&fn` take-handle,
    `handle(args)` indirect call, `fn` type keyword; `emit_call_target_guard`
    validates the runtime i64 against a host-built bitset allowlist before
    dispatch (REDSHELL guard #6), `BadCallTarget` trap on a bad handle.
    Pinned by `examples/function_refs_test.cpp`. Two open items documented at
    `ROADMAP.md` Tier 2 (bare-`fn` signature hole → `fn(...)->...` typed fn
    params v2+; cross-module handles → v2+). See `docs/plan_FUNCTION_REFS.md`.

  The four new syntax forms (`&fn`, `handle(args)`, the `fn` type keyword,
  `enum E { ... }` / `E::A`) are documented at `COMPILER_PIPELINE.md` Section 1.

  **Follow-on commits** then built the dynamic-registration extension the
  Tier 2 fn-refs feature enables — `extensions/lifecycle/`
  (`register_routine`/`unregister_routine` + the `host_routines()` accessor;
  `LIFECYCLE.md` §2; pinned by `examples/ext_lifecycle_test.cpp`) — and wired
  the CLI `--tick` to the B1 per-call context model
  (the `--tick` thread-safety bug, fixed; the CLI is now a B1 host). Demo
  script: `examples/scripts/dynamic_registration.ember`. See
  `docs/v1.0_INTEGRATION_NOTES.md` §5.

  (The original v1.0 fluent-API milestone scope — `TypeBuilder`/
  `StructBuilder`/`engine_t` — is itself still trigger-gated on "a host wants
  script-visible C++ struct types," per the v0.6 integration notes; the
  concurrency + Tier 2 batch shipped under the v1.0 banner regardless.)

## Explicitly skipped (YAGNI ladder - full per-item reasoning in the
detail docs, summarized here)

- No parser generator - grammar is small, hand-rolled recursive
  descent (`COMPILER_PIPELINE.md` Section 2).
- No LLVM/Cranelift/asmjit - own minimal encoder, ISA subset is tiny
  (`CODEGEN_SPEC.md` Section 3, `RESEARCH_NOTES.md` on binwrite/Zydis).
- No declaration-string binding parser - descriptor structs are a
  one-line call site (`BINDING_API.md` Section 1-Section 2).
- No tracing GC v1 - every reference category has a non-tracing
  lifetime (`MEMORY_AND_GC.md` Section 1, Section 8).
- No generics/closures v1 (script-side `enum` shipped v1.0; `switch` shipped
  v0.2) - no game-scripting use case demands generics/closures yet
  (`DESIGN.md` non-goals, `COMPILER_PIPELINE.md` Section 1); `enum` and `switch`
  moved off the non-goal list when measured demand arrived (`ROADMAP.md`).
- No bytecode interpreter fallback - JIT only, one execution path
  (`RESEARCH_NOTES.md` on AngelScript's bytecode-VM-by-default being
  the differentiator).
- No phi nodes in IR - slot-backed merge vars instead
  (`COMPILER_PIPELINE.md` Section 5, `CODEGEN_SPEC.md` Section 5).
- No rel8 branch shrinking, no shared-epilogue tail, no
  omit-frame-pointer, no block reordering - correctness-first v1,
  all are additive later if benchmarks justify (`CODEGEN_SPEC.md` Section 4/Section 6).
- No `goto`, no C preprocessor, no multiple inheritance / diamonds,
  no self-hosting - hard non-goals, never added (`ROADMAP.md`).
- No templates/classes/coroutines/exceptions/heap/lambdas/modules in v1
  - each is a tracked deferral with a re-entry trigger in `ROADMAP.md`,
  not a forgotten gap (`GAP_ANALYSIS.md` Section 2).
- Script-side first-class function references shipped **v1.0** (`&fn` /
  `handle(args)` / the `fn` type keyword, `ROADMAP.md` Tier 2 ✓), with the
  REDSHELL guard #6 call-target-provenance invariant (`SAFETY_AND_SANDBOX.md`
  §7a) as the runtime backstop. Dynamic `register_routine`-style registration
  is host-defined (no ember core change) once a real mod wants it;
  annotation-based static discovery covers the game-loop case (`LIFECYCLE.md`).
- No builtins for arrays/maps/strings - exposed as **native addons**
  on host-owned memory (Tier 0, ships with v1.0, `GAP_ANALYSIS.md` Section 3,
  `ROADMAP.md`), not language builtins needing GC.
