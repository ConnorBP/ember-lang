# ember - lifecycle & routines

The native-JIT-language-equivalent of `main()` + `register_routine(cast(fn), data)`
(`RESEARCH_NOTES.md`), expressed ember's way: **annotation-based
discovery + host name/slot lookup**. Script-side first-class function
references (`&fn` / `handle(args)` / the `fn` type) shipped in v1.0
(`ROADMAP.md` Tier 2 ✓), which is what enables the dynamic
`register_routine(&fn, data)` path in Section 2 — v1's annotation model
is the static half, the dynamic-registration extension is the runtime
half, both shipped v1.0.

## 1. Entry point

A module may declare a function annotated `@entry`:

```
@entry
fn main() -> i64 {
    // setup, register handlers via returning annotation set, etc.
    return 1;
}
```

- `main` is **not** a magic name - the host opts in by querying
  `get_annotated_functions(prog, "@entry")` (declared in `src/lifecycle.hpp`;
  the `@` is optional — `"entry"` works too) and calling the (single) result.
  This keeps the entry-point convention explicit and avoids a reserved-name
  trap. If zero functions are annotated `@entry`, the host simply doesn't
  auto-call anything (a module that only registers handlers via other
  annotations doesn't need a `main`).
- Return value semantics match a typical native-JIT scripting language's lifecycle
  (`RESEARCH_NOTES.md`): `> 0` ⇒ module stays loaded; `<= 0` ⇒ module
  unloaded (host-owned teardown: free the dispatch table + globals block +
  executable pages). The host decides what "unload" means for its
  integration; ember just provides the return value. There is no
  `ember_module_destroy` facade — unload is host-owned teardown.
- `main` runs **once**, at host discretion (typically right after
  `ember_compile`), via an ordinary `ember_call_void(entry, ctx)` (or
  `ember_call_i64(entry, ctx, arg)` when an argument is passed). No special
  "init phase" in the JIT - `@entry` is pure metadata, the call is a normal
  dispatch-table call.

## 2. Routines (per-frame / per-event callbacks)

A typical native-JIT scripting language's `register_routine(cast(fn), data)` lets script dynamically
register a function to tick each frame. ember v1's equivalent is
**static** - the script declares the routine via annotation, the host
discovers and drives it:

```
@on_tick
fn update_physics(state: slice<f32>) { ... }

@event("player_hit")
fn on_player_hit(info: HitInfo) { ... }
```

- Host queries `get_annotated_functions(prog, "@on_tick")`
  once after compile, resolves each to a slot index
  (`HOT_RELOAD.md` Section 7 - caching the slot index is safe, indices never
  change), and calls it per frame via `ember_call_void(fn, ctx)` or
  `ember_call_i64(fn, ctx, arg)` (declared in `src/engine.hpp`), passing the
  routine's arguments through the dispatch table.
- **Argument passing**: the host passes whatever the routine's
  signature declares - `update_physics` takes a `slice<f32>`, so the
  host passes `(ptr, len)` per `spec/BINDING_API.md` Section 4. The host *knows*
  the routine's signature because it resolved the name through the
  module's function-info table (introspection, `planning/DESIGN.md` Section 8), not
  because the annotation encodes it (`spec/TYPE_SYSTEM.md` Section 10 - annotations
  carry literal args only, no type info; the *function signature* is
  the type contract, queried separately).
- **Dynamic registration** (script decides at runtime what to hook,
  unregister later) **shipped v1.0** as `extensions/lifecycle/`
  (`register_routine(&fn, data) -> id` / `unregister_routine(id)`), enabled
  by the Tier 2 first-class function refs that shipped in the same batch.
  The fn-handle param is typed (`is_fn_handle`) so sema rejects a forged
  plain i64; the slot's provenance was validated at the `&fn` site, so the
  host trusts it the way it trusts any sema-resolved slot and calls it via
  the dispatch table — the SAME call mechanism as the static `@on_tick`
  path, just discovered by the script at runtime. The host's
  `ext_lifecycle::host_routines()` accessor returns the `(slot, data)`
  pairs for the host to iterate + call per frame. Pinned by
  `examples/ext_lifecycle_test.cpp` (ctest target `ext_lifecycle`); demo
  script `examples/scripts/dynamic_registration.ember`. v1's static
  annotation model covers the common game case: "this function is the
  physics tick, always, for this module's whole loaded lifetime." Dynamic
  registration is for tooling/scripts that hot-swap handlers at runtime —
  rare in games, now in-tree as an extension rather than deferred.

## 3. Unload

A module unloads when:
- `@entry`'s `main` returns `<= 0` (Section 1), **or**
- the host explicitly triggers unload (host-owned teardown:
  free dispatch table + globals block + executable pages; there is no
  `ember_module_destroy` facade).

On unload: retired code pages are freed (no in-flight calls should
exist - the host's responsibility to not unload while a call is live,
`HOT_RELOAD.md` Section 4's in-flight-call analysis is about *reload*, not
*unload*; unload-while-live is a host bug, documented as such),
dispatch table freed, globals block freed. The `module_t` handle is
invalidated.

## 4. Reload interaction (`HOT_RELOAD.md`)

Single-function reload preserves globals and the replaced function's slot
index (`HOT_RELOAD.md` Sections 1/3), so an `@on_tick` routine reloaded mid-session keeps its slot,
the host's cached slot index stays valid, and the next per-frame call
picks up the new body. `@entry`/`main` is **not** re-run on reload
(it already ran at load; re-running setup code on every hot-reload
would re-initialize state the user is iteratively tweaking, defeating
the point of hot reload) - if the script author wants reload-time
re-init, they annotate a separate `@on_reload` function, which the
host opts to call after a successful `reload_function` (see `HOT_RELOAD.md`
for the `HotReloadDomain`-based migration recipe).

## 5. What this deliberately does NOT cover (cross-ref)

- **Coroutines / `yield`** (`ROADMAP.md` Tier 4) — **shipped** as the
  `coroutine` extension (`extensions/coroutine/`, `ember_ext_coroutine`):
  `coroutine_start`/`coroutine_next`/`coroutine_done` over Windows fibers, a
  `yield expr;` statement (sema marks the containing fn `is_coroutine`), and
  the `__ember_coro_yield` native the tree-walker lowers `yield` to. Pinned by
  the `tests/lang/valid_coroutine_*` lang tests. The per-frame `@on_tick` /
  `@event` routine model in §1–§2 is the *static* state-machine pattern;
  coroutines are the *sequential-looking-code* primitive on top of it. See
  `ROADMAP.md` Tier 4 (coroutines ✓ shipped).
- **Exceptions caught in-script** (`ROADMAP.md` Tier 4) — **shipped** as
  `try { ... } catch (name) { ... }` / `throw expr;` (`TryCatchStmt` /
  `ThrowStmt` in `src/ast.hpp`). A `throw` (an `i64` value for v1) unwinds to
  the nearest enclosing `catch` via the per-context catch-handler stack
  (`context_t::catch_bufs`); a throw with no enclosing `try` falls through to
  the host checkpoint as a `TrapReason::UnhandledThrow` (a recoverable runtime
  trap, not a sema error). Pinned by the `try_catch` ctest + the
  `tests/lang/valid_try_catch`/`valid_nested_try_catch`/`valid_throw_*` lang
  tests. See `ROADMAP.md` Tier 4 (exceptions ✓ shipped). Before try/catch
  shipped, a routine that faulted aborted the whole `ember_call_void`/
  `ember_call_i64` invocation via the non-local unwind
  (`spec/SAFETY_AND_SANDBOX.md` Section 2/Section 7); in-language `try`/`catch`
  now lets the script recover at a chosen frame instead.
