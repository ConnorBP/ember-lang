# ember - x86-64 codegen spec

Detail doc for DESIGN.md Section 3/Section 6. This is the "prove we can actually
compile to native machine code" layer. Target: Windows x64 first
(workspace is Windows-centric); SysV V1.1 port noted where it differs.

## 1. Calling convention (host boundary): Win64 fastcall

- Integer/pointer args 1-4: `rcx, rdx, r8, r9`. Args 5+: stack, right
  to left, each slot 8 bytes.
- Float/double args: `xmm0-xmm3`, positionally parallel with the
  integer slots (i.e. arg 2 being a float still consumes the "slot 2"
  position - it just uses xmm1 instead of rdx, but if arg1 is int and
  arg2 is float, arg2 uses xmm1 not xmm0). This is the real Win64 rule
  and it's a common bug source - codegen must track slot index
  separately from "which register class."
- Return: integer/pointer in `rax`, float/double in `xmm0`. Struct
  return >8 bytes: caller passes hidden pointer as first arg (in
  `rcx`), callee writes through it, `rax` also returns that pointer.
  Struct return <=8 bytes and POD: returned directly in `rax` (packed).
- Caller must reserve 32 bytes of "shadow space" below its call's
  stack args, always, even if callee takes <4 args - required by the
  ABI so callee can spill register args there if needed.
- Stack must be 16-byte aligned at the point of `call`. Since `call`
  pushes an 8-byte return address, the callee sees `rsp % 16 == 8`
  immediately after entry.
- Caller-saved (scratch, callee may clobber): `rax, rcx, rdx, r8, r9,
  r10, r11`, `xmm0-xmm5`.
- Callee-saved (must preserve if used): `rbx, rbp, rsi, rdi, r12-r15`,
  `xmm6-xmm15`. Our codegen prefers caller-saved regs for temporaries
  precisely to avoid save/restore overhead; callee-saved regs are only
  allocated under register pressure (see Section 5).

Script-to-script calls do **not** need to follow this convention
exactly since both sides are our own codegen - but v1 makes
script-to-script calls use the *same* convention as script-to-native
so there is exactly one calling convention in the whole system. No
"internal fastpath ABI" - simpler, and one indirect call is already
cheap. Revisit only if benchmarks show the extra shadow-space
reservation matters (YAGNI).

## 2. Stack frame layout

```
higher addresses
+--------------------------+
| shadow space (32B, for   |   <- rsp+0..31 at time of any CALL this fn makes
| calls *this* fn makes)   |
+--------------------------+
| outgoing stack args      |   (if this fn calls something with >4 args)
+--------------------------+
| local variable slots     |   (spills, fixed arrays, struct locals)
+--------------------------+
| saved callee-saved regs  |   (only ones actually used)
+--------------------------+
| [rbp]  <- rbp points here|
+--------------------------+
| return address           |   (pushed by CALL instruction)
+--------------------------+
lower addresses (this is where rsp sits after prologue)
```

- Prologue: `push rbp; mov rbp, rsp; sub rsp, FRAME_SIZE`.
- `FRAME_SIZE` = round_up_16(callee_saved_bytes + locals_bytes +
  outgoing_args_bytes + 32). The +32 is this function's own shadow
  space for calls *it* makes (only reserved if the function actually
  calls anything; leaf functions that make zero calls skip it).
- Epilogue: `mov rsp, rbp; pop rbp; ret`. (Callee-saved reg restores
  happen before this, popped in reverse push order.)
- Incoming register args are moved into the frame's arg-vreg storage
  at function entry if they need to survive a call (register
  allocator decides - see Section 5); otherwise they stay live in their
  incoming register until first clobbered.
- Frame pointer (`rbp`) is always used in v1, even though it costs a
  register - makes stack unwinding for debug/exception reporting
  trivial and stack-depth-guard bookkeeping simple. Omit-frame-pointer
  optimization is a post-v1 concern (YAGNI, no evidence it matters
  yet).

## 3. Instruction encoding reference

REX prefix: `0100WRXB` - `W`=1 selects 64-bit operand size, `R`
extends ModRM.reg, `X` extends SIB.index, `B` extends
ModRM.rm/SIB.base. Omit REX entirely when W=0 and no extended (r8-r15)
registers are used and operand is 32-bit (default size).

ModRM: `mod(2) reg(3) rm(3)`. `mod=11` = register-direct. `mod=00/01/10`
= memory with 0/1-byte-disp/4-byte-disp, `rm=100` means SIB follows,
`rm=101` with `mod=00` means RIP-relative disp32 (no SIB).

SIB: `scale(2) index(3) base(3)`.

Concrete byte sequences (all Win64, `rax`=0 `rcx`=1 `rdx`=2 `rbx`=3
`rsp`=4 `rbp`=5 `rsi`=6 `rdi`=7 `r8`=8..`r15`=15):

| Instruction | Encoding |
|---|---|
| `mov r64, imm64` | `REX.W+B8+rd io`  -  `48 B8+r <imm64 8 bytes>` |
| `mov r64, imm32` (sign-extend, when it fits) | `REX.W C7 /0 id`  -  `48 C7 C0+r <imm32>` |
| `mov r32, imm32` | `B8+rd id` (no REX needed if r<8) |
| `mov r64, r64` | `REX.W 89 /r`  -  `48 89 <ModRM: mod=11 reg=src rm=dst>` |
| `mov r64, [mem]` | `REX.W 8B /r` |
| `mov [mem], r64` | `REX.W 89 /r` |
| `lea r64, [mem]` | `REX.W 8D /r` |
| `add r64, r64` | `REX.W 01 /r` (`01` = add r/m,reg  -  dest is r/m) |
| `add r64, imm32` | `REX.W 81 /0 id` |
| `sub r64, r64` | `REX.W 29 /r` |
| `sub r64, imm32` | `REX.W 81 /5 id` |
| `imul r64, r64` | `REX.W 0F AF /r` (dest = reg, reads reg*r/m) |
| `idiv r64` (rax:rdx / r64) | `REX.W F7 /7`  -  must `cqo` first to sign-extend rax into rdx |
| `and/or/xor r64,r64` | `REX.W 21/09/31 /r` |
| `not r64` | `REX.W F7 /2` |
| `neg r64` | `REX.W F7 /3` |
| `shl/shr/sar r64, imm8` | `REX.W C1 /4,/5,/7 ib` |
| `cmp r64, r64` | `REX.W 39 /r` |
| `cmp r64, imm32` | `REX.W 81 /7 id` |
| `test r64, r64` | `REX.W 85 /r` |
| `cqo` | `REX.W 99` |
| `push r64` | `50+rd` (REX.B if r>=8) |
| `pop r64` | `58+rd` |
| `jmp rel32` | `E9 <rel32>` |
| `jmp rel8` | `EB <rel8>` |
| `jcc rel32` (e.g. `je`) | `0F 8x <rel32>`  -  `84`=je/jz, `85`=jne/jnz, `8C`=jl, `8D`=jge, `8E`=jle, `8F`=jg, `82`=jb/jae-complement, `83`=jae, `86`=jbe, `87`=ja |
| `jcc rel8` | `7x <rel8>`  -  same condition codes, one-byte form (`74`=je etc.) |
| `call rel32` | `E8 <rel32>`  -  only used for calls to code inside our own JIT arena (script-to-script direct, rare  -  see Section 7, we prefer indirect) |
| `call r/m64` | `FF /2`  -  indirect call, register or `[mem]` operand  -  this is the *primary* call form (dispatch table, native fn via register) |
| `ret` | `C3` |
| `nop` | `90` |
| `int3` | `CC` (used for trap stubs / debug breaks) |
| `setcc r8` | `0F 9x /r` zero-extend result with subsequent `movzx` if needed |
| `movzx r64, r8/r16/r32` | `REX.W 0F B6/B7 /r` (r32 src doesn't need movzx, mov does it: writing r32 zero-extends to r64 automatically per x86-64 rule) |
| `movsx r64, r8/r16/r32` | `REX.W 0F BE/BF /r` (r8/r16), `REX.W 63 /r` (r32->r64) |

SSE scalar float subset:

| Instruction | Encoding |
|---|---|
| `movss xmm, xmm/mem` | `F3 0F 10 /r` |
| `movss mem, xmm` | `F3 0F 11 /r` |
| `movsd xmm, xmm/mem` | `F2 0F 10 /r` |
| `movsd mem, xmm` | `F2 0F 11 /r` |
| `addss/subss/mulss/divss` | `F3 0F 58/5C/59/5E /r` |
| `addsd/subsd/mulsd/divsd` | `F2 0F 58/5C/59/5E /r` |
| `ucomiss xmm,xmm` | `0F 2E /r` (sets ZF/PF/CF like integer cmp, for float compares) |
| `ucomisd xmm,xmm` | `66 0F 2E /r` |
| `cvtsi2sd xmm, r64` | `F2 REX.W 0F 2A /r` |
| `cvtsi2ss xmm, r64` | `F3 REX.W 0F 2A /r` |
| `cvttsd2si r64, xmm` | `F2 REX.W 0F 2C /r` (truncating float->int, needed for explicit casts) |
| `cvtss2sd` / `cvtsd2ss` | `F3 0F 5A /r` / `F2 0F 5A /r` (f32<->f64 conversion) |
| `pxor xmm,xmm` (zero a float reg) | `66 0F EF /r` |
| `xorps xmm,xmm` (also zeroing, no prefix needed) | `0F 57 /r` |

XMM registers with index >=8 (xmm8-xmm15) need `REX.R`/`REX.B` same as
GP regs - encoder must handle this uniformly (register number is
always 0-15 regardless of class, REX bit logic is identical).

Immediate sign/zero rules the encoder must get right (common bug
source):
- `mov r64, imm32` via `C7` sign-extends the imm32 to 64 bits. If the
  constant is a large unsigned 32-bit value intended to stay positive
  in 64-bit (e.g. `0xFFFFFFFF` meant as `4294967295` not `-1`), must
  use the 10-byte `mov r64,imm64` (`B8`) form instead, or `mov r32,imm32`
  (implicit zero-extend to r64) if the destination's upper 32 bits
  being zero is acceptable. Encoder picks the shortest *correct*
  encoding: if imm fits signed-32 and sign-extension gives the right
  64-bit value, use `81/C7`; if it fits unsigned-32, use the 32-bit
  `mov` form (zero-extends); otherwise full 64-bit immediate.

## 4. Label / patch system

Two-pass-per-function emission (not two passes over the whole module  - 
each function's code is self-contained and emitted independently, so
functions compile in isolation except for the dispatch-table slot
indirection):

```cpp
struct Label { uint32_t id; };

struct PendingFixup {
    uint32_t code_offset;   // offset of the byte AFTER which the rel32/rel8 lives
    uint32_t label_id;
    enum { REL8, REL32 } kind;
};

struct RipFixup {
    uint32_t code_offset;   // offset of the disp32 field
    uint32_t data_offset;   // offset into this function's rodata blob (constants)
};
```

- `Label alloc_label()` - reserves an id, no address yet.
- `void bind(Label)` - records `label_id -> current code_offset` in a
  resolved map. A label may be bound exactly once; binding twice or
  never (dangling forward ref) is a **codegen-internal error** (assert
  / throw `InternalCompilerError`, never silently emit garbage - this
  indicates a lowering bug, not a script bug).
- Branch-emitting methods (`jmp(Label)`, `jcc(Label)`) always emit the
  **rel32 form** during the first pass (never try to guess rel8 up
  front - see below), record a `PendingFixup{REL32}`, and emit 4 zero
  placeholder bytes.
- After the whole function body is emitted, a `resolve_fixups()` pass
  walks all `PendingFixup`s: looks up `label_id` in the resolved map
  (if missing -> InternalCompilerError, a label was referenced but
  never bound), computes `rel32 = target_offset - (fixup_offset + 4)`,
  writes it in place (little-endian).
- **rel8 shrink pass (optional, v1 skips it):** since every branch
  starts as rel32 (5 or 6 bytes with the 0F prefix), code is
  correct but not size-minimal. A shrink-to-rel8 peephole pass is
  explicitly deferred - see DESIGN.md Section 10 YAGNI list extension below;
  correctness first, this only affects icache footprint at the margin
  and is trivial to bolt on later once benchmarks (v0.5) show it
  matters.
- RIP-relative data references (float/double constants that don't fit
  in an immediate, and large 64-bit integer constants that also can't
  be broken into a `movabs`-equivalent-in-hot-path cheaply): each
  function carries a small trailing rodata blob appended after its
  code bytes in the same exec-memory allocation, at a *known offset*
  computed once total code size is known. `RipFixup` entries get their
  disp32 patched the same pass as branches: `disp32 = (code_base +
  data_offset) - (code_base + code_offset + 4)`. Because code and data
  live in the same allocation, this is a fixed small computation, no
  absolute relocation needed even though it crosses "sections."
- **Cross-function references** (calling another script function
  directly by rel32 `call`, or referencing another function's rodata)
  are explicitly **not supported** - this is why script-to-script
  calls always go through the dispatch table (Section 7) via an *absolute*
  address loaded into a register, never a same-image rel32 `call`.
  This sidesteps the entire "what if the target moves during hot
  reload / what if functions are compiled in different exec-memory
  chunks" problem class. One rule, no exceptions, easy to reason about.

## 5. Deferred register allocation design: linear scan on SSA-lite IR

> **Not implemented in v1.0.** The shipped backend walks the AST directly and
> stack-spills expression intermediates. Everything in this section is a future,
> benchmark-gated design, not a description of current codegen.

"SSA-lite" (see COMPILER_PIPELINE.md Section 5 for the deferred IR definition): each
IR value is assigned exactly once, values are typed, control flow is
basic blocks with explicit terminators, but there are **no phi
nodes** - instead, values that are live across a block boundary from
multiple predecessors are pre-assigned a stable **frame slot** (not a
register) by the lowering pass whenever a variable is reassigned inside
a loop body or an if/else arm (i.e. anything that in real SSA would
need a phi becomes a stack slot with explicit stores/loads at each
assignment site). This trades a small amount of load/store traffic for
completely eliminating phi-resolution/parallel-copy complexity in
codegen - acceptable per the "correctness and simplicity over
micro-optimization in v1" stance; revisit only if profiling shows
loop-carried variables are a hot spot (unlikely - they'll live in a
register across the loop body itself via normal linear-scan liveness,
this only affects the merge point bookkeeping).

Algorithm (classic Poletto & Sarkar linear scan, scoped per function):
1. Number all IR instructions linearly in emission order (one number
   per instruction across all basic blocks, in the block layout order
   chosen by lowering - blocks are laid out in source order with
   `if`/`while` bodies inline and `else`/loop-exit as forward jumps;
   no block reordering in v1).
2. Compute live intervals per virtual register: `[first_def,
   last_use]`, from a single backward liveness pass over the
   instruction-numbered IR (using per-block live-in/live-out sets,
   standard dataflow fixed point - loops need this to run to a fixed
   point since back-edges make it non-trivially single-pass, but
   function bodies are small so this is cheap).
3. Two disjoint physical register pools:
   - **GP pool** (int/pointer/bool values): caller-saved order
     preferred first - `rax(skip, return value), r10, r11, rdx, r8,
     r9, rcx` (careful: `rcx/rdx/r8/r9` also hold incoming args - the
     allocator's initial state pre-binds arg vregs to their incoming
     register per the calling convention, then those registers become
     free for reuse once the arg's live interval ends), then
     callee-saved `rbx, rsi, rdi, r12-r15` if caller-saved pool is
     exhausted (spilling a callee-saved reg costs one extra
     push/pop pair in prologue/epilogue, tracked once per function not
     per use).
   - **XMM pool**: `xmm0-xmm5` (caller-saved) first, `xmm6-xmm15`
     (callee-saved) under pressure.
   - `rsp`/`rbp` never enter the allocatable pool.
4. Active-interval-list sweep: process intervals sorted by start
   point; when starting a new interval, if a free physical register
   exists in the right pool, assign it; otherwise **spill** - choose
   the active interval with the *furthest remaining last-use* (classic
   linear-scan spill heuristic - minimizes reload count) and move it
   to a frame slot, freeing its register for the new interval.
5. Spilled values: every use becomes an explicit load from its frame
   slot into a scratch register immediately before use, and every def
   becomes an explicit store immediately after def. No "spill live
   range splitting" (an interval, once spilled, stays spilled for its
   whole remaining lifetime in v1) - simpler, and register pressure in
   small game-script functions is expected to be low enough that
   spilling is rare in practice; if benchmarks show otherwise, revisit.
6. **Calls clobber all caller-saved registers.** Any interval that is
   live across a call instruction and currently assigned a
   caller-saved register must be spilled around that call (store
   before, reload after) *or* pre-assigned a callee-saved register if
   available - allocator prefers callee-saved assignment for intervals
   whose liveness span contains >=1 call, decided during the initial
   pool-choice step (a cheap pre-pass over intervals: mark
   "crosses-a-call" before the main sweep, bias pool order for those).
7. **Struct-by-value temporaries** (struct literal being constructed,
   or a struct arg being prepared for a call) don't live in a single
   register - they get a dedicated frame slot for their whole
   lifetime unconditionally (never register-candidates). Field
   writes/reads during construction are direct stores/loads to that
   slot at known offsets (struct layout from TYPE_SYSTEM.md Section 2).

Edge cases:
- **Zero live registers available in a pool and no spill candidate
  exists (impossible in practice given 14 GP + rsp/rbp excluded, but
  guarded anyway):** InternalCompilerError, not UB.
- **Function has more simultaneously-live values than
  registers+reasonable stack** - not a hard limit in v1 (frame size
  just grows), but flag as future work if a script triggers pathological
  frame sizes; add a compile-time frame-size sanity cap (e.g. 64KB) that
  turns into a compile *error* (not a crash) if exceeded, since that's
  almost certainly a compiler bug or a genuinely absurd function rather
  than legitimate script code.
- **Loop-carried spilled variable inside a hot inner loop**: accepted
  cost in v1 (see SSA-lite tradeoff above).

## 6. Prologue/epilogue generation detail

1. Compute `used_callee_saved` (from regalloc output) and
   `locals_bytes` (from frame slot count * 8, or larger for
   struct/array slots per their actual size, aligned to their natural
   alignment) and `outgoing_args_bytes` (max over all call sites in
   this function of `max(0, arg_count-4)*8`) and `makes_any_call`
   (bool).
2. `FRAME_SIZE = round_up_16(locals_bytes + outgoing_args_bytes +
   (makes_any_call ? 32 : 0))`.
3. Emit: `push rbp` / `mov rbp, rsp` / for each reg in
   `used_callee_saved`: `push reg` / `sub rsp, FRAME_SIZE`.
   (Pushes happen *before* the `sub` so their bytes are accounted
   for in 16-byte alignment math: total post-prologue adjustment =
   `8 (rbp push) + 8*len(used_callee_saved) + FRAME_SIZE`, and since
   `call` left rsp at `%16==8`, then `push rbp` makes it `%16==0`;
   each further `push` flips parity again - so `used_callee_saved`
   count parity must be folded into `FRAME_SIZE`'s rounding, i.e.
   compute `FRAME_SIZE` to make the *total* frame including the
   callee-saved pushes land back on 16-byte alignment, not just
   `locals+args`.)
4. Move incoming register args that the allocator decided must live in
   a frame slot (spilled from the start, e.g. because the function has
   more live args than free registers, or an arg is address-taken)  - 
   store them right after the prologue, before any other body code.
5. Epilogue (one per `return` statement - v1 does not do a single
   shared epilogue with jumps-to-exit; each `return` emits its own
   full epilogue inline, which is simpler and fine since script
   functions are small; revisit only if code-size benchmarks demand a
   shared epilogue tail): move return value into `rax`/`xmm0` per
   convention, `pop` callee-saved regs in reverse push order, `mov
   rsp, rbp` (this also drops FRAME_SIZE, no separate `add rsp` needed),
   `pop rbp`, `ret`.
6. **Function with no explicit trailing `return` and non-void return
   type**: sema-level error (COMPILER_PIPELINE.md Section 4), never a codegen
   concern - codegen can assume every path either returns or the
   function is void (falls off the end -> implicit `ret` with
   whatever garbage is in rax, which is fine because sema guarantees
   this only happens for void functions).

## 7. Script-to-script calls (dispatch table)

- Every script function has a stable slot index assigned at compile
  time (first-compile order; stable across recompiles of *other*
  functions in the same module - only the reloaded function's own
  slot content changes, indices never get reassigned, see
  HOT_RELOAD.md).
- Dispatch table is a flat `void*[]` (or `std::atomic<void*>[]` for the
  reload-safety story - see HOT_RELOAD.md Section 5) owned by the `module_t`.
- Call sequence emitted at every script-to-script call site:
  ```
  mov  r11, [dispatch_table_base + slot*8]   ; absolute addr of table, baked in
                                              ; as a 64-bit immediate at compile time
  mov  <arg regs per convention...>
  call r11
  ```
  `dispatch_table_base` is an absolute pointer, embedded as an
  immediate (`mov r11, imm64` = the `48 BB` form using r11's encoding,
  or loaded via a RIP-relative rodata slot if we want relocatable
  code later - v1 uses the plain 10-byte immediate form since exec
  memory is never persisted/relocated, see MEMORY_AND_GC.md, so a
  baked absolute address is safe for the module's lifetime). Using
  `[dispatch_table_base + slot*8]` as a *memory operand directly* in
  the `call` (`call [imm64 + slot*8]`, i.e. skip the intermediate mov
  into r11 and just do `call [rXX]` with rXX holding the table base
  plus offset folded via addressing mode, or even a single `FF 14 25
  <disp32>` absolute-disp32 form if the dispatch table's address is
  low enough - not guaranteed on 64-bit, so the two-instruction form
  above is the portable baseline) is a valid micro-optimization for
  later; v1 keeps the two-instruction form for clarity and
  correctness first.
- This means a script-to-script call is **one absolute-address load +
  one indirect call** - no rel32 patching, no cross-function code
  dependency, and hot-reloading any function (including the callee)
  never requires touching the caller's code bytes at all. This is the
  entire point of the indirection (DESIGN.md Section 6).
- **Recursive calls** (function calling itself) go through the same
  dispatch-table indirection, not a direct rel32 self-call - keeps the
  rule uniform (a function can be hot-reloaded while it's on the call
  stack recursing; see HOT_RELOAD.md Section 4 for what that means for
  in-flight frames).
- **Forward-referenced functions** (calling a function declared later
  in the source, or not yet compiled) are resolved by slot index at
  sema time (all function signatures are registered in a first pass
  before any function body is lowered - see COMPILER_PIPELINE.md Section 3),
  so codegen never needs a "function not compiled yet" placeholder
  state; the slot exists (possibly still pointing at a "trap: call to
  uncompiled function" stub - see edge case below) before any caller
  is codegen'd.
- **Edge case - call to a function whose compilation failed / is
  mid-recompile:** slot points at a shared trap stub
  (`ember_trap_uncompiled`) that raises a host-visible runtime error
  (SAFETY_AND_SANDBOX.md Section 7) rather than jumping through a null/garbage
  pointer. Slots are never literally null during normal operation.

## 8. Script-to-native calls

- Native function pointer is known at registration time and does not
  move (it's a real C++ function in the host binary) - so unlike
  script-to-script calls, codegen embeds the pointer as a plain
  64-bit immediate, no table indirection needed (there's nothing to
  hot-reload on the native side within a script-engine session).
  ```
  mov  rax, imm64        ; native fn pointer, 48 B8 <8 bytes>
  mov  <arg regs...>     ; per Win64 fastcall, exactly as if C++ called it
  call rax
  ```
- Argument marshalling: primitives map directly to their CC slot (int
  -> GP reg or stack, float -> xmm reg or stack) with **zero
  conversion** - this is the whole performance point vs AngelScript's
  generic marshalling layer. Slices (ptr+len) are passed as **two
  consecutive argument slots** (pointer in one GP reg/stack slot,
  length as i64 in the next slot) - i.e. from the native function's
  perspective a `Slice<T>` script param is exactly `(T* ptr, int64_t
  len)`, two normal C ABI args; the native-side binding signature
  documents this explicitly (BINDING_API.md Section 6).
- Native struct-by-value arguments are supported only when the tightly packed
  Ember aggregate is at most 8 bytes. Sema rejects larger native aggregate
  arguments before codegen; a correct Win64 indirect-by-value implementation
  for those shapes is deferred.
- Return value: `rax`/`xmm0` per convention; struct return >8 bytes
  uses the hidden-pointer-return convention (Section 1) - caller allocates
  the destination slot (a local, or the caller's own struct-return
  slot if tail-returning it), passes its address as the hidden first
  arg.
- **Permission check (`PERM_FFI`)**: emitted as compile-time
  gating, not a runtime check - if the native function being called
  doesn't have `PERM_FFI` granted for the calling module (or is
  unregistered entirely), sema rejects the call at compile time
  (SAFETY_AND_SANDBOX.md Section 6), so by the time codegen sees the call it
  is already known-permitted; no runtime permission branch is ever
  emitted (zero-cost - this is *why* it's a compile-time gate and not
  a runtime flag check).
- **Varargs / variable-arity native functions**: not supported in v1
  - every `NativeFn` has a fixed `param_count` (BINDING_API.md Section 1),
  script call sites must match arity exactly (checked at sema, same as
  any other function call type-check). If a host wants a variadic-like
  API, it registers multiple fixed-arity overloads or takes an
  explicit slice arg.

## 9. Bounds check emission

For `slice[i]` or fixed-array `arr[i]` where `i` is a runtime value
(constant indices into fixed arrays get bounds-checked at *compile*
time instead - see edge case below):

```
cmp  i_reg, len_reg      ; unsigned compare (indices are never negative
                         ; in the type system - see TYPE_SYSTEM.md Section 7 -
                         ; so this is a plain unsigned cmp, not two
                         ; checks for <0 and >=len)
jae  .oob_trap           ; jump if i >= len (unsigned "above or equal")
<computed address load: base + i*elem_size (+ elem_size via lea with
 scale if elem_size is 1/2/4/8, else imul then add)>
mov/movss/movsd  dst, [addr]     ; or store form for assignment targets
...
jmp  .after
.oob_trap:
  call [ember_trap_bounds_slot]   ; shared per-module trap stub, see below
  ; unreachable after this (trap stub does not return - see below)
.after:
```

- The `.oob_trap` stub is **one shared stub per module** (not
  duplicated per call site) - call sites jump/call into it with the
  failing index and a small "site id" or (file,line) constant already
  loaded into fixed scratch registers (e.g. `r10`=index, `r11`=site
  info pointer) before the jump, so the stub can report a useful
  error without each call site needing its own error-reporting code
  (keeps code size down, matches "hundreds of bounds checks in a hot
  loop shouldn't bloat icache" goal).
- The trap stub calls into the host runtime error path
  (SAFETY_AND_SANDBOX.md Section 7) which by policy **does not return**  - 
  it unwinds the current `context_t`'s execution back to the
  host-managed checkpoint via `__builtin_longjmp` (the raw B1 thunks do
  not create that checkpoint; see SAFETY_AND_SANDBOX.md Section 2) - so the
  `jae .oob_trap` path never needs a "return and propagate an error
  code up through every caller" plumbing story. This is deliberate:
  checked-error propagation through every stack frame would cost a
  branch on every call return; a non-local jump on the *rare* failure
  path is free on the fast path.
- **Constant index into a fixed-size array** (`arr[3]` where array
  size is known at compile time and index is a literal): sema/lowering
  checks `3 < array_len` at compile time; if false, **compile error**,
  not a runtime trap - no code is emitted for this check at all in the
  passing case (zero runtime cost, matches goal of bounds checks being
  "cheap" - the cheapest possible bounds check is the one done at
  compile time).
- **Constant index into a slice**: slices never have a compile-time-known
  length (length is a runtime field, even if the pointer/data is
  compile-time known) - always emits the runtime check. This is a
  necessary consequence of what a slice *is* (TYPE_SYSTEM.md Section 4); no
  way around it, and it's still just one cheap predictable-branch
  compare.
- **Negative index**: the language has no signed array-index type
  reaching this code path - see TYPE_SYSTEM.md Section 7 for how index
  expressions are constrained to unsigned integer types at the type
  level, so "negative index" is a *type error at sema*, not a runtime
  case codegen has to handle. (A script author computing an
  out-of-range unsigned value via underflow, e.g. `i - 1` where `i ==
  0`, still hits the normal `jae` bounds trap - that's correct
  behavior, not a special case.)
- **Zero-length slice**: `cmp i, 0; jae .oob_trap` - any index at all
  traps, which is correct (there is nothing to index).

## 10. Integer division / overflow traps

- `idiv`/`div` by a runtime value: emit `test divisor, divisor; je
  .divzero_trap` immediately before the `cqo`+`idiv` sequence, in both
  debug and release builds - **divide-by-zero is always checked**,
  never gated behind the debug/release overflow flag (SAFETY_AND_SANDBOX.md
  Section 5), because an unchecked `idiv` by zero raises a hardware `#DE`
  exception that would otherwise have to be caught via an OS-level
  SEH/signal handler - far more expensive and fragile than one `test`+
  `je` per division. Divisor-is-constant-nonzero at compile time
  (literal divisor) skips the check entirely (same "cheapest check is
  compile-time" principle as bounds).
- `INT64_MIN / -1` (the one case that overflows a *signed* division
  and also raises `#DE` on real hardware, distinct from div-by-zero):
  checked explicitly when the divisor is a runtime value and could be
  -1 (i.e. always, unless divisor is a non-(-1) compile-time constant):
  `cmp divisor, -1; jne .safe; cmp dividend, INT64_MIN; jne .safe; jmp
  .overflow_trap; .safe: <idiv>`. Same shared-trap-stub pattern as
  bounds checks.
- Signed integer add/sub/mul overflow: **checked only in debug builds**
  (compile flag, per DESIGN.md Section 5) via `jo`/`jc` immediately after the
  arithmetic op (x86 sets OF on signed overflow for
  add/sub/imul-single-operand-form automatically - zero extra
  instructions needed beyond the conditional jump, `jo .overflow_trap`
  right after the `add`/`sub`, and for `imul` the two/three-operand
  forms also set OF correctly). Release builds skip the `jo` entirely
  - pure wraparound, matches documented policy.
- Unsigned overflow: never trapped (wraparound is defined behavior for
  unsigned types in this language, matching C semantics for `u*`
  types) - no check emitted in either build mode.

## 11. Function-call site argument spilling for >4 args

When a script or native call has more than 4 total argument slots
(counting both int and float args against the same positional slot
count, per Section 1's slot-parallel rule):
- Args 5+ are written directly to their stack slot in the *caller's*
  outgoing-args area (Section 2), each a plain `mov [rsp+offset], reg`, no
  extra shadow-space-relative addressing subtlety since the outgoing
  args area is laid out contiguously below this call's required 32-byte
  shadow space, at a fixed known offset computed at frame-layout time
  (Section 6 step 1).
- No dynamic stack pointer adjustment at the call site itself (no
  `push`-per-arg) - the space was already reserved in `FRAME_SIZE` up
  front, keeping `rsp` stable for the whole function body, which is
  what makes frame-slot addressing (`[rbp - k]`) valid everywhere
  without tracking a moving `rsp` offset per instruction.

## 12. `switch` lowering

Two strategies, chosen per-switch at lowering time based on case-label
density (`COMPILER_PIPELINE.md` Section 6 / `TYPE_SYSTEM.md` Section 11.6):

**A) Jump table** (dense case set: `max_label - min_label < ~2 × count`):
- Emit a rodata blob (this function's trailing rodata, Section 4's `RipFixup`
  target) holding an array of rel32 offsets, one per value in
  `[min_label, max_label]`, each pointing at the corresponding
  case-body label (or at `switch_exit_bb` for the gaps).
- Scrutinee normalization: `sub scrutinee, min_label` (unsigned),
  `cmp` against `count`, `jae switch_exit_bb` (out of range falls
  through to default/exit - same as no-case-matched).
- Indirect jump: `lea rXX, [rip + table_base]`, `mov rYY, [rXX +
  normalized*4]`, `add rYY, rXX` (rel32 is relative to the *load*
  instruction's end, so add the table base back), `jmp rYY`.
  - Encoding: the `lea`/`mov`/`add`/`jmp` sequence is 4 instructions;
  an alternative is `jmp [rip + table_base + normalized*4]` directly
  (absolute-disp32 indirect jump, `FF 25 <disp32>`) when the table
  fits in 32-bit disp range from the jmp site - always true for
  in-arena rodata since the whole arena is within one allocation
  region. **v1 uses the `FF 25` form** (single indirect jmp) when
  the table is RIP-reachable in 32 bits (always, in-arena), else the
  lea/mov/add/jmp form. One-instruction fast path on the hot case.

**B) Cmp/je cascade** (sparse case set):
- One `cmp scrutinee, label_imm` / `je case_body_label` per case, in
  source order. Default (or fall-through-to-exit) is the implicit
  "no match" path at the end. Cheap to emit, slower at runtime for
  many cases - chosen only when the jump table would be mostly gaps.

**Case body `break`**: lowers to `jmp switch_exit_bb` (not a loop
`break` - `break` inside a `switch` breaks the switch, not any
enclosing loop; an enclosing loop's `break` must be reached via control
flow outside the switch, which is unambiguous from the AST nesting).

**No-fallthrough guarantee** (TYPE_SYSTEM.md Section 11.6): sema rejects a
case body that falls off its end without `break`/`return`/
`continue`/trap - so lowering never emits a fall-through-to-next-case
path; each case body ends with an explicit terminator jumping to
`switch_exit_bb` (or returning/trapping). This is what lets us skip
the entire Duff-device-style fallthrough machinery C requires.

## 13. `defer` emission

The implemented v1 shape is deliberately narrower than lexical RAII:
codegen gathers deferred expressions at function level and emits them LIFO at
ordinary function exit. Nested block fallthrough and loop `break`/`continue`
do not trigger cleanup, and trap unwind bypasses defers. A defer in a loop is
therefore emitted at most once by the current static lowering. Sema restricts
references to parameters and globals so values remain addressable at function
exit. Per-block lists and lexical cleanup-edge architecture are deferred; they
must not be assumed by hosts or scripts.

## 14. What "prove it compiles" means for v0.1 (acceptance criteria)

Concrete, testable claims the v0.1 milestone (DESIGN.md Section 9) must
satisfy before moving on, restated here in codegen terms:
1. A hand-built IR for `fn add(a: i64, b: i64) -> i64 { return a + b;
   }` produces a byte sequence that, when copied into an
   executable page and cast to `int64_t(*)(int64_t,int64_t)` and
   called with `(3, 4)` via the *real* Win64 CC (i.e. called directly
   from C++, no wrapper), returns `7`.
2. The same for an `f64` version using `xmm0`/`xmm1` and `addsd`,
   proving the float register/convention path independently from the
   int path.
3. A function with a forward `jmp`/`jcc` over an `if` with no `else`
   (label bound *after* the branch is emitted) round-trips correctly
   - proves the two-pass fixup system (Section 4) actually works, not just
   the straight-line case.
4. A function that needs to spill (contrived: many simultaneously
   live locals in one expression) still produces correct results  - 
   proves Section 5's spill path, not just the happy "everything fits in
   registers" case.
5. A recursive function (e.g. naive fibonacci) compiled through the
   dispatch-table call path (Section 7) and invoked from the host produces
   the correct result - proves indirection works for the harder
   "callee is also the caller" case before relying on it for hot
   reload later.

These five are the actual measurable definition of "fully compile our
scripts into actual x86-64" for the purposes of this pass; anything
not listed here (structs, slices, natives) is proven in later
milestones per DESIGN.md Section 9, using the same encoder and fixup
machinery specified above (no new mechanism needed for those, only
new IR shapes lowering into the instructions already defined in Section 3).

The shipped runner also executes the expanded criteria in Section 15
(full-ISA byte-exact coverage + `.em` round-trip), added with the
`.em` bundling format between v0.1 and v0.2.

## 15. v0.1 acceptance suite (expanded)

The original v0.1 gate is Section 14 (five criteria). Two further tests
shipped with the `.em` bundling format and the full-encoder coverage
proof, between v0.1 and v0.2. They are recorded here rather than
folded into Section 14 so Section 14 stays an accurate historical claim about the
original gate.

### 15.6 Full Section 3 ISA byte-exact encoder coverage

Emits every instruction in the Section 3 subset once and asserts the
produced bytes match the spec's reference encodings exactly. Covers
the GP-integer table (mov r64/imm64, imm32 sign-extended, imm32
zero-extended, the smart shortest-form encoder, mov r,r, mov r↔mem,
add/sub/imul, cqo/idiv, and/or/xor, not/neg, shl/shr/sar, cmp/test,
push/pop, setcc, movzx/movsx/movsxd), the control-flow table
(jmp/jcc/call via label rel32 fixups, call r11, ret, nop), and the
SSE-scalar-double table (movsd x/x and x↔mem, addsd/subsd/mulsd/divsd,
comisd, cvtsi2sd r64→x, cvttsd2si x→r64, pxor). The expected byte
vector is built independently of the encoder and compared for
equality; on mismatch the runner dumps both sequences and the first
differing byte. This proves the whole encoder is correct, not just
the subset the five Section 14 tests touch.

### 15.7 `.em` serialize → load → run round-trip

Proves the bundling path (`BUNDLING_AND_EM_MODULES.md` Section 2): a
JIT-built function is serialized to a `.em` file, reloaded, and the
loaded code runs identically to the JIT'd version - one execution
path, no interpreter fallback.

Concretely: build fib with the external-reloc encoder form
(`mov_r_imm64_external`, Section 2.4 - emits the same `REX.W + 0xB8+r +
imm64` bytes but with zero placeholder imm64 bytes and an
`AbsFixup{kind=DispatchTableBase}` recorded on the label/patch
system). Capture the post-`resolve_fixups` bytes *before* publishing
the exec page (the serializer needs a clean copy). Build an
`EmModule` (one function record: fib, slot 0, the captured code,
empty rodata, relocs from `lp.abs_fixups()`), `write_em_file` it to
`fib_roundtrip.em`, `load_em_file` it back into a `LoadedModule`.
The loader allocates the loaded module's own dispatch table and
globals block, copies the code into a fresh exec page, and repoints
each `EmReloc`'s 8-byte imm64 placeholder at the loaded module's own
dispatch-table base (kind 0) or globals base (kind 1). Call the
loaded entry as `int64_t(*)(int64_t)` and assert fib(0,1,2,3,10) ==
(0,1,1,2,55), matching the JIT'd fib.

The format contract is `src/em_file.hpp` (magic `0x454D424C` "EMBL",
version 1, 40-byte header, per-function records with code/rodata/relocs,
globals block, name directory). `EmReloc{offset, kind}` mirrors
`AbsFixup::Kind` (Section 4) so a `static_cast<uint8_t>` round-trips
losslessly through the file. v0.1 coverage is single-function (fib),
no globals, no rodata; the GlobalsBase reloc path, multi-function
dispatch tables, and rodata RIP-relative on a loaded page are
code-complete in `em_loader.cpp` but untested pending the v0.2 parser
(correct by inspection; see `BUNDLING_AND_EM_MODULES.md` Section 2.5).
