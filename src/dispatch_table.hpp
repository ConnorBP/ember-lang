// ember dispatch table (v0.2 minimal).
// CODEGEN_SPEC.md Section 7 / HOT_RELOAD.md Section 1: one pointer slot per script
// function; script-to-script and host-to-script calls go through
// `call [table_base + slot*8]`.
#pragma once
#include <cstdint>
#include <vector>
#include <atomic>

namespace ember {

struct DispatchTable {
    std::vector<std::atomic<void*>> slots;

    DispatchTable() = default;            // empty (v1.0: lets a host hold a table field that's sized later)
    explicit DispatchTable(size_t count) : slots(count) {
        for (auto& s : slots) s.store(nullptr, std::memory_order_relaxed);
    }

    void set(size_t slot, void* fn) {
        slots[slot].store(fn, std::memory_order_release);
    }

    void* get(size_t slot) const {
        return slots[slot].load(std::memory_order_acquire);
    }

    void* base() { return const_cast<void*>(static_cast<const void*>(slots.data())); }
};

} // namespace ember
