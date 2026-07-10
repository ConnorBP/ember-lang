# Slice-of-Stack-Local Escape Safety — Investigation & Fix Design

**Read-only investigation.** No `src/`, `extensions/`, or `examples/` files
were modified. The only artifacts created are this doc and the probes under
`tmp_edit/slice_escape/` (gitignored). The gate (`ctest` 22/22, lang 245/0/0)
was confirmed green before and after; the working tree has no `src/` changes
(`git status` shows only untracked `demo/` docs + the gitignored probes).

This is a **safety study of a real, pre-existing unsoundness** in the ember JIT
compiler's slice-escape guards. The string-encryption transient fix (commit
`e98dc87`) made the hole *more visible* — `StringLit` temps are now
stack-resident `slice<u8>` values with the same shape as the existing
`local_array_view` problem — but did **not** introduce it. The
`local_array_view` gap predates `e98dc87`; `e98dc87` just added a second,
untracked class of stack-resident slice (the StringLit temp) that slips through
the same holes.

The deliverable is: (1) empirical proof of every open escape category, for both
a `local_array_view` slice and a `StringLit`-derived slice; (2) an analysis of
the native-retain question; (3) a recommended fix with exact sema changes; and
(4) the interaction with the parallel const-mode investigation, with which this
converges.

---

## 1. The question

Does the ember sema front end reject every program in which a `slice<T>` value
whose backing store lives in a **stack frame** (a fixed-array local viewed via
`a[..]`, or an encrypted-`StringLit` temp XOR-decrypted into a frame slot)
**escapes** the frame that backs it — i.e. is stored, returned, or passed to a
callee that retains the pointer past the frame's lifetime?

The honest answer, verified by the probes below: **no**. Escape is guarded in
only **2 of 5** categories for the `local_array_view` class, and in **0 of 5**
categories for the `StringLit`-derived class (which `is_local_array_view` does
not track at all). Three of the five categories are open for *both* classes; the
`StringLit` class adds a fourth open category (return) that is guarded for
`local_array_view` but not for `StringLit`.

---

## 2. Current mechanism (cited, with line numbers at HEAD `e98dc87`)

### 2.1 `is_local_array_view` — the syntactic escape approximation

`src/sema.cpp:604-613`:

```cpp
bool is_local_array_view(const Expr& e) const {
    if (dynamic_cast<const ViewExpr*>(&e)) return true;          // L605
    if (auto* id = dynamic_cast<const Ident*>(&e)) {             // L606
        for (int i = int(scopes.size()) - 1; i >= 0; --i)
            for (const auto& v : scopes[size_t(i)])
                if (v.name == id->name) return v.local_array_view;   // L609
    }
    if (auto* t = dynamic_cast<const TernaryExpr*>(&e))           // L611
        return is_local_array_view(*t->then_e) || is_local_array_view(*t->else_e);
    return false;                                                 // L613
}
```

It tracks a `ViewExpr` over a fixed array, propagates the bit through an `Ident`
(via the `Var::local_array_view` flag, `sema.cpp:234`), and through a `Ternary`
branch. It is **set** in:

- `declare()` `sema.cpp:593-601` — the `local_array_view` parameter, threaded
  from the LetStmt initializer (`sema.cpp:1590`:
  `declare(ls->name, decl_ty, ls->is_const, is_local_array_view(*ls->init), ls->loc)`).
- `AssignExpr` `sema.cpp:1286`:
  `v->local_array_view = v->local_array_view || local_view;` — carries the bit
  through slice alias chains (`s = a[..]; return s;`).

### 2.2 The escape guards that DO exist (2 of 5)

| Category | Location | Guard |
|---|---|---|
| **C1 returned from fn** | `ReturnStmt` `sema.cpp:1601-1602` | `if (vt && vt->is_slice && is_local_array_view(*rs->value)) err("cannot return a slice/view derived from a local fixed array", ...);` |
| **C2a stored to a global (Ident target)** | `AssignExpr` `sema.cpp:1288-1290` | `if (gi != globals.end() && local_view) err("cannot store a slice/view derived from a local fixed array in a global", ...);` |

### 2.3 The escape guards that DO NOT exist (3 of 5)

| Category | Location | What runs instead |
|---|---|---|
| **C3 passed to a native** | `CallExpr` native-arg loop `sema.cpp:992-1000` (and receiver `:984`) | `check_value(c->args[i], want)` + `types_compatible` — **no `is_local_array_view` check** |
| **C2b stored to a struct field (FieldExpr target)** | `AssignExpr` `sema.cpp:1278-1290` | The guard block opens `if (auto* tid = dynamic_cast<Ident*>(a->target.get()))` — a `FieldExpr` target (`gs.data = s;`) **fails the `dyn_cast`** and falls through with **no `local_view` check** |
| **C5 passed as an arg to a script fn / fn-handle / cross-module call** | script-fn `:1100-1107`, fn-handle `:806-813`, cross-module `:887-892` | `check_value(arg, want)` + `types_compatible` — **no `is_local_array_view` check** on any of the four call-arg paths |

### 2.4 The `StringLit` class — invisible to `is_local_array_view`

`src/codegen.cpp:2367-2470` (the StringLit `eval` case): an encrypted StringLit
(`lit->encrypted && lit->baked_key != 0`) decrypts **inline into a
compiler-hidden temp frame slot** (`alloc_str_temp`, `codegen.cpp:114-119`) and
yields the slot's address as the slice ptr. The plaintext is **transient** —
it lives only in the stack frame for the expression's lifetime.

`src/sema.cpp:671-724` (the StringLit `check_expr` case): the literal resolves
to `slice<u8>` by default. It is promoted to a `string` handle (`implicit_to_string
= true`) **only** when `!expected || (expected->prim == Prim::I64 && expected->
struct_name == "string")` (`sema.cpp:716`). So:

- `let s = "lit";` (untyped) → `expected == nullptr` → **handle** (safe).
- `let s: string = "lit";` → `expected` is `string` → **handle** (safe).
- `let s: u8[] = "lit";` → `expected` is `slice<u8>` → `implicit_to_string`
  **stays false** → the slice points into a stack temp → **unsafe on escape**.

The `StringLit` AST node is **not** a `ViewExpr`, and the temp is **not** a
declared local, so `is_local_array_view` (`sema.cpp:604`) never returns true for
it. Consequently **every** escape category above is unguarded for a
`StringLit`-derived `slice<u8>` — including C1 (return) and C2a (global store),
which *are* guarded for `local_array_view`.

---

## 3. The 5-category escape matrix (filled from the probes)

Each cell is the **current** compiler's verdict, verified empirically (see §4).
"SEMA_REJECT" = rejected at sema (safe). "RUN_OK" = compiles + runs + returns the
correct value (accidentally safe — the one shipped slice-taking native copies,
or the callee reads-and-returns before the frame dies). "RUN_GARBAGE" = compiles
+ runs + returns a value that is NOT what the slice would have read had its
backing survived (proves a dangling pointer). "CRASH" = compiles + segfaults at
runtime (exit 139 — a dangling pointer that hit a guard page or corrupted state).

| Category | `local_array_view` (`let s: i64[] = a[..];`) | `StringLit`-derived (`let s: u8[] = "lit";`) |
|---|---|---|
| **C1 return** | ✅ SEMA_REJECT (guarded `:1601`) | ❌ **CRASH / RUN_GARBAGE** — `is_local_array_view` never sees the StringLit, so the return guard's `is_local_array_view(*rs->value)` is false; the dangling `slice<u8>` is returned |
| **C2a store to global (Ident)** | ✅ SEMA_REJECT (guarded `:1289`) | ❌ **CRASH** — same reason; the global slice's ptr dangles into the dead frame |
| **C2b store to struct field (FieldExpr)** | ❌ **RUN_GARBAGE** — the `Ident` dyn_cast at `:1278` fails for a `FieldExpr` target; no `local_view` check runs | ❌ **RUN_GARBAGE** — same FieldExpr-target gap, plus the StringLit is untracked |
| **C3 pass to a native** | ⚠️ RUN_OK (accidentally safe) — `string_from_slice` **copies**; guard absent but masked | ⚠️ RUN_OK (accidentally safe) — same; `string_from_slice` copies the stack-temp bytes into the host store during the call |
| **C4 closure / fn-ref capture** | N/A — ember v1 has **no closures**; `&fn` is a compile-time slot-index reification (`FnHandleExpr :744-769`), no environment capture | N/A — same |
| **C5 pass as arg to a script fn / fn-handle / cross-module call** | ❌ **CRASH** — no `is_local_array_view` guard on any of the four call-arg paths; a callee that retains the ptr (stores to a global) makes it dangle | ❌ **CRASH** — same; the StringLit is untracked too |

**Summary of the open holes:**
- **C1** and **C2a** are guarded for `local_array_view` but **open for
  `StringLit`** (the StringLit is invisible to `is_local_array_view`).
- **C2b** (struct field, FieldExpr target) is **open for both** — the
  `Ident`-target dyn_cast misses `FieldExpr` targets entirely.
- **C3** (native arg) is **open for both** but **accidentally safe today**
  because the one shipped slice-taking native (`string_from_slice`) copies. A
  retaining native would be unsound.
- **C5** (script-fn / fn-handle / cross-module arg) is **open for both** and
  **exploitable** (a retaining callee crashes).
- **C4** is **N/A** — no closures in v1.

---

## 4. Evidence (probe output)

Two evidence channels, both at HEAD `e98dc87` with string encryption ON
(`string_xor_key = 0xA5`, matching `ember_cli`):

1. **`ember_cli run <probe>.ember`** — fresh process per probe, so a segfault
   kills that process (exit 139), not the harness. This is the runtime
   crash/garbage evidence.
2. **`tmp_edit/slice_escape/slice_escape_harness.exe`** — an in-process C++
   harness (built with the task's exact g++ line) that classifies the **sema**
   boundary (does the current compiler ACCEPT or REJECT the program?) for all
   probes, and the **runtime** boundary for the probes that don't segfault. The
   harness deliberately splits "sema says OK" (the hole is open at compile time)
   from "runtime crashes/garbage" (the hole is exploitable): the sema-OK verdict
   alone is the unsoundness — a program that should be rejected is accepted.

### 4.1 `ember_cli` runs (fresh process each)

```
=== cat1_return_localview.ember ===            # C1 localview — GUARDED
ember: sema errors (1): line 7: cannot return a slice/view derived from a local fixed array
exit=2

=== cat1_return_stringlit.ember ===            # C1 StringLit — happens to read 65
exit=65                                         # (bytes still in the dead frame, not yet reused)

=== cat1_return_stringlit_dangling.ember ===   # C1 StringLit — AGGRESSIVE (scribble then read)
exit=255                                        # PROVES dangling: 255 == the 0xFF scribble sentinel, NOT 65 ('A')

=== cat2a_global_store_localview.ember ===     # C2a localview — GUARDED
ember: sema errors (1): line 8: cannot store a slice/view derived from a local fixed array in a global
exit=2

=== cat2a_global_store_stringlit.ember ===     # C2a StringLit — CRASH
Segmentation fault
exit=139

=== cat2b_struct_field_localview.ember ===      # C2b localview — GARBAGE (FieldExpr-target gap)
exit=192                                        # NOT 10; the global struct's slice field got a dangling ptr

=== cat2b_struct_field_stringlit.ember ===     # C2b StringLit — GARBAGE
exit=192                                        # NOT 72 ('H')

=== cat3_native_arg_localview.ember ===        # C3 localview — accidentally SAFE (string_from_slice copies)
exit=42

=== cat3_native_arg_stringlit.ember ===        # C3 StringLit — accidentally SAFE
exit=42

=== cat4_fnhandle_no_capture.ember ===         # C4 — N/A (no closures); fn-handle call is safe here
exit=11

=== cat5_scriptfn_arg_localview.ember ===      # C5 localview — CRASH (callee retains in a global)
Segmentation fault
exit=139

=== cat5_scriptfn_arg_stringlit.ember ===    # C5 StringLit — CRASH
Segmentation fault
exit=139
```

### 4.2 In-process harness (sema classification + safe-probe runtime)

Built with:
```
g++ -std=c++17 -O2 -Isrc -Iextensions/string \
    tmp_edit/slice_escape/slice_escape_harness.cpp \
    buildt/libember_ext_string.a buildt/libember_frontend.a buildt/libember.a \
    -o tmp_edit/slice_escape/slice_escape_harness.exe -lkernel32
```

The harness classifies each of the 11 probes. The probes that segfault when run
(`cat1_stringlit_dangling`, `cat2a_stringlit`, `cat2b_*`, `cat5_*`) are
sema-checked only in-process (their runtime crash evidence is the `ember_cli`
runs above). Key verdicts:

```
=== cat1_return_localview ===        sema+run: SEMA_REJECT -> SAFE — rejected at sema
=== cat1_return_stringlit ===       sema: SEMA_OK -> UNSAFE — sema accepts a program that segfaults/reads garbage at runtime
=== cat2a_global_store_localview ===  sema+run: SEMA_REJECT -> SAFE — rejected at sema
=== cat2a_global_store_stringlit ===  sema: SEMA_OK -> UNSAFE
=== cat2b_struct_field_localview ===  sema: SEMA_OK -> UNSAFE   (FieldExpr-target gap; ember_cli returns 192)
=== cat2b_struct_field_stringlit ===  sema: SEMA_OK -> UNSAFE
=== cat3_native_arg_localview ===    sema+run: RUN_OK (value=42) -> accidentally SAFE (string_from_slice copies)
=== cat3_native_arg_stringlit ===    sema+run: RUN_OK (value=42) -> accidentally SAFE
=== cat4_fnhandle_no_capture ===     sema+run: RUN_OK (value=11) -> accidentally SAFE (N/A category; callee reads+returns before frame dies)
=== cat5_scriptfn_arg_localview ===  sema: SEMA_OK -> UNSAFE   (ember_cli SEGFAULTS)
=== cat5_scriptfn_arg_stringlit ===  sema: SEMA_OK -> UNSAFE   (ember_cli SEGFAULTS)
```

The harness confirms the **sema boundary** is the unsoundness: every probe that
should be rejected (C1 StringLit, C2a StringLit, C2b both, C5 both) is
**SEMA_OK** today — the compiler accepts a program that crashes or reads garbage
at runtime. The two `local_array_view` probes that ARE rejected (C1, C2a
localview) and the two accidentally-safe probes (C3 both) round out the matrix.

### 4.3 The dangling pointer is real, not theoretical

`cat1_return_stringlit_dangling.ember` is the cleanest single proof. It returns
**255**, which is exactly the `0xFF` byte `scribble()` wrote into the reused
stack region — not **65** (`'A'`), which is what the slice would have read had
its backing store survived. The returned `slice<u8>`'s `ptr` field points into
`producer()`'s dead frame; after `producer()` returns, its frame is popped;
`scribble()` reuses that stack memory and fills it with `0xFF`; `main()` then
reads `s[0]` through the dangling ptr and gets `0xFF = 255`. This is a
**soundness bug**: `let s: u8[] = "literal"; return s;` compiles silently and
returns a dangling stack pointer.

---

## 5. Gap analysis

### 5.1 The `StringLit` tracking gap (the bigger half)

`is_local_array_view` (`sema.cpp:604-613`) recognizes `ViewExpr`, `Ident`,
`Ternary`. A `StringLit` that resolved to `slice<u8>` (i.e. `implicit_to_string`
is false) is **none of these**, so the function returns false. This silently
disables the two existing guards (C1, C2a) for the StringLit class and leaves
the three open categories (C2b, C3, C5) open for it too.

The fix is local: add a `StringLit` case to `is_local_array_view` that returns
true iff the StringLit's resolved type is a slice (i.e. it took the
stack-temp-slice path, not the handle path). See §6.1.

### 5.2 The FieldExpr-target gap (C2b)

The AssignExpr guard (`sema.cpp:1278-1290`) opens with
`if (auto* tid = dynamic_cast<Ident*>(a->target.get()))`. A `FieldExpr` target
(`gs.data = s;`) fails this `dyn_cast`, so the entire `local_view` block is
skipped. This bites **both** classes (the `localview` probe returns 192, not
10), not just StringLit.

The precise fix needs to know whether the `FieldExpr` target's **root base**
outlives the frame — i.e. is a global (or a field of a global, transitively). A
local struct field store (`localStruct.field = a[..];`) is only unsafe if the
local struct itself escapes, which is the harder escape analysis. For v1, the
conservative-and-correct cut is: reject a localview slice stored to a
`FieldExpr` (or `IndexExpr`) target **whose root base is a global**. See §6.2.

### 5.3 The call-arg gap (C3 + C5) — four paths, none guarded

There are **four** call-arg paths in `sema.cpp`, and **none** has an
`is_local_array_view` guard:

1. Native call args + receiver — `:984` (receiver) and `:992-1000` (the `for (size_t i = ...; i <
   c->args.size(); ++i)` loop).
2. Script-fn call args — `:1100-1107`.
3. Fn-handle (indirect) call args — `:806-813` (`for (auto& a : c->args)`).
4. Cross-module call args — `:887-892`.

Each does `check_value(arg, want)` + `types_compatible` and stops. The fix adds
the same `is_local_array_view` check after each `check_value`. See §6.3.

### 5.4 The native-retain question (C3's severity)

`NativeSig` (`sema.hpp:62-69`) carries `name`, `fn_ptr`, `ret`, `params`,
`permission` — and **no "retains/borrows" annotation**:

```cpp
struct NativeSig {
    std::string name;
    void* fn_ptr = nullptr;
    Type ret;
    std::vector<Type> params;
    uint32_t permission = 0;
};
```

**Can a native retain a slice ptr past the call today?** Yes — there is nothing
in the type system or the binding API that prevents a host native from storing
the `uint8_t*` it receives. The only reason C3 is "accidentally safe" today is
that **the one shipped slice-taking native, `string_from_slice`
(`extensions/string/ext_string.cpp:38-46`), copies** the bytes into a host-owned
`std::string` and returns a handle; it does not retain the ptr. The f-string
converter `__fstring_to_string` (`sema.cpp:820-843`) retargets to
`string_from_slice` too, so it also copies. The array extension
(`extensions/array/ext_array.cpp`) takes **handles** (`i64`), not raw slice
ptrs, so it is not a slice-retain surface.

So: **no shipped native retains a slice ptr**, but the **absence of the guard
plus the absence of a retains annotation means a third-party native could
retain one and sema would not catch it.** The fix should not depend on a
retains annotation that does not exist. See §6.3 for the recommendation
(conservative reject vs. a new annotation).

### 5.5 The closure / fn-ref question (C4 — N/A)

`FnHandleExpr` (`sema.cpp:744-769`, `ast.hpp:130`) takes `&fn` and reifies it as
**the slot index baked as an i64 literal** — a compile-time value, **not** a
runtime closure. There is **no environment capture**: the handle carries no
frame, no captured locals. `grep -rn "Closure\|capture" src/{ast,sema,parser,codegen}.hpp.cpp`
returns nothing. So a slice local **cannot** be captured by a fn handle in v1.
Category C4 is **N/A** for v1.

The fn-handle-call **arg** path (`:806-813`) does share the C5 hole (no
`is_local_array_view` guard on the arg), but it is only exploitable if the
callee retains the ptr — same as a direct named call. The probe
`cat4_fnhandle_no_capture.ember` confirms: `h(s)` where `takes_slice` reads
`s[0]` and returns before `main`'s frame dies is safe (returns 11); a retaining
callee would crash, same as C5.

---

## 6. Recommended fix (one approach, exact changes)

**Recommendation: approach (a) — extend `is_local_array_view` to cover
`StringLit`, then add the 3 missing escape guards (C2b FieldExpr-target, C3
native-arg, C5 call-arg ×4 paths). This is a REJECTION fix** — it makes the
unsafe programs compile-errors, matching the style of the two existing guards
(`:1601`, `:1289`). It is the minimal change that closes every open category
for both classes, with no new codegen lowering.

Approach (b) — force escaping StringLits to the `string`-handle path — is
**not independently sufficient** for C1/C2a where the escape target is typed
`slice<u8>` (a `fn -> u8[] { return "lit"; }` cannot return a `string` handle;
the types are incompatible), and it does nothing for the `local_array_view`
class. It is, however, the right **lowering** for the StringLit class in the
cases where the escape target IS `string`-typed — and that is exactly the
const-mode "owned" path (see §7). So the recommendation is (a) as the safety
guard, with (b) as a follow-on lowering for ergonomic StringLit-to-`string`
escapes (the convergence with the const-mode work).

Approach (c) — a general slice-lifetime/borrow analysis — is **not needed** for
v1 soundness. The syntactic `is_local_array_view` approximation plus the 5
guards is sufficient because the only producers of stack-backed slices are
`ViewExpr`-over-fixed-array and the StringLit temp, both of which the
approximation can be taught to see. A real borrow checker is a larger, separate
investment; the 5-guard rejection set is the conservative sound floor.

### 6.1 Extend `is_local_array_view` to cover `StringLit` (closes C1+C2a for StringLit)

`src/sema.cpp:604-613` — add a `StringLit` case that returns true iff the
literal resolved to a slice (i.e. it did NOT take the `implicit_to_string`
handle path):

```cpp
bool is_local_array_view(const Expr& e) const {
    if (dynamic_cast<const ViewExpr*>(&e)) return true;
    // A StringLit that resolved to slice<u8> (not promoted to a `string`
    // handle via implicit_to_string) backs into a compiler-hidden stack
    // temp frame slot (codegen's alloc_str_temp) — same lifetime shape as a
    // ViewExpr over a fixed array. Track it so the existing escape guards
    // (return, global-store) and the three added below cover it too.
    if (auto* sl = dynamic_cast<const StringLit*>(&e))
        return sl->ty && sl->ty->is_slice;
    if (auto* id = dynamic_cast<const Ident*>(&e)) {
        for (int i = int(scopes.size()) - 1; i >= 0; --i)
            for (const auto& v : scopes[size_t(i)])
                if (v.name == id->name) return v.local_array_view;
    }
    if (auto* t = dynamic_cast<const TernaryExpr*>(&e))
        return is_local_array_view(*t->then_e) || is_local_array_view(*t->else_e);
    return false;
}
```

The `sl->ty` field is set in the StringLit `check_expr` case (`sema.cpp:674,
724`) before any guard could run (guards run after `check_value`/`check_expr`
returns), so it is populated. For a StringLit promoted to `string`,
`sl->ty->is_slice` is false, so the function returns false — correct (the handle
is owned and safe). This single line makes the existing C1 (`:1601`) and C2a
(`:1289`) guards fire for StringLit too.

**Trivial.** ~3 lines.

### 6.2 C2b — guard the FieldExpr (and IndexExpr) target whose root base is a global

`src/sema.cpp:1278-1290` — after the existing `Ident`-target block, add a block
that handles `FieldExpr`/`IndexExpr` targets by chasing to the root `Ident` and
checking whether it is a global:

```cpp
if (auto* tid = dynamic_cast<Ident*>(a->target.get())) {
    // ... existing Ident-target block unchanged ...
} else if (local_view) {
    // The target is a FieldExpr (gs.data = s;) or IndexExpr (garr[0] = s;).
    // Chase to the root base; if it is a GLOBAL, the store escapes the frame.
    // (A local-struct/local-array target is only unsafe if the local itself
    // escapes — that is the harder escape analysis; for v1 we conservatively
    // reject only the global-rooted case, matching the existing global-store
    // guard's intent.)
    const Expr* root = a->target.get();
    while (auto* fe = dynamic_cast<const FieldExpr*>(root)) root = fe->base.get();
    while (auto* ix = dynamic_cast<const IndexExpr*>(root)) root = ix->base.get();
    if (auto* rid = dynamic_cast<const Ident*>(root)) {
        if (!lookup_local_var(rid->name)) {
            auto gi = globals.find(rid->name);
            if (gi != globals.end())
                err("cannot store a slice/view derived from a stack local into a "
                    "field/element of a global '" + rid->name + "'",
                    a->loc.line, a->loc.col);
        }
    }
}
```

**Moderate.** ~15 lines. The harder follow-on — rejecting a `localStruct.field
= a[..];` where `localStruct` is later returned or stored to a global — needs the
struct-escapes analysis (§8); the v1 cut above closes the **global-rooted** case,
which is what the probes exploit.

### 6.3 C3 + C5 — guard all four call-arg paths

Add the same check after each `check_value(arg, want)` on the four call-arg
paths. A single helper keeps it DRY:

```cpp
void check_arg_no_local_view_escape(const CallExpr& c, const Expr& arg, size_t i) {
    if (arg.ty && arg.ty->is_slice && is_local_array_view(arg))
        err("cannot pass a slice derived from a stack local to '" + c->name +
            "' (arg " + std::to_string(i+1) + "); the callee may retain the "
            "pointer past the frame. Use a `string` handle or copy to a slice "
            "backed by rodata/globals.", arg.loc.line, arg.loc.col);
}
```

Call sites (after each `check_value` + `types_compatible`):
- Native receiver `:984` and native args `:992-1000` — C3.
- Script-fn args `:1100-1107` — C5.
- Fn-handle args `:806-813` — C5 (indirect).
- Cross-module args `:887-892` — C5.

**Native-retain decision (C3):** the recommendation is the **conservative
reject** — reject any `local_array_view` (or StringLit-slice) passed to ANY
native, with the error above. Rationale: (1) `NativeSig` has no retains
annotation today, and adding one + auditing every host native is a larger change
than the guard; (2) the one shipped slice-taking native (`string_from_slice`)
**copies** — a caller who wants the copy already passes a `slice<u8>` and gets a
`string` handle back; if the caller instead has a `local_array_view` slice, the
fix says "materialize it to a `string` handle first (via `string_from_slice` on
a non-escaping copy) or to a rodata/global-backed slice, then pass that." This
makes the unsafe programs compile-errors with a clear message, matching the
existing guards' style. A future "borrowing native" annotation could relax C3
for known-copier natives, but that is an **ergonomic follow-on**, not a
soundness requirement — the conservative reject is sound.

**Trivial-to-moderate.** ~1 line per call site (the helper is ~5 lines); 4
sites.

### 6.4 No codegen changes

Approach (a) is sema-only. Codegen is unchanged: the programs that previously
compiled-and-crashed now fail at sema; the programs that still compile are
exactly the ones whose slices do not escape (and are therefore safe — the
backing store outlives every use that reaches it within the frame).

### 6.5 Summary of the fix's scope

| Hole | Fix location | Effort |
|---|---|---|
| StringLit invisible to `is_local_array_view` (C1/C2a/C2b/C3/C5 for StringLit) | `is_local_array_view` `:604` — add `StringLit` case | trivial (~3 lines) |
| C2b FieldExpr-target (both classes) | AssignExpr `:1278` — add FieldExpr/IndexExpr root-base-is-global block | moderate (~15 lines) |
| C3 native-arg (both classes) | native-arg loop `:992` + receiver `:984` — add guard | trivial (~1 line/site) |
| C5 script-fn / fn-handle / cross-module arg (both classes) | `:1100`, `:806`, `:887` — add guard | trivial (~1 line/site) |
| C1, C2a for StringLit | (closed automatically by 6.1) | — |
| C1, C2a for local_array_view | already guarded | — |
| C4 closure capture | N/A (no closures in v1) | — |

The 3 of 5 missing guards the task names: **C2b (FieldExpr) and C5 (call-arg) are
trivial to add** (the FieldExpr one needs the root-base chase, ~15 lines; the
call-arg ones are ~1 line each via a helper). **C3 (native-arg) is trivial to
add as a conservative reject** but would benefit from a "borrowing native"
annotation as a follow-on ergonomic relaxation — that is the part that "needs
real work" only if the goal is to keep C3's accidentally-safe copier natives
working without forcing the caller to materialize first.

---

## 7. Interaction with the const-mode investigation

The parallel `demo/STRING_CONST_MODE_INVESTIGATION.md` studies the
const-vs-owned classification that commit `e98dc87` skipped. Its central finding
(reproduced in its §"The convergence"): an escaping StringLit forced to a
`string` handle **is** the const-mode "owned" path — decode-once-to-permanent.
The two investigations **converge**:

- **This doc** establishes the **safety floor**: every escaping stack-backed
  slice (both classes) must be rejected at sema (approach a), with the 5 guards
  above. This is the soundness requirement; it does not depend on the const-mode
  decision.
- **The const-mode doc** establishes the **ergonomic lowering** for the
  StringLit class: where the escape target is `string`-typed, the escaping
  StringLit should take `implicit_to_string` (the owned handle path) rather than
  be rejected. That is approach (b) here, applied only where the types permit
  (a `fn -> string { return "lit"; }` can return a handle; a `fn -> u8[] {
  return "lit"; }` cannot and must be rejected).

The recommended ordering: **land (a) first** (the safety floor — makes every
unsafe program a compile-error). Then **layer (b) on top** as the const-mode
owned-path lowering: teach the StringLit `check_expr` case (`sema.cpp:716`) to
set `implicit_to_string` not just on `expected == string || expected == nullptr`
but also when the StringLit is in an **escaping context** (return into a
`string`, store to a `string` global, arg to a `string`-typed param) — so those
specific escapes become owned handles instead of rejections. (a) remains the
backstop for the cases (b) cannot reach: the `slice<u8>`-typed escape targets
and the entire `local_array_view` class.

The convergence point is exact: **(a) is the safety guard; (b) is the lowering
that makes the StringLit-to-`string` escapes ergonomic rather than rejected.**
The const-mode investigation's "owned path" and this doc's "approach (b)" are
the same code change (`implicit_to_string` set on escaping-context StringLits),
arrived at from two directions (safety vs. lowering).

---

## 8. Scope limits & follow-ons

1. **Struct-escapes analysis (the harder half of C2b).** The fix in §6.2 closes
   the **global-rooted** FieldExpr-target case. A `localStruct.field = a[..];`
   where `localStruct` is a **local** that is later returned or stored to a
   global is not closed by §6.2 — closing it needs to know whether the local
   struct escapes, which is a small escape analysis over the local's subsequent
   uses. This is a **follow-on**, not a blocker: the probes that exploit C2b
   today all use a global-rooted target, which §6.2 closes. The remaining case
   (local struct that escapes) is rarer and can be addressed by extending
   `is_local_array_view` to track "this local struct contains a localview slice
   field" and then checking that local struct at its own escape points (return,
   global-store, call-arg) — reusing the same 5 guards at the struct level.

2. **A "borrowing native" annotation for C3.** The conservative reject in §6.3
   forces a caller with a `local_array_view` slice to materialize it before
   passing to `string_from_slice` (which copies anyway). Annotating `NativeSig`
   with a `bool borrows` / `enum class ArgLifetime { Borrow, Copy, Retain }` and
   allowing `Borrow`/`Copy` natives to receive a localview slice (the native
   does not retain past the call) would restore the accidentally-safe copier
   path without the caller materializing. This is an **ergonomic follow-on**;
   the conservative reject is sound without it.

3. **The `Ternary` propagation for StringLit.** `is_local_array_view` propagates
   through `Ternary` branches (`:611`). With §6.1, a `Ternary` whose branches are
   StringLit-slices is covered. A `Ternary` whose one branch is a `local_array_view`
   Ident and the other is a StringLit is also covered (both halves return true).
   No additional change.

4. **No closures in v1 (C4).** If a future tier adds closures that capture
   slice-typed locals, C4 becomes live and would need its own escape guard at
   the capture site. That is out of scope for v1.

---

## 9. Artifacts

- `tmp_edit/slice_escape/cat1_return_localview.ember` ... `cat5_scriptfn_arg_stringlit.ember`
  — the 13 `.ember` probes (one per cell, plus the aggressive dangling proofs).
- `tmp_edit/slice_escape/slice_escape_harness.cpp` + `slice_escape_probes.inc` —
  the in-process C++ harness (built with the task's exact g++ line).
- `tmp_edit/slice_escape/slice_escape_harness.exe` — the built harness.

All under `tmp_edit/` (gitignored). No `src/`, `extensions/`, or `examples/`
files were modified. Gate confirmed green (`ctest` 22/22, lang 245/0/0) before
and after.
