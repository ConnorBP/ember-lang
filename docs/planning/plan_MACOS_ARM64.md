# Plan — macOS (Apple Silicon / ARM64) support

**Status:** COMPLETE (56/56 CTest, 471/471 lang_suite; full language parity +
self-hosted ARM64 codegen + coroutines). Implementation record:
`MACOS_ARM64_PROGRESS.md`. The design below is retained as the design record;
phase headings are annotated ✅ DONE. This document is the design record for
adding macOS support to ember, which originally targeted ONLY Windows x86-64
(Win64 ABI, MinGW g++, raw x86-64 machine-code emission).

**Target:** `arm64-apple-darwin` ONLY, for the first milestone. Apple Silicon
(arm64). Explicitly **out of scope** for v1 of this effort:

- Intel macOS (`x86_64-apple-darwin`, SysV ABI) — different ABI (SysV, not
  Win64) on a same-ish ISA; YAGNI. Add later if ever needed.
- `arm64e` (Apple's pointer-authenticated variant) — PAC adds signing/signing-
  key concerns; treat as a later target.
- Windows ARM64 (`arm64-windows`) — different ABI again; not asked for.

We develop and test natively on a Mac (Apple Silicon), so we can actually build
and run here — unlike the existing "Linux/macOS path written but untested"
state noted in `src/platform.cpp`.

---

## 0. Why this is large — the platform-coupling map — ✅ DONE (audit validated; every coupling resolved across the phases below)

Ember is a **JIT compiler that emits raw native machine code** and **calls it
with a hand-written ABI**. It is not an interpreter. Porting is therefore NOT
"gating a few `#ifdef _WIN32` calls"; it is authoring a **second instruction-set
backend** plus a second ABI, while keeping the front-end (lexer/parser/sema),
the IR, the sandbox, and the host API unchanged.

The platform coupling, audited against the current tree:

### Instruction set (the dominant cost)
| File | Lines | Coupling |
|---|---|---|
| `src/x64_emitter.hpp` | 576 | **THE ISA layer.** Emits raw x86-64 bytes. `enum Reg { rax..r15 }`, `enum Xmm { xmm0..15 }`, `enum Cond` (x86 jcc nibbles), REX/ModRM/SIB byte construction, SSE scalar float (`movss`/`addss`/…), Win64 stack-alignment tracking (`rsp_mod16_`, 32-byte shadow space, `win64_call_frame_size`). A new `Arm64Emitter` is the ARM64 equivalent. |
| `src/codegen.cpp` | 6148 | Tree-walker codegen. Uses `X64Emitter` directly, assumes Win64 ABI: `rcx/rdx/r8/r9` args, `rax` return, `r14 = context_t*`, `rbp` frame, shadow space. **Stays Windows-x64-only** (see §1 decision). |
| `src/thin_lower.cpp` | 3151 | ThinIR lowering → `ThinOp` spine. Currently emits x86 via `X64Emitter` AND leaks architecture: `rbp`-negative frame offsets, x86 register identities, call layout. The ARM64 path reuses the IR-spine work but gets its own emitter + a target-aware lower. |
| `src/thin_emit.cpp` | 2634 | `emit_x64(ThinFunction, CodeGenCtx)` — the ThinIR→machine-code emitter (and the v5 `.em` IR re-lower path). ARM64 gets `emit_arm64`. |
| `src/peephole.cpp` | 204 | x86 peephole patterns (rel32→rel8, etc.). **ARM peepholes stay separate** — do not pollute this file. |
| `src/regalloc.cpp` | 428 | Linear-scan over Win64 callee-saved pool (`rbx/rsi/rdi/r12/r13/r15`, `r14 = context` avoided). ARM64 gets a target-configured regalloc over ARM64 callee-saved regs. |

### ABI / calling convention
| File | Coupling |
|---|---|
| `src/dispatch_abi.hpp` | `enum TargetArch { X64_Win64 }` — **only one value**. ABI fingerprint encodes `kWin64ConventionVersion`. `WordClass { GP, XmmF32, XmmF64, HiddenPtr, Stack }` maps to Win64 (`rcx/rdx/r8/r9` + `xmm0-3`). Needs `Arm64_Darwin` + a separate convention version + arch-neutral FP word classes. |
| `src/engine.cpp` | **Inline GCC asm** (`__asm__`) for ABI thunks: `pushq %r14`, `movq %rcx, %r11`, `subq $32,%rsp` (shadow space), keyed thunks install `r14`/`r15`. Guarded by `#if defined(__GNUC__) && defined(__x86_64__)`. **There is NO arm64 asm path** — these thunks do not exist on ARM64 and every `ember_call_*` / keyed entry would fail to link. |

### Traps / exceptions (the recoverable sandbox)
| File | Coupling |
|---|---|
| `src/context.hpp` | `EMBER_SETJMP`/`EMBER_LONGJMP`: `__builtin_setjmp`/`__builtin_longjmp` on MinGW, **plain `setjmp`/`longjmp` in the `else` branch** (already covers clang/macOS). Host checkpoint is portable. BUT the **JIT try/catch** saves a CUSTOM 64-byte buffer `catch_bufs[256][8]` = `[rbx, rbp, r12, r13, r14, r15, rsp, rip]` — **x86 callee-saved + rip**. This layout is x86-specific and must become a target-defined opaque save area. Trap emission uses `ud2` (x86 illegal op); catch entry found via `lea rip` (RIP-relative). ARM64 has no RIP — uses PC-relative (`adrp`/`add` / `adr`) — and its trap is a UDF encoding. |
| `src/codegen.cpp` | `@obf_keyed` gate emits x86 `cpuid` (`0x0F 0xA2`) keyed on CPUID leaf-1 EAX (`current_cpuid_signature()` uses `__cpuid`). **ARM64 has no CPUID**; `MIDR_EL1` is kernel-only on Apple Silicon and unreadable from user space. Disable `@obf_keyed` on ARM64 with a diagnostic; later route it through the existing host key-provider abstraction (`src/key_provider.cpp`), never a fake hardware identity. |

### OS APIs
| File | Coupling |
|---|---|
| `src/platform.cpp` | **Already has a Linux/macOS mmap/mprotect/munmap path** (untested). TWO macOS bugs: (1) `#include <linux/limits.h>` — Linux-only, **breaks compilation on macOS** (use `<limits.h>` / `PATH_MAX`); (2) `executable_path()` reads `/proc/self/exe` (Linux) — macOS needs `_NSGetExecutablePath`. **Also:** plain `mprotect(PROT_READ|PROT_EXEC)` is **insufficient on Apple Silicon** (see §3 — needs `MAP_JIT` + `pthread_jit_write_protect_np`). |
| `src/jit_memory.cpp` | Cleanly delegates to `platform::alloc_rw/protect_rx/free_page` (good abstraction). **Real bug:** `free_executable(ptr)` calls `free_page(ptr, 0)`; on the POSIX path `munmap(ptr, 0)` silently frees nothing → a JIT-memory leak. Windows ignores size. Fix: track the rounded alloc size and pass it to free. |
| `src/safety.cpp` | RSS: Windows `GetProcessMemoryInfo`; Linux `/proc/self/statm`; macOS needs `mach_task_basic_info` via `task_info` (noted as TODO in `safety.hpp`). |
| `extensions/coroutine/` | Windows fibers only (`CreateFiber`/`SwitchToFiber`). Non-Windows gets an **empty stub** (`ext_coroutine_stub.cpp`). **`ucontext` is NOT a good macOS answer** — deprecated/problematic on Apple. Defer; later write a small Darwin ARM64 context-switch in assembly. |
| `src/em_file.hpp` | **GOOD NEWS:** already has full arch/OS/CC detection — `EMBER_EM_ARCH` (`arch=arm64`), `EMBER_EM_OS` (`os=darwin`), `EMBER_EM_CC` (`cc=aapcs64`), and `EM_TARGET_ABI_HASH`. The `.em` identity layer is already arch-aware. **ONE leak:** the `EM_TARGET_ABI_HASH` string hardcodes `"code=x64-v1"` (em_file.hpp:273) — must become arch-specific (`code=arm64-v1` on ARM64). |

### Build
| File | Coupling |
|---|---|
| `CMakeLists.txt` (91 KB) | MinGW g++ 15.2.0, Windows-first. `if(MSVC AND x64) FATAL_ERROR`. Already has `if(WIN32)/else()` branches (coroutine stub, `psapi` link, Win-only asm). Needs Apple Clang, arm64 detection, and gating of the x86-only sources/asm on non-x86. |

### Two codegens, one IR
Ember has **two** machine-code backends, both emitting x86 directly:
- **Tree-walker** (`codegen.cpp`) — the default.
- **ThinIR** (`thin_lower.cpp` + `thin_emit.cpp`) — enabled with `--passes`, with an optimization/obfuscation pass pipeline.

The ThinIR backend is the natural multi-target seam: it already has an
IR (`ThinFunction` / `ThinOp`) that is *mostly* arch-neutral, and a separate
emit step. The tree-walker is a 6148-line single-target emitter with no IR.

---

## 1. Key architectural decision — ThinIR is the sole ARM64 path — ✅ DONE (ThinIR forced on ARM64; tree-walker is a hard compile error; --passes no-op)

**Decision:** the ARM64 backend uses **ThinIR only**. The tree-walker
(`codegen.cpp`) and `X64Emitter` stay **Windows-x64-only, unchanged**. We do
NOT:
- template the codegen on an emitter type, nor
- build a virtual "universal emitter" with arch-neutral register enums, nor
- port the 6148-line tree-walker to ARM64.

**Rationale:** x86 and ARM64 are not isomorphic. The x86 register file
(`rax..r15`, `xmm0..15`, REX extended-register encoding, `rsp`/`rbp` roles),
the Win64 shadow-space/16-byte-align invariant, SSE scalar float, and
RIP-relative addressing do not map cleanly onto ARM64's orthogonal
`x0..x30`/`v0..v31` + `sp`/`lr`/`pc` + AAPCS64 + NEON. Templating or
abstracting would force a lowest-common-denominator emitter that serves
neither target well and would touch all ~12.5K codegen lines at once.

ThinIR already separates **IR** (arch-neutral spine) from **emit** (arch-
specific). The ARM64 work is therefore:
1. refactor the architecture leaks out of `thin_lower.cpp` (rbp offsets, x86
   register identities, call layout) into **target metadata**;
2. add `emit_arm64` + `Arm64Emitter`;
3. add a target-configured regalloc (start frame-only/no-regalloc on ARM64,
   enable physical registers later);
4. **auto-select ThinIR on ARM64 even without `--passes`** (the tree-walker is
   simply unavailable on the ARM64 target).

Unsupported ThinIR features on ARM64 must produce an **explicit compile
diagnostic** — never silently emit x86 or change semantics. Long term, close
the ThinIR feature gaps and optionally retire the tree-walker; do not
duplicate it for ARM.

**Consequence:** `--passes` becomes a no-op flag on ARM64 (ThinIR is always
on); the tree-walker-specific paths are compiled out on non-x86. The
optimization passes (`extensions/opt`) operate on `ThinFunction` and are
arch-neutral — they keep working on ARM64 unchanged.

---

## 2. Register allocation for ARM64 (Apple ABI constraints) — ✅ DONE (frame-only; x19=context, x20=rbx-role, never x18; regalloc a later add)

AAPCS64 + Apple platform rules (from the advisor; to verify against current
Apple toolchain docs during phase 3):

- **Do NOT use `x18`** — Apple reserves it as a platform register. Using it
  corrupts Apple runtime state. This is a hard rule.
- **`context_t*`** (the role `r14` plays on Win64): use a **callee-saved**
  register, e.g. `x19`, so it survives script→script calls. Reserve a second
  callee-saved reg for keyed transient state (the role of `r15`).
- `x16`/`x17` are inter-procedure scratch (IP0/IP1) — usable as scratch but
  not across calls.
- `x29` = FP, `x30` = LR — managed by the prologue/epilogue.
- SIMD/FP: `v0..v7` argument/return; `v8..v15` callee-saved (**low 64 bits
  only** — must be preserved as 8-byte pairs); `v16..v31` caller-saved.
- ARM64 conditional branches (`B.cond`) have a **±1 MiB range**; ember permits
  functions larger than that → plan **veneer trampolines** for out-of-range
  branches from day one of the assembler.
- Relocatable 64-bit addresses: prefer a **literal pool** of pointer cells
  (one 8-byte slot, patched like today's `AbsFixup` imm64) over patching a
  multi-instruction `movz`/`movk` sequence, which is awkward to relocate.

---

## 3. W^X executable memory on Apple Silicon (phase 2 — mandatory details) — ✅ DONE (MAP_JIT + pthread_jit_write_protect_np + mprotect(RX) + sys_icache_invalidate; the mandatory mprotect was the empirical finding)

The current `platform.cpp` POSIX path (`mmap(PROT_READ|PROT_WRITE)` then
`mprotect(PROT_READ|PROT_EXEC)`) is **insufficient on Apple Silicon**.
Required:

- Allocate with **`MAP_JIT`** (`mmap(..., PROT_READ|PROT_WRITE, MAP_PRIVATE |
  MAP_ANON | MAP_JIT, -1, 0)`). MAP_JIT is required for JIT pages on arm64
  macOS under the hardened runtime.
- Toggle writability with **`pthread_jit_write_protect_np(0/1)`** — this is a
  **thread-local** toggle, NOT a per-page `mprotect` replacement. Model write
  access as a **scoped write window** (RW: `pthread_jit_write_protect_np(0)`;
  ...copy/patch...; RX: `pthread_jit_write_protect_np(1)`). Because it is
  thread-local, concurrent threads must each manage their own window.
- After final patching and **before execution**, call
  **`sys_icache_invalidate(addr, len)`** (or `__builtin___clear_cache`). This
  is **mandatory on ARM** — the D-cache and I-cache are not coherent, so
  freshly written code can execute stale I-cache lines without it.
- The hardened runtime requires the **`com.apple.security.cs.allow-jit`**
  entitlement for the host process. Document this; the CLI/dev harness needs it.
- **16 KiB pages** are common on Apple Silicon (vs 4 KiB elsewhere) — page-
  size rounding must use `sysconf(_SC_PAGESIZE)`, not a hardcoded 4096.
- **Fix the free-size bug:** `free_executable(ptr)` passes size `0` to
  `free_page` → `munmap(ptr, 0)` frees nothing on POSIX. Track the rounded
  allocation size (e.g. in `jit_memory` or via a size-aware free API) and pass
  it through. Windows `VirtualFree(.., 0, MEM_RELEASE)` ignores size, so the
  fix is cross-platform-safe.

---

## 4. Phased plan — ✅ DONE (all phases landed)

Each phase ends in something that builds and/or runs. The first useful
milestone (end of phase 5): **macOS ARM64 builds natively and safely runs an
integer/control-flow ThinIR subset with native calls and recoverable traps.**
Full language parity is a later milestone, not a prerequisite for proving the
backend. (All phases shipped; see the per-phase ✅ DONE annotations below +
`MACOS_ARM64_PROGRESS.md`.)

### Phase 0 — Target contract + host-only macOS build — ✅ DONE
**Goal:** the project compiles on macOS-ARM64 with the JIT/codegen stubbed
("arm64 codegen not yet supported"); all non-JIT tooling builds.

- Add a central `TargetInfo { TargetArch arch; enum Os; enum Abi; }` (header,
  e.g. `src/target_info.hpp`). `TargetArch` gains `Arm64_Darwin` alongside
  `X64_Win64`. A build-time macro selects the active target.
- **CMake:** enable Apple Clang; detect `APPLE`/`CMAKE_OSX_ARCHITECTURES`/
  `__aarch64__`; gate the x86-only sources (`codegen.cpp` tree-walker,
  `x64_emitter.hpp` consumers, the `engine.cpp` x86 inline asm, `peephole.cpp`,
  x86 `regalloc`) behind `if(EMBER_TARGET_X64)`; on arm64 compile a stub
  `codegen` that throws `std::runtime_error("arm64 codegen not supported yet")`
  so the front-end (lexer/parser/sema/`ember_check`/`sema_check`) and `.em`
  tooling build and run.
- **`platform.cpp`:** fix the macOS compile bug (`<linux/limits.h>` → portable
  `PATH_MAX`), and implement `executable_path()` for macOS via
  `_NSGetExecutablePath` (`<mach-o/dyld.h>`). Keep the Linux path intact.
- **`safety.cpp`:** implement `process_rss_kb()` for macOS via
  `mach_task_basic_info` / `task_info` (`<mach/mach.h>`).
- Keep the coroutine extension stubbed; have unsupported execution features
  emit a clear "not supported on this target" diagnostic.
- **Exit criteria:** `cmake -G Ninja .. && cmake --build .` succeeds on
  macOS-ARM64; `ember_check`/`sema_check` run on `.ember` files; any JIT run
  fails with the explicit stub error (not a link error).

### Phase 1 — Darwin JIT memory (W^X) — ✅ DONE
**Goal:** safe executable-memory alloc/free/seal/patch on Apple Silicon.

- Add a dedicated **Darwin** `platform::` implementation using `MAP_JIT` +
  `pthread_jit_write_protect_np` (scoped write window) as described in §3.
  Keep the existing Windows and Linux paths.
- Call `sys_icache_invalidate` / `__builtin___clear_cache` after the final
  patch in `seal_executable` (or a new `flush_icache` step) before execution.
- Fix `free_executable`'s size-0 munmap leak (track rounded size; thread it
  through `free_page`). Verify `jit_memory`'s two-phase (`alloc_executable_rw`
  → patch → `seal_executable`) still works with the thread-local toggle.
- Document the `com.apple.security.cs.allow-jit` entitlement requirement.
- **Exit criteria:** a tiny C++ test allocates an RX page, copies a hand-
  written ARM64 `ret` (or `mov x0,#42; ret`) stub, seals it, and calls it —
  returns correctly; the page frees cleanly.

### Phase 2 — Target/ABI seam — ✅ DONE
**Goal:** the ABI layer knows about `Arm64_Darwin`; native-call classification
follows Apple's ARM64 ABI.

- `dispatch_abi.hpp`: add `TargetArch::Arm64_Darwin` + a separate convention
  version (`kArm64DarwinConventionVersion = 1`). In **new code**, name FP
  word classes arch-neutrally (e.g. `FpF32`/`FpF64`); preserve the old
  serialized `XmmF32`/`XmmF64` numbering where the on-disk format demands it.
- Centralize script-call and native-call classification. **Script→script**
  may use an Ember-private convention (we control both sides); **native calls
  and host thunks MUST obey the Darwin ARM64 ABI**, including:
  - AAPCS64 argument registers `x0..x7` (GP) / `v0..v7` (FP);
  - **HFA** (Homogeneous Floating-point Aggregate) rules — aggregates of ≤4
    same FP members passed in FP registers (Win64 never does this);
  - Apple's small-integer and variadic-call differences;
  - return in `x0` (or `v0`), `x8` = indirect-result pointer for large
    aggregates.
- Define the ARM64 register reservations (§2): `x19` = `context_t*`, a second
  callee-saved reg for keyed state; avoid `x18`; respect `x16/x17`, `x29/x30`,
  and the `v8..v15` low-64 preservation rule.
- `em_file.hpp`: make the `"code=x64-v1"` component of `EM_TARGET_ABI_HASH`
  (line 273) **arch-specific** (`code=arm64-v1` on ARM64) so a macOS `.em`
  carries a correct, distinct ABI hash.
- **Exit criteria:** ABI classifier produces correct `Arm64_Darwin`
  fingerprints; a `.em` written on macOS hashes with the arm64 codegen tag.

### Phase 3 — ARM64 assembler — ✅ DONE
**Goal:** `Arm64Emitter` can emit the instruction subset ThinIR needs.

- New `src/arm64_emitter.hpp`: fixed-width 32-bit instruction encoding, a
  label/branch-fixup system (mirroring `X64Emitter`'s), and:
  - integer arithmetic/logic (`add`/`sub`/`and`/`orr`/`eor`/`mul`/`lsl`/`lsr`/
    `asr`/`mov`/`mvn`),
  - comparisons + conditional sets (`cmp`/`subs`/`cset`),
  - loads/stores (`ldr`/`str` x8/x16/x32/x64, signed/zero-extend variants),
  - branches (`b`/`bl`/`b.cond`/`cbz`/`cbnz`/`ret`),
  - calls/returns (`bl` register-indirect via `blr`),
  - FP scalar (NEON `fmov`/`fadd`/`fsub`/`fmul`/`fdiv`/`fcmp`/`fcvt`),
  - **literal pools** for relocatable 64-bit addresses (one 8-byte pointer
    cell per `AbsFixup`, patched at load/JIT time — the ARM64 analogue of
    today's imm64 reloc),
  - **conditional-branch veneers** for out-of-range `B.cond` (≥±1 MiB).
- ARM peepholes live in a **separate** file (do not touch `peephole.cpp`).
- **Exit criteria:** unit tests assert exact bytes for each emitted form
  against known-good ARM64 encodings; round-trip through a disassembler
  (`llvm-objdump -d` / `otool -tv`) where feasible.

### Phase 4 — Minimal safe end-to-end ThinIR on ARM64 — ✅ DONE
**Goal:** a real, safe ARM64 backend runs a subset. **This is the first
"useful" milestone.**

- **Auto-select ThinIR on ARM64** even without `--passes` (the tree-walker is
  unavailable). Make `--passes` a no-op on ARM64.
- `emit_arm64(ThinFunction, CodeGenCtx)`: emit ARM64 for the integer/control-
  flow subset — integers, locals, arithmetic, conditionals, loops, script
  calls, **native scalar calls**, globals, return values.
- Refactor architecture leaks in `thin_lower.cpp` into target metadata
  (rbp-negative frame offsets → target-defined frame base; x86 register
  identities → target register set; call layout → ABI-aware). Start
  **frame-only, no regalloc** on ARM64; enable physical registers later.
- **ABI thunks:** add Darwin ARM64 entry/re-entry/keyed thunks in a
  **separate `.S` file** selected by CMake (install `x19` = context, the
  keyed reg, set up the JIT call). **Do not add more inline asm to
  `engine.cpp`** — the x86 inline-asm approach does not scale to a second
  arch; an out-of-line `.S` is cleaner and keeps `engine.cpp` arch-neutral.
- Preserve sandboxing as part of THIS milestone: per-frame instruction
  budget, call-depth guard, bounds checks, and **recoverable host traps**
  (setjmp/longjmp checkpoint — already portable). Feature-subset milestones
  are fine; an unsafe bypass is not.
- **Exit criteria:** `ember run` on macOS-ARM64 executes
  `fn main() -> i64 { ... }` integer/control-flow scripts correctly; the
  instruction-budget, call-depth, and bounds traps fire and recover via the
  host checkpoint; a native scalar call round-trips.

### Phase 5 — Traps and language exceptions — ✅ DONE
**Goal:** `try`/`catch`/`throw` and traps work on ARM64.

- Host `setjmp`/`longjmp` checkpoint stays as-is (portable).
- Replace the fixed x86 `catch_bufs[256][8] = [rbx,rbp,r12,r13,r14,r15,rsp,rip]`
  contract with a **target-defined opaque, aligned save area + stride**. ARM64
  catch state covers: required callee-saved GP registers, SP, the resume
  PC/LR, and any callee-saved SIMD state the regalloc uses.
- Catch-entry address: use **PC-relative** addressing (`adr`/`adrp`+`add`)
  instead of x86 `lea rip`.
- Trap instruction: an ARM **UDF** encoding is the hard-fault fallback only;
  **normal recoverable traps continue to call the host trap stub** (the
  recoverable model is unchanged — only the illegal-instruction encoding
  differs).
- **Exit criteria:** `try`/`catch`/`throw` and every `TrapReason` recover
  correctly on ARM64; the catch stack nests and unwinds across frames.

### Phase 6 — Language parity on ARM64 (incremental) — ✅ DONE (6a floats, 6b/6c slices/structs/strings, 6d for-each+match lowered to ThinIR on BOTH arches, 6e defer/constexpr/global-string gaps)
**Goal:** bring the ARM64 ThinIR backend to language parity, feature by feature.

Order (cheapest→hardest, each independently shippable):
1. **Floats** (NEON scalar) — `f32`/`f64` arithmetic, conversions, comparisons.
2. **Slices + bounds checks** — `{ptr, len}` view, OOB → trap.
3. **Structs / aggregates by value** — including Apple ARM64 HFA passing/
   return (diverges from Win64; needs the §2 ABI work fully wired).
4. **Lambdas + GC** — the tracing GC (`extensions/gc`) is arch-neutral, but
   the shadow-stack frame-record linkage (`gc_frame_head`) and by-ref capture
   must work through the ARM64 frame layout.
5. **Cross-module calls** — dispatch table + the keyed-dispatch thunks on ARM.
6. **String/array/map/vec/… extensions** — most are arch-neutral host code;
   verify the few that touch ABI (struct-by-value vec/quat/mat) on ARM64.

Each sub-step: add the ThinIR ARM64 emit support, gate unsupported-on-ARM
constructs with a diagnostic until they land, and pin behavior with tests.

### Phase 7 — Modules, `.em`, and self-hosting on ARM64 — ✅ DONE (v5 IR loader dispatches to emit_arm64; self-hosted ARM64 codegen target built t24–t26; 5/5 self-hosted tests PASS — see §6 update below)
**Goal:** the `.em` ecosystem works on macOS-ARM64.

- `EM_TARGET_ABI_HASH`: the arch-specific `code=arm64-v1` tag (done in phase 2)
  means a macOS `.em` is rejected on Windows and vice versa (correct).
- **v5 IR loading:** today the loader re-lowers IR via `emit_x64`; make it
  dispatch to `emit_x64` OR `emit_arm64` by target. Initially allow ARM `.em`
  **output only for all-IR modules**; reject raw-x86 records by policy + ABI
  hash. Raw ARM machine-code modules need backend-specific reloc metadata —
  defer.
- **Self-hosted compiler + `call_raw`:** the self-hosted codegen
  (`self_hosted/codegen.ember`) currently emits **x64 bytes** and runs them
  via `call_raw`. On ARM64 this would emit unrunnable x86. **Gate the self-
  hosted codegen + `call_raw` off on ARM64** until the self-hosted codegen
  gains an ARM64 target (a separate, later effort). The host-written compiler
  (phases 4–6) is the path to ARM64 execution in the meantime.

### Phase 8 — Deferred platform features — ✅ DONE (coroutines via darwin_arm64_ctx_switch.S; cross-module function handles; keyed paths remain stubbed/fail-closed)
- **Coroutines:** keep unavailable initially. **Do not promise `ucontext`** as
  the production design — it is deprecated/problematic on Apple. Later, write
  a small **Darwin ARM64 context-switch in assembly** (save/restore callee-
  saved GP + SIMD + SP/LR) or adopt a suitable already-installed dependency
  (none currently — do not add one for this).
- **`@obf_keyed`:** disable on ARM64 with a clear diagnostic. Do NOT
  substitute a fake CPU identity (`MIDR_EL1` is user-unreadable on Apple
  Silicon). Later, route the gate through the existing **host key-provider
  abstraction** (`src/key_provider.cpp`) rather than hardware identification.
- **V6 keyed dispatch:** enable on ARM64 only after its target capability
  matrix and ARM thunks are defined.
- **`arm64e` (PAC):** a later target, not this effort.

---

## 5. Things that will bite us (verified against the tree)

- **`engine.cpp` inline asm has no arm64 path.** Every `ember_call_*` and
  keyed thunk is `#if defined(__GNUC__) && defined(__x86_64__)` only. On ARM64
  these symbols do not exist → link failure, unless phase 4's `.S` thunks
  provide them. (Fix: phase 4.)
- **`"code=x64-v1"` hardcoded in `EM_TARGET_ABI_HASH`** (em_file.hpp:273). A
  macOS `.em` would mis-hash as x64. (Fix: phase 2.)
- **v5 `.em` IR re-lowers via `emit_x64` only.** Loading an IR `.em` on ARM64
  would emit x86 into an ARM64 page → crash. (Fix: phase 7.)
- **`catch_bufs` 64-byte layout is x86-specific** (context.hpp). ARM64 needs a
  target-defined save area. (Fix: phase 5.)
- **Self-hosted codegen emits x64** and `call_raw` runs it. On ARM64 this is
  unrunnable; gate it off until the self-hosted codegen has an ARM64 target.
  (Fix: phase 7.) ✅ Fixed in Phase 7 — the self-hosted codegen gained an ARM64
  target (`cg_target`/`CG_TARGET_ARM64` in `self_hosted/codegen.ember`); all
  5 self-hosted tests PASS (lex/parse/sema/codegen/full_pipeline).
- **`free_executable` size-0 munmap leak** (jit_memory.cpp:40). POSIX frees
  nothing. (Fix: phase 1.) ✅ Fixed in Phase 1 — `jit_memory` tracks the rounded
  alloc size in a size map (now mutex-guarded per the quality audit) and passes
  it to `free_page`/`munmap`.
- **`<linux/limits.h>` on macOS** (platform.cpp). Compile error on macOS.
  (Fix: phase 0.)
- **Plain `mprotect` for RX on Apple Silicon** is insufficient — needs
  `MAP_JIT` + `pthread_jit_write_protect_np` + `sys_icache_invalidate`.
  (Fix: phase 1.) ✅ Fixed in Phase 1 — the empirical finding was stronger still:
  the thread-local toggle alone **bus-errors**; a **mandatory**
  `mprotect(PROT_READ|PROT_EXEC)` after the toggle is required to grant
  executability. See `CODEGEN_SPEC_ARM64.md` §5.
- **`x18` is Apple's platform register** — never use it for context/scratch.
  (Constraint: phases 2/4.)
- **ARM64 `B.cond` ±1 MiB range** — large functions need veneers. (Fix:
  phase 3.)
- **`@obf_keyed` `cpuid` gate** — no ARM64 equivalent; disable + diagnostic.
  (Fix: phase 8.) ✅ Fixed in Phase 8 — `@obf_keyed` is disabled on ARM64 (no
  CPUID/MIDR; MIDR_EL1 is kernel-only on Apple Silicon); route through the host
  key-provider abstraction remains a future item.
- **Coroutines (`ucontext`)** — deprecated/problematic on Apple; defer, then
  assembly context-switch. (Fix: phase 8.) ✅ Fixed in Phase 8 — a hand-written
  AAPCS64 context switch (`darwin_arm64_ctx_switch.S`, no `ucontext`) ships;
  `yield` lowers to a `__ember_coro_yield` native. Keyed coroutines stay
  fail-closed.

---

## 6. What is NOT in scope (YAGNI, for this effort)

- Intel macOS (`x86_64-apple-darwin`, SysV ABI).
- Windows ARM64 (`arm64-windows`).
- `arm64e` (pointer authentication).
- Porting the tree-walker codegen to ARM64 (ThinIR is the sole ARM64 path).
- ~~`ucontext`-based coroutines on macOS~~ — **✅ DONE in Phase 8** via a
  hand-written AAPCS64 context switch (`darwin_arm64_ctx_switch.S`); `ucontext`
  was deliberately NOT used.
- Raw-ARM-machine-code `.em` modules (backend-specific relocs; defer — IR
  `.em` only initially). The v5 IR-on-disk loader dispatches to `emit_arm64`
  (Phase 7 ✅).
- ~~An ARM64 target for the self-hosted codegen~~ — **✅ DONE in Phase 7**
  (t24–t26): `self_hosted/codegen.ember` gained `cg_target`/`CG_TARGET_ARM64`
  + the ARM64 emitter port (`cg_arm_insn` and the `cg_arm_*` family) +
  `cg_detect_target`/`native_target_arch` + the prologue-check gate + the
  AAPCS64 prologue/epilogue (`stp`/`ldp` + ADD-immediate `mov x29,sp`). All 5
  self-hosted tests PASS (lex/parse/sema/codegen/full_pipeline). Moved out of
  out-of-scope; the self-hosted codegen now emits correct executing AArch64
  via `call_raw`.

---

## 7. First useful milestone (definition of done for the initial port) — ✅ DONE (and far exceeded)

**macOS ARM64 builds natively and safely runs an integer/control-flow ThinIR
subset**, including native scalar calls and recoverable traps, with the
sandbox (instruction budget, call-depth guard, bounds checks, host trap
recovery) intact. This was the end of phase 5. **Full language parity (phases
6–8) subsequently shipped, incrementally**: the final state is **56/56 CTest,
471/471 lang_suite** — full language parity (int/control-flow/try-catch/throw/
floats/slices/structs(HFA + by-value + by-ptr)/strings/f-strings/for-each/
match/defer/constexpr/global-init/function-refs/cross-module handles) + a
self-hosted ARM64 codegen target + coroutines. See `MACOS_ARM64_PROGRESS.md`
for the implementation record + `docs/spec/CODEGEN_SPEC_ARM64.md` for the
backend reference.
