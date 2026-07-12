// ember platform abstraction layer.
//
// Wraps OS-specific calls (JIT memory allocation/protection, executable path
// resolution) behind a single API. The Windows path is the existing code;
// the Linux/macOS path (mmap/mprotect, /proc/self/exe) is written but UNTESTED
// (no Linux build environment — verify on Linux before relying on it).
//
// This is the groundwork for the platform ports (ROADMAP: Linux x64, macOS,
// 32-bit, ARM64). The coroutine fiber abstraction is in ext_coroutine.hpp
// (CreateFiber/SwitchToFiber on Windows; ucontext on Linux — TODO).
//
// docs/ROADMAP.md "Platform support" section.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace ember::platform {

// ---- JIT memory (W^X: RW during patch, RX during execute) ----

// Allocate a writable (NOT executable) page of `size` bytes. Returns nullptr
// on failure. The caller copies code in, then calls protect_rx to seal it.
void* alloc_rw(size_t size);

// Flip a page from RW to RX (W^X seal). Returns false on failure.
bool protect_rx(void* ptr, size_t size);

// Flip a page back from RX to RW (for patching, then re-seal with protect_rx).
// Returns false on failure.
bool protect_rw(void* ptr, size_t size);

// Free an allocation from alloc_rw.
void free_page(void* ptr, size_t size);

// ---- Executable path ----

// Returns the path to the current executable (for the standalone bundler
// to find its embedded .em module). Empty string on failure.
std::string executable_path();

} // namespace ember::platform
