// thin_ir_mutation.hpp — checked, transactional IR mutation utilities.
//
// This is the shared authoring helper for production and example passes. It
// centralizes the VReg / frame / CFG / regalloc bookkeeping that the existing
// obfuscation passes previously hand-rolled, and it does so transactionally:
// a pass stages a set of changes through ThinIRMutation, and the original
// ThinFunction is left UNCHANGED on failure or when the mutation is abandoned
// (destroyed without committing). Only a successful `commit()` publishes the
// staged growth to the function.
//
// Design ref: docs/planning/plan_POLYMORPHIC_CODE_ENGINE.md §6.1.
//
// The helper owns:
//   - central VReg enumeration, including implicit dst+1 slice/lambda pair
//     results, call args, and terminator cond/ret (the same enumeration the
//     serializer uses for the ir_blob header max_vreg — see
//     compute_central_max_vreg);
//   - checked declared_max_vreg updates without overflow;
//   - negative frame-offset allocation with non-overlap checks against the
//     function's existing frame regions (params, rbx save, struct-ret ptr,
//     already-allocated local slots) and against regions staged earlier in
//     this same mutation;
//   - frame_size alignment (16-byte) and max-span checks against the
//     configured + hard bounds;
//   - block splitting, edge redirection, and block-ID canonicalization with
//     target remapping;
//   - deterministic growth accounting against PassGrowthLimits (added VRegs,
//     added blocks, added frame bytes, added instructions);
//   - stale-regalloc clearing on a committed relevant change.
//
// Semantics (fixed by §6.1):
//   - max_sites and per-pass added-resource caps are SOFT success ceilings:
//     a failed allocation returns a failure Result and stages nothing; the
//     pass stops before the next site;
//   - every allocation preflights its limit BEFORE reserving the resource;
//   - the original function is left unchanged on failure or abandoned
//     mutation (the destructor restores the construction-time snapshot);
//   - commit() publishes the staged frame/vreg bookkeeping and clears stale
//     `f.ra` when a relevant change was staged.

#pragma once

#include "thin_ir.hpp"          // ThinFunction, ThinBlock, ThinTerm, VReg, Type

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ember {

// ─── Growth limits (§4.2) ───
// Soft per-pass ceilings. A failed allocation returns a failure Result and
// stages nothing; the pass reports completed-with-truncation. The hard
// ceilings (IR_MAX_VREGS etc. from thin_ir_ser.hpp + a 1MB frame cap matching
// the validator) are always enforced on top of these.
struct PassGrowthLimits {
    uint32_t max_sites = 64;
    uint32_t max_added_instructions = 4096;
    uint32_t max_added_blocks = 256;
    uint32_t max_added_vregs = 8192;
    uint32_t max_added_frame_bytes = 64 * 1024;
    uint32_t max_added_rodata_bytes = 4 * 1024 * 1024;
    uint32_t growth_numerator = 3;
    uint32_t growth_denominator = 1;
};

// Hard frame-size ceiling (matches validate_thin_function's 1MB cap).
constexpr int32_t THIN_HARD_MAX_FRAME_SIZE = (1 << 20);

// ─── Typed mutation result/status ───
// Every mutating operation returns a Result<T>. A failure carries a short
// diagnostic string and stages nothing. The pass may stop before the next
// site (a soft ceiling) or report the failure up to checked execution.

enum class MutationStatus : uint8_t {
    Ok,                 // operation succeeded
    LimitExceeded,      // a configured/hard growth ceiling was hit
    InvalidArgument,    // the requested operation is malformed (bad block id,
                        // bad split index, bad alignment, overflow)
    NotCommitted,       // operation attempted after the mutation was abandoned
};

struct MutationError {
    MutationStatus status = MutationStatus::Ok;
    std::string message;
};

template <typename T>
struct MutationResult {
    std::optional<T> value;            // present on success
    MutationError error;               // present on failure
    bool ok() const { return value.has_value(); }
    explicit operator bool() const { return ok(); }
    const T& get() const { return *value; }
    T& get() { return *value; }
};

// Void specialization: no value payload, just ok()/error. Used by the CFG
// operations (redirect_edge, canonicalize_block_ids, commit) that report only
// success/failure.
template <>
struct MutationResult<void> {
    bool has_value = false;            // true on success
    MutationError error;               // present on failure
    bool ok() const { return has_value; }
    explicit operator bool() const { return ok(); }
    void get() const {}
};

// A typed alias for the scalar allocation result: the fresh VReg + its
// rbp-negative frame spill slot.
struct ScalarAlloc {
    VReg vreg = 0;
    int32_t frame_off = 0;
};

// A typed alias for the pair allocation result: two consecutive VRegs (ptr at
// v, len at v+1) + one 16-byte frame slot. Used for slice/lambda results.
struct PairAlloc {
    VReg vreg_lo = 0;     // ptr / fn_slot
    VReg vreg_hi = 0;     // len / env_ptr  (== vreg_lo + 1)
    int32_t frame_off = 0;
};

// ─── Central VReg-bound enumeration ───
// The highest VReg+1 referenced anywhere in the function, INCLUDING the
// implicit dst+1 second word of a slice/lambda pair result, call args, and
// terminator cond/ret. This is the single source of truth used by
// ThinIRMutation (fresh-VReg allocation starts above this) and by the
// serializer (the ir_blob header max_vreg). Returns max(ir_max,
// f.declared_max_vreg) so a pass that bumped declared_max_vreg without adding
// a visible reference still allocates above it.
uint32_t compute_central_max_vreg(const ThinFunction& f);

// ─── ThinIRMutation ───
// Construct with a reference to the function to mutate + the growth limits.
// The constructor snapshots the function (deep copy) so a later abandon or
// failure can restore it. Call the allocation / CFG methods to stage changes;
// call commit() to publish the staged frame/vreg bookkeeping. If the object
// is destroyed without commit(), the snapshot is restored (the function is
// unchanged).
//
// Allocation methods (allocate_scalar / allocate_pair / allocate_frame_bytes)
// do NOT mutate the function's frame plan immediately — they return the
// would-be VReg / frame offset and record the staged growth internally. The
// pass uses the returned values to build instructions and insert them into the
// function's blocks (those insertions are direct mutations; the snapshot
// restore on abandon undoes them). commit() writes frame.next_local_off,
// frame.frame_size (aligned), declared_max_vreg, and clears stale `f.ra`.
//
// CFG methods (split_block / redirect_edge / canonicalize_block_ids) mutate
// the function's blocks directly so the pass can insert instructions into the
// split blocks; the snapshot restore on abandon undoes them. They enforce the
// added-block / added-instruction limits before mutating.
class ThinIRMutation {
public:
    // Construct a transactional mutation. Snapshots `f` for rollback.
    ThinIRMutation(ThinFunction& f, const PassGrowthLimits& limits);
    ~ThinIRMutation();

    ThinIRMutation(const ThinIRMutation&) = delete;
    ThinIRMutation& operator=(const ThinIRMutation&) = delete;
    ThinIRMutation(ThinIRMutation&&) = delete;
    ThinIRMutation& operator=(ThinIRMutation&&) = delete;

    // Allocate one scalar VReg + an 8-byte rbp-negative frame spill slot.
    // `width` is the operand byte width (1/2/4/8) recorded on the spill; the
    // slot is always 8 bytes (the backend's full qword cell). Pre-checks the
    // added-VReg and added-frame-byte limits and the non-overlap against
    // existing + staged frame regions.
    MutationResult<ScalarAlloc> allocate_scalar(const Type* type, int32_t width);

    // Allocate a consecutive two-word pair (slice {ptr,len} or lambda
    // {fn_slot,env_ptr}) + a 16-byte rbp-negative frame slot. The two VRegs
    // are consecutive (hi == lo + 1). Pre-checks the added-VReg (2) and
    // added-frame-byte (16) limits and non-overlap.
    MutationResult<PairAlloc> allocate_pair(const Type* type);

    // Allocate `size` bytes at a negative rbp offset with `alignment` (1/2/4/
    // 8/16). Pre-checks the added-frame-byte limit, non-overlap, and the hard
    // frame-size ceiling. Returns the rbp-negative begin offset. Used for
    // StringDecrypt buffers, dead-code injection storage, etc.
    MutationResult<int32_t> allocate_frame_bytes(uint32_t size, uint32_t alignment);

    // Allocate `size` bytes of rodata and return the addend (byte offset into
    // the function's rodata blob). Pre-checks the added-rodata-byte limit and
    // the hard rodata-size ceiling. The caller writes the bytes into
    // f.rodata at the returned offset AFTER a successful commit (the
    // allocation only reserves the offset + tracks the growth). Used by
    // str_encrypt's atomic rodata rebuild when overlapping references
    // require different keys. Returns InvalidArgument if size == 0.
    MutationResult<uint32_t> allocate_rodata(uint32_t size);

    // Split `block` at `instruction_index` (the suffix starting at that index
    // moves to a new continuation block; the original keeps [0, index)).
    // `instruction_index` must be in [0, block.instrs.size()]. The new block
    // is appended after `block` in the block vector with a temporary id, and
    // the original's terminator becomes Jmp -> new block. Returns the new
    // block id (a temporary id valid until canonicalize_block_ids()). Pre-
    // checks the added-block limit. On failure stages nothing.
    MutationResult<uint32_t> split_block(uint32_t block, size_t instruction_index);

    // Redirect a CFG edge: in block `from`, change a terminator target that
    // currently equals `old_to` to `new_to`. Applies to Jmp.target and
    // Branch.target / Branch.false_target. Returns InvalidArgument if `from`
    // has no edge currently equal to `old_to`.
    MutationResult<void> redirect_edge(uint32_t from, uint32_t old_to, uint32_t new_to);

    // Renumber every block.id to its vector index and remap every Jmp/Branch
    // target to the new id. Entry (blocks[0]) becomes id 0. This is the
    // canonicalization the emitter's block_labels indexing contract requires.
    MutationResult<void> canonicalize_block_ids();

    // Publish the staged growth to the function: set frame.next_local_off,
    // align frame.frame_size to 16 (growing only), set declared_max_vreg to
    // the staged next vreg, and clear stale `f.ra` if a relevant change was
    // staged. After commit the destructor performs NO rollback. Returns Ok on
    // success. Calling commit() twice is a no-op second call (returns Ok).
    MutationResult<void> commit();

    // Atomic per-site preflight (§6.1): check that one site's worst-case
    // resource consumption (vregs, frame_bytes, instructions, blocks, rodata)
    // fits within ALL configured soft limits + hard ceilings + the growth
    // ratio, WITHOUT staging anything. On success, increments only the site
    // counter (used_sites_); the individual allocate_* / split_block /
    // record_* methods increment the resource counters as they consume. On
    // failure, increments NOTHING and returns LimitExceeded. A pass calls this
    // BEFORE mutating a site so a mid-site limit failure cannot leave an
    // orphan partial allocation (if preflight passes, the subsequent allocate
    // calls succeed).
    MutationResult<void> reserve_site(uint32_t vregs, uint32_t frame_bytes,
                                      uint32_t instructions, uint32_t blocks,
                                      uint32_t rodata_bytes = 0);

    // Record `count` instructions added directly by the pass (not via an
    // allocate method). Used after inserting instructions into blocks so the
    // added-instruction + growth-ratio accounting stays accurate. Returns
    // LimitExceeded if the count would exceed max_added_instructions.
    MutationResult<void> record_added_instructions(uint32_t count);

    // True if commit() has been called.
    bool committed() const { return committed_; }

    // True if at least one allocation / CFG op succeeded (a change was staged).
    bool staged_any_change() const { return staged_change_; }

    // Access to the staged next VReg / next frame offset (for inspection by
    // tests; passes use the allocation methods instead).
    uint32_t staged_next_vreg() const { return next_vreg_; }
    int32_t staged_next_local_off() const { return next_off_; }

private:
    ThinFunction& f_;
    PassGrowthLimits limits_;
    // Snapshot for rollback (deep copy made at construction, discarded on
    // commit, restored on abandon).
    std::unique_ptr<ThinFunction> snapshot_;
    bool committed_ = false;
    bool staged_change_ = false;
    // Staged growth counters.
    uint32_t added_vregs_ = 0;
    uint32_t added_blocks_ = 0;
    uint32_t added_instructions_ = 0;
    uint32_t added_frame_bytes_ = 0;
    uint32_t added_rodata_bytes_ = 0;
    uint32_t used_sites_ = 0;
    uint32_t initial_instructions_ = 0;
    // Staged next VReg / next frame offset / next rodata addend.
    uint32_t next_vreg_ = 1;
    int32_t next_off_ = 0;
    uint32_t next_rodata_off_ = 0;
    int32_t initial_frame_size_ = 0;
    // Staged frame regions (rbp-negative begin, size) for non-overlap checks.
    struct Region { int64_t begin; uint64_t size; };
    std::vector<Region> staged_regions_;

    // Helpers.
    bool region_overlaps_existing(int64_t begin, uint64_t size) const;
    MutationStatus check_frame_bounds(int64_t begin, uint64_t size) const;
    void record_region(int64_t begin, uint64_t size);
};

} // namespace ember
