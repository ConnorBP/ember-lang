# demo/game — game-entity simulation: notes & kinks surfaced

A multi-file **game-entity simulation** — the language's headline use case. Four
modules (`entity.ember` → `physics.ember`, `spawn.ember`, `main.ember`) exercise
the mostly-untouched **vec/quat/mat extensions**, the **`--tick` runtime**, the
**`@entry` / `@on_tick`** annotations, **`register_routine` / `unregister_routine`**
(the lifecycle extension's dynamic-routine path), and the **`array<T>` extension**
as the SoA backing store — together, end-to-end, on a deterministic sim.

## What the sim does

- **`entity.ember`** — `struct Entity` (AoS view) + an `array<f32>` SoA store
  (`store_pos_vel`, 4 f32s/entity: pos_x/y, vel_x/y) and an `array<i64>` active
  flag store (`store_active`). `init_store` / `seed_entity` / `load_entity` /
  `store_entity` / `entity_pos_x|y` manage it. Globals are scalars only (audit M11).
- **`physics.ember`** — `integrate` (semi-implicit Euler) + `bounce` (elastic
  wall collision). **Three representations of a 2D position interplay on the hot
  path**: (1) the **host vec3 extension** (opaque `vec3` handles — velocity is
  lifted via `vec3_new`, gravity added via the overloaded `+` → `vec3_add` native,
  read back via `vec3_x/y`); (2) a **script-level `V3` struct** (position is a
  script `V3`, added via `v3_add` — struct-by-value, both args locals per the
  Win64 ABI restriction); (3) the **`array<f32>` SoA store**. `probe_extensions`
  also calls `mat4_identity` + `mat4_get` (mat extension) and `quat_new` +
  `quat_w` (quat extension) to prove all three math extensions are wired and
  readable on the game-entity path. `sqrt` (math extension) is called in `bounce`.
- **`spawn.ember`** — a **dynamically-registered spawner routine** (the lifecycle
  extension's headline use). `@entry` calls `register_spawner(5)`, which takes
  `&spawner` (a `fn` handle) and calls `register_routine(h, 5)` → a 1-based
  routine id. The host's `--tick` loop re-snapshots `host_routines()` each tick
  and calls `spawner(5)`. The spawner seeds a new entity every 5 ticks; when the
  arena fills it calls `unregister_routine(spawner_id)` to **self-remove** — the
  dynamic lifecycle's self-removal path.
- **`main.ember`** — `@entry` sets up the store, probes the extensions, seeds 3
  deterministic entities, registers the spawner, returns 1 (stay-loaded).
  `@on_tick` steps every live entity and bumps `frame_counter`. At tick 20 it
  runs the **deterministic assertion**: entity 0 must be at the closed-form
  semi-implicit-Euler position, and the frame counter must equal the tick count.

## Run

```
./buildt/ember_cli.exe run demo/game/main.ember --tick --tick-count 20 --tick-interval 1
```

- **Success (deterministic)**: `ember: stopped after 20 ticks` (no trap), exit 1.
  5/5 runs identical. Breaking the closed form → `stopped after 19 ticks (a tick
  trapped ...)`, exit 70 — the assertion is meaningful (it catches a regression
  in the integrator or the closed form).

## The deterministic assertion result

Entity 0 starts at `pos=(0,50)`, `vel=(5,10)`, under gravity `g=-9.8`, `dt=0.1`,
semi-implicit Euler (velocity updated first, then position uses the new
velocity). The discrete closed form (per step k=1..t: `v_k = v_0 + k*g*dt`;
`p_t = p_0 + dt·Σ v_k`) is:

```
px(20) = v0_x · dt · t = 5 · 0.1 · 20 = 10.0
py(20) = py0 + vy0·dt·t + 0.5·g·dt²·t·(t+1) = 50 + 20 + 0.5·(-9.8)·0.01·20·21 = 49.42
```

At tick 20 the sim's integrated entity 0 matches the closed form to within f32
noise (drift < 1e-5, well under the 0.001 epsilon). The frame counter == 20
(guaranteed structurally — `@on_tick` is called once per tick and bumps it once).
The spawner grew `entity_count` from 3 to 6 (dynamic-routine path fired).

## Kinks surfaced (the real deliverable)

### Real language/tooling bugs the demo surfaced → fixed + locked in with regression tests

**G1. `@entry` return-sign clamp corrupted the lifecycle signal.** The CLI did
`exit_code = int(r & 0x7fffffff)` then `entry_says_stay = (exit_code > 0)`. The
clamp turns a **negative** unload return (e.g. `-2`) into a **large positive**
(`0x7ffffffe`), so `> 0` was TRUE and `--tick` started ticking a module that had
asked to unload (../../docs/LIFECYCLE.md §1: `<= 0 ⇒ unload`). The demo hit this directly:
`probe_extensions` initially failed (see G3) and `@entry` returned `-2`, which
the clamp read as `254 > 0` → stay-loaded → the tick thread ran with **0 dynamic
routines** (the spawner was never registered because main returned early at the
probe check). **Fix**: capture the **signed** return (`entry_ret`) separately; use
`entry_ret > 0` for the lifecycle decision, and the clamp only for the process
exit code. (The source fix was already in HEAD commit 5568af6; the demo surfaced
that the **`buildt/ember_cli.exe` binary was stale** — built before that fix landed —
so the buggy behavior was live at runtime. Rebuilding picked up the fix.) **Regression**:
`tests/lang/lifecycle_entry_unload.ember` (asserts a negative `@entry` does NOT
start the tick loop).

**G2. A tick-time trap did not propagate to the process exit code.** In
`--tick` mode, a trapped tick printed `(a tick trapped ...)` but the process
exited with `@entry`'s positive stay-loaded return (e.g. 1) — so a test harness
could **not distinguish a clean tick run from a tick-time assertion failure by
exit code**. This directly undermined the demo's mandate: the only host-observable
failure channel for a tick-time assertion (no `print`/`log` native; `@on_tick`
return values are discarded by the host's tick loop) was human-readable print, not
a programmatic exit code. **Fix**: when `tick_trapped` is true, set `exit_code = 70`
(the same recoverable-trap code used for a trap in the main `@entry` call). (As
with G1, the source fix was already in HEAD 5568af6; the stale binary was the live
bug. Rebuilding fixed it; the regression test below locks it in.) **Regression**:
`tests/lang/lifecycle_tick_trap_exit.ember` (asserts a tick trap exits 70 + prints
the trap line). Now the demo's deterministic assertion is **harness-observable**:
pass = exit 1, fail = exit 70.

### Language limitations worked around in the demo (documented, not bugs)

**L1. Host vec/quat/mat handles are nominal types, not `i64`.** `vec3_new(...)`
returns a value typed `vec3` by sema (`bind_handle` tags `struct_name`), not a
plain `i64`. So `let v : i64 = vec3_new(...)` is a **type mismatch** ("let type
mismatch (i64 = vec3)"). A script must annotate `let v : vec3 = vec3_new(...)`.
The same holds for `mat4`/`quat`/`vec2`/`vec4` and the sync-extension handle types
(`atomic`/`spsc`/`mpsc`/`mpmc`/`swapbuf`). This is **correct** (the nominal typing
prevents forging a handle from an integer — `sema_invalid_integer_to_handle.ember`
asserts `let bad: vec3 = 999` is rejected). It's a surface that was **under-exercised**:
the prior `demo/concurrency/tick_sim.ember` declared sync handles as `: i64` and
so never compiled (broken at sema). The demo annotates every handle with its
nominal type.

**L2. Tagged handles cannot be compared to integer `0`.** `if (m == 0)` for a
`mat4` handle is rejected ("comparison requires same-type operands (got mat4 and
i64)") — handles are nominal, not forgeable, and `0` is an `i64`. To **probe a
handle for liveness**, read a component: `mat4_get(m, 0, 0) == 1.0f` (identity's
[0][0]) and `quat_w(q) == 1.0f` (identity quat's w). This is a better probe
anyway (it proves the handle is valid AND readable), and is what `probe_extensions`
does. (The `array_new` return is a **plain `i64`**, not a tagged handle, so
`store_pos_vel == 0` is a valid null-check — only the vec/quat/mat/sync handles
are nominal.)

**L3. `quat_new` parameter order is `(x, y, z, w)`, not `(w, x, y, z)`.** The
identity quaternion is `quat_new(0, 0, 0, 1)`; `quat_new(1, 0, 0, 0)` has `w=0`.
Standard order (matches the host struct `Quat { x, y, z, w }`), but the comment
in the first draft of the demo wrongly said `(w, x, y, z)` — the demo's
`probe_extensions` initially returned 0 (it built `(1,0,0,0)` expecting
identity, got `quat_w == 0`). Fixed to `quat_new(0, 0, 0, 1)`.

**L4. Struct-return + struct-by-value-arg ABI restrictions (Win64 hidden
pointer).** ~~A fn cannot `return Entity { ... }` directly — must `let r : Entity
= Entity { ... }; return r;`. A struct-by-value arg must be a plain local — not a
struct literal or a struct-returning fn call.~~ **RESOLVED:** both restrictions
are relaxed — a fn may now `return Entity { ... };` directly and a struct-by-
value arg may be a struct literal or a struct-returning fn call (codegen
materializes these into a compiler-hidden temp frame slot). The demo's
workarounds (stashing every struct return in a local, passing only locals as
struct args) still work but are no longer required; pinned by the non-circular
`binding_abi_test` probes [2c] (struct-literal return), [2d] (`v3_dot(v3_up(),
v3_up())` — struct-ret-call as arg), and [2e] (nested struct-ret-call arg).
These were the same restrictions `../NOTES.md` (the prior demo) recorded
(M10-adjacent).

**L5. No aggregate global initialization (audit M11).** Globals accept only
scalar initializers. The SoA store handles (`store_pos_vel`, `store_active`) are
`global : i64 = 0` (scalar), seeded by `init_store` in `@entry`. No struct/array
literals at global scope. Consistent with the prior demo.

   **RESOLVED (first-class struct / aggregate pass, 2026-07-10):** aggregate
globals now ship — struct, fixed-array, and slice globals all type-check,
initialize, load, and store, with the globals table carrying typed per-global
offsets/sizes (slices 16 bytes ptr+len, structs/arrays their full layout). The
`init_store`-in-`@entry` workaround still compiles and is still the right shape
for this sim (the SoA store handles are host-created `array<T>` extension
handles, not script aggregate literals), but a struct/array/slice *value* global
is no longer rejected. Pinned by the non-circular `aggregate_global_test` ctest
probes [1]-[8]. Documented in `../../docs/spec/TYPE_SYSTEM.md` §12.2 and
`../../docs/spec/CODEGEN_SPEC.md` §16.

**L6. `let` bindings are immutable; `let mut` opts into reassignment.** The
`bounce` locals (`px`, `py`, `vx`, `vy`) and the `approx_eq` slack (`d`) must be
`let mut` to reassign. Globals are storage, not bindings, so `frame_counter =
...` (no `mut`) is fine. Standard Rust-style; documented here because the sim
reassigns locals in the hot path.

**L7. `fn` handles are a distinct `fn` type, not `i64`.** `let h = &spawner` (no
annotation — inferred `fn`); `register_routine(h, 5)` takes a `fn_handle`-tagged
i64 param; a fn-taking-fn param is typed `f: fn`. A `fn` cannot be `return`-ed
from an `i64` fn nor cast `as i64` (nominal, like vec3). Correct behavior
(`function_refs_test` A2); the spawner uses it as designed.

**L8. Imports are textual concatenation into one flat-namespace program.**
`resolve_imports` inlines imported files' text (idempotent on cycles) and parses
the result as one `Program`. So `physics.ember` uses `entity.ember`'s `Entity`
struct, `gravity`/`bounds_x` globals, and `load_entity`/`store_entity` fns
**without importing entity** — they're all in one program regardless of textual
order (sema builds the struct/global/fn tables whole-program). The import graph
`main → entity → physics` + `main → spawn → entity` works, including the
`entity ↔ physics` reference cycle (entity imports physics; physics uses entity's
symbols). This is why the demo can have a real layered architecture (physics is a
leaf that references the entity store) rather than forcing every cross-module use
to flow importer→importee.

### Observability gap worked around (documented; the workaround is the design)

**O1. No `print`/`log` native in the standard CLI extensions; `@on_tick` return
values are discarded by the host's tick loop.** So a tick-time assertion has **no
direct output channel** — the script cannot print "assertion failed" nor surface
a value via a return. The demo's design uses the **recoverable-trap surface**
(REDSHELL V6/V7) as the assertion's observable failure signal: on assertion
failure, `raise_assert_trap` does `1 / 0` → integer divide-by-zero → the tick
thread longjmps to its checkpoint → the CLI reports `(a tick trapped ...)`.
Combined with the **G2 fix** (tick-trap → exit 70), the assertion is now
harness-observable. On success the sim runs cleanly to `--tick-count N` with no
trap (exit 1). This is a deliberate, documented design choice, not a bug — the
trap surface is the language's "script signals a fatal invariant violation"
channel, and using it for a deterministic assertion is a legitimate application
of that surface. A future `print`/`assert` native would make this more ergonomic,
but adding a native is a feature, not a bug fix, so it's out of scope here.

## Features confirmed working (the headline use case, end-to-end)

vec3 extension on the hot path (`vec3_new` / `vec3_x` / `vec3_y` / overloaded `+`
→ `vec3_add`) · mat extension (`mat4_identity` / `mat4_get`) · quat extension
(`quat_new` / `quat_w`) · math extension (`sqrt`) · `array<f32>` + `array<i64>`
SoA backing store (`array_new` / `array_set_f32` / `array_get_f32` / `array_set_i64`
/ `array_get_i64`) · `@entry` lifecycle (return > 0 = stay loaded) · `@on_tick`
discovered by `get_annotated_functions` · `register_routine` / `unregister_routine`
dynamic routine (the lifecycle extension, with self-removal on arena-full) · the
`--tick` runtime (tick thread, B1 per-call context) · multi-module imports with a
reference cycle · script `struct` by value across modules · a deterministic
closed-form assertion of integrated state after a fixed tick count.
