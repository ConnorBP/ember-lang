// ext_array.cpp - ember extension: array<T> host-store type.
// Relocated from prism/src/prism/prism_script_host.cpp (array block) during
// the restructure Section 6 audit. The host keeps a thin prism::GetArrayBytes
// wrapper (in prism_script_host.cpp) that forwards to get_bytes(), so
// shader_api.cpp's custom draw calls and the host's read_bulk native are
// unchanged. The store, handle ABI, bounds-check convention, and get/set
// natives are unchanged.
#include "ext_array.hpp"
#include "ast.hpp"
#include "binding_builder.hpp"  // BindingBuilder: deduped I/H/add registration
#include "../gc/ext_gc.hpp"     // c1 GC trace-callback + write-barrier facade
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

using namespace ember;  // bind_handle, BindingBuilder, type_* singletons

namespace ember::ext_array {

// --- array<T> host store (opaque i64 handle; host owns the byte vector + elem_size) ---
struct ArraySlot { std::vector<uint8_t> bytes; int64_t elem_size = 1; };
static std::vector<ArraySlot> g_arrays;
// Serializes all g_arrays store operations (push_back in arr_new/alloc_bytes,
// arr_slot lookups, per-slot byte mutations) so concurrent context_t's calling
// array natives cannot race on vector reallocation (Sec-5). Mirrors the
// g_store_mutex pattern in ext_sync.cpp and g_mutex in ext_lifecycle.cpp.
static std::mutex g_store_mutex;
// A single extension container is limited to 1 GiB.  This is checked before
// every allocation/growth so script sizes cannot wrap size_t or exhaust the
// host through one array.
static constexpr size_t MAX_CONTAINER_BYTES = size_t(1) << 30;
static bool checked_bytes(int64_t elem_size, int64_t count, size_t* out) {
    if (elem_size < 1 || count < 0) return false;
    const uint64_t es = uint64_t(elem_size), n = uint64_t(count);
    if (n != 0 && es > uint64_t(MAX_CONTAINER_BYTES) / n) return false;
    *out = size_t(es * n);
    return *out <= MAX_CONTAINER_BYTES;
}

// ===========================================================================
// c1 GC trace-callback integration.
//
// An array<i64> (elem_size == 8) can hold a GC object pointer in any of its
// aligned 8-byte elements. For an UNPINNED GC object stored there to survive
// gc_collect, the array store must report those element values as root
// candidates to the collector. We register ONE idempotent trace callback per
// thread (against that thread's thread-local GC runtime via the ext_gc facade)
// that walks g_arrays under g_store_mutex and, for every slot whose
// elem_size == 8, reports each aligned 8-byte element via visitor.report().
// The heap visitor validates each candidate (live?) and rejects ordinary
// integers, null, and stale / non-live addresses -- so a u8 / f32 array
// (elem_size != 8) and a plain-integer i64 slot NEVER create a false root.
//
// The token is THREAD-LOCAL because the GC runtime is thread-local: each OS
// thread registers its own callback on its own heap, and the callback walks the
// (process-wide, mutex-protected) store. A pointer from thread B's heap stored
// into an array is reported during thread A's collect, but the visitor rejects
// it (not live on A's heap), so cross-thread pointers do not false-root.
//
// Lifecycle: the callback is registered lazily on the first GC-pointer-capable
// mutation (set_i64 / push_i64) and only when the GC runtime is already
// initialized on this thread (gc_runtime_initialized), so a thread that never
// uses the GC stays in pure non-GC mode (no inert heap is materialized).
// gc_reset() (ext_gc) clears the heap's registrations, invalidating outstanding
// tokens; the next mutation detects this (the stale unregister is a no-op) and
// re-registers fresh -- so the callback cannot outlive reset / runtime teardown
// and is always reattached when needed. reset() (this extension) unregisters
// the current thread's token + clears the store, so the callback does not
// outlive the store either.
//
// Synchronization / deadlock: the trace callback acquires g_store_mutex during
// collect() (the store must be stable while it is walked). Mutations acquire
// g_store_mutex for the payload write + the trace-callback registration, then
// RELEASE g_store_mutex before calling gc_write_barrier (which parks at the
// cooperative-STW safepoint under part_cv, NOT the heap m_lock, while a collect
// is in progress). collect is invoked only from gc_collect / gc_alloc_env
// (never from within a store-mutex-held mutation), and the callback's
// visitor.report() does mark_and_push (no mutex). Because no mutation holds
// g_store_mutex across the barrier, the trace callback (g_store_mutex under
// the heap m_lock) cannot AB-BA deadlock a parked mutator (which holds neither
// g_store_mutex nor the heap m_lock while it waits on part_cv). The strict
// cooperative STW protocol (the collector waits unbounded for every other
// participant to park or exit) guarantees no mutation is mid-write when the
// callback walks the store on this runtime.
static thread_local ember::gc::GcTraceToken g_trace_token = 0;

// The trace callback: walk every array slot, report aligned i64 elements of
// elem_size==8 arrays as root candidates. user_data is unused (the callback
// walks the file-static store). ACQUIRES g_store_mutex so the process-wide
// store is stable while it is walked: collect() is invoked from gc_collect /
// gc_alloc_env on the owning thread OUTSIDE any store-mutex-held mutation (a
// mutation releases g_store_mutex before calling gc_write_barrier, which parks
// under part_cv -- NOT the heap m_lock -- while a collect is in progress, so it
// never holds g_store_mutex while the collect runs the callback), so locking
// here cannot self-deadlock + cannot nest with a mutation's lock. Under the
// strict cooperative STW protocol every other participant is parked (not
// mutating) when the callback runs, so the store walk is race-free. Without
// this lock a concurrent mutation on another thread (g_arrays is process-wide)
// would race the callback's read.
static void array_trace_cb(void* /*user_data*/, ember::gc::GcTraceVisitor& visitor) {
    std::lock_guard<std::mutex> lock(g_store_mutex);
    for (const ArraySlot& slot : g_arrays) {
        if (slot.elem_size != 8) continue;  // only i64 elements can hold a ptr
        const size_t n = slot.bytes.size() / 8;
        for (size_t i = 0; i < n; ++i) {
            int64_t v;
            std::memcpy(&v, slot.bytes.data() + i * 8, 8);
            visitor.report(reinterpret_cast<void*>(static_cast<uintptr_t>(v)));
        }
    }
}

// Ensure exactly one trace callback is registered on this thread's GC runtime,
// re-registering if the previous token was invalidated by gc_reset(). Called
// UNDER g_store_mutex by the pointer-capable mutations. No-op when the GC
// runtime is not initialized on this thread (pure non-GC mode). Idempotent in
// steady state: unregister of a still-valid token removes it, register mints a
// fresh one, so the heap's callback list stays at exactly one array entry.
static void ensure_gc_trace_cb() {
    if (!ember::ext_gc::gc_runtime_initialized()) return;
    if (g_trace_token != 0) {
        ember::ext_gc::gc_unregister_trace_callback(g_trace_token);  // no-op if stale
    }
    g_trace_token = ember::ext_gc::gc_register_trace_callback(nullptr, &array_trace_cb);
}

// Unregister this thread's trace callback (teardown). Called UNDER g_store_mutex
// by reset(). A no-op if never registered / already invalidated by gc_reset.
static void drop_gc_trace_cb() {
    if (g_trace_token != 0) {
        ember::ext_gc::gc_unregister_trace_callback(g_trace_token);
        g_trace_token = 0;
    }
}
static int64_t arr_new(int64_t elem_size, int64_t count) noexcept {
    size_t bytes = 0;
    if (!checked_bytes(elem_size, count, &bytes)) return 0;
    try {
        ArraySlot slot;
        slot.elem_size = elem_size;
        slot.bytes.assign(bytes, 0);
        g_arrays.push_back(std::move(slot));
        return int64_t(g_arrays.size());  // handle = index (1-based)
    } catch (const std::bad_alloc&) {
        return 0;
    } catch (const std::length_error&) {
        return 0;
    }
}
static ArraySlot* arr_slot(int64_t h) {
    if (h < 1 || h > int64_t(g_arrays.size())) return nullptr;
    return &g_arrays[size_t(h - 1)];
}
extern "C" {
    static int64_t n_array_new(int64_t esz, int64_t n) { std::lock_guard<std::mutex> lock(g_store_mutex); return arr_new(esz, n); }
    static int64_t n_array_length(int64_t h) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s = arr_slot(h); return s ? int64_t(s->bytes.size() / s->elem_size) : 0; }
    static void n_array_resize(int64_t h, int64_t n) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = arr_slot(h); size_t bytes = 0;
        if (!s || !checked_bytes(s->elem_size, n, &bytes)) return;
        try { s->bytes.resize(bytes, 0); }
        catch (const std::bad_alloc&) {} catch (const std::length_error&) {}
    }
    static void n_array_set_u8(int64_t h, int64_t i, int64_t v) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s = arr_slot(h); if (s && i>=0 && i<int64_t(s->bytes.size())) s->bytes[size_t(i)] = uint8_t(v); }
    static int64_t n_array_get_u8(int64_t h, int64_t i) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s = arr_slot(h); return (s && i>=0 && i<int64_t(s->bytes.size())) ? int64_t(s->bytes[size_t(i)]) : 0; }
    static void n_array_set_f32(int64_t h, int64_t i, float v) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s = arr_slot(h); if (s && i>=0 && s->elem_size==4 && size_t(i) < s->bytes.size()/s->elem_size) std::memcpy(&s->bytes[size_t(i)*4], &v, 4); }
    static float n_array_get_f32(int64_t h, int64_t i) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s = arr_slot(h); float v=0; if (s && i>=0 && s->elem_size==4 && size_t(i) < s->bytes.size()/s->elem_size) std::memcpy(&v, &s->bytes[size_t(i)*4], 4); return v; }
    static void n_array_set_i64(int64_t h, int64_t i, int64_t v) {
        void* owner = nullptr;
        void* child = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_store_mutex);
            auto* s = arr_slot(h);
            if (s && i>=0 && s->elem_size==8 && size_t(i) < s->bytes.size()/s->elem_size) {
                std::memcpy(&s->bytes[size_t(i)*8], &v, 8);
                // c1: an i64 slot can hold a managed pointer. Register the trace
                // callback (idempotent) so a future collect sees this slot. The
                // write barrier is invoked OUTSIDE g_store_mutex (below) so it
                // can park at the cooperative-STW safepoint without AB-BA
                // deadlocking the trace callback (which takes g_store_mutex
                // under the heap m_lock during a collect).
                ensure_gc_trace_cb();
                owner = reinterpret_cast<void*>(s);
                child = reinterpret_cast<void*>(static_cast<uintptr_t>(v));
            }
        }
        if (owner) ember::ext_gc::gc_write_barrier(owner, child);
    }
    static int64_t n_array_get_i64(int64_t h, int64_t i) { std::lock_guard<std::mutex> lock(g_store_mutex); auto* s = arr_slot(h); int64_t v=0; if (s && i>=0 && s->elem_size==8 && size_t(i) < s->bytes.size()/s->elem_size) std::memcpy(&v, &s->bytes[size_t(i)*8], 8); return v; }
    static void n_array_push_u8(int64_t h, int64_t v) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = arr_slot(h);
        if (!s || s->elem_size != 1 || s->bytes.size() >= MAX_CONTAINER_BYTES) return;
        try { s->bytes.push_back(uint8_t(v)); }
        catch (const std::bad_alloc&) {} catch (const std::length_error&) {}
    }
    static void n_array_push_f32(int64_t h, float v) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = arr_slot(h);
        if (!s || s->elem_size != 4 || s->bytes.size() + 4 > MAX_CONTAINER_BYTES) return;
        try { size_t off = s->bytes.size(); s->bytes.resize(off + 4); std::memcpy(&s->bytes[off], &v, 4); }
        catch (const std::bad_alloc&) {} catch (const std::length_error&) {}
    }
    static void n_array_push_i64(int64_t h, int64_t v) {
        void* owner = nullptr;
        void* child = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_store_mutex);
            auto* s = arr_slot(h);
            if (!s || s->elem_size != 8 || s->bytes.size() + 8 > MAX_CONTAINER_BYTES) return;
            try { size_t off = s->bytes.size(); s->bytes.resize(off + 8); std::memcpy(&s->bytes[off], &v, 8);
                  // c1: same as set_i64 -- a pushed i64 may be a managed pointer.
                  // Register the trace callback under the lock; the write barrier
                  // is invoked OUTSIDE g_store_mutex (below) so it can park at
                  // the cooperative-STW safepoint without AB-BA deadlock.
                  ensure_gc_trace_cb();
                  owner = reinterpret_cast<void*>(s);
                  child = reinterpret_cast<void*>(static_cast<uintptr_t>(v));
            }
            catch (const std::bad_alloc&) {} catch (const std::length_error&) {}
        }
        if (owner) ember::ext_gc::gc_write_barrier(owner, child);
    }
    static int64_t n_array_pop_u8(int64_t h) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = arr_slot(h);
        if (!s || s->elem_size != 1 || s->bytes.empty()) return 0;
        uint8_t v = s->bytes.back(); s->bytes.pop_back(); return int64_t(v);
    }
    static float n_array_pop_f32(int64_t h) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = arr_slot(h);
        if (!s || s->elem_size != 4 || s->bytes.size() < 4) return 0;
        float v; std::memcpy(&v, &s->bytes[s->bytes.size() - 4], 4);
        s->bytes.resize(s->bytes.size() - 4); return v;
    }
    static int64_t n_array_pop_i64(int64_t h) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = arr_slot(h);
        if (!s || s->elem_size != 8 || s->bytes.size() < 8) return 0;
        int64_t v; std::memcpy(&v, &s->bytes[s->bytes.size() - 8], 8);
        s->bytes.resize(s->bytes.size() - 8); return v;
    }
    static void n_array_clear(int64_t h) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = arr_slot(h);
        if (s) s->bytes.clear();
    }
    static void n_array_remove(int64_t h, int64_t i) {
        std::lock_guard<std::mutex> lock(g_store_mutex);
        auto* s = arr_slot(h);
        if (!s || i < 0 || s->elem_size == 0) return;
        size_t count = s->bytes.size() / s->elem_size;
        if (size_t(i) >= count) return;
        size_t off = size_t(i) * size_t(s->elem_size);
        s->bytes.erase(s->bytes.begin() + ptrdiff_t(off),
                       s->bytes.begin() + ptrdiff_t(off + size_t(s->elem_size)));
    }
}

// Exposed so a host native that receives an array<u8> handle (process
// read-into-array, custom shader draw calls) can read the backing bytes
// without going through a registered accessor native. The host keeps its
// own thin wrapper (prism::GetArrayBytes) that forwards here.
//
// Re-implemented here (not forwarding to arr_slot) to keep the symbol
// self-contained; the bounds/invalid-handle semantics match the original.
bool get_bytes(int64_t handle, uint8_t** out_data, int64_t* out_len) {
    std::lock_guard<std::mutex> lock(g_store_mutex);
    auto* s = arr_slot(handle);
    if (!s) return false;
    *out_data = s->bytes.data();
    *out_len = int64_t(s->bytes.size());
    return true;
}

bool copy_f32(int64_t handle, float* out_data, int64_t max_count,
              int64_t* out_count) {
    std::lock_guard<std::mutex> lock(g_store_mutex);
    auto* s = arr_slot(handle);
    if (!s || s->elem_size != int64_t(sizeof(float)) || max_count < 0 ||
        !out_count || (!out_data && !s->bytes.empty()))
        return false;
    const int64_t count = int64_t(s->bytes.size() / sizeof(float));
    if (count > max_count) return false;
    if (!s->bytes.empty()) std::memcpy(out_data, s->bytes.data(), s->bytes.size());
    *out_count = count;
    return true;
}

// Host-side alloc: mint an array<u8> handle owning a copy of [data, data+len).
// Mirrors ext_string::alloc -- a host native that produces a byte buffer (e.g.
// ext_io::file_read_bytes) calls this to hand the script an owned, persistent
// array handle the script then reads via array_get_u8 / passes to other
// natives. elem_size is fixed at 1 (u8); the handle is 1-based, 0 = failure.
// A null data with len 0 returns a valid empty array (0-byte file -> empty
// array, not failure); a null data with len > 0 returns 0 (nothing to copy).
int64_t alloc_bytes(const uint8_t* data, int64_t len) {
    std::lock_guard<std::mutex> lock(g_store_mutex);
    if (len < 0) return 0;
    if (!data && len > 0) return 0;
    size_t bytes = 0;
    if (!checked_bytes(1, len, &bytes)) return 0;
    try {
        ArraySlot slot;
        slot.elem_size = 1;
        slot.bytes.assign(bytes, 0);
        if (data && len > 0)
            std::memcpy(slot.bytes.data(), data, size_t(len));
        g_arrays.push_back(std::move(slot));
        return int64_t(g_arrays.size());  // handle = index (1-based)
    } catch (const std::bad_alloc&) {
        return 0;
    } catch (const std::length_error&) {
        return 0;
    }
}

// Registered surface is byte-identical to the old I/H/add lambda form
// (ext_registration_test asserts array_new -> i64 2 params; array_length
//  -> i64 1 param; array_get_u8 -> u8; array_push_u8 -> void 2 params).
void register_natives(std::unordered_map<std::string, NativeSig>& m) {
    BindingBuilder b;
    b.add("array_new",   type_i64(), {type_i64(),type_i64()}, (void*)&n_array_new);
    b.add("array_length",type_i64(), {type_i64()},             (void*)&n_array_length);
    b.add("array_resize",type_void(), {type_i64(),type_i64()}, (void*)&n_array_resize);
    b.add("array_set_u8",type_void(), {type_i64(),type_i64(),type_i64()}, (void*)&n_array_set_u8);
    b.add("array_get_u8",bind_prim(Prim::U8),  {type_i64(),type_i64()}, (void*)&n_array_get_u8);
    b.add("array_set_f32",type_void(), {type_i64(),type_i64(),type_f32()}, (void*)&n_array_set_f32);
    b.add("array_get_f32",type_f32(), {type_i64(),type_i64()}, (void*)&n_array_get_f32);
    b.add("array_set_i64",type_void(), {type_i64(),type_i64(),type_i64()}, (void*)&n_array_set_i64);
    b.add("array_get_i64",type_i64(), {type_i64(),type_i64()}, (void*)&n_array_get_i64);
    b.add("array_push_u8",type_void(), {type_i64(),type_i64()}, (void*)&n_array_push_u8);
    b.add("array_push_f32",type_void(), {type_i64(),type_f32()}, (void*)&n_array_push_f32);
    b.add("array_push_i64",type_void(), {type_i64(),type_i64()}, (void*)&n_array_push_i64);
    b.add("array_pop_u8", bind_prim(Prim::U8), {type_i64()}, (void*)&n_array_pop_u8);
    b.add("array_pop_f32",type_f32(), {type_i64()}, (void*)&n_array_pop_f32);
    b.add("array_pop_i64",type_i64(), {type_i64()}, (void*)&n_array_pop_i64);
    b.add("array_clear", type_void(), {type_i64()}, (void*)&n_array_clear);
    b.add("array_remove",type_void(), {type_i64(),type_i64()}, (void*)&n_array_remove);
    NativeTable t = b.build();
    for (auto& kv : t.natives) m[kv.first] = std::move(kv.second);
}

void reset() {
    std::lock_guard<std::mutex> lock(g_store_mutex);
    g_arrays.clear();
    // c1: drop this thread's trace callback so it does not outlive the store.
    // After this, a collect on this thread reports no array roots (the store is
    // empty + the callback is unregistered); any GC object that was rooted only
    // via an array entry is reclaimed on the next collect.
    drop_gc_trace_cb();
}

} // namespace ember::ext_array
