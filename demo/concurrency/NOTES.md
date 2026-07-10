# Concurrency demo — notes & kinks surfaced

A two-shape demo exercising the ember sync extension's ~28-native surface (atomics, swapbuf, SPSC/MPSC/MPMC queues) under the single-context contract. Shape (A) is the real concurrency test (threads racing through shared queues); shape (B) is the API-ergonomics + tick-integration test (single-thread pipeline).

## Status: both shapes pass, deterministically

- **Shape (A) `concurrency_demo.exe`**: SPSC, MPSC, MPMC, ATOMIC all PASS. 5/5 runs green. Real threads, real contention, no-loss + FIFO verified.
- **Shape (B) `tick_demo.exe`**: 14/14 assertions PASS. 3/3 runs green. Deterministic: produced=600, consumed=600, back frame gen=199 items=3, SPSC empty.
- **Gate**: `ninja` succeeds; `ext_sync_test.exe` still PASSES (no regression — `extensions/sync/` untouched).
- **Extension fixes made**: **none.** Every kink was demo-side (the single-context contract shaping the design, not an extension bug).

## The pipeline design

### Shape (A) — host-driven real concurrency

The single-context contract (`src/context.hpp`: a `context_t` is NOT entered concurrently by multiple threads) means a pure-ember script cannot spawn threads. The host-driven shape mirrors `examples/thread_safety_test.cpp`:

1. **Compile once** with `use_context_reg=true` (B1 mode): the SAME compiled entry serves N per-thread `context_t`s (r14 = per-call ctx). The sync natives are registered; the module's globals use the v1.0 thread-safe globals path (`ctx.globals_index` threaded, `g_globals_for_codegen = nullptr`).
2. **Shared sync primitives created on the host** via the registered natives' fn_ptrs (`native_ptr<...>(m.natives, "spsc_new")` — the same fn_ptr the JIT would call). These live in the extension's process-global `g_store`, so all threads race through the SAME rings.
3. **N threads, each own `context_t`**, call the same compiled ember fn via `ember_call_i64(entry, &thread_ctx, handle)`. The handle is passed as the single i64 arg; the queues are process-global so threads genuinely race.

**Work discipline (bounded batch per call + host re-call loop):** ember has no script-visible `yield` (the sync extension is explicit about this — no script-visible mutex/yield). A busy-wait-retry loop inside an ember fn would spin in JIT'd code, burning the context budget (or starving a partner on a contended core). So each ember fn does a BOUNDED batch (`BATCH=64` items, `RETRIES=64` per item) then RETURNS to the host, which `std::this_thread::yield()`s and re-calls. Progress persists in per-role globals (shared backing store, single writer per global → no race). This mirrors how a real host drives staged script work per frame, keeps each ember call bounded (no budget exhaustion on a legit busy-wait), and gives the host the deadline + yield.

**The four tests:**
- **SPSC**: 1 producer (push 1..100000) + 1 consumer (verify FIFO `v == count+1` + no-loss `count == 100000`), cap 256 (rounded). Small cap forces real contention (producer fills the ring, waits for consumer).
- **MPSC**: 2 producers (base 0 + base 100000, 50000 each, each on its own producer sub-handle from `mpsc_producer_handle`) + 1 consumer (drain 100000). Cap 256 per producer ring.
- **MPMC**: 2 producers (20000 each) + 2 stateless consumers (cap 2048). No-loss = total consumed == 40000.
- **ATOMIC**: 4 threads each `fetch_add` 50000 on ONE shared atomic. Final == 200000. This is the M1 race-safety fix test (concurrent `fetch_add` must not lose increments).

**Determinism + assertions:** every test is bounded (30s `TIMEOUT` deadline, fixed work counts). SPSC asserts FIFO + no-loss; MPSC asserts both producers finished + consumer drained 2N; MPMC asserts producers done + 2 consumers drained 2*MPMC_N; ATOMIC asserts `val == 4*50000`. The progress globals are read back from the shared globals store (or the atomic via `atomic_load`) after threads join.

### Shape (B) — single-thread tick sim

An ember-only pipeline (`tick_sim.ember`) driven by a non-interactive tick harness (mirrors `ember_cli`'s `--tick` loop but fixed count, no keypress):

1. **`@entry setup()`** creates the primitives (SPSC cap 4, 2 atomics for produced/consumed counters, a 2-cell swapbuf, a tick_gen atomic) and stores handles in globals.
2. **`@on_tick producer()`** pushes `PER_TICK=3` items (value `gen*1000+i`) into the SPSC with a bounded retry-on-full loop, bumps the produced atomic, publishes a frame `[gen, items_this_tick]` to the swapbuf front, and `swapbuf_swap()`s (publishes).
3. **`@on_tick consumer()`** drains the SPSC until empty (size-guarded, single-threaded so `spsc_size` is exact), bumping the consumed atomic per pop.
4. The host ticks 200 times, then reads back via `_host` accessors: `atomic_load` the counters, `swapbuf_back_ptr` the back frame, `spsc_size` the queue.

**Determinism:** produced == 200*3 == 600; consumed == 600 (consumer drains each tick, no loss); back frame gen == 199 (last published), items == 3; SPSC empty. All asserted.

## Kinks surfaced (the real deliverable)

### K1: typed handle params — sema rejects `h: i64` passed to `spsc_push`

**Symptom:** `sema: arg 1 of 'spsc_push': expected spsc, got i64`.

**Cause:** the sync extension tags each handle distinctly via `bind_handle("spsc")` / `"mpsc"` / `"mpmc"` / `"atomic"` / `"swapbuf")` (`ext_sync.cpp::register_natives`). `Type::same` checks `struct_name` equality, so a plain `i64` is NOT `same` as an `spsc` handle. My fns declared `h: i64`; sema correctly rejected them.

**Fix (demo):** declare params with their tag: `fn spsc_producer_step(h: spsc)`, `fn mpsc_consumer_step(h: mpsc)`, `fn mpmc_consumer_step(h: mpmc)`, `fn atomic_bumper(h: atomic)`. Notably `mpsc_push` takes the PRODUCER sub-handle which is an `spsc` (the MPSC container holds per-producer SPSC rings), so `fn mpsc_producer_a_step(ph: spsc)`. The tag is purely a compile-time type-system thing; at the ABI the handle is an i64 (prim=I64), so `ember_call_i64` passing the i64 handle works. This is the same pattern as `demo/game/physics.ember`'s `fn to_vec3(...) -> vec3` / `let vel_h : vec3 = ...`.

**Judge: fix-the-demo.** This is the documented typed-handle design (mirrors ext_array/ext_vec), not an extension bug.

### K2: `budget exceeded` trap on the first queue-fn call — caused by my `memset` zeroing the const globals

**Symptom:** SPSC/MPSC/MPMC producer+consumer all trapped `budget exceeded` on the first call (`last=0`); ATOMIC passed.

**Cause (diagnosed via capturing `ectx.last_trap`):** I did `std::memset(m.globals_store.data(), 0, ...)` before each test to "reset progress globals." But the const globals `BATCH` and `RETRIES` (`const BATCH : i64 = 64`) are ALSO in the globals store, and const globals are read at runtime (NOT folded as immediates — `codegen.cpp`'s Ident path reads them via `load_global_to_rax` from the backing store). The memset zeroed `BATCH=0` and `RETRIES=0`. With `BATCH=0`, the producer's outer loop `while (pushed < BATCH)` = `while (0 < 0)` never entered → returned 0 → host re-called → looped. But the *consumer*, with an empty queue and `spsc_size==0`, returned immediately, and the host re-called in a tight loop. The budget (not reset between calls) drained across the rapid re-calls → `budget exceeded`. The ATOMIC test passed because `atomic_bumper` is a simple bounded loop that doesn't read `BATCH`/`RETRIES`.

**Fix (demo):** replaced the three `memset` calls with `m.reseed_globals()`, which zeros the store THEN re-runs `eval_global_initializers(program, ...)` to re-seed ALL globals (const + mutable) to their declared initial values. This preserves `BATCH=64`/`RETRIES=64` and resets the mutable progress globals to their initial 1/0. I kept the `Program` in `DemoModule` for this. (This also fixed the SPSC FIFO off-by-one that the zeroed `spsc_prod_next` would have caused: producer pushing 0 first while consumer expects 1.)

**Judge: fix-the-demo.** The globals store holds both const and mutable globals; zeroing it clobbered the const ones. The reseed helper is the correct "clean slate" for a re-run.

### K3: MPMC consumers trapped + count short — DATA RACE on shared mutable globals (the single-context contract biting)

**Symptom:** MPMC: both producers finished (20001 each), but consumers trapped (`budget exceeded`) with total consumed 39977 (short of 40000).

**Cause:** my first MPMC consumer used two shared mutable globals (`mpmc_ca_count`, `mpmc_cb_count`) that BOTH consumer threads wrote concurrently based on the popped value's range. Two threads writing the same int64 cell in the shared globals backing store is a **data race** (plain stores, no synchronization). The globals backing store is shared across all per-thread contexts (one `GlobalsBlock` for the whole module — `thread_safety_test` uses the same shape). The sync primitives (atomics/queues) are internally synchronized, but PLAIN globals are not. The race corrupted the counts AND the consumers' done check (`sum >= 40000`) saw stale values, causing extra spinning → budget exhaustion.

This is the single-context contract biting in a different form: the contract says a `context_t` is not shared across threads, but the GLOBALS store IS shared, and concurrent writes to the same plain global are UB.

**Fix (demo):** made the MPMC consumer **stateless** — it pops a bounded batch and returns the count popped THIS call as a LOCAL (no shared mutable global). Each consumer thread accumulates its own thread-local total in the host's `mpmc_consumer_worker`. The two consumers share ONLY the mpmc ring (internally synchronized). The host stops a consumer when producers are done (signaled via a C++ `std::atomic<bool>`) AND `mpmc_size(h) == 0`. No-loss = `c0.total + c1.total == 40000`.

I first considered using an ember `atomic` for the shared consumed counter (host creates it, publishes the handle into a global), but hit K4 (can't initialize an `atomic`-typed global to `0`). The stateless design is cleaner anyway — it tests that the MPMC ring alone (no shared counter) preserves no-loss under 2-producer/2-consumer contention.

**Judge: fix-the-demo.** The extension is correct (the queues are internally synchronized; the race was on MY globals, not the extension's storage). The stateless consumer is the right design for multi-consumer with host-side accumulation.

### K4: can't initialize a handle-typed global to `0` (sema rejects `global h : atomic = 0`)

**Symptom (shape B first attempt):** `sema: global initializer type mismatch (atomic = i64)`. Then `global mpmc_cons_h : atomic;` → `parse: expected '='` (globals REQUIRE an initializer).

**Cause:** `Type::same` rejects `0` (i64) vs an `atomic` handle. And the parser requires `=` + an init expr for every global (`expect(Tk::Assign, "'='")`). So a handle-typed global needs a type-matching initializer, and there's no handle literal in ember.

**Fix (shape B):** declare handle-typed globals with a CALL initializer that type-checks: `global spsc_h : spsc = spsc_new(4);`. `spsc_new` returns `spsc`, which `same`-checks against the `spsc` global. `eval_global_initializers` only folds CONST initializers (a call is not const-foldable), so the global starts at 0 (invalid handle); `@entry setup()` creates the real primitives and reassigns. This is the standard "host-created handle stored in a global" pattern. (For shape A I avoided this entirely by passing handles via `ember_call_i64` args, not globals.)

   **Note (first-class struct / aggregate pass, 2026-07-10):** the *aggregate value-global* half of this story is now resolved — `struct`/`array`/`slice` value globals ship with const-foldable struct/array/slice initializers baked at load (`docs/TYPE_SYSTEM.md` §12.2). This kink, however, is the *handle-typed* half, which is NOT what that pass addressed: a sync handle is an opaque `i64` with a nominal `struct_name`, and there is no handle literal in ember, so a handle-typed global still needs a CALL initializer (`spsc_new(4)`) for the type-check and still starts at 0 until `@entry` reassigns it. The call-init-for-type-check workaround remains the correct shape for handle-typed globals; only the aggregate value-global rejection it was adjacent to has lifted.

**Judge: fix-the-demo.** The const-fold restriction on global initializers is documented (`globals.hpp::eval_global_initializers`); the call-init-for-type-check workaround is the clean shape.

### K5: `let` without `mut` is immutable (Rust-like)

**Symptom (shape B):** `sema: cannot assign to const variable 'pushed_this_tick'`.

**Cause:** `let pushed_this_tick : i64 = 0;` is immutable; the producer increments it.

**Fix (demo):** `let mut pushed_this_tick : i64 = 0;`. Standard ember.

### K6: `atomic_bumper` return value design — must signal done

**Caught while writing the harness (before running):** `atomic_bumper` originally returned `50000` (the count). But `worker_loop` re-calls while the fn returns `!= -1` (the "not done" convention). Returning `50000` → worker_loop re-calls → the bumper does ANOTHER 50000 `fetch_add` → loops until deadline, wildly over-counting.

**Fix (demo):** `atomic_bumper` returns `-1` (done) after its one bounded loop, so `worker_loop` calls it EXACTLY ONCE per thread. The other fns (producers/consumers) are batched step fns that return a count (≥0) or -1 (done) or -2 (FIFO/range violation); `worker_loop` re-calls them until -1. The bumper is the one fn that does all its work in a single call (50000 fetch_adds is well under the 500M budget).

## What the demo proves about the sync surface

- **M1 race-safety fix holds**: 4 threads × 50000 `fetch_add` on one atomic → exactly 200000 (no lost increments). This is the M1 fix (the lookup-vs-free lifetime race + the shared_ptr lease) under real contention.
- **SPSC FIFO + no-loss**: 100000 items pushed 1..N, popped in exact order `v == count+1`, none lost, under producer/consumer contention with cap 256 << N.
- **MPSC no-loss**: 2 producers × 50000 each, single consumer drained all 100000 (each producer's full stream delivered).
- **MPMC no-loss**: 2 producers + 2 consumers, 40000 total pushed == 40000 total consumed, under real MPMC contention (the internal-mutex ring).
- **swapbuf frame publish (shape B)**: the double-buffered frame model — write front, swap, host reads back the last published frame via `swapbuf_back_ptr` — works deterministically in a tick loop (back frame gen=199, items=3).
- **The single-context contract is respected**: every host thread owns its own `context_t`; no two ember-calling threads share a context. The globals store is shared (read-only handles, single-writer mutable progress globals, or stateless consumers) so no globals race.
- **B1 mode (use_context_reg=true) works**: one compiled body serves N per-thread contexts; each thread's trap would longjmp to ITS OWN checkpoint (none trapped in the passing runs, but the infrastructure is there).

## Limitations noted

- **No script-visible `yield`**: the bounded-batch + host re-call design works around this, but it means ember fns can't do a graceful cooperative busy-wait inside a single call. The sync extension documents this as deliberate (no script-visible mutex/yield pending the function-ref decision). The host re-call loop is the equivalent of a host-driven `yield`.
- **`spsc_size` is "approximate (cross-thread)" under contention** (per the impl comment): it's an acquire-load pair of head/tail that can observe a transient state. In shape (A) I avoid relying on it for correctness (the SPSC consumer uses the `v < 0` sentinel directly; the MPMC consumer uses the host's `mpmc_size` only as a STOP heuristic after producers are done, never for a correctness decision). In shape (B) it's exact (single-threaded).
- **Globals backing store is shared across threads**: fine for read-only handles and single-writer progress globals, but concurrent writes to the SAME plain global are UB (K3). Use atomics or per-thread locals for shared mutable counts.
