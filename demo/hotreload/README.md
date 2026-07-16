# Hot-reload demo

`hotreload_demo.cpp` combines three host-side features in one deterministic
harness:

1. write and reload a `.em` module;
2. replace one function through `HotReloadDomain`, then quiesce and reclaim old
   executable pages safely;
3. drive the dispatch slot in a fixed tick loop before and after publication.

Hot reload is a C++ embedding API, so this is intentionally a host harness, not
a pure `.ember` script.

## Build

From the repository root, after building `buildt`:

```bash
/c/msys64/mingw64/bin/g++.exe -std=c++17 -O2 -Isrc \
  demo/hotreload/hotreload_demo.cpp \
  buildt/libember_frontend.a buildt/libember.a buildt/libember_ed25519.a \
  -o demo/hotreload/hotreload_demo.exe
```

`libember_ed25519.a` is required by the current module writer/loader link. Keep
`libember_frontend.a` before `libember.a` for the MinGW static link.

Run:

```bash
./demo/hotreload/hotreload_demo.exe
```

It exits 0 and ends with:

```text
expected: 11 11 11 | 22 22 22 | 33 c1 33 c2 33 c3
actual:   11 11 11 | 22 22 22 | 33 c1 33 c2 33 c3
```

## Sequence

- Compile `render_v1.ember`, write a temporary `.em`, load it, and verify the
  loaded renderer emits `11`.
- Run three guarded v1 ticks.
- Reload `renderer` from v2, verify the publication epoch increases and the
  new page is callable, quiesce, then run three v2 ticks.
- Reload v3, repeat publication/reclamation checks, and run three stateful v3
  ticks.
- Verify two old pages were reclaimed, no retired pages remain, and the current
  v3 page still runs after reclamation.

Every outer call acquires `domain.guard()` before loading/calling the slot. On a
successful reload the harness disowns the old `CompiledFn` executable pointer
because the reload domain owns the retired page.

## Important reload constraints

A single-function reload may use existing module globals but cannot introduce a
new global. The initial module therefore declares `frame_count`, although v1
and v2 do not use it. The globals block and its typed index/offset/type maps are
created at initial compile; existing JIT pages depend on that fixed layout.

Both initial compilation and reload contexts must receive:

- `globals_base`;
- `globals_index`;
- `globals_types` (and current typed layout data where required).

The callability probe executes real code. The harness isolates its output and,
for stateful v3, reseeds `frame_count` before the measured tick sequence so the
probe does not shift `c1` to `c2`.

## Relation to current runtime

This demo exercises the legacy `HotReloadDomain` API directly. The repository
also contains keyed single-function and whole-generation reload paths; see the
keyed reload tests for those contracts. The local harness remains useful
because it combines ordinary dispatch, `.em` round-trip, epoch reclamation, and
a stateful deterministic sequence in one small program.
