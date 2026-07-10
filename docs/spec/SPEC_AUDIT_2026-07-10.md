# Ember Spec Audit — 2026-07-10 (spec errors & mis-scoped claims)

**Audited revision:** `8062195` (`Slice-escape safety stage 1: close C1/C2a (StringLit) + C2b (both classes)`), HEAD of `master`.
**Audited tree state:** working tree had pre-existing uncommitted edits (a "REDSHELL" → "call-target-provenance" rename pass across several docs); this audit reads committed HEAD code and the committed doc text. The audit itself is read-only — no `src/`, `docs/`, or test file was modified.
**Gate (confirmed green before AND after, unmodified):** `ctest` 22/22 in `buildt/`; `tests/run_lang_tests.sh buildt` → **255 passed, 0 failed, 0 skipped**.
**Scope:** read-only audit of the six `docs/spec/` docs (`BINDING_API.md`, `CODEGEN_SPEC.md`, `COMPILER_PIPELINE.md`, `MEMORY_AND_GC.md`, `SAFETY_AND_SANDBOX.md`, `TYPE_SYSTEM.md`) and the five `docs/`-root usage docs that carry spec claims (`BUNDLING_AND_EM_MODULES.md`, `HOT_RELOAD.md`, `LIFECYCLE.md`, `MODULES.md`, `ROADMAP.md`), against the committed `src/` implementation. Findings are claims the spec makes that are (a) factually wrong against the code, (b) mis-scoped — a "not done / out of scope" claim that should be in scope, or (c) internally inconsistent across docs.
**Style:** follows `docs/audit/AUDIT_2026-07-09.md` — numbered findings, severity table, exact doc + line + claim, evidence against it, recommended fix (doc change vs. implementation change vs. both).

## Severity legend

| Severity | Meaning |
|---|---|
| Critical | Spec claim materially misrepresents a security/safety boundary or a shipped-vs-deferred status in a way that would lead a host author to write unsafe code or a maintainer to re-open a closed gap. |
| High | Spec claim is factually wrong against the implementation OR a "out of scope" claim that should be in scope for a safe v1, with a concrete code-contradiction. |
| Medium | Spec claim is stale (describes pre-shipped behavior) or under-states an open hole, with no immediate safety regression but real risk of confusion. |
| Low | Internal inconsistency or wording drift with low practical impact. |

## Finding summary

| ID | Severity | Doc | One-line claim |
|---|---|---|---|
| F1 | High | `BUNDLING_AND_EM_MODULES.md` §1.3 (:122), §4 (:517), `MODULES.md` §6 (:296) | "No visibility / pub / export — everything included is visible" / "pub/priv is a language extension, not a bundling concern" — mis-scoped; pub/priv visibility IS a bundling concern. |
| F2 | High | `BUNDLING_AND_EM_MODULES.md` §2 (Part 2), `CODEGEN_SPEC.md` §15, `SAFETY_AND_SANDBOX.md` §1 | `.em` is raw x86-64 native code (code-injection risk); the spec frames this as an acceptable "ABI/process-trusted" tradeoff with no in-scope fix. Should be in scope. |
| F3 | High | `MEMORY_AND_GC.md` §4 (:88-90) | "v1 global storage supports scalar/handle globals only… Sema rejects slice, fixed-array, and by-value struct globals; a typed aggregate global layout remains deferred" — factually wrong; aggregate globals shipped 2026-07-10. |
| F4 | Medium | `MEMORY_AND_GC.md` §3 (:74-77), `COMPILER_PIPELINE.md` §4 (:326-333) | Slice-escape provenance check is described as a single complete mechanism blocking "the two escape routes (return, global-store)"; call-arg passing is called "perfectly fine." C3 (native arg) and C5 (script-fn arg) are an unguarded open hole. |
| F5 | Medium | `TYPE_SYSTEM.md` §9 (:258-268) | `auto` presented as the working "one type-inference rule" of v1 with no deprecation note; `auto` was deprecated 2026-07-10 (commit `d852160`). |
| F6 | Medium | `ROADMAP.md` (:43-44) | "The current tree configures 20 CTest tests when the AngelScript SDK is present and 19 when it is absent" — stale; the current tree configures 22 (the struct/aggregate pass added `array_lit` + `aggregate_global`). |
| F7 | Low | `MEMORY_AND_GC.md` §6 (:159-168) vs `ROADMAP.md` string-encryption entry | Memory doc's string-literal lifetime framing predates the 2026-07-10 inline-stack-XOR string-encryption lowering; the framing still holds but the "literal's rodata lives in the same code page" claim is now only one of two paths (the other is a transient stack temp). |

---

## F1 — pub/priv visibility IS a bundling concern (mis-scoped)

**Severity:** High (mis-scoped; the user raised this as a confirmed example).

**The claim (three locations, all agreeing):**

- `docs/BUNDLING_AND_EM_MODULES.md:122` (§1.3, "What `include` does *not* need"):
  > "No visibility / `pub` / `export` - everything included is visible (matches C `#include`; a private-include model is a language extension with no v1 use case)."
- `docs/BUNDLING_AND_EM_MODULES.md:517` (§4, "What this plan deliberately does *not* do"):
  > "No namespace/visibility on `include`.** All included names land in one flat module scope (`spec/COMPILER_PIPELINE.md` Section 4). `pub`/`priv` is a language extension, not a bundling concern."
- `docs/MODULES.md:296` (§6, "Shipped link grammar"):
  > "No `pub`/`export`.** Visibility remains deferred; every function in a registered module is callable cross-module, matching C `#include`'s "everything included is visible" stance (`BUNDLING_AND_EM_MODULES.md` Section 1.3) and the existing flat module scope (`spec/COMPILER_PIPELINE.md` Section 4)."

**Why it's wrong / mis-scoped (with code evidence):**

The claim asserts that pub/priv is a *language* (scope) concern, not a *bundling* (module-surface) concern, and that "everything included is visible" is the correct v1 stance. The user's position — which the code confirms — is that for a bundled `.em` module (or a JIT module exposed via the live `link` registry), the exported surface should be a **publishable, typed, restricted set**, not "every function." A module should be able to hide internal helpers.

The current export model, read from the source, is "export every function":

- `src/module_linker.hpp` `build_jit_exports(prog, module_id)` iterates **every** `fn` in `prog.funcs` and emits one `ModuleExport` per function (`exp.fn_name = fn.name; exp.slot = fn.slot; …`). There is no filter, no `pub` flag, no export-set declaration.
- `src/module_linker.hpp` `build_em_exports(mod, module_id)` iterates **every** entry in `mod.name_table` (the loaded module's name→slot directory) and emits one `ModuleExport` per name. The `name_table` is itself built by the `.em` writer from **every** function in the module (the writer writes a name-directory entry per function — `em_file.hpp` `EmModule::name_table` is "the name->slot directory," and `em_writer.cpp` populates it from the full function set).
- `src/em_loader.cpp` publishes **every** function's page to the dispatch table (`staged.dispatch[parsed.functions[i].slot_index] = entries[i]`) and exposes **every** function via `LoadedModule::entry_by_name` (a linear scan of the full `name_table`).
- The v2 "export_signature" (`em_file.hpp` `EmSignature`, `em_loader.cpp` `parse_signature` / `signatures_by_slot`) carries a canonical `Type` (return + params) **per function** — it is type metadata for sema's cross-module arg/return checking, **not** an export-restriction surface. Every function still gets a signature and is still exported.

So the implementation today has no mechanism for a module to declare "these N functions are my public API; the other M are internal helpers." A `.em` module's entire function set is its exported surface, callable cross-module by any importer. The spec's "everything included is visible" stance is an accurate description of the *current* code — but the user's point is that this is the wrong stance for a safe bundling story, and the spec's framing of pub/priv as "a language extension, not a bundling concern" actively mis-scopes it: visibility of a *bundled module's exported surface* is a bundling/linking concern (what the loader/linker honors), distinct from in-module name scoping (which is the language/namespace concern).

**Recommended fix (implementation change + doc change, both):**

1. **Doc change (near-term, honest status):** In `BUNDLING_AND_EM_MODULES.md` §1.3 and §4, and `MODULES.md` §6, replace the claim "pub/priv is a language extension, not a bundling concern" with an honest statement that v1 has *no* export restriction: every function in a bundled/linked module is exported and callable cross-module, internal helpers cannot be hidden, and this is a **tracked bundling gap** (not a language-only concern). Note the seam: in-module name scoping (namespaces, `ROADMAP.md` Tier 6) is a language concern; the *exported surface* of a bundled module is a bundling/linking concern. The two are siblings, not the same thing.

2. **Implementation change (the fix design):** Add an explicit, restricted, typed **export table** to the `.em` format and the JIT module shape, honored by the loader/linker:
   - **Format:** a new `.em` section (version bump to v3) listing the `(slot, name, canonical signature)` triples that are the module's *public* entry points. Functions not in the export table are still serialized (their code pages still exist, they still occupy dispatch slots for intra-module calls), but the loader does NOT add them to `LoadedModule::name_table` and `build_em_exports` does NOT export them. The export table is the module's declared public ABI.
   - **Source-side declaration:** a small grammar addition — `pub fn name(...) -> ret { … }` marks a function as exported; a bare `fn` is module-private (internal helper). This is the Rust/Python inversion of C's default-public. `pub` is a *declaration modifier* on the `FuncDecl`, recorded in `ast.hpp` (`FuncDecl::is_exported`), threaded through sema into the `EmModule::exports` set the writer serializes. No new type, no new scope concept — just a per-function visibility bit that the bundler/linker consumes.
   - **Sema/loader change:** `build_jit_exports` / `build_em_exports` filter to `is_exported` (JIT) / the export-table entries (`.em`). Cross-module `mod::fn` resolution in sema rejects a name that is not in the target's export table ("fn 'helper' is private to module 'mod'"). The loader's `entry_by_name` resolves only against the export table, not the full slot set.
   - **What this does NOT require:** no namespaces, no in-module scoping change, no type-system change. `pub` is purely an export-surface declaration consumed at the bundle/link boundary. It is exactly a bundling concern, as the user says.

   This is a real implementation change (format version bump + grammar + sema + loader + linker), not a doc-only fix. The doc change (#1) should land first to make the gap honest; the implementation change (#2) is the remediation.

---

## F2 — `.em` modules are raw x86-64 code (code-injection risk); spec frames the risk as out-of-scope

**Severity:** High (mis-scoped; the user raised this as a confirmed example).

**The claim:**

- `docs/BUNDLING_AND_EM_MODULES.md` §2.1 ("Decision: Option B"): `.em` is "serialized native code + relocations" — "Serialize the already-emitted x64 bytes per function + the dispatch slot table + the globals block + a small relocation table." §2.2 format: `code : bytes[code_size]` with the comment `"(raw x64, position-independent EXCEPT for the reloc'd imm64s)"`.
- `docs/spec/CODEGEN_SPEC.md` §15.7 (`.em` round-trip): "a JIT-built function is serialized to a `.em` file, reloaded, and the loaded code runs identically to the JIT'd version - one execution path."
- `docs/BUNDLING_AND_EM_MODULES.md` §2.5.1 (:455-461) and `src/em_loader.hpp` (:44-52) frame the safety posture: "This is still native code, not a sandbox or an authenticated untrusted-code container" / "v1 `.em` contains native x86-64… Loading is therefore an ABI/process-trusted operation, not validation of an untrusted or portable code container."
- `docs/spec/SAFETY_AND_SANDBOX.md` §1 (:8-18): the threat model scopes to "script source is untrusted-but-not-hostile-network-input" and explicitly disclaims "sandboxing arbitrary internet-supplied bytecode." The threat model is silent on `.em` as a distinct attack surface.

**Why it's mis-scoped (with code evidence):**

(a) **What the `.em` format actually is today.** `src/em_file.hpp` defines the on-disk struct: `EmFunctionRecord::code` is `std::vector<uint8_t>` — the post-`resolve_fixups` x64 byte vector. `src/codegen.cpp`'s finalize (the `finalize` path ending at `:out.bytes = std::move(cg.e.code); out.rodata = std::move(cg.rodata);`) produces `CompiledFn::bytes` = the raw x64 instruction stream. `src/em_writer.cpp` serializes `f.code` directly as the `code` payload. `src/em_loader.cpp` reads it back: `f.code.assign(code, code + code_size);` then `alloc_executable_rw(combined)` (RW, never RWX), `memcpy`-equivalent placement of the code bytes into the exec page, reloc patching, then `seal_executable(page, …)` (RW→RX W^X). The loaded page is then called via the dispatch table as `int64_t(*)(int64_t)`-shaped host calls. **Confirmed: a `.em` function record is arbitrary x86-64 that gets mapped executable and called, with the host's privileges.**

(b) **The real attack surface.** A malicious `.em` is arbitrary native x86: the loader does NOT disassemble, validate, or re-derive the bytes — it `memcpy`s them and makes the page RX. The only pre-publication validation is structural (magic, version, bounded counts/sizes, reloc offsets in range, canonical-type shape, native-binding signatures match the host allowlist). None of that inspects the *code bytes*. A `.em` with a valid header, valid build_id/target_abi, valid type signatures, and valid native-binding names/signatures still injects arbitrary x86 that runs with the host's privileges the moment the dispatch slot is called. The 07-09 audit's C1 (reloc-offset OOB write) and C2 (unbounded allocations) were loader-robustness defects on top of this; the underlying surface — "`.em` is native code that runs" — is by design, and the spec's "ABI/process-trusted" framing concedes it.

(c) **Does ember have a serializable IR today?** No. `src/codegen.hpp` line 1-2: "codegen - tree-walking AST -> x86-64… a simple stack-spilling tree-walker (correctness first)." `src/codegen.cpp` `struct CG` walks the AST directly and emits x64; there is no `IrFunction`/`IrInstr`/`IrValue`/`BasicBlock`/`run_linear_scan`/`emit_x64` in the codegen source (verified by grep). `docs/spec/CODEGEN_SPEC.md` §5 (line 238) and `docs/spec/COMPILER_PIPELINE.md` §5 both mark "SSA-lite" IR as **deferred** ("Not implemented in v1.0. The shipped backend walks the AST directly and stack-spills expression intermediates. Everything in this section is a future, benchmark-gated design"). So an "IL-`.em`" (a byte representation of an intermediate representation the loader re-validates and re-lowers) would require a **new serializable IR pass between sema and codegen** — it does not exist today.

(d) **What "byte representation of the lexer IL" would mean concretely.** It would mean: after sema, lower the typed AST to the SSA-lite IR (`COMPILER_PIPELINE.md` §5's `IrFunction` — blocks, instrs, vreg types), serialize *that* IR to `.em` instead of the x64 bytes, and have the loader re-run `run_linear_scan` + `emit_x64` (`COMPILER_PIPELINE.md` §8) to re-emit x64 at load time, after re-validating types and structure. The loader becomes a codegen.

(e) **The tradeoff.** An IL-`.em` that the loader re-lowers to x86 means the loader IS a codegen (re-verifies types, re-emits x64) — that is a *bigger* and more attack-relevant surface than the current "memcpy + reloc" loader, because a bug in the loader-side regalloc/emit now produces wrong native code from otherwise-valid IR, and the loader must ship the entire regalloc+emit backend. The BUNDLING doc §2.1 Option-A Con calls this out precisely: "the loader must ship the entire regalloc+emit backend… the only real win is 'no parser/sema on load.'" Re-validating IR types at load is real defense-in-depth (a malicious IR can be type-checked and structurally rejected before any x64 is emitted), but it is not a sandbox — a valid-but-malicious IR that passes type checks still lowers to x64 that runs with host privileges. The reduction is "malicious x64 bytes → malicious IR that must survive type/structure validation before becoming x64," not "malicious input → no native execution."

(f) **What the v2 "export_signature" actually verifies today.** The BUNDLING doc §2.5.1 (:448-450) says v2 "stores canonical `Type`-based export signatures." Read against `src/em_file.hpp` + `src/em_loader.cpp`: `EmSignature` is a `Type ret` + `vector<Type> params` — a **type** signature for sema's cross-module arg/return checking (`canonical_type_same` in `em_loader.cpp`), NOT a cryptographic signature of the code bytes. The v2 `EM_BUILD_ID` / `EM_TARGET_ABI_HASH` (`em_file.hpp`) are FNV1a-64 hashes of *compiler/arch/OS/CC string literals* (`"ember-em-v2;gcc=…"` / `"arch=x86_64;ptr=64;os=windows;cc=win64-x64;…"`), computed at compile time from `__GNUC__`/`_MSC_VER`/`_M_X64`/`_WIN64` macros — they are **identity** checks (did this `.em` come from a compatible toolchain/target), NOT **content** authentication. A malicious `.em` produced by the same compiler/ABI carries a valid build_id/abi_hash + valid type signatures and still injects arbitrary x86. **There is no cryptographic integrity check on the `.em` code bytes today.**

**Recommended fix (implementation change; doc change to make the gap honest):**

The user asks for a recommendation among: IL-`.em` (re-lowered + re-validated at load), OR signed raw-x86 `.em` (cryptographic integrity, no re-lowering), OR both. Recommendation:

- **Primary: signed raw-x86 `.em` (cryptographic integrity).** This is the smaller, more defensible fix for v1. Add a cryptographic signature (e.g. Ed25519 over the SHA-256 of the code+rodata+reloc+globals+name-table bytes, keyed by a host-supplied public key) to the v3 `.em` header. The loader verifies the signature *before* `alloc_executable_rw` and rejects on mismatch. This closes the code-injection surface for the threat model that matters (a `.em` that did not come from the host's trusted toolchain) without shipping a regalloc/emit backend in the loader, without a new IR pass, and without changing the execution path. The build_id/abi_hash stay as the compatibility check; the signature is the integrity check. Cost: a small crypto dependency (or a vendored Ed25519 impl, ~1k LoC) + a host key-management story. This is the standard "signed native artifact" pattern (analogous to signed DLLs / signed firmware images).
- **Secondary, defense-in-depth: IL-`.em` as a future option.** An IL-`.em` that the loader re-lowers after re-validation is genuine defense-in-depth (reject malformed IR before any x64 exists), but it requires the deferred SSA-lite IR to ship first (`COMPILER_PIPELINE.md` §5/§8) AND it makes the loader a codegen. Defer this until/unless the IR ships AND a concrete threat (e.g. a signed-`.em` bypass) justifies the larger loader surface. Record it as a tracked option in BUNDLING §2.1, not a v1 commitment.
- **Both is not recommended for v1** — signing is the load-bearing fix; the IL re-lower is additive hardening that should wait for its own trigger.
- **Doc change (make the gap honest now):** `SAFETY_AND_SANDBOX.md` §1's threat model should explicitly name `.em` as a distinct, stronger attack surface than script source (it is native x86, not text), and state that v1's `.em` integrity guarantee is **identity + type-signature compatibility, not content authentication** — a `.em` from an untrusted source is a code-injection vector, and hosts must treat `.em` provenance as a host trust decision (only load `.em` from a trusted toolchain or after signature verification, once shipped). `BUNDLING_AND_EM_MODULES.md` §2.5.1 should say the same. This is a doc-only change that can land immediately; the signature implementation is the remediation.

---

## F3 — "Sema rejects aggregate globals" is factually wrong (stale)

**Severity:** High (factually wrong against the implementation).

**The claim:**

`docs/spec/MEMORY_AND_GC.md` §4 (:88-90):
> "v1 global storage supports scalar/handle globals only: one eight-byte slot per declaration. Sema rejects slice, fixed-array, and by-value struct globals; a typed aggregate global layout remains deferred."

**Why it's wrong (with code evidence):**

Aggregate globals shipped 2026-07-10 (commit `9e90cf8`, ROADMAP "SHIPPED — first-class struct / aggregate"). The implementation accepts them:

- `src/sema.cpp:503-505` (the global-declaration check): "Aggregate globals (struct / fixed-array / slice) are accepted in v1 (chunk c3): a typed globals-block layout + load-time const folding of aggregate initializers backs them."
- `docs/spec/TYPE_SYSTEM.md` §12.2 (the shipped spec for the same feature) documents the typed global-table layout: "A `struct`, fixed-`array`, or `slice` typed global now type-checks, initializes, loads, and stores… the per-global table carries an offset and size per global, with 8-byte alignment… A slice global occupies 16 bytes… a struct global occupies its tightly-packed Ember size… a fixed-array global occupies `N * sizeof(T)`." This is the *opposite* of MEMORY_AND_GC §4's "one eight-byte slot per declaration… Sema rejects slice, fixed-array, and by-value struct globals."
- `docs/spec/CODEGEN_SPEC.md` §16.4 documents the matching codegen: "The globals block is no longer a flat `i64[]` of 8-byte scalar slots; it is a typed block with per-global offset and size."
- Regression: `aggregate_global_test` (ctest target `aggregate_global`, the +1 test that took the gate from 20→22), probes [1]-[8] pin struct/array/slice global read + by-value arg/return + `.em` round-trip (slice relative-ptr relocation).

So MEMORY_AND_GC.md §4 is stale: it describes the pre-2026-07-10 scalar-only globals model, while two other spec docs (TYPE_SYSTEM §12.2, CODEGEN_SPEC §16.4) and the implementation agree aggregate globals ship. This is an internal inconsistency as well as a factual error.

**Recommended fix (doc change):** Update `MEMORY_AND_GC.md` §4 to describe the shipped typed-globals-block model (per-global offset + size, 8-byte alignment, slice=16 bytes, struct=tightly-packed size, fixed-array=`N*sizeof(T)`), and cross-reference `TYPE_SYSTEM.md` §12.2 and `CODEGEN_SPEC.md` §16.4 for the full contract. Remove "Sema rejects slice, fixed-array, and by-value struct globals; a typed aggregate global layout remains deferred." The "Global initializer" paragraph that follows (:92-95) also needs updating: it says "evaluated once at `ember_compile` time… a memcpy of constant bytes" — the shipped path folds aggregate initializers at *load* time via `eval_global_initializers` (per TYPE_SYSTEM §12.2's "const-initializer-folding rule… evaluated at load time by the host's `eval_global_initializers`"), not at compile time. Align the two.

---

## F4 — Slice-escape provenance check under-states the open hole (C3/C5 unguarded)

**Severity:** Medium (under-states an open hole; the ROADMAP already tracks Stage 2 as deferred, but the spec presents the closed part as the whole mechanism).

**The claim:**

- `docs/spec/MEMORY_AND_GC.md` §3 (:74-77):
  > "Passing such a slice as an argument to a call made *from* the current frame is fine (callee's frame is nested strictly inside the caller's - the local array is still alive for the callee's entire execution) - the provenance tag only blocks the two escape routes (return, global-store) that could outlive the frame, not ordinary downward parameter passing."
- `docs/spec/COMPILER_PIPELINE.md` §4 (:326-333): describes the "Slice-escape provenance check" as running on "return statements and global-assignment statements specifically (the two escape routes)."

**Why it under-states the open hole (with code evidence):**

The spec presents call-arg passing as unconditionally safe ("is fine," "callee's frame is nested strictly inside the caller's lifetime") and frames the provenance check as a single complete mechanism blocking "the two escape routes (return, global-store)." The actual implementation is a **5-category** escape analysis (C1 return, C2a global-Ident-store, C2b global-rooted FieldExpr/IndexExpr-store, C3 native-arg, C5 script-fn/fn-handle/cross-module-arg), of which Stage 1 (commit `8062195`, HEAD) closed C1/C2a/C2b for both the `ViewExpr`-over-fixed-array class and the `StringLit`-derived-`slice<u8>` class, and C3/C5 are explicitly **open**:

- `src/sema.cpp` `is_local_array_view` (line :628) is consumed only at: LetStmt init (:1657, declares the bit), ReturnStmt (:1668, C1), AssignExpr Ident target (:1315, C2a), AssignExpr FieldExpr/IndexExpr target (:1329, C2b). There is **no** consumption at the `CallExpr` native-arg loop or the script-fn/fn-handle/cross-module-arg paths (verified by grep — `is_local_array_view` appears at exactly those 5 sites).
- `src/sema.cpp:605-625` (the `is_local_array_view` comment block) explicitly states: "STAGE 2 (deferred): C3 (a stack-backed slice passed to a NATIVE that may retain the ptr) and C5 (a stack-backed slice passed to a script fn / fn-handle / cross-module call that may retain it) are NOT guarded at the call-arg sites. A blanket reject there was rejected because it breaks the legitimate synchronous pattern `return_slice_defer(return_values[..])`… Closing C3/C5 needs a real borrow/escape analysis."
- `docs/ROADMAP.md` (the "Slice-of-stack-local escape safety" entry) records this honestly: "STAGE 1 DONE (2026-07-10), STAGE 2 DEFERRED… C3… and C5… are NOT yet guarded… No shipped native retains a slice ptr today, so C3 is 'accidentally safe'; C5 (a retaining script fn) is the residual live hole."

The spec's "the two escape routes (return, global-store)" is now an accurate description of the *closed* set, but the spec does not mention that a third escape route — a callee that *retains* the ptr past the frame's lifetime (stores it to a global, or holds it in a host data structure) — is an open hole at the call-arg sites. The spec's "passing such a slice as an argument to a call… is fine" is only true for a callee that does not retain; a retaining callee (C5 for script fns, C3 for natives that retain) dangles the ptr. The spec presents the safe case as the only case.

This is not a regression the spec introduced — the ROADMAP tracks Stage 2 as deferred — but the spec docs (MEMORY_AND_GC §3, COMPILER_PIPELINE §4) describe the mechanism as more complete than it is, and assert call-arg passing is unconditionally fine when it is conditionally fine (fine for non-retaining callees, an open hole for retaining ones).

**Recommended fix (doc change):** In `MEMORY_AND_GC.md` §3 and `COMPILER_PIPELINE.md` §4, add a Stage-2 note: the provenance check currently blocks C1 (return) and C2a/C2b (global-store, including global-rooted field/element store), for both the `ViewExpr`-over-fixed-array class and the `StringLit`-derived-`slice<u8>` class. **C3** (a stack-backed slice passed to a native that may retain the ptr) and **C5** (a stack-backed slice passed to a script fn / fn-handle / cross-module call that may retain it) are **not yet guarded** at the call-arg sites — closing them needs a real borrow/escape analysis (propagate the localview bit through a call's return value, reject only at the actual escape point, and add a `borrows`/`retains` annotation to `NativeSig` so C3 can distinguish copying natives like `string_from_slice` from retaining ones). Soften "passing such a slice as an argument to a call… is fine" to "is fine for a callee that does not retain the ptr past the frame; a retaining callee is the open C3/C5 hole (Stage 2 deferred, see ROADMAP)." Cross-reference the ROADMAP entry and `demo/SLICE_ESCAPE_SAFETY_INVESTIGATION.md` for the 5-category matrix. This is a doc-only change; the C3/C5 implementation is the Stage-2 remediation the ROADMAP already tracks.

---

## F5 — `auto` presented as a working v1 feature, no deprecation note (stale)

**Severity:** Medium (stale; the deprecation shipped, but the spec for the feature does not record it).

**The claim:**

`docs/spec/TYPE_SYSTEM.md` §9 (:258-268):
> "`auto` is allowed **only** as a local-variable declaration's type, and only when the declaration has an initializer: `auto x = expr;` infers `x`'s type as exactly the type of `expr`… This is deliberately the smallest possible inference rule… matches 'monomorphic types only so simple.'"

The section presents `auto` as a current, non-deprecated v1 feature — the "one type-inference rule" — with no deprecation note.

**Why it's stale (with code evidence):**

`auto` was deprecated 2026-07-10 (commit `d852160` "Deprecate 'auto' (redundant with let-inference); fix the false doc claim"). The implementation emits a non-fatal deprecation warning:

- `src/sema.cpp:1600-1604`: "`auto` is deprecated: it's a redundant spelling of `let x = expr;` inference (both share the is_auto path; `let x = expr;` is the canonical inference form… `warn('auto' is deprecated; use 'let x = expr;' for inference or 'let x: T = expr;' for an explicit type', …)`".
- `docs/ROADMAP.md` "Slated for removal (deprecated)" section: "`auto x = expr;` — deprecated (2026-07-10). A redundant spelling of `let x = expr;`… Migrate any `auto x = e;` to `let x = e;`. Removed from the in-tree tests; remove from the language after a grace period."
- `docs/spec/COMPILER_PIPELINE.md` §2 (the grammar note) DOES record the deprecation: "`auto x = expr;` is a **deprecated** redundant spelling of `let x = expr;`… `auto` is kept working but emits a deprecation warning and is slated for removal."

So COMPILER_PIPELINE §2 is correct, but TYPE_SYSTEM §9 — the dedicated section that defines the inference rule — still presents `auto` as the working "one type-inference rule" with no deprecation note. A reader who lands on TYPE_SYSTEM §9 (the authoritative type-system section for the feature) gets the stale, pre-deprecation framing.

**Recommended fix (doc change):** Update `TYPE_SYSTEM.md` §9 to lead with the deprecation: "`auto` is **deprecated** (2026-07-10); it is a redundant spelling of `let x = expr;` (both share the same inference path; `auto` emits a non-fatal deprecation warning and is slated for removal — see ROADMAP). The canonical forms are `let x = expr;` (inference) and `let x: T = expr;` (explicit). The inference rule below is preserved for reference and because `let`-inference shares it." Then keep the existing rule text (it accurately describes the shared inference path). Cross-reference COMPILER_PIPELINE §2's grammar note. This is a doc-only change; the deprecation already shipped in code.

---

## F6 — "20 CTest / 19" count is stale (current tree is 22)

**Severity:** Medium (stale; describes the tree as it was before the struct/aggregate pass).

**The claim:**

`docs/ROADMAP.md` (:43-44):
> "The current tree configures 20 CTest tests when the AngelScript SDK is present and 19 when it is absent (only the benchmark is conditional)."

**Why it's stale (with code evidence):**

The 2026-07-10 first-class struct/aggregate pass (commit `9e90cf8`) added two tests — `array_lit` and `aggregate_global` — taking the gate from 20 to 22. `docs/ROADMAP.md` itself records this later in the same file (:437): "ctest 22/22 (was 20, +1 `array_lit`, +1 `aggregate_global`…)." The current gate (confirmed green for this audit): `ctest` 22/22 in `buildt/`. The :43-44 sentence describes the "current tree" but the current tree is 22, not 20.

**Recommended fix (doc change):** Update the :43-44 sentence to "The current tree configures 22 CTest tests when the AngelScript SDK is present and 21 when it is absent (only the benchmark is conditional)." (Verify the absent-SDK count — the struct pass added two unconditional tests, so 19→21 absent-SDK; confirm against a no-SDK configure before committing, since this audit ran against a tree with the SDK present.) This is a doc-only change.

---

## F7 — String-literal lifetime framing predates the inline-stack-XOR encryption lowering (minor drift)

**Severity:** Low (the framing still holds for one of two paths; the other path is unmentioned).

**The claim:**

`docs/spec/MEMORY_AND_GC.md` §6 (:159-168):
> "A string literal in script source (`"hello"`) is emitted as a `RipFixup`-referenced rodata blob (CODEGEN_SPEC.md Section 4) embedded in the *literal's containing function's* compiled code - i.e. string literals are function-local rodata, valid for exactly as long as that function's code page is alive… a literal-derived slice is **not** subject to Section 3's dangling-check; only locally-computed `arr[..]` views of stack data are."

**Why it's drift (with code evidence):**

The 2026-07-10 string-encryption pass (commit `e98dc87`, ROADMAP "Runtime string encryption — DONE") changed how an *encrypted* string literal is lowered: "an encrypted string literal is now decrypted inline into a compiler-hidden temp frame slot at each use site (see codegen's StringLit eval case / `alloc_str_temp` / `count_str_temps_block`). The plaintext is TRANSIENT — it lives only on the caller's stack frame for the expression's lifetime and is reclaimed at frame teardown." So when `Program::string_xor_key != 0` (the CLI default is now `0xA5`, ON by default), an encrypted string literal's bytes are NOT function-local rodata — they are a transient stack temp (the encrypted bytes are in rodata, but the *plaintext* the slice points at is the stack temp). The §6 claim "a string literal… is emitted as a RipFixup-referenced rodata blob… string literals are function-local rodata" describes the unencrypted path; the encrypted path is a different lifetime shape (stack temp, reclaimed at frame teardown).

This is not a safety regression — the slice-escape Stage-1 fix (F4) explicitly covers `StringLit`-derived `slice<u8>` via `is_local_array_view` returning true for a `StringLit` whose `ty->is_slice` (sema.cpp:640-642), so the encrypted-path stack temp IS now subject to the §3 dangling check (the ROADMAP records this: "Stage 1… (1) `is_local_array_view` now covers a `StringLit` whose resolved type is `slice<u8>`"). But §6's "a literal-derived slice is **not** subject to Section 3's dangling-check" is now too broad — it is true for the unencrypted rodata path and false for the encrypted stack-temp path (which IS now checked).

**Recommended fix (doc change):** In `MEMORY_AND_GC.md` §6, add a note that the string-encryption lowering (ROADMAP, 2026-07-10) adds a second path: when `Program::string_xor_key != 0`, an encrypted string literal is decrypted inline into a compiler-hidden stack temp at each use site, so the *plaintext* the slice points at is a transient stack slot, not function-local rodata. A `slice<u8>` derived from such an encrypted literal IS subject to §3's dangling check (the Stage-1 slice-escape fix covers it via `is_local_array_view`). The unencrypted path remains rodata-backed and exempt as §6 states. Cross-reference the ROADMAP string-encryption entry and `demo/STRING_ENCRYPTION_ANALYSIS.md`. This is a doc-only change.

---

## Cross-checks performed (no finding, recorded for completeness)

The following spec claims were checked against the source and found **accurate** (not recorded as findings):

- `docs/spec/CODEGEN_SPEC.md` §5 / `docs/spec/COMPILER_PIPELINE.md` §5 — the SSA-lite IR + linear-scan regalloc is marked deferred; `src/codegen.hpp` line 1-2 confirms codegen is a tree-walking stack-spilling emitter with no `IrFunction`/`run_linear_scan`/`emit_x64`. Accurate. (Relevant to F2's finding that no serializable IR exists today.)
- `docs/spec/SAFETY_AND_SANDBOX.md` §7a — the call-target-provenance guard (bitset allowlist, `emit_call_target_guard`, `BadCallTarget` trap) is implemented per `src/codegen.cpp` / `src/sema.cpp`; the bare-`fn` signature hole and cross-module-handles deferral are documented honestly in ROADMAP Tier 2. Accurate.
- `docs/spec/BINDING_API.md` (the "v0.3 working binding API" block + the deferred `TypeBuilder`/`StructBuilder`/`EnumBuilder` surface) — the shipped API is `src/binding_builder.hpp`'s `BindingBuilder`; the fluent `TypeBuilder` surface is correctly labeled deferred. Accurate.
- `docs/BUNDLING_AND_EM_MODULES.md` §1.4 (:136) "Single-function reload (`reload_function`): **changed in v1.0**" — the 07-10 scrutiny report's M-M3-1 found this was stale ("unchanged") and it was remediated; the current text correctly says "changed in v1.0" and gives the `HotReloadDomain&` migration recipe. Accurate. (F1 and F2 are the NEW bundling-doc findings the user asked about; the previously-remediated stale claims are not re-reported.)
- `docs/MODULES.md` §5 / §8 v1/v2 signature text — the 07-10 scrutiny report's M-docs-1 found the v1/v2 signature text was stale and it was remediated; the current text correctly distinguishes v1 ABI-trusted `unknown_sig` from v2 canonical `Type` signatures + build/ABI identity. Accurate. (F2 is about the *missing* cryptographic content-authentication, which is a different gap than the v1/v2 type-signature text that was already fixed.)
- `docs/spec/CODEGEN_SPEC.md` §8 / §16 + `docs/spec/TYPE_SYSTEM.md` §12 — native struct-by-value >8 bytes is rejected by sema; the struct-literal-return / struct-by-value-arg temp lowering and aggregate-global codegen are documented consistently with `src/codegen.cpp`'s `alloc_struct_temp` / `stash_struct_arg` / typed-globals paths. Accurate.
- `docs/HOT_RELOAD.md` — the `HotReloadDomain` epoch-reclamation model, the `old_entry` removal, the guard-before-every-outer-call recipe, and the duplicate-current-page-alias rejection are documented consistently with `src/hot_reload.hpp` and the 07-10 scrutiny M-M3-2 remediation. Accurate.

---

## Recommended remediation order

1. **F1 doc change** (make the pub/priv gap honest in BUNDLING §1.3/§4 + MODULES §6) — lands immediately, zero source risk. F1 implementation (export table + `pub` modifier + loader/linker filter) is the larger remediation, scheduled after review.
2. **F3 doc change** (update MEMORY_AND_GC §4 to the shipped typed-globals model) — lands immediately; it is a pure stale-text fix that aligns MEMORY_AND_GC with the already-shipped TYPE_SYSTEM §12.2 / CODEGEN_SPEC §16.4.
3. **F5 doc change** (lead TYPE_SYSTEM §9 with the `auto` deprecation) — lands immediately.
4. **F6 doc change** (20→22 CTest count) — lands immediately; verify the absent-SDK count first.
5. **F4 doc change** (add the C3/C5 Stage-2-deferred note to MEMORY_AND_GC §3 + COMPILER_PIPELINE §4) — lands immediately; the C3/C5 implementation is the Stage-2 remediation the ROADMAP already tracks.
6. **F7 doc change** (add the encrypted-string stack-temp path to MEMORY_AND_GC §6) — lands immediately.
7. **F2 doc change** (name `.em` as a distinct code-injection surface in SAFETY §1 + BUNDLING §2.5.1; state the v1 guarantee is identity + type-signature compatibility, not content authentication) — lands immediately.
8. **F2 implementation** (signed raw-x86 `.em` — Ed25519 over SHA-256 of the code/rodata/reloc/globals/name-table bytes, verified before `alloc_executable_rw`) — the load-bearing security remediation, scheduled after review. F2 secondary (IL-`.em` re-lowering) is deferred until the SSA-lite IR ships AND a concrete threat justifies the larger loader surface.

Doc changes 1, 3, 4, 5, 6, 7 are all pure stale-text / under-statement fixes with no source side effect and no gate impact. F1-impl and F2-impl are the two real implementation changes; both are scheduled for after-review, not part of this read-only audit.
