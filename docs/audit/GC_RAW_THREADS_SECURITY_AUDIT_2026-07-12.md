# Security Audit — ember GC / Raw Execution / Threads feature attack surfaces

**Date:** 2026-07-12
**Repository:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**HEAD:** `f256ff9` ("Release script: ignore vst3sdk submodule dirty state in milestone check")
**Auditor scope:** READ-ONLY deep audit. No tracked source files edited. No commits. No probes added to tracked source. Targeted repros were built and run from `tmp_edit/` (gitignored) linking the pre-built static libs in `build/`; pre-built test binaries in `build/` (MinGW g++ 15.2.0) were executed read-only.
**Scope (per task):** `src/gc.{hpp,cpp}`, `extensions/gc/`, by-reference capture in `src/parser.cpp` / `src/sema.cpp` / `src/codegen.cpp` / `src/thin_lower.cpp`, registration/use in `examples/ember_cli.cpp`; `extensions/call_raw/`, `src/jit_memory.{hpp,cpp}`, `ext_array` byte-handle access, CLI registration; `extensions/thread/`, `src/context.hpp`, `src/engine.{hpp,cpp}`, `src/dispatch_table.hpp`, CLI init/reset. Existing `gc_full`/`gc_integration`/`call_raw`/`in_context_threads`/`thread_safety` tests inspected and run.

---

## 0. Read-only provenance / git state (actual initial `git status`)

The exact `git status` captured at the start of this audit (before any tool calls beyond `git status` / `git log` / `ls`):

```
On branch master. Your branch is up to date with 'origin/master'.
Changes not staged for commit:
        modified:   thirdparty/vst3sdk (modified content)   # submodule; pre-existing, not touched
Untracked files:
        docs/audit/SANDBOX_REVALIDATION_2026-07-12.md       # 14:26:53 -0400, pre-existing, NOT created by this audit
        docs/audit/SECURITY_AUDIT_20COMMITS_2026-07-12.md   # 14:25:50 -0400, pre-existing, NOT created by this audit
no changes added to commit.
```

`git status --porcelain`:
```
 M thirdparty/vst3sdk
?? docs/audit/SANDBOX_REVALIDATION_2026-07-12.md
?? docs/audit/SECURITY_AUDIT_20COMMITS_2026-07-12.md
```

`git submodule status`: `9fad977... thirdparty/vst3sdk (v3.7.3_build_20-15-g9fad977)` (dirty content, pre-existing).

**Two** pre-existing untracked audit documents were already present in `docs/audit/` before this audit began (both dated 2026-07-12, timestamps 14:25:50 and 14:26:53). They are separate, independent audits by another auditor and are **not** products of this audit:

- `docs/audit/SANDBOX_REVALIDATION_2026-07-12.md` (37 KB, 14:26:53) — sandbox-guarantee re-validation (ThinIR guard stripping N1–N3/C1–C2, prior-findings re-check).
- `docs/audit/SECURITY_AUDIT_20COMMITS_2026-07-12.md` (33 KB, 14:25:50) — last-20-commits audit (VST3 hot-reload UAF F1, raw audio buffers F2, etc.).

This audit is an **independent** deliverable covering the GC / raw-execution / threads feature surfaces specifically. It does not assert ownership of the two pre-existing documents and makes no timeline claim about when audit tool calls began relative to their timestamps. During this session a further independent audit doc (`docs/audit/ATTACK_SURFACE_SWEEP_2026-07-12.md`, timestamp 14:38:49) appeared, also not created by this audit. The only file this audit created is this report (a new untracked file in `docs/audit/`, the canonical audit-deliverable location, matching the pre-existing untracked audit docs) plus the gitignored repro sources/binaries in `tmp_edit/`. **No tracked file was edited.** `git diff` after this audit shows only `thirdparty/vst3sdk | 0` (the pre-existing dirty submodule pointer, 0 insertions/0 deletions, untouched); `git status --porcelain` shows the dirty submodule plus untracked docs/audit files (the two pre-existing + ATTACK_SURFACE_SWEEP + this report).

---

## 1. Test results (pre-built `build/` binaries, run read-only; no rebuilds)

| Suite | Result | Notes |
|---|---|---|
| `gc_test` (gc_core) | **PASS** | 28 sub-checks; mark-sweep, root scanning, ref-graph, cycles, clear. |
| `gc_integration_test` (gc_integration) | **PASS** | Parts A (host API) / B (codegen `use_gc_env`) / C (script `gc_new`/`gc_delete`); 2000-lambda auto-collect mid-run; by-ref read/write/nested. |
| `gc_full_test` (gc_full) | **PASS** | (a) by-ref capture, (b) new/delete unreachable reaped, (c) reachable survive. |
| `call_raw_test` (call_raw) | **PASS** | incl. "[3] sema rejects call_raw without PERM_FFI". |
| `in_context_threads_test` (in_context_threads) | **PASS** | 8 scenarios incl. trap-while-worker, nested spawn, 20-worker stress, guard exercise. |
| `thread_safety_test` (thread_safety) | **PASS** | B1 per-thread context, budget trap, shared-body two-thread. |
| `ctest -R gc_core|gc_integration|gc_full|call_raw|thread_safety|in_context_threads` | **6/6 PASS** | 0 failures. |
| lang suite (`tests/run_lang_tests.sh build`) | **463 passed / 0 failed / 0 skipped** | No regressions. |

**No regressions in the audited feature suites.** All five feature test executables and the lang suite pass with the current `build/` binaries. (The two pre-existing audit docs report a handful of infra-only ctest "failures" outside this scope — VST3 harness exes absent from `build/`, bundler/self_hosted_preview ctest path registration — neither of which is in this audit's feature surface and both of which PASS when run directly with correct args per those docs.)

---

## 2. Verified-in-place prior fixes (confirmed present in current source)

These were checked by reading the current source (not assumed from commit messages):

- **23b952a** `call_raw` null guard — `extensions/call_raw/ext_call_raw.cpp`: `if (fn_ptr == 0) return INT64_MIN;` present. Codegen frame-size int32 overflow check — `src/codegen.cpp:5100-5106` (`int64_t total_check = ...; if (total_check > INT32_MAX) return CompiledFn{};`). Lexer 1 MB string cap — `src/lexer.cpp:303-307` (`MAX_STRING_LITERAL = 1<<20`). **All present.**
- **05ad5f8** GC `header_bytes` trailer at `user-4` — `src/gc.cpp` `alloc()` stores `hdr_total` at `user-4`; `collect()` reads it back. **Present.** (The dead `obj - 16` placeholder on the first root line is immediately overwritten by the `user-4` read — harmless cruft, not a bug.)
- **456b80b** in-context threads deadlock-free join — `g_setup_mutex` released before `done_cv.wait`; `ThreadSlot*` stable for the worker's lifetime while `in_use`. **Present.**
- **W^X** enforced everywhere: `alloc_executable_rw` (RW) → patch → `seal_executable` (RX); no RWX pages (`src/jit_memory.cpp`, `src/platform.cpp` `protect_rx` = `PAGE_EXECUTE_READ` / `PROT_READ|PROT_EXEC`). **Present.**
- **REDSHELL fn-handle range/provenance guard** in `thread_spawn` — `resolve_entry` rejects `handle<0` (cross-module bit-63), `handle>=slot_count`, and null-published dispatch slots (`extensions/thread/ext_thread.cpp`). The *range/provenance* half is sound. **(The signature-validation half is NOT sound — see H3.)**
- **PERM_FFI gating** for `call_raw`/`make_executable`/`free_executable_ptr` — enforced at sema call sites; verified: `tmp_edit/audit_raw_no_ffi.ember` → sema exit 2 with "requires PERM_FFI permission" for all three. **Correct.**
- **ext_array element bounds** — division-based `size_t(i) < bytes.size()/elem_size` checks under `g_store_mutex`; `checked_bytes` + `MAX_CONTAINER_BYTES` cap in `alloc_bytes`. **Present.** (But see H4 for the `get_bytes` unlock-before-return hole that bypasses this protection for the `make_executable` consumer.)
- GC pin-slot lifetime consistency — `remove_root` before `delete` in `unpin_env` and `gc_reset`; `gc_delete` on an already-unrooted handle returns 0 (safe); double `gc_reset` is a no-op on an empty heap. **Sound.**

---

## 3. Ranked findings

### H1 — ember_cli does not lock `ctx->call_mutex` around the outer `ember_call` → reproducible segfault when a script uses `thread_spawn` and traps while a worker runs
**Severity: HIGH. Evidence: 5/5 segfaults (exit 139).** Repro: `tmp_edit/audit_thread_race_trap.ember` via `build/ember_cli.exe run` (5/5 crashed).
**Root cause:** `examples/ember_cli.cpp` run path calls `ember::ember_call_void(entry, &ectx)` **without** locking `ectx.call_mutex`. `ext_thread::n_thread_join` does `g_ctx->call_mutex.unlock()` (unlock-on-unlocked = UB; on this MinGW build it returns, masking it) and the worker's `thread_worker` does `__builtin_setjmp(ctx->checkpoint)`, **overwriting the main thread's checkpoint**. `restore_state` only puts the caller's checkpoint back if the worker runs to completion; if the **main** thread traps while the worker is still mid-run, the trap stub `longjmp`s to `ctx->checkpoint`, which holds the **worker's** context → cross-thread longjmp into a dead/wrong stack → segfault. The `in_context_threads_test` harness (`run_main_locked`, lines 163-175) **does** lock `call_mutex` around the outer call, which is why that suite passes and the CLI does not.
**Impact:** a script (no `--ffi` needed — threads are ungated, see M1) can crash the host process.
**Fix:** lock `ectx.call_mutex` around the outer `ember_call` in `ember_cli`'s run/bench/tick paths, mirroring `run_main_locked`, with unlock in both the normal and trap-longjmp return paths.

### H2 — By-reference capture can escape their creating frame with NO sema guard → 100% reproducible UAF/segfault
**Severity: HIGH. Evidence: 6/6 segfaults (both stack-env and `--gc-env`).** Repro: `tmp_edit/audit_byref_escape.ember` via `ember_cli.exe run` and `ember_cli.exe run --gc-env` (both crash, exit 139).
**Root cause:** a `fn[&x]` lambda's env slot holds a **pointer to the creating frame's stack slot** (codegen: `lea rax, [rbp + cit->second]`, `src/codegen.cpp:2003-2011`; read = double-deref `load rax,[rax+off]; load rax,[rax]` at 2180-2196; write = store-through at 2792-2810). Sema has a slice-escape analysis but **no** analogous by-ref-capture-escape analysis (`src/sema.cpp:3718-3762` builds the by-ref flag with no escape check). A by-ref-capturing lambda can be `return`ed, stored in a global, or handed to a thread; once the frame dies the env's pointer dangles. **The `--gc-env` path does NOT fix this:** the GC heap env survives, but the by-ref pointer inside it still targets the dead stack slot. `ext_gc.hpp` even claims `--gc-env` exists "so lambdas can outlive their creating frame" — this is **false for by-ref captures** (true only for by-value captures).
**Impact:** unguarded UAF; script crash or (with a crafted follow-up) potential stack read/write primitive. No permission gate (by-ref is a language feature, ungated).
**Fix:** sema should reject by-ref captures that escape (return / global-store / thread-arg / coroutine-hand-off), mirroring the slice-escape borrow analysis. At minimum reject `return <by-ref-capturing lambda>` and global stores of one. Document that `--gc-env` only makes by-VALUE captures escape-safe.

### H3 — `thread_spawn` does NOT validate the worker signature; handles for void / floating-point / wrong-arity / ABI-incompatible fns pass sema and are called as `i64(i64)` → ABI mismatch
**Severity: HIGH. Evidence: targeted signature-mismatch repro, 4/4 ABI-incompatible workers accepted.** Repro: `tmp_edit/sig_mismatch_repro.cpp` → `tmp_edit/sig_mismatch_repro.exe`.

```
[A void-worker]     sema ACCEPTED, join returned: 2148384863744   (()->void; rax was uninitialized leftover)
[B float-worker]    sema ACCEPTED, join returned: 1              ((f32)->i64; arg passed in rcx as i64, worker wanted f32 in xmm0)
[C two-arg-worker]  sema ACCEPTED, join returned: 792777961271   ((i64,i64)->i64; 'b' in rdx never set by thread_spawn -> garbage a+b)
[D no-arg-worker]   sema ACCEPTED, join returned: 42             (()->i64; arg ignored, harmless here but arity unchecked)
[E control i64(i64)]sema ACCEPTED, join returned: 42             (correct: 21*2)
```

**Root cause:** `ext_thread::register_natives` registers the `thread_spawn` first param as a **bare** `fn` — `fn_param = type_i64(); fn_param.is_fn_handle = true;` with `has_recorded_sig` **never set** (`extensions/thread/ext_thread.cpp` register_natives, comment "no recorded sig"). `thread_worker` then **unconditionally** invokes the entry as `ember_call_i64(entry, ctx, arg)` = `int64_t(*)(int64_t)` (`i64(i64)`). Sema's `types_compatible` (`src/sema.cpp:518-535`) matches a fn-handle `want` against a fn-handle/lambda `got`: if **both** carry `has_recorded_sig` it compares params/ret; but if **"one or both bare (no recorded sig): compatible (bare fn accepts any)"** — so `&void_worker` (recorded sig `()->void`), `&float_worker` (`(f32)->i64`), `&two_arg_worker` (`(i64,i64)->i64`), `&no_arg_worker` (`()->i64`) are all `types_compatible` with the bare `thread_spawn` param. The range/provenance guard (H-REDSHELL) is sound, but **no signature is ever recorded or checked**. The existing tests use only `fn worker(n: i64) -> i64` (exactly `i64(i64)`), so this is untested.
**Impact:** a script can spawn a worker whose ABI differs from `i64(i64)`; the worker reads garbage args (rcx/rdx/xmm0 mismatch) and returns whatever is in `rax` — wrong results, and for a float/struct-returning worker the SSE/stack-state mismatch can corrupt the caller. No permission gate (threads ungated, M1).
**Fix:** register `thread_spawn`'s param with `has_recorded_sig=true`, `recorded_params={i64}`, `recorded_ret={i64}` so sema enforces `fn(i64)->i64`; OR record the worker's sig in the dispatch-slot metadata and validate it at `thread_spawn` time before resolving the entry. Add the A–D signature-mismatch cases to `in_context_threads_test`.

### H4 — `ext_array::get_bytes` returns a backing-vector pointer after releasing `g_store_mutex`; `make_executable` then copies from it unlocked → race/UAF (segfault)
**Severity: HIGH. Evidence: 5/5 segfaults (exit 139).** Repro: `tmp_edit/get_bytes_race_repro.cpp` → `tmp_edit/get_bytes_race.exe` (5/5 crashed; TSan unavailable on MinGW so demonstrated by crash).

**Root cause:** `ext_array::get_bytes(handle, &data, &len)` takes `g_store_mutex`, reads `*out_data = s->bytes.data()` (a pointer **into the backing `std::vector`**), `*out_len = s->bytes.size()`, and **releases the mutex** (`extensions/array/ext_array.cpp:141-146`). `ext_call_raw::n_make_executable` then does `std::vector<uint8_t> code(data, data + size_t(len))` — copying `[data, data+len)` **without the lock** (`extensions/call_raw/ext_call_raw.cpp`). A concurrent `array_resize` / `array_push_*` / `ext_array::reset()` on the same handle (or any handle, for `reset()`) from another context/thread reallocates or frees the backing vector, invalidating `data` → the unlocked copy reads freed/realloc'd memory → UAF. The repro drives `array_resize` across growth-forcing sizes plus periodic `reset()` (which `g_arrays.clear()` frees every backing vector) from thread T1 while T2 mirrors `n_make_executable`'s exact two unlocked steps. **The prior audit's claim that this path is "mutex-protected" is incomplete: the bounds check is under the lock, but the pointer is used after the lock is released.**
**Impact:** a script that calls `make_executable` on an `array<u8>` while another context resizes/resets it can crash or copy garbage into an executable page (then `call_raw` it). Requires `PERM_FFI` (make_executable is gated), so within the FFI trust boundary, but still a memory-safety hole.
**Fix:** add a `get_bytes_copy(handle, std::vector<uint8_t>& out)` API that copies under `g_store_mutex` and returns the copy; or have `n_make_executable` hold a lock through the copy (expose a `copy_bytes_locked` helper). Do not hand out a pointer into the backing vector to be used after unlock.

### H5 — `thread_reset` frees `ThreadSlot`s (and CLI frees executable pages) while detached workers still run through them → UAF (segfault)
**Severity: HIGH. Evidence: 3/3 segfaults (exit 139).** Repro: `tmp_edit/thread_reset_uaf_repro.cpp` → `tmp_edit/thread_reset_uaf.exe` (3/3 crashed). This corrects and **upgrades** the prior audit's L3 from "low context-lifetime concern" to HIGH.

**Root cause (two compounding UAFs):**
1. **`thread_reset` frees slots out from under detached workers.** `ext_thread::thread_reset()` detaches any `in_use && joinable` thread, then **immediately** `g_threads.clear()` (`extensions/thread/ext_thread.cpp` thread_reset). `g_threads` owns the `ThreadSlot`s via `unique_ptr`, so `clear()` destroys every slot. But each detached worker holds a **raw `ThreadSlot*`** (passed to `thread_worker(entry, ctx, arg, raw)`) and, after `reset` returns, publishes its result **through that slot**: `slot->result = ...; slot->done = true; slot->done_cv.notify_all();` and `ctx->call_mutex.unlock()` (`thread_worker`). The slot is freed → the worker writes through a dangling pointer → UAF. The `raw_slot`/join comments claim "in_use slots are never erased" — true for `join`, but **`reset` erases them all regardless of `in_use`**.
2. **CLI cleanup frees executable pages BEFORE `thread_reset`.** `ember_cli`'s `do_cleanup` (`examples/ember_cli.cpp:331-341`) runs `for (auto& fn : fns) if (fn.exec) free_executable(fn.exec);` **first**, then the extension resets including `ext_thread::thread_reset()`. A detached worker still mid-`ember_call` is **executing code in one of those just-freed pages** → execute-freed-code UAF. (The `in_context_threads_test` `cleanup` has the **same order** — free pages, then `thread_reset` — but that harness joins every thread before cleanup, so it never hits this; `ember_cli` does not guarantee quiescence.)

**Impact:** a script that `thread_spawn`s and does not `join` (or one that traps/returns before joining) leaves a detached worker; `do_cleanup` at run end frees its code and slot out from under it → crash or memory corruption. `ember test` calls `run_ember_file` N times in one process, so a late worker from file N can corrupt file N+1. The `thread_reset` comment acknowledges the host "must keep alive past reset OR call reset only when quiescent" — but `ember_cli` does **not** guarantee quiescence.
**Fix:** `thread_reset` must **join (or quiesce) every in_use worker before freeing its slot** — not detach-then-clear. `do_cleanup` must call `thread_reset` (which joins) **before** freeing executable pages, and must not free the context while any worker references it. Reset must join/quiesce before freeing slots, code, context, or extension state.

### M1 — GC + thread natives are NOT permission-gated; engine-internal `__ember_gc_*` natives are callable by name from script
**Severity: MEDIUM. Evidence:** `gc_new`/`gc_delete`/`gc_collect`/`gc_live` run without `--ffi` (exit 1 = ran, returned `n`); `thread_spawn`/`thread_join` run without `--ffi` (exit 45 = worker summed 0..9); `__ember_gc_alloc_env`/`__ember_gc_live` (engine-internal) run without `--ffi` (exit 1 = returned n=1). Repros: `tmp_edit/audit_gc_no_ffi.ember`, `audit_thread_no_ffi.ember`, `audit_internal_gc.ember`.
**Root cause:** `ext_gc::register_natives` and `ext_thread::register_natives` register all natives with `permission=0` (default). Only `ext_call_raw` uses `PERM_FFI`. A module compiled with NO `--ffi` can still allocate raw GC heap handles, unroot them, spawn OS threads, and call the internal `__ember_gc_*` natives by name.
**Impact:** threads can crash the host (H1/H5); `gc_new`/`gc_delete` give raw-heap-handle access (UAF if misused, no type safety). For a sandbox that uses `--ffi` as the trust boundary, these bypass it. Ungated thread creation also undermines sandbox resource guarantees (M2).
**Fix:** gate `thread_spawn` (at least) behind `PERM_FFI` or a new `PERM_THREAD`/`PERM_GC`; gate the user-facing `gc_*` behind the same; hide `__ember_gc_*` from script call sites (mangle the name, or reject in sema when the caller is script, not codegen).

### M2 — Thread budget/depth bypass + weak resource ceiling; ungated thread creation undermines sandbox resource guarantees
**Severity: MEDIUM.** `n_thread_spawn` consumes no `ctx->budget_remaining` and does not increment `call_depth`; `thread_worker` does `save_state` (snapshots `budget_remaining`), `reset_for_call` (which does **not** reset budget), runs `ember_call_i64`, then `restore_state` (puts the caller's budget back). So each spawned worker **inherits the caller's full budget**, and `restore_state` erases the worker's depth increments — a budget-limited script can spawn N workers each getting a full budget → total execution far exceeds the per-call budget. The slot ceiling is `1<<20` (~1.05M) threads (`ext_thread.cpp:213`) with **no link to ctx budgets**; each Windows thread reserves ~1 MiB virtual stack → ~1 TiB virtual reservation near the ceiling (OOM/OOM-DoS). No per-spawn cost.
**Impact:** a sandbox that sets a budget to cap a script cannot cap thread-amplified execution.
**Fix:** charge `thread_spawn` against `ctx->budget_remaining` (or a separate thread-budget ceiling); cap concurrent live threads to a small sandbox default (e.g. 64 or 256) independent of the `1<<20` slot ceiling; do NOT restore the worker's budget from the caller's snapshot — give the worker a fresh sub-budget.

### M3 — IR backend (`--passes`) silently miscompiles capturing lambdas (by-value AND by-ref), returning 0 with no diagnostic
**Severity: MEDIUM. Evidence:** `tmp_edit/audit_gc_passes.ember` (by-ref, expect 99) under `--passes constprop` → exit 0; `tmp_edit/audit_bv_lambda.ember` (by-value, expect 42) under `--passes constprop` → exit 0; baseline (no `--passes`) → exit 99 (correct).
**Root cause:** `src/thin_lower.cpp` / `thin_ir.*` have **no** lambda/capture support (grep for `use_gc_env`/`__ember_gc`/`env_ptr`/`by_ref`/`capture` in `thin_lower.cpp` returns nothing). The tree backend (`src/codegen.cpp`) handles captures; `--passes` routes through `thin_lower`. There is no guard rejecting capturing lambdas in the IR backend and no mutual-exclusion between `--gc-env` and `--passes` (`ember_cli` just sets `ctx.use_gc_env = opts.gc_env` AND `ctx.enable_ir_backend = true` when both flags are given).
**Impact:** silent wrong results; a user who turns on `--passes` (optimization) on a capturing-lambda program gets incorrect output with no error.
**Fix:** in the IR backend, reject capturing lambdas (sema or `compile_func` error) instead of emitting wrong code; OR mark `--gc-env`/`--captures` incompatible with `--passes` at the CLI with a clear message.

### M4 — GC alloc exception safety + pin-failure UAF
**Severity: MEDIUM.** In `src/gc.cpp` `alloc()`, `std::malloc` is checked (nullptr → return nullptr) but `m_live.insert(user)` can **throw** (`unordered_set` rehash) AFTER malloc+memset → the raw pointer is leaked (not in `m_live`, not freed) AND the exception propagates out of `alloc`, through `gc_alloc_env`, through the `extern "C"` native `n_gc_alloc_env` (**not `noexcept`**) into JIT'd code → `std::terminate` (ember's trap model is longjmp, not C++ exceptions; JIT'd code cannot catch a C++ exception). Separately, in `ext_gc.cpp` `gc_alloc_env`, `pin_env(env)`'s return value is **ignored** (line 113); if the pin slot `new (std::nothrow) void*` fails (or `pinned[env]` insertion throws), the env is allocated but **not pinned**, then the auto-collect threshold check fires and may sweep the env the caller just received → UAF.
**Impact:** OOM during GC alloc/pin can terminate the process or hand the caller an unpinned, collectable handle.
**Fix:** mark the GC natives `noexcept` and catch `bad_alloc`/`length_error` inside (return 0 on OOM, like `ext_array` does); free the raw block if `m_live.insert` throws; check `pin_env`'s return and free+fail the alloc if pinning fails.

### M5 — GC thread-local heap vs cross-thread handles; `gc_reset` does not clear worker heaps
**Severity: MEDIUM.** `ext_gc`'s `GcRuntime` is `thread_local` (`g_rt`, `ext_gc.cpp:38`). A spawned worker that calls `gc_new`/`__ember_gc_alloc_env` lazily creates its **own** `GcRuntime` (via `rt()`), which `gc_reset` on the main thread **never clears** → leak. Worse: GC handles are plain `i64` with no thread tag; a handle allocated on thread A used on thread B sees a different heap → `is_live()` returns false → the pointer is treated as non-GC → reads/writes through it are against a foreign heap's freed memory → UAF. `ext_array` handles share across threads safely because `ext_array`'s store is process-global (mutex-protected); `ext_gc`'s per-thread heaps break that invariant.
**Fix:** either make the GC heap process-global with a mutex (the `gc.hpp` "follow-up"), or tag GC handles with the owning thread and reject cross-thread use; ensure `gc_reset` clears ALL thread heaps (track them in a registry).

### L1 — GC allocation-size / RefMap defense-in-depth gaps in `src/gc.cpp` `alloc()`
**Severity: LOW (currently unreachable; defense-in-depth).**
- `total = hdr_total + size` can in principle wrap if `size` is near `SIZE_MAX`; not currently reachable from script (`gc_alloc_env` rejects `size<=0` and `malloc` fails on huge sizes) but no explicit wrap check.
- `hdr->size = uint32_t(size)` **truncates** `size > 4 GiB` → `live_bytes`/stats drift and the trace bound `off + 8 > hdr->size` uses the truncated value.
- ref offsets are NOT validated (8-alignment, `< size`); the trace check `off + 8 > hdr->size` can itself **wrap** (`uint32_t off + 8`) when `off` is near `UINT32_MAX`, bypassing the guard and reading OOB. Currently unreachable (`ext_gc` always uses `refmap_none`, so `ref_count=0` and the trace loop body never runs) but a latent trap for any future RefMap caller.
**Fix:** check `total` wrap; reject `size > 4 GiB` (or > a sane cap); validate ref offsets (8-aligned, `off+8 <= size` with 64-bit arithmetic) in `alloc`.

### L2 — `call_raw` / `free_executable_ptr` provenance + Linux `free_executable` size loss
**Severity: LOW (documented raw-capability posture; PERM_FFI-gated).** `call_raw` only guards `fn_ptr==0` (returns `INT64_MIN`, the 23b952a fix); any non-null `i64` is reinterpreted and called → branch to arbitrary host code. `free_executable_ptr` on a non-page or double-freed ptr is UB (Windows `VirtualFree` is tolerant; Linux `munmap` needs the size which `free_executable` does **NOT** pass — `free_page(ptr, 0)` → `munmap(ptr, 0)` is `EINVAL`/no-op → leak on Linux). `PERM_FFI` gating is correct and enforced (verified).
**Fix (optional, defense-in-depth):** track live executable allocations in a (`PERM_FFI`-gated) set; validate `call_raw` targets against it; pass the real size to `munmap` on Linux (track alloc sizes).

### L3 — `gc_delete`-then-use UAF is inherent to the raw-handle design
**Severity: LOW (documented "raw capability, not policy").** `gc_delete(p)` unroots `p`; a subsequent auto-collect (threshold) frees it; the script still holds `p` and any read/write through it is UAF. Easy to hit because auto-collect fires inside `gc_alloc_env`. Not a bug per se; a sandbox note.
**Optional fix:** zero the handle in the caller's slot on delete (needs a handle→slot map) or document loudly.

---

## 4. Recommended hardening order

1. **H1** — lock `ctx->call_mutex` around the outer `ember_call` in `ember_cli` (run/bench/tick). Stops the thread+trap crash; small, localized.
2. **H5** — make `thread_reset` **join/quiesce** in_use workers before freeing slots; reorder `do_cleanup` so `thread_reset` runs **before** freeing executable pages and the context. Stops the detach-into-freed-slot/code UAF.
3. **H3** — record/validate the `thread_spawn` worker signature as `fn(i64)->i64` (sema recorded-sig param or runtime metadata). Add the A–D signature-mismatch tests.
4. **H4** — add a copy-under-lock `get_bytes` API (or hold the lock through the copy in `make_executable`). Stops the unlocked-copy race/UAF.
5. **H2** — sema by-ref-capture escape analysis (reject `return`/global-store/thread-arg of a by-ref-capturing lambda). Stops the dangling-stack-pointer UAF.
6. **M1** — permission-gate `thread_spawn` (and user-facing `gc_*`); hide `__ember_gc_*` from script. Closes the sandbox bypass.
7. **M2** — thread budget charge + small concurrent-thread ceiling; fresh sub-budget per worker. Restores sandbox resource guarantees.
8. **M3** — IR-backend capturing-lambda rejection (or `--gc-env`/`--captures` ⊥ `--passes` at the CLI). Stops silent miscompilation.
9. **M4 / M5 / L1 / L2 / L3** — exception safety + pin-failure handling; cross-thread heap; GC alloc/RefMap defense-in-depth; Linux `free_executable` size; `gc_delete`-then-use documentation.

---

## 5. Files inspected (read-only; none edited)

- `src/gc.hpp`, `src/gc.cpp`; `extensions/gc/ext_gc.hpp`, `extensions/gc/ext_gc.cpp`
- `extensions/call_raw/ext_call_raw.hpp`, `extensions/call_raw/ext_call_raw.cpp`; `src/jit_memory.hpp`, `src/jit_memory.cpp`; `src/platform.hpp`, `src/platform.cpp`; `extensions/array/ext_array.hpp`, `extensions/array/ext_array.cpp` (`get_bytes`/`alloc_bytes`/`reset`)
- `extensions/thread/ext_thread.hpp`, `extensions/thread/ext_thread.cpp`; `src/context.hpp`; `src/engine.hpp`, `src/engine.cpp`; `src/dispatch_table.hpp`
- by-ref capture: `src/parser.cpp` (893-950), `src/sema.cpp` (324-330, 3350-3356, 3718-3847), `src/codegen.cpp` (134-142, 1885-2116, 2175-2210, 2775-2830, 4975-4985), `src/thin_lower.cpp` (no capture support — M3), `src/codegen.hpp` (215-230 `use_gc_env`), `src/ast.hpp` (402-412)
- `src/binding_builder.hpp` (`PERM_FFI`, `NativeSig`); `src/sema.cpp` `types_compatible` (505-535) + fn-handle typing (1615-1700)
- CLI wiring: `examples/ember_cli.cpp` (registration 144-154, cleanup 331-341, context/thread/gc init 520-551, run path ~700-712, flags 1477-1511)
- Tests: `examples/gc_test.cpp`, `gc_integration_test.cpp`, `gc_full_test.cpp`, `call_raw_test.cpp`, `in_context_threads_test.cpp`, `thread_safety_test.cpp`; `tests/lang/valid_gc_by_ref.ember`, `valid_gc_by_ref_write.ember`, `valid_threads.ember`

## 6. Repro artifacts (gitignored, in `tmp_edit/`; not tracked, not committed)

- `tmp_edit/sig_mismatch_repro.cpp` / `.exe` — H3 signature-mismatch evidence (4/4 ABI-incompatible workers accepted).
- `tmp_edit/get_bytes_race_repro.cpp` / `.exe` — H4 unlocked-copy race evidence (5/5 segfaults).
- `tmp_edit/thread_reset_uaf_repro.cpp` / `.exe` — H5 reset-frees-slots/code UAF evidence (3/3 segfaults).
- `tmp_edit/audit_thread_race_trap.ember` — H1 CLI thread+trap crash (5/5 segfaults via `ember_cli.exe run`).
- `tmp_edit/audit_byref_escape.ember` — H2 by-ref escape UAF (6/6 segfaults, stack-env + `--gc-env`).
- `tmp_edit/audit_gc_no_ffi.ember`, `audit_thread_no_ffi.ember`, `audit_internal_gc.ember`, `audit_raw_no_ffi.ember` — M1 permission-gating evidence.
- `tmp_edit/audit_gc_passes.ember`, `audit_bv_lambda.ember` — M3 IR-backend silent miscompilation.

---

**Summary:** Security posture reviewed; all existing fixes verified present; the GC/raw/threads feature commits (`0647aad`→`99d8379`) checked; no regressions (all feature suites + lang suite pass). Five HIGH findings (H1–H5), five MEDIUM (M1–M5), three LOW (L1–L3). The two pre-existing 2026-07-12 audit docs in `docs/audit/` are independent and not products of this audit. No tracked file was edited or committed.
