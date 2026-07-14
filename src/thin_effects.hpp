// thin_effects.hpp — the public exhaustive operation-effect model for Thin IR.
//
// This is the AUTHORITATIVE side-effect classification shared by the
// optimization passes, the dead-code/store eliminators, and any future
// transform that needs to know whether an instruction may be removed or
// reordered. It replaces the private `is_side_effecting`/`is_pure` switch
// that previously lived in extensions/opt/ext_opt.cpp.
//
// Design ref: docs/planning/plan_POLYMORPHIC_CODE_ENGINE.md §6.2.
//
// The model is exhaustive over the current ThinOp set:
//   - pure arithmetic / moves / casts / compares (no memory effect);
//   - implicit producer frame-home writes (a producer pins its result to
//     meta.frame_off — an implicit frame write that DCE/const-prop must
//     honor);
//   - explicit frame stores/loads (StoreFrame/LoadFrame);
//   - global loads/stores (LoadGlobal/StoreGlobal);
//   - indirect / computed-address memory (StoreAddr, FieldAddr/IndexAddr
//     address computations, CopyBytes source/dest ranges);
//   - calls (conservatively effectful until purity is explicit ABI metadata);
//   - traps / guards (BoundsCheck, DivOverflowCheck, DepthCheck, BudgetCheck,
//     CallTargetGuard — may trap, never removable by a dead-result rule);
//   - aggregate initialization (StructLitInit/ArrayLitInit — frame writes);
//   - StringDecrypt (writes a decrypted-data temp buffer + a slice result
//     slot, and reads encrypted rodata);
//   - escaped addresses (a frame address computed by FieldAddr/IndexAddr and
//     then passed to a call or stored indirectly aliases unknown memory).
//
// Static flags alone NEVER authorize removal. `removable_if_result_dead()`
// is the single predicate a dead-result rule (DCE, CSE-coalesce) consults: it
// combines the descriptor with the instruction's own shape to decide whether
// dropping the instruction (when its dst VReg is unused) preserves every
// observable effect. A side-effecting instruction is never removable, even if
// its dst is unused.

#pragma once

#include "thin_ir.hpp"          // ThinInstr, ThinOp, ThinMeta, Type
#include "x64_emitter.hpp"      // AbsFixup::Kind (reloc base kind)

#include <cstdint>
#include <vector>

namespace ember {

// One effect flag. `EffectFlags` is the bitset formed by OR-ing these. The
// numeric values are powers of two so they compose as a bitmask; they are NOT
// serialized (the descriptor is a compile-time analysis artifact, never
// persisted).
enum class ThinEffectFlag : uint32_t {
    None              = 0,
    ReadsFrame        = 1u << 0,   // reads an rbp-relative frame slot
    WritesFrame       = 1u << 1,   // writes an rbp-relative frame slot
    ReadsGlobal       = 1u << 2,   // reads the globals block
    WritesGlobal      = 1u << 3,   // writes the globals block
    ReadsIndirect     = 1u << 4,   // reads through a computed/unknown address
    WritesIndirect    = 1u << 5,   // writes through a computed/unknown address
    CallsUnknown      = 1u << 6,   // may call an unknown callee (call barrier)
    MayTrap           = 1u << 7,   // may trap / abort execution
    WritesTemp        = 1u << 8,   // writes a temporary buffer (StringDecrypt)
    ImplicitSpillWrite= 1u << 9,   // producer pins result to frame_off (implicit write)
    EscapesAddress    = 1u << 10,  // produces an address that may escape to unknown memory
};

// A bitset of ThinEffectFlag values. OR-composes; tests with `has(flag)`.
struct EffectFlags {
    uint32_t bits = 0;
    EffectFlags() = default;
    explicit EffectFlags(uint32_t b) : bits(b) {}
    EffectFlags(ThinEffectFlag f) : bits(static_cast<uint32_t>(f)) {}
    EffectFlags& operator|=(EffectFlags o) { bits |= o.bits; return *this; }
    friend EffectFlags operator|(EffectFlags a, EffectFlags b) { return EffectFlags{a.bits | b.bits}; }
    bool has(ThinEffectFlag f) const { return (bits & static_cast<uint32_t>(f)) != 0; }
    bool none() const { return bits == 0; }
    bool any() const { return bits != 0; }
};

// Which address space a byte interval refers to. `Unknown` covers computed
// addresses (StoreAddr [src2+off], indirect loads/stores through a register)
// and any access whose exact space cannot be proven at classification time.
enum class MemorySpace : uint8_t {
    Frame,      // rbp-relative slot (meta.frame_off / field_off / data_temp_off)
    Global,     // the module globals block (base_kind == GlobalsBase)
    Rodata,     // function-local rodata (ConstStringRef / StringDecrypt source)
    Unknown,    // computed / indirect / escaped address
};

// A byte interval touched by a read or write. `begin` is rbp-relative for
// Frame space, a globals-block offset for Global, a rodata offset for Rodata,
// and 0 + unknown=true for Unknown. `size` is the byte count; `unknown=true`
// means the interval's begin/size cannot be pinned (the access is through a
// computed address) and the descriptor's `aliases_unknown_memory` is also set.
struct ByteInterval {
    MemorySpace space = MemorySpace::Unknown;
    int64_t begin = 0;
    uint64_t size = 0;
    bool unknown = false;
};

// The full effect descriptor for one instruction. `reads` and `writes` carry
// the exact/overlapping byte intervals the instruction touches (frame slots,
// globals, rodata, copy ranges, string-decrypt buffers). `flags` is the
// composable bitmask. `aliases_unknown_memory` is set when any access is
// through a computed/escaped address (so a caller's exact-offset frame-slot
// map must be flushed).
struct ThinEffectDescriptor {
    EffectFlags flags;
    std::vector<ByteInterval> reads;
    std::vector<ByteInterval> writes;
    bool aliases_unknown_memory = false;
};

// Classify the effects of one ThinInstr. This is the single source of truth:
// the optimization passes and any custom pass consult this instead of
// re-deriving a private side-effect table. Classification is conservative —
// when an access cannot be pinned to a known space, it is reported as
// Unknown + aliases_unknown_memory.
ThinEffectDescriptor classify_thin_effects(const ThinInstr& in);

// The dead-result predicate. Returns true ONLY when dropping `in` (because its
// dst VReg is unused) preserves every observable effect: the instruction must
// be free of reads/writes/calls/traps/escapes/implicit-spill-writes, and its
// dst must be a real removable producer (dst != 0). A side-effecting
// instruction is never removable even if its dst is unused. `desc` should be
// the result of `classify_thin_effects(in)`; passing a mismatched descriptor
// is treated conservatively (removable only if both the instruction shape and
// the descriptor agree the instruction is effect-free).
//
// Note: StoreFrame is handled by the caller's dead-STORE analysis (it has no
// dst VReg and is removable only when its slot is provably dead). This
// predicate returns false for StoreFrame.
bool removable_if_result_dead(const ThinInstr& in, const ThinEffectDescriptor& desc);

} // namespace ember
