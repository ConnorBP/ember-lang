#include "jit_memory.hpp"
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace ember {

void* alloc_executable(const std::vector<uint8_t>& code) {
    // Reserve + commit, RWX. Round size up to page boundary (VirtualAlloc
    // does this anyway, but be explicit for the memcpy length sanity).
    SIZE_T size = code.size();
    void* mem = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
    if (!mem) return nullptr;
    std::memcpy(mem, code.data(), code.size());
    return mem;
}

void free_executable(void* ptr) {
    if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
}

} // namespace ember
