# ember Security Audit — Latest 20 Commits (read-only)

**Date:** 2026-07-12
**Repository:** `E:/DEVELOPER/PROJECTS/sus/hyper_workspace/ember`
**HEAD:** `f256ff9` (Release script: ignore vst3sdk submodule dirty state in milestone check)
**Window:** 20 commits — `f256ff9` (newest) … `0a76c59` (oldest)
**Prior audit baseline:** `acf01d0` ("ROADMAP: add platform support section") — ancestor of HEAD; 53 commits between baseline and HEAD. The entire VST3 wrapper is new relative to this baseline (it first appears at `2eae6d4`).
**Auditor scope:** read-only deep audit. No tracked files edited. `thirdparty/vst3sdk` not entered or modified.
**Reference prior audits:** `docs/audit/SECURITY_AUDIT_2026-07-11.md` (HEAD `bf76217`, 75 commits before baseline), `docs/audit/OPTIMIZATION_PASSES_READ_ONLY_AUDIT_2026-07-12.md` (added in commit `69404ad`, inside this window).

---

## 1. Executive summary

The 20-commit window is dominated by the VST3 plugin wrapper (Phases 2–8), the IR optimization/obfuscation pass surface, and release-tooling changes. One **HIGH** use-after-free regression was found in the hot-reload reclamation path introduced in Phase 7 (`587f9d4`); the raw audio-buffer natives remain an intentional, `PERM_FFI`-gated arbitrary memory surface (documented, not a new flaw); and several **pre-existing, still-open** optimization-pass correctness defects (documented in the in-window OPT audit) persist unchanged through this window. All previously confirmed critical/high findings from the 2026-07-11 security audit are verified fixed at HEAD, and none were re-introduced by this window.

| # | Severity | Finding | Commit | Status |
|---|----------|---------|--------|--------|
| F1 | **HIGH** | Hot-reload use-after-free: `audio_readers_` grace-period TOCTOU lets the watcher free a module the audio thread is crossfading on | `587f9d4` | **NEW, open** |
| F2 | **MEDIUM** | Raw `load_f32/store_f32/load_f64/store_f64/load_i32/store_i32` are unchecked arbitrary host-memory read/write primitives (intentional, `PERM_FFI`-gated) | `085eaaf` | Intentional/by-design; trust boundary = `PERM_FFI` |
| F3 | **MEDIUM** | `save_state()`/`load_state()` script return values reinterpreted as raw pointers with no validation | `01f38d9` | Open (defense-in-depth); within script-author trust boundary |
| F4 | **LOW** | Release milestone gate uses imprecise `grep -v 'thirdparty/vst3sdk'` substring filter; `VERSION` argument unsanitized | `f256ff9`, `4445fe3` | Open (developer-run script trust model) |
| F5 | **LOW** | `BlockMergePass` example lacks the loop-member safety guard that the real `SimplifyCFGPass` has (documentation/example only, opt-in) | `358b590` | Open (example code) |
| F6 | **INFO** | Realtime-contract test can count watcher-thread allocations (false-positive risk in test, not a runtime vuln) | `587f9d4` | Test quality |
| — | **INFO (pre-existing, unchanged)** | OPT passes CSE/DCE/ConstProp/LICM/Forward/DSE/InstCombine have documented correctness defects (C1–C10) incl. implicit-frame-write erasure (C3), non-SSA forwarding (C7), signed-overflow const folding (C9) | pre-baseline; audited in `69404ad` | Open; **not modified, not worsened** by this window |

**Prior-audit fix verification (HEAD `bf76217` findings, all present at baseline `acf01d0`, confirmed intact at HEAD):**
- CRITICAL #1 — v5 IR `frame_off` validation gap (return-address overwrite): **FIXED** at `src/thin_ir_ser.cpp:671-673` (now checks every instr with `frame_off != 0`, rejects `>= 0` and `< -frame_size`). Not touched by this window.
- HIGH #2 — `ext_array` element bounds via `size_t` multiply overflow: **FIXED** (division-based check `size_t(i) < bytes.size()/elem_size`; `checked_bytes` + `MAX_CONTAINER_BYTES` cap). Not touched by this window.
- MEDIUM #3/#4 — `bad_alloc`/`length_error` through native boundary in vec/mat/quat/string: **FIXED** (try/catch around allocations). Not touched by this window.
- LOW #6 — duplicate block IDs in v5 IR validator: **FIXED** at `src/thin_ir_ser.cpp:605` (`seen_ids.insert(...).second`). Not touched by this window.

**No regressions** to any prior fix were introduced by the 20 commits under review.

---

## 2. Commit-by-commit coverage table

Each row records whether the commit changes executable code or security assumptions, and what was inspected. "Window order" is newest→oldest as listed by `git log --oneline -20`.

| # | Commit | Subject | Exec code? | Sec assumptions? | Files inspected | Verdict |
|---|--------|---------|------------|------------------|-----------------|---------|
| 1 | `f256ff9` | Release script: ignore vst3sdk submodule dirty state | No (shell) | Yes — milestone gate filter | `scripts/prepare_release.sh` | F4: imprecise `grep -v` substring; low |
| 2 | `3176a6b` | VST3 wrapper: processor improvements (Phase 8) | Yes | Yes — param profile/bounds | `vst3_ember_processor.cpp/.h` | Clean: tighter param-ID check (`id >= plugin_parameter_count_`), span div-zero guarded, no OOB |
| 3 | `8e62dd1` | VST3 Phase 8: example plugins + guide | No (.ember/docs) | No | `delay_vst/filter_vst/oscillator_vst.ember`, guide | Clean: examples use bounds-checked typed accessors |
| 4 | `587f9d4` | VST3 Phase 7: stress tests + realtime + fuzz + soak | Yes | **Yes — reclamation redesign** | `vst3_ember_processor.cpp/.h`, `vst3_stress_tests.cpp` | **F1 (HIGH): replaced epoch-based `HotReloadDomain` with `audio_readers_` counter TOCTOU UAF**; F6 test note |
| 5 | `4445fe3` | Release script: both compilers | No (shell) | Yes — artifact selection | `scripts/prepare_release.sh` | F4: `VERSION` unsanitized (path traversal); masked-failure `|| true`; low |
| 6 | `5c89528` | VST3 Phase 6 + self-hosted audit | Yes | Yes — MIDI/f64/events/state | `ext_audio.cpp/.hpp`, `vst3_ember_processor.cpp/.h`, audit doc | Clean: f64 precision guard, event index/capacity/range bounds; `n_audio_add_event` fully validated |
| 7 | `01f38d9` | VST3 Phase 5: hot reload + state migration | Yes | Yes — hot-reload lifetime | `vst3_ember_processor.cpp/.h` | F3: `save_state`/`load_state` raw-pointer reinterpret (defense-in-depth). Phase-5 epoch reclamation was sounder than Phase-7 replacement |
| 8 | `f58f040` | VST3 Phase 4: typed audio pipeline + automation | Yes | Yes — typed AudioContext ABI | `ext_audio.cpp/.hpp`, `vst3_ember_processor.cpp/.h`, `vst_dsp_harness.cpp`, `sema.cpp` | Clean: typed accessors add channel/index/precision bounds checks; raw natives retained (F2) |
| 9 | `489573a` | README use cases + v1.1 | No (docs) | No | `README.md` | N/A |
| 10 | `7d16850` | VST3 CMake linking fix | No (build) | No | `examples/vst3_wrapper/CMakeLists.txt` | N/A (build only) |
| 11 | `342352b` | VST3 Phase 3: MinGW aligned_alloc + wrapper fixes | Yes | No | `vst3_ember_processor.cpp` | Clean: `_aligned_malloc/_aligned_free` compatibility; 6-line change |
| 12 | `2eae6d4` | VST3 Phase 3: vendor SDK 3.8.0 + minimal wrapper | Yes | Yes — new wrapper, submodule | `vst3_ember_processor.cpp/.h`, factory/entry/cmake, `.gitmodules` | Clean initial wrapper; submodule boundary reviewed (not entered). Vendor tree = third-party scope |
| 13 | `085eaaf` | VST3 Phase 2: @realtime checker + audio natives + DSP harness | Yes | **Yes — raw ptr natives + RT checker** | `ext_audio.cpp/.hpp`, `sema.cpp`, `vst_dsp_harness.cpp`, `ember_cli.cpp`, `sema_check.cpp` | **F2: raw `load_f32` etc. unchecked (intentional PERM_FFI)**; RT checker conservative (blocklist+allowlist, unknown rejected) |
| 14 | `585d7fe` | VST3 planning doc + roadmap | No (docs) | No | `plan_VST3_EMBER_WRAPPER.md`, `ROADMAP.md` | N/A |
| 15 | `69404ad` | Scheduled maintenance: PASS_AUTHORING + audit + log | No (docs) | No (audit doc only) | `OPTIMIZATION_PASSES_READ_ONLY_AUDIT_2026-07-12.md` et al. | Documents pre-existing opt-pass defects C1–C10; notes bounds-elim header/cpp mismatch (see §4) |
| 16 | `214aaa3` | Obfuscation passes: MBA + constant encoding | Yes | Yes — IR transforms | `ext_obf.cpp/.hpp` | Clean: MBA identities valid mod 2^N; const-encode shift form guarded; `BoundsCheck` never touched |
| 17 | `358b590` | Custom pass examples + docs | Yes (examples) | Yes — example pass mutation | `nop_injection_pass.cpp`, `block_merge_pass.cpp`, `minimal_pass.cpp`, `custom_passes.hpp` | F5: `BlockMergePass` lacks loop-member guard (example only); NOP/merge otherwise value-preserving |
| 18 | `21f8f87` | Optimization pass: bounds-elim | Yes | **Yes — removes BoundsCheck ops** | `ext_opt.cpp/.hpp` | **Reviewed sound**: canonical `while(i<N)` only, constant bound fits width (no IV wrap), single +1 update, no calls/indirect IV writes, removes only pre-update checks feeding fixed-base IndexAddr; overflow math guarded |
| 19 | `a1a1163` | Optimization pass: SimplifyCFG v2 | Yes | Yes — CFG transform | `ext_opt.cpp/.hpp`, test | Clean: v2 has loop-header/loop-member skip (explicitly fixes the v1 regression); constant-branch folding local/narrow; reachability traversal retains loop backedges |
| 20 | `0a76c59` | Revert SimplifyCFG — not value-preserving | Yes (removal) | Yes — removes broken pass | `ext_opt.cpp/.hpp` | Clean: reverts v1 that returned validation 116; net state = sound v2 from `a1a1163` |

---

## 3. Detailed findings

### F1 — Hot-reload use-after-free in crossfade reclamation (HIGH, NEW, open)

**Introduced by:** commit `587f9d4` (VST3 Phase 7), which replaced the epoch-based `ember::HotReloadDomain` + `ExecutionGuard` reclamation with a single `std::atomic<uint32_t> audio_readers_` counter.
**Files:** `examples/vst3_wrapper/vst3_ember_processor.cpp`, `examples/vst3_wrapper/vst3_ember_processor.h`.

**Root cause — TOCTOU between the watcher's grace-period check and the audio thread's retire+use:**

The watcher reclaims with a single observation of the reader counter (`reclaimRetiredModule`, lines 505-508):
```cpp
void EmberProcessor::reclaimRetiredModule() {
    // The watcher is the sole reclaimer. acquire observes process() reader
    // enrollment/exit; zero is the grace period for the old immutable plan.
    if (audio_readers_.load(std::memory_order_acquire) != 0) return;   // line 506
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);       // line 507
}
```

The audio thread enrolls, retires the old module into `retired_`, then uses it for the crossfade (`process`, lines 785-803):
```cpp
    audio_readers_.fetch_add(1, std::memory_order_acq_rel);             // line 785  enroll
    {
        EmberModule* old = activatePendingModule();                     // line 787
        // activatePendingModule() does: retired_.store(old, ...);       // line 499  retire old
        ...
        const std::size_t fadeFrames = !sample64 && old && old->process_f32 &&
            crossfade_channels_[0] ? std::min<...>({crossfade_samples_, numSamples, ...}) : 0;  // 790
        if (fadeFrames > 0) {
            ...
            old->process_f32(reinterpret_cast<int64_t>(&oldContext),    // line 802  EXECUTE old's JIT pages
                             static_cast<int64_t>(fadeFrames));
        }
```

`activatePendingModule` (line 473-) stores the previous `current_` into `retired_` (line 499):
```cpp
    if (old) retired_.store(old, std::memory_order_release);
```

**Exploit path / race timeline:**
1. No `process()` in flight: `audio_readers_ == 0`. `retired_` holds some prior module (or null).
2. Watcher: `audio_readers_.load() == 0` (line 506) — decides to reclaim.
3. Audio thread enters `process()`: `fetch_add(1)` → readers=1 (line 785).
4. Audio thread: `activatePendingModule()` consumes `pending_` (set earlier by the watcher), sets `current_ = candidate`, **`retired_.store(old = previous current)`** (line 499), returns `old`.
5. Audio thread: `fadeFrames > 0` (default crossfade = 64 samples), calls `old->process_f32(...)` (line 802) — **executing the old module's JIT code, which is the value now in `retired_`**.
6. Watcher: `retired_.exchange(nullptr)` (line 507) returns the module stored at step 4 (`old`) and **`delete`s it** — `~EmberModule` calls `ember::free_executable` on each `fn.exec`, unmapping the executable page the audio thread is still executing at step 5.

**Result:** use-after-free / execution of freed memory. The grace-period check at step 2 only proves no reader was active *at the check*; it does not prevent a reader from enrolling afterward (step 3), retiring a module (step 4), and using it (step 5) before the watcher's `delete` (step 6). The check and the delete are not atomic with respect to new enrollments.

**Why the prior Phase-5/6 design was sounder:** before `587f9d4`, reclamation used `reload_domain_.reclaim()` (epoch-based):
```cpp
// Phase 6 (5c89528):
void EmberProcessor::reclaimRetiredModule() {
    if (reload_domain_.reclaim() == 0) return;   // only frees pages whose retirement_epoch < min active guard entry_epoch
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);
}
```
`HotReloadDomain::reclaim()` (`src/hot_reload.hpp`) frees a retired page only when no guard that entered before the page's retirement epoch is still active, and `activatePendingModule` operated under a `ExecutionGuard` serialized with `publish()` via the domain mutex. That epoch contract prevented a newly-enrolled reader from keeping an older page alive or from being crossfaded on a page the reclaimer frees. The Phase-7 replacement discarded that serialization, leaving the counter-only check whose TOCTOU is described above.

**Trigger conditions:** the plugin must be built with hot reload enabled (default), audio processing active, a script edit producing a new `pending_` module, and `EMBER_VST3_CROSSFADE_SAMPLES` ≥ 1 (default 64) with an f32 process path. The race is non-deterministic and timing-sensitive; the in-tree stress test `runHotReloadStress()` (`vst3_stress_tests.cpp`) exercises exactly this scenario (audio thread in a tight `process()` loop + 3 file-appended reloads, 64-sample blocks so `fadeFrames == 64`) but its 350 ms inter-reload sleep makes the tight window unlikely to fire, and it is not run under ThreadSanitizer.

**Severity rationale:** HIGH. The freed object is executable JIT memory unmapped mid-execution on the realtime thread → crash or, depending on allocator reuse, corrupt execution. It is reachable by ordinary user action (editing the ember script while the DAW plays), not only by a crafted input.

**Recommended fix (not applied — read-only audit):** restore an epoch/RCU-style guarantee. Either (a) keep `HotReloadDomain` for the wrapper (the Phase-5/6 design), or (b) make the counter check and retire store non-overlapping: e.g., have `reclaimRetiredModule()` re-check `audio_readers_` *after* `retired_.exchange()` and only `delete` if still 0 (and otherwise re-publish the module to `retired_` for a later retry), since a reader that enrolled after the first check and stored a *new* retired value must still be inside its `fetch_add`/`fetch_sub` window. Concretely, a correct minimal shape is:
```cpp
void EmberProcessor::reclaimRetiredModule() {
    if (audio_readers_.load(std::memory_order_acquire) != 0) return;
    EmberModule* m = retired_.exchange(nullptr, std::memory_order_acq_rel);
    if (!m) return;
    // A reader could have enrolled between the load and the exchange and stored
    // a module it is still crossfading on. Re-check; if a reader is now active,
    // put it back and defer.
    if (audio_readers_.load(std::memory_order_acquire) != 0) {
        retired_.store(m, std::memory_order_release);
        return;
    }
    delete m;
}
```
(This still has a residual window if a reader enrolls between the second load and `delete`; the fully sound fix is the epoch domain or a reader that does not retire-then-use across the unguarded boundary — e.g., retire `old` only after the crossfade completes, or pin `old` with a reference count.)

---

### F2 — Raw audio-buffer natives are unchecked arbitrary memory R/W (MEDIUM, intentional/by-design)

**Introduced by:** commit `085eaaf` (VST3 Phase 2).
**Files:** `extensions/audio/ext_audio.cpp` (`n_load_f32`, `n_store_f32`, `n_load_f64`, `n_store_f64`, `n_load_i32`, `n_store_i32`).

**Evidence (current HEAD):**
```cpp
float n_load_f32(int64_t ptr, int64_t index) {
    return reinterpret_cast<float*>(ptr)[index];          // no null/bounds/length check
}
void n_store_f32(int64_t ptr, int64_t index, float value) {
    reinterpret_cast<float*>(ptr)[index] = value;         // arbitrary host write
}
// (f64/i32 variants identical)
```
All six are registered `PERM_FFI` (`ext_audio.cpp` register_natives). The Phase-2 header explicitly stated "Hosts must validate buffer ranges before entering JIT code"; the Phase-4 typed `audio_load_sample`/`audio_store_sample`/`_f64` accessors were added *with* channel/index/precision bounds checks, but the raw six were retained.

**Assessment:** This is an intentional capability-gated surface, not an oversight. An ember `i64` is a raw host pointer; `PERM_FFI` is the trust boundary — only FFI-permissioned scripts can call these. Within that boundary a script already has arbitrary native-call capability, so the raw accessors do not create a *new* privilege. The risk is mis-trust: a host that grants `PERM_FFI` to untrusted scripts (e.g., a DAW loading a third-party `.ember` plugin with FFI enabled) hands the script an arbitrary read/write primitive over the host process. The typed accessors should be the preferred API for plugin DSP; the raw six should be treated as "ffi escape hatch, caller validates."

**Note on the @realtime checker:** `load_f32`/`store_f32`/`load_f64`/`store_f64`/`load_i32`/`store_i32` are listed in `realtime_safe_native` (`src/sema.cpp:3284-`), so `@realtime` functions *may* call them. `@realtime` is a realtime-contract annotation (no alloc/IO/lock), **not** a memory-safety annotation; it does not restrict pointer values. This is consistent but worth stating explicitly: `@realtime` does not make raw pointer access safe.

**Severity:** MEDIUM (capability-gated; becomes HIGH if a host grants `PERM_FFI` to untrusted scripts). No code change required if the trust model is intentional; recommend documenting the "caller validates buffer ranges" contract on each raw native and discouraging `PERM_FFI` for untrusted plugins.

---

### F3 — `save_state`/`load_state` script return values reinterpreted as raw pointers (MEDIUM, defense-in-depth)

**Introduced by:** commit `01f38d9` (VST3 Phase 5); unchanged through HEAD.
**File:** `examples/vst3_wrapper/vst3_ember_processor.cpp` (`activatePendingModule`, `getState`, `setState`).

**Evidence:**
```cpp
// activatePendingModule (line ~483):
if (old && old->save_state) {
    const auto* state = reinterpret_cast<const ScriptStateBuffer*>(old->save_state());
    if (state && state->data && state->size > 0 &&
        state->size <= static_cast<int64_t>(kMaxStateBytes)) {
        statePointer = state->data; stateLength = state->size;
    }
}
if (candidate->load_state)
    candidate->load_state(statePointer, stateLength);
```
`save_state()` returns `int64_t` (validated by the wrapper to have signature `fn save_state() -> i64`). The wrapper reinterprets that `i64` as `const ScriptStateBuffer*` and dereferences `state->data`/`state->size` with no validation that the returned integer is a valid pointer to a `ScriptStateBuffer`. A buggy or malicious script returning an arbitrary integer yields an arbitrary host read (of `data`/`size`) and, if the size check passes, an arbitrary-pointer read by `load_state`.

**Assessment:** within the script-author trust boundary (the script is the plugin's own DSP code, already `PERM_FFI`-capable and able to call `load_f32` on arbitrary pointers), so this does not create a *new* privilege over F2. It is a defense-in-depth gap: the wrapper trusts a script-produced integer to be a valid pointer. `setState` (host→plugin) is well-validated (magic/version/size bounds, `kMaxStateBytes` cap, exact `readRaw` length) — that direction is clean.

**Severity:** MEDIUM (defense-in-depth). Recommend the wrapper either validate the returned pointer lies within the module's known allocations, or require `save_state` to fill a wrapper-provided buffer rather than return a pointer.

---

### F4 — Release script: imprecise dirty-tree filter + unsanitized VERSION (LOW)

**Commits:** `f256ff9` (filter), `4445fe3` (artifact selection).
**File:** `scripts/prepare_release.sh`.

**Evidence:**
```bash
# f256ff9:
if [[ -n "$(git status --porcelain | grep -v 'thirdparty/vst3sdk')" ]]; then   # substring filter
```
`grep -v 'thirdparty/vst3sdk'` suppresses *any* porcelain line containing that substring, not just the exact submodule path. A real dirty change to a path containing the substring (e.g. a hypothetical `…/thirdparty/vst3sdk_notes`) would be silently ignored, weakening the "clean tree" milestone gate. Anchored/path-scoped check (`git status --porcelain -- thirdparty/vst3sdk`) would be precise.

```bash
# 4445fe3 / throughout:
VERSION="${1:-}"
ARTIFACT_DIR="buildt/release-$VERSION"
mkdir -p "$ARTIFACT_DIR"
git tag -a "$VERSION" -m "ember $VERSION release"
```
`VERSION` is user-supplied and used unquoted-in-expansion inside double-quoted paths and as a git tag. It is not validated. Quoting prevents shell command injection, but `../` in `VERSION` yields path traversal outside `buildt/` (e.g. `buildt/release-../../x`), and a malformed tag is created. Several copy steps also use `|| echo WARN` / `|| true`, masking missing-artifact failures so a release could ship without the compiler.

**Assessment:** LOW. The script is developer-run with a trusted version argument (the trust model is "the release operator supplies a sane `vX.Y.Z`"). Recommend `[[ "$VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([.-].+)?$ ]]` validation, anchored submodule filter, and failing (not warning) on missing required artifacts.

---

### F5 — `BlockMergePass` example lacks loop-member safety guard (LOW, example only)

**Commit:** `358b590`.
**File:** `examples/custom_pass/block_merge_pass.cpp`.

**Evidence:** the example merge pass checks `predecessor_count[target_id] != 1`, `bi == 0`, `bi == ai`, and `IR_MAX_INSTRS`, but — unlike the real `SimplifyCFGPass` in `extensions/opt/ext_opt.cpp` (which has `if (loop_header[bi] || in_loop[ai] || in_loop[bi]) continue;`) — it does **not** skip loop members. For pure-`Jmp` cycles this preserves semantics (infinite loop → infinite loop) and no `BoundsCheck`/safety op is removed, but it can produce a single self-loop block from a multi-block `Jmp` cycle. As opt-in example/documentation code (not registered by default; a host must call `ember::examples::custom_pass::register_passes` explicitly), it does not affect the default security posture.

**Assessment:** LOW / documentation. Recommend adding a comment noting the example is deliberately simpler than the production `SimplifyCFGPass` and that production loop merges need the loop-member guard, to avoid leading extension authors to ship an over-aggressive merge.

---

### F6 — Realtime-contract test may count watcher-thread allocations (INFO, test quality)

**Commit:** `587f9d4`.
**File:** `examples/vst3_wrapper/stress_tests/vst3_stress_tests.cpp`.

`runRealtimeContract()` wraps each `process()` in an `AllocationScope` that sets a process-wide `g_track_allocations` and counts hits on a globally-overridden `operator new`. The hot-reload watcher thread (started in `Fixture`'s `initialize`) polls every 250 ms and calls `readTextFile` (which allocates a `std::string`) regardless of file changes. If that allocation coincides with an `AllocationScope` window, `allocations.count() == 0` can fail as a false positive. This is a test flakiness risk, not a runtime vulnerability; it may also mask genuine watcher allocations from being distinguished from audio-thread allocations. (The override also makes the test Windows-centric via `_putenv_s` in `runHotReloadStress`.)

---

## 4. Pre-existing optimization-pass defects (unchanged through this window; documented in-window)

The in-window commit `69404ad` added `docs/audit/OPTIMIZATION_PASSES_READ_ONLY_AUDIT_2026-07-12.md`, which documents correctness defects C1–C10 in CSE, DCE, ConstProp, LICM, Forward, DSE, InstCombine. Cross-checking the **current** `extensions/opt/ext_opt.cpp` against that audit confirms the following remain open and were **not modified** by any commit in this window (the only `ext_opt.cpp` touchers in-window are `21f8f87` bounds-elim, `a1a1163` SimplifyCFG v2, `0a76c59` revert — none touch CSE/DCE/ConstProp/LICM/Forward/DSE/InstCombine):

- **C3 (Critical, security-relevant):** DCE/ConstProp sweep/CSE treat a producer with non-zero `meta.frame_off` as a removable pure instr, erasing its implicit frame write; a later `LoadFrame` then reads uninitialized/stale memory. Current `DeadCodeElimPass`/`ConstPropPass` use `is_pure(op) && dst unused` with no `frame_off` check (`ext_opt.cpp` `is_pure`/`compute_used_vregs`). If the stale/uninitialized value feeds an index or a bounds comparison, this can yield OOB access. Exploit path: crafted v5 `.em` (unsigned in dev mode) — note the v5 `frame_off` *validator* is now fixed (prior CRITICAL #1), but the *passes* still mis-optimize validator-clean IR.
- **C7 (Critical):** `StoreToLoadForwardPass` forwards through a non-SSA source redefinition; the source comment asserts VRegs are SSA, contradicting the Thin IR contract. Exploitable via hand-built/deserialized IR with VReg redefinition.
- **C9 (Critical):** `fold_int_binop` computes signed `a + b`/`a - b`/`a * b` (C++ UB on overflow) instead of unsigned-then-cast; a folded bounds constant could be wrong.
- C1/C2 (CSE non-SSA + block-boundary miscompiles), C4 (stale slot fact), C5a–c (CSE key omissions), C6 (LICM speculates a trap past a zero-trip loop), C8a–c (memory/alias model gaps), C10 (LICM reorders non-SSA defs) — all still open per the audit and current code.

**bounds-elim note:** the OPT audit (§5) states "There is no corresponding implementation or registration in the reviewed `ext_opt.cpp`" for `bounds-elim`. That statement is **inaccurate**: `git show 69404ad:extensions/opt/ext_opt.cpp` contains `BoundsCheckElimPass` (3 occurrences) and registers it (`reg.add<BoundsCheckElimPass>("bounds-elim")`), present since `21f8f87`. The OPT audit therefore did **not** review the bounds-elim pass; this audit did (see §5).

**Regression check:** none of C1–C10 were worsened by this window (no edits to the affected pass logic). The window's opt changes are additive/conservative: a sound `bounds-elim`, a corrected `SimplifyCFG` v2, and a revert of the broken v1.

---

## 5. Deep review: `bounds-elim` pass (removes `BoundsCheck` ops) — verified sound

**Commit:** `21f8f87`. **File:** `extensions/opt/ext_opt.cpp` (`BoundsCheckElimPass::run`, ~line 1240+).

This is the highest-stakes pass because it deletes `BoundsCheck` instructions (a false positive = memory unsafety). The proof obligations, all verified against the current code:

1. **Canonical shape only:** header is a `Branch` with exactly 2 predecessors; body is a single `Jmp` block with exactly 1 predecessor (the header); latch is a single `Jmp` with exactly 1 predecessor (the body) targeting the header; a unique preheader `Jmp`s to the header. Any deviation → `return all()` (no transform). Single-block body means `if`/`break`/`continue` (which make `body.term` a `Branch` or add predecessors) cause a skip.
2. **Boolean wrapper unwound:** requires `cond = Cmp(wrapper, ==0)` where `wrapper = Cmp(i<N, cmp==2)`, matching the lowerer's `inner = (i<N); wrapped = (inner==0); branch wrapped ? exit : body`.
3. **IV is a frame slot:** `compare->src1` is a `LoadFrame` of `iv_slot` (`iv_slot != 0`); the bound `N` is constant (`compare->src2==0` immediate or a local `ConstInt`/`Move` chain via `const_before`, depth ≤ 8).
4. **No IV wrap-around:** `bound_fits_compare(bound, width, is_unsigned)` rejects `bound <= 0` and requires `bound ≤ max(width, signedness)` for width 1/2/4 (width 8 accepts any positive). Because `i` runs `0..N-1` and `N ≤ max`, `i = i + 1` never wraps past `N`, so `i < N` stays false-terminating. The update `Add` width is checked equal to `compare` width.
5. **Sole +1 update:** exactly one store overlaps the IV slot (`iv_stores == 1`), it is `slot = load(slot) + 1` in the body (`load_of_slot_before` + `Add` + immediate/const `1`, both operand orders handled), and it is located at `update_index`.
6. **No aliasing writes to IV:** any `CallNative/CallScript/CallIndirect/CallCrossModule`, `CopyBytes`, `StructLitInit`, `ArrayLitInit`, `StringDecrypt`, `StoreAddr` in header/latch, or `StoreAddr` in the latch → `unsafe_loop_write`. In the body, `StoreAddr` is allowed only at a fixed-base `IndexAddr` whose index is a fresh load of the IV slot and whose computed range `[first, first+span)` is proven disjoint from `[iv_slot, iv_slot+8)`.
7. **Overflow-guarded span math:** `iterations = bound-1` (bound>0 so ≥0), `elem_width = addr->meta.width > 0`; `if (iterations > (INT64_MAX - write_width)/elem_width) unsafe`; then `first > INT64_MAX - span` re-guarded before `intervals_overlap`. No 64-bit overflow reachable.
8. **Only pre-update checks removed:** the removal loop runs `i < update_index`; a `StoreAddr` at/after the update → `unsafe_loop_write`; the matched `IndexAddr` must appear before the update (`j < update_index`) with no intervening redefinition of the index (`candidate.dst == check.src1` breaks). After the update `i` may equal `N`, so post-update accesses are never de-checked.
9. **Matching bound + fixed base:** removed checks require `check.imm.i == bound` (same immediate N = array length) and `check.src2 == 0` (immediate form), so dynamic-length slice checks (src2 ≠ 0) are retained; the `IndexAddr` must be fixed-base (`src1 == 0`, `meta.width > 0`).

**Conclusion:** the pass is conservative and sound on the canonical loop; ambiguous CFG, dynamic lengths, aliases, non-canonical updates, and post-update accesses are all left checked. No regression: it is purely additive this window.

**`SimplifyCFG` v2 (`a1a1163`) — also reviewed sound:** constant-branch folding is local and narrow (only `ConstBool`/`Move` chains + the `Cmp ==0` wrapper in the branch's own block); unreachable removal is a reachability traversal that retains loop backedges reached from entry; block merging skips `loop_header[bi] || in_loop[ai] || in_loop[bi]` — the explicit fix for the v1 regression that returned validation 116 (reverted in `0a76c59`). The v1→v2→revert sequence leaves a sound CFG pass at HEAD.

---

## 6. VST3 integration / submodule boundary

- `thirdparty/vst3sdk` is a git submodule (SDK 3.8.0, MIT) added in `2eae6d4`; not entered or modified per scope. The wrapper integrates via `SingleComponentEffect` and the SDK hosting utilities used by the stress test (`eventlist.h`, `hostclasses.h`, `parameterchanges.h`).
- The wrapper's own assumptions about the SDK are the review target: it trusts host `ProcessData` shapes (buffer pointer arrays, `numChannels`, `numSamples`) per the VST3 contract, and validates the wrapper-facing fields (`numOutputs==1`, `numChannels==2`, non-null `outputBuffers`, per-channel null checks → bypass). Host→plugin `setState` is magic/version/size-validated. Plugin→host `getState` writes the script-produced state bytes verbatim after bounds-checking `size ≤ kMaxStateBytes`.
- The submodule shows as modified in `git status` (expected; the release script's `f256ff9` filter addresses this). No wrapper code reads from the vendor tree at runtime; the boundary is compile-time only.

---

## 7. Bottom line

- **One HIGH regression introduced this window:** F1, the hot-reload `audio_readers_` TOCTOU use-after-free (`587f9d4`), which is a step down from the sound epoch-based reclamation it replaced. Exploitable by editing a script during playback (crossfade path); non-deterministic but real.
- **Two MEDIUM intentional/defense-in-depth items:** F2 (raw buffer natives, `PERM_FFI`-gated by design) and F3 (unvalidated script `save_state` pointer, within the FFI trust boundary).
- **Two LOW + one INFO** release-script and example-pass hygiene items (F4, F5, F6).
- **Pre-existing OPT-pass defects C1–C10 remain open and unchanged** (not introduced or worsened this window; documented in the in-window OPT audit). C3/C7/C9 are security-relevant (can corrupt index/bounds values via crafted `.em` in dev mode).
- **All prior confirmed critical/high fixes verified intact** at HEAD (v5 `frame_off` validator, `ext_array` bounds, native-boundary exception containment, duplicate-block-ID rejection); **no regressions** to them.
- **`bounds-elim` (the pass that removes safety checks) is sound**; `SimplifyCFG` v2 is sound; obfuscation passes preserve `BoundsCheck` and use valid mod-2^N identities.

No edits or commits were made. This is a read-only review.
