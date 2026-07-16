# Research notes and prior-art decisions

This document records the research that shaped Ember. It is not a current
feature-gap list; current status lives in `README.md` and `ROADMAP.md`.

## 1. Native JIT rather than an interpreter

AngelScript's normal execution model is a bytecode VM. Its optional JIT hook
does not remove the complexity of a bytecode baseline and generic call
trampolines. Ember instead chose one execution model: native code.

Consequences:

- source and ThinIR ultimately execute as native machine code;
- public IR-on-disk modules are validated and re-emitted to native code;
- no bytecode interpreter fallback is planned;
- native ABI lowering and W^X publication are central correctness boundaries.

The benchmark harness confirms the basic premise: compute-heavy Ember workloads
outperform the AngelScript interpreter, although the baseline Ember tree walker
still trails optimized C++ on spill-heavy paths. ThinIR, optimization passes,
and register allocation address that second comparison.

## 2. Minimal owned x86-64 emitter

A surveyed sibling binary-rewriting project used Zydis encoder requests to
patch instructions in an already laid-out PE image. That architecture had no
label graph, relocation ownership, or executable-memory lifecycle suitable for
an in-process compiler.

Decision: Ember owns the small encoder subset it emits, including labels,
fixups, external relocations, Win64 call lowering, and RW-to-RX page
publication. Zydis was not added as a JIT dependency.

This decision remains current. A decoder/disassembler could still be useful as
a debug verifier, but it is not required by the execution path.

## 3. Explicit binding descriptors

AngelScript parses declaration strings such as registered global-function and
object-method signatures. Ember instead uses typed C++ descriptors:

- `BindingBuilder`
- `NativeSig`
- canonical Ember `Type` values
- explicit permission bits
- registered operator overload tables

This avoids a second declaration parser at the host boundary and allows the
same canonical signature machinery to validate JIT calls, public `.em` native
bindings, self-hosted EMBM native relocations, and keyed dispatch domains.

## 4. Dispatch-table indirection from Mun

Mun demonstrated the value of stable function identity separated from current
code address. Ember adopted the idea, not Mun's LLVM/AOT implementation:

- ordinary script calls use stable dispatch slots;
- hot reload replaces slot contents without rewriting callers;
- cross-module calls add stable module identity through `ModuleRegistry`;
- function handles carry logical slots rather than executable pointers;
- keyed modules preserve logical identity while routing to versioned physical
  layouts.

Ember deliberately skipped LLVM as a required dependency to retain low compile
latency and a small embedding footprint. Its ThinIR and emitter are native to
this project.

## 5. Public module format evolution

The first public `.em` design serialized native x86 plus relocations because it
maximized load-time savings and retained one execution path. Security research
then made the trust boundary explicit:

- raw x86 is native-code execution by construction;
- build/ABI hashes provide compatibility, not authentication;
- Ed25519 v4 authenticates raw content but does not sandbox it;
- v5 ThinIR reduces the input surface by structural/semantic validation and
  host-side re-emission before executable allocation;
- v6 adds capability-negotiated keyed-dispatch metadata.

The public format magic is `EMBL`. The self-hosted compiler later introduced a
separate `EMBM` in-memory module image with v1/v2 layouts. The similar names do
not imply format compatibility.

See `BUNDLING_AND_EM_MODULES.md`, `MODULES.md`, and
`../self_hosted/MODULE_IMAGE_FORMAT.md`.

## 6. Polymorphic passes and keyed dispatch

The pass system grew from simple named pass registration into configured,
transactional factories with deterministic seed derivation. The current tree
ships 18 optimization and 7 obfuscation passes. Named profiles are ordinary
recipes expanded through the same registry; there is no hidden second pass
manager.

Environmental keyed dispatch follows a constrained design:

- provider material is adapted into transient route words;
- callables are grouped by exact ABI/visibility/calling-mode/domain identity;
- versioned permutation chooses bounded physical routes;
- immutable records are validated before publication;
- artifacts and runtime state store no expected environmental key, key digest,
  or verifier;
- wrong/missing capabilities fail closed.

This makes routing an input to dispatch layout and resolution rather than a
secret-comparison branch embedded in module code.

## 7. GC strategy

Early design notes deferred a script-visible collector because the original
language subset used values, slices, and host-owned opaque handles. Lambdas,
managed allocation, extension-owned references, and concurrent execution later
provided concrete re-entry triggers.

Current decision:

- tracing mark-sweep;
- precise frame and global root maps, not conservative stack scanning;
- extension trace callbacks for host stores;
- GC-backed by-reference closure environments;
- language-level managed `new`/`delete` pointers;
- write-barrier bookkeeping;
- cooperative stop-the-world coordination for concurrent participants.

A concurrent or generational collector remains a performance option. It is no
longer accurate to describe GC integration, by-reference capture, or managed
allocation as unimplemented.

## 8. Self-hosting

The project first proved an Ember-written lexer/parser/sema/codegen subset and
then used a differential matrix to drive full completion. The completed state
is:

- 188/188 parity tests;
- no unsupported cases, mismatches, or hangs;
- one-stage self-compilation proof;
- two-generation bootstrap in which compiler A emits compiler B as EMBM v2 and
  B compiles/runs tests correctly;
- bundled self-hosted compiler preview built by the production executable
  bundler.

The self-hosted path validates several earlier architecture choices at once:
strings and I/O, aggregate ABI, dispatch identity, native symbolic relocation,
W^X loading, exceptions, closures, coroutines, and module ownership.

## 9. VST3 as an integration stress case

The VST3 wrapper was chosen because an audio callback stresses ABI correctness,
f32/f64 throughput, allocation/lock restrictions, hot reload publication, state
migration, and host integration. The implementation now includes 13 Ember DSP
examples, realtime validation/stress tests, background reload, automation,
MIDI, state/presets, and latency/tail handling.

The Phase 9 node-graph work deliberately compiles a validated acyclic graph to
Ember source instead of interpreting nodes on the audio thread. JSON
persistence and deterministic source generation are shipped; a graphical
VSTGUI editor remains a separate UI project.

## 10. Decisions that remain in force

- Native execution only; no bytecode interpreter.
- No mandatory LLVM dependency.
- Stable logical dispatch identity; no unguarded raw-pointer caching.
- Typed binding descriptors rather than declaration strings.
- C-style procedural language rather than class/vtable OOP.
- No C preprocessor or `goto`.
- Measured optimization work, not pass additions for their own sake.
- Explicit trust and capability policy at every module-loader boundary.
