// ember executable-memory allocator.
// docs/spec/CODEGEN_SPEC.md Section jit_memory: W^X - VirtualAlloc RW, copy code,
// VirtualProtect to RX. The page is never simultaneously writable and
// executable (v0.4 hardening, red-team V5 mitigation: a write primitive
// that ever lands in a JIT page must not also be an execute page).
//
// Two-phase API for callers that must patch the bytes after copy but
// before execution (e.g. em_loader applies load-time relocations into the
// page): alloc_executable_rw() returns an RW page; seal_executable() flips
// it to RX and freezes it. finalize-time callers with no post-copy patches
// use alloc_executable() (the one-shot RW->memcpy->RX convenience).
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace ember {

// One-shot: allocate RW, copy `code`, flip to RX, return the executable
// pointer. Use when no post-copy patching is needed (e.g. engine finalize).
void* alloc_executable(const std::vector<uint8_t>& code);

// Two-phase: allocate RW + copy `code`, return a WRITABLE (not yet
// executable) pointer. Caller patches bytes (relocs), then calls
// seal_executable() to flip RX. Returns nullptr on alloc/copy failure.
void* alloc_executable_rw(const std::vector<uint8_t>& code);
// Flip a page from alloc_executable_rw to EXECUTE_READ (W^X seal). Returns
// false on VirtualProtect failure (caller should free_executable the page).
bool  seal_executable(void* ptr, size_t size);

// Frees an allocation returned by alloc_executable / alloc_executable_rw.
void  free_executable(void* ptr);

} // namespace ember
