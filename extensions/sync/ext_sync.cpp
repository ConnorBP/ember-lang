// ext_sync.cpp - ember extension: cross-thread sync primitives.
// See docs/plan_SYNC_QUEUES.md for the full design + scope-honesty statement.
//
// A Tier-0-shaped extension mirroring ext_array/ext_string (see the header
// comment of ext_sync.hpp for the S0 scope statement + the S7 REDSHELL
// guard #8 compliance notes). One TU per the extensions/README.md purity
// rule (self-contained, depends only on ember public headers + stdlib).
//
// Host store per primitive: vector<shared_ptr<Slot>>, 1-based handles. One
// store-management mutex serializes lookup/publication, alloc/free, and reset
// across contexts. A lookup returns a shared ownership lease, so vector growth,
// free, or reset cannot invalidate an acquired slot; primitive operations on
// that stable slot remain lockless (except MPMC's own queue mutex).
//
// Memory-ordering policy (per plan S2.3/S3.3/S4.3):
//   - Atomics: load=acquire, store=release, RMW=acq_rel. (seq_cst would also
//     be correct and simpler; acquire/release is the documented choice.)
//   - Swap buffer: write side cells are plain stores (single-writer per side);
//     swap is fetch_xor(acq_rel); read side load=acquire. The acq_rel/acquire
//     pair orders the publisher's writes before the reader's reads.
//   - SPSC ring: head/tail monotonic (not modulo); producer store=release,
//     consumer load=acquire on the cross-thread index; own index=relaxed.
//   - MPMC ring: host-internal std::mutex held only across the index-reserve
//     + cell read/write critical section; NEVER held across an ember call;
//     never slept on. Cannot deadlock the script (script never holds it) or
//     the host (no host thread holds it while calling ember).
//
// All queue natives are NON-BLOCKING: push returns 0 if full, try_pop returns
// INT64_MIN if empty. No native waits. The empty sentinel is INT64_MIN;
// scripts must agree never to push INT64_MIN (the host accessors return a
// bool+value pair, so host C++ consumers don't deal with the sentinel).
#include "ext_sync.hpp"
#include "ast.hpp"
#include "binding_builder.hpp"  // BindingBuilder: add() registration
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

using namespace ember;  // bind_handle, BindingBuilder, type_* singletons, make_prim

// Slots contain non-movable atomics/mutexes and therefore live behind
// shared_ptr. Besides keeping stable addresses across vector growth, shared
// ownership closes the lookup-vs-free/reset lifetime race.

namespace ember::ext_sync {

// Sentinel returned by *_try_pop when empty. Plan S4.1 recommendation (b):
// single-native, no empty/try_pop race for multi-consumer queues. Scripts
// must agree never to push INT64_MIN. Host accessors return bool+value so
// host C++ consumers never see this.
static constexpr int64_t EMPTY_SENTINEL = INT64_MIN;
// Hard allocation ceiling for each sync container, including both swap-buffer
// sides and all per-producer rings in an MPSC container.
static constexpr uint64_t MAX_CONTAINER_BYTES = uint64_t(1) << 30;
static constexpr int64_t MAX_I64_SLOTS = int64_t(MAX_CONTAINER_BYTES / sizeof(int64_t));
static constexpr size_t MAX_STORE_SLOTS = size_t(1) << 20;
static std::recursive_mutex g_store_mutex;

// ============================================================================
// 1. ATOMICS (int-only; aint8/16/32/64). Plan S2.
//    Lock-free by construction: std::atomic ops are non-blocking for the
//    integral types on x86-64. No lock held across an ember native boundary;
//    each native does its op and returns. A script cannot deadlock on an
//    atomic (nothing to wait on); livelock from a script-authored CAS retry
//    loop is bounded by the instruction budget (SAFETY S3).
// ============================================================================
struct AtomicSlot {
    std::atomic<int64_t> v{0};
    int width = 64;            // 8/16/32/64 -- store/fetch_add mask to this
};
static std::vector<std::shared_ptr<AtomicSlot>> g_atomics;
static std::vector<int64_t>                   g_atomics_free;

static std::shared_ptr<AtomicSlot> atom_slot(int64_t h) {
    std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
    if (h < 1 || h > int64_t(g_atomics.size())) return {};
    return g_atomics[size_t(h - 1)];
}

// Mask a value to the atomic's width so an aint8 genuinely holds 8 bits
// (store(h, 0x1FF) on an aint8 stores 0xFF, per plan S2.3 + S6 test 2).
// UNSIGNED bit-pattern masking: keep the low `width` bits as a non-negative
// i64 (0xFF stays 255, not -1). The plan's test matrix expects 0xFF / 0xFFFF /
// 0xFFFFFFFF, so we mask to the unsigned range, not the signed two's-complement
// range. (Signed atomics would be a different, narrower contract; this matches
// the plan + the test expectations.)
static int64_t atom_mask(int64_t v, int width) {
    uint64_t u = uint64_t(v);
    switch (width) {
        case 8:  return int64_t(u & 0xFFull);
        case 16: return int64_t(u & 0xFFFFull);
        case 32: return int64_t(u & 0xFFFFFFFFull);
        default: return v;          // 64: no masking
    }
}

static int64_t atom_alloc(int width, int64_t init) {
    std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
    if (width != 8 && width != 16 && width != 32 && width != 64) width = 64;
    init = atom_mask(init, width);
    if (!g_atomics_free.empty()) {
        int64_t idx = g_atomics_free.back();
        g_atomics_free.pop_back();
        auto& slot = g_atomics[size_t(idx - 1)];
        slot = std::make_shared<AtomicSlot>();
        slot->width = width;
        slot->v.store(init, std::memory_order_relaxed);  // slot is exclusively ours
        return idx;
    }
    if (g_atomics.size() >= MAX_STORE_SLOTS) return 0;
    g_atomics.push_back(std::make_shared<AtomicSlot>());
    auto& s = g_atomics.back();
    s->width = width;
    s->v.store(init, std::memory_order_relaxed);
    return int64_t(g_atomics.size());
}

extern "C" {
    static int64_t n_atomic_new(int64_t width, int64_t init) {
        try { return atom_alloc(int(width), init); }
        catch (const std::bad_alloc&) { return 0; }
        catch (const std::length_error&) { return 0; }
    }
    static int64_t n_atomic_load(int64_t h) {
        auto s = atom_slot(h);
        return s ? s->v.load(std::memory_order_acquire) : 0;
    }
    static void n_atomic_store(int64_t h, int64_t v) {
        auto s = atom_slot(h);
        if (s) s->v.store(atom_mask(v, s->width), std::memory_order_release);
    }
    // fetch_add: for width<64 wrap modulo 2^width via a CAS loop (the
    // standard fetch_add would overflow the int64_t, not the width). For
    // width==64 a plain fetch_add is fine. Returns the OLD value. Plan S2.3.
    static int64_t n_atomic_fetch_add(int64_t h, int64_t delta) {
        auto s = atom_slot(h); if (!s) return 0;
        if (s->width == 64) {
            return s->v.fetch_add(delta, std::memory_order_acq_rel);
        }
        int64_t old = s->v.load(std::memory_order_acquire);
        for (;;) {
            int64_t nev = atom_mask(old + delta, s->width);
            if (s->v.compare_exchange_weak(old, nev,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return old;   // old is the pre-swap value
            }
            // compare_exchange_weak updated `old` to the current value on
            // failure; retry. (Host-local var, invisible to the script.)
        }
    }
    static int64_t n_atomic_fetch_sub(int64_t h, int64_t delta) {
        auto s = atom_slot(h); if (!s) return 0;
        if (s->width == 64) {
            return s->v.fetch_sub(delta, std::memory_order_acq_rel);
        }
        int64_t old = s->v.load(std::memory_order_acquire);
        for (;;) {
            int64_t nev = atom_mask(old - delta, s->width);
            if (s->v.compare_exchange_weak(old, nev,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return old;
            }
        }
    }
    // cas returns i64 1/0 (not bool) so it round-trips through ember's i64
    // ABI without a bool-marshalling special case (mirrors array_get_u8
    // returning i64 even for a u8). Plan S2.2.
    static int64_t n_atomic_cas(int64_t h, int64_t expected, int64_t desired) {
        auto s = atom_slot(h); if (!s) return 0;
        desired = atom_mask(desired, s->width);
        // compare_exchange_strong modifies `expected` on failure to the
        // current value -- that's a host-local var, not a script-visible
        // out-param, so it's invisible to the script (the script gets only
        // the bool; it re-reads via load if it wants the current value).
        bool ok = s->v.compare_exchange_strong(expected, desired,
                    std::memory_order_acq_rel, std::memory_order_acquire);
        return ok ? 1 : 0;
    }
    static int64_t n_atomic_swap(int64_t h, int64_t v) {
        auto s = atom_slot(h); if (!s) return 0;
        return s->v.exchange(atom_mask(v, s->width), std::memory_order_acq_rel);
    }
    static void n_atomic_free(int64_t h) {
        try {
            std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
            if (atom_slot(h)) {
                g_atomics_free.reserve(g_atomics_free.size() + 1);
                g_atomics[size_t(h - 1)].reset();
                g_atomics_free.push_back(h);
            }
        } catch (const std::exception&) { /* leave the live slot unchanged */ }
    }
}

bool atomic_load_host(int64_t handle, int64_t* out_val) {
    auto s = atom_slot(handle); if (!s) return false;
    *out_val = s->v.load(std::memory_order_acquire);
    return true;
}
bool atomic_store_host(int64_t handle, int64_t val) {
    auto s = atom_slot(handle); if (!s) return false;
    s->v.store(atom_mask(val, s->width), std::memory_order_release);
    return true;
}
bool atomic_cas_host(int64_t handle, int64_t expected, int64_t desired, bool* out_swapped) {
    auto s = atom_slot(handle); if (!s) return false;
    desired = atom_mask(desired, s->width);
    *out_swapped = s->v.compare_exchange_strong(expected, desired,
                    std::memory_order_acq_rel, std::memory_order_acquire);
    return true;
}

// ============================================================================
// 2. SWAP BUFFER / SWAPCHAIN -- double-buffer with atomic pointer flip. Plan S3.
//    The script writes the FRONT side (exclusively script-owned); the host
//    reads the BACK side (exclusively host-owned); swap() is ONE fetch_xor(1)
//    RMW that flips front<->back. Per-cell writes are plain stores (safe
//    because each side has a single owner); only the front index is atomic.
//    Deadlock-free by construction: one atomic RMW per swap, none per cell,
//    no waiting. A torn frame is a PROTOCOL question (host reads between two
//    script swaps), not a deadlock -- the render-overlay use case is "host
//    reads the most recent fully-published frame each tick."
// ============================================================================
struct SwapBufSlot {
    std::vector<int64_t> side[2];   // two equal-size buffers (host heap)
    std::atomic<int>     front{0};   // 0 or 1 -- which side the script writes
    int64_t              cap = 0;
};
static std::vector<std::shared_ptr<SwapBufSlot>> g_swapbufs;
static std::vector<int64_t>                    g_swapbufs_free;

static std::shared_ptr<SwapBufSlot> swapbuf_slot(int64_t h) {
    std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
    if (h < 1 || h > int64_t(g_swapbufs.size())) return {};
    return g_swapbufs[size_t(h - 1)];
}

static int64_t swapbuf_alloc(int64_t cap) {
    std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
    if (cap < 0 || uint64_t(cap) > MAX_CONTAINER_BYTES / (2 * sizeof(int64_t))) return 0;
    auto fresh = std::make_shared<SwapBufSlot>();
    fresh->cap = cap;
    fresh->side[0].assign(size_t(cap), 0);
    fresh->side[1].assign(size_t(cap), 0);
    fresh->front.store(0, std::memory_order_relaxed);
    if (!g_swapbufs_free.empty()) {
        int64_t idx = g_swapbufs_free.back();
        g_swapbufs_free.pop_back();
        g_swapbufs[size_t(idx - 1)] = std::move(fresh);
        return idx;
    }
    if (g_swapbufs.size() >= MAX_STORE_SLOTS) return 0;
    g_swapbufs.push_back(std::move(fresh));
    return int64_t(g_swapbufs.size());
}

extern "C" {
    static int64_t n_swapbuf_new(int64_t cap) {
        try { return swapbuf_alloc(cap); }
        catch (const std::bad_alloc&) { return 0; }
        catch (const std::length_error&) { return 0; }
    }
    static int64_t n_swapbuf_capacity(int64_t h) {
        auto s = swapbuf_slot(h); return s ? s->cap : 0;
    }
    // Write to the FRONT (script-owned) side. Plain store -- safe because only
    // the script thread touches the front side; only the front INDEX is atomic.
    static void n_swapbuf_write(int64_t h, int64_t idx, int64_t val) {
        auto s = swapbuf_slot(h); if (!s) return;
        if (idx < 0 || idx >= s->cap) return;   // bounds: out-of-range is a no-op (mirrors ext_array)
        int f = s->front.load(std::memory_order_acquire);
        s->side[f][size_t(idx)] = val;
    }
    // Read the BACK (host-owned) side. Symmetric accessor for the
    // script-consumes case; the host typically reaches in via swapbuf_back_ptr.
    static int64_t n_swapbuf_read(int64_t h, int64_t idx) {
        auto s = swapbuf_slot(h); if (!s) return 0;
        if (idx < 0 || idx >= s->cap) return 0;  // bounds: out-of-range -> 0 (mirrors ext_array)
        int b = 1 - s->front.load(std::memory_order_acquire);
        return s->side[b][size_t(idx)];
    }
    // The one atomic op: flip front<->back. fetch_xor(1, acq_rel): release on
    // the publisher's cell writes, acquire on the reader's cell reads.
    static void n_swapbuf_swap(int64_t h) {
        auto s = swapbuf_slot(h); if (!s) return;
        s->front.fetch_xor(1, std::memory_order_acq_rel);
    }
    static void n_swapbuf_free(int64_t h) {
        try {
            std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
            if (swapbuf_slot(h)) {
                g_swapbufs_free.reserve(g_swapbufs_free.size() + 1);
                g_swapbufs[size_t(h - 1)].reset(); g_swapbufs_free.push_back(h);
            }
        } catch (const std::exception&) { /* leave the live slot unchanged */ }
    }
}

bool swapbuf_back_ptr(int64_t handle, int64_t** out_data, int64_t* out_len) {
    auto s = swapbuf_slot(handle); if (!s) return false;
    int b = 1 - s->front.load(std::memory_order_acquire);
    *out_data = s->side[b].data();
    *out_len  = s->cap;
    return true;
}
int64_t swapbuf_front_index_host(int64_t handle) {
    auto s = swapbuf_slot(handle); if (!s) return -1;
    return int64_t(s->front.load(std::memory_order_acquire));
}
bool swapbuf_front_write_host(int64_t handle, int64_t idx, int64_t val) {
    auto s = swapbuf_slot(handle); if (!s) return false;
    if (idx < 0 || idx >= s->cap) return false;
    int f = s->front.load(std::memory_order_acquire);
    s->side[f][size_t(idx)] = val;
    return true;
}
bool swapbuf_swap_host(int64_t handle) {
    auto s = swapbuf_slot(handle); if (!s) return false;
    s->front.fetch_xor(1, std::memory_order_acq_rel);
    return true;
}

// ============================================================================
// 3. SPSC queue -- single-producer single-consumer lock-free ring. Plan S4.3.
//    Textbook bounded SPSC: separate head (producer) / tail (consumer) atomic
//    indices, MONOTONIC (not modulo) so the full/empty check is unambiguous;
//    %cap indexes the ring. Capacity rounded up to a power of two (plan S4.3
//    rec: slight perf, the modulo is cheap; documented choice). acquire/
//    release pair orders the producer's write before the consumer's read.
//    Contract: exactly one producer + one consumer. A second producer calling
//    push concurrently is a contract violation (not a deadlock -- undefined).
//    Deadlock-free: no CAS, no waiting.
// ============================================================================
struct SpscSlot {
    std::vector<int64_t> buf;       // capacity slots (host heap)
    std::atomic<int64_t> head{0};    // producer writes here (only producer touches)
    std::atomic<int64_t> tail{0};    // consumer reads here (only consumer touches)
    int64_t cap = 0;                 // power of two
};
static std::vector<std::shared_ptr<SpscSlot>> g_spsc;
static std::vector<int64_t>                  g_spsc_free;

static std::shared_ptr<SpscSlot> spsc_slot(int64_t h) {
    std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
    if (h < 1 || h > int64_t(g_spsc.size())) return {};
    return g_spsc[size_t(h - 1)];
}

// Round capacity up to a power of two without signed overflow.  Zero retains
// the historical minimum-capacity behavior; negative and >1 GiB requests fail.
static bool round_up_pow2(int64_t requested, int64_t* out) {
    if (requested < 0) return false;
    uint64_t v = requested == 0 ? 1 : uint64_t(requested);
    if (v > uint64_t(MAX_I64_SLOTS)) return false;
    --v;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16; v |= v >> 32;
    ++v;
    if (v > uint64_t(MAX_I64_SLOTS)) return false;
    *out = int64_t(v);
    return true;
}

static int64_t spsc_alloc(int64_t requested_cap) {
    std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
    int64_t cap = 0;
    if (!round_up_pow2(requested_cap, &cap)) return 0;
    auto fresh = std::make_shared<SpscSlot>();
    fresh->cap = cap;
    fresh->buf.assign(size_t(cap), 0);
    fresh->head.store(0, std::memory_order_relaxed);
    fresh->tail.store(0, std::memory_order_relaxed);
    if (!g_spsc_free.empty()) {
        int64_t idx = g_spsc_free.back(); g_spsc_free.pop_back();
        g_spsc[size_t(idx - 1)] = std::move(fresh);
        return idx;
    }
    if (g_spsc.size() >= MAX_STORE_SLOTS) return 0;
    g_spsc.push_back(std::move(fresh));
    return int64_t(g_spsc.size());
}

// Shared SPSC ring impl (used by both the native and the _host accessor;
// they're the same operation, just different entry points).
static bool spsc_push_impl(SpscSlot* s, int64_t v) {
    if (!s) return false;
    int64_t h = s->head.load(std::memory_order_relaxed);
    int64_t t = s->tail.load(std::memory_order_acquire);
    if (h - t >= s->cap) return false;          // full -> 0 (non-blocking)
    s->buf[size_t(h & (s->cap - 1))] = v;
    s->head.store(h + 1, std::memory_order_release);
    return true;
}
static bool spsc_try_pop_impl(SpscSlot* s, int64_t* out) {
    if (!s) return false;
    int64_t t = s->tail.load(std::memory_order_relaxed);
    int64_t h = s->head.load(std::memory_order_acquire);
    if (t == h) return false;                   // empty
    *out = s->buf[size_t(t & (s->cap - 1))];
    s->tail.store(t + 1, std::memory_order_release);
    return true;
}

extern "C" {
    static int64_t n_spsc_new(int64_t cap) {
        try { return spsc_alloc(cap); }
        catch (const std::bad_alloc&) { return 0; }
        catch (const std::length_error&) { return 0; }
    }
    static int64_t n_spsc_push(int64_t h, int64_t v) {
        return spsc_push_impl(spsc_slot(h).get(), v) ? 1 : 0;
    }
    static int64_t n_spsc_try_pop(int64_t h) {
        int64_t v = 0;
        return spsc_try_pop_impl(spsc_slot(h).get(), &v) ? v : EMPTY_SENTINEL;
    }
    static int64_t n_spsc_size(int64_t h) {
        auto s = spsc_slot(h); if (!s) return 0;
        int64_t hd = s->head.load(std::memory_order_acquire);
        int64_t tl = s->tail.load(std::memory_order_acquire);
        return hd - tl;                          // approximate (cross-thread)
    }
    static void n_spsc_free(int64_t h) {
        try {
            std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
            if (spsc_slot(h)) {
                g_spsc_free.reserve(g_spsc_free.size() + 1);
                g_spsc[size_t(h - 1)].reset(); g_spsc_free.push_back(h);
            }
        } catch (const std::exception&) { /* leave the live slot unchanged */ }
    }
}
bool spsc_push_host(int64_t handle, int64_t val, bool* out_pushed) {
    auto s = spsc_slot(handle); if (!s) return false;
    *out_pushed = spsc_push_impl(s.get(), val);
    return true;
}
bool spsc_try_pop_host(int64_t handle, int64_t* out_val, bool* out_popped) {
    auto s = spsc_slot(handle); if (!s) return false;
    *out_popped = spsc_try_pop_impl(s.get(), out_val);
    return true;
}

// ============================================================================
// 4. MPSC queue -- multi-producer single-consumer. Plan S4.4.
//    Design (a): per-producer SPSC rings + a shared consumer side that
//    round-robins them. Producers never contend (each owns its ring); the
//    single consumer does N try_pops per drain. Fully lock-free (it's N
//    SPSC rings), no CAS, no waiting. Chosen over design (b) CAS-single-ring
//    for v1 simplicity (plan rec). If a real script needs dynamic producer
//    registration or single-ring MPSC, ship (b) as a v2.
//
//    API: mpsc_new(cap, producer_count) -> mpsc container handle;
//    mpsc_producer_handle(h, idx) -> i64 sub-handle (a SPSC handle baked into
//    the container); mpsc_push(producer_h, v); mpsc_try_pop(h) round-robins.
//    Two-handle shape is a slight deviation from "one i64 per primitive" but
//    avoids the CAS-MPSC subtlety (the documented tradeoff).
// ============================================================================
// MPSC reuses the SPSC ring as its per-producer store. The mpsc "container"
// holds a list of SPSC handles (its producers) + a round-robin cursor.
struct MpscSlot {
    std::vector<int64_t> producers;  // SPSC handles, one per producer slot
    std::atomic<int64_t> cursor{0};   // round-robin index (single consumer)
};
static std::vector<std::shared_ptr<MpscSlot>> g_mpsc;
static std::vector<int64_t>                  g_mpsc_free;

static std::shared_ptr<MpscSlot> mpsc_slot(int64_t h) {
    std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
    if (h < 1 || h > int64_t(g_mpsc.size())) return {};
    return g_mpsc[size_t(h - 1)];
}

static int64_t mpsc_alloc(int64_t requested_cap, int64_t producer_count) {
    std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
    if (producer_count < 0) return 0;
    if (producer_count == 0) producer_count = 1;
    int64_t cap = 0;
    if (!round_up_pow2(requested_cap, &cap)) return 0;
    const uint64_t per_producer = uint64_t(cap) * sizeof(int64_t) + sizeof(int64_t);
    if (uint64_t(producer_count) > MAX_CONTAINER_BYTES / per_producer) return 0;

    auto fresh = std::make_shared<MpscSlot>();
    fresh->producers.reserve(size_t(producer_count));
    // Reserve rollback bookkeeping before creating any child rings.  Once
    // this succeeds, the catch path below cannot allocate while restoring
    // every child handle to the free list.
    g_spsc_free.reserve(g_spsc_free.size() + size_t(producer_count));
    try {
        for (int64_t i = 0; i < producer_count; ++i) {
            int64_t ph = spsc_alloc(cap);
            if (ph == 0) throw std::bad_alloc();
            fresh->producers.push_back(ph);
        }
        fresh->cursor.store(0, std::memory_order_relaxed);
        if (!g_mpsc_free.empty()) {
            int64_t idx = g_mpsc_free.back(); g_mpsc_free.pop_back();
            g_mpsc[size_t(idx - 1)] = std::move(fresh);
            return idx;
        }
        if (g_mpsc.size() >= MAX_STORE_SLOTS) throw std::length_error("sync store capacity");
        g_mpsc.push_back(std::move(fresh));
        return int64_t(g_mpsc.size());
    } catch (...) {
        for (auto ph : fresh->producers) {
            if (spsc_slot(ph)) {
                g_spsc[size_t(ph - 1)].reset();
                g_spsc_free.push_back(ph);
            }
        }
        throw;
    }
}

extern "C" {
    static int64_t n_mpsc_new(int64_t cap, int64_t producer_count) {
        try { return mpsc_alloc(cap, producer_count); }
        catch (const std::bad_alloc&) { return 0; }
        catch (const std::length_error&) { return 0; }
    }
    // Returns the i64 producer sub-handle (a SPSC handle). The script passes
    // this to mpsc_push. Mirrors how a host would hand out per-producer rings.
    static int64_t n_mpsc_producer_handle(int64_t h, int64_t idx) {
        auto s = mpsc_slot(h); if (!s) return 0;
        if (idx < 0 || idx >= int64_t(s->producers.size())) return 0;
        return s->producers[size_t(idx)];
    }
    // Push to a producer's SPSC ring. The first arg is the PRODUCER sub-handle
    // (from mpsc_producer_handle), NOT the mpsc container handle -- a script
    // that wants to push must first get its producer handle. (This is the
    // two-handle shape; document it.)
    static int64_t n_mpsc_push(int64_t producer_h, int64_t v) {
        return spsc_push_impl(spsc_slot(producer_h).get(), v) ? 1 : 0;
    }
    // Round-robin all producer rings, pop the first non-empty. Single consumer
    // -- only the consumer touches `cursor`, so it's relaxed on the consumer's
    // own load; the per-ring acquire/release does the cross-thread ordering.
    static int64_t n_mpsc_try_pop(int64_t h) {
        auto s = mpsc_slot(h); if (!s) return EMPTY_SENTINEL;
        int64_t n = int64_t(s->producers.size());
        int64_t start = s->cursor.load(std::memory_order_relaxed);
        for (int64_t i = 0; i < n; ++i) {
            int64_t idx = (start + i) % n;
            auto ring = spsc_slot(s->producers[size_t(idx)]);
            int64_t v = 0;
            if (spsc_try_pop_impl(ring.get(), &v)) {
                s->cursor.store((idx + 1) % n, std::memory_order_relaxed);
                return v;
            }
        }
        return EMPTY_SENTINEL;
    }
    static int64_t n_mpsc_size(int64_t h) {
        auto s = mpsc_slot(h); if (!s) return 0;
        int64_t total = 0;
        for (auto ph : s->producers) {
            auto ring = spsc_slot(ph);
            if (ring) {
                int64_t hd = ring->head.load(std::memory_order_acquire);
                int64_t tl = ring->tail.load(std::memory_order_acquire);
                total += hd - tl;
            }
        }
        return total;
    }
    static void n_mpsc_free(int64_t h) {
        try {
            std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
            auto s = mpsc_slot(h); if (!s) return;
            g_spsc_free.reserve(g_spsc_free.size() + s->producers.size());
            g_mpsc_free.reserve(g_mpsc_free.size() + 1);
            for (auto ph : s->producers) {
                if (spsc_slot(ph)) { g_spsc[size_t(ph - 1)].reset(); g_spsc_free.push_back(ph); }
            }
            g_mpsc[size_t(h - 1)].reset();
            g_mpsc_free.push_back(h);
        } catch (const std::exception&) { /* leave all live slots unchanged */ }
    }
}
bool mpsc_push_host(int64_t handle, int64_t val, bool* out_pushed) {
    // The host accessor takes the PRODUCER sub-handle (same as the native).
    auto s = spsc_slot(handle); if (!s) return false;
    *out_pushed = spsc_push_impl(s.get(), val);
    return true;
}
bool mpsc_try_pop_host(int64_t handle, int64_t* out_val, bool* out_popped) {
    auto s = mpsc_slot(handle); if (!s) return false;
    int64_t n = int64_t(s->producers.size());
    int64_t start = s->cursor.load(std::memory_order_relaxed);
    for (int64_t i = 0; i < n; ++i) {
        int64_t idx = (start + i) % n;
        auto ring = spsc_slot(s->producers[size_t(idx)]);
        if (spsc_try_pop_impl(ring.get(), out_val)) {
            s->cursor.store((idx + 1) % n, std::memory_order_relaxed);
            *out_popped = true;
            return true;
        }
    }
    *out_popped = false;
    return true;
}

// ============================================================================
// 5. MPMC queue -- multi-producer multi-consumer. Plan S4.5 design (alpha).
//    Bounded ring + HOST-INTERNAL std::mutex held only across the index-
//    reserve + cell read/write critical section. The lock is NEVER held across
//    an ember call boundary and is NEVER slept on -> cannot deadlock the
//    script (script never holds it) or the host (no host thread holds it while
//    calling ember). Vyukov lock-free MPMC (design beta) is a documented FUTURE
//    drop-in optimization if a profile shows the internal mutex as a real
//    contention point -- premature lock-free MPMC is exactly the subtlety the
//    "hard-to-accidentally-deadlock" framing wants to avoid in v1.
// ============================================================================
struct MpmcSlot {
    std::vector<int64_t> buf;       // capacity slots (host heap)
    int64_t              head = 0;  // next write index (under the lock)
    int64_t              tail = 0;  // next read index  (under the lock)
    int64_t              cap = 0;
    std::mutex           lock;       // host-internal; NEVER held across an ember call
};
static std::vector<std::shared_ptr<MpmcSlot>> g_mpmc;
static std::vector<int64_t>                  g_mpmc_free;

static std::shared_ptr<MpmcSlot> mpmc_slot(int64_t h) {
    std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
    if (h < 1 || h > int64_t(g_mpmc.size())) return {};
    return g_mpmc[size_t(h - 1)];
}

static int64_t mpmc_alloc(int64_t requested_cap) {
    std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
    if (requested_cap < 0 || requested_cap > MAX_I64_SLOTS) return 0;
    int64_t cap = requested_cap == 0 ? 1 : requested_cap;
    auto fresh = std::make_shared<MpmcSlot>();
    fresh->cap = cap;
    fresh->buf.assign(size_t(cap), 0);
    fresh->head = 0; fresh->tail = 0;
    if (!g_mpmc_free.empty()) {
        int64_t idx = g_mpmc_free.back(); g_mpmc_free.pop_back();
        g_mpmc[size_t(idx - 1)] = std::move(fresh);
        return idx;
    }
    if (g_mpmc.size() >= MAX_STORE_SLOTS) return 0;
    g_mpmc.push_back(std::move(fresh));
    return int64_t(g_mpmc.size());
}

static bool mpmc_push_impl(MpmcSlot* s, int64_t v) {
    if (!s) return false;
    std::lock_guard<std::mutex> g(s->lock);    // short critical section, no ember call inside
    if (s->head - s->tail >= s->cap) return false;   // full -> 0
    s->buf[size_t(s->head % s->cap)] = v;
    ++s->head;
    return true;
}
static bool mpmc_try_pop_impl(MpmcSlot* s, int64_t* out) {
    if (!s) return false;
    std::lock_guard<std::mutex> g(s->lock);    // short critical section, no ember call inside
    if (s->tail == s->head) return false;     // empty
    *out = s->buf[size_t(s->tail % s->cap)];
    ++s->tail;
    return true;
}

extern "C" {
    static int64_t n_mpmc_new(int64_t cap) {
        try { return mpmc_alloc(cap); }
        catch (const std::bad_alloc&) { return 0; }
        catch (const std::length_error&) { return 0; }
    }
    static int64_t n_mpmc_push(int64_t h, int64_t v) {
        return mpmc_push_impl(mpmc_slot(h).get(), v) ? 1 : 0;
    }
    static int64_t n_mpmc_try_pop(int64_t h) {
        int64_t v = 0;
        return mpmc_try_pop_impl(mpmc_slot(h).get(), &v) ? v : EMPTY_SENTINEL;
    }
    static int64_t n_mpmc_size(int64_t h) {
        auto s = mpmc_slot(h); if (!s) return 0;
        std::lock_guard<std::mutex> g(s->lock);    // exact under-lock
        return s->head - s->tail;
    }
    static void n_mpmc_free(int64_t h) {
        try {
            std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
            if (mpmc_slot(h)) {
                g_mpmc_free.reserve(g_mpmc_free.size() + 1);
                g_mpmc[size_t(h - 1)].reset(); g_mpmc_free.push_back(h);
            }
        } catch (const std::exception&) { /* leave the live slot unchanged */ }
    }
}
bool mpmc_push_host(int64_t handle, int64_t val, bool* out_pushed) {
    auto s = mpmc_slot(handle); if (!s) return false;
    *out_pushed = mpmc_push_impl(s.get(), val);
    return true;
}
bool mpmc_try_pop_host(int64_t handle, int64_t* out_val, bool* out_popped) {
    auto s = mpmc_slot(handle); if (!s) return false;
    *out_popped = mpmc_try_pop_impl(s.get(), out_val);
    return true;
}

// ============================================================================
// register_natives -- one BindingBuilder with all natives. Mirrors ext_array's
// registration shape verbatim (add -> build -> loop-insert into m).
//
// Type choices (per plan): every handle param/return is bind_handle(<tag>) so
// sema tags them distinctly (atomic vs swapbuf vs spsc vs mpsc vs mpmc). All
// value/flag returns are type_i64() (atomic_cas returns i64 1/0 not bool, so
// it round-trips through ember's i64 ABI without bool marshalling; push
// returns i64 1/0; try_pop returns the value-or-INT64_MIN i64). Widths and
// indices are i64. No operator overloads (these are method-call natives).
// ============================================================================
void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    Type A = bind_handle("atomic");
    Type S = bind_handle("swapbuf");
    Type P = bind_handle("spsc");
    Type M = bind_handle("mpsc");
    Type C = bind_handle("mpmc");

    // --- atomics (int-only; aint8/16/32/64) ---
    b.add("atomic_new",       A, {type_i64(), type_i64()},                 (void*)&n_atomic_new);
    b.add("atomic_load",      type_i64(), {A},                             (void*)&n_atomic_load);
    b.add("atomic_store",     type_void(), {A, type_i64()},                (void*)&n_atomic_store);
    b.add("atomic_fetch_add", type_i64(), {A, type_i64()},                 (void*)&n_atomic_fetch_add);
    b.add("atomic_fetch_sub", type_i64(), {A, type_i64()},                 (void*)&n_atomic_fetch_sub);
    b.add("atomic_cas",       type_i64(), {A, type_i64(), type_i64()},     (void*)&n_atomic_cas);
    b.add("atomic_swap",      type_i64(), {A, type_i64()},                 (void*)&n_atomic_swap);
    b.add("atomic_free",      type_void(), {A},                            (void*)&n_atomic_free);

    // --- swap buffer ---
    b.add("swapbuf_new",       S, {type_i64()},                            (void*)&n_swapbuf_new);
    b.add("swapbuf_capacity",  type_i64(), {S},                            (void*)&n_swapbuf_capacity);
    b.add("swapbuf_write",     type_void(), {S, type_i64(), type_i64()},   (void*)&n_swapbuf_write);
    b.add("swapbuf_read",      type_i64(), {S, type_i64()},                (void*)&n_swapbuf_read);
    b.add("swapbuf_swap",      type_void(), {S},                           (void*)&n_swapbuf_swap);
    b.add("swapbuf_free",      type_void(), {S},                           (void*)&n_swapbuf_free);

    // --- SPSC queue ---
    b.add("spsc_new",      P, {type_i64()},                                (void*)&n_spsc_new);
    b.add("spsc_push",     type_i64(), {P, type_i64()},                    (void*)&n_spsc_push);
    b.add("spsc_try_pop", type_i64(), {P},                                 (void*)&n_spsc_try_pop);
    b.add("spsc_size",    type_i64(), {P},                                  (void*)&n_spsc_size);
    b.add("spsc_free",    type_void(), {P},                                 (void*)&n_spsc_free);

    // --- MPSC queue (design (a): per-producer SPSC rings + container handle) ---
    b.add("mpsc_new",             M, {type_i64(), type_i64()},              (void*)&n_mpsc_new);
    b.add("mpsc_producer_handle", P, {M, type_i64()},                      (void*)&n_mpsc_producer_handle);
    b.add("mpsc_push",            type_i64(), {P, type_i64()},             (void*)&n_mpsc_push);
    b.add("mpsc_try_pop",         type_i64(), {M},                         (void*)&n_mpsc_try_pop);
    b.add("mpsc_size",            type_i64(), {M},                          (void*)&n_mpsc_size);
    b.add("mpsc_free",            type_void(), {M},                         (void*)&n_mpsc_free);

    // --- MPMC queue (design (alpha): bounded ring + host-internal std::mutex) ---
    b.add("mpmc_new",      C, {type_i64()},                                 (void*)&n_mpmc_new);
    b.add("mpmc_push",     type_i64(), {C, type_i64()},                     (void*)&n_mpmc_push);
    b.add("mpmc_try_pop", type_i64(), {C},                                  (void*)&n_mpmc_try_pop);
    b.add("mpmc_size",    type_i64(), {C},                                  (void*)&n_mpmc_size);
    b.add("mpmc_free",    type_void(), {C},                                 (void*)&n_mpmc_free);

    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

// Clear all five stores + all five free-lists. Mirrors ext_array::reset /
// ext_string::reset; a host wanting per-run or per-unload isolation calls this.
void reset() {
    std::lock_guard<std::recursive_mutex> guard(g_store_mutex);
    g_atomics.clear();      g_atomics_free.clear();
    g_swapbufs.clear();     g_swapbufs_free.clear();
    g_spsc.clear();         g_spsc_free.clear();
    g_mpsc.clear();         g_mpsc_free.clear();
    g_mpmc.clear();         g_mpmc_free.clear();
}

} // namespace ember::ext_sync
