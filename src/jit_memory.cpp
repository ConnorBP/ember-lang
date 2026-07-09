#include "jit_memory.hpp"
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace ember {

void* alloc_executable(const std::vector<uint8_t>& code) {
    void* mem = alloc_executable_rw(code);
    if (!mem) return nullptr;
    if (!seal_executable(mem, code.size())) {
        VirtualFree(mem, 0, MEM_RELEASE);
        return nullptr;
    }
    return mem;
}

// Two-phase: RW page, code copied in, NOT yet executable. Caller patches,
// then seal_executable() flips RX.
void* alloc_executable_rw(const std::vector<uint8_t>& code) {
    SIZE_T size = code.size();
    void* mem = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
    if (!mem) return nullptr;
    std::memcpy(mem, code.data(), code.size());
    return mem;
}

// W^X seal: RW -> RX. After this the page is executable and NOT writable.
bool seal_executable(void* ptr, size_t size) {
    DWORD old_prot = 0;
    return VirtualProtect(ptr, size, PAGE_EXECUTE_READ, &old_prot) != 0;
}

void free_executable(void* ptr) {
    if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
}

} // namespace ember
