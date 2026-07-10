// ember single-function hot reload (HOT_RELOAD.md §3/§5).
//
// A HotReloadDomain serializes guard entry with publication, assigns one
// monotonic epoch to every successful slot publication, and owns replaced
// executable pages until they are safe to free. Every OUTER host-to-script
// invocation using the associated dispatch table must hold an ExecutionGuard
// from before loading the slot through return from JIT code. Script-to-script
// and recursive calls are covered by that outer guard. Loading/caching a raw
// entry outside a guard is unsupported.
//
// Recompilation is still per-function: parse, sema, codegen, and finalize all
// complete before HotReloadDomain::publish is called. A failure in any of those
// phases neither touches the dispatch table nor advances the domain epoch.
#pragma once
#include "ast.hpp"           // Program, FuncDecl
#include "codegen.hpp"       // compile_func, CodeGenCtx, CompiledFn
#include "engine.hpp"        // finalize, free_executable
#include "sema.hpp"          // NativeSig, OpOverloadTable, StructLayoutTable
#include "dispatch_table.hpp"// DispatchTable
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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

    private:
        void reset() {
            if (domain_) {
                domain_->leave(entry_epoch_);
                domain_ = nullptr;
            }
        }

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
    // this domain. On failure (null/same entry, epoch exhaustion, or inability
    // to record retirement), the table and epoch remain unchanged and the
    // caller still owns new_entry.
    PublicationInfo publish(DispatchTable& table, size_t slot, void* new_entry) {
        PublicationInfo info;
        if (!new_entry || slot >= table.slots.size()) return info;

        std::lock_guard<std::mutex> lock(mu_);
        void* old_entry = table.get(slot);
        if (old_entry == new_entry || epoch_ == std::numeric_limits<uint64_t>::max()) return info;

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
            try {
                retired_.push_back({old_entry, next_epoch});
            } catch (...) {
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
    // guard this completes immediately after publication.
    size_t quiesce() {
        std::unique_lock<std::mutex> lock(mu_);
        const uint64_t target_epoch = epoch_;
        cv_.wait(lock, [&] {
            for (const auto& page : retired_) {
                if (page.epoch <= target_epoch && !eligible_locked(page.epoch)) return false;
            }
            return true;
        });
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

// Result of a reload attempt. On success new_fn/current entry remains owned by
// the host while it is current. The replaced page has already transferred to
// `domain`; no owning old-entry pointer is returned, preventing caller/domain
// double-free. A later successful publication transfers new_fn.exec to the
// domain in the same way.
struct ReloadResult {
    bool ok = false;
    std::string error;
    uint64_t publication_epoch = 0;
    uint64_t retirement_epoch = 0;
    bool old_page_retired = false;
    CompiledFn new_fn{};
};

// Reload one existing function from a COMPLETE replacement declaration.
// Slot indices and the canonical call signature cannot change. The caller must
// use `domain.guard()` around every outer invocation sharing `table`.
inline ReloadResult reload_function(const std::string& new_fn_source,
                                    Program& prog,
                                    DispatchTable& table,
                                    HotReloadDomain& domain,
                                    const CodeGenCtx& ctx,
                                    const std::unordered_map<std::string, NativeSig>& natives,
                                    const OpOverloadTable* overloads,
                                    const StructLayoutTable* structs) {
    ReloadResult r;
    auto lr = tokenize(new_fn_source, "<reload>");
    if (!lr.ok) { r.error = "reload lex: " + lr.error; return r; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { r.error = "reload parse: " + pr.error; return r; }
    if (pr.program.funcs.size() != 1) { r.error = "reload: expected exactly one function"; return r; }
    auto& new_fn = pr.program.funcs[0];

    auto it = std::find_if(prog.funcs.begin(), prog.funcs.end(),
                           [&](const FuncDecl& f){ return f.name == new_fn.name; });
    if (it == prog.funcs.end()) { r.error = "reload: function '" + new_fn.name + "' not in module"; return r; }
    int slot = it->slot;
    if (slot < 0) { r.error = "reload: slot not assigned for '" + new_fn.name + "'"; return r; }

    auto words = [&](const Type& t) -> int {
        if (t.is_slice) return 2;
        if (!t.struct_name.empty() && structs) {
            auto si = structs->find(t.struct_name);
            if (si != structs->end()) return (si->second.size + 7) / 8;
        }
        return 1;
    };
    auto mismatch = [&](const std::string& what) {
        r.error = "reload: incompatible signature for '" + new_fn.name + "': " + what;
    };
    if (new_fn.params.size() != it->params.size()) {
        mismatch("arity changed from " + std::to_string(it->params.size()) + " to " +
                 std::to_string(new_fn.params.size()));
        return r;
    }
    for (size_t i = 0; i < it->params.size(); ++i) {
        const Type& old_ty = *it->params[i].ty;
        const Type& new_ty = *new_fn.params[i].ty;
        if (!old_ty.same(new_ty) || words(old_ty) != words(new_ty)) {
            mismatch("parameter " + std::to_string(i + 1) + " changed from " +
                     old_ty.to_string() + " to " + new_ty.to_string());
            return r;
        }
    }
    if (!it->ret->same(*new_fn.ret) || words(*it->ret) != words(*new_fn.ret)) {
        mismatch("return type changed from " + it->ret->to_string() + " to " +
                 new_fn.ret->to_string());
        return r;
    }
    new_fn.slot = slot;

    // Program is non-copyable. Temporarily install the replacement so whole-
    // module sema can resolve calls, restoring the old declaration on failure.
    FuncDecl old_fn = std::move(*it);
    *it = std::move(new_fn);
    std::unordered_map<std::string, int> reload_slots;
    for (const auto& f : prog.funcs) reload_slots[f.name] = f.slot;
    auto sr = sema(prog, natives, reload_slots, 0, overloads, structs ? structs : nullptr);
    if (!sr.ok) {
        std::string e = "reload sema: ";
        for (auto& err : sr.errors) e += "line " + std::to_string(err.line) + ": " + err.msg + "; ";
        r.error = e;
        *it = std::move(old_fn);
        return r;
    }

    CompiledFn cf = compile_func(*it, ctx);
    if (!finalize(cf)) {
        r.error = "reload: alloc_executable failed";
        *it = std::move(old_fn);
        return r;
    }

    // This is the only publication point. Recording retirement is prepared
    // before the release store; a publication failure leaves epoch/table intact.
    auto publication = domain.publish(table, size_t(slot), cf.entry);
    if (!publication.ok) {
        free_executable(cf.exec);
        cf.exec = nullptr;
        cf.entry = nullptr;
        r.error = "reload: publication/retirement failed";
        *it = std::move(old_fn);
        return r;
    }

    r.publication_epoch = publication.publication_epoch;
    r.retirement_epoch = publication.retirement_epoch;
    r.old_page_retired = publication.old_page_retired;
    r.new_fn = std::move(cf);
    r.ok = true;
    return r;
}

} // namespace ember
