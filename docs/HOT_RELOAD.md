# ember - hot reload and executable-page reclamation

ember ships atomic single-function replacement plus a host-visible epoch
reclamation domain. Added/removed functions and whole-module transactions
remain deferred.

## 1. Dispatch table layout and slot stability

```cpp
struct DispatchTable {
    std::vector<std::atomic<void*>> slots;
};
```

- One table normally belongs to one module. A function receives its stable slot
  during initial registration; reload never renumbers slots.
- Generated script-to-script calls read the slot for every call. Existing stack
  frames continue in their already-selected code version.
- `DispatchTable::set` is a `std::memory_order_release` store and `get` is a
  `std::memory_order_acquire` load. Finalized RX code and its bytes therefore
  happen-before a caller that acquires the published entry.
- A host may cache a function name or stable slot index. An unguarded cached raw
  entry pointer is unsupported because reclamation can invalidate it.

## 2. Added/removed functions (deferred)

`reload_function` replaces exactly one function already present in the module.
Adding functions, growing a live table, and replacing removed functions with a
trap stub require a future whole-module reload transaction.

## 3. Single-function publication protocol

The public helper takes the module state and its reclamation domain:

```cpp
ReloadResult reload_function(source, program, table, domain, codegen_ctx,
                             natives, overloads, struct_layouts);
```

1. Lex and parse exactly one complete function declaration.
2. Find its existing stable slot and verify the canonical call ABI: arity,
   ordered parameter types/word shapes, and return type/word shape must match.
3. Temporarily install the replacement AST for whole-module sema, compile it,
   allocate a fresh page, and seal it RX. Any lex, parse, signature, sema,
   codegen/finalize, or publication-preparation failure restores the old AST.
   The table and domain epoch remain unchanged.
4. `HotReloadDomain::publish` records the replaced page for retirement, release-
   stores the new entry into the slot, and advances the domain's monotonic epoch
   exactly once. Recording retirement is prepared before publication, so an
   allocation failure cannot publish an untracked replacement.
5. `ReloadResult` reports `publication_epoch`, `retirement_epoch`, and
   `old_page_retired`, and carries the current `new_fn`. It intentionally does
   **not** return an owning old-page pointer: the old executable allocation has
   transferred to the domain and must not be freed by the caller. A host that
   keeps `CompiledFn` bookkeeping must clear/remove the replaced record; the
   domain is its sole executable-page owner after successful publication.

The current `CompiledFn` remains host-owned while it is published. On its next
successful replacement, its executable page transfers to the domain. The host
continues to own current pages when tearing down a module, after first ensuring
there are no calls and destroying/quiescing its domain.

## 4. Execution guards and in-flight calls

Every **outer host-to-script invocation** that can use a reloadable dispatch
table must be guarded from before loading the entry until after JIT code
returns:

```cpp
auto guard = module.reload_domain.guard();
void* entry = module.table.get(slot); // acquire load while guarded
return reinterpret_cast<Fn>(entry)(args...);
```

The same rule applies to raw B1 context thunks: create the guard before loading
an entry and retain it across `ember_call_*`. `context_t` does not contain epoch
state; contexts and reclamation domains have independent lifetimes and there is
no global participant registry.

One outer guard covers all nested script-to-script calls and recursion because
it remains active for the entire call tree. A native that returns to the same
outer script invocation is also covered. A separate host re-entry after the
outer invocation has returned needs its own guard.

A frame already executing an old version at publication continues normally.
Fresh calls through the slot can observe the new version. In particular,
recursive calls made after publication may enter the new version while older
recursive frames finish in the old version. This per-call identity is deliberate.

Do not load an entry before entering the guard, retain it after leaving the
guard, or invoke a raw cached pointer later. Such pointers are unsupported and
may refer to a reclaimed page.

## 5. Epoch reclamation API

`HotReloadDomain` is a small host-owned domain; normally it is stored beside the
dispatch table. It uses a mutex/condition variable, has no background thread,
and performs no process-global registration.

- `domain.guard()` returns a move-only RAII `ExecutionGuard`. Guard entry
  records the current epoch under the same mutex that serializes publication.
- A successful publication changes epoch `E-1 -> E` and retires the replaced
  page at `E`.
- A retired page at `E` is eligible when no active guard has an entry epoch less
  than `E`. Publication sets the epoch and release-stores the slot while holding
  the domain mutex; a guard enrolled at `E` or later is therefore serialized
  after that store and does not pin the old page.
- `domain.reclaim()` is nonblocking. It frees every currently eligible retired
  page and returns the number freed.
- `domain.quiesce()` snapshots the current epoch, waits until pages retired
  through that epoch are eligible, frees them, and returns the number freed.
  Publications after its snapshot are not part of that wait.
- `epoch()`, `retired_page_count()`, and `reclaimed_page_count()` provide host-
  visible status/test instrumentation.
- Domain destruction performs a final `quiesce()`. The domain must outlive all
  of its guards. Destruction does not own or free entries still current in the
  table.

The domain mutex provides the crucial enrollment/publication ordering; the
release/acquire dispatch slot publishes executable data. Guard exit notifies
blocking quiescers. Page freeing occurs while domain state is locked, after the
eligibility predicate proves no pre-publication outer call remains.

For an uncomplicated single-threaded host, publication occurs outside a script
call and `quiesce()` immediately afterward reclaims the replaced page without
waiting. Concurrent hosts can use `reclaim()` opportunistically or call
`quiesce()` at an explicit lifecycle boundary.

## 6. Whole-module reload (deferred)

No whole-module reload API ships. Transactional batch replacement, added slot
assignment, removed-function trap stubs, global-layout migration, and atomic
multi-function version sets remain deferred. Single-function reload does not
change globals or declarations beyond the replacement function body.

## 7. Host integration checklist

1. Keep one `HotReloadDomain` beside each reloadable table (or deliberately use
   one domain for several tables and guard all outer calls through it).
2. Enter a guard before every host slot load and keep it through script return.
3. Publish only through `reload_function`/the domain; never write a reloadable
   slot and free its prior entry manually.
4. After success, remove the replaced page from caller-owned `CompiledFn`
   bookkeeping; consume `new_fn` as the new current page.
5. Use `reclaim()` or `quiesce()` to perform actual frees. At shutdown, stop new
   calls, drain guards, quiesce the domain, then free current pages/table state.
