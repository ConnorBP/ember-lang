#include "jit_memory.hpp"
#include "platform.hpp"
#include "safety.hpp"
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace ember {

// Track the rounded allocation size for each executable page so free_executable
// can munmap the exact mapped length (POSIX munmap with size 0 is a no-op — a
// JIT-memory leak that this fixes, plan_MACOS_ARM64.md Phase 1). Windows
// VirtualFree(.., 0, MEM_RELEASE) ignores size, so the map is harmless there.
// The map is shared mutable container state: insert/erase/find/rehash race even
// on DIFFERENT keys, so every access is serialized by page_sizes_mutex (a
// concurrent compile/free from another thread of a distinct page still touches
// the same map — the earlier "distinct entries are safe" claim was wrong).
namespace {
std::unordered_map<void*, size_t>& page_sizes() {
    static std::unordered_map<void*, size_t> m;
    return m;
}
std::mutex& page_sizes_mutex() {
    static std::mutex mu;
    return mu;
}
// Overflow-checked page rounding delegates to platform::round_up_to_page
// (shared helper: rejects 0 + guards against `n + page_sz - 1` overflow for
// huge/malformed code sizes, which would wrap to a tiny length and mmap the
// wrong region). Returns 0 on overflow — the caller treats that as an
// allocation failure upstream.
inline size_t round_up_to_page(size_t n) {
    size_t out = 0;
    ember::platform::round_up_to_page(n, out);
    return out;
}
} // namespace

void* alloc_executable(const std::vector<uint8_t>& code) {
    // SAFETY FAILSAFE: check process RSS before allocating another executable
    // page. Repeated compilation (hot reload, benchmarks, pass tests) can
    // accumulate executable + retained-byte memory; this stops unbounded JIT
    // memory growth before it exhausts host RAM.
    safety::check_memory_limit();
    void* mem = alloc_executable_rw(code);
    if (!mem) return nullptr;
    if (!seal_executable(mem, code.size())) {
        size_t rounded;
        {
            std::lock_guard<std::mutex> lk(page_sizes_mutex());
            auto it = page_sizes().find(mem);
            rounded = (it != page_sizes().end()) ? it->second : round_up_to_page(code.size());
            if (it != page_sizes().end()) page_sizes().erase(it);
        }
        ember::platform::free_page(mem, rounded);
        return nullptr;
    }
    return mem;
}

// Two-phase: RW page, code copied in, NOT yet executable. Caller patches,
// then seal_executable() flips RX. On Apple Silicon the page is MAP_JIT and
// the calling thread is left write-enabled (alloc_rw toggled
// pthread_jit_write_protect_np(0)); seal_executable flips it to RX + flushes
// the icache. The post-seal patch path must wrap its writes in protect_rw
// (re-enable writes) then re-seal.
void* alloc_executable_rw(const std::vector<uint8_t>& code) {
    void* mem = ember::platform::alloc_rw(code.size());
    if (!mem) return nullptr;
    std::memcpy(mem, code.data(), code.size());
    {
        std::lock_guard<std::mutex> lk(page_sizes_mutex());
        page_sizes()[mem] = round_up_to_page(code.size());
    }
    return mem;
}

// W^X seal: RW -> RX. After this the page is executable and NOT writable.
// On Apple Silicon this disables thread-local writes + invalidates the icache.
bool seal_executable(void* ptr, size_t size) {
    return ember::platform::protect_rx(ptr, size);
}

// Frees an allocation returned by alloc_executable / alloc_executable_rw.
// Passes the tracked rounded size so POSIX munmap frees the whole mapped
// region (the previous size-0 path leaked every JIT page on Linux/macOS).
void free_executable(void* ptr) {
    if (!ptr) return;
    size_t rounded;
    {
        std::lock_guard<std::mutex> lk(page_sizes_mutex());
        auto& sizes = page_sizes();
        auto it = sizes.find(ptr);
        rounded = (it != sizes.end()) ? it->second : 0;
        if (it != sizes.end()) sizes.erase(it);
    }
    // Windows ignores the size; POSIX needs the exact rounded length. If we
    // somehow have no tracked size (e.g. an external page), fall back to 0 —
    // Windows frees it; POSIX will leak that one page (logged nowhere; the
    // safety RSS cap still bounds total growth).
    ember::platform::free_page(ptr, rounded);
}

} // namespace ember
