# Game simulation demo

A deterministic multi-file entity simulation exercising Ember's game-host
surface end to end:

- `entity.ember`: `Entity` plus `array<f32>`/`array<i64>` SoA storage;
- `physics.ember`: semi-implicit Euler, collision response, vec3 overloads,
  script structs, `mat4`, `quat`, and `sqrt`;
- `spawn.ember`: dynamic lifecycle routine registration and self-removal;
- `main.ember`: entry setup, tick processing, and a tick-20 assertion.

## Run

Build the current CLI first, then run from the repository root:

```bash
./buildt/ember_cli.exe run demo/game/main.ember \
  --tick --tick-count 20 --tick-interval 1
```

Expected output ends with:

```text
ember: stopped after 20 ticks
```

The process exits **1**, not 0: `main` intentionally returns a positive
stay-loaded lifecycle value. A tick-time trap exits 70.

## Deterministic check

Entity 0 starts at position `(0, 50)` with velocity `(5, 10)`, gravity `-9.8`,
and `dt=0.1`. Semi-implicit Euler updates velocity before position. At tick 20:

```text
x = 10.0

y = 50 + 10*0.1*20 + 0.5*(-9.8)*0.1^2*20*21 = 49.42
```

The script accepts f32 error below 0.001 and also requires
`frame_counter == 20`. The dynamic spawner runs every five ticks and grows the
entity count from three to six.

## Extension and lifecycle coverage

- vec3 construction/component reads and overloaded addition on the hot path;
- mat4 identity/read and quaternion construction/read probes;
- f32 `sqrt` from the math extension;
- typed host-backed arrays for entity storage;
- `@entry`, `@on_tick`, `register_routine`, and `unregister_routine`;
- the CLI's fixed-count tick thread and recoverable-trap exit path.

Opaque handles are nominal types. Declare a value returned by `vec3_new` as
`vec3` (likewise `quat`, `mat4`, and sync handle types), not `i64`. A nominal
handle cannot be compared with a plain integer zero; probe a meaningful
component instead. The array extension is an older plain-i64 handle surface,
so its handles can be stored as `i64`.

`quat_new` takes `(x, y, z, w)`, making identity
`quat_new(0.0f, 0.0f, 0.0f, 1.0f)`.

## Current language status

Several workarounds preserved in the source are no longer required:

- aggregate globals, array literals, direct struct-literal returns, and
  struct-returning-call arguments are implemented;
- string/console I/O exists in `ember_ext_io`, but is `PERM_FFI`-gated. This
  demo intentionally uses a recoverable trap as its assertion channel and
  therefore needs no `--ffi`;
- lifecycle entry sign handling and tick-trap exit propagation are fixed and
  covered by `lifecycle_entry_unload.ember` and
  `lifecycle_tick_trap_exit.ember`.

Imports are textual, cycle-safe whole-program inclusion. Sema constructs symbol
tables for the complete resolved program, so the layered entity/physics/spawn
references resolve even when textual order differs from call direction.
