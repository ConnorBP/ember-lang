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
}

uint32_t ModuleRegistry::register_module(const std::string& name,
                                         void* dispatch_table_base,
                                         std::string* err) {
    // Reload (Section 4): same name -> update the existing entry's table pointer,
    // keep the id. Callers that cached (module_id, slot) pick up the new table
    // on the next call.
    auto it = by_name_.find(name);
    if (it != by_name_.end()) {
        uint32_t id = it->second;
        entries_[id] = dispatch_table_base;
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

} // namespace ember
