// hot_reload_domain.hpp — the HotReloadDomain reclamation class, extracted
// from hot_reload.hpp so the CORE `ember` lib can hold an ExecutionGuard
// without pulling the codegen/sema/parser headers `reload_function` needs.
//
// hot_reload.hpp keeps `reload_function` (which needs codegen.hpp / sema.hpp /
// parser.hpp) and now `#include`s this header for the class. The CORE lib
// (engine.cpp) includes ONLY this header, so the one-way link direction
// (core must not depend on ember_frontend) is preserved: the class depends on
// dispatch_table.hpp + the stdlib, nothing frontend-specific.
//
// Red 8 (plan_IMPLICIT_ENVIRONMENTAL_KEYED_DISPATCH.md §9.8, §12.4): the keyed
// host boundary holds an ExecutionGuard on this domain from resolution through
// return or recovered trap. longjmp skips C++ destructors, so the guard cannot
// be plain RAII across a trap — ExecutionGuard::reset() is PUBLIC so the keyed
// call core can manually leave the domain on a trapped exit (and on a normal
// exit) instead of relying on a destructor that will not run.
//
// See hot_reload.hpp for the full hot-reload design + reload_function.
#pragma once
#include "dispatch_table.hpp"  // DispatchTable
#include "jit_memory.hpp"      // free_executable (reclamation uses it)
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ember {

// A small, host-owned reclamation domain for one reloadable dispatch table (or
// a set of tables whose outer calls are all guarded by this same domain). It
// has no background worker and no process-global participant registry.
class HotReloadDomain {
public:
    struct PublicationInfo {
        bool ok = false;
        const char* error = nullptr;
        uint64_t publication_epoch = 0;
        uint64_t retirement_epoch = 0; // zero when there was no old page
        bool old_page_retired = false;
    };

    class ExecutionGuard {
    public:
        explicit ExecutionGuard(HotReloadDomain& domain)
            : domain_(&domain), entry_epoch_(domain.enter()) {}
        ~ExecutionGuard() { reset(); }

        ExecutionGuard(const ExecutionGuard&) = delete;
        ExecutionGuard& operator=(const ExecutionGuard&) = delete;

        ExecutionGuard(ExecutionGuard&& other) noexcept
            : domain_(other.domain_), entry_epoch_(other.entry_epoch_) {
            other.domain_ = nullptr;
        }
        ExecutionGuard& operator=(ExecutionGuard&& other) noexcept {
            if (this != &other) {
                reset();
                domain_ = other.domain_;
                entry_epoch_ = other.entry_epoch_;
                other.domain_ = nullptr;
            }
            return *this;
        }

        uint64_t entry_epoch() const { return entry_epoch_; }

        // Red 8 (§9.8, §12.4): release the guard early. The keyed host boundary
        // calls this on BOTH normal and trapped exits — longjmp skips C++
        // destructors, so the guard cannot be plain RAII across a trap. The
        // guard is held from resolution through return/trap; this lets the core
        // leave the domain deterministically on every exit path instead of
        // relying on a destructor that will not run after a longjmp. Idempotent
        // (safe to call again from the destructor or a second reset()).
        void reset() {
            if (domain_) {
                domain_->leave(entry_epoch_);
                domain_ = nullptr;
            }
        }

    private:
        HotReloadDomain* domain_ = nullptr;
        uint64_t entry_epoch_ = 0;
    };

    HotReloadDomain() = default;
    HotReloadDomain(const HotReloadDomain&) = delete;
    HotReloadDomain& operator=(const HotReloadDomain&) = delete;

    // Domain lifetime must exceed every guard. Destruction establishes a
    // final quiescent point and frees any still-retired pages; it does not own
    // or free the current entries still published in the dispatch table.
    ~HotReloadDomain() { quiesce(); }

    ExecutionGuard guard() { return ExecutionGuard(*this); }

    // Publish a finalized page and transfer ownership of the replaced page to
    // this domain. On failure (null/same entry, epoch exhaustion, retired-page
    // cap, or inability to record retirement), the table and epoch remain
    // unchanged and the caller still owns new_entry.
    PublicationInfo publish(DispatchTable& table, size_t slot, void* new_entry) {
        PublicationInfo info;
        if (!new_entry || slot >= table.slots.size()) {
            info.error = "invalid entry or dispatch slot";
            return info;
        }

        std::lock_guard<std::mutex> lock(mu_);
        void* old_entry = table.get(slot);
        if (old_entry == new_entry) {
            info.error = "entry is already published";
            return info;
        }
        if (epoch_ == std::numeric_limits<uint64_t>::max()) {
            info.error = "publication epoch exhausted";
            return info;
        }

        // An executable page must be uniquely bound to one (domain, table, slot)
        // for retirement ownership to be sound. If old_entry is still current in
        // another slot of this table, retiring it here would let reclaim free a
        // page that the other slot still publishes — a guarded call through
        // that other slot would then jump to decommitted memory. Scan every
        // other slot before retiring: if any still holds old_entry, do NOT retire
        // it to retired_ (the page stays current via that other slot and will be
        // retired only when its last publishing slot is replaced).
        bool old_still_aliased = false;
        if (old_entry) {
            for (size_t i = 0; i < table.slots.size(); ++i) {
                if (i == slot) continue;
                if (table.get(i) == old_entry) { old_still_aliased = true; break; }
            }
        }

        const uint64_t next_epoch = epoch_ + 1;
        if (old_entry && !old_still_aliased) {
            // Reclaim everything currently eligible before enforcing the hard
            // cap. If a hung ExecutionGuard pins all old pages, reject this
            // publication rather than allowing executable memory to grow
            // without bound (or unsafely freeing code still in execution).
            if (retired_.size() >= kMaxRetiredPages) {
                reclaim_locked(epoch_);
                if (retired_.size() >= kMaxRetiredPages) {
                    info.error = "retired executable-page cap reached";
                    return info;
                }
            }
            try {
                retired_.push_back({old_entry, next_epoch});
            } catch (...) {
                info.error = "unable to record retired executable page";
                return info;
            }
        }

        // Publish the new epoch before the release-store. Guard enrollment is
        // serialized by this mutex: a guard is either recorded at the old
        // epoch before the store, or can enter only after the store completed
        // and records the new epoch. DispatchTable::get acquire-pairs with set.
        epoch_ = next_epoch;
        table.set(slot, new_entry);

        info.ok = true;
        info.publication_epoch = next_epoch;
        info.old_page_retired = old_entry != nullptr && !old_still_aliased;
        info.retirement_epoch = (old_entry && !old_still_aliased) ? next_epoch : 0;
        cv_.notify_all();
        return info;
    }

    // Nonblocking reclamation. A page retired at E is eligible exactly when no
    // active guard entered before E. Guards entering at E or later were
    // serialized after the release publication and therefore do not pin that
    // old page.
    size_t reclaim() {
        std::lock_guard<std::mutex> lock(mu_);
        return reclaim_locked(epoch_);
    }

    // Wait until every page retired no later than the epoch observed on entry
    // is eligible, then free those pages. Newer publications are intentionally
    // outside this call's snapshot. In a single-threaded host with no active
    // guard this completes immediately after publication. A hung guard cannot
    // block shutdown forever: after 30 seconds, report the timeout and reclaim
    // only pages that are actually safe to free.
    size_t quiesce() {
        std::unique_lock<std::mutex> lock(mu_);
        const uint64_t target_epoch = epoch_;
        const bool quiescent = cv_.wait_for(lock, kQuiesceTimeout, [&] {
            for (const auto& page : retired_) {
                if (page.epoch <= target_epoch && !eligible_locked(page.epoch)) return false;
            }
            return true;
        });
        if (!quiescent) {
            std::fprintf(stderr,
                         "[hot reload] quiesce timed out after 30 seconds; "
                         "%zu retired executable page(s) remain\n",
                         retired_.size());
            std::fflush(stderr);
        }
        return reclaim_locked(target_epoch);
    }

    uint64_t epoch() const {
        std::lock_guard<std::mutex> lock(mu_);
        return epoch_;
    }
    size_t retired_page_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return retired_.size();
    }
    uint64_t reclaimed_page_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return reclaimed_pages_;
    }

private:
    static constexpr size_t kMaxRetiredPages = 64;
    static constexpr std::chrono::seconds kQuiesceTimeout {30};

    struct RetiredPage {
        void* entry = nullptr;
        uint64_t epoch = 0;
    };

    uint64_t enter() {
        std::lock_guard<std::mutex> lock(mu_);
        const uint64_t observed = epoch_;
        active_epochs_.insert(observed);
        return observed;
    }

    void leave(uint64_t entry_epoch) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = active_epochs_.find(entry_epoch);
        if (it != active_epochs_.end()) active_epochs_.erase(it);
        cv_.notify_all();
    }

    bool eligible_locked(uint64_t retirement_epoch) const {
        return active_epochs_.empty() || *active_epochs_.begin() >= retirement_epoch;
    }

    size_t reclaim_locked(uint64_t through_epoch) {
        size_t reclaimed = 0;
        auto it = retired_.begin();
        while (it != retired_.end()) {
            if (it->epoch <= through_epoch && eligible_locked(it->epoch)) {
                free_executable(it->entry);
                it = retired_.erase(it);
                ++reclaimed;
                ++reclaimed_pages_;
            } else {
                ++it;
            }
        }
        return reclaimed;
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    uint64_t epoch_ = 0;
    uint64_t reclaimed_pages_ = 0;
    std::multiset<uint64_t> active_epochs_;
    std::vector<RetiredPage> retired_;
};

} // namespace ember
