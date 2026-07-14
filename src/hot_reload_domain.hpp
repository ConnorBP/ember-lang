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
#include <functional>            // std::function (publish_keyed_slot ownership predicate)
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

    // ─── Red 10 (§12.2, §10.2, §12.4): keyed physical-slot publication ────
    // Publish a finalized page to ONE atomic<void*> physical slot of a keyed
    // module's dispatch storage (RecordBuilderStorage::storage / a
    // KeyedDispatchStorage backing array), transferring ownership of the
    // replaced page to this domain. This is the keyed analogue of publish()
    // above: it serializes publication + retirement atomically with guard
    // enrollment by holding mu_ THROUGH the release-store, so there is no
    // window where a guard can enter between retirement and publication and
    // execute an old page without pinning it (the §12.4 invariant the keyed
    // reload must honor).
    //
    // `physical_slots` is the keyed physical storage array (size
    // `physical_slot_count`); `physical_slot` is the keyed domain slot
    // P(K, domain, ordinal) the reload selected (NOT the raw logical index).
    // `is_owned_old_page` is a caller-supplied predicate that PROVES the
    // acquire-loaded old entry is an OWNED executable page this domain may
    // retire + later free. The static padding trap target and any non-owned
    // entry MUST return false from the predicate — they are NEVER retired or
    // freed (§7.3, §10.4: the padding target is a single shared static stub;
    // a non-owned entry belongs to another publication/slot). Only when the
    // predicate returns true is the old page recorded into retired_ for
    // reclamation after guards drain. The caller retains ownership of
    // new_entry on a failure return; on success the caller's new_entry is
    // published (the caller keeps the owning CompiledFn for the new page; the
    // domain owns the OLD page once retired).
    //
    // On failure (null new_entry, out-of-range slot, same entry, epoch
    // exhaustion, retired-page cap, or inability to record retirement), the
    // storage slot, the epoch, and retired_ remain UNCHANGED and the caller
    // still owns new_entry — no partial state is observable.
    PublicationInfo publish_keyed_slot(std::atomic<void*>* physical_slots,
                                       size_t physical_slot_count,
                                       size_t physical_slot,
                                       void* new_entry,
                                       const std::function<bool(void* old_entry)>& is_owned_old_page) {
        PublicationInfo info;
        if (!new_entry || !physical_slots || physical_slot >= physical_slot_count) {
            info.error = "invalid entry or keyed physical slot";
            return info;
        }

        std::lock_guard<std::mutex> lock(mu_);
        void* old_entry = physical_slots[physical_slot].load(std::memory_order_acquire);
        if (old_entry == new_entry) {
            info.error = "keyed entry is already published";
            return info;
        }
        if (epoch_ == std::numeric_limits<uint64_t>::max()) {
            info.error = "publication epoch exhausted";
            return info;
        }

        // Only retire an OWNED executable page the caller proved it owns. The
        // static padding trap target and any non-owned entry are NOT retired
        // (the predicate returns false): freeing them would free a shared
        // static stub or another publication's page. An old entry the
        // predicate rejects is left in place conceptually (it is overwritten
        // by the release-store below, but the domain takes no ownership of
        // it, so reclaim never frees it — the entity that owns it remains
        // responsible). This is the §10.4 / Red 10 ownership proof: every
        // retired page is a real JIT-compiled page the reload actually
        // replaced, never the padding target and never a non-owned entry.
        const bool retire_old = old_entry && is_owned_old_page && is_owned_old_page(old_entry);

        // An owned executable page must be uniquely bound to one (domain,
        // storage, slot) for retirement ownership to be sound. If the old
        // entry still appears in another physical slot of this storage,
        // retiring it here would let reclaim free a page that the other slot
        // still publishes. Scan every other slot before retiring: if any still
        // holds old_entry, do NOT retire it (the page stays current via that
        // other slot and is retired only when its last publishing slot is
        // replaced).
        bool old_still_aliased = false;
        if (retire_old) {
            for (size_t i = 0; i < physical_slot_count; ++i) {
                if (i == physical_slot) continue;
                if (physical_slots[i].load(std::memory_order_acquire) == old_entry) {
                    old_still_aliased = true;
                    break;
                }
            }
        }
        const bool will_retire = retire_old && !old_still_aliased;

        const uint64_t next_epoch = epoch_ + 1;
        if (will_retire) {
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
        // and records the new epoch. The acquire-load above pairs with this
        // release-store, so a reader that acquire-loads the slot sees either
        // the old entry (pinned by a guard that entered before the store) or
        // the new entry (a guard entering after sees the new epoch). There is
        // no window between retirement and publication.
        epoch_ = next_epoch;
        physical_slots[physical_slot].store(new_entry, std::memory_order_release);

        info.ok = true;
        info.publication_epoch = next_epoch;
        info.old_page_retired = will_retire;
        info.retirement_epoch = will_retire ? next_epoch : 0;
        cv_.notify_all();
        return info;
    }

    // ─── Red 10 (§12.3, §10.2, §12.4): keyed whole-generation publication ──
    // Atomically retire the OLD generation's owned executable pages, advance
    // the epoch, and release-publish the NEW generation's record — all under
    // mu_, serialized with guard enrollment. This is the keyed analogue of
    // publish_keyed_slot for a WHOLE generation swap: a guard is either
    // recorded at the old epoch before the registry store, or can enter only
    // after the store completed (the lock was held THROUGH the store) and
    // records the new epoch. There is NO window where a guard can enter
    // between old-page retirement and the new-record publication, acquire the
    // OLD record, and execute a retired page that concurrent reclamation may
    // free (the §12.4 invariant).
    //
    // `enumerate_old_real_pages` is invoked UNDER mu_ to collect the old
    // generation's real (non-padding) executable entry pointers the domain
    // should retire (the caller captures the old record and reads its
    // physical_slots under the lock). `is_owned_old_page` PROVES each entry is
    // an OWNED executable page this domain may retire + later free; the static
    // padding trap target and any non-owned entry MUST return false — they are
    // NEVER retired or freed (§7.3, §10.4). `do_registry_release_store` is the
    // caller's callback that performs the registry's single release-store of
    // the new record pointer (e.g. registry->publish_dispatch_record(id, rec));
    // it is invoked UNDER mu_ AFTER retirement + epoch advance, so the store
    // is atomic with guard enrollment. The domain stays decoupled from the
    // registry / ModuleDispatchRecord (it only calls void() callbacks).
    //
    // On failure (null callbacks, retired-page cap exceeded even after a
    // reclaim, or inability to record retirement), NOTHING is retired, the
    // epoch is UNCHANGED, and do_registry_release_store is NOT invoked — the
    // registry pointer is unchanged and the caller still owns the new record
    // + the old pages remain current. No partial state is observable and no
    // false success is reported: retirement failure does NOT publish.
    PublicationInfo publish_keyed_generation(
        const std::function<std::vector<void*>()>& enumerate_old_real_pages,
        const std::function<bool(void* old_entry)>& is_owned_old_page,
        const std::function<void()>& do_registry_release_store) {
        PublicationInfo info;
        if (!enumerate_old_real_pages || !do_registry_release_store) {
            info.error = "keyed gen-publish: null callback";
            return info;
        }

        std::lock_guard<std::mutex> lock(mu_);

        if (epoch_ == std::numeric_limits<uint64_t>::max()) {
            info.error = "publication epoch exhausted";
            return info;
        }

        // Collect the old generation's real pages UNDER the lock (the caller's
        // callback reads the old record's physical_slots here, so the read is
        // serialized with any concurrent publication on this domain).
        std::vector<void*> old_pages = enumerate_old_real_pages();

        // Filter to OWNED, non-padding, non-null pages the domain may retire.
        // An executable page must be uniquely bound to one (domain, storage,
        // slot); duplicate entries (the same page in two slots) are retired
        // once. The static padding trap target and non-owned entries are NEVER
        // retired (the predicate returns false) — freeing them would free a
        // shared static stub or another publication's page.
        std::vector<void*> to_retire;
        to_retire.reserve(old_pages.size());
        for (void* p : old_pages) {
            if (!p) continue;
            if (is_owned_old_page && !is_owned_old_page(p)) continue;
            bool dup = false;
            for (void* q : to_retire) { if (q == p) { dup = true; break; } }
            if (!dup) to_retire.push_back(p);
        }

        const uint64_t next_epoch = epoch_ + 1;

        // Enforce the hard retired-page cap. Reclaim everything currently
        // eligible first; if a hung guard pins all old pages and the cap would
        // still be exceeded, REJECT this publication rather than publishing
        // without retirement (which would leave old pages untracked) or
        // unsafely freeing code still in execution. No mutation has occurred
        // yet (retired_ untouched, epoch unchanged), so a reject leaves the
        // domain byte-for-byte unchanged.
        if (!to_retire.empty()) {
            if (retired_.size() + to_retire.size() > kMaxRetiredPages) {
                reclaim_locked(epoch_);
            }
            if (retired_.size() + to_retire.size() > kMaxRetiredPages) {
                info.error = "retired executable-page cap reached";
                return info;
            }
        }

        // Record all old pages at next_epoch. If the push throws (OOM), roll
        // back every entry we added so retired_ is byte-for-byte unchanged and
        // we do NOT advance the epoch or publish — no partial state, no false
        // success.
        const size_t retired_before = retired_.size();
        try {
            for (void* p : to_retire) retired_.push_back({p, next_epoch});
        } catch (...) {
            retired_.resize(retired_before);
            info.error = "unable to record retired executable pages";
            return info;
        }

        // Advance the epoch, then release-publish the new record UNDER the
        // lock. Guard enrollment is serialized by mu_: a guard is either
        // recorded at the old epoch before this store, or can enter only after
        // the store completed (the lock was held through the store) and
        // records the new epoch. A reader that acquire-loads the registry
        // pointer sees either the old generation (pinned by a guard that
        // entered before the store) or the new generation (a guard entering
        // after sees the new epoch). There is no window between retirement and
        // publication in which a guard could enter, acquire the old record,
        // and execute a page that reclamation may free.
        epoch_ = next_epoch;
        do_registry_release_store();

        info.ok = true;
        info.publication_epoch = next_epoch;
        info.old_page_retired = !to_retire.empty();
        info.retirement_epoch = !to_retire.empty() ? next_epoch : 0;
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
