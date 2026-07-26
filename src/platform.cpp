// ember platform abstraction layer — implementation.
//
// Windows path: VirtualAlloc/VirtualProtect/VirtualFree (existing code).
// Linux/macOS path: mmap/mprotect/munmap — UNTESTED (no Linux build env).
//
// docs/ROADMAP.md "Platform support" section.
#include "platform.hpp"
#include <cstring>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

namespace ember::platform {

void* alloc_rw(size_t size) {
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

bool protect_rx(void* ptr, size_t size) {
    DWORD old_prot = 0;
    return VirtualProtect(ptr, size, PAGE_EXECUTE_READ, &old_prot) != 0;
}

bool protect_rw(void* ptr, size_t size) {
    DWORD old_prot = 0;
    return VirtualProtect(ptr, size, PAGE_READWRITE, &old_prot) != 0;
}

void free_page(void* ptr, size_t /*size*/) {
    if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
}

long page_size() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return static_cast<long>(si.dwPageSize);
}

std::string executable_path() {
    char buf[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::string(buf, len);
}

bool round_up_to_page(size_t n, size_t& out) {
    if (n == 0) return false;  // 0 rounds to 0 -> mmap(..,0,..) fails EINVAL
    long ps = page_size();
    if (ps <= 0) ps = 4096;
    size_t p = size_t(ps);
    // Guard against `n + p - 1` overflow (huge/malformed sizes).
    if (n > SIZE_MAX - (p - 1)) return false;
    size_t rounded = ((n + p - 1) / p) * p;
    out = rounded;
    return true;
}

} // namespace ember::platform

#elif defined(__APPLE__)
// ---- macOS (Darwin) — Apple Silicon W^X JIT memory ----
// plan_MACOS_ARM64.md Phase 1. On arm64 macOS the simple mmap/mprotect model is
// INSUFFICIENT: under the hardened runtime, JIT pages must be allocated with
// MAP_JIT, and writability is controlled by a THREAD-LOCAL toggle
// (pthread_jit_write_protect_np), not a per-page mprotect. After writing code
// and before executing it, the D-cache and I-cache are incoherent on ARM, so
// sys_icache_invalidate is MANDATORY.
//
// Model (mirrors the existing jit_memory two-phase API: RW alloc -> patch ->
// RX seal):
//   alloc_rw        : mmap(MAP_JIT | PROT_READ|PROT_WRITE) + enable writes on
//                     this thread (pthread_jit_write_protect_np(0)). The page
//                     is writable on the calling thread until protect_rx.
//   protect_rw      : re-enable thread-local writes (for post-seal patching).
//   protect_rx      : disable thread-local writes (pthread_jit_write_protect_np(1))
//                     + sys_icache_invalidate(addr,len) — the W^X seal.
//   free_page       : munmap(ptr, size). The caller MUST pass the rounded size
//                     (jit_memory tracks it; the old size-0 path leaked on POSIX).
//
// The toggle is thread-local, so each thread manages its own write window —
// correct for the per-thread alloc->patch->seal sequences ember uses. The
// hardened runtime requires the com.apple.security.cs.allow-jit entitlement for
// the host process; document it (the dev CLI runs without hardened runtime).
#  include <sys/mman.h>
#  include <unistd.h>
#  include <pthread.h>               // pthread_jit_write_protect_np
#  include <libkern/OSCacheControl.h> // sys_icache_invalidate
#  include <mach-o/dyld.h>           // _NSGetExecutablePath
#  include <climits>
#  include <cstring>
#  include <string>
#  ifndef PATH_MAX
#    define PATH_MAX 4096
#  endif

namespace ember::platform {

void* alloc_rw(size_t size) {
    // Reject size==0 explicitly: the old `((0 + p - 1)/p)*p` rounded to 0 and
    // mmap(..,0,..) fails with EINVAL (returning MAP_FAILED), so callers got
    // nullptr anyway — but the intent (a 0-byte alloc) is a caller bug, so fail
    // fast + deterministically rather than relying on mmap to reject it.
    size_t rounded = 0;
    if (!round_up_to_page(size, rounded)) return nullptr;  // size==0 or overflow
    // MAP_JIT is required for JIT pages on arm64 macOS under the hardened
    // runtime. PROT_READ|PROT_WRITE: writable now (sealed to RX by protect_rx).
    void* mem = mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    // Enable writes on this thread for the upcoming code copy/patch. The caller
    // seals with protect_rx (which disables writes + flushes the icache).
    pthread_jit_write_protect_np(0);
    return mem;
}

bool protect_rx(void* ptr, size_t size) {
    // W^X seal: disable thread-local writes (pthread_jit_write_protect_np(1)),
    // flip the page to PROT_READ|PROT_EXEC (the toggle controls writability;
    // mprotect is required to GRANT executability — the toggle alone leaves the
    // page non-executable and executing it bus-errors), and invalidate the
    // icache so the freshly written code is actually executed.
    pthread_jit_write_protect_np(1);
    if (mprotect(ptr, size, PROT_READ | PROT_EXEC) != 0) {
        // RESTORE the write-ENABLED state on mprotect failure. Without this,
        // the thread is left write-DISABLED (the pthread_jit_write_protect_np(1)
        // above already flipped it), so any subsequent compilation write on
        // this thread (e.g. the next alloc_rw's code copy, or a post-seal patch)
        // would fault with a write-protect violation. Restore before returning
        // the error so the caller can proceed / clean up safely.
        pthread_jit_write_protect_np(0);
        return false;
    }
    sys_icache_invalidate(ptr, size);
    return true;
}

bool protect_rw(void* ptr, size_t size) {
    // Re-enable writes for post-seal patching: flip the page back to RW (drop
    // PROT_EXEC — W^X, never RWX) and enable the thread-local write toggle.
    // Re-seal with protect_rx after patching.
    if (mprotect(ptr, size, PROT_READ | PROT_WRITE) != 0) return false;
    pthread_jit_write_protect_np(0);
    return true;
}

void free_page(void* ptr, size_t size) {
    // size MUST be the rounded allocation size (munmap with size 0 is a no-op on
    // POSIX — a leak). jit_memory tracks + passes the rounded size.
    if (ptr) munmap(ptr, size);
}

long page_size() {
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? ps : 16384;
}

std::string executable_path() {
    // macOS: _NSGetExecutablePath returns a path possibly with symlinks; realpath
    // canonicalizes it. The buffer may be too small on first try (the function
    // reports the needed size via the out param), so retry once.
    uint32_t bufsize = 0;
    if (_NSGetExecutablePath(nullptr, &bufsize) == -1) {
        std::string buf(bufsize, '\0');
        if (_NSGetExecutablePath(buf.data(), &bufsize) == 0) {
            char resolved[PATH_MAX] = {0};
            if (realpath(buf.c_str(), resolved)) return std::string(resolved);
            return buf;
        }
        return {};
    }
    bufsize = PATH_MAX;
    std::string buf(bufsize, '\0');
    if (_NSGetExecutablePath(buf.data(), &bufsize) == 0) {
        char resolved[PATH_MAX] = {0};
        if (realpath(buf.c_str(), resolved)) return std::string(resolved);
        buf.resize(strlen(buf.c_str()));
        return buf;
    }
    return {};
}

bool round_up_to_page(size_t n, size_t& out) {
    if (n == 0) return false;
    long ps = page_size();
    if (ps <= 0) ps = 16384;
    size_t p = size_t(ps);
    if (n > SIZE_MAX - (p - 1)) return false;
    size_t rounded = ((n + p - 1) / p) * p;
    out = rounded;
    return true;
}

} // namespace ember::platform

#elif defined(__linux__)
// ---- Linux ----
// UNTESTED: no Linux build env — verify on Linux before relying on this.
#  include <sys/mman.h>
#  include <unistd.h>
#  include <linux/limits.h>  // PATH_MAX on Linux
#  include <string>

namespace ember::platform {

void* alloc_rw(size_t size) {
    size_t rounded = 0;
    if (!round_up_to_page(size, rounded)) return nullptr;  // size==0 or overflow
    void* mem = mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    return mem;
}

bool protect_rx(void* ptr, size_t size) {
    return mprotect(ptr, size, PROT_READ | PROT_EXEC) == 0;
}

bool protect_rw(void* ptr, size_t size) {
    return mprotect(ptr, size, PROT_READ | PROT_WRITE) == 0;
}

void free_page(void* ptr, size_t size) {
    if (ptr) munmap(ptr, size);
}

long page_size() {
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? ps : 4096;
}

std::string executable_path() {
    char buf[PATH_MAX] = {0};
    ssize_t len = readlink("/proc/self/exe", buf, PATH_MAX - 1);
    if (len <= 0) return {};
    return std::string(buf, size_t(len));
}

bool round_up_to_page(size_t n, size_t& out) {
    if (n == 0) return false;
    long ps = page_size();
    if (ps <= 0) ps = 4096;
    size_t p = size_t(ps);
    if (n > SIZE_MAX - (p - 1)) return false;
    size_t rounded = ((n + p - 1) / p) * p;
    out = rounded;
    return true;
}

} // namespace ember::platform

#else
#  error "unsupported platform: add a platform.cpp implementation for this target"
#endif // _WIN32 / __APPLE__ / __linux__
