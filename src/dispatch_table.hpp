// ember dispatch table (v0.2 minimal).
// docs/spec/CODEGEN_SPEC.md Section 7 / docs/HOT_RELOAD.md Section 1: one pointer slot per script
// function; script-to-script and host-to-script calls go through
// `call [table_base + slot*8]`.
#pragma once
#include <cstdint>
#include <vector>
#include <atomic>
#include <stdexcept>

namespace ember {

struct DispatchTable {
    std::vector<std::atomic<void*>> slots;

    DispatchTable() = default;            // empty (v1.0: lets a host hold a table field that's sized later)
    explicit DispatchTable(size_t count) : slots(count) {
        for (auto& s : slots) s.store(nullptr, std::memory_order_relaxed);
    }

    void set(size_t slot, void* fn) {
        if (slot >= slots.size())
            throw std::out_of_range("DispatchTable::set: slot out of range");
        // A null dispatch entry would be dereferenced by the generated
        // `call [base + slot*8]` and fault with 0xC0000005 outside Ember's
        // trap model. Reject it at publication time as a host-API misuse so
        // the error is a recoverable C++ exception (catchable by the host)
        // rather than a process fault. HotReloadDomain::publish already
        // rejects null before reaching here; this guards direct host writes.
        if (!fn)
            throw std::invalid_argument("DispatchTable::set: null function");
        slots[slot].store(fn, std::memory_order_release);
    }

    void* get(size_t slot) const {
        if (slot >= slots.size()) return nullptr;
        return slots[slot].load(std::memory_order_acquire);
    }

    // Atomic batch publication: validate EVERY (slot, entry) pair FIRST
    // (slot in range + entry non-null), then commit all pairs with release
    // ordering. Returns false on a validation failure with NO slot mutated —
    // the table is left byte-for-byte unchanged, so a host that staged its
    // compiled functions into private ownership can free them and report a
    // clean publication failure with no partial record visible to callers.
    // This is the prevalidated batch dispatch publication path the host
    // boundary uses so a half-published dispatch table is impossible. A pair
    // list may be empty (returns true; nothing to publish is a valid no-op).
    bool publish_batch(const std::vector<std::pair<size_t, void*>>& entries) {
        for (const auto& [slot, fn] : entries) {
            if (slot >= slots.size()) return false;     // out of range -> reject, no mutation
            if (!fn) return false;                      // null fn -> reject, no mutation
        }
        // All pairs validated: commit atomically (release ordering so a
        // caller that loads a slot with acquire sees a fully-published table).
        for (const auto& [slot, fn] : entries)
            slots[slot].store(fn, std::memory_order_release);
        return true;
    }

    // True iff every slot is null (no record visible). Used by hosts/tests to
    // assert the table is observably unchanged after a failed publication.
    bool all_clear() const {
        for (const auto& s : slots) if (s.load(std::memory_order_acquire) != nullptr) return false;
        return true;
    }

    void* base() { return const_cast<void*>(static_cast<const void*>(slots.data())); }
};

} // namespace ember
