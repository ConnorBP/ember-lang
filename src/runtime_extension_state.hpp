// runtime_extension_state.hpp — Red 8
// (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §6.6, §6.7, §10.3, §14.2):
// the ownership-safe per-runtime extension-state container that replaces the
// Red 5 placeholder `ModuleInstance::future_runtime_state` opaque pointer.
//
// This is the GREEN side of the §10.3 extension-state migration. The §4.10 /
// §10.3-listed hazard is that the lifecycle / thread / coroutine extensions
// store their mutable state in FILE-STATIC globals:
//   - ext_lifecycle.cpp: g_routines / g_free / g_mutex;
//   - ext_thread.cpp:    g_ctx / g_dispatch_base / g_slot_count / g_threads /
//                        g_threads_free / g_setup_mutex;
//   - ext_coroutine.cpp: g_ctx / g_dispatch_base / g_slot_count / g_coros /
//                        g_coros_free / g_main_fiber / g_setup_mutex +
//                        thread_local g_current_coro.
// These are PROCESS-GLOBAL singletons: two ModuleInstances in one process
// share (and clobber) the same routine table, thread registry, and fiber store.
// Keyed dispatch cannot tolerate that (§10.3), because two runtimes may carry
// different dispatch records, providers, and route words, and a file-static
// g_ctx / g_dispatch_base can only record ONE runtime's values.
//
// Red 8 moves each extension's mutable state off the file-scope globals and
// onto a per-runtime container owned by ModuleInstance. This header defines
// that container: `RuntimeExtensionState`, a plain struct with three sub-state
// structs (LifecycleState / ThreadState / CoroutineState) that mirror the
// shapes the file-static globals had, so the migration is a move, not a
// redesign. A ModuleInstance owns a `std::shared_ptr<RuntimeExtensionState>`
// (shared so an extension native running under the keyed host boundary can
// hold a reference to the current runtime's state without a process-global
// lookup, and so a spawned OS thread can capture the state pointer for its
// worker).
//
// CONSTRAINTS (§6.4, §10.3, §14.6): this container holds NO route material. It
// stores NO route word, NO expected key, NO machine fingerprint, NO key
// digest/verifier, and NO predecoded permutation. It stores only the
// extension's mutable OPERATIONAL state: the routine table, the thread
// registry, the fiber store, and the per-runtime context/dispatch-base/slot-
// count handles the keyed init APIs populate. The route word is a per-call
// transient derived at the keyed host boundary (§6.3) from the provider held
// on the ModuleInstance; it is installed in r15 for the call tree and never
// lives here. The provider REFERENCE lives on the ModuleInstance (§10.3), not
// here. A native that needs to re-resolve a logical entry (lifecycle tick,
// thread worker) constructs a DispatchKeyAdapter from the ModuleInstance's
// provider at INVOCATION/EXECUTION time and derives the route word transiently
// — it does not cache a route word here.
//
// TLS (§6.6, §10.3): the keyed host boundary (engine.cpp) sets a thread-local
// `ModuleInstance*` identifying the current keyed runtime on entry and clears
// it on every normal/trapped exit. That TLS identifies a RUNTIME; it carries NO
// route material (a bare pointer). Extension natives consult it to find the
// current runtime's RuntimeExtensionState. The legacy/identity compatibility
// wrappers (thread_init(ctx,base,count) / coroutine_init(...) /
// lifecycle::reset()) explicitly select ONE identity runtime (the file-static
// default store) WITHOUT the keyed paths consulting those singletons — the
// keyed paths use the TLS -> ModuleInstance -> ext_state chain only.
//
// This header is self-contained: it depends only on context.hpp + the stdlib,
// so it lives in the CORE `ember` lib with no ember_frontend dependency
// (one-way link direction), matching hot_reload_domain.hpp.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "context.hpp"          // context_t, TrapReason

namespace ember {

// Forward decl: the host-owned per-runtime container (src/module_instance.hpp)
// holds a shared_ptr to RuntimeExtensionState. The instance is the owner; the
// extensions hold a raw or shared reference for the duration of a call.
struct ModuleInstance;

// ─── Per-runtime lifecycle state (§10.3, replaces ext_lifecycle g_routines) ──
// Mirrors the file-static g_routines / g_free / g_mutex shape so the migration
// is a move. A 1-based-id routine table with a tombstone free-list, guarded by
// a mutex (registration/unregistration/snapshot/count are serialized so vector
// growth/tombstoning cannot race host readers). The host receives an owned
// snapshot; no lock is held while invoking a routine. Stores NO route material
// — only the routine table (logical handles + opaque data the script passed).
struct LifecycleRuntimeState {
    struct Slot {
        bool active = false;
        int64_t slot = 0;    // the fn handle (a dispatch-table logical slot from &fn)
        int64_t data = 0;    // the opaque data arg the script passed
    };
    std::vector<Slot> routines;
    std::vector<int64_t> free_ids;     // free-list of freed ids (tombstones)
    std::mutex mutex;
};

// ─── Per-runtime thread state (§6.6, replaces ext_thread globals) ──────────
// Mirrors the file-static g_threads / g_threads_free / g_setup_mutex shape,
// PLUS the per-runtime context/dispatch-base/slot-count handles (g_ctx /
// g_dispatch_base / g_slot_count) so two runtimes record their OWN values
// instead of clobbering one process-global triple. A spawned worker captures
// a pointer to THIS state (and to the owning ModuleInstance) so it resolves
// its logical entry at EXECUTION time through the current dispatch record
// (§12.4), never a raw entry cached at spawn.
//
// `instance` is a non-owning pointer back to the owning ModuleInstance (the
// instance owns this state via shared_ptr; the back-pointer is valid for the
// state's lifetime, which is the instance's lifetime). It carries NO route
// material — only the runtime identity the worker needs to reach the
// provider + dispatch record for re-resolution.
struct ThreadRuntimeState {
    struct ThreadSlot {
        std::thread        th;            // the OS thread (default-constructed if free)
        int64_t            result   = 0;
        bool               done     = false;
        bool               trapped  = false;
        int                trap_reason = 0;
        std::mutex         done_lock;
        std::condition_variable done_cv;
        bool               in_use = false;
        // The logical handle (bare dispatch slot) the script spawned with,
        // captured at spawn so the worker can RE-RESOLVE at execution time
        // (§12.4) instead of caching a raw entry.
        int64_t            logical_handle = -1;
        int64_t            arg = 0;
    };
    std::vector<std::unique_ptr<ThreadSlot>> threads;
    std::vector<int64_t>                     free_ids;
    std::recursive_mutex                     setup_mutex;
    // The per-runtime context + dispatch table the spawned threads call into.
    // Set by keyed thread init writing into THIS runtime's state. These
    // identify the runtime's dispatch storage (a base pointer + count); they
    // carry NO route material.
    context_t* ctx           = nullptr;
    void*      dispatch_base = nullptr;   // atomic<void*>[] base
    int64_t    slot_count    = 0;
    ModuleInstance* instance = nullptr;   // back-pointer for re-resolution (no route material)
};

// ─── Per-runtime coroutine state (§6.7, replaces ext_coroutine globals) ────
// Mirrors the file-static g_coros / g_coros_free / g_setup_mutex +
// g_ctx/g_dispatch_base/g_slot_count/g_main_fiber shape. §6.7: a suspended
// coroutine can retain machine registers in a fiber context; the coroutine
// contract must explicitly save/restore r15, pin the code generation, and
// avoid leaking one runtime's route state into another fiber. Until dedicated
// tests prove those invariants, coroutine_start in KEYED mode returns a TYPED
// unsupported-mode failure (§6.7 fail-closed). This state is the per-runtime
// container the migration targets; the fail-closed gate is in ext_coroutine.
//
// `last_start_status` records the typed outcome of the most recent
// coroutine_start on this runtime: when keyed mode rejects start, it sets
// unsupported_mode=true so the host can assert the typed failure rather than a
// silent wrong-behavior. Stores NO route material.
struct CoroutineRuntimeState {
    struct Coroutine {
        void*     fiber        = nullptr;   // the Windows fiber (owns the stack)
        void*     entry        = nullptr;   // JIT'd fn entry (identity mode)
        context_t* ctx         = nullptr;
        int64_t   arg          = 0;
        int64_t   yield_value  = 0;
        bool      done         = false;
        bool      started      = false;
        bool      trapped      = false;
        int       trap_reason  = 0;
        void*     caller_fiber = nullptr;
        Coroutine* caller_coro = nullptr;
        bool      in_use       = false;
    };
    std::vector<std::unique_ptr<Coroutine>> coros;
    std::vector<int64_t>                     free_ids;
    std::recursive_mutex                     setup_mutex;
    context_t* ctx           = nullptr;
    void*      dispatch_base = nullptr;
    int64_t    slot_count    = 0;
    void*      main_fiber    = nullptr;   // the thread-as-fiber
    // The typed outcome of the most recent coroutine_start on this runtime.
    struct StartStatus {
        bool ok                = false;  // a coroutine was created
        bool unsupported_mode  = false;  // keyed mode rejected start (§6.7 fail-closed)
        std::string reason;              // structured diagnostic
    };
    StartStatus last_start_status;
};

// ─── The per-runtime container owned by ModuleInstance (§10.3) ─────────────
// Plain data. A ModuleInstance holds a shared_ptr to one of these. The three
// sub-state structs are independently mutex-guarded so a lifecycle tick, a
// thread join, and a coroutine next on the SAME runtime do not contend on one
// lock (mirroring the separate file-static mutexes they replace). The
// container stores NO route material (§6.4, §10.3, §14.6): no route word, no
// expected key, no fingerprint, no digest/verifier. The provider reference
// lives on the ModuleInstance; the extensions reach it through the
// back-pointers in ThreadRuntimeState / CoroutineRuntimeState when they need
// to re-derive a route word transiently at invocation/execution.
struct RuntimeExtensionState {
    LifecycleRuntimeState  lifecycle;
    ThreadRuntimeState     thread;
    CoroutineRuntimeState  coroutine;
};

} // namespace ember
