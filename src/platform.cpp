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

std::string executable_path() {
    char buf[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::string(buf, len);
}

} // namespace ember::platform

#else
// ---- Linux / macOS ----
// UNTESTED: no Linux build env — verify on Linux before relying on this.
#  include <sys/mman.h>
#  include <unistd.h>
#  include <linux/limits.h>  // PATH_MAX on Linux
#  include <string>

namespace ember::platform {

void* alloc_rw(size_t size) {
    // mmap a page-aligned, writable, non-executable region.
    long page_sz = sysconf(_SC_PAGESIZE);
    if (page_sz <= 0) page_sz = 4096;
    size_t rounded = ((size + page_sz - 1) / page_sz) * page_sz;
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

std::string executable_path() {
    // Linux: /proc/self/exe symlink. macOS: _NSGetExecutablePath (TODO).
    char buf[PATH_MAX] = {0};
    ssize_t len = readlink("/proc/self/exe", buf, PATH_MAX - 1);
    if (len <= 0) return {};
    return std::string(buf, size_t(len));
}

} // namespace ember::platform

#endif // _WIN32
