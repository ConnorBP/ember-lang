# String Const-Mode Classification — Feasibility & Subsumption Study

> **Historical investigation — conclusion implemented through slice safety.**
> This is a pre-fix snapshot, not a description of current sema verdicts.
> Encrypted literals used as slices still decode into per-use frame storage;
> literals converted to the extension `string` become owned copies. Sema now
> treats a slice-typed `StringLit` as stack-backed and rejects its escape
> through return/global/retaining-call paths, including borrowed/retained
> script-function propagation. Therefore the proposed separate const-mode
> classifier remains unnecessary. See `src/sema.cpp::is_local_array_view`,
> the Stage 2 borrow/retain pre-pass, and the `sema_invalid_stringlit_*` tests.

**Read-only investigation.** No `src/`, `extensions/`, or `examples/` files were
modified. The only artifacts created are this doc and the probe under
`tmp_edit/constmode/` (gitignored). The gate (`ctest` 22/22, lang 245/0/0) was
confirmed green before and after; the build (`ninja -C buildt`) was "no work to
do" and the working tree is clean (`git status` empty). This doc mirrors the
structure of `demo/STRING_ENCRYPTION_ANALYSIS.md`: question → current mechanism
(cited) → probe + evidence → gap analysis → recommendation.

---

## The question (the skipped classification)

The transient string-encryption fix (commit `e98dc87`) lowered ALL encrypted
string literals to an **inline-stack-XOR** into a compiler-hidden temp frame
slot (`alloc_str_temp` in `src/codegen.cpp`'s StringLit eval case). The
const/non-const classification that `STRING_ENCRYPTION_ANALYSIS.md`
recommended was **deliberately skipped**. The commit message / roadmap note
(`docs/ROADMAP.md:319-328`) justifies the skip with two claims:

1. The `string`-handle path copies out of the stack temp immediately (the
   handle owns the only persistent copy), so it's safe either way.
2. A raw `slice<u8>` literal that escapes the frame has the "same pre-existing
   dangling-slice-of-stack-local problem `local_array_view` already has" —
   not a regression introduced here.

The skipped classification would distinguish:

- **const/transient use**: a literal that flows into a `slice<u8>` context and
  does NOT escape the frame → stays on the stack (current behavior, transient).
- **owned/held use**: a literal that will be retained (stored, returned,
  captured, or converted to a `string` handle) → decode ONCE to a permanent
  place on first access.

The investigation's job is to produce evidence — not speculation — for three
sub-questions:

1. **Can sema actually classify this at the StringLit node?** What information
   is available at the StringLit's sema check, and what escape analysis would
   const-vs-owned need? Is `is_local_array_view` generalizable to StringLit, or
   does StringLit need a fresh pass?
2. **What would "decode-once to a permanent place" for the owned path actually
   lower to?** Is the owned path just "always use the handle path for escaping
   literals," or does it need new infrastructure (rodata-backed permanent
   slice, per-call host slot)?
3. **Is there a real benefit?** Does const-classification pay for itself, or
   does fixing slice-safety subsume it (i.e. once escaping slices are
   rejected/owned, every escaping StringLit is forced to the handle path
   anyway, and const-mode is just an optimization that saves a copy on the
   transient path)?

---

## TL;DR — the answer

> **SUBSUMED BY SLICE-SAFETY.** Const-mode classification is *feasible* at
> the sema layer (the `implicit_to_string` flag already cleanly partitions the
> owned-vs-transient cases for the *non-escaping* literals), but it does **not
> pay for itself**, and the one case where it would change behavior — a raw
> `slice<u8>` literal that escapes the frame — is **already unsafe today** and
> is the precise bug the parallel slice-safety investigation is fixing.
>
> The probe (`tmp_edit/constmode/probe.cpp`) confirms empirically:
>
> - **Q1 (escaping slice literal):** `fn leak() -> u8[] { return "SECRET_TOKEN_42"; }`
>   compiles cleanly (sema does NOT catch it — `is_local_array_view` does not
>   recognize a StringLit), and the returned slice's ptr is into `leak()`'s
>   reclaimed frame slot. Reading the bytes immediately after the call returns
>   `"SECRET_TOKEN_42"` (correct, the bytes are still there until something
>   reuses the stack), but after a follow-up `smash()` call that reuses the
>   same stack region, the bytes read `"ZZZZZZZZZZZZZZZ"` (0x5A, the smash
>   filler). **The slice dangles.** This is a real bug, but it is the
>   dangling-slice-of-stack-local bug, NOT a string-encryption bug.
> - **Q2 (owned string handle):** `fn owned_s() -> string { let s =
>   "OWNED_TOKEN_99"; return s; }` returns a handle whose content
>   (`ext_string::slot(h)`) reads `"OWNED_TOKEN_99"` both before AND after
>   `smash()`. **The owned path is already persistent and safe** —
>   `string_from_slice` copies the bytes out of the stack temp into the host
>   `std::string` store, and the handle owns the only persistent copy. Nothing
>   needs to change here.
> - **Q3 (transient slice arg):** `use_slice_arg("TRANSIENT_OK", 12)` (a
>   StringLit passed as a `slice<u8>` arg to a native, used within the call)
>   reads `"TRANSIENT_OK"` correctly inside the native. **The transient stack
>   path is safe for non-escaping use.**
>
> The conclusion: **defer const-mode classification until the slice-safety fix
> lands, then re-evaluate.** Once escaping slices are rejected (or forced to
> an owned form) at sema, every escaping StringLit is either rejected (the
> `return "lit"` as `u8[]` case) or forced through `implicit_to_string` (the
> `let s: string = "lit"; return s;` case, already safe). At that point the
> only StringLit slices that remain are non-escaping transients, for which
> the current stack-XOR is already correct and optimal. Const-mode becomes a
> pure micro-optimization (saving the `string_from_slice` copy on the transient
> path) with no safety upside — and the transient path doesn't make that copy
> today anyway, so there's literally nothing to save.

---

## The current mechanism (cited)

### 1. Sema bakes every StringLit as encrypted-when-key≠0, with no const gate

`src/sema.cpp:671-735` (the `StringLit` case of `check_expr`):

```cpp
if (auto* lit = dynamic_cast<StringLit*>(&e)) {
    e.ty = lit->ty = intern(make_slice(std::make_shared<Type>(make_prim(Prim::U8))));
    uint8_t key = prog->string_xor_key;
    auto enc = std::make_shared<std::string>(lit->s);
    if (key != 0) { for (auto& c : *enc) c ^= key; lit->encrypted = true; }
    lit->baked_key = key;
    lit->baked_ptr = reinterpret_cast<const uint8_t*>(enc->data());
    lit->baked_len = int64_t(enc->size());
    prog->rodata_store.push_back(std::move(enc));
    // ... implicit_to_string stamping (see §2 below) ...
}
```

Key facts:
- **All** string literals bake identically when `key != 0`. There is **no**
  `is_const`/mutability/escape test here. (The CLI default is now `0xA5`, ON —
  `examples/ember_cli.cpp`'s `string_xor_key` line, per the e98dc87 commit.)
- The `StringLit` node (`src/ast.hpp:86-110`) carries `baked_ptr`, `baked_len`,
  `baked_key`, `encrypted`, and the `implicit_to_string` / `to_string_native_*`
  fields set by the implicit-conversion block below.

### 2. `implicit_to_string` already partitions owned vs transient — but only by immediate context type

`src/sema.cpp:700-735`:

```cpp
if (!expected || (expected->prim == Prim::I64 && expected->struct_name == "string")) {
    auto nit = natives->find("string_from_slice");
    if (nit != natives->end()) {
        lit->implicit_to_string = true;
        lit->to_string_native_fn = nit->second.fn_ptr;
        lit->to_string_native_name = nit->first;
        const Type* string_ty = expected;
        if (!string_ty) { Type t; t.prim = Prim::I64; t.struct_name = "string"; string_ty = intern(t); }
        e.ty = lit->ty = string_ty;
    }
}
```

`expected` is the *immediate* context type threaded down from the parent
(`check_value` → `check_expr`):

| Site | `expected` at the StringLit | `implicit_to_string` | Resolved type |
|---|---|---|---|
| `print_str("lit")` (native `fn(u8[])`) | `u8[]` (slice) | **false** | `u8[]` |
| `let s: u8[] = "lit";` | `u8[]` | **false** | `u8[]` |
| `return "lit";` in `-> u8[]` fn | `u8[]` | **false** | `u8[]` |
| `let s = "lit";` (untyped) | `nullptr` | **true** | `string` |
| `let s: string = "lit";` | `string` | **true** | `string` |
| `return "lit";` in `-> string` fn | `string` | **true** | `string` |
| `global g: string = "lit";` | (`string`, at global init) | n/a — baked by `eval_global_initializers` | `string` handle |

So `implicit_to_string` ALREADY cleanly distinguishes the two const-mode
classes for the **non-escaping** literals:

- `implicit_to_string == true` → the literal is being converted to an owned
  `string` handle → **owned path**. (This is the "decode-once to a permanent
  place" case, and it ALREADY works — see Q2.)
- `implicit_to_string == false` → the literal stays a raw `slice<u8>` →
  **transient path** (current stack-XOR behavior).

### 3. The escape-analysis machinery does NOT cover StringLit

`src/sema.cpp:593-613` — the `is_local_array_view` approximation:

```cpp
void declare(const std::string& n, const Type* t, bool is_const,
             bool local_array_view = false, Loc loc = {0, 0}) { /* ... */ }

bool is_local_array_view(const Expr& e) const {
    if (dynamic_cast<const ViewExpr*>(&e)) return true;          // a[..]
    if (auto* id = dynamic_cast<const Ident*>(&e)) {              // a flagged local
        for (int i = int(scopes.size()) - 1; i >= 0; --i)
            for (const auto& v : scopes[size_t(i)])
                if (v.name == id->name) return v.local_array_view;
    }
    if (auto* t = dynamic_cast<const TernaryExpr*>(&e))
        return is_local_array_view(*t->then_e) || is_local_array_view(*t->else_e);
    return false;                                                // <-- StringLit falls here
}
```

It is seeded ONLY by `ViewExpr` (the `a[..]` form) and propagated through
`Ident` reads of flagged locals. It is threaded through:
- `LetStmt` (`src/sema.cpp:1590`): `declare(ls->name, decl_ty, ls->is_const, is_local_array_view(*ls->init), ls->loc)`.
- `AssignExpr` (`src/sema.cpp:1279-1290`): sets `v->local_array_view` for a slice target, and rejects storing a local-array-derived slice into a global.
- `ReturnStmt` (`src/sema.cpp:1601-1602`): rejects returning a local-array-derived slice.

**A `StringLit` is none of those.** `is_local_array_view` returns `false` for a
bare StringLit. So a StringLit in the same escape positions where `a[..]` IS
caught — `return "lit"` as `u8[]`, `out = "lit"` into a global slice, `let s: u8[] = "lit"; return s;` — is **NOT caught**. The escape rejection at sema.cpp:1289-1290 and 1601-1602 never fires for a StringLit.

This is the gap the parallel slice-safety investigation is responsible for,
and it is the gap that determines whether const-mode classification is
worth doing separately.

### 4. Codegen lowers every encrypted literal to the inline-stack-XOR, then conditionally to a handle

`src/codegen.cpp:2367-2530` (the `StringLit` case of `eval`):

- For every encrypted literal (`lit->encrypted && lit->baked_key != 0`):
  `alloc_str_temp(baked_len)` reserves a `ceil(baked_len,8)`-byte frame slot,
  then an inline byte-XOR loop (unrolled for `len ≤ 256`, runtime loop for
  longer) decrypts rodata → the frame slot, and `rax = slot base, rdx = len`
  is the slice ABI. No heap, no host native, no leak. (The old
  `__str_decrypt` heap-ptr contract is gone — `str_decrypt_fn`/`str_decrypt_name`
  were dropped from `CodeGenCtx` in e98dc87.)
- For the unencrypted branch (`key == 0`): raw rodata pointer.
- Then, **if `lit->implicit_to_string`**: `string_from_slice(ptr, len)` is
  called (rcx=ptr, rdx=len → rax = i64 handle), and the handle stands in for
  the expression's value.

The frame slot is **reclaimed when the function returns** (it's rbp-relative,
part of the frame; `emit_epilogue` does `add rsp, frame_size; pop rbp; ret`).
The slot's bytes are therefore live for the duration of the *expression* and
the *statement* that contains it — and no longer.

### 5. `string_from_slice` COPIES (the handle owns the only persistent copy)

`extensions/string/ext_string.cpp:38-48`:

```cpp
static int64_t n_string_from_slice(uint8_t* p, int64_t len) {
    if (len < 0 || uint64_t(len) > uint64_t(MAX_STRING_BYTES) || (!p && len != 0)) return 0;
    try {
        if (len == 0) return str_new(std::string());
        return str_new(std::string(reinterpret_cast<const char*>(p), size_t(len)));  // <-- COPY
    } ...
}
```

`str_new` (`ext_string.cpp:24`) pushes into `g_strings`, a host-owned
`std::vector<std::string>`. The `std::string(p, len)` constructor **copies**
`len` bytes out of `p` into the new string's own storage. The stack temp `p`
is dead the moment `string_from_slice` returns — and the handle's content is
independent of it. **This is why the owned path is already safe.**

### 6. The owned-path infrastructure already exists for globals

`src/globals.hpp:66` — `GlobalInitCtx::string_alloc_fn` is a host hook that
materializes a `string`-typed global's literal initializer into a live handle
at load:

```cpp
} else if (t && t->prim == Prim::I64 && t->struct_name == "string" && gic.string_alloc_fn) {
    if (auto* lit = dynamic_cast<const StringLit*>(g.init.get())) {
        int64_t h = gic.string_alloc_fn(lit->s.c_str(), int64_t(lit->s.size()));
        std::memcpy(slot, &h, 8);
        ++baked;
    }
}
```

`examples/ember_cli.cpp:465-472` wires it to `ext_string::alloc`. The
regression test `tests/lang/runtime_global_string_init.ember` covers it
(245/0/0 includes it). So a `string`-typed global's literal is decoded ONCE
to a permanent place (the host store) at load — the very "decode-once to a
permanent place" the const-mode owned path wants. **For the global case, the
owned path is already shipped.**

---

## The probe and its output (evidence)

Probe: `tmp_edit/constmode/probe.cpp` (build: `tmp_edit/constmode/probe_build.sh`,
gitignored). It mirrors `em_roundtrip_test`'s harness but compiles three ember
fns with encryption ON (the CLI default `0xA5`) and drives them from C,
inspecting the returned slice's ptr and the handle's content before and after
a stack-smashing follow-up call. The slice-returning C thunk captures both
`rax` (ptr) and `rdx` (len) via a register-asm variable (a plain C cast to
`int64_t()` would lose `rdx`).

Source under test:

```
fn leak() -> u8[] { return "SECRET_TOKEN_42"; }       // Q1: escaping slice literal
fn smash() -> i64 {                                    // Q1/Q2: stack reuser
    let buf: u8[256];
    let mut i: i64 = 0;
    while (i < 256) { buf[i] = 0x5A; i = i + 1; }
    return 0;
}
fn owned_s() -> string { let s = "OWNED_TOKEN_99"; return s; }  // Q2: owned handle
fn use_transient() -> i64 { use_slice_arg("TRANSIENT_OK", 12); return 0; }  // Q3: transient arg
```

A host native `use_slice_arg(uint8_t* p, int64_t len)` reads and prints the
slice bytes within the call (Q3).

### Observed output (`tmp_edit/constmode/probe_output.txt`)

```
sema: OK (4 funcs)

=== Q1: leak() returns an escaping StringLit slice ===
leaked slice: ptr=00000059ae7ff1c9 len=15
bytes read IMMEDIATELY after leak() returns (before any other C call): "SECRET_TOKEN_42"
now calling smash() (256-byte frame filled with 0x5A='Z')...
bytes read AFTER smash(): "ZZZZZZZZZZZZZZZ"
Q1 verdict: DANGLING - slice ptr was into leak()'s reclaimed frame; smash() overwrote it.
  (immediate="SECRET_TOKEN_42" -> after_smash="ZZZZZZZZZZZZZZZ")

=== Q2: owned_s() returns a string handle (owned path) ===
handle = 1
ext_string::slot(handle) BEFORE smash(): "OWNED_TOKEN_99" (len 14)
ext_string::slot(handle) AFTER  smash(): "OWNED_TOKEN_99" (len 14)
Q2 verdict: PERSISTENT - handle owns its own std::string copy; survives smash().

=== Q3: use_transient() passes a StringLit slice to a native (transient) ===
[use_slice_arg] ptr=00000059ae7ff1cc len=12 "TRANSIENT_OK"
Q3: native saw the slice ptr=00000059ae7ff1cc and read the bytes above (should be "TRANSIENT_OK")
    (transient use is safe: the frame slot is live for the duration of the call.)

========================================
 CONST-MODE INVESTIGATION SUMMARY
========================================
Q1 escaping StringLit slice: DANGLING (ptr into reclaimed frame; smash() overwrote it)
    -> an escaping raw-slice<u8> literal is UNSAFE (the dangling-slice-of-stack-local problem, UNCAUGHT for StringLit) today.
Q2 owned string handle:       PERSISTENT (handle owns the only copy; survives frame teardown)
    -> the implicit_to_string path is safe today.
Q3 transient slice arg:      native read correct bytes within the call
    -> the transient (stack) path is safe for non-escaping use.
========================================
```

### The sema-side evidence (run with the built `sema_check.exe`)

The probe confirms the *runtime* consequence. The *sema* classification
question is confirmed separately with the in-tree `buildt/sema_check.exe`
(which registers all six extensions and sets `string_xor_key = 0xA5`, exactly
as the lang suite does):

```
$ buildt/sema_check.exe  <(fn bad() -> u8[] { return "dangles?"; })           # StringLit returned as slice
OK: 2 funcs type-checked                                            # NOT caught — compiles

$ buildt/sema_check.exe  <(global out: u8[] = ""; fn bad() -> i64 { out = "dangles?"; return 0; })  # StringLit -> global slice
OK: 2 funcs type-checked                                            # NOT caught — compiles

$ buildt/sema_check.exe  <(fn bad() -> u8[] { let mut s: u8[] = "dangles?"; return s; })  # StringLit -> local -> return
OK: 2 funcs type-checked                                            # NOT caught — compiles

# Contrast: the same patterns with a local fixed array ARE caught:
$ buildt/sema_check.exe tests/lang/sema_invalid_local_slice_return.ember
SEMA_ERROR: line 1:34  cannot return a slice/view derived from a local fixed array

$ buildt/sema_check.exe tests/lang/sema_invalid_local_slice_global_store.ember
SEMA_ERROR: line 2:33  cannot store a slice/view derived from a local fixed array in a global

$ buildt/sema_check.exe tests/lang/sema_invalid_local_slice_assignment_escape.ember
SEMA_ERROR: line 6:5  cannot return a slice/view derived from a local fixed array
```

### What the probe + sema evidence proves

| Sub-question | Result | Implication |
|---|---|---|
| **Q1: does an escaping StringLit slice dangle?** | **DANGLING** — bytes correct immediately (stack not yet reused), `"ZZZ..."` after `smash()` reuses the frame | An escaping raw `slice<u8>` literal is **UNSAFE today**. The returned ptr is into the callee's reclaimed frame slot. |
| **Q1-sema: is the escape caught?** | **NO** — `return "lit"` as `u8[]`, `out = "lit"`, `let s: u8[] = "lit"; return s;` all compile cleanly | `is_local_array_view` does NOT recognize a StringLit. The escape rejection at sema.cpp:1289/1601 never fires for literals. This is a real gap — but it is the **slice-safety** gap, not a string-encryption gap. |
| **Q2: is the owned `string`-handle path persistent?** | **PERSISTENT** — handle reads `"OWNED_TOKEN_99"` before AND after `smash()` | `string_from_slice` copies into the host `std::string` store; the handle owns the only persistent copy. The owned path is **already safe and shipped** (and globals already use `string_alloc_fn` for the same property). |
| **Q3: is the transient slice-arg path safe within the call?** | **SAFE** — native read `"TRANSIENT_OK"` correctly | The frame slot is live for the duration of the call. The transient stack-XOR path is correct for non-escaping use. |

A subtle corollary of Q1: the bytes are correct *immediately* after the call
returns (before any other C-stack operation reuses the frame). This is the
same property that makes the bug **silent in practice** — a program that
consumes the returned slice synchronously, in the same call frame before any
other call lands on the reclaimed stack, will read the right bytes and never
notice. The bug only manifests when the slice outlives the frame that produced
it (stored to a global, returned to a caller that retains it across further
calls, captured across a loop iteration that reuses the frame). That silence
is exactly why the bug has shipped this long without a regression test — there
is no in-tree test that returns a slice literal (`grep "return.*\"" tests/lang`
returns nothing).

---

## The classification rule (answered against the evidence)

### What sema knows at the StringLit node

At the StringLit's `check_expr`, sema has:

- `expected` — the *immediate* context type (a slice, a `string`, or
  `nullptr` for an untyped let).
- The `implicit_to_string` flag it stamps from `expected` — which already
  partitions owned (`true`) vs transient (`false`) for the **non-escaping**
  literals.
- It does **NOT** have, at this node, any information about whether the
  resulting slice value *escapes the frame*. Escape is a property of the
  *parent statement* (is this a `return`? a store to a global? a store to a
  local that is later returned?), not of the literal itself.

### The exact classification rule (two layers)

**Layer 1 — owned vs transient (already implemented, free):**

```
implicit_to_string == true  → owned path  (decode-once to permanent via string_from_slice; SAFE today)
implicit_to_string == false → transient path (inline-stack-XOR; safe IFF the slice does not escape)
```

This layer is **already correct** and needs no new analysis. Q2 proves the
owned path is persistent; Q3 proves the transient path is safe for
non-escaping use.

**Layer 2 — does the transient slice escape the frame? (NOT implemented; this is the gap):**

The transient-path literal is safe ONLY if the resulting `slice<u8>` value
does not outlive the frame. The escape positions are:

- `return "lit";` in a `-> u8[]` fn (escapes via the return).
- `out = "lit";` into a global `u8[]` (escapes into the global).
- `let s: u8[] = "lit";` followed by `return s;` / `out = s;` (escapes via the local).
- captured across a loop iteration that reuses the frame (the loop body's
  frame slot is reused each iteration; a slice from iteration N read in N+1
  dangles — though this is a narrower case).

This is **the same escape analysis `is_local_array_view` approximates for
`a[..]`**, extended to cover StringLit. It is NOT a fresh pass — it is a
one-line generalization of the existing machinery (see below).

### Is `is_local_array_view` generalizable, or does StringLit need a fresh pass?

**Generalizable, with one caveat.** The existing `is_local_array_view` is
seeded by `ViewExpr` and propagated through `Ident` reads of flagged locals.
To cover StringLit, the seed must additionally include the StringLit's own
stack temp — i.e. a raw `slice<u8>` StringLit is itself a "local-array-view-
like" value (its ptr is into a frame-scoped slot, exactly like `a[..]`'s ptr
into the local fixed array). Concretely:

```cpp
bool is_local_array_view(const Expr& e) const {
    if (dynamic_cast<const ViewExpr*>(&e)) return true;
    if (dynamic_cast<const StringLit*>(&e)) return true;   // <-- the one-line generalization
    if (auto* id = dynamic_cast<const Ident*>(&e)) { /* unchanged */ }
    if (auto* t = dynamic_cast<const TernaryExpr*>(&e)) { /* unchanged */ }
    return false;
}
```

With that one line added, `is_local_array_view(*ls->init)` returns true for
`let s: u8[] = "lit";`, the `local_array_view` bit propagates onto `s`, and
the existing `return s;` / `out = s;` rejections fire. The bare
`return "lit";` and `out = "lit";` cases are caught by the existing
ReturnStmt/AssignExpr checks that call `is_local_array_view(*rs->value)` /
`is_local_array_view(*a->value)` directly on the literal.

**The caveat:** this is a *conservative first cut* (exactly the shape the
original analysis doc suggested), NOT full escape analysis. It would also
reject a transient-but-non-escaping use like `let s: u8[] = "lit"; print_str(s);`
(where `s` is consumed synchronously and never escapes), because the bit is
set at declaration time without tracking whether `s` is subsequently
returned/stored. That is the same over-conservatism `local_array_view`
already has for `a[..]` — and it is the right conservatism for a safety check
(reject the maybe-escaping case; the user who genuinely wants a transient
local slice passes the literal directly to the consumer: `print_str("lit")`,
which never binds a name and is never flagged). A full escape analysis that
tracks "is this local ever returned/stored" would be strictly more precise,
but it is the slice-safety investigation's job to decide whether to invest in
that, not the const-mode investigation's.

**This is the decisive evidence for the subsumption verdict.** The one
mechanism that would make const-mode classification *change behavior* —
flagging the escaping-transient-slice case — is **identical** to the
mechanism the slice-safety fix needs. They are not two features; they are one
check. Once that check exists, const-mode has nothing left to do.

---

## What "decode-once to a permanent place" for the owned path would lower to (answered)

The investigation asked to evaluate three options for the owned path, against
the real codegen/emitter API:

**(a) The `string`-handle path (already works).** This IS the owned path
today. `implicit_to_string` stamps the literal; codegen emits the inline
stack-XOR into the temp, then `string_from_slice(temp_ptr, len)`, then
discards the temp (the frame reclaims it at function return). The handle owns
the only persistent copy (Q2 proves it survives `smash()`). **There is nothing
to add.** The owned path does not need a new lowering; it needs the
classification to route escaping literals to it — but routing an escaping
`slice<u8>` literal to the handle path requires the literal's *declared
context* to be `string`, not `u8[]`, which is a source-level decision
(`let s: string = "lit"; return s;`), not a compiler decision the literal can
make for itself at its `check_expr` (where `expected` is already `u8[]` for a
`-> u8[]` return). So "always use the handle path for escaping literals" is
not actually available as a codegen choice for the `return "lit"` as `u8[]`
case — the function's return type is `u8[]`, and the literal resolves to
`u8[]`. The only correct fix for that case is to **reject it** (the slice-safety
fix), not to silently re-route it to a handle.

**(b) A rodata-backed permanent slice (decrypt once into a host-owned buffer
indexed by literal, freed at shutdown).** This would be a new lowering: at
first access, decrypt the literal into a host-owned buffer (one per literal,
keyed by `baked_ptr`), and return a slice whose ptr is that persistent buffer
rather than the frame temp. This WOULD fix Q1 — the slice's ptr would outlive
the frame. But it has costs:
- A host-side buffer table (one entry per distinct literal, lifetime = program),
  with a first-access initialization (either JIT'd lazy init with a guard, or
  a load-time pre-decrypt of all literals).
- It defeats the **transience** property the whole e98dc87 effort just shipped:
  the plaintext would be resident in a host buffer for the program's lifetime,
  in one copy per literal. That is the *opposite* of the transient-stack
  design goal. (`STRING_ENCRYPTION_ANALYSIS.md`'s entire recommendation was
  to move OFF a persistent-heap buffer onto a transient stack slot; this
  option moves back.)
- It only helps the `-> u8[]` return case, which is the case that should be
  rejected anyway (a function returning a `u8[]` slice is returning a borrowed
  view; the right owned return type is `string`, which already works).

So (b) is a strictly-worse design that re-introduces the leak the prior
investigation closed, to paper over a bug that should be fixed at sema
instead. **Not recommended.**

**(c) A per-call permanent slot on the host side.** Same downsides as (b)
without the dedup; strictly worse. **Not recommended.**

**Conclusion on the owned path:** the owned path is already (a). Options (b)
and (c) are anti-patterns relative to the transient design. The only work the
owned path needs is **classification** to ensure escaping literals route to
it — and that routing is only available for literals whose context type is
already `string` (which `implicit_to_string` already handles correctly). The
escaping `u8[]` literal cannot be re-routed to the handle path at codegen; it
must be rejected at sema. **So the owned path needs no new codegen; it needs
the slice-safety rejection to remove the `u8[]` escape case from the
language.**

---

## The gap analysis (current vs. desired, per the const-mode framing)

| Aspect | Desired (const-mode) | Current (e98dc87) | Verdict |
|---|---|---|---|
| Owned literal (→ `string` handle) | decode-once to permanent; persistent | inline-XOR → `string_from_slice` copy → handle owns copy; **persistent** (Q2) | **Already correct** — no work |
| Owned literal (→ `string` global) | decode-once at load | `string_alloc_fn` bakes handle at load | **Already correct** — shipped (`runtime_global_string_init.ember`) |
| Transient literal, non-escaping (slice arg) | stack-XOR, transient | stack-XOR, transient; **safe within the call** (Q3) | **Already correct** — no work |
| Transient literal, escaping (`return "lit"` as `u8[]`, `out = "lit"`) | decode-once to permanent (the const-mode "owned" class) | stack-XOR into frame temp; **DANGLING** (Q1); **NOT caught** at sema | **The gap** — but it is the slice-safety gap, not a const-mode gap |
| Const-vs-non-const distinction at the literal | sema classifies by escape | no classification; `implicit_to_string` classifies by immediate context type only | The missing piece is the escape analysis — which is the slice-safety fix |

The single row where const-mode would change behavior (the escaping transient
literal) is the single row that is already a slice-safety bug. There is no row
where const-mode improves a *correct* current behavior.

---

## Recommendation

### Verdict: SUBSUMED BY SLICE-SAFETY. Defer const-mode classification until the slice-safety fix lands, then re-evaluate.

The evidence:

1. **The owned path is already shipped and correct** (Q2 + the
   `string_alloc_fn` global path). The "decode-once to a permanent place"
   lowering for the owned class is the inline-XOR + `string_from_slice`
   combination e98dc87 already emits; the handle owns the only persistent
   copy. There is no new codegen to write for the owned class.

2. **The transient path is already correct for non-escaping use** (Q3). The
   inline-stack-XOR is the right lowering for a slice that does not outlive
   the frame; it is the transient property the whole e98dc87 effort shipped.

3. **The only case where const-mode would change behavior — an escaping raw
   `slice<u8>` literal — is already unsafe today** (Q1), and it is unsafe for
   the *same reason* `a[..]` is unsafe: the slice's ptr is into a frame-scoped
   slot that doesn't outlive the frame. The fix for that case is to **reject
   it at sema** (the slice-safety fix), not to silently decode-once it into a
   persistent buffer (option (b) above, which re-introduces the leak
   `STRING_ENCRYPTION_ANALYSIS.md` just closed).

4. **The classification mechanism const-mode needs is identical to the
   mechanism the slice-safety fix needs.** The one-line generalization of
   `is_local_array_view` to include `StringLit` (cited above) is the single
   change that makes the escaping-literal case rejected. Once that lands,
   every escaping StringLit is either rejected (`return "lit"` as `u8[]`) or
   already routed to the owned handle path (`let s: string = "lit"; return
   s;`, which `implicit_to_string` already handles). The only StringLit
   slices that remain are non-escaping transients, for which the current
   stack-XOR is already correct and optimal. **Const-mode has nothing left
   to do.**

5. **As a pure micro-optimization, const-mode saves nothing.** The transient
   path doesn't call `string_from_slice` today (it has no handle to copy
   into), so there's no copy to save. The owned path's `string_from_slice`
   copy is the *point* of the owned path (it's what makes the handle
   persistent). There is no redundant copy to eliminate on either path.

### Concrete recommendation

- **Do NOT implement const-mode classification as a separate feature.** It
  would duplicate the slice-safety escape analysis with no safety or
  performance upside.

- **Do implement the slice-safety fix, and have it cover StringLit.** The
  minimal change is the one-line `is_local_array_view` generalization cited
  above (add `if (dynamic_cast<const StringLit*>(&e)) return true;`), which
  makes the existing ReturnStmt/AssignExpr/GlobalStore escape rejections fire
  for literals too. This is a conservative first cut (it also rejects the
  `let s: u8[] = "lit"; print_str(s);` synchronous-consumption case, which is
  safe but indistinguishable from the escaping case without full provenance
  tracking) — the same conservatism `local_array_view` already has for
  `a[..]`. Whether to invest in the full "is this local ever
  returned/stored" provenance analysis to recover the synchronous-consumption
  case is the slice-safety investigation's call, not this one's.

- **After the slice-safety fix lands, re-evaluate const-mode.** At that point
  the question becomes purely: "is there a perf win on the transient path
  from classifying literals?" — and the answer is expected to be no (the
  transient path already has no redundant copy). If a benchmark later shows
  the inline-XOR's per-use re-decrypt cost matters for a hot literal, the
  optimization to consider is a per-literal decode-once-to-a-permanent-slice
  (option (b)) — but that is a perf optimization with a transience cost, to
  be weighed against the threat model, not a safety feature. Until such a
  benchmark exists, it is YAGNI.

### Roadmap framing

This should become a single line in `docs/ROADMAP.md`'s "Investigation-backed
candidate changes" section, beneath the existing "Runtime string encryption —
DONE" entry:

> **String const-mode classification — INVESTIGATED, SUBSUMED.** The
> const/non-const literal classification `STRING_ENCRYPTION_ANALYSIS.md`
> recommended was investigated (`demo/STRING_CONST_MODE_INVESTIGATION.md`)
> and found subsumed by the parallel slice-safety work: the owned path is
> already correct (the `string`-handle copy is persistent), the transient
> path is already correct for non-escaping use, and the one case where
> classification would change behavior (an escaping raw `slice<u8>` literal)
> is already unsafe and is the dangling-slice-of-stack-local bug the
> slice-safety fix addresses. Deferred until the slice-safety fix lands; not
> a separate feature.

The slice-safety fix itself (the `is_local_array_view` generalization to
cover StringLit + `ArrayLit`-backed slices, plus the decision on full
provenance tracking vs. the conservative first cut) belongs in its own
roadmap entry under the H11 audit finding (`docs/audit/AUDIT_2026-07-09.md`
§H11), not here.

---

## Appendix: files touched / not touched

- **Created by this investigation (the only new files):**
  - `demo/STRING_CONST_MODE_INVESTIGATION.md` (this doc)
  - `tmp_edit/constmode/probe.cpp`, `tmp_edit/constmode/probe_build.sh`,
    `tmp_edit/constmode/probe_output.txt` (all gitignored under `tmp_edit/`)
- **Not modified by this investigation:** `src/`, `extensions/`, `examples/`,
  `CMakeLists.txt`, no test file. No editor was opened on any source file;
  the probe only compiled `tmp_edit/constmode/probe.cpp` and linked
  `buildt/libember_*.a`. The sema-side evidence was produced with the
  pre-built `buildt/sema_check.exe` (no rebuild).
- **Gate:** `git status` empty (clean working tree on entry and exit);
  `ninja -C buildt` → "no work to do"; `ctest` (22 tests) → 100% passed, both
  before and after; `tests/run_lang_tests.sh buildt` → 245 passed, 0 failed,
  0 skipped, both before and after. No regression. The gate was necessarily
  unaffected because the investigation is read-only (no `src/` edit); the
  before/after confirmation is by discipline, not because a change was
  expected.
- **Stale-probe discipline:** `probe_build.sh` deletes `probe.o`/`probe.exe`
  before each relink so g++ recompiles `probe.cpp` against the committed
  `buildt/libember.a` objects (no `libember` rebuild; the probe links fresh
  each run).
