// ember executable-memory allocator (v0.1).
// CODEGEN_SPEC.md Section jit_memory: VirtualAlloc RWX, copy code, return
// executable pointer. W^X (RW then RX) is a post-v1 hardening; RWX
// is fine for the v0.1 proof.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace ember {

// Allocates executable memory, copies `code` into it, returns the
// entry pointer. The allocation is owned by the caller (caller must
// keep it alive for as long as any JIT'd function is callable).
void* alloc_executable(const std::vector<uint8_t>& code);

// Frees an allocation returned by alloc_executable.
void  free_executable(void* ptr);

} // namespace ember
