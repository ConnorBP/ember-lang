# ember Attack-Surface Sweep — Native Capability & Handle-Lifecycle Audit (read-only)

**Date:** 2026-07-12
**Repository:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**HEAD:** `f256ff9`
**Scope:** read-only attack-surface sweep of `ember/` — inventory every `NativeSig`/`BindingBuilder` registration under `extensions/**` and every registration set exposed by the shipping hosts; classify each native by capability + permission bit; trace every opaque-handle extension through lookup/lifecycle and test the negative/zero/oversized/stale/cross-type/double-free/concurrent matrix by inspection; sweep `src`+`extensions` for unchecked arithmetic/missing maximums/unchecked indexing/exception-boundary crossings/unsafe reinterpretation; distinguish documented accepted risks from new issues/regressions.
**Posture:** READ-ONLY. No tracked source files edited. No commits. This document is the deliverable (a new untracked file).
**Prior reports reconciled:** `SECURITY_AUDIT_20COMMITS_2026-07-12.md`, `SANDBOX_REVALIDATION_2026-07-12.md`, `OPTIMIZATION_PASSES_READ_ONLY_AUDIT_2026-07-12.md`, `FINAL_SANDBOX_REDTEAM_2026-07-11.md`. This sweep corrects the materially incomplete prior draft per the validation feedback (git status, host inventory, the hot-reload regression, the invalid GC trigger, the handle matrix, and the exception/resource findings).

---

## 0. Git status (corrected)

```
On branch master, up to date with origin/master.
Changes not staged for commit:
        modified:   thirdparty/vst3sdk   (modified content, nested submodule)
Untracked files:
        docs/audit/SANDBOX_REVALIDATION_2026-07-12.md
        docs/audit/SECURITY_AUDIT_20COMMITS_2026-07-12.md
```

So the working tree is **`M thirdparty/vst3sdk` PLUS two untracked audit documents**, not "only a dirty nested submodule". (The `SANDBOX_REVALIDATION` doc's own header line "working tree: only `thirdparty/vst3sdk` submodule dirty" is itself inaccurate on this point — it omits the two untracked `.md` files that sit next to it.) No tracked source under `src/`, `extensions/`, or `examples/` is modified relative to HEAD `f256ff9`; all findings below are against the committed tree.

---

## 1. Shipping-host native inventory (corrected)

`PERM_FFI = 1u<<0` (`src/binding_builder.hpp:40`); sema enforces it at every call site: `if ((nit->second.permission & PERM_FFI) && !(perms & PERM_FFI)) err(...)` (`src/sema.cpp:1985-1986`). A native registered with the default `permission=0` is callable by **any** module, including one compiled with `perms=0`. The permission bit is therefore the only compile-time gate on a native's capability.

### 1.1 Host registration sets

| Host | File:lines | Extensions registered | Sema permission | Notes |
|---|---|---|---|---|
| **CLI (reference)** | `examples/ember_cli.cpp:144-154` | vec, quat, mat, string, array, math, map, sync, lifecycle, io, coroutine, call_raw, audio, thread, gc (+ overloads) | `PERM_FFI` iff `--ffi` (`:435,450,1559`) | The only host that registers the full set, including audio/thread/coroutine/gc. |
| **ember_bundle** | `examples/ember_bundle.cpp:97-103` | vec, quat, mat, string, array, math, map, sync, lifecycle, io, call_raw (+ overloads) | `PERM_FFI` (`:435` EmLoadPolicy) | **Omits audio, thread, coroutine, gc.** Same allowlist as the stub. |
| **ember_stub_main** | `examples/ember_stub_main.cpp:73-79` | vec, quat, mat, string, array, math, map, sync, lifecycle, io, call_raw (+ overloads) | `PERM_FFI` (`EmLoadPolicy` per stub loader) | **Omits audio, thread, coroutine, gc.** Comment claims "Registering all standard extensions" — inaccurate (4 are absent). |
| **vst3_ember_processor** | `examples/vst3_wrapper/vst3_ember_processor.cpp:218-219` | audio, math only | `PERM_FFI` (`:222`) | The VST3 plugin processor. Typed audio accessors + math; no thread/coroutine/gc/array/map/io/call_raw. |
| **vst_dsp_harness** | `examples/vst_dsp_harness.cpp:88-98` | audio + 2 custom (`delay_buffer`, `delay_size`) | `PERM_FFI` (`:100`) | The custom pair are **ungated** (registered with default `permission=0`); audio is `PERM_FFI`. **Not** "audio+math" — math is absent here. |
| **game_host** | `examples/game_host.cpp:132-143` | 9 custom `BindingBuilder` natives only | `0` (`:94` `sema(..., 0, ...)`) | No standard extensions. The 9 are game-logic (entity get/set, delta_time, speed get/set, log). |

**game_host custom natives** (`examples/game_host.cpp:132-142`, all `permission=0`, sema run with `perms=0`):
`get_entity_count()→i64`, `get_entity_x(i64)→f32`, `set_entity_x(i64,f32)→void`, `get_entity_y(i64)→f32`, `set_entity_y(i64,f32)→void`, `get_delta_time()→f32`, `get_speed()→f32`, `set_speed(f32)→void`, `log(i64)→void`. These mutate/read host game state (entity table, speed) — host-owned, bounded (3 entities), no raw-pointer/FS/thread surface. Consistent with the `perms=0` compile. Not a capability exposure.

**vst_dsp_harness custom natives** (`examples/vst_dsp_harness.cpp:90-93`, `permission=0`):
- `delay_buffer()→i64` → `return reinterpret_cast<int64_t>(g_delay_buffer);` (`:158-160`) — **returns a raw host `float*` as an i64**, ungated.
- `delay_size()→i64` → `return g_delay_samples;` (`:162-164`) — scalar, benign.
The harness grants `PERM_FFI` to sema (`:100`) and registers audio (so `load_f32`/`store_f32` are available), placing `delay_buffer` inside the FFI trust boundary in this host. **But the native itself is mis-gated**: it is not marked `PERM_FFI`, so a host that registers `delay_buffer` without granting `PERM_FFI` hands a non-FFI script a raw host pointer (a raw-memory read/write primitive once paired with any pointer-deref native). See F-7.

### 1.2 Extension-by-extension native capability matrix

"Gate" = `PERM_FFI` bit on the `NativeSig`. "Cap" = capability class. All line refs are to the `register_natives` block unless noted.

| Extension | File | Natives | Gate | Capabilities |
|---|---|---|---|---|
| **vec** | `ext_vec.cpp:112-134` | 21 (vec2/3/4 new + xyzw getters/setters) | **none (0)** | Allocation (host heap `Vec*`), field R/W. Bounded by `MAX` per op? — see prior MEDIUM #3/#4 (fixed: try/catch around allocs). |
| **quat** | `ext_quat.cpp:65-75` | 9 | **none (0)** | Allocation, field R/W. Fixed exception containment. |
| **mat** | `ext_mat.cpp:90-95` | 4 (mat4 new/identity/get/set) | **none (0)** | Allocation, element R/W (2D index, bounds-checked). Fixed exception containment. |
| **string** | `ext_string.cpp:217-235` | 16 (new/from_slice/length/char_at/from_*/identity/find/substr/fmt1-4) | **none (0)** | Allocation, substring/fmt construction. Fixed exception containment on the constructors. **`slot()` host accessor returns a raw `std::string*` after releasing the store mutex** (`:197-199`) — see F-5. |
| **math** | `ext_math.cpp:60-99` | ~30 pure f32/f64/i64 math | **none (0)** | Pure arithmetic, no allocation/IO. Safe. |
| **array** | `ext_array.cpp:181-199` | 17 (new/length/resize/set/get u8/f32/i64/push/pop/clear/remove) | **none (0)** | **Allocation (unbounded growth up to `MAX_CONTAINER_BYTES=1GiB` per array, `:33`), byte/element R/W, raw `get_bytes()` host accessor returns `uint8_t*` after lock release (`:141-149`)** — see F-5. No per-handle free (only `reset()`). |
| **map** | `ext_map.cpp:82-90` | 7 (new/set/get/contains/length/remove/clear) | **none (0)** | **Allocation. `MAX_MAPS=100000` caps map OBJECTS (`:30`), NOT entries — entries unbounded** — see F-6. `map_set` insertion can throw on rehash, uncaught. |
| **sync** | `ext_sync.cpp:731-777` | 24 (atomic/swapbuf/spsc/mpsc/mpmc new/load/store/fetch/cas/swap/free/push/pop/size) | **none (0)** | **Blocking-adjacent (atomics CAS retry), shared queues, OS-level concurrency primitives.** Exception-contained (`*_new` wrap alloc in try/catch `bad_alloc`/`length_error`). `MAX_STORE_SLOTS=1<<20` per primitive. shared_ptr leases. |
| **lifecycle** | `ext_lifecycle.cpp:60-68` | 2 (register_routine/unregister_routine) | **none (0)** | **Allocation, fn-handle storage. Generationless free-list reuse → stale-ID ABA** — see F-4. Exception-contained on register. |
| **io** | `ext_io.cpp:216-236` | 11 (print/println/print_i64/print_f64/read_line/file_read_bytes/file_write_bytes/file_exists/path_exists/path_basename/path_dirname) | **PERM_FFI (all 11)** | **Filesystem R/W, console, blocking stdin read.** Gated. **But `read_line`/`path_basename`/`path_dirname` can throw `bad_alloc`/`length_error` uncaught** — see F-3. Uses `ext_string::slot`/`ext_array::get_bytes` (F-5). |
| **coroutine** | `ext_coroutine.cpp:394-414` | 4 (coroutine_start/next/done + `__ember_coro_yield`) | **none (0)** | **OS fiber create/switch (Windows `CreateFiberEx`), JIT execution of a fn handle, cooperative scheduling.** Slot alloc (`make_unique`+`push_back`) uncaught — see F-3. Generationless free-list + plain-i64 handle → cross-type aliasing (F-4). S4 (checkpoint misrouting across yield) documented in `SANDBOX_REVALIDATION`. |
| **thread** | `ext_thread.cpp:327-344` | 3 (thread_spawn/join/trap_reason) | **none (0)** | **OS thread spawn + JIT execution + blocking join.** Slot alloc (`make_unique`+`push_back` at `:214`) uncaught — see F-3. `std::thread` ctor wrapped (`:225-231`). Generationless free-list + plain-i64 handle → cross-type aliasing (F-4). S7 (`call_mutex` contract) documented. |
| **gc** | `ext_gc.cpp:200-218` | 7 (`__ember_gc_alloc_env`/`collect`/`live` + `gc_new`/`gc_delete`/`gc_collect`/`gc_live`) | **none (0)** | **Raw host-heap allocation + pin/root + tracing collection.** Multiple uncaught throw sites — see F-2. `gc_new(size)` rejects `size<=0` (`:105`); the prior "huge-size overflow" trigger is **invalid** on x64 (see §3). |
| **audio** | `ext_audio.cpp:206-271` | 27 (typed audio accessors + raw `load_f32`/`store_f32`/`load_f64`/`store_f64`/`load_i32`/`store_i32`) | **PERM_FFI (all 27)** | **Raw arbitrary host-memory R/W via the 6 raw natives (`:260-271`, `reinterpret_cast<float*>(ptr)[index]` unchecked)** — F-2 of `SECURITY_AUDIT_20COMMITS`, intentional/by-design. Typed accessors bounds-checked. |
| **call_raw** | `ext_call_raw.cpp:111-118` | 3 (call_raw/make_executable/free_executable_ptr) | **PERM_FFI (all 3)** | **Executable-memory mint + raw indirect call + free.** `make_executable` vector ctor uncaught — see F-3. `call_raw` null-guarded (`:83-85`); non-null garbage = UB by design. |
| **opt** | `ext_opt.cpp:1700` | 0 natives (registers IR **passes**, not natives) | n/a | Compile-time IR transforms. Not a runtime native surface. (C1–C10 opt-pass defects documented in `OPTIMIZATION_PASSES_READ_ONLY_AUDIT`; not re-litigated here.) |
| **obf** | `ext_obf.cpp:642` | 0 natives (registers IR passes) | n/a | Compile-time. |

### 1.3 Capability classes registered WITHOUT `PERM_FFI`

The sweep specifically looked for filesystem / process / raw-memory / executable-memory / thread / blocking / allocation capabilities registered without `PERM_FFI` (i.e. callable by a `perms=0` module):

| Capability class | Natives | Gate | Verdict |
|---|---|---|---|
| **Filesystem R/W** | io `file_read_bytes`/`file_write_bytes`/`file_exists`/`path_exists` | **PERM_FFI** | Properly gated. |
| **Blocking stdin read** | io `read_line` | **PERM_FFI** | Properly gated (but see F-3 for throw). |
| **Raw arbitrary host-memory R/W** | audio `load_f32`/`store_f32`/`load_f64`/`store_f64`/`load_i32`/`store_i32` | **PERM_FFI** | Properly gated (intentional FFI escape hatch; `SECURITY_AUDIT_20COMMITS` F2). |
| **Raw host pointer leak (custom)** | `vst_dsp_harness.cpp` `delay_buffer` | **none (0)** | **Mis-gated** — F-7. Returns `reinterpret_cast<int64_t>(g_delay_buffer)`. |
| **Executable-memory mint + raw call** | call_raw `make_executable`/`call_raw`/`free_executable_ptr` | **PERM_FFI** | Properly gated. |
| **OS thread spawn + blocking join** | thread `thread_spawn`/`thread_join`/`thread_trap_reason` | **none (0)** | **Ungated** — F-8. A `perms=0` module can spawn OS threads that execute JIT code and block the caller. |
| **OS fiber create/switch + JIT exec** | coroutine `coroutine_start`/`coroutine_next`/`coroutine_done`/`__ember_coro_yield` | **none (0)** | **Ungated** — F-8. |
| **Raw host-heap allocation + GC** | gc `gc_new`/`gc_delete`/`gc_collect`/`gc_live`/`__ember_gc_*` | **none (0)** | **Ungated** — F-2/F-8. |
| **Unbounded allocation (array/map/sync/string/vec/quat/mat)** | all `*_new`/`push`/`set`/insertion | **none (0)** | Ungated allocation surfaces. array/string/vec/quat/mat/sync/lifecycle are exception-contained; **map insertion and thread/coroutine/gc slot alloc are not** (F-2/F-3/F-6). |
| **Concurrency primitives (atomics/queues)** | sync 24 natives | **none (0)** | Ungated; exception-contained; bounded by `MAX_STORE_SLOTS`. |

**Bottom line on gating:** the only truly dangerous *raw* capabilities (FS, raw memory R/W, executable memory + raw call) ARE `PERM_FFI`-gated. The gating gap is the **concurrency/process/allocation** axis: thread/coroutine/gc are ungated, so a `perms=0` module can spawn threads/fibers, allocate unbounded host heap, and run the collector. Whether that is a policy bug depends on the threat model: in the CLI's default non-`--ffi` run, a script can still spawn threads and allocate GC heap. The bundle/stub hosts do NOT register thread/coroutine/gc, so their non-FFI surface is allocation-only (array/map/string/sync). The vst3 processor registers only audio+math (no thread/coroutine/gc) — its non-FFI surface is math only. The vst_dsp_harness leaks `delay_buffer` ungated (F-7).

---

## 2. Opaque-handle lifecycle matrix (per-handle case results)

All extensions use **1-based i64 handles into a `std::vector` of slots** (the "Tier-0 shape"). Two sub-patterns exist:
- **Plain-slot** (`ext_array`, `ext_map`, `ext_string`, `ext_lifecycle`): `vector<Slot>`; lookup returns a raw `Slot*` (or `std::string*`); **no per-handle free** for array/map/string (only `reset()` clears the whole store); lifecycle has a free-list.
- **shared_ptr-slot** (`ext_sync`): `vector<shared_ptr<Slot>>`; lookup returns a `shared_ptr` lease; **per-handle `*_free` exists**; free-list reuse.
- **unique_ptr-slot** (`ext_thread`, `ext_coroutine`): `vector<unique_ptr<Slot>>` with an `in_use` flag; lookup checks `in_use`; free-list reuse; `reset()` only clears when all slots are free (thread) / reclaims fibers (coroutine).
- **raw-pointer-handle** (`ext_gc`): the "handle" is the user pointer itself (`int64_t` of the heap allocation); `gc_delete` unpins; no free-list (the GC reclaims).

No extension uses a **generation counter**. The case matrix below is by inspection of the lookup + lifecycle code.

Legend: ✅ = handled safely (returns 0/nullptr/no-op, no UAF/wild access); ⚠️ = unsafe (UAF, aliasing, or logical ABA); △ = safe-by-shared_ptr-lease but stale-alias possible; ➖ = not applicable (no per-handle free).

### 2.1 Case matrix

| Extension (lookup site) | negative h | zero h | oversized h | stale h (freed, not reused) | stale h (freed+reused → ABA) | cross-type numeric alias | double-free | concurrent (lookup vs free/reset/realloc) |
|---|---|---|---|---|---|---|---|---|
| **ext_array** `arr_slot` `:56-58` | ✅ `h<1`→null | ✅ `h<1`→null | ✅ `h>size`→null | ✅ no per-handle free; slot stays valid | ⚠️ `reset()`+new `:204` revives index 1..N → stale handle aliases new array | ⚠️ plain i64; a map/string/sync/thread/coro/gc handle value indexes `g_arrays` | ➖ no free native | ⚠️ `get_bytes` `:141-149` returns `uint8_t*` after lock release; `resize`/`push`/`clear`/`reset` on the handle UAFs the caller (F-5) |
| **ext_map** `map_slot` `:32-34` | ✅ | ✅ | ✅ | ✅ no per-handle free | ⚠️ `reset()`+new `:95` revives index | ⚠️ plain i64 cross-type | ➖ | ✅ all ops under `g_store_mutex`; no raw-ptr-out accessor |
| **ext_string** `str_slot` `:45` | ✅ | ✅ | ✅ | ✅ no per-handle free | ⚠️ `reset()`+new `:253` revives index | ⚠️ plain i64 cross-type | ➖ | ⚠️ `slot()` `:197-199` returns `std::string*` after lock release; a concurrent `string_new` (vector `push_back` reallocates) UAFs the caller (F-5) |
| **ext_lifecycle** `n_unregister_routine` `:47-56` | ✅ bounds-checked | ✅ | ✅ | ✅ `active=false` tombstone; re-unregister returns 0 | ⚠️ **generationless free-list reuse**: unregister pushes id to `g_free` (`:55`), register reuses it (`:40-42`) with a NEW handle/data → stale id aliases new routine; `unregister_routine(stale_id)` silently removes the reused routine (F-4) | ⚠️ plain i64 cross-type | ➖ (unregister is idempotent on tombstone) | ✅ under `g_mutex` |
| **ext_sync** `atom_slot`/`swapbuf_slot`/`spsc_slot`/`mpsc_slot`/`mpmc_slot` `:79-82` etc. | ✅ | ✅ | ✅ | △ `*_free` resets shared_ptr + pushes handle to free-list; a stale handle lookup returns the **null shared_ptr** (slot reset) → `if (!s) return 0` safe | ⚠️ **generationless free-list reuse**: `*_new` reuses the handle index with a fresh `make_shared` (`:109-115` etc.) → stale handle aliases a NEW object of the same primitive (logical ABA; the OLD shared_ptr is gone so no UAF, but the script's stale handle now names a different atomic/queue with different width/capacity) (F-4) | ⚠️ plain i64 cross-type (an atomic handle value indexing `g_swapbufs` returns a SwapBufSlot shared_ptr → wrong primitive, but each `*_op` re-locks + re-looks-up via its own typed slot fn, so the cross-type alias is read as the wrong primitive's slot) | ✅ `*_free` is idempotent-ish (second free: `atom_slot(h)` after reset returns null → no-op) | ✅ shared_ptr lease keeps slot alive across free/reset; operations on the leased slot are lockless but the slot itself is stable |
| **ext_thread** `raw_slot` `:74-78` | ✅ | ✅ | ✅ `h>size`→null | ✅ no per-handle free; `in_use` stays true after join (so `trap_reason` works); `reset()` only clears when all free | ⚠️ **generationless free-list reuse**: `thread_spawn` reuses `g_threads_free` index (`:207-211`) with a fresh `ThreadSlot` → stale handle aliases a new thread; `thread_join(stale)`/`trap_reason(stale)` operate on the wrong thread (F-4) | ⚠️ plain i64 cross-type (a coroutine handle value indexing `g_threads` → if that slot is an in_use ThreadSlot, `thread_join` waits on the wrong cv) | ➖ no free native (reset only) | ✅ slot lookup under `g_setup_mutex`; `ThreadSlot*` stable for lifetime (in_use slots never erased) |
| **ext_coroutine** `raw_slot` `:128-131` | ✅ | ✅ | ✅ | ✅ `in_use` flag | ⚠️ **generationless free-list reuse** (`:298-302`) → stale handle aliases new coroutine (F-4) | ⚠️ plain i64 cross-type | ➖ (no per-handle free; `coroutine_reset` reclaims) | ✅ under `g_setup_mutex`; plus S4 (checkpoint misrouting, documented) |
| **ext_gc** (handle = user ptr) `is_live` `gc.cpp:71` | ✅ `size<=0` rejected at alloc `:105`; `gc_delete`/`is_live` on a non-live ptr return 0/false | n/a (0 is never a live ptr) | n/a | ✅ `gc_delete` unpins (idempotent: second unpin returns 0 `:78-83`); the GC frees the memory; `is_live`→false | ⚠️ **no generation**: after GC frees an object, `malloc` can reuse the exact address → a stale `env_ptr` aliases a NEW GC object; `gc_delete(stale)` unpins the new object; `is_live(stale)`→true (F-4) | ⚠️ a raw host pointer from anywhere (audio `load_f32` result, `delay_buffer`, etc.) is a "valid" gc handle iff it's in `m_live` → cross-type aliasing with any host pointer that collides | ✅ no double-free (delete is unpin) | ⚠️ `m_live.insert` (`gc.cpp:57`), `pinned[env]` (`ext_gc.cpp:73`), `add_root` push_back (`gc.cpp:60`) can throw after alloc; concurrent collect vs alloc is NOT mutex-protected in `GcHeap` (the extension locks only its own `pinned` map, not the heap) — F-2 |

### 2.2 Key handle-lifecycle findings

- **F-4 (MEDIUM, new characterization) — generationless free-list / address reuse → stale-handle ABA across ext_sync, ext_lifecycle, ext_thread, ext_coroutine, ext_gc; reset+realloc revives stale IDs in ext_array/ext_map/ext_string.** No extension embeds a generation/version in the handle. Concrete exploits:
  - **lifecycle**: `register_routine(&A, d1)`→id 5; `unregister_routine(5)`; `register_routine(&B, d2)` reuses id 5; a script still holding 5 now names routine B; `unregister_routine(5)` silently removes B. Evidence: `ext_lifecycle.cpp:40-42` (reuse), `:47-56` (unregister by id, no gen check).
  - **sync**: `atomic_new`→h 5; `atomic_free(5)`; `atomic_new` reuses h 5 with a different width; a stale h 5 now names the new atomic. The OLD shared_ptr is gone (no UAF), but the script's logical identity is broken. Evidence: `ext_sync.cpp:109-115` (reuse), `:197-205` (free).
  - **thread/coroutine**: `thread_spawn`→h 5; (reset reclaims after join); `thread_spawn` reuses h 5; stale h 5 names the new thread. Evidence: `ext_thread.cpp:207-211`; `ext_coroutine.cpp:298-302`.
  - **array/map/string**: `reset()` clears the store (`ext_array.cpp:204`, `ext_map.cpp:95`, `ext_string.cpp:253`); the next `*_new` returns 1 again → a stale handle 1 held across `reset()` aliases a brand-new object. No per-handle free exists, so this is the only reuse path.
  - **gc**: the handle IS the user pointer; after the GC frees an object, `malloc` may hand back the same address → stale `env_ptr` aliases a new GC object; `gc_delete(stale)` unpins the new object; `is_live(stale)`→true.
  - **Cross-type numeric aliasing (all plain-i64 handles)**: array/map/string/sync/lifecycle/thread/coroutine handles are all bare 1-based i64s with no type tag (sema tags the *parameter* for lifecycle's `fn_handle` and audio's struct handles, but the *runtime handle value* is an untagged i64). A script that passes a map handle (say 3) to `array_get_u8(3, 0)` gets `g_arrays[2]` — if a third array exists, it reads array bytes; if not, `arr_slot` returns null → 0. The cross-type alias is only dangerous when both stores happen to have a live slot at the same index, but it is a real confused-deputy surface. The shared_ptr extensions (sync) re-lookup via their own typed slot fn, so a cross-type alias reads the wrong primitive's slot (still wrong, but not a wild pointer). The plain-slot extensions (array/map/string) return a raw pointer into the wrong store's vector → read/write of the wrong object's bytes.

  Severity MEDIUM: requires the script to hold a stale handle across a free+realloc (or across `reset()`), or to mix handle types. Not a wild-pointer UAF in the shared_ptr extensions (lease protects), but a logical-identity break + confused deputy; in the plain-slot extensions a cross-type alias is a raw read/write of the wrong store.

- **F-5 (MEDIUM, broader than the prior single call_raw TOCTOU) — `ext_string::slot` and `ext_array::get_bytes` return raw pointers after releasing their store mutexes, opening concurrent-realloc/reset UAF windows in every IO/audio/call_raw consumer.**
  - `ext_string::slot` (`ext_string.cpp:197-199`): takes `g_store_mutex`, returns `str_slot(handle)` (a `std::string*` into `g_strings`), **then the `lock_guard` dtor releases the mutex**. The caller now holds a raw `std::string*` with no lock.
  - `ext_array::get_bytes` (`ext_array.cpp:141-149`): takes `g_store_mutex`, sets `*out_data = s->bytes.data()`, `*out_len = ...`, returns true, **then releases the mutex**. The caller holds a raw `uint8_t*` into the vector's buffer with no lock.
  - Consumers: every `ext_io` native that takes a string/array handle — `n_print`/`n_println` (`ext_io.cpp:71-73`, `s->data()`), `n_file_read_bytes` (`:129` `path->c_str()`), `n_file_write_bytes` (`:154` `path->c_str()` + `:158` `data`/`len`), `n_file_exists` (`:173`), `n_path_exists`/`n_path_basename`/`n_path_dirname` (`:186,195,204`) — and `ext_call_raw::n_make_executable` (`ext_call_raw.cpp:91` `get_bytes` then `std::vector<uint8_t> code(data, data+size_t(len))` reads `data` unlocked). The audio host accessors are not affected (audio operates on the host-provided `AudioContext`, not via slot/get_bytes).
  - Trigger: two `context_t`s (or a host thread + a script thread) — thread A calls `print(s)` (holds `s = slot(h)` after lock release); thread B calls `string_new()` (`push_back` on `g_strings` → vector reallocates → the `std::string` A holds is moved/destroyed → A's `s->data()` is a dangling read) or `string_from_slice` (same) or `reset()`. Same shape for `file_write_bytes` using `get_bytes` + a concurrent `array_push`/`resize`/`reset`. The call_raw path is the single TOCTOU the prior draft reported; the same defect class covers all IO string/array consumers.
  - Severity MEDIUM (DoS/crash via UAF read or write; requires concurrency, which the thread/coroutine extensions explicitly enable). The fix is to either copy under the lock (the IO natives already mostly copy/forward) or hold the lock across the consumer's use (a `shared_ptr`-lease model like ext_sync, or a recursive lock + RAII lease).

---

## 3. Arithmetic / maximums / indexing / cast sweep

Across `src/` and `extensions/`, searched for unchecked add/multiply/align/cast on attacker-controlled counts/lengths/frame-sizes/offsets/alloc-sizes/file-sizes; missing maximums; unchecked vector/string indexing; `bad_alloc`/standard exceptions crossing the C/native boundary; unsafe pointer reinterpretation.

### 3.1 Confirmed-safe (verified, no issue)

- **`ext_array` element bounds** — division-based check `size_t(i) < bytes.size()/elem_size` (`ext_array.cpp:70-75`); `checked_bytes` + `MAX_CONTAINER_BYTES=1GiB` cap (`:33-39`); resize/push guarded by `MAX_CONTAINER_BYTES`. Prior HIGH #2 fix intact.
- **`ext_array` arithmetic** — `checked_bytes` rejects `es > MAX/n` (mul overflow) (`:37`) and `*out > MAX` (`:39`). No add-overflow in `n_array_push_*` (`:79,86,93` use `s->bytes.size() + N` with a `<= MAX` guard first).
- **v5 IR `frame_off` validator** — `src/thin_ir_ser.cpp:668-672` checks every instr with `frame_off != 0`, rejects `>= 0` and `< -frame_size`. Prior CRITICAL #1 fix intact.
- **v5 IR duplicate block IDs** — `src/thin_ir_ser.cpp:605` `seen_ids.insert(...).second`. Prior LOW #6 fix intact.
- **`bounds-elim` pass** — sound on the canonical loop; reviewed in `SECURITY_AUDIT_20COMMITS` §5. Not re-litigated.
- **`ext_sync` capacity math** — `MAX_CONTAINER_BYTES / (2*sizeof(int64_t))` for swapbuf (`:256`), `/ per_producer` for mpsc (`:497`), `MAX_I64_SLOTS` for mpmc (`:648`); `round_up_pow2` overflow-guarded. `MAX_STORE_SLOTS=1<<20` per primitive.
- **vec/mat/quat/string allocation exception containment** — prior MEDIUM #3/#4 fix intact (try/catch `bad_alloc`/`length_error` around the constructors).

### 3.2 F-2 (HIGH, corrected) — GC exception boundaries throw after allocation, uncaught across the native boundary

The prior draft's **F-1 ("`gc_new(0xFFFFFFFFFFFFFFF0)` overflows size_t after header add") is INVALID on the supported x64 build and is removed.** `gc_new(0xFFFFFFFFFFFFFFF0)` passes `int64_t size = -16` (the literal is a negative `int64_t`), and `gc_alloc_env` rejects it at `ext_gc.cpp:105` (`if (!r || size <= 0) return 0;`). Positive script input is at most `INT64_MAX`; the header add is `hdr_total = sizeof(Header) + ref_bytes + 4` (≤ a few dozen bytes) and `total = hdr_total + size`; for `size ≤ INT64_MAX` on 64-bit `size_t`, `total` does not overflow `size_t` (and `std::malloc(total)` with `total ≈ INT64_MAX` returns nullptr, handled at `gc.cpp:45`). No overflow-reachable path.

The **real** GC exception-boundary issues (per feedback point 4), all uncaught across the native → JIT boundary:

1. **Lazy `make_unique` in `rt()`** — `ext_gc.cpp:43-44`: `if (!g_rt) g_rt = std::make_unique<GcRuntime>();`. `GcRuntime` contains `gc::GcHeap heap` + `std::unordered_map<void*, void**> pinned`; constructing the map can throw `bad_alloc`. `rt()` has no try/catch and is called from every GC native (`gc_alloc_env:104`, `gc_collect:124`, `gc_live_count:139`, …). A throw propagates through the native → JIT (no unwind info) → `std::terminate`.
2. **`m_live.insert(user)` in `GcHeap::alloc`** — `src/gc.cpp:57`: `m_live.insert(user);` (an `unordered_set<void*>`) can throw `bad_alloc` on rehash **after** `std::malloc(total)` succeeded and the header was written. The allocated block is **leaked** (no cleanup; `m_live` does not contain it, so `clear()`/`collect()` will never free it), `m_stats` is left inconsistent (the `live_objects++`/`live_bytes +=` at `:58-59` run before the insert? — no, insert is at `:57`, stats after; if insert throws, stats are not incremented but the block is leaked). The exception propagates up through `gc_alloc_env` (no try/catch) → native boundary → JIT → terminate.
3. **`pin_env`'s `pinned[env] = slot`** — `ext_gc.cpp:73`: `r->pinned[env] = slot;` (`unordered_map::operator[]` can throw `bad_alloc` on rehash) **after** the slot was `new`-allocated (`:75` nothrow, handled) AND `r->heap.add_root(slot)` registered it (`gc.cpp:60` `m_roots.push_back(addr)` — **also** a throw site, vector growth). If `pinned[env]` throws: the slot is leaked, the root is registered-but-untracked (so `gc_reset`'s `for (auto& kv : g_rt->pinned)` loop at `:62-66` will never find it to `remove_root`/`delete` it → permanent leak + a dangling root slot pointing at the freed env after a later collect), and the exception propagates. This is called from `gc_alloc_env:111` **after** a successful alloc — so a throw here leaks the just-allocated env too.
4. **`GcHeap::add_root` `m_roots.push_back`** — `src/gc.cpp:60`: vector growth `bad_alloc`; called from `pin_env` after slot alloc; same leak/propagate shape as (3).
5. **`GcHeap::collect` worklist/vector growth** — `src/gc.cpp:78` `std::vector<void*> worklist;` + `worklist.push_back(obj)` (`:93,108`); `to_free.push_back` (`:122`). All can throw `bad_alloc`; `collect` is called from `gc_collect` (no try/catch) and **from inside `gc_alloc_env`** (`ext_gc.cpp:113-115` auto-collect trigger) — a throw during the auto-collect propagates out of `gc_alloc_env` after the env was already allocated and pinned → the env is "live" but the native returns by throwing → terminate, and the script never gets the pointer (leak from the script's view).

**Trigger:** memory pressure during any `gc_new`/`__ember_gc_alloc_env`/`gc_collect` (the auto-collect at `ext_gc.cpp:113` fires when `live_objects >= threshold`, so a script that allocates `threshold` envs then one more triggers a collect inside the alloc). Realistic under a low-RAM host or a large `threshold`+many-live scenario. **Impact:** `std::terminate` (process abort) because the C++ exception unwinds through JIT frames with no unwind info (the host thunks `ember_call_void_thunk`/`ember_call_i64_thunk` at `src/engine.cpp:222-247` are pure asm with no try/catch; JIT code is pure asm). Plus leaked blocks/roots in the (1)/(3)/(4) cases.

**Severity HIGH** because `gc_new`/`gc_collect` are **ungated** (`permission=0`) — a `perms=0` module can trigger this with no `PERM_FFI`. The other allocation natives that throw-and-terminate (F-3) are at least partly gated (io) or narrower.

**Recommendation (not applied):** wrap `rt()`, `gc_alloc_env`, `gc_collect`, and the `pin_env` map/root insertions in try/catch that returns 0 / a safe sentinel on `bad_alloc`/`length_error`, and free the just-allocated block + slot + unregister the root before returning 0. Mirror `ext_sync`'s `n_*_new` pattern (`ext_sync.cpp:128-130` etc.).

### 3.3 F-3 (MEDIUM) — uncaught `bad_alloc`/`length_error` across the native boundary in map insertion, thread/coroutine slot alloc, `make_executable`, and IO string growth

The C++ exception → JIT → `terminate` shape (F-2) applies to every native that allocates without a try/catch. Confirmed uncaught sites:

- **`ext_map::n_map_set`** — `ext_map.cpp:49-51`: `s->entries[k] = v;` (`:51`) — `unordered_map::operator[]` can throw `bad_alloc` on rehash. **`n_map_new`'s `emplace_back` IS caught** (`:41-42` `catch (...) { return 0; }`), but `map_set` insertion is NOT. Per feedback point 6: `map_get`/`map_contains`/`map_length`/`map_remove`/`map_clear` do **not** allocate (lookup/erase/clear only) — only `map_set` (insertion) is the risk. **`MAX_MAPS=100000` caps map objects, NOT entries** (`:30` checks `g_maps.size() >= MAX_MAPS` only in `n_map_new`) — entries are unbounded, so a script can grow one map's `unordered_map` until a rehash throws. Trigger: `perms=0` module (map is ungated) calling `map_set` in a loop under memory pressure.
- **`ext_thread::n_thread_spawn` slot alloc** — `ext_thread.cpp:214`: `g_threads.push_back(std::make_unique<ThreadSlot>());` is **outside** the try/catch (the try at `:225` only wraps `std::thread` ctor). `make_unique`/`push_back` `bad_alloc` → terminate. (The `std::thread` ctor throw IS caught at `:226-231`, freeing the slot.)
- **`ext_coroutine::n_coroutine_start` slot alloc** — `ext_coroutine.cpp:305`: `g_coros.push_back(std::make_unique<Coroutine>());` unwrapped. `CreateFiberEx` (`:321`) is a C call returning nullptr (handled at `:323-330`), but the `make_unique`/`push_back` throw is not.
- **`ext_call_raw::n_make_executable`** — `ext_call_raw.cpp:88`: `std::vector<uint8_t> code(data, data + size_t(len));` can throw `bad_alloc` for a large `len` (the `len` came from `get_bytes`, bounded by `MAX_CONTAINER_BYTES=1GiB` at array alloc, but a 1GiB vector ctor can still throw on a constrained host). No try/catch. (Gated by `PERM_FFI`, so lower severity than the ungated map/thread/coroutine/gc cases.)
- **`ext_io::n_read_line`** — `ext_io.cpp:92-95`: `line += buf;` (string append growth) can throw `bad_alloc`; no try/catch. (Gated `PERM_FFI`.)
- **`ext_io::n_path_basename`/`n_path_dirname`** — `ext_io.cpp:196-204` → `path_basename_str` (`:47` `p.substr(pos, end-pos)`) / `path_dirname_str` (`:57` `p.substr(...)`): substring construction can throw `bad_alloc`/`length_error`; no try/catch around the `alloc(path_basename_str(*path))` call. (Gated `PERM_FFI`.)

**Already-contained (contrast, model to follow):** `ext_array` (`:44-52,67-68,80-81,87-88,94-95,163-173`), `ext_lifecycle` (`:36-46`), `ext_sync` (`:128-130,275-277,421-424,517-528` all `*_new` wrap alloc in try/catch), `ext_string` constructors (`:49-56,66-69,72-76,84-88,91-94,101-112`), vec/mat/quat (prior fix). The gaps are specifically map-insertion, thread/coroutine slot-alloc, GC (F-2), make_executable, and the two IO path/string natives.

**Severity MEDIUM** (the ungated map/thread/coroutine cases are reachable by `perms=0` modules; the gated io/call_raw cases are within the FFI trust boundary but still terminate the process rather than trapping). Trigger: memory pressure during the named allocation. Impact: `std::terminate` (no unwind info through JIT).

### 3.4 F-6 (LOW-MEDIUM) — `ext_map` entries unbounded

`MAX_MAPS=100000` (`ext_map.cpp:30`) caps the number of map **objects**, checked only in `n_map_new` (`:40`). It does **not** cap entries per map. `map_set` (`:49-51`) inserts without an entry-count or byte-count limit. A `perms=0` module (map is ungated) can grow a single map's `unordered_map` without bound (until `bad_alloc` on rehash → F-3 terminate, or OOM-kill). This is a resource-exhaustion / DoS surface. (Compare `ext_array`'s `MAX_CONTAINER_BYTES=1GiB` per array, `ext_sync`'s `MAX_CONTAINER_BYTES`/`MAX_STORE_SLOTS`.) Recommendation: cap entries per map (or total bytes) and reject `map_set` over the cap.

### 3.5 F-7 (LOW) — `vst_dsp_harness` `delay_buffer`/`delay_size` mis-gated (raw host pointer, `permission=0`)

`examples/vst_dsp_harness.cpp:90-93` registers `delay_buffer`/`delay_size` via `state_bindings.add(...)` with the **default `permission=0`** (no `PERM_FFI`). `n_delay_buffer` (`:158-160`) returns `reinterpret_cast<int64_t>(g_delay_buffer)` — a raw host `float*`. The harness grants `PERM_FFI` to sema (`:100`) and registers audio, so within this host the native is inside the FFI trust boundary. **But the native itself is not tagged `PERM_FFI`**, so a host that copies this registration pattern without granting `PERM_FFI` hands a non-FFI script a raw host pointer (a raw-memory read/write primitive once paired with any pointer-deref native). Defense-in-depth: tag `delay_buffer` (and any raw-pointer-returning custom native) with `PERM_FFI`. (Example/harness code, low blast radius, but a bad pattern to copy.)

### 3.6 F-8 (INFO/policy) — thread/coroutine/gc ungated

`ext_thread` (`:327-344`), `ext_coroutine` (`:394-414`), `ext_gc` (`:200-218`) register all natives with `permission=0`. A `perms=0` module can therefore spawn OS threads, create/switch fibers, allocate GC heap, and run the collector — capabilities that are arguably in the same class as the gated `call_raw`/audio/io surface (process-level effects, JIT execution, host heap). The bundle/stub hosts do not register these three; the CLI does (so `ember` without `--ffi` still exposes them); the vst3 processor does not. This is a **policy observation**, not a code bug: if the intent is "non-FFI scripts are sandboxed", then thread/coroutine/gc should be `PERM_FFI`-gated (or a new `PERM_CONCURRENCY`/`PERM_ALLOC` bit). If the intent is "these are core language facilities", the current state is consistent. Flagging for an explicit decision; no code change required if the policy is intentional.

### 3.7 Unsafe pointer reinterpretation (sweep)

- **audio raw natives** — `reinterpret_cast<float*>(ptr)[index]` etc. (`ext_audio.cpp:180-200`): intentional, `PERM_FFI`-gated (F2 of `SECURITY_AUDIT_20COMMITS`).
- **`call_raw`** — `reinterpret_cast<Fn>(fn_ptr)` (`ext_call_raw.cpp:85`): intentional, `PERM_FFI`-gated, null-guarded.
- **vst3 `save_state`/`load_state`** — `reinterpret_cast<const ScriptStateBuffer*>(old->save_state())` (`vst3_ember_processor.cpp:483-490`): F3 of `SECURITY_AUDIT_20COMMITS`, defense-in-depth, within FFI trust boundary. Unchanged.
- **`gc` header recovery** — `reinterpret_cast<Header*>(static_cast<char*>(obj) - hb)` (`src/gc.cpp:81,99,128,135`): `hb` is read from `user - 4` (the `header_bytes` trailer stored at alloc `:55`). If a non-GC pointer is passed to `is_live`/`gc_delete`, `is_live` returns false (the `m_live.count` check gates), so the header is never recovered for a foreign pointer. Safe for `is_live`/`gc_delete`. The header recovery inside `collect` only runs on pointers known to be in `m_live`. Safe.
- **`gc` `hdr->size = uint32_t(size)` truncation** — `src/gc.cpp:43`: `size` is `size_t` (from `gc_alloc_env(int64_t size)` with `size > 0`, so `size ∈ [1, INT64_MAX]`); stored as `uint32_t`. For `size > UINT32_MAX` (≥ 4 GiB), the stored size truncates. `std::memset(user, 0, size)` at `:56` uses the **full** `size_t` (correct), but the header's `size` field is truncated. Downstream: `collect`'s `live_bytes -= hdr->size` (`:135`) underflows; the `off + 8 > hdr->size` ref-offset guard (`:103`) uses the truncated value (a real offset could be falsely judged out-of-bounds → safe direction, just skips a ref). Requires a successful `malloc(>4GiB)` which is unlikely on a constrained host but possible on a 64-bit system with enough RAM. **LOW** — note for completeness; not practically exploitable for memory unsafety (the truncation is in the safe direction for the ref guard; the `live_bytes` underflow is a stats-cosmetic issue). Fix: reject `size > UINT32_MAX` in `gc_alloc_env`, or widen the header `size` field to 64-bit.

---

## 4. The current hot-reload regression (F-1, HIGH, must be in the conclusion)

**This is a documented, current, HIGH regression introduced by commit `587f9d4` (VST3 Phase 7) and is present at HEAD `f256ff9`.** It is detailed in `SECURITY_AUDIT_20COMMITS_2026-07-12.md` §F1 and **must be carried in the "no regressions" conclusion** — the prior draft's "no regressions" was wrong to omit it.

**Files:** `examples/vst3_wrapper/vst3_ember_processor.cpp`, `examples/vst3_wrapper/vst3_ember_processor.h`.

**Root cause — TOCTOU between the watcher's grace-period check and the audio thread's retire+use:**

The watcher reclaims with a single observation of the reader counter (`reclaimRetiredModule`, `:505-508`):
```cpp
void EmberProcessor::reclaimRetiredModule() {
    if (audio_readers_.load(std::memory_order_acquire) != 0) return;   // :506
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);       // :507
}
```

The audio thread enrolls, retires the old module into `retired_`, then uses it for the crossfade (`process`, `:785-803`):
```cpp
    audio_readers_.fetch_add(1, std::memory_order_acq_rel);             // :785  enroll
    {
        EmberModule* old = activatePendingModule();                     // :787  retire old into retired_ at :499
        ...
        if (fadeFrames > 0) {
            ...
            old->process_f32(reinterpret_cast<int64_t>(&oldContext),    // :802  EXECUTE old's JIT pages
                             static_cast<int64_t>(fadeFrames));
        }
```

`activatePendingModule` (`:499`): `if (old) retired_.store(old, std::memory_order_release);`

**Race timeline:**
1. No `process()` in flight: `audio_readers_ == 0`. `retired_` holds a prior module (or null).
2. Watcher: `audio_readers_.load() == 0` (`:506`) — decides to reclaim.
3. Audio thread enters `process()`: `fetch_add(1)` → readers=1 (`:785`).
4. Audio thread: `activatePendingModule()` consumes `pending_` (set earlier by the watcher), sets `current_ = candidate`, **`retired_.store(old = previous current)`** (`:499`), returns `old`.
5. Audio thread: `fadeFrames > 0` (default `EMBER_VST3_CROSSFADE_SAMPLES = 64`), calls `old->process_f32(...)` (`:802`) — executing the old module's JIT code, which is the value now in `retired_`.
6. Watcher: `retired_.exchange(nullptr)` (`:507`) returns the module stored at step 4 (`old`) and **`delete`s it** — `~EmberModule` calls `ember::free_executable` on each `fn.exec`, unmapping the executable page the audio thread is still executing at step 5.

**Result:** use-after-free / execution of freed executable memory on the realtime thread. The `:506` check only proves no reader was active *at the check*; it does not prevent a reader from enrolling afterward (`:785`), retiring a module (`:499`), and using it (`:802`) before the watcher's `delete` (`:507`).

**Why this is a regression:** Phase 5/6 (`01f38d9`/`5c89528`) used an epoch-based `ember::HotReloadDomain` + `ExecutionGuard` (`src/hot_reload.hpp`) that freed a retired page only when no guard that entered before the page's retirement epoch was still active, with `activatePendingModule` serialized under the domain mutex. Phase 7 (`587f9d4`) replaced that with the single `audio_readers_` counter, discarding the serialization. The replacement is strictly weaker.

**Trigger conditions (practically valid):** plugin built with hot reload enabled (default), audio processing active, a script edit producing a new `pending_` module, `EMBER_VST3_CROSSFADE_SAMPLES ≥ 1` (default 64), f32 process path. Reachable by ordinary user action (editing the ember script while the DAW plays). Non-deterministic / timing-sensitive; the in-tree `runHotReloadStress()` (`vst3_stress_tests.cpp`) exercises the scenario but its 350 ms inter-reload sleep makes the tight window unlikely to fire, and it is not run under ThreadSanitizer.

**Severity: HIGH.** Freed object is executable JIT memory unmapped mid-execution on the realtime thread → crash or, depending on allocator reuse, corrupt execution.

**Recommended fix (not applied — read-only):** restore an epoch/RCU guarantee. Either (a) keep `HotReloadDomain` (the Phase-5/6 design), or (b) make the counter check and retire store non-overlapping: re-check `audio_readers_` *after* `retired_.exchange()` and only `delete` if still 0 (else re-publish to `retired_` for a later retry) — note this still has a residual window if a reader enrolls between the second load and `delete`; the fully sound fix is the epoch domain, or retire `old` only after the crossfade completes / pin `old` with a reference count.

---

## 5. Documented accepted risks (not new, verified unchanged)

These are pre-existing, documented, and confirmed still present at HEAD. Listed so the synthesis can distinguish "accepted risk" from "new issue/regression".

- **S1** budget/depth/trap OFF by default (`codegen.hpp:100,110,88`); no `safe_defaults()`. CLI opts in for JIT only (`ember_cli.cpp:528-530`). (`SANDBOX_REVALIDATION` S1.)
- **S2** lambda `env_ptr` escape; GC-heap env structural fix exists but opt-in (`use_gc_env=false` default); sema `is_lambda` stopgap NOT added. (`SANDBOX_REVALIDATION` S2.)
- **S4** coroutine checkpoint + per-call state misrouting across yield (`ext_coroutine.cpp:209-274,343-373`); untested. (`SANDBOX_REVALIDATION` S4.)
- **S5** trap = process death by default (`ud2` when `trap_stub=null`). (`SANDBOX_REVALIDATION` S5.)
- **S6** call-target guard no-op when unconfigured (`fn_slot_count==0`). (`SANDBOX_REVALIDATION` S6.)
- **S7** in-context thread `call_mutex` contract unenforced. (`SANDBOX_REVALIDATION` S7.)
- **N1** ThinIR silently miscompiles cross-module function handles (`thin_lower.cpp:1631-1637`); HIGH, opt-in (`enable_ir_backend`). (`SANDBOX_REVALIDATION` N1.)
- **N2/N3** ThinIR double-guard / no cross-aware guard. (`SANDBOX_REVALIDATION` N2/N3.)
- **C1/C2** sandbox guards stripped on v5 `.em` re-emit + CLI `--emit-em` guard-free. (`SANDBOX_REVALIDATION` C1/C2.)
- **OPT C1–C10** pre-existing optimization-pass correctness defects (CSE/DCE/ConstProp/LICM/Forward/DSE/InstCombine), incl. C3 (implicit-frame-write erasure), C7 (non-SSA forwarding), C9 (signed-overflow const folding). Not modified/worsened by the recent window. (`OPTIMIZATION_PASSES_READ_ONLY_AUDIT`; `SECURITY_AUDIT_20COMMITS` §4.)
- **audio raw natives F2** — `load_f32`/`store_f32`/`load_f64`/`store_f64`/`load_i32`/`store_i32` unchecked arbitrary host-memory R/W, `PERM_FFI`-gated, intentional. (`SECURITY_AUDIT_20COMMITS` F2.)
- **vst3 `save_state`/`load_state` raw-pointer reinterpret F3** — defense-in-depth, within FFI trust boundary. (`SECURITY_AUDIT_20COMMITS` F3.)
- **release script F4 / `BlockMergePass` F5 / realtime-test F6** — LOW/INFO. (`SECURITY_AUDIT_20COMMITS` F4/F5/F6.)

---

## 6. Prior-fix verification (all intact at HEAD, no regressions except F-1)

| Prior fix | Site | Status |
|---|---|---|
| v5 IR `frame_off` validator (CRITICAL #1) | `src/thin_ir_ser.cpp:668-672` | ✅ intact, not touched by recent commits |
| `ext_array` element-bounds (HIGH #2) | `ext_array.cpp:70-75` + `checked_bytes`/`MAX_CONTAINER_BYTES` | ✅ intact |
| vec/mat/quat/string alloc exception containment (MEDIUM #3/#4) | per-extension try/catch | ✅ intact (string constructors covered; the `slot()` accessor UAF is a separate issue, F-5) |
| v5 IR duplicate block IDs (LOW #6) | `src/thin_ir_ser.cpp:605` | ✅ intact |
| `PERM_FFI` 3 gates (sema + v2-v4 bind + v5 IR rebind) | `sema.cpp:1985`, `em_loader.cpp:435,713-716` | ✅ intact |
| raw-x86 default-reject | `em_loader.hpp:130`, `em_loader.cpp:581-585` | ✅ intact |
| `catch_bufs` overflow (S3) | `src/codegen.cpp:4521-4525` | ✅ intact (commit `61aa818`) |
| `bounds-elim` pass soundness | `ext_opt.cpp` `BoundsCheckElimPass` | ✅ sound (reviewed `SECURITY_AUDIT_20COMMITS` §5) |

**The one regression is F-1 (§4):** the Phase-7 hot-reload `audio_readers_` TOCTOU UAF (`587f9d4`), a step down from the sound epoch-based reclamation it replaced. This is the only HIGH regression in the recent commit window and it stands at HEAD.

---

## 7. Prioritized findings (for synthesis)

| # | Sev | New/Reg/Accepted | Finding | Evidence | Trigger |
|---|---|---|---|---|---|
| **F-1** | **HIGH** | **REGRESSION (commit `587f9d4`)** | Hot-reload `audio_readers_` TOCTOU UAF: watcher frees `retired_` (`:507`) while the audio thread crossfades on `old->process_f32` (`:802`) after enrolling (`:785`) and retiring (`:499`) | `vst3_ember_processor.cpp:506-507,499,785,802` | Edit a script during DAW playback (crossfade path, default 64 samples) |
| **F-2** | **HIGH** | **NEW (characterization)** | GC exception boundaries throw after allocation, uncaught across native→JIT boundary → `terminate` + leaks: `rt()` `make_unique` (`ext_gc.cpp:43-44`), `m_live.insert` (`gc.cpp:57`), `pinned[env]`+`add_root` (`ext_gc.cpp:73`/`gc.cpp:60`), `collect` worklist (`gc.cpp:78,93,108,122`) + auto-collect inside `gc_alloc_env` (`ext_gc.cpp:113`) | as cited | Memory pressure during `gc_new`/`gc_collect` (ungated → `perms=0` reachable) |
| **F-3** | **MEDIUM** | **NEW** | Uncaught `bad_alloc`/`length_error` across native→JIT boundary → `terminate`: `map_set` insertion (`ext_map.cpp:51-53`, ungated, entries unbounded), thread slot alloc (`ext_thread.cpp:214`), coroutine slot alloc (`ext_coroutine.cpp:305`), `make_executable` vector ctor (`ext_call_raw.cpp:88`), IO `read_line` growth (`ext_io.cpp:92-95`), IO `path_basename`/`path_dirname` substr (`ext_io.cpp:196-204`/`:47,57`) | as cited | Memory pressure during the named allocation |
| **F-4** | **MEDIUM** | **NEW (characterization)** | Generationless free-list/address reuse → stale-handle ABA + reset-revives-stale-ID across all opaque-handle extensions; plain-i64 handles permit cross-type numeric aliasing | `ext_lifecycle.cpp:40-42,47-56`; `ext_sync.cpp:109-115,197-205`; `ext_thread.cpp:207-211`; `ext_coroutine.cpp:298-302`; `ext_array.cpp:204`/`ext_map.cpp:95`/`ext_string.cpp:253` (reset+realloc); `ext_gc` (malloc address reuse) | Hold a handle across free+realloc or `reset()`, or mix handle types |
| **F-5** | **MEDIUM** | **NEW (broader than prior call_raw TOCTOU)** | `ext_string::slot` (`:197-199`) and `ext_array::get_bytes` (`:141-149`) return raw pointers after releasing their store mutexes → concurrent-realloc/reset UAF in every IO/call_raw string/array consumer | `ext_string.cpp:197-199`, `ext_array.cpp:141-149`, `ext_io.cpp:71,129,154,158,173,186,195,204`, `ext_call_raw.cpp:91` | Two contexts: A holds the returned ptr; B does `string_new`/`array_push`/`resize`/`reset` |
| **F-6** | **LOW-MED** | **NEW** | `ext_map` entries unbounded (`MAX_MAPS` caps objects not entries; `map_set` has no entry/byte cap) → DoS via OOM (→ F-3 terminate on rehash) | `ext_map.cpp:30,40,49-51` | `perms=0` module calling `map_set` in a loop |
| **F-7** | **LOW** | **NEW** | `vst_dsp_harness` `delay_buffer`/`delay_size` mis-gated (`permission=0`); `delay_buffer` returns a raw host `float*` | `vst_dsp_harness.cpp:90-93,158-160` | A host copying the pattern without `PERM_FFI` leaks a raw ptr to non-FFI scripts |
| **F-8** | **INFO** | **NEW (policy)** | thread/coroutine/gc natives ungated (`permission=0`); a `perms=0` module can spawn threads/fibers, allocate GC heap, run the collector | `ext_thread.cpp:327-344`, `ext_coroutine.cpp:394-414`, `ext_gc.cpp:200-218` | Any non-`--ffi` CLI run (bundle/stub/vst3-processor do not register these) |
| **(accepted)** | — | accepted, unchanged | S1/S2/S4/S5/S6/S7, N1/N2/N3, C1/C2, OPT C1–C10, audio-raw-F2, vst3-save_state-F3, release-script-F4, BlockMerge-F5, rt-test-F6 | see §5 | documented |
| **(prior fixes)** | — | verified intact | v5 `frame_off`, `ext_array` bounds, vec/mat/quat/string alloc containment, dup block IDs, `PERM_FFI` 3 gates, raw-x86 default-reject, `catch_bufs` S3, `bounds-elim` soundness | see §6 | — |

**Regression conclusion:** one HIGH regression (F-1, `587f9d4`) stands at HEAD. All prior confirmed critical/high/medium fixes are intact and were not re-introduced. The new findings (F-2 through F-8) are pre-existing conditions in the current tree (not introduced by the recent 20-commit window, except where noted), now characterized correctly; the prior draft's invalid GC-overflow HIGH (former F-1) is removed.

---

*End of sweep. READ-ONLY; no tracked source files modified, no commits. Evidence cited to current file:line at HEAD `f256ff9`.*
