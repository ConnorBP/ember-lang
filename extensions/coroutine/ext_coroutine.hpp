// ext_coroutine.hpp - ember extension: coroutines with yield (#21).
//
// An ember *extension* (see ember/extensions/README.md): a reusable,
// non-cheat-specific addon. A coroutine is a function that can `yield` —
// suspend execution + hand a value to the caller, then resume on the next
// `coroutine_next()` call. Mirrors ext_thread's host-setup shape (1-based
// handles, register_natives/init/reset), but the ONE structural difference is
// the store holds Windows fibers (cooperative same-thread coroutines with
// their own stacks) instead of OS threads.
//
// === SCOPE (single-threaded cooperative coroutines) =======================
//
// Coroutines run on the SAME OS thread as the caller (Windows fibers are
// cooperative, not preemptive — SwitchToFiber is a direct stack/context
// switch, no OS scheduler involvement). There is therefore NO concurrency
// with the caller and NO call_mutex contention: the caller is frozen at the
// SwitchToFiber call site while the coroutine runs, exactly like a regular
// call frame, just on a separate stack. This is simpler + safer than the
// in-context-thread addon (ext_thread): no shared mutable state, no locking.
//
// coroutine_start(fn_handle, arg) -> coroutine_handle
//   Creates a coroutine that will run `fn` (a script fn, resolved via the
//   dispatch table) with the single i64 `arg`. Does NOT start it yet — the
//   fn runs to its first yield on the first coroutine_next. Returns a 1-based
//   handle (0 = creation failed). fn_handle is a bare intra-module dispatch
//   slot (a `&fn` value); a cross-module handle (bit 63 set) is rejected.
//
// coroutine_next(handle) -> i64
//   Resumes the coroutine: on the first next() it starts the fn (running to
//   the first yield); on subsequent next()s it resumes after the last yield.
//   Returns the yielded value. When the fn RETURNS (the coroutine is done),
//   coroutine_next returns the fn's return value once + sets the done flag;
//   further coroutine_next calls on a done coroutine return the last value
//   (a finished coroutine yields no more — the script should check
//   coroutine_done before calling next again, mirroring how a generator
//   protocol works).
//
// coroutine_done(handle) -> bool
//   Returns true once the coroutine's fn has returned (the coroutine is
//   finished + will yield no more values). Returns false while it is still
//   suspended at a yield (more values available).
//
// `yield value;` (inside a coroutine fn) is lowered by codegen to a call to
// the internal native __ember_coro_yield(value): it stores the value into the
// current coroutine + SwitchToFiber back to the caller's fiber. When the
// caller resumes the coroutine (coroutine_next -> SwitchToFiber to the coro
// fiber), __ember_coro_yield returns and the fn continues after the yield.
//
// === HOST SETUP ============================================================
//
// Natives only receive i64 args; they do NOT receive the context_t*. So the
// host MUST call coroutine_init(ctx, dispatch_base, slot_count) once before
// any coroutine_start, registering the context + dispatch table the coroutines
// will call into (mirrors ext_thread's thread_init). The host also calls it to
// convert the calling thread to a fiber (fibers require the thread to be a
// fiber first — ConvertThreadToFiberEx — so GetCurrentFiber() returns the
// caller's fiber). coroutine_reset() clears the registry + frees every
// coroutine's fiber + stack (the host calls it between independent runs).
//
// === CORRECTNESS MODEL =====================================================
//
// Fibers are cooperative + same-thread: SwitchToFiber is a direct register +
// stack switch with no OS scheduler involvement, so the coroutine runs to its
// next yield (or return) before control returns to the caller. The caller's
// in-progress ember_call state (budget/depth/catch/checkpoint) is frozen at
// the SwitchToFiber site inside coroutine_next; the coroutine's fn runs via
// its OWN ember_call_i64 (in the trampoline), which installs r14 = ctx (the
// same context_t* — coroutines share the caller's context, exactly like a
// nested script call). A trap inside the coroutine unwinds to the trampoline's
// OWN checkpoint (setjmp), which marks the coroutine done + switches back to
// the caller (the trap does NOT unwind to the caller's checkpoint — the
// coroutine is isolated by its own setjmp, mirroring ext_thread's worker).
//
// r14 (the context register) is preserved across SwitchToFiber: the fiber
// switch saves + restores ALL registers (it is a full context switch), so the
// coroutine fn sees the same r14 = ctx the caller had, AND ember_call_i64's
// thunk re-installs it explicitly anyway. The caller's r14 is restored when
// SwitchToFiber switches back.

#pragma once
#include "ast.hpp"             // Type, NativeSig, NativeTable
#include "binding_builder.hpp" // BindingBuilder
#include "context.hpp"         // context_t
#include <cstdint>

namespace ember::ext_coroutine {

// Register the coroutine natives (coroutine_start/next/done + the internal
// __ember_coro_yield) into the host's native table. Called once at host
// startup, like every other extension's register_natives.
void register_natives(std::unordered_map<std::string, NativeSig>& m);

// Host setup: record the context + dispatch table the coroutines call into,
// and convert the calling thread to a fiber (idempotent — a no-op if the
// thread is already a fiber). Returns false on a bad argument (null ctx /
// null dispatch_base / non-positive slot_count); returns true otherwise (the
// fiber conversion itself is best-effort — if it fails because the host
// already converted the thread elsewhere, GetCurrentFiber still works).
bool coroutine_init(ember::context_t* ctx, void* dispatch_base, int64_t slot_count);

// Clear the coroutine registry: delete every coroutine's fiber (which frees
// its stack) + drop the slot. Called by the host between independent runs
// (mirrors ext_thread::thread_reset / ext_sync::reset). Safe to call when the
// registry is empty. Does NOT convert the fiber back to a thread (the thread
// stays a fiber for the process lifetime — converting back + re-converting on
// the next run is fragile across N `ember test` iterations; staying a fiber
// is correct + free).
void coroutine_reset();

} // namespace ember::ext_coroutine
