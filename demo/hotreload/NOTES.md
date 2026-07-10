# hotreload demo — notes & kinks surfaced

A live-module hot-reload demo (`hotreload_demo.cpp`) that exercises the THREE
runtime features the prior demo (demo/NOTES.md) skipped:

1. **the `.em` bundler** — `write_em_file` / `load_em_file`
   (BUNDLING_AND_EM_MODULES.md). The harness compiles `render_v1.ember`,
   serializes it to a temp `.em`, loads it back in a fresh code path, and calls
   the loaded renderer to prove the bundle round-trips the same behavior as the
   JIT'd page (the em_roundtrip_test shape, applied to a reloadable renderer).

2. **HotReloadDomain (M3 epoch reclamation)** — `reload_function` +
   `ExecutionGuard` + `publish` + `reclaim`/`quiesce` (HOT_RELOAD.md). The
   harness swaps the `renderer` function body live, mid-tick-loop, from
   v1 → v2 → v3, and asserts each reload publishes a monotonic epoch, the new
   page is callable immediately after publication, and the old page is safely
   retired + freed via `quiesce` (no UAF: the newest page is still callable
   after the retired pages are freed).

3. **`--tick` together** — the host runs a fixed, DETERMINISTIC tick loop (no
   wall-clock, no keybind) that calls the `renderer` slot N times between
   reloads. The reload happens between ticks; the next tick after publication
   runs the NEW body, proving the swap takes effect atomically.

## Status: works end-to-end, deterministic

The harness builds with the pinned toolchain:

```
/c/msys64/mingw64/bin/g++.exe -std=c++17 -O2 -Isrc \
  demo/hotreload/hotreload_demo.cpp buildt/libember_frontend.a buildt/libember.a \
  -o demo/hotreload/hotreload_demo.exe
```

(link order: frontend before ember; no extension include dirs needed — the
harness uses `BindingBuilder`, not the extension headers directly.)

`hotreload_demo.exe` exits 0 with every assertion PASS. The deterministic
reload-sequence output is exactly:

```
11 11 11 | 22 22 22 | 33 c1 33 c2 33 c3
```

Read as: 3 ticks of v1 (marker 11) | reload to v2 → 3 ticks (marker 22) |
reload to v3 → 3 ticks (marker 33 + advancing counter c1, c2, c3).

## Why this is a C++ harness, not a .ember script

`reload_function` and `HotReloadDomain` are **host C++ APIs** (inline header
in `src/hot_reload.hpp`). A pure-ember script CANNOT call them — the reload is
a host-driven operation over a compiled module's dispatch table. So the demo is
a C++ host harness that uses the ember frontend + JIT to compile/run a script
`renderer`, and the host drives the reload. This mirrors
`examples/v0_6_hot_reload_test.cpp` (the reload shape) +
`examples/game_host.cpp` (the game-host compile + tick shape) +
`examples/em_roundtrip_test.cpp` (the `.em` bundler shape), combined into one
end-to-end live-reload demonstration.

## The reload sequence (what the harness does)

1. Register `emit_frame(code: i64) -> void` and `emit_count(c: i64) -> void`
   natives via `BindingBuilder` (both append to a host output buffer — the
   deterministic signal).
2. Compile `render_v1.ember` into a `LiveModule` with a persistent
   `HotReloadDomain` beside the `DispatchTable` (HOT_RELOAD.md §0 step 1).
3. **Feature 1 — bundler round-trip:** emit the v1 module to a temp `.em` via
   `write_em_file`, load it back via `load_em_file`, call the loaded renderer,
   assert it produces `"11 "` (same as the JIT'd v1). Remove the temp file.
4. **Features 2+3 — tick loop + hot reload:**
   - 3 ticks of v1 → `"11 11 11 "`.
   - `reload_function(v2_src, ...)` → assert monotonic epoch, old page retired,
     new page callable immediately, `quiesce` frees the v1 page (no UAF).
   - 3 ticks of v2 → `"22 22 22 "` (the marker switched atomically).
   - `reload_function(v3_src, ...)` → same assertions; `quiesce` frees v2.
   - 3 ticks of v3 → `"33 c1 33 c2 33 c3 "` (counter advances).
5. **No-UAF summary:** assert 2 pages reclaimed, 0 retired pending, and the v3
   page is still callable AFTER both old pages were freed (a 4th v3 tick
   produces `"33 c4 "` — would fault if `quiesce` had freed the live page).
6. Assert the full sequence string equals the expected deterministic output.

Every outer host-to-script call is wrapped in a `domain.guard()` before
loading the slot (HOT_RELOAD.md §0 step 2). On a successful reload the harness
disowns the old `CompiledFn` (nulls `exec`/`entry`) so the destructor does not
double-free it (the domain owns the retired page).

## Kinks surfaced (the real deliverable)

### Kink 1 — the immediate-callability probe polluted the deterministic output buffer

The "new page callable immediately after publication" assertion called the
renderer once, which appended to the shared host output buffer. So the v2
sequence read `"22 22 22 22"` (4× instead of 3×) — the probe's tick was
inseparable from the sequence's ticks.

**Judgment: fix the demo.** Introduced `probe_call`: save `g_out`, clear it,
call, restore. The probe now proves callability on a throwaway buffer without
touching the sequence capture. (Not a runtime bug — the probe genuinely
executed the new page; the issue was the harness double-counting its output.)

### Kink 2 — a reload CANNOT introduce a new global (real reload-model constraint)

The first v3 source declared `global frame_count : i64 = 0;` AND `fn renderer`.
`reload_function` ran whole-module sema against the existing `prog` (whose
`globals` reflected the v1 module — no `frame_count`), so the v3 body's
reference to `frame_count` failed sema: `undefined name 'frame_count'`.

**Judgment: fix the demo.** This is NOT a runtime bug — it is a real
reload-model constraint. `reload_function` swaps ONE function body and re-runs
whole-module sema against the existing `prog.globals` (which is fixed at
initial compile). The globals block is sized once (`gbs.assign(prog.globals.size()*8, 0)`)
and its base is baked as an absolute imm64 into every already-JIT'd page;
growing it on reload would move `.data()` and invalidate those baked
addresses. So a reload can use any PRE-EXISTING global but cannot INTRODUCE one.

**Fix:** the initial module (`render_v1.ember`) declares
`global frame_count : i64 = 0;` (the counter, unused by v1/v2). v3's reloaded
body reads/writes that pre-existing global. This is the correct shape: the
global is part of the module's fixed globals block from initial compile; reload
only swaps the renderer's executable page, which can use any existing global.
The demo's source-file comments document this constraint explicitly so the
next person doesn't repeat the mistake.

### Kink 3 — a reloaded body reading a global needs the globals index threaded into CodeGenCtx

After fixing Kink 2 (v3 compiled + published), the counter read garbage
(`c-1614696687`) — uninitialized memory, not 0.

**Judgment: fix the demo.** Codegen resolves a global's slot index via
`ctx.globals_index` (the v1.0 thread-safe path) OR falls back to the
process-global `g_globals_for_codegen->index`. My `compile_module` and
`make_ctx` set NEITHER — only `ctx.globals_base`. The initial v1 compile
happened to work because v1's body never reads `frame_count` (the global was
sized into the block but never touched). The v3 reload reads `frame_count`,
and with no index resolution it emitted a wild `[globals_base + garbage]`
read.

**Fix:** thread `ctx.globals_index = &m.gb.index` and `ctx.globals_types = &m.gb.types`
into both `compile_module` and `make_ctx` (the CLI's v1.0 thread-safe path;
`game_host`/`em_roundtrip` use the legacy `g_globals_for_codegen` pointer
instead — both work, the ctx path is preferred for parallel-compile safety).
This is a demo-harness omission, not a codegen bug.

### Kink 4 — the immediate-callability probe mutates shared global state

After Kink 3, the counter worked but was offset by one: `c2 c3 c4` instead of
`c1 c2 c3`. The v3 "callable immediately" probe had executed v3 once, bumping
`frame_count` 0→1 — and that mutation is NOT discarded (only the probe's
*output buffer* is discarded; the globals-block write is real and persists).

**Judgment: fix the demo.** Correct runtime behavior — the probe was a real
call, not a no-op peek; mutating the globals block is what "callable" means.
For a clean deterministic v3 sequence, the harness reseeds `frame_count = 0`
(memset the global's 8 bytes) after the probe, before the v3 tick loop. This
is the host seeding global state before a phase — exactly the pattern the docs
prescribe ("a host or @entry seeds globals"). v2's probe doesn't touch
`frame_count`, so v2 needed no reseed.

## Features confirmed working (the three the prior demo skipped)

- `.em` bundler round-trip (write → load → call) for a reloadable renderer —
  the loaded page produces the same behavior as the JIT'd page.
- `HotReloadDomain` epoch reclamation: monotonic publication epochs, old-page
  retirement on publication, `quiesce` frees retired pages, no UAF (newest
  page callable after reclamation), reclaimed-page counter tracks frees.
- `--tick`-style host tick loop driving a renderer slot under a per-call
  `ExecutionGuard`, with live reload between ticks taking effect atomically on
  the next tick.
- A reloaded body can read/write a PRE-EXISTING module global (the counter),
  carrying mutable state across ticks within its version.

## What this demo did NOT need to fix in the runtime

No `hot_reload.hpp` / runtime bugs were found. Every kink was a demo-harness
mistake (output-buffer pollution, asking a reload to introduce a new global,
forgetting to thread the globals index, probe side-effects on shared state).
The HotReloadDomain API behaved correctly throughout — monotonic epochs,
correct retirement, safe reclamation, no UAF. No regression test was needed
because no runtime code was changed.

## Gate / regression check

- `ninja` in `buildt/`: succeeds (no work to do — the harness is a standalone
  `.exe`, NOT in the CMake build, so it cannot regress the gate).
- `ninja test`: 20/20 tests pass (including `v0_6_hot_reload`,
  `em_roundtrip`, `game_host_integration`, `v0_5_live_modules`,
  `em_loader_hardening`, `v0_6_lifecycle`).
- The harness is built separately with the pinned g++ toolchain; it does not
  touch the CMake-built executables.
