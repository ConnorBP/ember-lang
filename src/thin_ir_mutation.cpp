// thin_ir_mutation.cpp — implementation of the checked, transactional mutation
// helper. See thin_ir_mutation.hpp for the design + the transaction contract.

#include "thin_ir_mutation.hpp"

#include "thin_ir_ser.hpp"   // IR_MAX_VREGS (hard ceiling)

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace ember {

// ─── Central VReg-bound enumeration ───
// The single source of truth: every VReg referenced anywhere in the function,
// INCLUDING the implicit dst+1 second word of a slice/lambda pair result, call
// args, and terminator cond/ret. Returns max(ir_max, f.declared_max_vreg) so a
// pass that bumped declared_max_vreg without adding a visible reference still
// allocates above it. The serializer uses this for the ir_blob header
// max_vreg; ThinIRMutation uses it as the fresh-VReg starting point.
uint32_t compute_central_max_vreg(const ThinFunction& f) {
    uint32_t max = 1;  // VRegs are 1-indexed; 0 = invalid/none
    auto bump = [&](uint32_t v) { if (v >= max) max = v + 1; };
    for (const auto& blk : f.blocks) {
        for (const auto& in : blk.instrs) {
            bump(in.dst); bump(in.src1); bump(in.src2);
            // Implicit dst+1 for slice/lambda pair results: the second word
            // (len / env_ptr) occupies dst+1 even when no explicit instruction
            // references it. The serializer's old explicit-field-only scanner
            // missed this; the central enumeration does not.
            if (in.dst != 0 && in.meta.type &&
                (in.meta.type->is_slice || in.meta.type->is_lambda))
                bump(in.dst + 1);
            // CopyBytes: dst is a dest-pointer VReg that is READ (a pointer
            // use), not a produced scalar. It still counts toward the VReg
            // bound (it is a referenced VReg), so the generic bump(in.dst)
            // above already covers it.
            for (uint32_t a : in.args) bump(a);
        }
        bump(blk.term.cond);
        bump(blk.term.ret);
    }
    return std::max(max, f.declared_max_vreg);
}

namespace {

// Build the list of existing frame regions (rbp-negative begin, size) the
// allocator must not overlap: the rbx-save slot, the struct-ret-ptr slot, the
// param spill slots, and the already-allocated local slots [frame.frame_size,
// -next_local_off) ... actually the locals span [next_local_off area]. The
// frame occupies [rbp - frame_size, rbp); in offset terms [-frame_size, 0).
// A region is [begin, begin+size) where begin is rbp-negative.
struct Region { int64_t begin; uint64_t size; };

std::vector<Region> existing_frame_regions(const ThinFunction& f) {
    std::vector<Region> r;
    // rbx save: 8 bytes at rbx_save_offset (when nonzero).
    if (f.frame.rbx_save_offset != 0)
        r.push_back({int64_t(f.frame.rbx_save_offset), 8});
    // struct-ret-ptr: 8 bytes (when returns_struct_by_ptr && off != 0).
    if (f.frame.returns_struct_by_ptr && f.frame.struct_ret_ptr_offset != 0)
        r.push_back({int64_t(f.frame.struct_ret_ptr_offset), 8});
    // params: span = max(type-derived, nwords*8), but conservatively 8 or 16.
    for (const auto& p : f.frame.params) {
        if (p.ty == nullptr) continue;  // __struct_ret_ptr sentinel
        if (p.off == 0) continue;
        uint64_t span = (p.ty && p.ty->is_slice) ? 16 : 8;
        uint64_t nwords_span = uint64_t(p.nwords) * 8u;
        if (nwords_span > span) span = nwords_span;
        r.push_back({int64_t(p.off), span});
    }
    // Already-allocated locals: NOT added as barrier regions. The locals
    // grow strictly downward from next_local_off, so a new allocation at
    // -next_off (the next slot) never overlaps an existing local (which
    // occupies [-next_local_off, -<first_local>), i.e. ABOVE the next slot).
    // The staged_regions_ list handles intra-mutation non-overlap. Only the
    // fixed frame-plan regions (rbx save, struct-ret ptr, params) can collide
    // with a new allocation and are returned here.
    //
    // HOWEVER, when arg_temps_base is set, the arg-temps area lives BELOW the
    // locals (within [-frame_size, -next_local_off)). Growing from
    // next_local_off can overlap it. We add the entire below-locals region as
    // a barrier so the non-overlap check catches it. (The constructor also
    // starts next_off_ at frame_size when arg_temps_base is set, so the first
    // allocation is already below the frame — this barrier is the belt-and-
    // suspenders defense for any code path that doesn't rely on that start.)
    if (f.frame.arg_temps_base != 0 && f.frame.frame_size > f.frame.next_local_off) {
        int64_t barrier_begin = -int64_t(f.frame.frame_size);
        int64_t barrier_end = -int64_t(f.frame.next_local_off);
        r.push_back({barrier_begin, uint64_t(barrier_end - barrier_begin)});
    }
    return r;
}

bool overlaps(int64_t a_begin, uint64_t a_size, int64_t b_begin, uint64_t b_size) {
    // Two spans [a_begin, a_begin+a_size) and [b_begin, b_begin+b_size).
    int64_t a_end = a_begin + int64_t(a_size);
    int64_t b_end = b_begin + int64_t(b_size);
    return a_begin < b_end && b_begin < a_end;
}

} // namespace

ThinIRMutation::ThinIRMutation(ThinFunction& f, const PassGrowthLimits& limits)
    : f_(f)
    , limits_(limits)
    , snapshot_(std::make_unique<ThinFunction>(f))
    , next_vreg_(compute_central_max_vreg(f))
    , next_off_(f.frame.next_local_off)
    , next_rodata_off_(static_cast<uint32_t>(f.rodata.size()))
    , initial_frame_size_(f.frame.frame_size) {
    // Count the initial instruction count for growth-ratio accounting.
    for (const auto& blk : f.blocks)
        initial_instructions_ += static_cast<uint32_t>(blk.instrs.size());
    // When the frame has a reserved arg-temp area below the locals, start new
    // allocations below the ENTIRE existing frame so they cannot overlap the
    // arg temps (or any other reserved region below next_local_off).
    if (f.frame.arg_temps_base != 0 && f.frame.frame_size > next_off_) {
        next_off_ = f.frame.frame_size;
    }
}

ThinIRMutation::~ThinIRMutation() {
    if (!committed_ && snapshot_) {
        // Abandoned mutation: restore the construction-time snapshot so the
        // function is unchanged (including any direct block/instr mutations
        // the pass made, and the original frame plan / declared_max_vreg / ra).
        f_ = std::move(*snapshot_);
    }
}

bool ThinIRMutation::region_overlaps_existing(int64_t begin, uint64_t size) const {
    // Existing fixed regions (rbx save, struct-ret-ptr, params).
    auto existing = existing_frame_regions(f_);
    for (const auto& r : existing)
        if (overlaps(begin, size, r.begin, r.size)) return true;
    // Staged regions from earlier in this mutation.
    for (const auto& r : staged_regions_)
        if (overlaps(begin, size, r.begin, r.size)) return true;
    return false;
}

MutationStatus ThinIRMutation::check_frame_bounds(int64_t begin, uint64_t size) const {
    // The new region's low end is begin (rbp-negative); its span is
    // [begin, begin+size). The frame must grow to cover [-end, 0) where
    // end = -(begin) = next_off after allocation. We check the hard ceiling
    // and the configured added-frame-byte budget in the caller; here we check
    // the hard frame-size ceiling.
    int64_t needed_bytes = -begin;  // depth below rbp
    if (needed_bytes > int64_t(THIN_HARD_MAX_FRAME_SIZE))
        return MutationStatus::LimitExceeded;
    return MutationStatus::Ok;
}

void ThinIRMutation::record_region(int64_t begin, uint64_t size) {
    staged_regions_.push_back({begin, size});
}

MutationResult<ScalarAlloc> ThinIRMutation::allocate_scalar(const Type* /*type*/, int32_t width) {
    MutationResult<ScalarAlloc> res;
    if (committed_) {
        res.error = {MutationStatus::NotCommitted, "allocate_scalar after commit"};
        return res;
    }
    (void)width;
    // Enforce the added-VReg limit before reserving (checked: no overflow).
    if (1 > limits_.max_added_vregs ||
        added_vregs_ > limits_.max_added_vregs - 1) {
        res.error = {MutationStatus::LimitExceeded, "max_added_vregs exceeded"};
        return res;
    }
    // Checked VReg arithmetic: next_vreg_ + 1 must not overflow uint32_t or
    // exceed the hard IR_MAX_VREGS ceiling.
    if (next_vreg_ >= IR_MAX_VREGS) {
        res.error = {MutationStatus::LimitExceeded, "hard VReg ceiling exceeded"};
        return res;
    }
    // Enforce the added-frame-byte limit (8 bytes for a scalar slot, checked).
    if (8 > limits_.max_added_frame_bytes ||
        added_frame_bytes_ > limits_.max_added_frame_bytes - 8) {
        res.error = {MutationStatus::LimitExceeded, "max_added_frame_bytes exceeded"};
        return res;
    }
    // Checked frame arithmetic: next_off_ + 8 must not overflow int32. Use
    // int64_t for the intermediate and check before casting back.
    int64_t new_off_64 = int64_t(next_off_) + 8;
    if (new_off_64 > int64_t(INT32_MAX)) {
        res.error = {MutationStatus::LimitExceeded, "frame offset overflow"};
        return res;
    }
    int32_t new_off = int32_t(new_off_64);
    int64_t begin = -int64_t(new_off);
    if (check_frame_bounds(begin, 8) != MutationStatus::Ok) {
        res.error = {MutationStatus::LimitExceeded, "hard frame-size ceiling exceeded"};
        return res;
    }
    if (region_overlaps_existing(begin, 8)) {
        res.error = {MutationStatus::InvalidArgument, "frame region overlap"};
        return res;
    }
    // Reserve.
    VReg v = next_vreg_;
    next_vreg_ += 1;
    next_off_ = new_off;
    added_vregs_ += 1;
    added_frame_bytes_ += 8;
    record_region(begin, 8);
    staged_change_ = true;
    res.value = ScalarAlloc{v, int32_t(begin)};
    return res;
}

MutationResult<PairAlloc> ThinIRMutation::allocate_pair(const Type* /*type*/) {
    MutationResult<PairAlloc> res;
    if (committed_) {
        res.error = {MutationStatus::NotCommitted, "allocate_pair after commit"};
        return res;
    }
    // Two consecutive VRegs + a 16-byte frame slot.
    if (2 > limits_.max_added_vregs ||
        added_vregs_ > limits_.max_added_vregs - 2) {
        res.error = {MutationStatus::LimitExceeded, "max_added_vregs exceeded (pair)"};
        return res;
    }
    // Checked VReg arithmetic: next_vreg_ + 2 must not overflow or exceed the
    // hard ceiling.
    if (next_vreg_ > IR_MAX_VREGS - 2) {
        res.error = {MutationStatus::LimitExceeded, "hard VReg ceiling exceeded (pair)"};
        return res;
    }
    if (16 > limits_.max_added_frame_bytes ||
        added_frame_bytes_ > limits_.max_added_frame_bytes - 16) {
        res.error = {MutationStatus::LimitExceeded, "max_added_frame_bytes exceeded (pair)"};
        return res;
    }
    // Checked frame arithmetic: next_off_ + 16 must not overflow int32.
    int64_t new_off_64 = int64_t(next_off_) + 16;
    if (new_off_64 > int64_t(INT32_MAX)) {
        res.error = {MutationStatus::LimitExceeded, "frame offset overflow (pair)"};
        return res;
    }
    int32_t new_off = int32_t(new_off_64);
    int64_t begin = -int64_t(new_off);
    if (check_frame_bounds(begin, 16) != MutationStatus::Ok) {
        res.error = {MutationStatus::LimitExceeded, "hard frame-size ceiling exceeded (pair)"};
        return res;
    }
    if (region_overlaps_existing(begin, 16)) {
        res.error = {MutationStatus::InvalidArgument, "frame region overlap (pair)"};
        return res;
    }
    VReg lo = next_vreg_;
    VReg hi = lo + 1;
    next_vreg_ += 2;
    next_off_ = new_off;
    added_vregs_ += 2;
    added_frame_bytes_ += 16;
    record_region(begin, 16);
    staged_change_ = true;
    res.value = PairAlloc{lo, hi, int32_t(begin)};
    return res;
}

MutationResult<int32_t> ThinIRMutation::allocate_frame_bytes(uint32_t size, uint32_t alignment) {
    MutationResult<int32_t> res;
    if (committed_) {
        res.error = {MutationStatus::NotCommitted, "allocate_frame_bytes after commit"};
        return res;
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        res.error = {MutationStatus::InvalidArgument, "alignment must be a nonzero power of two"};
        return res;
    }
    // Checked added-frame-byte limit: added_frame_bytes_ + size must not
    // overflow uint32_t.
    if (size > limits_.max_added_frame_bytes ||
        added_frame_bytes_ > limits_.max_added_frame_bytes - size) {
        res.error = {MutationStatus::LimitExceeded, "max_added_frame_bytes exceeded"};
        return res;
    }
    // Align the ALLOCATED REGION's low end (begin) to `alignment`. The region
    // is [begin, begin+size) where begin = -new_off. For begin to be aligned,
    // new_off must be a multiple of alignment. We compute new_off as
    // align_up(next_off + size, alignment) so the region's lowest address
    // (begin = -new_off) is aligned. All arithmetic in int64_t to avoid
    // int32 overflow on attacker-controlled size/next_off.
    int64_t raw_depth = int64_t(next_off_) + int64_t(size);
    if (raw_depth > int64_t(INT32_MAX)) {
        res.error = {MutationStatus::LimitExceeded, "frame offset overflow"};
        return res;
    }
    // Align the total depth UP to `alignment` so begin = -depth is aligned.
    int64_t aligned_depth = raw_depth;
    if (alignment > 1) {
        int64_t mask = int64_t(alignment) - 1;
        if ((aligned_depth & mask) != 0)
            aligned_depth = (aligned_depth + mask) & ~mask;
    }
    if (aligned_depth > int64_t(INT32_MAX)) {
        res.error = {MutationStatus::LimitExceeded, "frame offset overflow (aligned)"};
        return res;
    }
    int32_t new_off = int32_t(aligned_depth);
    int64_t begin = -int64_t(new_off);
    if (check_frame_bounds(begin, size) != MutationStatus::Ok) {
        res.error = {MutationStatus::LimitExceeded, "hard frame-size ceiling exceeded"};
        return res;
    }
    if (region_overlaps_existing(begin, size)) {
        res.error = {MutationStatus::InvalidArgument, "frame region overlap"};
        return res;
    }
    next_off_ = new_off;
    added_frame_bytes_ += size;
    record_region(begin, size);
    staged_change_ = true;
    res.value = int32_t(begin);
    return res;
}

MutationResult<uint32_t> ThinIRMutation::allocate_rodata(uint32_t size) {
    MutationResult<uint32_t> res;
    if (committed_) {
        res.error = {MutationStatus::NotCommitted, "allocate_rodata after commit"};
        return res;
    }
    if (size == 0) {
        res.error = {MutationStatus::InvalidArgument, "allocate_rodata: size must be nonzero"};
        return res;
    }
    // Checked added-rodata-byte limit: added_rodata_bytes_ + size must not
    // overflow uint32_t.
    if (size > limits_.max_added_rodata_bytes ||
        added_rodata_bytes_ > limits_.max_added_rodata_bytes - size) {
        res.error = {MutationStatus::LimitExceeded, "max_added_rodata_bytes exceeded"};
        return res;
    }
    // Checked rodata-offset arithmetic: next_rodata_off_ + size must not
    // overflow uint32_t (rodata addends are uint32_t in the IR).
    uint64_t new_off_64 = uint64_t(next_rodata_off_) + uint64_t(size);
    if (new_off_64 > uint64_t(UINT32_MAX)) {
        res.error = {MutationStatus::LimitExceeded, "rodata offset overflow"};
        return res;
    }
    uint32_t addend = next_rodata_off_;
    next_rodata_off_ = uint32_t(new_off_64);
    added_rodata_bytes_ += size;
    staged_change_ = true;
    res.value = addend;
    return res;
}

MutationResult<uint32_t> ThinIRMutation::split_block(uint32_t block, size_t instruction_index) {
    MutationResult<uint32_t> res;
    if (committed_) {
        res.error = {MutationStatus::NotCommitted, "split_block after commit"};
        return res;
    }
    // Find the block by id.
    size_t bi = f_.blocks.size();
    for (size_t i = 0; i < f_.blocks.size(); ++i)
        if (f_.blocks[i].id == block) { bi = i; break; }
    if (bi == f_.blocks.size()) {
        res.error = {MutationStatus::InvalidArgument, "split_block: no such block id"};
        return res;
    }
    if (instruction_index > f_.blocks[bi].instrs.size()) {
        res.error = {MutationStatus::InvalidArgument, "split_block: instruction_index out of range"};
        return res;
    }
    if (1 > limits_.max_added_blocks ||
        added_blocks_ > limits_.max_added_blocks - 1) {
        res.error = {MutationStatus::LimitExceeded, "max_added_blocks exceeded"};
        return res;
    }
    // Mint a temporary id outside the current id domain.
    uint32_t next_id = 0;
    for (const auto& b : f_.blocks)
        if (b.id >= next_id) next_id = b.id + 1;
    // Also stay above any id we already minted in this mutation (none tracked
    // separately; canonicalize remaps at the end).
    ThinBlock& original = f_.blocks[bi];
    ThinTerm old_term = original.term;
    ThinBlock continuation;
    continuation.id = next_id;
    auto split = original.instrs.begin() + static_cast<ptrdiff_t>(instruction_index);
    continuation.instrs.insert(continuation.instrs.end(),
        std::make_move_iterator(split),
        std::make_move_iterator(original.instrs.end()));
    original.instrs.erase(split, original.instrs.end());
    continuation.term = old_term;
    original.term = {};
    original.term.kind = TermKind::Jmp;
    original.term.target = next_id;
    f_.blocks.insert(f_.blocks.begin() + static_cast<ptrdiff_t>(bi + 1),
                     std::move(continuation));
    added_blocks_ += 1;
    staged_change_ = true;
    res.value = next_id;
    return res;
}

MutationResult<void> ThinIRMutation::redirect_edge(uint32_t from, uint32_t old_to, uint32_t new_to) {
    MutationResult<void> res;
    if (committed_) {
        res.error = {MutationStatus::NotCommitted, "redirect_edge after commit"};
        return res;
    }
    size_t bi = f_.blocks.size();
    for (size_t i = 0; i < f_.blocks.size(); ++i)
        if (f_.blocks[i].id == from) { bi = i; break; }
    if (bi == f_.blocks.size()) {
        res.error = {MutationStatus::InvalidArgument, "redirect_edge: no such source block id"};
        return res;
    }
    ThinTerm& t = f_.blocks[bi].term;
    bool found = false;
    if (t.kind == TermKind::Jmp) {
        if (t.target == old_to) { t.target = new_to; found = true; }
    } else if (t.kind == TermKind::Branch) {
        if (t.target == old_to) { t.target = new_to; found = true; }
        if (t.false_target == old_to) { t.false_target = new_to; found = true; }
    }
    if (!found) {
        res.error = {MutationStatus::InvalidArgument, "redirect_edge: no edge matches old_to"};
        return res;
    }
    staged_change_ = true;
    res.has_value = true;
    return res;
}

MutationResult<void> ThinIRMutation::canonicalize_block_ids() {
    MutationResult<void> res;
    if (committed_) {
        res.error = {MutationStatus::NotCommitted, "canonicalize_block_ids after commit"};
        return res;
    }
    std::vector<uint32_t> old_ids;
    old_ids.reserve(f_.blocks.size());
    for (const auto& b : f_.blocks) old_ids.push_back(b.id);
    auto remap = [&](uint32_t old_id) -> uint32_t {
        for (size_t i = 0; i < old_ids.size(); ++i)
            if (old_ids[i] == old_id) return static_cast<uint32_t>(i);
        return old_id;
    };
    for (size_t i = 0; i < f_.blocks.size(); ++i) {
        ThinBlock& b = f_.blocks[i];
        if (b.term.kind == TermKind::Jmp) {
            b.term.target = remap(b.term.target);
        } else if (b.term.kind == TermKind::Branch) {
            b.term.target = remap(b.term.target);
            b.term.false_target = remap(b.term.false_target);
        }
        // Tier 4: TryCatch's meta.slot is a block-id reference (the catch
        // block whose entry rip the inline setjmp saves). A pass that
        // renumbers blocks MUST remap it too, or the Throw's longjmp lands at
        // the wrong (renumbered) block — a silent miscompile / infinite
        // loop. This is the only block-id reference outside the terminators.
        for (ThinInstr& in : b.instrs) {
            if (in.op == ThinOp::TryCatch && in.meta.slot >= 0)
                in.meta.slot = int32_t(remap(uint32_t(in.meta.slot)));
        }
        b.id = static_cast<uint32_t>(i);
    }
    staged_change_ = true;
    res.has_value = true;
    return res;
}

MutationResult<void> ThinIRMutation::reserve_site(uint32_t vregs, uint32_t frame_bytes,
                                      uint32_t instructions, uint32_t blocks,
                                      uint32_t rodata_bytes) {
    MutationResult<void> res;
    if (committed_) {
        res.error = {MutationStatus::NotCommitted, "reserve_site after commit"};
        return res;
    }
    // Atomic dry-run preflight: check ALL limits for this site's worst-case
    // resource consumption. If any limit would be exceeded, return failure
    // WITHOUT incrementing anything. On success, increment ONLY the site
    // counter; the individual allocate_*/split_block/record_* methods
    // increment the resource counters as they consume.

    // Site count (max_sites).
    if (used_sites_ + 1 > limits_.max_sites) {
        res.error = {MutationStatus::LimitExceeded, "max_sites exceeded"};
        return res;
    }

    // Soft VReg limit (checked: added_vregs_ + vregs must not overflow uint32).
    if (vregs > limits_.max_added_vregs ||
        added_vregs_ > limits_.max_added_vregs - vregs) {
        res.error = {MutationStatus::LimitExceeded, "max_added_vregs exceeded (preflight)"};
        return res;
    }
    // Hard VReg ceiling (checked: next_vreg_ + vregs must not overflow or
    // exceed IR_MAX_VREGS).
    if (vregs > IR_MAX_VREGS || next_vreg_ > IR_MAX_VREGS - vregs) {
        res.error = {MutationStatus::LimitExceeded, "hard VReg ceiling exceeded (preflight)"};
        return res;
    }

    // Soft frame-byte limit (checked: added_frame_bytes_ + frame_bytes).
    if (frame_bytes > limits_.max_added_frame_bytes ||
        added_frame_bytes_ > limits_.max_added_frame_bytes - frame_bytes) {
        res.error = {MutationStatus::LimitExceeded, "max_added_frame_bytes exceeded (preflight)"};
        return res;
    }
    // Hard frame-size ceiling (checked: next_off_ + frame_bytes must not
    // overflow int32 or exceed THIN_HARD_MAX_FRAME_SIZE).
    int64_t new_depth = int64_t(next_off_) + int64_t(frame_bytes);
    if (new_depth > int64_t(INT32_MAX)) {
        res.error = {MutationStatus::LimitExceeded, "frame offset overflow (preflight)"};
        return res;
    }
    if (new_depth > int64_t(THIN_HARD_MAX_FRAME_SIZE)) {
        res.error = {MutationStatus::LimitExceeded, "hard frame-size ceiling exceeded (preflight)"};
        return res;
    }

    // Soft instruction limit (checked: added_instructions_ + instructions).
    if (instructions > limits_.max_added_instructions ||
        added_instructions_ > limits_.max_added_instructions - instructions) {
        res.error = {MutationStatus::LimitExceeded, "max_added_instructions exceeded (preflight)"};
        return res;
    }

    // Soft block limit (checked: added_blocks_ + blocks).
    if (blocks > limits_.max_added_blocks ||
        added_blocks_ > limits_.max_added_blocks - blocks) {
        res.error = {MutationStatus::LimitExceeded, "max_added_blocks exceeded (preflight)"};
        return res;
    }

    // Soft rodata-byte limit (checked: added_rodata_bytes_ + rodata_bytes).
    if (rodata_bytes > limits_.max_added_rodata_bytes ||
        added_rodata_bytes_ > limits_.max_added_rodata_bytes - rodata_bytes) {
        res.error = {MutationStatus::LimitExceeded, "max_added_rodata_bytes exceeded (preflight)"};
        return res;
    }
    // Checked rodata-offset arithmetic: next_rodata_off_ + rodata_bytes must
    // not overflow uint32_t.
    if (uint64_t(next_rodata_off_) + uint64_t(rodata_bytes) > uint64_t(UINT32_MAX)) {
        res.error = {MutationStatus::LimitExceeded, "rodata offset overflow (preflight)"};
        return res;
    }

    // Growth ratio: final_instructions <= max(1, initial) * num / denom.
    // Using checked uint64 arithmetic. denom == 0 is an invalid option
    // (registration/build failure per §6.1); treat it as no ratio bound here
    // (the registry rejects it at registration time).
    if (limits_.growth_denominator != 0) {
        uint64_t base = initial_instructions_ != 0 ? uint64_t(initial_instructions_) : 1ULL;
        uint64_t ratio_bound = base * uint64_t(limits_.growth_numerator) /
                               uint64_t(limits_.growth_denominator);
        uint64_t final_count = uint64_t(initial_instructions_) +
                               uint64_t(added_instructions_) + uint64_t(instructions);
        if (final_count > ratio_bound) {
            res.error = {MutationStatus::LimitExceeded, "growth ratio exceeded (preflight)"};
            return res;
        }
    }

    // All checks passed: increment ONLY the site counter. The resource
    // counters are incremented by the individual consume methods.
    used_sites_ += 1;
    res.has_value = true;
    return res;
}

MutationResult<void> ThinIRMutation::record_added_instructions(uint32_t count) {
    MutationResult<void> res;
    if (committed_) {
        res.error = {MutationStatus::NotCommitted, "record_added_instructions after commit"};
        return res;
    }
    // Checked: added_instructions_ + count must not overflow or exceed the
    // soft limit.
    if (count > limits_.max_added_instructions ||
        added_instructions_ > limits_.max_added_instructions - count) {
        res.error = {MutationStatus::LimitExceeded, "max_added_instructions exceeded"};
        return res;
    }
    added_instructions_ += count;
    res.has_value = true;
    return res;
}

MutationResult<void> ThinIRMutation::commit() {
    MutationResult<void> res;
    if (committed_) {
        res.has_value = true;
        return res;
    }
    committed_ = true;
    if (!staged_change_) {
        // No-op mutation: discard the snapshot, leave the function as-is.
        snapshot_.reset();
        res.has_value = true;
        return res;
    }
    // Publish the staged frame + VReg bookkeeping.
    f_.frame.next_local_off = next_off_;
    // Update declared_max_vreg without overflow (guarded by the IR_MAX_VREGS
    // check at allocation time, so next_vreg_ <= IR_MAX_VREGS here).
    f_.declared_max_vreg = next_vreg_;
    // Align frame_size to 16, growing only. The needed depth is next_off_ + a
    // small reserve for the rbx save / alignment; match the lowerer's
    // conservative +16 reserve.
    int32_t needed = next_off_ + 16;
    if (needed > f_.frame.frame_size) {
        int32_t aligned = (needed + 15) & ~15;
        if (aligned > THIN_HARD_MAX_FRAME_SIZE) {
            res.error = {MutationStatus::LimitExceeded, "frame_size exceeds hard ceiling on commit"};
            // Restore the snapshot (the commit failed).
            f_ = std::move(*snapshot_);
            committed_ = false;
            return res;
        }
        f_.frame.frame_size = aligned;
    }
    // Clear stale regalloc: the staged change touched VRegs / frame layout,
    // so an out-of-band pre-pass register allocation cannot be reused safely.
    f_.ra = {};
    // Discard the snapshot (no rollback after a successful commit).
    snapshot_.reset();
    res.has_value = true;
    return res;
}

} // namespace ember
