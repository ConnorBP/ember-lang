// ember module registry impl (docs/MODULES.md Section 2).
//
// See module_registry.hpp for the design and the REGISTRY-BASE STABILITY
// INVARIANT. The implementation is small: size-at-construction +
// fill-by-index + no-grow-past-capacity, so `entries_.data()` (== `base()`)
// never moves after construction.

#include "module_registry.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ember {

ModuleRegistry::ModuleRegistry(uint32_t capacity)
    : capacity_(capacity) {
    // Size the backing storage NOW so base() is stable from construction.
    // fill-by-index in register_module never reallocates; capacity is a hard
    // ceiling (registering past it is a hard error, not a silent grow - see
    // the header's REGISTRY-BASE STABILITY INVARIANT).
    entries_.assign(static_cast<size_t>(capacity), nullptr);
    names_.resize(static_cast<size_t>(capacity));
    by_name_.reserve(static_cast<size_t>(capacity));
    // v1.0 Tier 2: size the handle-records array up front too (same stability
    // invariant — handle_records_base() must never move). Zero-filled so an
    // unregistered id has a null dispatch / 0 slot_count (a cross-module handle
    // into it fails the guard's range or slot_count check and traps).
    handle_records_.assign(static_cast<size_t>(capacity), ModuleHandleRecord{nullptr, nullptr, 0});
    // X1 redesign: size the per-module dispatch-slot-count array up front too
    // (same stability invariant — sized at construction, filled by index, never
    // grown). Zero-filled so an unregistered/unpublished id has a 0 dispatch
    // size and a CallCrossModule into it fails the validator's slot range
    // check (the secure default — a module that did not publish its dispatch
    // size cannot be the validated target of a cross-module direct dispatch).
    dispatch_slot_counts_.assign(static_cast<size_t>(capacity), 0);
}

uint32_t ModuleRegistry::register_module(const std::string& name,
                                         void* dispatch_table_base,
                                         std::string* err,
                                         void* allowlist_base,
                                         int64_t slot_count) {
    if (!dispatch_table_base) {
        if (err) *err = "ModuleRegistry: null dispatch table base";
        return UINT32_MAX;
    }
    if (slot_count < 0 || (slot_count > 0 && !allowlist_base)) {
        if (err) *err = "ModuleRegistry: invalid function-handle allowlist registration";
        return UINT32_MAX;
    }
    // Reload (Section 4): same name -> update the existing entry's table pointer,
    // keep the id. Callers that cached (module_id, slot) pick up the new table
    // on the next call.
    auto it = by_name_.find(name);
    if (it != by_name_.end()) {
        uint32_t id = it->second;
        entries_[id] = dispatch_table_base;
        handle_records_[id] = ModuleHandleRecord{dispatch_table_base, allowlist_base, slot_count};
        return id;
    }

    // New registration: assign the next dense id.
    if (next_id_ >= capacity_) {
        if (err) *err = "ModuleRegistry: capacity exhausted (registered " +
                        std::to_string(next_id_) + " of " +
                        std::to_string(capacity_) + "; grow is forbidden by the "
                        "REGISTRY-BASE STABILITY INVARIANT - raise the registry's "
                        "construction capacity)";
        return UINT32_MAX;
    }
    uint32_t id = next_id_++;
    entries_[id] = dispatch_table_base;
    names_[id]  = name;
    handle_records_[id] = ModuleHandleRecord{dispatch_table_base, allowlist_base, slot_count};
    by_name_.emplace(name, id);
    return id;
}

void* ModuleRegistry::resolve(uint32_t module_id) const {
    if (module_id >= next_id_) return nullptr;  // out of range -> nullptr (header docstring)
    return entries_[module_id];
}

void* ModuleRegistry::base() const {
    return const_cast<void*>(static_cast<const void*>(entries_.data()));
}

uint32_t ModuleRegistry::count() const { return next_id_; }

uint32_t ModuleRegistry::find_by_name(const std::string& name) const {
    auto it = by_name_.find(name);
    if (it == by_name_.end()) return UINT32_MAX;
    return it->second;
}

const std::string& ModuleRegistry::name_of(uint32_t module_id) const {
    static const std::string empty;
    if (module_id >= next_id_) return empty;
    return names_[module_id];
}

// Preflight: mirror register_module's acceptance conditions WITHOUT mutating.
// Same failure modes (null base, invalid allowlist is N/A here since the host
// boundary registers __main__ with the default no-allowlist), same id
// assignment (existing id for a reload, next dense id for a new name). The
// only side-effect-free read is by_name_ + next_id_ + capacity_ (all stable
// during a single publication sequence).
uint32_t ModuleRegistry::preflight_register_module(const std::string& name,
                                                    void* dispatch_table_base,
                                                    std::string* err) const {
    if (!dispatch_table_base) {
        if (err) *err = "ModuleRegistry: null dispatch table base";
        return UINT32_MAX;
    }
    auto it = by_name_.find(name);
    if (it != by_name_.end()) return it->second;  // reload keeps the id
    if (next_id_ >= capacity_) {
        if (err) *err = "ModuleRegistry: capacity exhausted (registered " +
                        std::to_string(next_id_) + " of " +
                        std::to_string(capacity_) + "; grow is forbidden by the "
                        "REGISTRY-BASE STABILITY INVARIANT - raise the registry's "
                        "construction capacity)";
        return UINT32_MAX;
    }
    return next_id_;  // the id a new registration would assign
}

// v1.0 Tier 2 cross-module handles: the per-module records table accessors.
void* ModuleRegistry::handle_records_base() const {
    return const_cast<void*>(static_cast<const void*>(handle_records_.data()));
}

uint32_t ModuleRegistry::handle_records_count() const { return next_id_; }

ModuleHandleRecord ModuleRegistry::handle_record(uint32_t module_id) const {
    if (module_id >= next_id_) return ModuleHandleRecord{nullptr, nullptr, 0};
    return handle_records_[module_id];
}

// v5 IR CallCrossModule dispatch-slot validation (X1 redesign). See the header
// docstring for the threat model. These mirror handle_record's range-checked,
// zero-on-out-of-range shape so a host can read a count for any id without a
// separate count() guard.
int64_t ModuleRegistry::dispatch_slot_count(uint32_t module_id) const {
    if (module_id >= next_id_) return 0;
    return dispatch_slot_counts_[module_id];
}

void ModuleRegistry::set_dispatch_slot_count(uint32_t module_id, int64_t count) {
    if (module_id >= next_id_) return;  // defensive; register_module failed
    dispatch_slot_counts_[module_id] = (count > 0) ? count : 0;
}

} // namespace ember
