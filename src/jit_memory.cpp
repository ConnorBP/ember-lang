#include "jit_memory.hpp"
#include "platform.hpp"
#include <cstring>

namespace ember {

void* alloc_executable(const std::vector<uint8_t>& code) {
    void* mem = alloc_executable_rw(code);
    if (!mem) return nullptr;
    if (!seal_executable(mem, code.size())) {
        ember::platform::free_page(mem, code.size());
        return nullptr;
    }
    return mem;
}

// Two-phase: RW page, code copied in, NOT yet executable. Caller patches,
// then seal_executable() flips RX.
void* alloc_executable_rw(const std::vector<uint8_t>& code) {
    void* mem = ember::platform::alloc_rw(code.size());
    if (!mem) return nullptr;
    std::memcpy(mem, code.data(), code.size());
    return mem;
}

// W^X seal: RW -> RX. After this the page is executable and NOT writable.
bool seal_executable(void* ptr, size_t size) {
    return ember::platform::protect_rx(ptr, size);
}

void free_executable(void* ptr) {
    // size unknown here (Windows VirtualFree doesn't need it; Linux munmap does
    // but we track it via the alloc_rw size). For Windows, size is ignored.
    if (ptr) ember::platform::free_page(ptr, 0);
}

} // namespace ember
