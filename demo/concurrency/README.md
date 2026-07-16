# Concurrency demo

This directory contains two complementary harnesses for `ember_ext_sync`.

- `concurrency_demo.cpp` + `race.ember`: real host threads contend on shared
  SPSC/MPSC/MPMC queues and an atomic.
- `tick_demo.cpp` + `tick_sim.ember`: one context runs a deterministic 200-tick
  producer/consumer/swap-buffer pipeline.

The committed executables are convenience artifacts; rebuild from source when
validating the current runtime.

## Build

From the repository root with the supported MinGW toolchain:

```bash
CXX=/c/msys64/mingw64/bin/g++.exe

$CXX -std=c++17 -O2 -pthread -Isrc -Iextensions/sync \
  demo/concurrency/concurrency_demo.cpp \
  buildt/libember_ext_sync.a buildt/libember_frontend.a \
  buildt/libember_import.a buildt/libember.a \
  -o demo/concurrency/concurrency_demo.exe

$CXX -std=c++17 -O2 -pthread -Isrc -Iextensions/sync \
  demo/concurrency/tick_demo.cpp \
  buildt/libember_ext_sync.a buildt/libember_frontend.a \
  buildt/libember_import.a buildt/libember.a \
  -o demo/concurrency/tick_demo.exe
```

Static-library order matters with MinGW: extension/frontend/import libraries
precede `libember.a` in the commands above.

## Run and expected results

```bash
./demo/concurrency/concurrency_demo.exe
./demo/concurrency/tick_demo.exe
```

Both exit 0 on success. Shape A checks:

- SPSC: 100,000 values, exact FIFO and no loss;
- MPSC: two producers of 50,000 values, all 100,000 drained;
- MPMC: two producers and two stateless consumers, 40,000 total drained;
- atomic: four threads x 50,000 `fetch_add`, final value 200,000.

Shape B reports 14 passing checks and the deterministic summary:

```text
produced=600 consumed=600 back_gen=199 back_items=3 spsc_size=0
```

## Concurrency contract

A `context_t` is per concurrent Ember entry. Shape A compiles one function body
with `use_context_reg=true`, then gives every host worker its own context while
sharing the dispatch table, globals block, and extension store. Never enter the
same `context_t` concurrently.

The queues and atomics synchronize their own storage. Ordinary Ember globals do
not become atomic merely because several contexts share them. Use one writer,
an extension atomic, or host-thread-local accumulation for shared mutable
state. The MPMC consumers therefore return per-call counts to their host
threads instead of racing on a plain global counter.

The Ember thread extension now provides script-visible `thread_spawn` and
`thread_join`, and coroutines provide `yield` on Windows. This demo deliberately
uses host-driven workers because it is testing the sync extension and the
per-context embedding contract directly.

## API details demonstrated

- Sync handles are nominal (`atomic`, `spsc`, `mpsc`, `mpmc`, `swapbuf`), not
  interchangeable with plain `i64` in source even though the ABI word is i64.
- MPSC producers use the `spsc` producer sub-handle returned by
  `mpsc_producer_handle`.
- `m.reseed_globals()` is the correct harness reset: zeroing the raw globals
  block would also erase initialized constants such as `BATCH` and `RETRIES`.
- Handle globals need a type-compatible initializer, for example
  `global h: spsc = spsc_new(4);`; `setup()` then replaces the initial zero
  produced by non-constant global-call initialization.
- Queue `size` observations are approximate under contention. Shape A does not
  use them as a no-loss proof; shape B is single-threaded, where the final size
  is deterministic.
- Bounded batches return to the host, which can yield and enforce a deadline,
  instead of busy-spinning indefinitely inside JIT code.
