// ext_thread.hpp - ember extension: in-context threads (Tier 4).
//
// An ember *extension* (see ember/extensions/README.md): a reusable,
// non-cheat-specific addon. Host-owned thread registry behind opaque i64
// handles. Mirrors ext_array/ext_sync's shape (1-based handle, slot(h)
// bounds check, register_natives/reset), the ONE structural difference is
// the store holds OS threads that call ember back into the SAME context_t.
//
// === SCOPE (in-context threads; the residual parallelism case) =============
//
// Multi-context parallelism already ships (per-thread context_t, r14 context
// register, sync queue primitives). That keeps the script side single-threaded
// per context (the U2 contract). This addon is the residual case for
// compute-heavy mods that need parallelism WITHIN ONE script context: two
// ember-calling OS threads sharing ONE context_t.
//
// thread_spawn(fn_handle, arg) -> thread_id
//   Spawns a native OS thread that calls the fn (resolved via the dispatch
//   table) on the SAME context_t as the caller, passing the single i64 arg.
//   The context_t's coarse `call_mutex` serializes the script-side execution:
//   the spawned thread locks it around its ember_call, so it cannot run
//   concurrently with another ember_call on the same context. The spawned
//   thread SAVES + RESTORES the per-call fields (budget/depth/catch/checkpoint)
//   around its call so the caller's in-progress state survives the interleaving.
//
// thread_join(thread_id) -> i64
//   Waits for the spawned thread to finish + returns its i64 result. If the
//   thread trapped (budget/stack/bounds/illegal/unhandled-throw), the trap
//   unwinds to the THREAD's host boundary (its own setjmp on the shared
//   context), NOT the caller's; thread_join returns INT64_MIN to signal the
//   thread trapped, and the trap reason is available via thread_trap_reason.
//   thread_join releases call_mutex while it waits, letting the spawned call
//   acquire it and run to completion (the coarse serialization is deadlock-free
//   because join is the one native that yields the lock).
//
// thread_trap_reason(thread_id) -> i64
//   Returns the TrapReason (as an i64) the spawned thread recorded, or 0
//   (None) if it completed normally. For inspecting a trapped thread.
//
// ATOMICS for shared mutable state across in-context threads are the SYNC
// extension's aint8/16/32/64 (atomic_new/load/store/fetch_add/fetch_sub/cas/
// swap), already exposed to scripts via ext_sync::register_natives. A script
// that wants in-context shared state registers BOTH the sync + thread addons
// and coordinates through the atomic handles (which are host-storage, fully
// thread-safe independent of the context). This addon depends on ext_sync
// only at the script level (the test harness registers both); there is no
// C++ link dependency.
//
// === HOST SETUP ============================================================
//
// Natives only receive i64 args; they do NOT receive the context_t*. So the
// host MUST call thread_init(ctx, dispatch_base, slot_count) once before any
// thread_spawn, registering the context + dispatch table the spawned threads
// will call into. This mirrors how ext_sync's host accessors are a separate
// host-side entry point. thread_reset() clears the registry (the host calls
// it between independent runs, like ext_sync::reset). The dispatch_base +
// slot_count let thread_spawn resolve a fn_handle (a bare dispatch slot) to
// the JIT entry + validate it against the registered range (REDSHELL guard:
// a forged/out-of-range handle returns thread_id 0 instead of calling a wild
// pointer). Only intra-module handles (bare slots, bit 63 clear) are accepted;
// a cross-module handle (bit 63 set) returns 0 (cross-module thread spawn is a
// non-goal for v1 — the spawned thread has no records-table plumbing).
//
// === CORRECTNESS MODEL =====================================================
//
// The single context_t has one checkpoint/budget/depth/catch-stack. Two
// concurrent ember_calls would race those. call_mutex serializes: only one
// thread is ever inside ember_call on this context. The spawned thread:
//   1. locks call_mutex
//   2. saves budget/depth/catch/checkpoint/has_checkpoint/last_error
//   3. reset_for_call + setjmp(checkpoint) [its own recovery point]
//   4. ember_call_i64(entry, ctx, arg)  [r14=ctx; trap -> longjmp to #3]
//   5. restores everything saved in #2
//   6. unlocks call_mutex
// The caller (host's outer call OR a sibling spawned thread) is blocked on
// call_mutex the whole time, so its in-progress state is frozen at a native
// call boundary; the save/restore in #2/#5 makes its resumption see the exact
// state it left. thread_join releases call_mutex while it waits (cv.wait),
// handing the lock to the spawned thread; when the spawned thread unlocks +
// notifies, join reacquires. No deadlock: the only native that yields the lock
// is join, and join yields only while waiting for a thread that needs the lock.
// ===========================================================================
#pragma once
#include "sema.hpp"
#include "context.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_thread {

// Register the thread natives (thread_spawn, thread_join, thread_trap_reason)
// into m. No operator overloads, so no register_overloads().
void register_natives(std::unordered_map<std::string, NativeSig>& m);

// Host setup: register the context + dispatch table the spawned threads call
// into. MUST be called once before any thread_spawn. dispatch_base is the
// DispatchTable::base() (an array of atomic<void*>; thread_spawn reads
// [dispatch_base + slot*8] under acquire). slot_count bounds the fn-handle
// range (the REDSHELL guard). Returns false if any argument is null/zero.
bool thread_init(ember::context_t* ctx, void* dispatch_base, int64_t slot_count);

// Clear the thread registry + join/detach any live threads (host isolation
// between runs, mirrors ext_sync::reset). After this, thread_init must be
// called again before the next thread_spawn.
void thread_reset();

} // namespace ember::ext_thread
