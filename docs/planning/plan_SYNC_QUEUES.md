# Plan — sync-extensions addon: atomics, swap buffer, SPSC/MPSC/MPMC queues

> **⚠ SHIPPED v1.0** — the `sync` extension (atomics, swap buffer,
> SPSC/MPSC/MPMC queues) landed in `extensions/sync/`, pinned by
> `examples/ext_sync_test.cpp` (ctest target `ext_sync`); see
> `v1.0_INTEGRATION_NOTES.md` §3. The text below is the historical planning
> record, left unchanged.
>
> **Status: research / planning only.** This document reads the code
> firsthand and lays out the design. No source is changed. The user is
> making a scoping decision from this — be concrete and honest about
> what these primitives do vs. what they deliberately do **not** do.
>
> **No mutex.** A script-visible `mutex` is explicitly deferred. The
> user's note: mutexes couple to the function-reference decision (a
> `scoped_with_lock(m) { ... }` block wants a callable/defer surface that
> depends on `plan_FUNCTION_REFS.md`). Atomics, swap buffers, and queues
> don't have that coupling, so they ship first.

---

## 0. The scope-honesty statement (read this first)

This is the load-bearing framing for the whole document. The parallel
`plan_CONTEXT_THREADSAFETY.md` defines two use cases; this addon is the
mechanism for one of them and explicitly **not** the other:

- **U1 (broad, NOT this addon's job).** *"Two host threads call ember fns
  at once"* — into the **same** or different `context_t`s. The context plan
  proves the ember `context_t` is **not** safe for concurrent calls today
  (per-call state — `jmp_buf checkpoint`, `int64_t budget_remaining`,
  `int32_t call_depth`, `last_trap`/`last_error` — is non-atomic, and the
  trap_ctx / budget_ptr / depth_ptr are baked as `imm64`s into the JIT'd
  body). Two threads calling ember fns into one context **race regardless of
  any sync primitive in this addon**, because the race is on `context_t`
  fields, not on queue/atomic storage. U1 is the *separate* context-thread-
  safety work (Option D + B1 in that plan, deferred to a later batch).

- **U2 (narrow, THIS addon's job).** *"A script callback running on host
  thread A safely hands data to / pulls data from host thread B via
  host-owned queues/atomics."* The queue/atomic **storage is host-side**;
  the host synchronizes it internally (lock-free atomics, or a host-internal
  lock for MPMC). The script holds an **i64 handle** into that host store
  and calls natives that mutate it. The script thread itself is
  single-threaded per `context_t` — **only the host state behind the handle
  is shared across threads.**

**Stated precisely, the honest boundary:**

> These primitives let a script coordinate with host threads on
> **host-owned shared state** behind i64 handles — a producer on host
> thread B pushes into a queue the script (on host thread A) pops from,
> an atomic the host writes the script reads, a swap buffer the script
> writes the host reads. They are **internally thread-safe at the host
> layer** (the storage is `std::atomic` / a lock-free ring / a host-internal
> mutex). They **do not make the ember `context_t` safe for concurrent
> calls**. If two threads call ember fns into the same context — even
> through these primitives — the context races exactly as documented in
> `plan_CONTEXT_THREADSAFETY.md` §1.2, and a trap on one thread can longjmp
> to the other thread's checkpoint (the §1.4 `--tick` bug, generalized).
> The context-thread-safety work is **separate** and ships in its own
> batch; this addon is usable the moment the host arranges that only one
> thread at a time calls ember into a given context (which is the U2 shape:
> script side stays single-threaded per context; the host threads that
> produce/consume the queues do not call ember at all, or call it on their
> *own* `context_t` per `plan_CONTEXT_THREADSAFETY.md` Option D).

**Why this is honest and not a hedge:** the queues work *because the
contention is on host storage, not on the context*. The moment contention
moves onto `context_t` (two threads calling `q.try_pop` *into the same
context* simultaneously), the queue's internal lock-freedom is irrelevant —
the budget decrement in the JIT'd body around the `try_pop` call already
raced. So the test matrix in §6 is designed so that the multi-thread stress
tests **never have two threads calling ember fns into the same context**;
the cross-thread traffic is host-thread → host-storage → script-thread,
with only the script thread touching ember.

**Use case this actually serves (concrete):** a render host runs a tick
callback on its render thread; the script writes a frame of overlay data into
a swap buffer; the host's present thread reads the back buffer. The script
also receives input events pushed by the host's input thread into an MPSC
queue and drains them in `@on_tick`. Both the render thread and the input
thread are **host** threads; they touch only the queue/swap-buffer host
storage (via `ext_sync`'s C++ accessors, see §5), never the ember context.
The script thread is the only ember caller. No context race is possible.
That is U2.

---

## 1. The shape — host C++ extension, opaque i64 handles (mirrors `ext_array` / `ext_string`)

This addon is a Tier-0-shaped extension exactly like the six that already
ship (`extensions/{array,string,vec,quat,mat,math}/`). It is **not** a
language/grammar/sema change (per `../../extensions/README.md`'s "what an
extension is — and is not" rule and ROADMAP Tier 0's "not language features"
framing). The pattern to mirror, read firsthand:

- `extensions/array/ext_array.cpp` + `.hpp` — the canonical i64-handle +
  host-vector + bounds-check pattern. `static std::vector<ArraySlot>
  g_arrays;` with `arr_slot(int64_t h)` doing `if (h<1 || h>size) return
  nullptr` (1-based handle, 0 = null). `extern "C" { static ...
  n_array_* (...) }` natives. `register_natives(map)` uses
  `BindingBuilder::add("name", ret, {params}, (void*)&fn)`. `reset()`
  clears the store. Public `get_bytes()` accessor for host-side natives
  that reach in by handle.
- `extensions/string/ext_string.cpp` — same shape, plus
  `register_overloads(table)` for `+`/`==`. Public `slot()` accessor.
- `src/binding_builder.hpp` — `bind_handle("name")` produces an i64 with a
  `struct_name` type-system tag (the convention these extensions use);
  `type_i64()` / `type_bool()` / `type_void()` for the rest.
- `extensions/math/ext_math.cpp` — the stateless pattern (no host store,
  no `reset()`). Not used here (everything in this addon has a host store)
  but confirms the "pure functions, no reset" variant exists for contrast.

This addon differs from `ext_array`/`ext_string` in exactly one structural
way: **the host store is internally synchronized** (atomics / lock-free
rings / a host-internal mutex), where `ext_array`/`ext_string`'s stores are
plain `std::vector`/`std::string` with no synchronization (they assume a
single script thread). Every other convention — opaque i64 handle, 1-based
indexing, `slot(h)` bounds check, `register_natives`/`reset` entry points,
public accessor for host-side reach-in — is preserved verbatim.

**Handle types (the type-system tags).** Each primitive gets its own
`bind_handle` tag so the script (and sema) can tell them apart, exactly as
`string` and `array` are distinct today:

| Primitive | `bind_handle` tag | backing host type |
|---|---|---|
| atomic i8/16/32/64 | `"atomic"` | `struct AtomicSlot { std::atomic<int64_t> v; };` (width stored as a field, masked on store) |
| swap buffer | `"swapbuf"` | `struct SwapBufSlot { std::vector<int64_t> buf[2]; std::atomic<int> front; };` |
| SPSC queue | `"spsc"` | `struct SpscSlot { ring + atomic head/tail }` (see §4.3) |
| MPSC queue | `"mpsc"` | `struct MpscSlot { ... }` (see §4.4) |
| MPMC queue | `"mpmc"` | `struct MpmcSlot { ring + host-internal std::mutex }` (see §4.5) |

The script side sees five distinct opaque handle types; sema gets them as
`i64` with a struct tag (the same way `array` vs `string` are both `i64`
underneath but tagged). No operator overloads (none of these have a `+` /
`==` that means anything); `register_overloads` is not implemented, mirroring
`ext_array` (which also has none).

---

## 2. ATOMICS — `aint8` / `aint16` / `aint32` / `aint64`

### 2.1 Recommendation: int atomics only; defer `afloat`

Atomic float is unusual and not what these primitives are for. The use case
(inter-thread signaling, counters, flags, sequence numbers) is integer-
shaped. A "atomic float" that's just `std::atomic<int32_t>` reinterpreted as
IEEE-754 is technically possible (`std::atomic_ref<float>` in C++20), but:

- It offers no `fetch_add` that's actually useful for floats under
  concurrency (the classic hazard: two threads doing `f += delta` lose
  precision / tear — and `atomic<float>::fetch_add` doesn't exist pre-C++20
  for float; you'd CAS-loop it, which is a lock-free loop a script can
  author if it ever needs it via `aint32` + bit-punning).
- The "atomic float" use case (interpolating a shared value across threads)
  is better served by `aint32` holding an IEEE-754 bit pattern the script
  reinterprets, or by a swap buffer (§3) for the "host writes a value, script
  reads the latest" pattern, which is the actual float-across-threads idiom.

**Recommendation: ship `aint8`/`aint16`/`aint32`/`aint64` only. Document that
float-across-threads goes through `aint32` (bit-pun) or a swap buffer.
`afloat` is a future addition if a real script wants it.**

### 2.2 API (all behind i64 handles tagged `"atomic"`)

```
atomic_new(width: i64, init: i64) -> atomic      // width in {8,16,32,64}; default 64
atomic_load(h: atomic) -> i64
atomic_store(h: atomic, v: i64)
atomic_fetch_add(h: atomic, delta: i64) -> i64  // returns OLD value
atomic_fetch_sub(h: atomic, delta: i64) -> i64  // returns OLD value
atomic_cas(h: atomic, expected: i64, desired: i64) -> i64   // 1 = swapped, 0 = not
atomic_swap(h: atomic, v: i64) -> i64           // returns OLD value (exchange)
atomic_free(h: atomic)
```

Notes:
- `atomic_cas` returns `i64` (1/0) rather than `bool` so it round-trips
  through ember's i64 ABI without a bool-marshalling special case (mirrors
  `array_get_u8` returning `i64` even for a u8). `type_i64()` return, not
  `type_bool()`.
- `width` is stored on the slot and the natives **mask on store** so a
  `aint8` genuinely holds an 8-bit value (a `store(h, 0x1FF)` on an `aint8`
  stores `0xFF`). This is a host-side masking, not a script-visible type —
  the script sees `i64` everywhere; the width is a behavior contract, same
  as `array`'s `elem_size`.
- `atomic_free` exists for explicit lifetime; leaks are addressed in §4.6.

### 2.3 Host impl

```cpp
struct AtomicSlot {
    std::atomic<int64_t> v;
    int width;          // 8/16/32/64 -- store masks to this
};
static std::vector<AtomicSlot> g_atomics;   // 1-based handles, mirror ext_array
```

- `n_atomic_new(width, init)` pushes a slot, returns index+1.
- `n_atomic_load(h)` does `slot(h)->v.load(std::memory_order_acquire)` (the
  default `memory_order_seq_cst` is fine and simpler; acquire/release is an
  optimization a real profile can switch in later — flag in a comment, not in
  the API).
- `n_atomic_store(h, v)` masks to `width` then `v.store(masked, release)`.
- `n_atomic_fetch_add(h, d)` does `v.fetch_add(d, acq_rel)`, returns old.
  (Width masking on `fetch_add` is subtle — for an `aint8`, `fetch_add`
  should wrap modulo 256. Implement as a CAS loop for widths < 64:
  `old = load; new = mask(old + d); cas until swapped; return old`. For
  `width == 64` it's a plain `fetch_add`. Documented in the impl comment.)
- `n_atomic_cas(h, exp, des)` does `v.compare_exchange_strong(exp, des,
  acq_rel, acquire)`, returns 1/0. **Important:** `compare_exchange_strong`
  **modifies `exp`** on failure to the current value — that's a host-local
  variable, not a script-visible out-param, so it's invisible to the script
  (the script gets only the bool). This is correct and matches the "CAS
  returns bool, script re-reads via `load` if it wants the current value"
  idiom.
- `n_atomic_free(h)` — see §4.6 for the slot-reclaim policy.

### 2.4 Deadlock-safety argument

**Lock-free by construction.** `std::atomic` operations are non-blocking (the
standard guarantees they're lock-free for the integral types on all
mainstream platforms; `is_lock_free()` returns `true` for `int64_t` on
x86-64). There is **no lock held across an ember native call boundary** —
each native does its atomic op and returns; no native holds a spinlock while
waiting for another thread. There is no waiting at all: `atomic_cas` returns
immediately (success or failure); `atomic_fetch_add` returns immediately.
**A script cannot deadlock on an atomic** because there is nothing to wait
on. The only failure mode is livelock (a CAS loop in script code that
retries forever under contention) — that's a script-author responsibility
(the same way a `while (true) { }` loop is), and the instruction budget
(`../spec/SAFETY_AND_SANDBOX.md §3`) catches runaway loops at the ember level anyway.

---

## 3. SWAP BUFFER / SWAPCHAIN — double-buffer with atomic pointer swap

### 3.1 The use case (concrete)

A script produces a frame of data (overlay vertices, a row of UI values, a
batch of events) the host consumes on a different thread. The naive version
— script writes a shared buffer, host reads it — races on every cell. A
swap buffer solves it: the script writes one side ("front"), the host reads
the other ("back"), and `swap()` atomically exchanges which side is which.
The host never sees a half-written frame; the script never races with a
reader.

### 3.2 API (behind i64 handle tagged `"swapbuf"`)

```
swapbuf_new(capacity: i64) -> swapbuf          // capacity = number of i64 slots per side
swapbuf_write(h: swapbuf, idx: i64, val: i64)  // writes to the FRONT (script-owned) side
swapbuf_read(h: swapbuf, idx: i64) -> i64      // reads the BACK (host-owned) side
swapbuf_swap(h: swapbuf)                       // atomically exchange front <-> back
swapbuf_capacity(h: swapbuf) -> i64
swapbuf_free(h: swapbuf)
```

- `swapbuf_write` is **script-side only** (writes front).
- `swapbuf_read` is **host-side** (reads back) — exposed as a native for
  symmetry, but the host typically reaches in via the public accessor
  `swapbuf_back_ptr(h, &out, &len)` (see §5), the same way the host's
  shader draw calls reach into `ext_array::get_bytes`. The native form lets
  a *script* also consume if the dataflow is reversed (host produces, script
  consumes) — the buffer is symmetric; "front" is just "the side the last
  `swap` made writable." Expose both; document the convention.
- `swapbuf_swap` is the one atomic operation. It exchanges `front` index
  (0<->1). After `swap`, what was front is now back (readable), what was back
  is now front (writable).

### 3.3 Host impl

```cpp
struct SwapBufSlot {
    std::vector<int64_t> side[2];   // two equal-size buffers
    std::atomic<int> front;          // 0 or 1 -- which side the script writes
};
static std::vector<SwapBufSlot> g_swapbufs;
```

- `n_swapbuf_new(cap)` allocates `side[0]` and `side[1]` each sized `cap`,
  `front = 0`.
- `n_swapbuf_write(h, idx, val)` — `int f = front.load(acquire); if (idx in
  range) side[f][idx] = val;`. **The write to `side[f][idx]` is a
  non-atomic `int64_t` store**, which is safe *because only the script
  thread touches `front` side* — the invariant is "the script owns the
  front side exclusively; the host owns the back side exclusively." No
  atomic per-cell store is needed; only the `front` index needs to be
  atomic.
- `n_swapbuf_swap(h)` — `front.fetch_xor(1, acq_rel)`. (XOR with 1 flips
  0<->1.) That's the entire swap: one atomic RMW. After this, the host's
  reader sees the just-written frame on the (new) back, and the script's
  next writes go to the (new) front (the stale frame the host already
  consumed).
- `n_swapbuf_read(h, idx)` — `int b = 1 - front.load(acquire); return
  side[b][idx]`. (Back = the side front is NOT.) The host reads back; this
  is the symmetric accessor for the script-consumes case.

**Memory-ordering argument.** The `fetch_xor(acq_release)` on `swap`
establishes a release on the script's writes (the writes to `side[old
front]` happen-before the `fetch_xor`) and an acquire on the host's read
(the host's `front.load(acquire)` sees the new front, and therefore sees all
the script's writes to what is now back). This is the standard
double-buffer release/acquire pattern; no per-cell atomics needed.

### 3.4 Deadlock-safety argument

**Lock-free by construction — one atomic RMW per swap, none per cell.**
There is no lock; there is no waiting. `swapbuf_swap` is a single
`fetch_xor` and returns. A script cannot deadlock: it writes cells
(non-blocking stores), swaps (one atomic, non-blocking), done. The only
hazard is a torn frame if the script `swap`s while the host is mid-read of
the back buffer — but that's a *protocol* question, not a deadlock:
- If the host reads the whole back buffer atomically-with-respect-to-the-
  script (i.e. the host reads between two script `swap`s), no tear. The
  protocol is "script: write frame, swap, write next frame, swap, ...; host:
  read back, read back, ..." — the script's `swap` is the publish, the host's
  read is the consume, and `acq_rel`/`acquire` orders them.
- If the host wants to never miss a frame and never read a half-frame, the
  host reads on its own tick, and the script's swap rate is decoupled — the
  host just sees "the most recent fully-published frame" each tick. This is
  the render-overlay use case and it's exactly what a swapchain is for.

No deadlock is possible because there is no contention-for-a-lock; there is
only an atomic index flip.

---

## 4. Queues — SPSC, MPSC, MPMC

All three share the same API shape and differ only in the host impl and the
deadlock-safety argument. A `try_pop` (non-blocking) is the only pop —
**there is no blocking `pop`**. A blocking pop would either spin (burns the
instruction budget, and a script spinning in `try_pop` is a runaway loop
`SAFETY §3` catches) or wait on a condvar (a condvar wait inside a native
holds a host lock across the wait, which is a **blocking native** — ember
has no blocking-native contract today, and adding one is out of scope for
this addon; a host that wants blocking can layer a `wait` on top via its own
mechanism). **`try_pop` returns an `i64` flag + the value** (see API below);
the script polls. This is the deadlock-avoidance keystone for all three
queues — see §4.1.

### 4.1 Shared API shape

```
<TYPE>_new(capacity: i64) -> <TYPE>          // bounded ring; capacity is the slot count
<TYPE>_push(h: <TYPE>, v: i64) -> i64         // 1 = pushed, 0 = full (non-blocking)
<TYPE>_try_pop(h: <TYPE>) -> i64             // returns value if non-empty; empty -> ???
<TYPE>_size(h: <TYPE>) -> i64                 // approximate (see per-impl)
<TYPE>_free(h: <TYPE>)
```

**The `try_pop` empty-sentinel problem.** `try_pop` needs to signal "empty"
distinctly from a valid value. Two options:
- **(a) Out-of-band via a second native:** `<TYPE>_try_pop(h) -> i64`
  returns the value, and a separate `<TYPE>_empty(h) -> i64` (1/0) lets the
  script check before popping. Race: between `empty` and `try_pop` another
  consumer could drain (for MPSC/MPMC). For SPSC (single consumer) this is
  safe. For MPSC/MPMC the script must tolerate `try_pop` returning a
  sentinel.
- **(b) Sentinel value:** pick a value (e.g. `INT64_MIN`) as "empty." The
  script must agree never to push `INT64_MIN`. Simple, single-native, no
  race. Matches how `array_get_u8` returns `0` for OOB.

**Recommendation: (b) sentinel, with a `try_pop_ok` two-return variant as a
future addition if a real script can't spare the sentinel.** The sentinel
keeps the ABI to a single i64 return (matches `array`/`string`'s
single-value-return convention) and avoids the empty/try_pop race for
multi-consumer queues. Document `INT64_MIN = empty` loudly. (The host-side
accessor — §5 — returns a `bool` + value, so host C++ consumers don't deal
with the sentinel.)

### 4.2 Deadlock-safety: the shared keystone — `try_pop` and `push` are both non-blocking

Across all three queue types, **`push` and `try_pop` never block**:
- `push` returns 0 immediately if full (it does not wait for a slot).
- `try_pop` returns the sentinel immediately if empty (it does not wait for
  a producer).

**No native in this addon ever holds a lock while waiting for another
thread.** The MPMC impl (§4.5) does take a host-internal `std::mutex`, but
it holds it only for the duration of the ring index update — it does **not**
hold it across an ember call boundary, and it does **not** sleep on it. So:

- A script cannot deadlock on a queue: every queue native returns in bounded
  time (one atomic RMW, or one short critical section).
- A script cannot deadlock the *host* either: the host-internal lock in MPMC
  is never held while the host waits for the script (the host doesn't call
  ember while holding it).

This is the structural property the user asked for ("hard-to-accidentally-
deadlock"): the addon exposes only non-blocking operations, so the only way
to "block" is a script-author-written `while (q.try_pop() == EMPTY) {}` poll,
which is a script-level construct the instruction budget (`SAFETY §3`)
bounds. No primitive in this addon can deadlock a script or the host.

### 4.3 SPSC — single-producer single-consumer (lock-free ring)

**The classic bounded SPSC ring** with separate `head` (producer) and `tail`
(consumer) atomic indices. This is the textbook lock-free SPSC; it's the
simplest correct lock-free queue that exists.

```cpp
struct SpscSlot {
    std::vector<int64_t> buf;       // capacity slots
    std::atomic<int64_t> head;      // producer writes here (only producer touches)
    std::atomic<int64_t> tail;      // consumer reads here (only consumer touches)
};
```

- `push`: `int64_t h = head.load(relaxed); if (h - tail.load(acquire) < cap)
  { buf[h % cap] = v; head.store(h + 1, release); return 1; } return 0;`
- `try_pop`: `int64_t t = tail.load(relaxed); if (t == head.load(acquire))
  return EMPTY; int64_t v = buf[t % cap]; tail.store(t + 1, release);
  return v;`

(The `cap` is a power of two in the recommended impl so `% cap` is `& (cap-1)`,
but a non-power-of-two works too with `%`; pick power-of-two for the slight
perf and document it. The `head - tail < cap` full-check uses the
**monotonic** head/tail (not modulo) so the count is unambiguous — this is
the standard SPSC trick; capacity is bounded, indices grow without bound,
`% cap` indexes the ring.)

**Deadlock-safety:** lock-free by construction — single producer, single
consumer, no CAS, no waiting. The `acquire`/`release` pair orders the
producer's write before the consumer's read. There is exactly one producer
and one consumer by the queue's contract; if a second producer calls `push`
concurrently the queue is undefined (the script-author / host-arranger's
contract violation, not a deadlock). Document this contract on the native.

### 4.4 MPSC — multi-producer single-consumer

Two legitimate lock-free designs; recommend the simpler:

**(a) Per-producer SPSC ring + a list of producer slots.** The host
registers N producer slots; each producer gets its own SPSC ring (§4.3);
the single consumer round-robins them. Producers never contend (each owns
its ring). The consumer does N `try_pop`s per drain. Simple, fully
lock-free, no CAS. Cost: the consumer must know the producer set; a dynamic
producer needs a "register producer slot" native. **Recommend this if the
producer set is static or host-managed** (the common case — the host knows
its input threads).

**(b) Single shared ring + CAS on `head`.** The classic MPSC: producers
CAS-reserve a head slot, write the cell, publish via a per-cell "ready" flag
the consumer checks. Fully lock-free, one ring, dynamic producers. Cost:
CAS contention under high producer counts; the consumer must spin/skip
not-yet-published cells (a cell is reserved but not yet written). More
complex; the "ready flag per cell" is a real subtlety. **Defer to a future
optimization if (a) doesn't cover a real use case.**

**Recommendation: ship (a) — per-producer SPSC + a `mpsc_new` that takes a
producer count, exposing `mpsc_push(producer_slot, v)` via a per-producer
handle.** This keeps the MPSC implementation entirely in terms of the
already-shipped SPSC ring (less code, less surface, no CAS). If a real
script later needs dynamic producer registration or single-ring MPSC, ship
(b) as a v2 of the queue layer. Document the choice; note that MPMC uses a
different, simpler answer (see §4.5).

**API for design (a):**
```
mpsc_new(capacity: i64, producer_count: i64) -> mpsc   // one shared consumer side, N producer sides
mpsc_producer_handle(h: mpsc, idx: i64) -> i64         // returns the i64 producer-sub-handle (a SPSC handle baked in)
mpsc_push(producer_h: i64, v: i64) -> i64               // pushes to that producer's SPSC ring
mpsc_try_pop(h: mpsc) -> i64                            // round-robins all producer rings, pops first non-empty
mpsc_size(h: mpsc) -> i64                              // sum of per-ring sizes (approximate)
mpsc_free(h: mpsc)
```

This exposes a two-handle shape (the mpsc "container" handle + per-producer
i64 sub-handles), which is a slight deviation from the "one i64 per
primitive" rule. Alternative that stays one-handle: `mpsc_push(h, v)` pushes
to a *single* shared ring under a CAS-on-head MPSC (design (b)), and the
producer-count is just the ring capacity sizing. **That's simpler for the
script (one handle) at the cost of the CAS-MPSC implementation.** The
decision is: if the CAS-MPSC impl is acceptable (it's well-understood, ~80
lines), prefer the single-handle `mpsc_push(h, v)` form for script
ergonomics. If the team wants to avoid the CAS subtlety in v1, ship the
two-handle per-producer form.

**Deadlock-safety (either design):** lock-free — no blocking, no lock held
across an ember call. (a) is SPSC rings (§4.3). (b) is CAS-loop `push` that
retries on contention but **never blocks** — a CAS-loop under heavy
contention is a livelock risk (bounded by the instruction budget if the
script loops, but the *push native itself* returns in bounded time per
call because the CAS retry count is capped inside the native). Document
the cap.

### 4.5 MPMC — multi-producer multi-consumer: the design decision

**The honest problem:** MPMC is NOT trivially lock-free. The two real
options:

**(alpha) Bounded MPMC ring + host-internal `std::mutex`.** A ring buffer
guarded by a `std::mutex` held only for the index-reserve-and-write (push)
or index-reserve-and-read (pop) critical section. The lock is **host-
internal** — it is never exposed to the script as a `mutex` primitive, and
**it is never held across an ember native call boundary** (the native takes
the lock, does the ring update, releases the lock, all before returning).
The script cannot deadlock on it because the script never holds it; the
host cannot deadlock on it because no host thread holds it while calling
ember (the host threads that produce/consume MPMC do so via the host-side
accessor, §5, and never call ember while holding it).

**(beta) Lock-free bounded MPMC ring (Dmitry Vyukov's sequence-lock design).**
Each cell carries an atomic sequence counter; producers CAS-reserve, write,
publish via the seq; consumers CAS-reserve, read, publish. Fully lock-free,
no mutex. ~120 lines, subtle (the seq/contended-CAS dance is a known
footgun — the turn-overflow at `seq == capacity` boundary is the classic
bug). Real perf win only under high producer x consumer contention.

**Recommendation: ship (alpha) — bounded ring + host-internal `std::mutex`.**

Rationale, stated honestly:
1. **Simplicity and correctness first.** The Vyukov MPMC is a real,
   published, correct design, but it is subtle and a from-scratch
   implementation is a bug magnet (the seq overflow, the ABA on the seq,
   the "is this cell published or being-written" race). The internal-mutex
   ring is ~30 lines and obviously correct.
2. **The lock is host-internal and never crosses an ember call.** This is
   the key property that keeps it out of the "mutex deferred" bucket. The
   user deferred *script-visible* mutexes (because they couple to the
   function-ref / `scoped_with_lock` decision). An internal `std::mutex`
   used to guard a ring index is **not** a script primitive — the script
   sees `mpmc_push`/`mpmc_try_pop`, not a lock. There is no `lock`/`unlock`
   in the script API. The deferred-mutex decision is untouched.
3. **It cannot deadlock the script or the host.** The critical section is
   "reserve a ring slot, write/read it, release" — bounded, no waiting, no
   condvar, no sleep. The lock is held for nanoseconds. No host thread
   holds it while calling ember (by the accessor contract, §5). No script
   call holds it across a return. A deadlock requires a lock held while
   waiting for another lock or a callback; neither happens here.
4. **(beta) is a future optimization.** If a profile shows the internal mutex
   is a real contention point under a real MPMC workload, ship (beta) as a
   drop-in replacement of the slot's impl — the script API is unchanged.
   Premature lock-free MPMC is exactly the kind of subtlety the user's
   "hard-to-accidentally-deadlock" framing wants to avoid in v1.

**API (behind i64 handle tagged `"mpmc"`):**
```
mpmc_new(capacity: i64) -> mpmc
mpmc_push(h: mpmc, v: i64) -> i64            // 1 = pushed, 0 = full
mpmc_try_pop(h: mpmc) -> i64                 // value or INT64_MIN sentinel
mpmc_size(h: mpmc) -> i64                    // under-lock, exact
mpmc_free(h: mpmc)
```

**Documented contract:** "MPMC uses a host-internal lock for correctness;
the lock is not exposed to the script and is never held across an ember
call. If you observe contention, the lock-free Vyukov variant is a future
drop-in."

### 4.6 Handle lifetime — `new`/`free` and leak policy

Mirrors `ext_array`/`ext_string`: the host store is a `std::vector<Slot>`,
handles are 1-based indices, `slot(h)` bounds-checks. Differences from
`ext_array`:

- **`<TYPE>_free(h)` exists.** `ext_array` has no `free` (it relies on
  `reset()` to drop everything per run); these primitives do too, but
  because the queues/atomics are intended for long-lived cross-thread
  channels, a script that creates one per `@on_tick` and never frees would
  grow the host store unboundedly. `free` lets a disciplined script release.
- **`free` slot-reclaim policy.** Two options:
  - **(i) Tombstone (mark free, reuse on `new`):** `free` sets a
    `dead=true` flag; `new` scans for a dead slot or appends. O(n) scan on
    `new` in the worst case, but `new` is rare. Simple, correct.
  - **(ii) Append-only (free is a no-op mark, never reused):** `free`
    marks dead; `reset()` reclaims. Simplest, but the store grows under
    leaky scripts until `reset`.
  - **Recommendation: (i) tombstone with a free-list.** Keep a
    `std::vector<int64_t> g_free_list` of dead-but-reusable slot indices;
    `new` pops from the free-list or appends; `free` pushes the index onto
    it. O(1) `new`/`free`, no scan. The store only grows if the script
    holds more *simultaneously live* handles than its high-water mark,
    which is the honest bound anyway.
- **Leak handling.** A script that forgets `free` leaks the host store
  slot (and, for queues, the ring buffer's heap). This is **documented,
  not prevented** — mirroring `ext_array`'s "leaks if the script forgets
  to drop the handle, host store grows until `reset()`" posture. The host's
  `ResetScriptHostState` (per `../../extensions/README.md`) calls `ext_sync::reset()`
  which clears all stores + the free-list. **Per-context arena reset on
  module unload** (the user's suggestion): `reset()` is the mechanism for
  this exactly as it is for `ext_array`; a host that wants per-unload
  cleanup calls `ext_sync::reset()` in its unload hook. No new mechanism
  needed — mirror the existing pattern.
- **Thread-safety of `new`/`free` themselves.** `g_atomics`/`g_swapbufs`/
  `g_queues` are `std::vector<Slot>` mutated by `new`/`free`. If two
  threads call `new`/`free` concurrently the vector races (same as
  `ext_array` today). **These are script-side operations** — and per the
  scope-honesty statement (§0), the script side is single-threaded per
  context. So `new`/`free` need no synchronization *under the U2 contract*.
  If a host violates U2 (calls `new` from two ember-calling threads into
  one context), the store races — but that's a context-race (§0), already
  documented as out of scope. Document this: "`new`/`free` are
  single-script-thread operations; the host-owned store they mutate is
  not synchronized for concurrent `new`/`free` because the script side is
  single-threaded per context by the U2 contract."

---

## 5. File layout — `extensions/sync/`

Mirrors `extensions/array/` exactly. One TU per the convention (array,
string, vec are each one `.cpp`; this addon is one `.cpp` too — it's not
large enough to split, and keeping it one TU matches the "self-contained
TU depending only on ember public headers + stdlib" purity rule from
`../../extensions/README.md`).

```
ember/extensions/sync/
├── ext_sync.hpp        # public API: register_natives, reset, + host-side accessors
└── ext_sync.cpp       # all five primitives' host impl + BindingBuilder registration
```

### `ext_sync.hpp` (public surface — mirrors `ext_array.hpp` / `ext_string.hpp`)

```cpp
// ext_sync.hpp - ember extension: cross-thread sync primitives (atomics,
// swap buffer, SPSC/MPSC/MPMC queues). See plan_SYNC_QUEUES.md.
//
// An ember *extension* (see ../../extensions/README.md): a reusable,
// non-host-specific addon. Host-owned, internally-synchronized storage
// behind opaque i64 handles. Each primitive's store is a separate host
// std::vector of slots; reset() clears them all.
//
// SCOPE (see plan_SYNC_QUEUES.md §0): these let a script coordinate with
// host threads on host-owned shared state. They do NOT make the ember
// context_t safe for concurrent calls — that's the separate
// plan_CONTEXT_THREADSAFETY.md work. Use under the U2 contract: the
// script side is single-threaded per context; host threads touch only
// the host storage via the accessors below, never the context.
#pragma once
#include "sema.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_sync {

// Register all sync primitives' natives (atomic_*, swapbuf_*, spsc_*,
// mpsc_*, mpmc_*) into m. No operator overloads (these are method-call
// natives, like ext_array), so no register_overloads().
void register_natives(std::unordered_map<std::string, NativeSig>& m);

// Clear all host stores + the free-lists. A host wanting per-run or
// per-unload isolation calls this (mirrors ext_array::reset / ext_string::reset).
void reset();

// ---- Host-side accessors (for host threads that produce/consume without
//      calling ember — the U2 shape). Mirrors ext_array::get_bytes /
//      ext_string::slot. Host threads use these; they never call ember. ----

// Atomic: load the current value from a host thread. acquire load.
bool atomic_load_host(int64_t handle, int64_t* out_val);
// Atomic: store from a host thread. release store.
bool atomic_store_host(int64_t handle, int64_t val);
// Atomic: CAS from a host thread. Returns true if swapped.
bool atomic_cas_host(int64_t handle, int64_t expected, int64_t desired, bool* swapped);

// Swap buffer: get a pointer + len to the BACK (host-readable) side's i64s.
// The host reads this without calling ember. Returns false on bad handle.
bool swapbuf_back_ptr(int64_t handle, int64_t** out_data, int64_t* out_len);
// Swap buffer: write to the FRONT side from a host thread (the host-produces case).
bool swapbuf_front_write_host(int64_t handle, int64_t idx, int64_t val);
// Swap buffer: swap from a host thread (publish a host-produced frame).
bool swapbuf_swap_host(int64_t handle);

// Queue: push from a host thread (the producer-on-host case). 1 = pushed, 0 = full.
bool spsc_push_host(int64_t handle, int64_t val, bool* out_pushed);
bool mpsc_push_host(int64_t handle, int64_t val, bool* out_pushed);
bool mpmc_push_host(int64_t handle, int64_t val, bool* out_pushed);
// Queue: try_pop from a host thread. Returns true if a value was popped (out_val set).
bool spsc_try_pop_host(int64_t handle, int64_t* out_val, bool* out_popped);
bool mpmc_try_pop_host(int64_t handle, int64_t* out_val, bool* out_popped);

} // namespace ember::ext_sync
```

(The `_host` suffix on the accessors is deliberate and load-bearing: it
tells a reader "this is the host-side entry, not an ember native" — the
same naming discipline that keeps `ext_array::get_bytes` clearly a
host-reach-in vs. an ember-callable `array_get_*`. A host thread producing
into a queue calls `spsc_push_host`; a script producing calls `spsc_push`
(registered as a native). They share the same underlying ring impl; the
accessor is the host-exposed C++ entry, the native is the ember-registered
fn-ptr.)

### `ext_sync.cpp` (impl)

One namespace `ember::ext_sync`. Layout, in order:
1. Includes: `ext_sync.hpp`, `ast.hpp`, `binding_builder.hpp`, `<atomic>`,
   `<vector>`, `<mutex>` (for MPMC), `<cstring>`.
2. Five `struct <Type>Slot { ... };` definitions.
3. Five `static std::vector<TypeSlot> g_<type>;` stores + five free-lists
   (`std::vector<int64_t> g_<type>_free`).
4. `slot<Type>(int64_t h)` bounds-check helpers (one per type, mirrors
   `arr_slot`).
5. `extern "C" { static ... n_<type>_<op>(...) }` blocks — one block per
   primitive (atomic, swapbuf, spsc, mpsc, mpmc), each containing that
   primitive's natives.
6. The host-side accessors (non-`extern "C"` — they're C++ functions the
   host calls directly, mirroring `ext_array::get_bytes`).
7. `register_natives(map)` — one `BindingBuilder` with all ~25 natives
   `add()`ed, `build()`, loop-insert into `m`.
8. `reset()` — clears all five stores + all five free-lists.

### CMake — one `ember_add_extension(sync, ...)` line

Add to the `ember_add_extension` block in `CMakeLists.txt` (the block at
the `ember_add_extension(math ...)` line, mirroring verbatim):

```cmake
ember_add_extension(sync   extensions/sync/ext_sync.cpp)
```

This produces `ember_ext_sync`, a static lib linking `ember_frontend`
PUBLIC (the macro does this; sync needs `make_prim`/`bind_handle`/`Type`
symbols from `ember_frontend`, same as the other six). A consumer (the host,
or a future second consumer) links it and calls
`ember::ext_sync::register_natives(m)` + `ember::ext_sync::reset()` from
its own host native-table builder + per-run reset, exactly as it does for
the other six extensions (`../../extensions/README.md` "How a host registers an
extension").

**Test-target wiring** (see §6): a new `ext_sync_test` executable linking
`ember ember_frontend ember_ext_sync` (+ `<thread>` for the multi-thread
stress tests), added via the same `add_executable` / `target_link_libraries`
/ `add_test` pattern as `ext_runtime_test` / `v0_4_hardening_test`.

---

## 6. Test matrix

Two tiers, matching the existing test-shape discipline (`ext_runtime_test`
for single-thread functional, `v0_4_hardening_test` for stress):

### Tier 1 — single-thread functional (must pass; no threads)

Proves the natives register, the host store works, and the semantics are
correct in the absence of contention. All of these are
single-`context_t`-single-thread; they exercise the full
lex->parse->sema->codegen->JIT->call path exactly like `ext_runtime_test`:

1. **atomic round-trip.** `atomic_new(64, 7); atomic_load -> 7;
   atomic_store(h, 42); atomic_load -> 42; atomic_fetch_add(h, 5) -> 42
   (old); atomic_load -> 47; atomic_cas(h, 47, 100) -> 1; atomic_load ->
   100; atomic_cas(h, 47, 200) -> 0 (fails, exp != cur); atomic_load ->
   100 (unchanged); atomic_swap(h, 999) -> 100; atomic_load -> 999.` Exit
   assert all values match.
2. **atomic width masking.** `atomic_new(8, 0); atomic_store(h, 0x1FF)`;
   `atomic_load -> 0xFF` (masked to 8 bits). Repeat for 16/32.
3. **swap buffer write/swap/read.** `swapbuf_new(4); write(h,0,11);
   write(h,1,22); swap; read(h,0) -> 11; read(h,1) -> 22; write(h,0,33)
   (now front is the old back); swap; read(h,0) -> 33; read(h,1) -> 22
   (the back from before the last swap, since after the second swap the
   old back became back again). ` Assert the double-swap frame ordering.
4. **swapbuf bounds.** `write(h, 999, 1)` is a no-op (out of range); `read(h,
   999)` returns 0 (or sentinel). Mirror `ext_array`'s bounds behavior.
5. **SPSC push/pop order.** `spsc_new(4); push 1, 2, 3, 4 -> all 1 (ok);
   push 5 -> 0 (full); try_pop -> 1; try_pop -> 2; try_pop -> 3; try_pop
   -> 4; try_pop -> EMPTY (INT64_MIN).` Assert FIFO order + full/empty
   signals.
6. **MPSC push/pop.** (Design (a) per-producer:) push to each producer,
   pop sees them in round-robin order. (Design (b) single-ring:) push from
   "the script as the single producer" (degenerate but tests the ring),
   pop order is FIFO.
7. **MPMC push/pop.** `mpmc_new(4); push 1,2,3,4 -> ok; push 5 -> 0 (full);
   try_pop -> 1; try_pop -> 2; ...; try_pop -> EMPTY.` Same as SPSC single-
   thread (the lock isn't exercised but the ring semantics are).
8. **handle lifetime.** `atomic_new -> h1; atomic_free(h1); atomic_load(h1)
   -> 0 (bad handle, post-free); atomic_new -> h2; assert h2 reuses h1's
   slot index (free-list works).` Mirror for all five primitives.
9. **reset clears.** Create handles, call `ext_sync::reset()`, assert all
   handles are now invalid (load returns 0 / sentinel). Mirrors
   `ext_array::reset` semantics.

### Tier 2 — multi-thread stress (the careful design; this is the keystone)

**The keystone constraint, restated for the tests:** the multi-thread
tests **never have two threads calling ember fns into the same context.**
The cross-thread traffic is:
- A **host thread** produces into a queue/atomic/swapbuf via the **host-side
  accessor** (`ext_sync::spsc_push_host`, etc. — the C++ functions, not the
  ember natives). The host thread does not call ember at all.
- The **script thread** consumes via the ember natives (`spsc_try_pop`, etc.)
  in a JIT'd `@on_tick`-shaped function. Only the script thread touches the
  ember context.

This is exactly the U2 shape (§0) and it's the only shape these tests
use. **No test assumes context thread-safety** — because no test puts two
ember-calling threads on one context.

Tests:

10. **atomic cross-thread signal.** Host thread T writes
    `atomic_store_host(h, 0xDEAD)` in a loop; the script (single thread)
    spins `while (atomic_load(h) != 0xDEAD) {}` then exits 0. Assert exit 0
    (the script saw the host's release store). Proves acquire/release
    ordering across threads on an atomic.
11. **SPSC: host produces, script consumes.** Host thread T pushes N values
    (e.g. 10,000) via `spsc_push_host`; the script drains in a loop via
    `spsc_try_pop`, counting until it has seen all N. Assert the script saw
    every value exactly once, in order (SPSC FIFO). Proves the lock-free
    ring's release/acquire across the producer-consumer seam. **This is the
    canonical U2 test** — host thread, script thread, no second ember
    caller.
12. **SPSC: script produces, host consumes.** Reverse of (11). Script
    pushes N; host thread drains via `spsc_try_pop_host`. Assert host saw
    all N in order. Proves the symmetric direction (the ring is symmetric;
    the test pins it).
13. **swap buffer: script produces frames, host consumes.** Script writes a
    frame of 4 i64s, `swap`s, repeats K times; host thread reads
    `swapbuf_back_ptr` on its own tick (e.g. busy-poll the front index),
    counts complete frames. Assert host saw K complete, non-torn frames
    (each frame's 4 values belong to the same generation). Proves the
    acq_rel/rel/acq double-buffer ordering — no torn frames.
14. **MPSC: two host threads produce, script consumes.** Two host threads
    each push N values via `mpsc_push_host` (design (a): each to its own
    producer sub-handle; design (b): both to the shared ring, contending on
    the CAS). Script drains, counting total = 2N. Assert no lost pushes,
    no duplicates (the script saw each value exactly once). Proves MPSC
    correctness under two producers. (Design (a): no contention, trivially
    correct; design (b): exercises the CAS — the real test of that impl.)
15. **MPMC: host threads produce + consume, script in the middle.** Two
    host threads produce (push) and two host threads consume (try_pop_host)
    on the same MPMC queue; the script also pushes/pops a few. Total pushed
    = host_total + script_total; total popped = same. Assert every pushed
    value was popped exactly once (no lost, no dup) — check by summing
    pushed values and summing popped values, assert equal. Proves the
    internal-mutex MPMC is correct under real MPMC contention. The script's
    own push/pop are a small minority of the traffic; the host threads do
    the bulk. **No second ember caller** — only the script thread calls
    ember; the host threads use accessors.
16. **contention soak.** (11)/(14)/(15) run for 10M operations with the host
    thread(s) at high frequency; assert no crash, no lost/dup, final counts
    exact. The soak is what catches an ABA or a torn read in a lock-free
    impl that a short test misses. (For MPMC's internal-mutex impl this is
    "just a soak" — it can't livelock; for the SPSC/MPSC lock-free impls
    this is the test that proves the memory ordering is right under load.)
17. **leak-bound check.** A script in a tight loop creates + frees an
    atomic 100k times; assert the host store size stays at the high-water
    mark (free-list reuse works, no unbounded growth). Then create 100k
    without freeing; assert store grew to 100k; `reset()`; assert back to
    0. Proves the lifetime policy (§4.6).

### What the test matrix deliberately does NOT test (matches the scope)

- **No "two ember-calling threads into one context" test.** That's U1, the
  context-thread-safety batch (`plan_CONTEXT_THREADSAFETY.md` §6 tests
  1-9). This addon's tests never construct that scenario; if a reviewer
  asks "what happens if two threads call `spsc_try_pop` into the same
  context," the answer is "the context races per §0 — out of scope for this
  addon; see the context plan."
- **No blocking-pop test.** There is no blocking pop (§4.1). A script that
  wants to block writes a poll loop; that's the instruction budget's domain,
  not a queue primitive's. Don't test "blocking pop" because it doesn't
  exist.
- **No mutex test.** No script-visible mutex ships (§0). The MPMC internal
  lock is not exposed; no test targets it directly (test (15) exercises it
  indirectly via the MPMC's correctness, which is the right level).

---

## 7. Backing-store-isolation guard compliance — backing-store isolation

The backing-store isolation invariant:

> **Backing-store isolation invariant:** array/string host stores must
> never be co-located with exec memory or the dispatch table (make today's
> accident an invariant). [Closes the V4 defense-in-depth finding; spec section:
> SAFETY §5]

The V4 finding it backs: every `array`/`string` native is per-index bounds-
checked and writes to **non-executable host storage** (`std::vector<uint8_t>`
/ `std::string`), so even a hypothetical overflow lands in heap, not the
JIT code page or dispatch table. The invariant the writeup wants made
*explicit* (not accidental): **extension host stores are host heap, never
exec memory or the dispatch table.**

**This addon's compliance:**

1. **All five stores are `std::vector<Slot>` on the host heap** — exactly
   the same storage class as `ext_array`'s `g_arrays` and `ext_string`'s
   `g_strings`. `AtomicSlot` holds a `std::atomic<int64_t>` (host heap);
   `SwapBufSlot` holds two `std::vector<int64_t>` (host heap); the queue
   slots hold `std::vector<int64_t>` rings (host heap) + atomic indices +
   (MPMC only) a `std::mutex` (host heap). **None of these are ever
   allocated from `jit_memory.cpp`'s `alloc_executable`** — they're plain
   `std::vector`/`std::atomic`/`std::mutex`, which the C++ allocator places
   in default (non-executable) heap pages. On Windows, `std::vector`'s
   backing is `HeapAlloc` (RW, not RX); on Linux, `mmap` MAP_PRIVATE |
   MAP_ANONYMOUS (RW, not RX). **The queues/atomics never touch a
   PAGE_EXECUTE_READWRITE page.**
2. **The handles are 1-based small integers into a `std::vector`, not
   pointers** — mirroring `ext_array` exactly. A script holding an `atomic`
   handle holds, e.g., `3` (the third slot), not a pointer. There is no
   native in this addon that returns a raw pointer as an `i64` (the pointer-leak
   V5 finding's "no native hands out an exec-memory address" invariant is
   preserved — this addon doesn't hand out *any* address, exec or not, as
   an i64; handles are indices).
3. **The one slice/pointer accessor** — `swapbuf_back_ptr` — returns a
   `int64_t*` to the *host heap* `std::vector`'s storage, to a *host C++
   caller*, not to the script. This mirrors `ext_array::get_bytes` (which
   returns a `uint8_t*` to a host caller). The V3 pointer-provenance invariant ("a
   slice ptr cannot be forged; no native constructs a slice from a
   script-supplied i64 ptr") is preserved: `swapbuf_back_ptr` is a C++
   function the host calls directly (it's not a registered ember native;
   it has no `BindingBuilder::add` entry), so a script cannot invoke it or
   supply a forged handle to it (the handle is still a bounds-checked
   1-based index).
4. **Bounds checks on every indexed native.** `swapbuf_write`/`read` check
   `idx` against capacity (mirroring `array_set_i64`'s `size_t(i)*8+8 <=
   size` check); out-of-range is a silent no-op (write) or sentinel (read),
   exactly as `ext_array` does. The queue `push`/`try_pop` don't take an
   index (the ring manages its own), so there's no script-supplied index to
   bounds-check — the capacity check is the ring's full/empty logic. No
   unbounded write path exists.
5. **No write to a passed address.** Mirroring the V5 finding ("no native
   writes to a passed address; all writes go into host-owned stores via
   1-based handle indices"): every native in this addon writes to its own
   slot's storage (`atomic_store` writes `slot->v`; `swapbuf_write` writes
   `slot->side[front][idx]`; `push` writes `slot->buf[head % cap]`). No
   native takes a `uint8_t*`/`int64_t*` from the script and writes through
   it. The only pointer-taking natives are the `_host` accessors, which take
   `int64_t* out` *output* pointers from a *host C++ caller* (not the
   script) — and those write *to the host caller's local*, not from a
   script-supplied address.
6. **MPMC's `std::mutex` is host heap, not exec.** Stated for completeness:
   the internal lock is a `std::mutex` member of `MpmcSlot`, allocated in
   the `std::vector<MpmcSlot>` on the host heap. It's not in exec memory;
   it's not on the dispatch table. The lock's existence doesn't change the
   backing-store isolation story — it's just another host-heap member.

**Net:** the addon is backing-store-isolation-guard compliant by construction, because
it's structurally identical to `ext_array`/`ext_string` (host `std::vector`
of slots, 1-based handles, bounds-checked indexed natives, host-callable
pointer accessors that never go through the script ABI) — and it adds no
new path to exec memory or the dispatch table. The one new element (the
MPMC internal `std::mutex`) is host heap and irrelevant to the exec/dispatch
isolation invariant.

The spec-line to add (mirroring the writeup's recommendation that the V4
invariant become an explicit rule, not an accident): in
`../spec/SAFETY_AND_SANDBOX.md` §5, add or affirm — *"extension host stores
(array, string, vec, quat, mat, **sync atomics/swap-buffers/queues**) are
host heap (`std::vector`/`std::atomic`/`std::mutex`), never co-located
with executable JIT memory or the dispatch table. Handles are 1-based
indices into these stores, never pointers."*

---

## 8. Summary of the scoping decision this plan presents

The user's brief asked for a plan that's "concrete + honest about the
context-thread-safety boundary." This plan's position, in one paragraph:

**Ship a Tier-0-shaped `extensions/sync/` extension with `aint8/16/32/64`,
a swap buffer, and SPSC/MPSC/MPMC queues — all behind opaque i64 handles
into host-owned, internally-synchronized storage, mirroring
`ext_array`/`ext_string`. No script-visible mutex (deferred — couples to
function refs). All natives are non-blocking (`try_pop`, full-return-0
`push`), so no native can deadlock a script or the host; MPMC uses a
host-internal `std::mutex` held only across a short ring-index critical
section, never across an ember call, so it can't deadlock either. These
primitives serve U2 — a script coordinating with host threads on host-
owned shared state — and explicitly **do not** make the ember `context_t`
safe for concurrent calls (U1 is the separate `plan_CONTEXT_THREADSAFETY.md`
batch). Use under the U2 contract: the script side is single-threaded per
context; host threads produce/consume via the `_host` C++ accessors and
never call ember. Tests reflect this: the multi-thread stress tests never
put two ember-calling threads on one context. The backing-store-isolation guard (backing-
store isolation) is satisfied by construction — the stores are host heap,
handles are indices, no native writes to a passed address.**

The decision points the user makes from this plan:
1. **`afloat`?** Recommend no (use `aint32` bit-pun or a swap buffer).
2. **MPSC design (a) per-producer-rings vs (b) CAS-single-ring?** Recommend
   (a) for v1 simplicity unless single-handle script ergonomics outweigh
   the CAS subtlety.
3. **MPMC (alpha) internal-mutex vs (beta) Vyukov lock-free?** Recommend
   (alpha); (beta) is a drop-in future optimization.
4. **`try_pop` empty-sentinel (`INT64_MIN`) vs `try_pop` + `empty`?**
   Recommend sentinel (single-native, no race); two-return variant later.
5. **Handle `free` reclaim: tombstone+free-list vs append-only?** Recommend
   free-list (O(1), bounded growth to high-water mark).
6. **Scope-honesty placement:** the §0 statement goes verbatim into the
   addon's `ext_sync.hpp` header comment + a section in
   `../spec/SAFETY_AND_SANDBOX.md` (§8 update), so a future reader of the code
   or the spec sees the boundary without re-reading this plan.
