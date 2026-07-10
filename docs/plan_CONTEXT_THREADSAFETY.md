# Plan — making the ember `context_t` safe for concurrent host-thread calls

> **Status: research / planning only.** This document reads the code
> firsthand and lays out design options. No source is changed. The user is
> making a scoping decision from this — be concrete and honest about which
> option unblocks "two host threads call ember fns at once" vs which only
> helps the narrower "script coordinates with host threads via queues on
> host state" case (the `aint*`/`thread` addon, ROADMAP Tier 5, which
> `SAFETY_AND_SANDBOX.md` §8 already defers).

This is the gate for the rest of a concurrency batch: every later item
(per-thread arena, sync primitives, hot-reload-vs-concurrent-call audit) takes
its posture from which option is picked here.

---

## 1. Current state — what's thread-safe, what isn't (with file:line evidence)

### 1.1 What is ALREADY safe to share across host threads

These are read-only or already-atomic after compilation, so two host threads
can touch them concurrently without ember-side synchronization:

- **The dispatch table slots.** `src/dispatch_table.hpp:6` declares
  `std::vector<std::atomic<void*>> slots`; `set` does `store(...,
  memory_order_release)` (`:9`), `get` does `load(memory_order_acquire)` (`:12`).
  `HOT_RELOAD.md §1` states the invariant ("a slot write during reload must be
  observed atomically by any thread concurrently executing a `call [slot]`"),
  and §4 ("slot indices never change for the lifetime of the `module_t`")
  means a host may cache a **slot index** across threads safely (caching a raw
  pointer is explicitly unsafe, `HOT_RELOAD.md §7`). Script→script calls go
  through `call [dispatch_base + slot*8]` (`src/codegen.cpp:647`, `:656`),
  re-reading the slot on every call. → **multi-thread reads of the dispatch
  table are safe today.** Retired-page reclamation subsequently shipped as
  the host-visible guarded epoch protocol in `HOT_RELOAD.md §5`; every outer
  host invocation must participate in that domain.

- **JIT'd code bytes after `finalize`.** `alloc_executable` commits the page
  (the REDSHELL writeup flagged RWX as a latent V5 catastrophe; v0.4 hardened
  it to RW→memcpy→RX, per `docs/DESIGN.md` v0.4 entry). After finalize the
  page is RX; multiple threads executing the same function body
  simultaneously is fine (it's pure code, no embedded mutable state in the
  body itself — all mutable state the body touches is reached through
  pointers it loads, see §1.2). → **multi-thread execution of a finalized
  function is safe.**

- **The `ModuleRegistry`** (`src/module_registry.hpp`), to the extent the
  host doesn't mutate it concurrently with calls. `cross_module` calls bake
  `registry_base` as a kind-2 reloc and read `[registry_base + mod_id*8]`
  (`src/codegen.cpp:718`–`724`). A registry that's done being built before
  threads start is read-only at call time.

### 1.2 What is NOT safe — the four pieces of per-call state, and the baked pointers

`context_t` (`src/context.hpp:44`–`72`) is a plain struct with no atomicity
and no synchronization. Four fields are mutated while JIT'd code runs, and a
fifth piece of state (the trap target) is *baked into the JIT'd bytes
themselves*:

1. **`jmp_buf checkpoint` + `bool has_checkpoint`** (`context.hpp:47`–`48`).
   `ember_call` (in the CLI: `examples/ember_cli.cpp:425`–`431`) does
   `__builtin_setjmp(ctx.checkpoint)` before entering JIT'd code; the trap
   stub `ember_cli_trap` (`ember_cli.cpp:97`–`110`) does
   `__builtin_longjmp(ctx->checkpoint, 1)`. **Two threads setjmp'ing into the
   same `checkpoint` is undefined** — the second setjmp clobbers the first's
   saved rsp/rbp/ip, and a longjmp from thread B unwinds to whatever thread A
   last saved, on thread A's stack. This is the most dangerous of the four:
   a misrouted longjmp corrupts a foreign thread's stack pointer.

2. **`int64_t budget_remaining`** (`context.hpp:54`). Decremented at every
   loop back-edge by JIT'd code: `src/codegen.cpp:266`–`282` (`emit_budget_check`).
   The JIT'd body does `mov rax, <budget_ptr>; sub qword [rax], body_cost; jg
   .continue; else trap`. Two threads decrementing the same `int64_t` is a
   classic read-modify-write race — lost decrements (a runaway loop on one
   thread may not be caught because the other thread's sub overwrites it) and
   torn reads on the `jg` branch.

3. **`int32_t call_depth`** (`context.hpp:57`). Incremented before every
   script-to-script call and decremented after: `src/codegen.cpp:293`–`310`
   (`emit_depth_check` / `emit_depth_leave`). Same RMW race as the budget —
   lost incs/deczs mean the depth guard can both false-trap (one thread's
   incs counted against the other's max) and fail to trap (real overflow
   hidden by a lost inc).

4. **`TrapReason last_trap` + `std::string last_error`** (`context.hpp:61`–`63`).
   Written by the trap stub before the longjmp (`ember_cli.cpp:66`–`67`). Two
   concurrent traps racing these fields produce garbage error reporting
   (interleaved bytes in `last_error`, a `last_trap` from the wrong thread).
   Lower severity than 1–3 (it's diagnostics, not control flow), but still a
   data race on a `std::string` (heap, can crash under concurrent mutation).

5. **The decisive constraint — `trap_ctx`, `budget_ptr`, `depth_ptr` are
   ABSOLUTE `imm64`s BAKED INTO THE JIT'd CODE.** At JIT time, codegen emits:
   - `src/codegen.cpp:242` — `mov rcx, imm64(ctx.trap_ctx)` (the trap stub's
     first arg, the `context_t*`).
   - `src/codegen.cpp:268` — `mov rax, imm64(ctx.budget_ptr)` (the budget
     counter address).
   - `src/codegen.cpp:295` and `:307` — `mov rax, imm64(ctx.depth_ptr)` /
     `mov r10, imm64(ctx.depth_ptr)` (the depth counter address).

   `CodeGenCtx` (`src/codegen.hpp:36`–`73`) carries these as plain pointers:
   `void* trap_ctx`, `int64_t* budget_ptr`, `int32_t* depth_ptr`. They are
   stamped into the function body once, at `compile_func` time. **A single
   JIT'd function body therefore has exactly ONE `context_t*` baked into it.**
   You cannot, at runtime, point the same compiled bytes at a different
   context. This is the central technical fact that shapes every option below.

### 1.3 The globals problem — process-wide, races at both emit time and run time

Two distinct races, both real, both named in the user's brief:

- **Emit-time race on `g_globals_for_codegen`.** `src/codegen.hpp:95`
  declares `extern GlobalsBlock* g_globals_for_codegen;`, defined at
  `src/codegen.cpp:2059` as a single process-wide pointer. The comment at
  `codegen.hpp:91`–`94` says it plainly: *"A single process-wide pointer
  (v1 frontend; the host wires one block per engine)."* The globals
  load/store paths read it directly at emit time to decide whether an
  `Ident` is a global and to look up its index/type:
  - `src/codegen.cpp:951`–`957` (global load in `eval`'s `Ident` case)
  - `src/codegen.cpp:1360`–`1365` (global store in `AssignExpr`'s `Ident` target)

  If two host threads concurrently `compile_func` against **different**
  `GlobalsBlock`s (e.g. two modules with their own globals, compiled in
  parallel), they race the single process-wide pointer: thread A sets it to
  `&gb_A`, thread B sets it to `&gb_B`, and the emit-time index lookup in one
  thread may run against the other thread's block. Result: a global resolved
  to the wrong index, baked into JIT'd bytes — a silent, permanent
  miscompile, not a runtime race. **This is a compile-path blocker for any
  option that wants parallel compilation.**

- **Run-time race on the shared globals block.** The baked `GlobalsBase`
  reloc is filled from `ctx.globals_base` at finalize (`src/codegen.cpp:2289`,
  `AbsFixup::GlobalsBase`). The JIT'd body does `mov r11, <globals_base>; mov
  rax, [r11 + off]` (`load_global_to_rax`, `codegen.cpp:878`–`883`) and the
  matching store (`store_rax_to_global`, `:885`–`888`; float variant
  `:891`–`900`). If two threads run functions that read/write the **same**
  globals block, the writes race (a non-atomic `mov [r11+off], rax`).
  `SAFETY_AND_SANDBOX.md §8` puts this squarely on the host: *"script-visible
  global mutable state shared across contexts is a host-design question, not
  something ember arbitrates."* So this is a **run-path question the host
  answers per use case**, not something ember can settle unilaterally — but
  any option that wants to *enable* shared-globals concurrency has to take a
  position on it.

### 1.4 The `--tick` mode is the existing concurrency bug, live in the tree

`examples/ember_cli.cpp:440`–`510` spawns a real `std::thread` that calls
`@on_tick` fns through the dispatch table while the **main thread sits in a
TUI poll** (`:471`–`477`). It reuses the single `ectx`:

- `ember_cli.cpp:328`–`333` builds one `context_t ectx`, sets
  `ctx.trap_ctx = &ectx`, `ctx.budget_ptr = &ectx.budget_remaining`,
  `ctx.depth_ptr = &ectx.call_depth`.
- The tick thread re-arms `ectx.has_checkpoint` and `__builtin_setjmp`s into
  `ectx.checkpoint` from the **tick thread** (`ember_cli.cpp:456`–`460`).
- The comment at `:476` admits the bug verbatim:
  *"The JIT'd code baked ctx.trap_ctx = &ectx at compile time, so a trap in a
  tick longjmps to ECTX's checkpoint (already cleared). For tick-safety we
  instead catch via a wrapper: … re-arm ectx."*

  Translation: a trap during a tick longjmps to whatever `ectx.checkpoint`
  last held — which the tick thread itself just setjmp'd, so in the
  *isolated* CLI it happens to land back in the tick thread. But the moment a
  second host thread is also using `ectx` (the exact case this plan is about),
  the longjmp target is non-deterministic between the two threads' setjmps.
  The "re-arm ectx" workaround is exactly the anti-pattern this plan has to
  eliminate: **two threads sharing one checkpoint + one set of counters.**

So: §1.2 item 5 (baked pointers) is not hypothetical — the existing
concurrent-code path in the tree already collides with it and already has a
confessed bug.

### 1.5 What the spec already says (so the options don't fight the spec)

`SAFETY_AND_SANDBOX.md §8` (the explicit non-goals section) says:

> *"DESIGN.md non-goals already exclude multithreaded execution inside one
> context; if a host runs multiple `context_t`s on multiple threads against
> the same `module_t` (allowed — the dispatch table and JIT'd code are
> read-only after compilation except during a hot-reload slot swap,
> HOT_RELOAD.md §5), that slot-swap's atomicity is the only thread-safety
> guarantee made; script-visible global mutable state shared across contexts
> is a host-design question, not something ember arbitrates (there is no
> script-level threading primitive to even create a race from script's own
> perspective in v1)."*

`ROADMAP.md` Tier 5 ("Threads (`thread` addon, `aint*` atomics)") says:

> *"multithreaded script execution inside one context is a v1 non-goal
> (SAFETY_AND_SANDBOX.md §8). … Dep: GC thread-safety, per-context arena, the
> whole memory model gets harder. Large; defer as long as possible —
> multi-context parallelism covers most real cases without in-context
> threading."*

`HOT_RELOAD.md §5` subsequently shipped concurrent epoch/quiescence
reclamation without adding epoch fields to per-thread contexts: hosts wrap
outer calls in a domain `ExecutionGuard`, and the domain tracks active entry
epochs. There is still no global context registry. The context remains the
per-call/per-thread unit for mutable execution state.

**Bottom line of §1:** the dispatch table, the JIT'd code, and the registry
are already shareable. The five things in §1.2 (checkpoint, budget, depth,
last_error, and the three baked pointers) are not. The globals block races at
both emit and run time (§1.3). The `--tick` code is a live instance of the bug
(§1.4). The spec has *already* picked the model — per-`context_t`
parallelism, no in-context threading (§1.5) — so the options are about how to
realize that model, not whether to overturn it.

---

## 2. The OPTIONS

Each option: what changes, what's safe, what's NOT safe, what it costs (JIT
change? spec change? perf), and what it actually lets the user do. At the end
of each, an explicit verdict on the two cases the user named:

- **(U1)** "two host threads call ember fns at once" — the broad case.
- **(U2)** "script coordinates with host threads via queues on host state"
  — the narrow case the `aint*`/`thread` addon's sync primitives would cover,
  *without* requiring context thread-safety (because in U2 the script side
  is single-threaded per context; only host state is shared, via host-owned
  queues/atomics the host already synchronizes).

### Option A — per-call context state (each `ember_call` gets its OWN checkpoint + budget + depth)

**Idea.** The spec's nested-`ember_call` checkpoint **stack**
(`SAFETY_AND_SANDBOX.md §2`: *"each call pushes its own checkpoint onto a small
stack of checkpoints on the `context_t`; a trap unwinds to the innermost
checkpoint only"*) is today shipped as a single `jmp_buf` (`context.hpp:8`–`11`
comment: *"v0.4 ships the single-call checkpoint … the spec's nested
ember_call checkpoint STACK is v1.0"*). Option A extends that stack so
concurrent calls don't share: each host→script entry pushes a fresh
`{checkpoint, budget_remaining, call_depth, last_trap, last_error}` frame.

**What changes.**
- `context_t` gains a small `std::vector<CallFrame>` (or a fixed-depth ring)
  holding per-call state; `reset_for_call` (`context.hpp:65`) becomes a push.
- The CLI's hand-rolled `__builtin_setjmp(ectx.checkpoint)`
  (`ember_cli.cpp:425`–`431`) becomes "push a frame, setjmp into *that*
  frame's `checkpoint`." A real `ember_call` wrapper (which the spec describes
  but the engine doesn't ship — `src/engine.hpp`/`engine.cpp` have no
  `ember_call`; the CLI calls JIT'd entry via a raw `reinterpret_cast<F0>(entry)()`
  at `:438`) would own this push/pop.
- The trap stub (`ember_cli_trap`) must longjmp to the **innermost frame on
  the calling thread's** frame stack.

**The trap-stub-finds-the-right-context problem (the crux).** This is where
§1.2 item 5 bites. The trap stub receives `rcx = ctx.trap_ctx` — a single
baked pointer (`codegen.cpp:242`). For Option A to work with a *shared*
`context_t`, the stub must, at trap time, find the **right** frame for the
**calling thread**. Two sub-paths:

  - **A1 — `thread_local` "current frame" pointer inside the shared context.**
    The trap stub reads a `thread_local CallFrame* ember_current_frame` to find
    the frame to longjmp to. The baked `trap_ctx` can stay `&ectx` (the shared
    context); the stub uses the thread_local to pick the frame. **This works
    and is cheap** (one `thread_local` load per trap; traps are cold). But it
    makes the per-call frame a *thread-local* concept inside a *shared*
    context — which is just Option B in disguise (see B). The budget/depth
    counters still need to be per-frame (so two concurrent calls decrement
    different `int64_t`s), and the JIT'd code's `budget_ptr`/`depth_ptr` are
    still baked — so the JIT'd body still points at ONE counter. → A1 only
    solves the checkpoint; it pushes the counter problem onto B.

  - **A2 — per-call budget/depth too, resolved at JIT time is impossible.**
    The baked `budget_ptr` (`codegen.cpp:268`) is a compile-time imm64. You
    cannot give each *call* its own counter while reusing the same compiled
    bytes, unless the bytes load the counter through an indirection that's
    resolved per-call (a `thread_local` pointer — option B). So **pure**
    Option A (per-call state, shared context, no thread_local) can solve the
    checkpoint but CANNOT solve the budget/depth without changing what the
    JIT'd code reads.

**What's safe.** The checkpoint (under A1). `last_trap`/`last_error` if they
move into the per-call frame.

**What's NOT safe.** Budget and depth, unless A is combined with B's
thread_local indirection. Globals still race (§1.3) — A says nothing about
globals.

**Cost.** Small struct change + a real `ember_call` wrapper (which the spec
already wants and the engine lacks). No JIT change *if* you accept A1 +
inherit B's counter indirection. Spec change: none — A *is* the spec's
nested-call checkpoint stack, finally shipped.

**What it unblocks.** A1+B-hybrid unblocks **U1**. Pure A (shared context,
per-call frames, no thread_local counters) unblocks **only U2** and even
then only if the host serializes calls or accepts racing budget/depth.

**Honest verdict.** "Per-call context state" sounds clean but, because of the
baked counter pointers, it doesn't stand alone — it collapses into "B for
the counters, A for the checkpoint." Worth doing as the *shipping shape* of
the checkpoint stack (the spec wants it anyway), but it is not, by itself, a
thread-safety answer.

### Option B — `thread_local` checkpoint / counters (each host thread has its own)

**Idea.** Give each host thread its own `jmp_buf` checkpoint, its own
`int64_t budget_remaining`, its own `int32_t call_depth`, all resolved via
`thread_local` storage. The JIT'd code and the trap stub read them through a
`thread_local` pointer rather than a baked imm64.

**What changes — the JIT'd code must change what it reads.** Today the body
does `mov rax, imm64(budget_ptr); sub qword [rax], cost` (`codegen.cpp:267`–`272`).
Under B, the body must instead load a `thread_local` pointer and indirect
through it. Two implementations:

  - **B1 — indirect through a single `thread_local context_t*`.** Declare
    `thread_local ember::context_t* ember_active_ctx;` The trap stub reads
    `ember_active_ctx` (not the baked `trap_ctx`) to find the longjmp target;
    the budget/depth emit rewrites to load `ember_active_ctx` once into a
    callee-saved register at function entry, then `sub qword [rax + offsetof(budget_remaining)], cost`.
    The baked `trap_ctx`/`budget_ptr`/`depth_ptr` in `CodeGenCtx` become
    unused (or kept for backward compat as "fixed-context mode").
    **This is a real JIT change**: every budget check goes from
    `mov rax, imm64; sub [rax]` to `mov rax, [thread_local_ptr]; sub [rax+off]`
    — one extra load per check (and the load is `thread_local`, which on
    Windows/ELF goes through TLS with a small extra cost; on a hot loop
    back-edge that's measurable). The depth check (per-call, not per-loop) is
    less sensitive.
  - **B2 — per-field `thread_local` storage.** `thread_local int64_t
    ember_budget;` `thread_local int32_t ember_depth;` `thread_local jmp_buf
    ember_checkpoint;` Codegen bakes a `mov rax, &ember_budget` — but
    `&thread_local` is *not* a link-time-constant address; it's resolved per
    thread via the TLS block. The emitter would have to emit a TLS-access
    sequence (e.g. on Windows, load from `gs:[0x28]`-style TLS slot; on ELF,
    `%fs:0`-relative). **This is a substantial JIT change** — the X64Emitter
    (`src/x64_emitter.hpp`) has no TLS-access emission today. Higher
    implementation cost than B1, and B1 is strictly more general (it gives you
    per-context too).

**The "can budget_ptr/depth_ptr be thread_local-resolved?" question
(answered).** Yes, but not by leaving the baked `imm64` in place — the bake
is a compile-time *address*, and a `thread_local` object's address is
per-thread. You must either (B1) indirect through a `thread_local` pointer to
a real object (the address-of-the-pointer *is* a constant TLS slot, but the
pointer's value varies per thread), or (B2) emit real TLS-access byte
sequences. B1 is the practical answer; B2 is over-engineering for no gain over
B1.

**What's safe.** Checkpoint, budget, depth — all four per-thread fields. Two
threads each running their own `ember_active_ctx` never touch the same
counters or the same `jmp_buf`.

**What's NOT safe — two things B does NOT solve.**
  - **Globals.** B says nothing about `g_globals_for_codegen` (§1.3 emit race)
    or about shared-globals writes (§1.3 run race). Two threads with their own
    `ember_active_ctx` but compiling/running against the same globals block
    still race. B *must* be paired with a globals decision (see "The globals
    problem" §3).
  - **Same-context concurrency.** If two threads deliberately point their
    `ember_active_ctx` at the **same** `context_t` (the user's "into the
    *same* context" phrasing), B does not help — it's a per-thread pointer,
    but if both set it to `&shared_ctx`, you're back to racing the shared
    context's fields. B's correctness *requires* that concurrent threads use
    *distinct* contexts (or distinct per-thread storage). → B is really
    "Option D implemented via `thread_local` indirection." See D.

**Cost.** JIT change (B1: emit a TLS-pointer load at function entry, carry it
in a callee-saved reg; per-check uses `[reg+off]` instead of `[imm64]`). One
extra load per loop back-edge (budget) and per call (depth) — the budget one
is the perf-sensitive one (it's on the hot loop path). Spec change: minor —
the trap stub's contract changes from "longjmp the baked ctx" to "longjmp the
thread's active ctx"; SAFETY §2 gets a sentence. The CLI's `ember_cli_trap`
(`ember_cli.cpp:97`–`110`) changes to read the thread_local.

**What it unblocks.** Paired with a globals decision, B1 unblocks **U1**
fully: two host threads, each with its own `context_t` set as
`ember_active_ctx`, call ember fns concurrently with correct traps, budgets,
and depth. It does NOT unblock "two threads into the *same* context" — and
per §1.5 the spec doesn't want that anyway.

**Honest verdict.** B1 is the **mechanism** that makes D (and the A1 hybrid)
actually work at the JIT level. It is not really a standalone "option" —
it's the implementation technique the recommended option (D) depends on. B2
is not worth it.

### Option C — coarse context mutex (one thread in the context at a time)

**Idea.** A `std::mutex` (or a spinlock) in or alongside `context_t`; every
`ember_call` takes it for the duration of the call. Trivially correct: only
one thread is ever inside the context.

**What changes.** Add a mutex. Wrap the call path. No JIT change. No spec
change beyond noting the lock.

**What's safe.** Everything — checkpoint, budget, depth, last_error all
become exclusive to the lock holder. Globals *within* the locked context are
safe too (only the holder runs).

**What's NOT safe.** Nothing inside the context. But: **the dispatch table
and JIT'd code are already lock-free-shareable (§1.1)**, so a context mutex is
strictly *more* conservative than necessary — it serializes calls that don't
need serialization. And `g_globals_for_codegen` (§1.3 emit race) is *not*
covered by a runtime context mutex at all — two threads *compiling* in
parallel still race the process-wide pointer. So C doesn't even enable
parallel compilation.

**Cost.** One lock acquire/release per `ember_call`. Cheap per call. But the
*purpose* of the user's batch is parallelism, and C kills exactly the
parallelism the batch wants. Perf: serializes all calls into the context —
the two host threads might as well be one.

**Does C satisfy "callbacks on different threads"?** It depends on what that
phrase means:
  - If it means "host thread T1 calls `@on_tick` while host thread T2 calls
    `@event("x")` *into the same context*" — **C makes this safe but
    sequential.** T2 blocks on T1's lock. The callbacks *fire* on their
    respective threads (so thread identity is preserved for host-side
    expectations like thread-affine APIs), but they do not run *concurrently*.
    If the host's requirement is merely "the right callback fires on the right
    thread, and I don't care if they're serialized," C satisfies it. If the
    requirement is "they run at the same time," C does not.
  - If it means "two host threads do real parallel ember work" — **C does not
    satisfy this.** It is the negation of the goal.

**What it unblocks.** C unblocks **U2 trivially** (script side stays
single-threaded per context; the lock is uncontended in the U2 shape anyway,
because U2 has one script thread per context). C unblocks **U1 only in the
degenerate "callbacks on different threads but serialized" sense** above —
which most callers reading "call ember fns at once" will not consider a
satisfying answer.

**Honest verdict.** C is the right *interim* answer (a one-line safety net
you can ship today to make the existing `--tick` bug not corrupt anything),
but it is not the answer the batch is gating on. Recommend: ship C as a
transitional guard under the existing `--tick` path immediately (fixing
§1.4), and treat it as the fallback if D proves too large for the first pass.

### Option D — per-thread `context_t` instances (each host thread gets its own context; shared dispatch table + globals via the registry)

**Idea.** Each host thread that wants to call ember allocates its own
`context_t` (private checkpoint, budget, depth, last_error). They share the
**dispatch table** (§1.1 — already atomic), the **JIT'd code** (§1.1 — RX,
shareable), and the **registry** (read-only after build). The globals
question is handled explicitly (§3 below).

**This is the spec's already-stated model.** `SAFETY §8` (§1.5) says exactly
this: *"a host runs multiple `context_t`s on multiple threads against the
same `module_t` (allowed — the dispatch table and JIT'd code are read-only
after compilation except during a hot-reload slot swap, HOT_RELOAD.md §5)."*
`HOT_RELOAD.md §5` now ships a separate host-owned quiescence domain. Option D
remains about context safety; a context does not itself own reclamation state.

**What changes.**
- **Host API.** A host wanting N concurrent caller threads allocates N
  `context_t`s (one per thread). Today the CLI allocates one `ectx`
  (`ember_cli.cpp:296`); a `--tick` that wanted a real concurrent caller
  would allocate a per-tick-thread `context_t`. The natural shape: a small
  `context_t` pool or a `thread_local context_t*` (which is B1's
  `ember_active_ctx`).
- **JIT'd code — the baked pointers.** Here's the catch: today, the trap_ctx,
  budget_ptr, depth_ptr are baked at *compile* time (`codegen.cpp:242`,
  `:268`, `:295`, `:307`) from `ctx.trap_ctx`/`ctx.budget_ptr`/`ctx.depth_ptr`.
  If you compile once and want N contexts to run the same bytes, the baked
  pointers point at ONE of them. So D, as-is, only works if either:
  - **D-shared-compile (compile once, N contexts): requires B1.** The JIT'd
    body must indirect through a `thread_local context_t*` (B1) so the same
    bytes work for any thread's context. Without B1, D-shared-compile is
    broken: thread B's calls trap into thread A's context (the baked one).
    This is the `--tick` bug again, in general form.
  - **D-per-compile (compile per context): wasteful.** Recompile the whole
    module once per `context_t`, each time baking that context's pointers.
    Correct, but you JIT the module N times (memory + time). Fine for tiny
    tests, wrong for production.

  So **D's correct production shape is "D + B1"**: per-thread `context_t`s,
  JIT'd once, the body reads its counters/checkpoint through a
  `thread_local context_t*`. That's the recommended option (§4).

- **Spec change.** Minimal — D *is* what §8 already describes. Add one
  sentence making the per-thread context allocation explicit and stating the
  `thread_local` indirection requirement. `SAFETY §2` (the checkpoint) gets
  the note that the checkpoint is per-`context_t` and a `context_t` is not
  shared across threads.

**What's safe.** Checkpoint, budget, depth, last_error — all private per
thread's context. No longjmp can land on a foreign thread (the longjmp target
is the calling thread's own `context_t.checkpoint`, which only that thread
setjmp'd). Two threads decrementing budgets/depths touch different `int64_t`s.

**What's NOT safe — globals (the real D question).** Per-thread contexts
don't share *execution* state, but they DO share the **script globals block**
if the host wires them to the same `GlobalsBlock`. Two threads running fns
that write `global g_player_health` race the non-atomic `mov [globals_base +
off]`. D does not solve this; it explicitly punts it to the host per §8. Two
sub-positions (see §3 for the full answer):
  - **D-private-globals:** each `context_t` gets its own globals block
    (private script state). No cross-thread globals race because there are no
    cross-thread globals. Covers "N independent mod instances" (e.g. N AI
    agents each with their own state). Does NOT cover "shared world state."
  - **D-shared-globals-atomics:** the shared globals block uses atomic loads/
    stores (and the host accepts that compound updates — `g += 1` — are not
    atomic in the script, requiring host-side `aint` natives or locks). This
    is exactly the `aint*` addon (ROADMAP Tier 5). It's *the* sync-primitive
    story the user referenced as U2.

**Cost.** D alone (with B1): one JIT change (the TLS indirection, B1), a host
API tweak (allocate per-thread context), one extra load per budget check.
Per-thread memory cost: one `context_t` per caller thread (≈ a jmp_buf + 16
bytes + a string, tiny). No recompile-per-context cost (D-shared-compile).
The `--tick` CLI path becomes "tick thread has its own `context_t`" — which
also *fixes §1.4 as a side effect* (the tick thread's traps longjmp to the
tick thread's own checkpoint, never the main thread's).

**What it unblocks.**
  - **D + B1 + private-globals: unblocks U1** for the "N independent
    contexts" shape (parallel ember work with no shared script state). This
    is the cleanest, smallest first pass.
  - **D + B1 + shared-globals-atomics: unblocks U1** for shared-world-state
    too, but pulls in the `aint*` addon (U2's territory) — which the user
    explicitly wants to *defer*. So this combination is the *second* pass,
    not the first.

**Honest verdict.** D is the cleanest option and the one the spec already
points at. Its only nontrivial dependency is B1 (the TLS indirection that lets
one JIT'd body serve N contexts). The scoping decision is really "D now
(private-globals, U1-for-independent-contexts) and defer D-shared-globals
(U1-for-shared-world + U2) to the `aint*` batch."

---

## 3. The globals problem — answered

The user asked specifically: *"Is 'globals are per-context, atomics for
cross-thread' the answer, or is shared-globals a real blocker?"*

**Answer: shared-globals is NOT a blocker for the first pass; it is a
*deferred* scope item, and the spec already says so.** Concretely:

1. **`g_globals_for_codegen` (the emit-time race) must be fixed regardless
   of which option is picked**, because it blocks *parallel compilation*
   even in single-context-per-thread worlds. The fix is mechanical and
   orthogonal to A/B/C/D: **stop reading a process-wide pointer at emit
   time; thread the `GlobalsBlock*` (or its `index`/`types` maps) through
   `CodeGenCtx`** so each `compile_func` call uses the caller's block. The
   `load_global_to_rax`/`store_rax_to_global` helpers already take `base` as
   a (now-unused) parameter and resolve the *runtime* address through
   `AbsFixup::GlobalsBase` filled from `ctx.globals_base` (`codegen.cpp:2289`)
   — so the *runtime* path already goes through `ctx`; only the *emit-time*
   decision (is this name a global? which index?) reads the global pointer.
   Moving that decision onto `ctx` (e.g. `ctx.globals_index`, `ctx.globals_types`)
   is a small, localized change with no JIT-bytes difference. **This is in
   scope for the first pass** no matter what, because it's a correctness bug
   for parallel compilation, not a concurrency feature.

2. **The shared-globals run-time race** is, per `SAFETY §8`, a *host-design
   question*. Three legitimate host choices, each its own scope:
   - **Private-globals-per-context (default for the first pass).** Each
     `context_t` owns its `GlobalsBlock`. No cross-thread globals race
     possible. Matches "N independent mod instances." This is what D +
     private-globals ships. The CLI's current single-context shape already
     does this (one `gb` at `ember_cli.cpp:288`–`297`); making it per-thread
     is just "allocate one `gb` per `context_t`."
   - **Shared-globals + host guarantees no concurrent writers.** The host
     seeds globals before any thread runs and promises read-only access
     during concurrent execution. Safe without atomics. Covers
     "configuration constants shared across parallel workers."
   - **Shared-globals + atomics (`aint*` addon).** The script-visible
     globals block uses atomic load/store; compound updates need
     `aint`/`compare_exchange` natives. This is the U2/sync-primitive story
     (ROADMAP Tier 5). **Deferred.**

So "globals are per-context (default), atomics for cross-thread (the deferred
`aint*` batch)" is exactly the answer. Shared-globals-concurrency is a *real*
feature (not a blocker), it just lives in a later batch, and the spec already
places it there.

---

## 4. Recommendation

**Recommended: Option D, implemented via B1, with private-globals-per-context
and the `g_globals_for_codegen` emit-time fix. Ship C as a transitional guard
under `--tick` immediately.**

Rationale:

- **D is the spec's own model** (§1.5 — `SAFETY §8` + `HOT_RELOAD §5` already
  say "multiple `context_t`s on multiple threads against the same
  `module_t`"). Picking D is realizing the documented design, not inventing one.
  It also matches the ROADMAP's explicit "multi-context parallelism covers
  most real cases without in-context threading."
- **B1 is the smallest JIT change that makes one compiled body serve N
  contexts.** Without it, D forces per-context recompilation (wasteful) or
  ships the `--tick` bug in general form. With it, the trap stub and the
  budget/depth checks read through a `thread_local context_t*` and "just
  work" per thread.
- **Private-globals-per-context is the honest scope for the first pass.** It
  unblocks U1 for the "N independent parallel callers" shape — the shape
  that matters for `--tick`-style "tick thread + main thread doing real
  different work" and for a game host running N mod instances on N worker
  threads. It does not pull in the `aint*`/sync-primitive work, which the
  user wants deferred.
- **The `g_globals_for_codegen` emit-time fix is non-negotiable and small.**
  It's a latent miscompile under any future parallel-compile scenario
  regardless of which concurrency option ships; it should be fixed in the
  first pass because it's a correctness item, not a feature.
- **C-as-transitional-guard** turns the existing `--tick` bug (§1.4) from
  "ships broken" into "ships safe but serialized" while D+B1 is being built,
  and remains the documented fallback if D+B1's TLS perf cost on the budget
  hot path proves unacceptable.

What the recommendation does **not** pick: A as a standalone (collapses into
B for the counters anyway), B2 (over-engineered TLS byte emission), C as the
end state (negates the batch's purpose), and shared-globals-atomics (deferred
to the `aint*` batch — U2).

---

## 5. Scope — first thread-safety pass vs. deferred

### IN scope for the first pass

1. **`g_globals_for_codegen` emit-time fix.** Thread the globals
   `index`/`types` (or the `GlobalsBlock*`) through `CodeGenCtx` so
   `compile_func` no longer reads a process-wide pointer (`codegen.cpp:951`,
   `:1360`). No JIT-bytes change (runtime path already uses
   `AbsFixup::GlobalsBase` from `ctx.globals_base`). **Blocks parallel
   compilation; ships regardless.**

2. **Option D host API: per-`context_t` per caller thread.** Document and
   implement the "allocate one `context_t` per concurrent caller thread"
   pattern. The CLI's `--tick` path gets a per-tick-thread `context_t`
   (fixing §1.4 directly).

3. **Option B1 JIT change: `thread_local context_t*` indirection.** The trap
   stub and the budget/depth emit read through a `thread_local` active-context
   pointer instead of baked `imm64`s. Specifically:
   - `emit_trap` (`codegen.cpp:222`–`253`): rcx (the `context_t*`) comes from
     the `thread_local` active ctx, not `ctx.trap_ctx`.
   - `emit_budget_check` (`:266`–`282`): load the budget counter via the
     `thread_local` ctx (+ `offsetof(budget_remaining)`), not a baked
     `budget_ptr`.
   - `emit_depth_check`/`emit_depth_leave` (`:293`–`310`): same for
     `call_depth`.
   - One TLS-pointer load per function entry into a callee-saved register;
     per-check uses `[reg+off]`.

4. **Spec update.** `SAFETY §2` (checkpoint is per-`context_t`, a context is
   not shared across threads) and `§8` (state D's per-thread-context model
   explicitly, name the `thread_local` indirection, restate that
   shared-globals is a host-design question deferred to `aint*`).

5. **A checkpoint-stack forward-compat note.** The spec's nested-`ember_call`
   checkpoint stack (`SAFETY §2`, shipped as single-`jmp_buf` today,
   `context.hpp:8`–`11`) remains a v1.0 item; the first pass keeps the
   single-`jmp_buf` per `context_t` but makes that `jmp_buf` provably
   single-thread (which is what D + B1 gives). The stack ships later, on top
   of the now-thread-safe single-`jmp_buf`.

6. **C as a transitional guard** under the existing `--tick` path, removed
   (or downgraded to an assertion) once D+B1 lands and is tested.

### STAYS deferred (out of scope for the first pass)

- **In-context threading / the `thread` addon / `aint*` atomics** (ROADMAP
  Tier 5, `SAFETY §8` v1 non-goal). This is U2's sync-primitive story:
  script-level `aint*`, locks, queues, `thread` blocks. The first pass
  *enables* host-level multi-context parallelism; it does *not* give the
  script any new concurrency primitive. The user explicitly wants to SEE the
  options before committing to the batch — this deferral is part of that
  scoping: the first pass is *not* the `aint*` batch.

- **Shared-globals concurrency (the run-time race on a shared `GlobalsBlock`).**
  Per §3, the first pass ships private-globals-per-context. Shared-globals +
  atomics is the `aint*` batch. A host that wants shared read-only config
  globals across threads may do so under the "host guarantees no concurrent
  writers" rule (§3) — that's a host discipline, not an ember change.

- **The nested-`ember_call` checkpoint stack** (the full `SAFETY §2` nested
  form). First pass: single-`jmp_buf` per `context_t`, but thread-safe.
  Nested (a native calling back into `ember_call` from within a call) ships
  later, building on the now-safe single-`jmp_buf`.

- **Concurrent hot-reload reclamation** (`HOT_RELOAD §5`) — subsequently
  shipped as a separate host epoch/quiescence domain. It intentionally adds no
  epoch fields to `context_t`; the outer host invocation owns an RAII guard.

- **Cross-thread forced unwind / preemptible wall-clock timeout**
  (`SAFETY §3` explicit deferral: "cross-thread forced unwind of running JIT
  code is a hard problem … deferred"). Unchanged; out of scope.

---

## 6. Test shape that would prove it

The proof is "two host threads do real concurrent ember work and the trap /
budget / depth machinery behaves correctly per-thread, with no cross-thread
longjmp and no shared-counter race." Concretely, a test suite shaped like:

1. **Single-thread regression (must not break).** Run the existing safe-execution
   tests (the v0.4 hardening suite, `examples/v0_4_hardening_test.cpp` shape —
   budget exhaustion, depth overflow, bounds, `@obf_keyed` mismatch, all
   trapping via the stub) against one `context_t` on one thread. Every trap
   still longjmps to the right checkpoint, `last_trap`/`last_error` still
   correct. **Proves B1 didn't regress the single-thread path.**

2. **Two-thread independent contexts, no traps (U1 happy path).** Two threads,
   each with its own `context_t` set as `ember_active_ctx`, each calling a
   long-running pure-compute fn (e.g. a fib loop) concurrently. Assert both
   return correct values; assert budgets decremented independently (each
   thread's `budget_remaining` ended where that thread's work put it, not
   some sum). **Proves the counters are per-context.**

3. **Two-thread independent contexts, one traps (U1 trap isolation).** As (2)
   but thread A's fn is an infinite loop that exhausts its budget and traps;
   thread B's fn is a normal compute. Assert: A's call returns with
   `last_trap == BudgetExceeded` and A's checkpoint unwound only A's call; B
   completes normally and B's `last_trap == None`. **Proves no cross-thread
   longjmp** (the core §1.2 item 1 hazard). This is the test that the existing
   `--tick` bug would fail.

4. **Two-thread, both trap concurrently (race on diagnostics).** Both threads'
   fns trap at roughly the same instant. Assert each thread's
   `last_trap`/`last_error` reflects *its own* trap reason, not the other's.
   **Proves `last_trap`/`last_error` (§1.2 item 4) are per-context.**

5. **Depth guard per-thread.** Thread A's fn recurses to depth 511 (traps);
   thread B's fn recurses to depth 10 (fine), concurrently. Assert A traps
   with `StackOverflow`, B completes. **Proves `call_depth` is per-context**
   (§1.2 item 3 — the lost-inc/false-trap race).

6. **Private-globals isolation.** Two threads, each with its own
   `GlobalsBlock`, each writing `global g` in a loop and reading it back.
   Assert each thread sees only its own writes (no cross-thread bleed).
   **Proves private-globals-per-context (§3 / D-private-globals).**

7. **`g_globals_for_codegen` emit-time fix (parallel compile).** Two threads
   concurrently `compile_func` against two *different* `GlobalsBlock`s
   (different global names/indices). Assert each compiled body references
   its own block's globals at the right indices (run each and check the
   right global is read). **Proves the §1.3 emit-time race is gone.** This
   test fails today against the unfixed `g_globals_for_codegen`.

8. **Dispatch-table sharing under concurrency.** Two threads call the same
   script fn through the shared `DispatchTable` concurrently (same slot,
   `table.get(slot)` on both threads). Assert both complete. **Proves §1.1's
   already-atomic slots are actually shareable in practice** (a sanity check
   that the already-safe path stays safe under the new model).

9. **`--tick` end-to-end (the §1.4 bug, fixed).** `ember_cli --tick` with a
   module whose `@on_tick` traps (e.g. a budget-exhausting tick fn). Assert
   the tick thread traps cleanly, the main thread's TUI/exit is unaffected,
   and no longjmp lands on the main thread. **The direct regression test for
   §1.4.** Under C-as-transitional-guard this should pass even before D+B1.

**What the test shape does NOT include (matches the deferrals):** no
`aint*`/script-level atomics test, no shared-globals-writers test, no
nested-`ember_call`-checkpoint-stack test, no cross-thread forced-unwind
test. Those are later batches.

---

## Appendix — file:line evidence index (for the implementer)

| Claim | Evidence |
|---|---|
| `context_t` fields are non-atomic | `src/context.hpp:44`–`72` |
| Single `jmp_buf` checkpoint (not the spec's stack) | `src/context.hpp:8`–`11`, `:47`–`48` |
| `budget_remaining` decremented at loop back-edges | `src/codegen.cpp:266`–`282` |
| `call_depth` inc/dec at script-to-script calls | `src/codegen.cpp:293`–`310`; call sites `:645`,`:653`,`:1563`,`:1572` |
| `trap_ctx` baked as imm64 | `src/codegen.cpp:242` |
| `budget_ptr` baked as imm64 | `src/codegen.cpp:268` |
| `depth_ptr` baked as imm64 | `src/codegen.cpp:295`, `:307` |
| `CodeGenCtx` carries the three pointers | `src/codegen.hpp:36`–`73` (`trap_stub`/`trap_ctx`/`budget_ptr`/`depth_ptr`) |
| `g_globals_for_codegen` process-wide pointer (defn) | `src/codegen.cpp:2059`; decl `src/codegen.hpp:95` |
| Globals emit-time decision reads process-wide ptr | `src/codegen.cpp:951`–`957` (load), `:1360`–`1365` (store) |
| Globals run-time address via `AbsFixup::GlobalsBase` | `src/codegen.cpp:878`–`900` (helpers), `:2289` (reloc fill) |
| Dispatch table is `std::atomic<void*>` release/acquire | `src/dispatch_table.hpp:6`–`13` |
| Slot indices stable; cache index not pointer | `docs/HOT_RELOAD.md §1`, `§4`, `§7` |
| `--tick` spawns a thread reusing one `ectx` (the live bug) | `examples/ember_cli.cpp:440`–`510`, esp. `:476`–`:481` |
| CLI calls JIT'd entry via raw fn-ptr (no `ember_call`) | `examples/ember_cli.cpp:432` (raw `reinterpret_cast<F0>(entry)()`); `src/engine.hpp`/`engine.cpp` have no `ember_call` |
| Trap stub `__builtin_longjmp`s the passed ctx | `examples/ember_cli.cpp:97`–`110` |
| Spec: per-`context_t` parallelism allowed, in-context not | `docs/SAFETY_AND_SANDBOX.md §8` |
| Hot reload: outer-call guards + concurrent epoch reclamation | `docs/HOT_RELOAD.md §5` |
| ROADMAP: `aint*`/`thread` deferred; multi-context covers most | `docs/ROADMAP.md` Tier 5 (`:119`–`125`) |
| REDSHELL: trap-surface / budgets / checkpoint spec | `EMBER_REDSHELL_WRITEUP.md` §0, V6-DoS, V7 (workspace root) |
