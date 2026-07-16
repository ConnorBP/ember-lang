# Ember lifecycle and routines

Ember lifecycle is a host convention built from function annotations, stable
dispatch identity, and optional dynamic routine registration. An annotation is
metadata; it does not make the JIT call a function automatically.

## 1. Static annotation discovery

```rs
@entry
fn start() -> i64 {
    return 1;
}

@on_tick
fn update(frame: i64) -> void {
}

@event("player_hit")
fn on_player_hit(amount: i64) -> void {
}
```

`src/lifecycle.hpp` provides:

```cpp
get_annotated_functions(program, "entry");
get_annotated_functions(program, "@on_tick");
get_event_handlers(program, "player_hit");
get_entry_function(program);
```

The leading at sign is optional in query strings because the parser stores the
annotation name without it. `AnnotatedFn` contains the function name, stable
slot, return type, parameter types, and annotation literal arguments.

The helpers inspect `Program` metadata. A host loading only an EMBL `.em` file
has an export directory and stored entry slot, but not the full arbitrary
annotation list. Preserve the `Program` or store application-specific lifecycle
metadata if arbitrary annotation discovery is required after source is gone.

## 2. Entry behavior

`entry` is the static startup convention. A module may have no entry function.
The CLI also uses `main` as a fallback when creating/running ordinary artifacts.

The JIT does not assign inherent meaning to an entry return value. A host may
adopt this lifecycle policy:

- result greater than zero: keep the module loaded;
- result zero or less: unload it.

That is a host decision, not automatic module destruction. Ember has no single
`ember_module_destroy` facade. The host owns current executable pages, globals,
dispatch storage, linked modules, extension stores, and reclamation-domain
shutdown ordering.

Call the entry through the ordinary context-aware call boundary and, for a
reloadable module, hold the module's reload guard before loading its entry.

## 3. Static tick and event routines

A typical host performs discovery once after compilation:

1. query `on_tick` or event handlers;
2. retain each stable logical slot and signature;
3. marshal arguments according to the function type;
4. enter the reload guard;
5. resolve the current entry and call it with a valid `context_t`.

Caching a logical slot is supported. Caching a raw executable pointer outside a
reload guard is not.

For slices, the host passes the two-word pointer/length ABI. Struct and float
arguments follow the documented Win64 ABI. The annotation does not encode a
signature; use `AnnotatedFn`/module export metadata.

## 4. Dynamic routine registration

The lifecycle extension exposes:

```rs
let id = register_routine(&update_one, data);
unregister_routine(id);
```

`register_routine` accepts a function-handle value and data, stores the logical
slot, and returns a host-owned routine ID. `host_routines()` returns `(slot,
data)` pairs for the host to drive. Registration does not start a scheduler or
background thread.

Function-handle provenance is established by sema and the runtime call-target
guard. A trusted host should still bounds-check the returned slot against the
active module before calling it.

The keyed lifecycle API adds per-runtime state:

- `lifecycle_init_keyed(ModuleInstance&)`
- `host_routines_keyed(ModuleInstance&)`
- `lifecycle_tick_keyed(ModuleInstance&, context_t&, DispatchKeyAdapter&)`

Keyed ticking resolves the logical handle through the module's immutable
keyed-dispatch record and transient route adapter. It does not assume the
logical slot is a physical dispatch index.

## 5. Tick threads and concurrent entry

The CLI's `--tick` mode drives static `on_tick` handlers and dynamically
registered routines on a tick thread. Current concurrent execution uses a
separate per-call `context_t` for each worker while sharing compiled code,
dispatch state, globals, and the context-owned GC runtime.

Per-call state includes:

- trap checkpoint and last error;
- budget and call depth;
- catch stack and thrown value;
- GC shadow-stack head;
- thread-local execution bookkeeping.

A shared `context_t` object is the configuration/ownership anchor, not one
mutable checkpoint reused simultaneously by all workers. The thread extension
creates worker call contexts seeded from the host settings and attaches them to
the shared GC heap. `call_mutex` remains for source compatibility but does not
serialize the current concurrent-entry path.

## 6. Coroutines

Coroutines are independent of lifecycle annotations. A function containing
`yield` can be driven through:

- `coroutine_start`
- `coroutine_next`
- `coroutine_done`

The Windows implementation uses fibers. A host may call a coroutine from an
entry, tick, event, or dynamic routine, but annotations do not automatically
create coroutine instances. For keyed modules, initialize/use the keyed
coroutine integration rather than treating a logical handle as a physical slot.

## 7. Exceptions and traps

`try`/`catch` handles Ember `throw` values inside script code. A throw without a
matching script catch becomes `TrapReason::UnhandledThrow` at the host
checkpoint.

Sandbox traps such as bounds failure, division failure, budget exhaustion, or
bad call target are host-level nonlocal unwinds. They do not run ordinary
script `defer` cleanup. The host must install the documented checkpoint/trap
stub around an outer call and perform host-owned cleanup after a trap.

## 8. Reload interaction

### Ordinary identity dispatch

`reload_function` preserves the existing logical slot and exact call ABI.
Static and dynamic routines that cache that slot observe the new body on their
next guarded call. A frame already executing the old body may finish there.

The entry function is not automatically re-run after reload. If an application
uses an `on_reload` annotation, the host must discover and invoke it explicitly
after successful publication.

### Keyed dispatch

`reload_keyed_function` preserves logical routine identity and publishes the
replacement at the derived physical route after verifying ABI, visibility,
calling mode, and dispatch domain. `replace_keyed_generation` may replace an
entire keyed topology under the stable module ID.

Lifecycle code must resolve logical handles each time through the current
module record; it must not cache a physical keyed index across generations.

## 9. Unload ordering

Before destroying a module:

1. stop new entry/tick/event/routine calls;
2. join worker threads and finish/abandon coroutine instances according to the
   extension contract;
3. drain reload guards;
4. quiesce the reload domain;
5. detach/reset GC and extension state as appropriate;
6. release current executable pages, linked-module handles, globals, dispatch
   and keyed record backing.

Destroying a module while script code is in flight is a host bug. Epoch
reclamation protects replaced pages; it is not a substitute for module-lifetime
coordination.
