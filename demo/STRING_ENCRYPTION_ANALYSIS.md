# String Encryption — Transient-Raw-Value Analysis

**Read-only investigation.** No `src/`, `extensions/`, or `examples/` files were
modified. The only artifacts created are this doc and the probe under
`tmp_edit/enc/` (gitignored). The gate (`ctest`, 20 tests) was confirmed green
before and after; the build (`ninja -C buildt`) was "no work to do".

---

## The question (the user's hypothesis)

> Do encrypted strings get baked into bytecode in such a way that they have to
> re-build them into the stack on each use? Any string the compiler has found
> to not be modified should have this behavior so that their raw value does not
> remain in memory (is transient). Of course, non-const strings cannot have
> this guarantee and will probably need to be decoded to a more permanent place
> upon first access.

Desired property:

- **CONST string literals** (compiler can prove never modified): decrypted
  plaintext is **TRANSIENT** — rebuilt into the stack on each use, so the raw
  plaintext does NOT remain resident in memory between uses. Only the encrypted
  form lives in rodata.
- **NON-CONST strings** (may be modified/held): decode once to a more permanent
  place (heap) on first access.

---

## TL;DR — the answer

**The raw value is NOT transient today.** Ember's automatic string encryption
does *encrypt literals into rodata* and *does* re-decrypt on every use, but it
decrypts **into a heap buffer** (not onto the stack), and that buffer **leaks**.
The plaintext therefore **remains resident on the heap** for the buffer's
lifetime — exactly the opposite of the desired property.

Concretely, the probe (`tmp_edit/enc/probe.cpp`) shows:

1. `__str_decrypt` is called **once per textual use** (no caching) — so for a
   literal used twice, the plaintext is materialized **twice**, in **two
   separate heap buffers**.
2. All of those heap buffers are **still readable after the function returns**
   — nothing frees them. The plaintext is resident for as long as the host
   keeps the process alive.
3. When a literal is converted to a `string` handle via `string_from_slice`,
   the handle **COPIES** the bytes into the ext_string host store — the decrypt
   buffer is then redundant, and it *still* leaks on top of the copy.

There is also **no const-vs-non-const distinction** in the lowering: every
encrypted literal takes the same path regardless of whether the binding that
holds it is `const`. The `is_const` field exists in the AST and is enforced for
assignment rejection, but it is never consulted by the string-encryption
machinery.

This should become a tracked roadmap item — see the recommendation.

---

## The current mechanism (cited)

### 1. Encryption is a per-compile XOR, set by the host before sema

`Program::string_xor_key` (`src/ast.hpp:308`) is a single `uint8_t` the host
sets before calling `sema`. The CLI sets it to `0` (`examples/ember_cli.cpp:302`)
with an explicit comment that a nonzero key would require a `__str_decrypt`
native the standard extension set does not register:

```cpp
// examples/ember_cli.cpp:295-302
// key=0: string literals bake as raw rodata pointers (codegen's unencrypted
// branch). A nonzero key would enable encrypted-rodata codegen, which emits
// a call to __str_decrypt — a host-side obfuscation native the standard
// extension set doesn't register, so it would crash on any string literal.
// Encrypted rodata is a host opt-in (the host registers __str_decrypt and
// sets a nonzero key); a standalone language CLI has no such host, so it
// must leave encryption off.
pr.program.string_xor_key = 0;
```

So **encryption is OFF by default** and is a host-opt-in feature with **no
in-tree demonstration**. Every in-tree host that exercises the key (e.g.
`examples/em_roundtrip_test.cpp:133` sets `0xA5`,
`examples/sema_check.cpp:98` sets `0xA5`) only does so on source that contains
**no string literals**, so the encrypted-rodata codegen path is never actually
run by any checked-in test.

### 2. Sema bakes EVERY string literal as encrypted-when-key≠0, with no const gate

`src/sema.cpp:669-678` (the `StringLit` case of `check_expr`):

```cpp
e.ty = lit->ty = intern(make_slice(std::make_shared<Type>(make_prim(Prim::U8))));
uint8_t key = prog->string_xor_key;
auto enc = std::make_shared<std::string>(lit->s);
if (key != 0) {
    for (auto& c : *enc) c ^= key;
    lit->encrypted = true;
}
lit->baked_key = key;
lit->baked_ptr = reinterpret_cast<const uint8_t*>(enc->data());
lit->baked_len = int64_t(enc->size());
prog->rodata_store.push_back(std::move(enc));
```

Key facts:

- **All** string literals are encrypted when `key != 0`. There is **no**
  `is_const`/mutability test here. A `let mut s = "lit"` and a `const s = "lit"`
  bake identically.
- The `StringLit` node (`src/ast.hpp:86-92`) carries `baked_ptr` (into
  `Program::rodata_store`, the encrypted bytes), `baked_len`, `baked_key`, and
  `encrypted`.

### 3. `is_const` exists but is NOT wired into string handling

The parser produces `const`/`let`/`auto` and tracks mutability:
`src/parser.cpp:698` (`s->is_const = !is_mut;`). Sema's `Var` carries `is_const`
(`src/sema.cpp:233`) and `check_expr`'s `AssignExpr` case rejects writes to
const bindings (`src/sema.cpp:1250,1255`). The `LetStmt` path
(`src/sema.cpp:1441-...`) threads `is_const` into `declare(...)`.

**But `is_const` is used only for assignment rejection.** It is never propagated
onto the `StringLit` node, never consulted by the encryption baking, and never
read by codegen's `StringLit` case. The const-vs-non-const distinction the
user's hypothesis needs **does not exist at the string-literal lowering
layer**.

### 4. Codegen lowers every encrypted literal to a `__str_decrypt` native call

`src/codegen.cpp:2115-2165` (the `StringLit` case of `eval`):

```cpp
const uint32_t string_off = append_rodata(lit->baked_ptr, size_t(lit->baked_len));
// ...
if (lit->encrypted && lit->baked_key != 0) {
    // call __str_decrypt(enc_ptr, len, key) -> rax = decrypted_ptr
    // Win64: rcx=enc_ptr, rdx=len, r8=key; shadow space reserved.
    int32_t call_sz = 32; // shadow space for 3-arg call
    e.sub_reg_imm32(Reg::rsp, call_sz);
    e.mov_reg_imm64_external(Reg::rcx, AbsFixup::FunctionRodataBase, string_off);
    e.mov_reg_imm64(Reg::rdx, lit->baked_len);
    e.mov_reg_imm64(Reg::r8, int64_t(lit->baked_key));
    emit_counted_named_native(ctx.str_decrypt_fn, ctx.str_decrypt_name,
                              "string decrypt native");
    e.add_reg_imm32(Reg::rsp, call_sz);
    // rax = decrypted ptr (from native), rdx = len (re-derived — Win64 clobbers rdx)
    e.mov_reg_imm64(Reg::rdx, lit->baked_len);
} else {
    // unencrypted: raw pointer (backward compat / key=0)
    e.mov_reg_imm64_external(Reg::rax, AbsFixup::FunctionRodataBase, string_off);
    e.mov_reg_imm64(Reg::rdx, lit->baked_len);
}
// ... then, if implicit_to_string, a string_from_slice(ptr,len) call ...
```

The contract codegen assumes is spelled out in the comment at
`src/codegen.cpp:2117-2122`:

> The host native allocates a decrypted buffer on the heap (not in JIT exec
> memory) and returns its address; rax= decrypted ptr, rdx=len (stays the
> same). Raw strings never appear in the executable memory.

So the codegen **assumes**:

- `__str_decrypt(enc_ptr, len, key)` returns a **heap** pointer in `rax`.
- The returned pointer is then used as the slice's `ptr` (rax=ptr, rdx=len).
- For the `string`-handle path, that same ptr is immediately passed to
  `string_from_slice(ptr, len)`.

**Codegen never frees the returned pointer.** It treats it as a value that
flows into the slice ABI and (optionally) into `string_from_slice`. There is no
lifetime annotation, no `__str_decrypt_free`, no stack-alloca alternative.

### 5. `__str_decrypt` has NO in-tree implementation — it is a pure host contract

`CodeGenCtx::str_decrypt_fn` (`src/codegen.hpp:54`) is `nullptr` by default and
is **only ever read** by codegen (`src/codegen.cpp:2135`). It is set to null (or
left null) in every in-tree host (`examples/ember_cli.cpp:399`,
`examples/em_roundtrip_test.cpp:155`,
`examples/binding_abi_test.cpp:163`, etc.). `extensions/AUDIT.md:97` classifies
it as a "language-feature host contract", explicitly NOT a `NativeSig`-registered
native.

`emit_counted_named_native(ctx.str_decrypt_fn, ctx.str_decrypt_name, ...)`
(`src/codegen.cpp:2135`) calls `emit_native` (`src/codegen.cpp:90`), which —
because `__str_decrypt` is not in the `NativeSig` table — takes the
`name.empty() || !ret || !params` branch and bakes `str_decrypt_fn` as a **raw
imm64** `mov rax, ptr` then `call rax`. So the contract is: the host hands
codegen a raw C function pointer; codegen bakes a direct call to it. The
behavior (heap alloc? stack? wipe?) is **entirely the host's choice** — codegen
has no opinion beyond "rax is a ptr I'll use as the slice ptr."

### 6. `string_from_slice` COPIES, it does not alias

`extensions/string/ext_string.cpp:38-48`:

```cpp
static int64_t n_string_from_slice(uint8_t* p, int64_t len) {
    if (len < 0 || uint64_t(len) > uint64_t(MAX_STRING_BYTES) || (!p && len != 0)) return 0;
    try {
        if (len == 0) return str_new(std::string());
        return str_new(std::string(reinterpret_cast<const char*>(p), size_t(len)));
    }
    ...
}
```

`str_new` (`ext_string.cpp:24`) pushes into `g_strings`, a host-owned
`std::vector<std::string>`. The `std::string(p, len)` constructor **copies**
`len` bytes out of `p` into the new string's own storage. The decrypt buffer
`p` is not retained by the handle.

This is the decisive point for property (c) of the probe: the `string` handle's
content is **independent** of the decrypt buffer. Which means the decrypt
buffer is **dead the moment `string_from_slice` returns** — and yet nothing
frees it.

---

## The probe and its output (evidence)

Probe: `tmp_edit/enc/probe.cpp` (build: `tmp_edit/enc/probe_build.sh`,
gitignored). It mirrors `em_roundtrip_test`'s harness but:

- sets `pr.program.string_xor_key = 0xA5` (encryption ON),
- registers a real `__str_decrypt` that XOR-decrypts into a **fresh heap
  buffer per call**, keeps every buffer alive (never freed), and counts calls,
- registers `ext_string::register_natives` (so `string_from_slice` exists for
  the `let s = "lit"` implicit conversion),
- compiles a `probe()` that uses a literal **twice** as a slice and **once**
  as a `string`.

Source under test:

```
fn probe() -> i64 {
    probe_use_slice("SECRET_A", 8);   // use #1 (slice path)
    probe_use_slice("SECRET_A", 8);   // use #2 (slice path, same literal)
    let s = "SECRET_B";               // use #3 (string handle path)
    return 0;
}
```

### Observed output (`tmp_edit/enc/probe_output.txt`)

```
=== JIT run ===
[__str_decrypt] call #1: enc=0000019deea701c6 len=8 key=0xa5 -> decrypted=0000019deed27fb0 "SECRET_A"
[probe_use_slice] ptr=0000019deed27fb0 len=8 "SECRET_A"
[__str_decrypt] call #2: enc=0000019deea701ce len=8 key=0xa5 -> decrypted=0000019deed27b10 "SECRET_A"
[probe_use_slice] ptr=0000019deed27b10 len=8 "SECRET_A"
[__str_decrypt] call #3: enc=0000019deea701d6 len=8 key=0xa5 -> decrypted=0000019deed273b0 "SECRET_B"
=== JIT done ===

========================================
 PROBE RESULTS
========================================
(a) __str_decrypt call count = 3
    (3 textual uses of string literals: 2x SECRET_A slice + 1x SECRET_B string)
    -> one decrypt PER use (no caching; re-decrypts every time)

(b) plaintext lingering in heap buffers AFTER JIT returned?
    distinct decrypted buffers handed out: 3
    buf[0] @ 0000019deed27fb0 : "SECRET_A"  (still readable: YES -- plaintext IS resident on the heap)
    buf[1] @ 0000019deed27b10 : "SECRET_A"  (still readable: YES -- plaintext IS resident on the heap)
    buf[2] @ 0000019deed273b0 : "SECRET_B"  (still readable: YES -- plaintext IS resident on the heap)

(c) string handle: copy or alias?
    ext_string::slot() reads the host-owned std::string store.
    If string_from_slice COPIED, the handle's content is independent
    of the decrypt buffer; if it ALIASED, freeing the decrypt buffer
    would invalidate the handle.
    ext_string::slot(1) = "SECRET_B" (len 8)
    handle data ptr == any decrypt buf ptr? NO  -> COPY (handle owns its own std::string)
========================================
```

### What the probe proves

| Sub-question | Result | Implication |
|---|---|---|
| (a) cached or per-use? | **3 calls for 3 uses** — `SECRET_A` decrypted twice into two distinct buffers | No caching. The compiler re-emits a decrypt call at **every** textual use site. |
| (b) transient? | **All 3 buffers still readable after `probe()` returns** | Plaintext is **resident on the heap** for the buffer's lifetime. The codegen never frees the decrypt buffer; a host that doesn't either leaks it. Not transient. |
| (c) handle copies or aliases? | **COPY** — `ext_string::slot(1)`'s data ptr ≠ any decrypt buf ptr | `string_from_slice` copies into the host `std::string` store. The decrypt buffer is **dead** the instant the conversion runs — and still leaks. |

A subtle but important corollary of (a)+(b): because every use re-decrypts to a
*fresh* buffer, a literal used N times leaves **N** plaintext copies on the
heap simultaneously (until GC/host cleanup), not one. The "rebuild on each use"
behavior the user *wants* does exist (per-use re-decrypt) — but it rebuilds to
the **heap**, not the **stack**, and the rebuilds **accumulate** rather than
vanish.

---

## The gap (current vs. desired)

| Aspect | Desired | Current |
|---|---|---|
| Const literal lowering | decrypt into a **stack alloca**, use for the expression, discard — plaintext lives only on the stack frame for the expression's lifetime | decrypt into a **heap buffer** via native call; buffer leaks |
| Non-const / held literal | decode once to a permanent place (heap/handle) on first access | same path as const — re-decrypts to a fresh heap buffer every use; `string` handle additionally copies |
| Const-vs-non-const distinction | compiler proves non-modification and selects the lowering | **no such analysis**; every encrypted literal takes the identical path regardless of `const` |
| Plaintext residency | transient (stack-scoped) | **resident on the heap** for the buffer's lifetime; N copies for N uses |
| Decrypt-buffer lifetime | bounded by the expression | unbounded (host must free; codegen doesn't) |
| `__str_decrypt` contract | (would differ per lowering class) | single heap-allocating native, no free, no const awareness |

The current design has one thing going for it — "raw strings never appear in
the executable memory" (`src/codegen.cpp:2120-2122`) is genuinely true: the
JIT'd code only ever holds the encrypted rodata pointer and the key, and the
plaintext is produced at runtime. That defeats *static* extraction of literals
from the JIT'd pages. But it does **not** deliver the *transient* runtime
property the user is after — the plaintext is resident on the heap the moment
any use runs, and stays there.

---

## Recommendation

### The property the user wants requires a codegen change, not just a better `__str_decrypt`

The current `__str_decrypt`-returns-heap-ptr contract is structurally unable
to deliver transience: by the time codegen sees the return value, the plaintext
is already on the heap and codegen has no way to bound its lifetime (it doesn't
free, and the slice ABI hands the ptr to arbitrary callees that may retain it).
A smarter host `__str_decrypt` could wipe its buffer, but it can't know *when*
to wipe — it returns before the bytes are used, and the caller may pass the ptr
to `probe_use_slice` (which reads it after the native returns). So the wipe
would have to happen at a point the host can't observe.

The clean fix is to **change the lowering** for const literals so the plaintext
never leaves the stack frame:

#### 1. Distinguish const vs non-const string literals in sema

Today `StringLit` has `encrypted`/`baked_key`/`baked_ptr` but no `is_const_use`
flag. Add one, set it in `check_expr`'s `StringLit` case based on the binding
context:

- A literal that flows **directly** into a `string`-handle conversion
  (`implicit_to_string` true) is **non-const** by intent — the user is asking
  for an owned, held string. → decode-once-to-permanent path.
- A literal that flows into a **slice<u8>** context (passed to a native taking
  `u8[]`, indexed, used in a comparison, etc.) and is **not stored into any
  mutable/escaping slot** is **const**. → transient stack-rebuild path.
- The hard part: a literal stored into a `let` (even `const`) that then
  *escapes* (returned, stored into a global, captured by a closure that
  outlives the frame) cannot be stack-scoped. This needs the same escape
  analysis the slice `local_array_view` machinery (`src/sema.cpp:589`,
  `is_local_array_view`) already approximates — that bit could be generalized
  to "does this slice value escape the frame?" and reused here.

A first cut can be conservative: treat **only** literals used as a bare
argument to a native whose param is `slice<u8>` and which the native is known
not to retain (the existing print-style natives) as const/transient; treat
everything else as non-const/decode-once. This captures the common
`print_str("literal")` / `probe_use_slice("literal")` case (the case the user's
example is about) without a full escape analysis.

#### 2. Lower const literals to a stack alloca + inline XOR, not a native call

For a const/transient literal, codegen already knows the rodata offset, the
length, and the key. Instead of:

```
sub rsp, 32
mov rcx, enc_ptr ; mov rdx, len ; mov r8, key
call __str_decrypt
add rsp, 32
; rax = decrypted heap ptr
```

emit (sketch — the actual emitter calls would mirror the unencrypted branch
plus a tiny inline XOR loop, or a rep stub):

```
; allocate the plaintext on THIS stack frame, sized to baked_len
sub rsp, <baked_len rounded to 16 + alignment>
mov rsi, <enc_ptr (rodata, FunctionRodataBase + string_off)>
mov rdi, rsp                    ; dest = stack buffer
mov rcx, <baked_len>            ; count
; inline byte-XOR loop:  mov al,[rsi+r] ; xor al,key ; mov [rdi+r],al
;   (or a tight rep movsb + a second pass for the XOR;
;    a single interleaved loop is few bytes and keeps the key in a reg)
; rax = rsp (the stack buffer), rdx = len
... use rax/rdx as the slice ...
; at end of the STATEMENT (not the expression), add rsp, <baked_len> -> discard
```

The plaintext now lives **only on the caller's stack frame for the duration of
the statement**, and is overwritten by the next statement's frame reuse. No
heap, no native call, no leak, no second copy. `__str_decrypt` is not involved
at all for this class — which also removes the "host must register a native"
barrier for the transient case.

This is strictly a codegen lowering choice; sema has already done the
encryption baking. The key (`baked_key`) is already on the `StringLit` node, so
the inline XOR has everything it needs at emit time.

#### 3. Keep the decode-once path for non-const literals, but fix the leak

For non-const literals (the `string`-handle path and any escaping use), the
current heap-allocating `__str_decrypt` + `string_from_slice` *copy* is close
to right, but the decrypt buffer leaks. Two options:

- **(preferred)** Skip `__str_decrypt` entirely for the `string` path: have
  codegen emit the inline stack XOR into a temporary, then call
  `string_from_slice(temp_ptr, len)`, then discard the temp. The handle owns
  the only persistent copy; the transient plaintext is on the stack only for
  the conversion call. This unifies the two paths (both use the inline XOR;
  the only difference is whether a `string_from_slice` follows).
- **(alternative)** Define a real `__str_decrypt` contract with a paired
  `__str_decrypt_free` and have codegen free after the `string_from_slice`
  copy. More moving parts, host still has to register two natives, and it
  only helps the non-const path.

Either way the `__str_decrypt`-returns-heap-ptr contract should be **retired**
in favor of the inline-stack-XOR lowering; it cannot deliver transience and
its "host opts in by registering a native" shape is what left the feature with
no in-tree demonstration in the first place.

#### 4. Roadmap framing

This is a real feature with a clean design, not a bug fix:

- **Now**: encryption is host-opt-in, un-demonstrated, heap-allocating,
  leaky, and const-blind. The "raw strings never appear in executable memory"
  property holds; the "plaintext is transient" property does not.
- **Proposed Tier**: a `STRING_TRANSIENT_DECRYPTION` roadmap item with two
  sub-tasks — (1) sema const/escape classification for string literals (reusing
  the `local_array_view` escape approximation as a seed), (2) codegen
  inline-stack-XOR lowering replacing the `__str_decrypt` native call for the
  const/transient class, with the decode-once `string` path reusing the same
  inline XOR + `string_from_slice`.

I think this **should** become a tracked roadmap item. The current
implementation is a half-feature: it defeats static extraction of literals
from JIT pages (good) but advertises an "encryption" property that, on the
runtime-memory axis the user actually cares about, does not hold — the
plaintext is resident on the heap, in N copies for N uses, with no cleanup. A
user who turns this on *expecting* transience (the word "encryption" invites
that expectation) gets the opposite. Either the feature should deliver the
transient property, or its documentation should be blunt that it does not and
was never intended to — right now it neither delivers nor disclaims.

---

## Appendix: files touched / not touched

- **Created by this investigation (the only new files):**
  - `demo/STRING_ENCRYPTION_ANALYSIS.md` (this doc)
  - `tmp_edit/enc/probe.cpp`, `tmp_edit/enc/probe_build.sh`,
    `tmp_edit/enc/probe_output.txt` (all gitignored under `tmp_edit/`)
- **Not modified by this investigation:** `src/`, `extensions/`, `examples/`,
  `CMakeLists.txt`. No editor was opened on any source file; the probe only
  compiled `tmp_edit/enc/probe.cpp` and linked `buildt/*.a`.
- **Pre-existing uncommitted working-tree changes (NOT mine, NOT touched):**
  `src/codegen.cpp` and `src/sema.cpp` carry uncommitted modifications
  (`git diff --stat HEAD` → ~179 insertions) implementing an unrelated
  "struct temp slots" feature (compiler-hidden frame slots for struct-by-value
  general-expression args and struct-literal returns). These were present in
  the working tree on entry; I did not create them and did not alter them.
  They do **not** touch the string-encryption code: the codegen `StringLit`
  eval case and the sema `StringLit` bake are **byte-identical** between the
  working tree and `HEAD` (verified by `diff` of the extracted regions). The
  only effect on this doc is that the struct-temp code inserted earlier in
  both files **shifted the line numbers** of the string-encryption code
  downward (codegen StringLit case: `HEAD` ~1984 → working tree 2115; sema
  bake: `HEAD` ~651 → working tree 669). All `src/codegen.cpp:N` and
  `src/sema.cpp:N` citations in this doc reference the **current working-tree**
  line numbers so a reader can find the code as it actually is; every other
  file cited (`src/ast.hpp`, `src/codegen.hpp`, `extensions/string/ext_string.cpp`,
  `examples/*`, `extensions/AUDIT.md`, `src/parser.cpp`) is unmodified vs
  `HEAD`, so its citations are stable.
- **Gate:** `ninja -C buildt` → clean ("no work to do" on entry; the final
  rebuild only relinked `game_host.exe` against unchanged objects); `ctest`
  (20 tests) → 100% passed, both before and after. No regression.
