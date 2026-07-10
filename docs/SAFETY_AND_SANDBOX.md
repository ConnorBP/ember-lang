# ember - safety & sandbox spec

Detail doc for DESIGN.md Section 5. Every "what happens when X goes wrong"
case, the shipped safety boundary, and explicitly deferred mechanisms.

## 1. Threat/trust model (explicit, so nothing is assumed)

- **Script source is untrusted-but-not-hostile-network-input**  - 
  the target use case is game mods/gameplay scripts, not sandboxing
  arbitrary internet-supplied bytecode against a malicious author
  trying to escape the process. Goals are: (a) a buggy script cannot
  corrupt host memory or crash the process, (b) a buggy or slow
  script cannot hang the game loop forever, (c) a script cannot call
  host functionality it wasn't explicitly given access to. Goals are
  **not**: defending against a script author who has also compromised
  the host binary, or side-channel/speculative-execution-class attacks.
  This scope matches the surveyed native-JIT language's and AngelScript's actual guarantees
  (RESEARCH_NOTES.md) - we are not claiming a stronger sandbox than
  either.
- **Native functions are trusted.** Once a host registers a
  `NativeFn`, ember assumes it behaves per its declared signature
  (correct arg count/types honored by the host implementation, does
  not itself corrupt memory). Ember cannot and does not verify native
  function *bodies* - same trust boundary as AngelScript/the surveyed native-JIT language
  bindings.

## 2. Execution entry point & non-local abort mechanism

The shipped `ember_call_void` and `ember_call_i64` functions are **raw B1
thunks**: they install the caller's `context_t*` in `r14`, invoke the supplied
entry pointer, and preserve the incoming nonvolatile `r14`. They do not reset
the context and do not establish a recoverable resume point.

A host requiring recoverable traps must set `has_checkpoint`, establish the
`__builtin_setjmp` checkpoint, invoke the raw thunk, and clear the checkpoint,
as `examples/ember_cli.cpp` and the safety tests do. A host may wrap that
pattern in its own safe-call API. A trap stub records the reason and performs
`__builtin_longjmp` to that host-managed checkpoint. `context_t` stores one
checkpoint; a nested checkpoint stack and a core safe-call wrapper remain
deferred. Calling trap-capable code through a raw thunk without a checkpoint
is not recoverable.
- **Trap during a native call made from script** (host code called by
  script itself faults or throws a real C++ exception): this is
  outside ember's control by definition (Section 1, native code is trusted)
  - a C++ exception thrown across the native->script->native boundary
  is **undefined by ember, documented as the host's responsibility**
  to not do (native bindings should not throw; if they must, they
  should catch internally and translate to `runtime_exception()`,
  Section 7). Ember does not install a top-level `catch(...)` around JIT'd
  code in v1 (would add per-call-site cost, and can't safely resume
  code mid-frame after a foreign exception regardless) - this is
  documented as a known boundary condition, not silently handled.

## 3. Instruction budget

- **Mechanism (implemented M4 model)**: `context_t` holds a single
  `int64_t budget_remaining` counter. Codegen's recursive
  `block_cost(function.body)` estimator is charged once at every function
  entry, after the frame, hidden return pointer, and parameters are safely
  established and before annotations/body execution. Existing
  body-cost charges on taken loop back-edges (`while`, `for`, and
  `do-while`) remain, so repeated work continues to consume budget. This is
  a coarse static statement-cost model, not a cycle or exact instruction
  count; native function bodies are trusted and are not assigned a cost.
  ```
  sub  [budget_ptr], coarse_cost_imm32
  jg   .continue            ; budget_remaining > 0, keep going
  call [ember_trap_budget_slot]   ; does not return (checkpoint unwind, Section 2)
  .continue:
  ```
  The recursive estimator and encoding saturate at `INT32_MAX`; a large
  legal function can never truncate into a negative or small `imm32` charge.
  In B1 mode the counter is addressed through the per-call `r14` context.
- **Exhaustion behavior**: traps via the shared budget-trap stub,
  same non-local unwind as bounds/div-by-zero (Section 2). The host's
  checkpoint wrapper resumes and observes "budget exceeded" through
  `last_error`/`last_trap`.
  Script does **not** get a chance to catch this - no in-language
  exception handling exists (DESIGN.md Section 2), matches "abort, don't try
  to let a runaway script gracefully handle its own runaway-ness."
- **No budget set (default)**: `budget_remaining` starts at
  `INT64_MAX` (or budget checks are simply compiled out entirely if
  the module was compiled with budgeting disabled - a module-level
  compile flag, since baking the checks in costs a `sub`+`jg` at entry
  and per loop iteration; hosts that don't need budgets
  at all, e.g. a fully-trusted internal tool script, can compile
  without the checks for zero overhead - explicit opt-in cost). The entry
  charge and every back-edge charge are both omitted when disabled.
  Enabling/disabling is decided at `ember_compile` time via an engine
  flag, not per-call, since it changes what code is emitted.
- **Wall-clock budget** (optional, in addition to instruction count):
  v1 does **not** implement a wall-clock deadline check compiled into
  the code (would need a syscall-class clock read on a hot path,
  expensive) - instead, wall-clock limiting is a **host
  responsibility** achieved by setting a conservative instruction
  budget calibrated to the host's frame-time target, exactly the
  surveyed native-JIT language's model. A true preemptible wall-clock timeout (e.g. via a
  watchdog thread that can force-unwind another thread's execution)
  is explicitly deferred - cross-thread forced unwind of running JIT
  code is a hard problem (needs safe-points, thread suspension,
  register-state inspection) with no current use case justifying the
  cost; instruction budgets are the v1 answer, documented as an
  approximation of time.

## 4. Combined script/native call-stack depth guard

- **Mechanism (implemented M4 model)**: `context_t` holds `int32_t
  call_depth` and a configured `max_call_depth`. Every script-issued call
  increments/checks before invocation and decrements after normal return:
  ordinary and indirect script calls, cross-module script calls, ordinary
  native calls, hidden-pointer struct-return native helpers, native operator
  overloads, and emitted string decrypt/conversion helpers. A native remains
  counted while it runs. If that native re-enters script and the re-entered
  script calls another native, those active edges compose into one combined
  stack depth. Sequential calls that return are therefore never mistaken for
  nesting or accumulated as a lifetime call count.
  ```
  inc  [call_depth_ptr]
  cmp  [call_depth_ptr], max_call_depth_imm
  jge  .depth_trap          ; does not return
  call ...
  dec  [call_depth_ptr]
  ```
  Combined with Section 3, the shipped model is **coarse function-entry plus
  loop-back-edge budget + combined script/native active-call depth**.
  Native bodies remain trusted; this counter bounds nesting across their
  script-visible boundary, not their internal C++ stack usage.
- **Why not just rely on OS guard-page stack-overflow (SEH) instead**:
  catching a real stack overflow via SEH on Windows is possible but
  leaves the stack in a state where you cannot reliably continue
  running *any* code (including cleanup) until the guard page is
  reset with `_resetstkoflw`-equivalent handling - fragile, and the
  explicit counter is one `inc`+`cmp` per call, cheaper and fully
  under our control, and gives a much better error ("call depth 512
  exceeded" vs "stack overflow, best of luck"). SEH-based overflow
  handling is a documented defense-in-depth backstop only (the host
  process should still set a reasonably sized stack; ember does not
  try to guarantee correctness if the depth limit itself is set
  absurdly high relative to actual stack size - that's a host
  misconfiguration, not an ember bug).
- **Exhaustion behavior**: same shared-trap/non-local-unwind pattern
  as Section 3. A non-local trap necessarily skips decrement instructions
  on abandoned frames. Before a later outer entry, the host must call
  `context_t::reset_for_call()` (or equivalently reset `call_depth`); that
  method explicitly clears the combined depth while preserving the host-owned
  budget. Normal returns remain balanced without host intervention.

## 5. Bounds checking

Fully specified in CODEGEN_SPEC.md Section 9 (exact instruction sequence).
Summary of the safety-relevant policy decisions:
- Always checked, in both debug and release builds - unlike overflow
  checks (Section 6), bounds checks are **never** compile-flag-disabled,
  because an out-of-bounds access is a direct host-memory-corruption
  vector (the actual security-relevant property DESIGN.md Section 5
  promises: "no raw pointer arithmetic exposed to script" is only
  true if every indexed access is actually checked, always).
- Trap, not clamp/saturate - an out-of-bounds index aborts via the
  shared trap stub (non-local unwind, Section 2), it does **not** silently
  clamp to a valid index or return a zeroed value. Silent clamping
  would hide script bugs and could produce subtly-wrong game behavior
  that's harder to diagnose than a hard stop.
- Compile-time-constant-index-into-fixed-array is checked at compile
  time with zero runtime cost (TYPE_SYSTEM.md Section 3, CODEGEN_SPEC.md Section 9)
  - this is a performance optimization, not a safety weakening, since
  the check still definitely happens, just earlier.

## 6. FFI permission gating (`PERM_FFI`)

- Every `NativeFn` registration carries a `permission` bitfield
  (BINDING_API.md Section 1); `PERM_FFI` (value `0x01`, matches the surveyed native-JIT language's
  constant, RESEARCH_NOTES.md) marks a native function as requiring
  explicit per-module opt-in to call.
- **Enforcement point**: compile time, at the call-site sema check
  (CODEGEN_SPEC.md Section 8) - if script code calls a `PERM_FFI`-gated
  native function and the compiling module wasn't granted that
  permission (`set_permissions`/`get_permissions`, mirroring the surveyed native-JIT language's
  API, DESIGN.md Section 8), it's a **compile error**, not a runtime
  check. Rationale restated from CODEGEN_SPEC.md Section 8: this makes
  permission-checking genuinely zero-cost at runtime (no branch ever
  emitted for it) while still being a real gate - a module that
  doesn't have the permission literally cannot produce code that
  calls the function, there's no "check bypassed" path to worry
  about since the call site doesn't exist in the compiled output.
- **Why gate at the module level and not finer-grained
  (per-function/per-context)**: matches the surveyed native-JIT language's model exactly
  (`set_permissions(engine, flags)` is engine-wide in the SDK surface
  we surveyed) and avoids a much more complex per-caller ACL system
  with no demonstrated game-scripting need yet - if a host needs
  finer granularity later (e.g. "this mod's module can't touch
  filesystem natives but this other trusted module can"), that's
  already expressible today by registering the sensitive natives on a
  separate `engine_t` instance used only for the trusted module's
  compilation, no new mechanism required (YAGNI).
- **Unregistered function name called from script**: ordinary compile
  error ("unknown function"), unrelated to permissions - not to be
  confused with a permission failure in error messages (different
  error codes/messages so host tooling can distinguish "this script
  tried to call something that doesn't exist" from "this script tried
  to call something it's not allowed to call").

## 7. Runtime error / exception model (non-bounds/budget/depth cases)

Native-triggerable runtime errors (a native binding decides mid-call
that something is wrong - e.g. an engine API rejecting invalid game
state) use the same non-local-unwind machinery (Section 2) via two
host-callable functions (mirrors the surveyed native-JIT language's `runtime_error`/
`runtime_exception`, RESEARCH_NOTES.md/DESIGN.md Section 8):
- `runtime_error(context_t*, const char* msg)` - records the message,
  triggers the same checkpoint-unwind as Section 3/Section 4/Section 5's traps. Intended
  for "this is definitely a bug, abort this call" situations
  (equivalent severity to a bounds-check trap).
- `runtime_exception(context_t*, const char* msg)` - same unwind
  mechanism, but recorded distinctly (`exception_pending`/
  `exception_value`/`exception_type`/`exception_clear`, DESIGN.md
  Section 8-style surface) so host code that called `ember_call` can
  distinguish "a hard internal-invariant trap happened" (bounds,
  budget, depth, div-by-zero - arguably an ember/engine bug or
  resource exhaustion) from "the script or a native function
  signaled an application-level error" (game-logic-level, expected to
  happen sometimes, e.g. "tried to equip an item that doesn't exist").
  This distinction matters for host error-handling/logging policy,
  not for the underlying unwind mechanism, which is identical either
  way.
- **All traps funnel through one unwind primitive.** There is exactly
  one "abort this guarded entry" mechanism in the whole engine
  (Section 2's host-managed checkpoint); bounds/budget/depth/div-zero/overflow/
  runtime_error/runtime_exception are all just different *reasons*
  recorded before invoking the same unwind - this is a deliberate
  simplification (one code path to get right, one thing to test)
  rather than N special-cased error-propagation mechanisms.

### 7a. Call-target provenance (first-class function refs — REDSHELL guard #6, shipped v1.0)

First-class function references (`&fn` / `handle(args)` / the `fn` type
keyword, `ROADMAP.md` Tier 2 ✓ shipped) open a new runtime surface: a
script can carry an i64 around and later use it as a call target. The
i64-as-call-target surface (the V2 vector the REDSHELL writeup names)
gets two guards, one at each layer:

- **Compile-time first line** (sema, `src/sema.cpp`): i64 ↔ fn
  assignment is forbidden either direction — a script cannot forge a fn
  handle by writing an arbitrary i64 into a `fn`-typed slot. A `&fn`
  handle is the only way to produce a fn-typed value; a native that
  returns `fn` (the `register_routine`-style shape) is the other trusted
  source.
- **Runtime last line** (codegen, `src/codegen.cpp` `emit_call_target_guard`):
  the JIT'd code validates the handle against a host-built bitset
  **allowlist** before indexing the dispatch table.

> **Call-target provenance (Tier 2).** A first-class function handle is an
> i64 = dispatch-table slot index. At every indirect call site (`handle(args)`),
> the JIT'd code validates the handle against a host-built bitset allowlist
> (one bit per registered script-fn slot, set by sema at compile time) before
> indexing the dispatch table. A handle that is out of range or whose bit is
> clear traps via `emit_trap(BadCallTarget)` (longjmp to the host-managed
> checkpoint). The JIT'd code never executes `call rax` / `call [table + h*8]`
> with an unvalidated script-supplied i64. This is the runtime backstop for
> the V2 ("i64-as-call-target") surface that first-class function references
> open; the type-level rule (no i64-to-fn assignment) is the compile-time
> first line, the bitset guard is the runtime last line.

`BadCallTarget` is a `TrapReason` (`src/context.hpp`) routed through the
same shared unwind as every other trap (Section 7 / Section 2). The allowlist
is built by `build_fn_allowlist(script_slots, slot_count)` in
`src/codegen.hpp`; the host pins its `.data()` base for the module's
lifetime and sets `CodeGenCtx::fn_allowlist_base` / `fn_slot_count` before
compiling. The guard is a **no-op (zero emitted)** when no allowlist is
configured (`fn_slot_count == 0`), so every existing module that doesn't use
function refs pays nothing. See `examples/function_refs_test.cpp` (ctest
target `function_refs`) for the out-of-range and in-range-unregistered
handle trap tests. Two open items are documented at `ROADMAP.md` Tier 2:
the **bare-`fn` signature hole** (a `fn`-typed param with no recorded sig
does not type-check args — a type-soundness hole, not a sandbox violation;
the guard still validates the handle, the called code still obeys all
budgets/bounds) and **cross-module handles** (deferred to v2+; the allowlist
is per-module).

## 8. What is explicitly NOT checked (documented gaps, not oversights)

- **Native function argument validity beyond type/arity.** If a host
  registers `set_health(entity_id: i64, hp: f32)` and script calls it
  with an `entity_id` that doesn't correspond to a real entity, that's
  the native function's own responsibility to validate and signal via
  `runtime_error`/`runtime_exception` (Section 7) - ember's type system
  guarantees the *shape* of the call (right number of args, right
  types, matches CODEGEN_SPEC.md's calling-convention placement), not
  domain-level validity of the values.
- **Aliasing/data-race safety across threads.** DESIGN.md non-goals
  already exclude multithreaded execution **inside one `context_t`**;
  v1.0 adds the multi-context model (Option D + B1, see Section 8a
  below) so a host may run **one `context_t` per worker thread** against
  shared compiled code — the dispatch table and JIT'd code are read-only
  after compilation except during a hot-reload slot swap
  (`HOT_RELOAD.md` Section 5); that slot-swap's atomicity is the only
  thread-safety guarantee made on that shared state. Script-visible global
  mutable state shared across contexts is a host-design question, not
  something ember arbitrates. There is no script-level primitive to create
  a data race **between two threads on one context** — running two ember-
  calling threads against a *single* `context_t` still races (a trap on one
  could longjmp to the other's checkpoint — the generalized `--tick` bug);
  the discipline is one-context-per-thread (Section 8a), not in-context
  threading. The sync-queue primitives (`extensions/sync/`) let a script
  coordinate with host threads on host-owned shared state behind i64
  handles under the **U2 contract** (script side single-threaded per
  context; host producer/consumer threads touch only the queue HOST storage
  via the `_host` accessors, never the context) — they do not make a
  `context_t` safe for concurrent calls.
- **Speculative-execution/side-channel classes** (Section 1) - out of scope,
  consistent with every other embedded-scripting engine surveyed.

## 8a. Context thread-safety (Option D + B1 — shipped v1.0)

The non-local-abort machinery in Sections 2–4 was originally
single-threaded-by-assumption: a checkpoint, a budget counter, and a
depth counter lived on one `context_t`, and a trap longjmp'd to that
context's checkpoint. v1.0 makes the per-`ember_call` state
**per-thread** so one compiled module can serve N concurrent caller
threads, each with its own `context_t`. Two pieces (plan:
`docs/plan_CONTEXT_THREADSAFETY.md`):

- **Option D — one `context_t` per thread.** Each concurrent caller
  thread allocates its own `context_t` (private `checkpoint` /
  `budget_remaining` / combined `call_depth` / `last_trap`); they share the
  dispatch table, JIT'd code, and module registry, all of which are
  read-only after compile. A trap on thread B longjmps to thread B's own
  checkpoint — no cross-thread checkpoint corruption. `context_t`
  (`src/context.hpp`) is restructured to a **POD prefix** — the JIT-read
  fields (`budget_remaining`, `call_depth`, `max_call_depth`, then
  `last_trap`) come first and are POD, so `[ctx_reg + offsetof(field)]`
  is standard and stable across compilers (the trailing `std::string`
  `last_error` is non-POD and stays out of the offset math).
  `context_offsets()` gives the exact byte offsets the codegen bakes
  into `[r14 + off]`.
- **Option B1 — the per-call context register.** With
  `CodeGenCtx::use_context_reg = true`, the entry/back-edge budget and
  combined-depth/trap emits
  (`emit_budget_check`, `emit_depth_check`, `emit_depth_leave`,
  `emit_trap` in `src/codegen.cpp`) read `context_t` fields through `r14`
  — the per-call context register, set by the host at entry and
  **callee-saved** so a script-to-script call forwards it (the callee
  inherits the same ctx). The trap stub's first arg (`rcx`) is loaded
  from `r14` rather than a baked imm64 pointer. The load-bearing
  consequence: **one compiled body serves N per-thread `context_t`s** —
  no per-context recompile. The host sets `r14 = ctx` at entry via the raw
  `ember_call_void` / `ember_call_i64` MinGW B1 thunks
  (`src/engine.{hpp,cpp}`); those thunks do not establish checkpoints.
  Legacy non-B1 hosts keep the baked-pointer
  behavior (`use_context_reg = false` is the default).

The globals-lookup emit-time fix that came with it: codegen now prefers
`ctx.globals_index` / `ctx.globals_types` (threaded through `CodeGenCtx`)
over the legacy process-wide `g_globals_for_codegen` pointer (which races
under parallel compilation); the legacy pointer stays as a null-by-default
backward-compat fallback so existing hosts keep working.

Pinned by `examples/thread_safety_test.cpp` (ctest target `thread_safety`):
the keystone two-thread proof — thread A (big budget) finishes and returns,
thread B (tiny budget) traps `BudgetExceeded` on its **own** `context_t`,
and neither thread's longjmp corrupts the other (the `--tick` bug,
generalized). The CLI `--tick` path is the first host to consume B1: commit
`c6457cd` wired `ember_cli --tick` to compile with `use_context_reg = true`,
call `@entry` via `ember_call_void(entry, &ectx)`, and run the tick thread on
its **own** `context_t` (`tick_ctx`) via `ember_call_void(f, &tick_ctx)` —
fully isolated from the main thread's `ectx`, so a tick trap stops the tick
thread, never the main thread. See `docs/v1.0_INTEGRATION_NOTES.md` §1/§5.

**What this does NOT change.** The shared compiled code is read-only
after compile except during a hot-reload slot swap (`HOT_RELOAD.md` Section 5);
in-context multithreaded execution (two ember-calling threads on one
`context_t`) is still a non-goal and still races — the model is
one-context-per-thread, not in-context threading. The sync-queue primitives
in `extensions/sync/` coordinate on host-owned shared state under the U2
contract; they do not remove the one-context-per-thread rule.
