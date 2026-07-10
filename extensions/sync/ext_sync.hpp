// ext_sync.hpp - ember extension: cross-thread sync primitives (atomics,
// swap buffer, SPSC/MPSC/MPMC queues). See docs/plan_SYNC_QUEUES.md.
//
// An ember *extension* (see ember/extensions/README.md): a reusable,
// non-cheat-specific addon. Host-owned, internally-synchronized storage
// behind opaque i64 handles. Each primitive's store is a separate host
// std::vector of slots; reset() clears them all. Mirrors ext_array/
// ext_string's shape verbatim (1-based handle, slot(h) bounds check,
// register_natives/reset, public accessor for host-side reach-in) -- the
// ONE structural difference is the host store is internally synchronized
// (std::atomic / lock-free ring / host-internal std::mutex for MPMC).
//
// === SCOPE (docs/planning/plan_SYNC_QUEUES.md S0 -- read this first) =================
//
// These primitives let a script coordinate with host threads on
// HOST-OWNED SHARED STATE behind i64 handles -- a producer on host
// thread B pushes into a queue the script (on host thread A) pops from,
// an atomic the host writes the script reads, a swap buffer the script
// writes the host reads. They are INTERNALLY THREAD-SAFE AT THE HOST
// LAYER (the storage is std::atomic / a lock-free ring / a host-internal
// mutex). They DO NOT make the ember context_t safe for concurrent calls.
//
// If two threads call ember fns into the same context -- even through
// these primitives -- the context races exactly as documented in
// docs/planning/plan_CONTEXT_THREADSAFETY.md S1.2, and a trap on one thread can longjmp
// to the other thread's checkpoint (the S1.4 --tick bug, generalized).
// The context-thread-safety work is SEPARATE and ships in its own batch;
// this addon is usable the moment the host arranges that only one thread
// at a time calls ember into a given context (which is the U2 shape:
// script side stays single-threaded per context; the host threads that
// produce/consume the queues do not call ember at all, or call it on
// their OWN context_t per docs/planning/plan_CONTEXT_THREADSAFETY.md Option D).
//
// Use under the U2 contract: the script side is single-threaded per
// context; host threads touch only the queue/swap-buffer/atomic HOST
// storage via the _host accessors below, never the context. The queues
// work because the contention is on host storage, not on the context.
// =======================================================================
//
// No operator overloads (these are method-call natives, like ext_array),
// so no register_overloads(). No script-visible mutex -- deferred per the
// plan (couples to the function-ref / scoped_with_lock decision, see
// docs/planning/plan_FUNCTION_REFS.md). MPMC uses a HOST-INTERNAL std::mutex held only
// across a short ring-index critical section, never across an ember call;
// it is not exposed to the script and cannot deadlock the script or host.
//
// All queue natives are NON-BLOCKING: push returns 0 immediately if full,
// try_pop returns INT64_MIN immediately if empty. No native in this addon
// holds a lock while waiting for another thread -- a script cannot
// deadlock on any primitive here (the only "blocking" is a script-authored
// poll loop, which the instruction budget bounds, see SAFETY_AND_SANDBOX S3).
//
// REDSHELL guard #8 (backing-store isolation): all five stores are
// host-heap vectors of shared stable slots (std::atomic / std::vector<int64_t>
// / std::mutex), NEVER co-located with exec JIT memory or the dispatch
// table. Handles are 1-based indices, never pointers. No native hands out
// an address as an i64. Bounds checks on every indexed native. Mirrors
// ext_array/ext_string's posture verbatim -- see plan S7.
#pragma once
#include "sema.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ember::ext_sync {

// Register all sync primitives' natives (atomic_*, swapbuf_*, spsc_*,
// mpsc_*, mpmc_*) into m. No operator overloads, so no register_overloads().
void register_natives(std::unordered_map<std::string, NativeSig>& m);

// Clear all host stores + the free-lists. A host wanting per-run or
// per-unload isolation calls this (mirrors ext_array::reset / ext_string::reset).
void reset();

// ---- Host-side accessors (for host threads that produce/consume WITHOUT
//      calling ember -- the U2 shape). Mirrors ext_array::get_bytes /
//      ext_string::slot. Host threads use these; they never call ember.
//      The _host suffix is load-bearing: it tells a reader "host-side
//      entry, not an ember native" (the naming discipline that keeps
//      ext_array::get_bytes clearly a host-reach-in vs an ember-callable
//      array_get_*). They share the same underlying impl as the natives. ----

// Atomic: load from a host thread (acquire). Returns false on bad handle.
bool atomic_load_host(int64_t handle, int64_t* out_val);
// Atomic: store from a host thread (release). Returns false on bad handle.
bool atomic_store_host(int64_t handle, int64_t val);
// Atomic: CAS from a host thread. *out_swapped = true if exchanged.
bool atomic_cas_host(int64_t handle, int64_t expected, int64_t desired, bool* out_swapped);

// Swap buffer: get a pointer + len to the BACK (host-readable) side's i64s.
// The host reads this without calling ember. Returns false on bad handle.
// (Mirrors ext_array::get_bytes -- a C++ host-callable accessor, NOT an
//  ember native; the handle is still a bounds-checked 1-based index, so
//  a script cannot forge or supply one to this function.)
bool swapbuf_back_ptr(int64_t handle, int64_t** out_data, int64_t* out_len);
// Swap buffer: which side is currently front (the script-writable side)?
// Host uses this to know which side a host-produce should write + swap.
// Returns -1 on bad handle.
int64_t swapbuf_front_index_host(int64_t handle);
// Swap buffer: write to the FRONT side from a host thread (host-produces case).
bool swapbuf_front_write_host(int64_t handle, int64_t idx, int64_t val);
// Swap buffer: swap from a host thread (publish a host-produced frame).
bool swapbuf_swap_host(int64_t handle);

// Queue: push from a host thread (producer-on-host case). 1=pushed, 0=full.
bool spsc_push_host(int64_t handle, int64_t val, bool* out_pushed);
bool mpsc_push_host(int64_t handle, int64_t val, bool* out_pushed);
bool mpmc_push_host(int64_t handle, int64_t val, bool* out_pushed);
// Queue: try_pop from a host thread. *out_popped=true + out_val set if a
// value was popped; *out_popped=false if empty. (Host C++ callers don't
// deal with the INT64_MIN sentinel the script-facing try_pop uses.)
bool spsc_try_pop_host(int64_t handle, int64_t* out_val, bool* out_popped);
bool mpsc_try_pop_host(int64_t handle, int64_t* out_val, bool* out_popped);
bool mpmc_try_pop_host(int64_t handle, int64_t* out_val, bool* out_popped);

} // namespace ember::ext_sync
