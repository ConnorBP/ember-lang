// regalloc.cpp — Stage 3: SSA-lite linear-scan register allocation.
//
// See regalloc.hpp for the contract + design. This file implements:
//   1. Live-interval computation: flatten the blocks into a linear instruction
//      sequence; for each register-candidate VReg (scalar int/bool), compute
//      [first_def, last_use] as the live interval.
//   2. Linear-scan: sort intervals by start; walk them; assign a free pool
//      register; when all are in use, spill the farthest-reaching active
//      interval to its frame slot.
//   3. Frame extension: add 8-byte save slots for each used callee-saved
//      register (except rbx, already saved) and update frame_size / next_local_off.
//
// Value-preservation: the regalloc ONLY changes WHERE a VReg lives (register vs
// frame slot), not WHAT value it holds. The emit_x64 changes that consume this
// result are value-equivalent (mov reg, rax after def; mov rax, reg before use;
// the computed value is identical). The existing frame-slot path is the
// fallback for spilled VRegs and for non-candidate VRegs (float/slice/struct).

#include "regalloc.hpp"
#include "ast.hpp"        // Type, Prim (for is_int / is_float / is_slice)
#include "x64_emitter.hpp" // Reg enum values

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ember {

namespace {

// The register pool: Win64 callee-saved registers NOT used as scratch by
// emit_x64. rbx (3) is first — it is already saved by the standard prologue
// (rbx_save_offset = -8), so the regalloc does NOT add a save slot for it.
// r14 (14) is the context register — avoided. rbp (5) / rsp (4) are frame/stack.
// rsi (6) / rdi (7) are callee-saved in Win64 and NOT used as scratch by the emit.
// r12 (12) / r13 (13) / r15 (15) are callee-saved and NOT used as scratch.
//
// Order: rbx first (no extra save cost), then the rest. The order matters for
// which registers get used first (and thus which need save slots).
constexpr Reg REG_POOL[] = {
    Reg::rbx,  // 3  — already saved (no extra save slot)
    Reg::r12,  // 12
    Reg::r13,  // 13
    Reg::r15,  // 15
    Reg::rsi,  // 6
    Reg::rdi,  // 7
};
constexpr int32_t POOL_SIZE = int32_t(sizeof(REG_POOL) / sizeof(REG_POOL[0]));

// rbx is at index 0 and is already saved by the standard prologue.
constexpr int32_t RBX_POOL_IDX = 0;

inline int32_t round16(int32_t n) { return (n + 15) & ~15; }

// Is this VReg a register candidate? Only scalar int/bool VRegs (NOT float,
// NOT slice, NOT struct, NOT fn-handle). The VReg must also have a frame_off
// (from the lowering's spill-slot pass) so it can be spilled back to its
// existing slot when the allocator evicts it.
//
// We determine "scalar int/bool" from the producing instruction's meta.type.
// If the type is unknown (nullptr) we conservatively skip it (not a candidate).
bool is_reg_candidate(const ThinInstr& in) {
    if (in.dst == 0) return false;
    // Only these ops produce scalar int/bool results that the emit keeps in rax
    // (and would benefit from a register). Slice/struct/address producers are
    // excluded. Float producers are excluded for v1 (separate xmm reg file).
    switch (in.op) {
    case ThinOp::ConstInt: case ThinOp::ConstBool:
    case ThinOp::Move:
    case ThinOp::Add: case ThinOp::Sub: case ThinOp::Mul:
    case ThinOp::Div: case ThinOp::Mod:
    case ThinOp::And: case ThinOp::Or: case ThinOp::Xor:
    case ThinOp::Shl: case ThinOp::Shr:
    case ThinOp::Neg: case ThinOp::Not: case ThinOp::BitNot:
    case ThinOp::Cmp:
    case ThinOp::Cast:
        break;  // candidate if type is scalar int/bool
    case ThinOp::CallNative: case ThinOp::CallScript:
    case ThinOp::CallIndirect: case ThinOp::CallCrossModule:
        // scalar/float call returns — candidate only if int/bool return
        break;
    default:
        return false;  // LoadFrame/StoreFrame/addresses/aggregates/guards/etc.
    }
    const Type* ty = nullptr;
    if (in.op == ThinOp::CallNative || in.op == ThinOp::CallScript ||
        in.op == ThinOp::CallIndirect || in.op == ThinOp::CallCrossModule) {
        ty = in.ret_type;
    } else {
        ty = in.meta.type;
    }
    if (!ty) return false;
    if (ty->is_slice) return false;
    if (ty->is_float()) return false;
    if (!ty->struct_name.empty()) return false;
    if (ty->is_fn_handle) return false;
    // must be int/bool
    if (!ty->is_int() && ty->prim != Prim::Bool) return false;
    // must have a frame_off (spill slot) so we can evict it back to its slot.
    // The lowering's spill-slot pass assigns one to every plain scalar dst;
    // if it's missing, we conservatively skip (not a candidate).
    if (in.meta.frame_off == 0) return false;
    return true;
}

// Collect every VReg reference (def + uses) from an instruction.
// `def` = in.dst (the defined VReg). `uses` = src1, src2, args[], term cond/ret.
// We record the VReg + whether this position is a def or a use.
struct VRegRef { VReg vreg; bool is_def; };

void collect_refs(const ThinInstr& in, std::vector<VRegRef>& out) {
    if (in.dst != 0) out.push_back({in.dst, true});
    if (in.src1 != 0) out.push_back({in.src1, false});
    if (in.src2 != 0) out.push_back({in.src2, false});
    for (VReg a : in.args) if (a != 0) out.push_back({a, false});
}

void collect_refs_term(const ThinTerm& term, std::vector<VRegRef>& out) {
    if (term.cond != 0) out.push_back({term.cond, false});
    if (term.ret != 0) out.push_back({term.ret, false});
}

// A live interval for a VReg: [start, end] in the linear instruction sequence.
struct LiveInterval {
    VReg vreg;
    uint32_t start;  // first def position
    uint32_t end;    // last use position (inclusive)
    int32_t frame_off;  // the VReg's existing frame slot (for spill)
};

} // namespace

void run_regalloc(ThinFunction& thf, int32_t num_regs) {
    thf.ra.enabled = false;
    thf.ra.map.clear();
    thf.ra.frame_reg_map.clear();
    thf.ra.used_reg_ids.clear();
    thf.ra.save_offsets.clear();

    if (thf.blocks.empty()) return;

    int32_t pool = (num_regs > 0 && num_regs <= POOL_SIZE) ? num_regs : POOL_SIZE;
    if (pool <= 0) return;
    thf.ra.num_regs = pool;

    // ─── 1. Flatten the blocks into a linear instruction sequence ───
    // Position = (block_index, instr_index) flattened to a single uint32_t.
    // We use a simple counter that increments per instruction across all blocks
    // in order. The terminator counts as one position too (it uses VRegs).
    struct Pos { uint32_t flat; const ThinInstr* in; const ThinTerm* term; };
    std::vector<Pos> positions;
    positions.reserve(thf.blocks.size() * 8);
    uint32_t flat = 0;
    for (const auto& blk : thf.blocks) {
        for (const auto& in : blk.instrs) {
            positions.push_back({flat, &in, nullptr});
            ++flat;
        }
        positions.push_back({flat, nullptr, &blk.term});
        ++flat;
    }

    // ─── 2. Compute live intervals ───
    // For each register-candidate VReg, find [first_def, last_use].
    // first_def = the minimum flat position where the VReg is defined (dst).
    // last_use = the maximum flat position where the VReg is used (src/arg/term).
    // If a VReg is only defined (never used), its interval is [def, def] — it's
    // dead but we still assign it (harmless; the emit will mov reg, rax and the
    // register is freed at the next def).
    //
    // We also need the VReg's frame_off (from the producing instruction's
    // meta.frame_off) for spill. A VReg defined in multiple blocks (a join)
    // shares one frame slot (the lowering ensures this), so any def's
    // meta.frame_off is the right spill slot.
    std::unordered_map<VReg, LiveInterval> intervals;
    for (const auto& pos : positions) {
        std::vector<VRegRef> refs;
        if (pos.in) collect_refs(*pos.in, refs);
        else if (pos.term) collect_refs_term(*pos.term, refs);
        for (const auto& r : refs) {
            auto it = intervals.find(r.vreg);
            if (it == intervals.end()) {
                LiveInterval li;
                li.vreg = r.vreg;
                li.frame_off = 0;
                if (r.is_def) {
                    li.start = pos.flat;
                    li.end = pos.flat;  // may grow with uses
                } else {
                    // use before def? (shouldn't happen for well-formed IR, but
                    // be safe: start at this use so the interval covers it)
                    li.start = pos.flat;
                    li.end = pos.flat;
                }
                intervals[r.vreg] = li;
            } else {
                LiveInterval& li = it->second;
                if (r.is_def && pos.flat < li.start) li.start = pos.flat;
                if (!r.is_def && pos.flat > li.end) li.end = pos.flat;
                // also extend end if a def is after the current end (rare: the
                // VReg is redefined — the new def is also a "use position" for
                // the register, since the register is written here)
                if (r.is_def && pos.flat > li.end) li.end = pos.flat;
            }
        }
    }

    // Record the frame_off for each candidate VReg from its producing instr.
    // Only VRegs that are reg candidates (is_reg_candidate) get into the
    // allocation. Others are left out of the map (emit uses their frame slot).
    std::vector<LiveInterval> candidates;
    for (const auto& blk : thf.blocks) {
        for (const auto& in : blk.instrs) {
            if (!is_reg_candidate(in)) continue;
            auto it = intervals.find(in.dst);
            if (it == intervals.end()) continue;
            it->second.frame_off = in.meta.frame_off;
        }
    }
    for (auto& [v, li] : intervals) {
        if (li.frame_off != 0) candidates.push_back(li);
    }

    if (candidates.empty()) return;  // nothing to allocate

    // ─── 3. Sort by start position (linear-scan order) ───
    std::sort(candidates.begin(), candidates.end(),
              [](const LiveInterval& a, const LiveInterval& b) {
                  if (a.start != b.start) return a.start < b.start;
                  return a.end < b.end;  // tie-break: shorter interval first
              });

    // ─── 4. Linear-scan ───
    // active = list of intervals currently holding a register, sorted by end.
    // For each interval in order:
    //   - expire: remove from active all intervals with end < current.start
    //     (free their registers).
    //   - if a free register exists, assign it; add to active.
    //   - else spill: the farthest-reaching active interval (largest end) is
    //     spilled to its frame slot. If the spilled interval's end > current's
    //     end, the current interval takes the freed register. Else the current
    //     interval itself is spilled (it's the shortest-lived).
    std::vector<LiveInterval*> active;  // pointers into candidates (sorted by end)
    std::vector<int32_t> free_regs;     // pool indices of free registers
    for (int32_t i = 0; i < pool; ++i) free_regs.push_back(i);

    // Assignment: vreg -> {in_reg, reg_id, frame_off}
    // Initialize all candidates as spilled (in_reg=false, frame_off=their slot).
    // Linear-scan will flip some to in_reg=true.
    for (auto& li : candidates) {
        thf.ra.map[li.vreg] = {false, -1, li.frame_off};
    }

    for (auto& li : candidates) {
        // expire: free registers from active intervals whose end < li.start
        for (auto it = active.begin(); it != active.end(); ) {
            if ((*it)->end < li.start) {
                // free the pool index that this interval's reg_id corresponds to
                int32_t rid = thf.ra.map[(*it)->vreg].reg_id;
                for (int32_t pi = 0; pi < pool; ++pi)
                    if (int32_t(REG_POOL[pi]) == rid) { free_regs.push_back(pi); break; }
                it = active.erase(it);
            } else {
                ++it;
            }
        }

        if (!free_regs.empty()) {
            // assign a free register
            int32_t reg_idx = free_regs.back();
            free_regs.pop_back();
            thf.ra.map[li.vreg].in_reg = true;
            thf.ra.map[li.vreg].reg_id = int32_t(REG_POOL[reg_idx]);
            active.push_back(&li);
        } else {
            // spill: find the farthest-reaching active interval
            auto farthest = std::max_element(active.begin(), active.end(),
                [](const LiveInterval* a, const LiveInterval* b) {
                    return a->end < b->end;
                });
            if (farthest != active.end() && (*farthest)->end > li.end) {
                // spill the farthest-reaching one; current takes its register
                int32_t reg_id = thf.ra.map[(*farthest)->vreg].reg_id;
                thf.ra.map[(*farthest)->vreg].in_reg = false;
                thf.ra.map[(*farthest)->vreg].reg_id = -1;
                thf.ra.map[li.vreg].in_reg = true;
                thf.ra.map[li.vreg].reg_id = reg_id;
                active.erase(farthest);
                active.push_back(&li);
            } else {
                // current is the shortest-lived (or tie): spill it to its slot
                // (already initialized as in_reg=false, frame_off=its slot)
            }
        }

        // keep active sorted by end (for the farthest-reaching search + expiry)
        std::sort(active.begin(), active.end(),
            [](const LiveInterval* a, const LiveInterval* b) {
                return a->end < b->end;
            });
    }

    // ─── 5. Promote hot scalar frame slots into any still-unused registers ───
    // The lowering represents mutable locals as LoadFrame/StoreFrame pairs. A
    // VReg-only allocator cannot remove those loop-carried memory operations:
    // every iteration creates fresh load/result VRegs and then writes the local
    // slot back. Promote the most frequently accessed plain scalar slots after
    // VReg allocation. Callee-saved homes make this valid across calls and CFG
    // edges; the prologue initializes parameter/local values in memory before
    // the first promoted LoadFrame lazily seeds the register.
    std::vector<bool> reg_used(pool, false);
    for (const auto& [v, a] : thf.ra.map) {
        if (a.in_reg && a.reg_id >= 0)
            for (int32_t i = 0; i < pool; ++i)
                if (int32_t(REG_POOL[i]) == a.reg_id) { reg_used[i] = true; break; }
    }

    std::unordered_map<int32_t, int32_t> frame_accesses;
    std::unordered_map<int32_t, int32_t> frame_loads;
    std::unordered_map<int32_t, int32_t> frame_stores;
    for (const auto& blk : thf.blocks) {
        for (const auto& in : blk.instrs) {
            if (in.meta.frame_off == 0 || in.meta.field_off != 0) continue;
            if (in.op != ThinOp::LoadFrame && in.op != ThinOp::StoreFrame) continue;
            if (in.op == ThinOp::LoadFrame && in.src1 != 0) continue;
            if (in.op == ThinOp::StoreFrame && (in.src2 != 0 || in.src1 == 0)) continue;
            const Type* ty = in.meta.type;
            if (!ty || ty->is_slice || ty->is_float() || !ty->struct_name.empty() ||
                ty->is_fn_handle || (!ty->is_int() && ty->prim != Prim::Bool)) continue;
            ++frame_accesses[in.meta.frame_off];
            if (in.op == ThinOp::LoadFrame) ++frame_loads[in.meta.frame_off];
            else ++frame_stores[in.meta.frame_off];
        }
    }
    std::vector<std::pair<int32_t, int32_t>> hot_slots; // {access count, offset}
    hot_slots.reserve(frame_accesses.size());
    std::unordered_map<int32_t, bool> loop_carried_slots;
    for (const auto& blk : thf.blocks) {
        bool has_backedge = false;
        if (blk.term.kind == TermKind::Jmp) has_backedge = blk.term.target <= blk.id;
        else if (blk.term.kind == TermKind::Branch)
            has_backedge = blk.term.target <= blk.id || blk.term.false_target <= blk.id;
        if (!has_backedge) continue;
        for (const auto& in : blk.instrs) {
            if (in.op == ThinOp::StoreFrame && in.src1 != 0 && in.src2 == 0 &&
                in.meta.frame_off != 0 && in.meta.field_off == 0)
                loop_carried_slots[in.meta.frame_off] = true;
        }
    }
    for (const auto& [off, count] : frame_accesses) {
        // Only loop-carried mutable source slots are worthwhile and safe to
        // promote. Read-only locals often alias a producing VReg's spill slot;
        // promoting those would split the value between two register homes.
        if (frame_loads[off] != 0 && loop_carried_slots[off])
            hot_slots.push_back({count, off});
    }
    std::sort(hot_slots.begin(), hot_slots.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second > b.second;
              });
    for (const auto& [count, off] : hot_slots) {
        (void)count;
        int32_t free_idx = -1;
        for (int32_t i = 0; i < pool; ++i) {
            if (!reg_used[i]) { free_idx = i; break; }
        }
        if (free_idx < 0) break;
        thf.ra.frame_reg_map[off] = int32_t(REG_POOL[free_idx]);
        reg_used[free_idx] = true;
    }

    // ─── 6. Record used pool registers + allocate save slots ───
    // A pool register is "used" by either a VReg interval or promoted slot.

    // Extend the frame to add save slots for used callee-saved registers.
    // rbx (pool idx 0) is already saved at rbx_save_offset — skip it.
    // Each save slot is 8 bytes at a new rbp-negative offset.
    int32_t save_top = thf.frame.next_local_off;  // grows downward (negative)
    for (int32_t i = 0; i < pool; ++i) {
        if (!reg_used[i]) continue;
        if (i == RBX_POOL_IDX) {
            // rbx is already saved; record the existing offset (no new slot)
            thf.ra.used_reg_ids.push_back(int32_t(REG_POOL[i]));
            thf.ra.save_offsets.push_back(thf.frame.rbx_save_offset);
            continue;
        }
        save_top += 8;
        int32_t off = -save_top;
        thf.ra.used_reg_ids.push_back(int32_t(REG_POOL[i]));
        thf.ra.save_offsets.push_back(off);
    }

    // Update the frame size to fit the new save slots. The new frame_size must
    // be at least save_top + 16 (matching the lowering's convention: the +16
    // covers the win64 call-frame alignment headroom + the struct-ret-ptr /
    // arg-temps area that may sit above the spill slots). We also preserve any
    // existing headroom (the lowering's frame_size already includes it).
    int32_t needed = save_top + 16;
    int32_t new_frame = std::max(int32_t(thf.frame.frame_size), needed);
    thf.frame.next_local_off = save_top;
    thf.frame.frame_size = round16(new_frame);

    thf.ra.enabled = true;
}

} // namespace ember
