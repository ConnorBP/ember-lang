# ember - safety & sandbox spec

Detail doc for DESIGN.md Section 5. Every "what happens when X goes wrong"

> **Implementation status: v0.1** - this is the v1.0 design spec. The
> current repo implements the JIT codegen proof (encoder, label/patch,
> exec-mem, `.em` format). See `README.md` for what's shipped; see
> `CODEGEN_SPEC.md` Section 12 + Section 15 for the acceptance suite. This doc's
> content is the target design, not a claim of current implementation.
case, and exactly where in the compiled code the check lives.

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

Every `ember_call(context, fn_name, args, argc)` establishes a
**resume point** before entering script code: saves the current
`rsp`/`rbp`/callee-saved-register state (a small `struct
ExecutionCheckpoint` - effectively a `setjmp`-equivalent, implemented
directly rather than pulling in libc `setjmp/longjmp` semantics we
don't need the C-runtime-signal-mask parts of) into the `context_t`.
Any trap (Section 4-Section 7 below) performs the equivalent of `longjmp` back to
this checkpoint: restores `rsp`/`rbp`/callee-saved regs, and
`ember_call` returns `false` with an error recorded on the context
(retrievable like AngelScript's/the surveyed native-JIT language's exception-as-data model,
RESEARCH_NOTES.md - no C++ exception ever crosses the JIT/native
boundary).
- **Nested `ember_call`** (a native function called from script calls
  back into `ember_call` for a different function): each call
  pushes its own checkpoint onto a small stack of checkpoints on the
  `context_t`; a trap unwinds to the *innermost* checkpoint only,
  it a  a  which correctly resumes at the right nested `ember_call`
  invocation, not the outermost one - plain checkpoint stack (`std::vector`
  in the context struct), pop on normal return, pop-and-jump on trap.
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

- **Mechanism**: `context_t` holds a single `int64_t budget_remaining`
  counter. Every JIT'd function's prologue decrements it by a
  **per-function static cost estimate** (a simple count of emitted IR
  instructions for straight-line code - not a cycle-accurate model,
  just a coarse "how much work happened" proxy, matches the surveyed native-JIT language's
  `set_budget(instructions)` being instruction-count not
  cycle-count, RESEARCH_NOTES.md) and checks `<= 0`. **Every loop
  back-edge** (the jump from a `while`/`for` body back to its
  condition check) also decrements by the loop body's static
  instruction count and checks - this is what actually catches
  runaway loops, since a single function-entry check wouldn't catch
  an infinite `while(true){}` that never returns.
  ```
  sub  [budget_ptr], body_cost_imm
  jg   .continue            ; budget_remaining > 0, keep going
  call [ember_trap_budget_slot]   ; does not return (checkpoint unwind, Section 2)
  .continue:
  ```
  `budget_ptr` is loaded once (into a callee-saved register or a
  fixed stack slot) at function entry rather than re-fetched from
  `context_t` on every check, to keep the check itself to a single
  `sub`+`jg`.
- **Exhaustion behavior**: traps via the shared budget-trap stub,
  same non-local unwind as bounds/div-by-zero (Section 2). `ember_call`
  returns `false`, error info records "budget exceeded" (retrievable
  via `last_error`/`exception_*`, DESIGN.md Section 8-style API).
  Script does **not** get a chance to catch this - no in-language
  exception handling exists (DESIGN.md Section 2), matches "abort, don't try
  to let a runaway script gracefully handle its own runaway-ness."
- **No budget set (default)**: `budget_remaining` starts at
  `INT64_MAX` (or budget checks are simply compiled out entirely if
  the module was compiled with budgeting disabled - a module-level
  compile flag, since baking the check in always costs a `sub`+`jg`
  per loop iteration even when unused; hosts that don't need budgets
  at all, e.g. a fully-trusted internal tool script, can compile
  without the checks for zero overhead - explicit opt-in cost).
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

## 4. Stack depth guard

- **Mechanism**: `context_t` holds `int32_t call_depth` and a
  configured `max_call_depth` (default e.g. 512 - generous for game
  script call trees, well below actual native-stack-overflow territory
  given ember frames are small). Every script-to-script *and*
  script-to-native call increments `call_depth` before the call and
  decrements after (native calls count too - a native function that
  calls back into script via `ember_call`, Section 2, shares the same depth
  counter across the boundary, since the actual native OS stack is
  the real resource being protected regardless of which side is
  "currently executing").
  ```
  inc  [call_depth_ptr]
  cmp  [call_depth_ptr], max_call_depth_imm
  jg   .depth_trap          ; does not return
  call ...
  dec  [call_depth_ptr]
  ```
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
  as Section 3.

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
  one "abort this `ember_call`" mechanism in the whole engine
  (Section 2's checkpoint stack); bounds/budget/depth/div-zero/overflow/
  runtime_error/runtime_exception are all just different *reasons*
  recorded before invoking the same unwind - this is a deliberate
  simplification (one code path to get right, one thing to test)
  rather than N special-cased error-propagation mechanisms.

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
  already exclude multithreaded execution inside one context; if a
  host runs multiple `context_t`s on multiple threads against the
  same `module_t` (allowed - the dispatch table and JIT'd code are
  read-only after compilation except during a hot-reload slot swap,
  HOT_RELOAD.md Section 5), that slot-swap's atomicity is the only
  thread-safety guarantee made; script-visible global mutable state
  shared across contexts is a host-design question, not something
  ember arbitrates (there is no script-level threading primitive to
  even create a race from script's own perspective in v1).
- **Speculative-execution/side-channel classes** (Section 1) - out of scope,
  consistent with every other embedded-scripting engine surveyed.
