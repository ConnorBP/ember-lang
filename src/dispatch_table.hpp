// ember dispatch table (v0.2 minimal).
// docs/spec/CODEGEN_SPEC.md Section 7 / docs/HOT_RELOAD.md Section 1: one pointer slot per script
// function; script-to-script and host-to-script calls go through
// `call [table_base + slot*8]`.
#pragma once
#include <cstdint>
#include <vector>
#include <atomic>
#include <cstddef>
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

// ─── Keyed physical dispatch storage (Red 4, plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md
//   §4.5, §10.1, §10.4) ───────────────────────────────────────────────────────
// The physical dispatch backing for a keyed module. This is the publication-
// safe atomic slot storage a ModuleDispatchRecord::physical_slots points at.
// It is a vector of std::atomic<void*> indexed by PHYSICAL slot (the key-
// influenced permutation order), NOT by logical slot — a bare logical index
// into this array would reach an arbitrary same-domain entry rather than the
// intended callable (§9.8), so every read goes through the keyed resolver.
//
// Publication is strict (§10.4 / §14.2 "Physical table publication with
// null/unfinalized entry" is a should-fail): publish_keyed validates EVERY
// (slot, entry) pair FIRST (slot in range + entry non-null), then commits all
// pairs with release ordering. A null or unfinalized entry aborts publication
// with NO slot mutated — the storage is left byte-for-byte unchanged, so a
// half-published keyed table is impossible. This converges JIT and loaded
// modules on the same publication-safe atomic storage the existing DispatchTable
// uses for identity mode (§4.5).
//
// Identity-layout (disabled / unkeyed) behavior is preserved unchanged: the
// existing DispatchTable above is untouched, and a keyed module whose mode is
// Identity does not use this storage. KeyedDispatchStorage is ONLY the physical
// backing for keyed mode; it adds no new logical-slot read path.
struct KeyedDispatchStorage {
    std::vector<std::atomic<void*>> slots;

    KeyedDispatchStorage() = default;
    explicit KeyedDispatchStorage(size_t physical_count) : slots(physical_count) {
        for (auto& s : slots) s.store(nullptr, std::memory_order_relaxed);
    }

    size_t size() const { return slots.size(); }

    // Acquire-load a physical slot. Returns nullptr for an out-of-range index
    // (defensive — the resolver range-checks first, but a direct host read
    // must never fault). This is the ONLY sanctioned read of a keyed physical
    // slot; it is NEVER indexed by a bare logical slot (§9.8).
    void* load_physical(size_t physical_slot) const {
        if (physical_slot >= slots.size()) return nullptr;
        return slots[physical_slot].load(std::memory_order_acquire);
    }

    // Expose the physical slot array base for a ModuleDispatchRecord's
    // physical_slots pointer. The record holds a `const std::atomic<void*>*`
    // that points at this storage's backing array. Lifetime: the storage must
    // outlive any record that references it (the host owns the storage on the
    // ModuleInstance, §10.3).
    const std::atomic<void*>* physical_base() const {
        return slots.data();
    }

    // Strict batch publication: validate EVERY (slot, entry) pair FIRST (slot
    // in range + entry non-null), then commit all with release ordering.
    // Returns false on a validation failure with NO slot mutated — the storage
    // is left byte-for-byte unchanged, so a host that staged its compiled
    // functions into private ownership can free them and report a clean
    // publication failure with no partial record visible to callers. This is
    // the §10.4 publication invariant: no keyed module becomes reachable until
    // every physical position is filled with a non-null finalized entry.
    bool publish_keyed(const std::vector<std::pair<size_t, void*>>& entries) {
        for (const auto& [slot, fn] : entries) {
            if (slot >= slots.size()) return false;   // out of range -> reject, no mutation
            if (!fn) return false;                    // null/unfinalized -> reject, no mutation
        }
        for (const auto& [slot, fn] : entries)
            slots[slot].store(fn, std::memory_order_release);
        return true;
    }

    // True iff every physical slot is null (no record visible). Used by tests
    // to assert the storage is observably unchanged after a failed publication.
    bool all_clear() const {
        for (const auto& s : slots) if (s.load(std::memory_order_acquire) != nullptr) return false;
        return true;
    }
};

} // namespace ember
