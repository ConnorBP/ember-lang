// src/aapcs64_classify.hpp — AAPCS64 (ARM64 Procedure Call Standard) argument
// + return classification for the ember ARM64 backend.
// plan_MACOS_ARM64.md Phase 6c. A shared, tested classifier INDEPENDENT of the
// emit code: emit_arm64's call marshaling + param spills + return handling
// consume its result. This mirrors the role of Win64's word-class logic but for
// AAPCS64, which diverges radically from Win64 (HFA — Homogeneous Float
// Aggregate — passed in FP registers; ≤16B composites in up to 2 regs;
// >16B by hidden pointer; GP x0-x7 + FP v0-v7 are INDEPENDENT register streams).
//
// STAGING (advisor-guided): this first version supports
//   • scalars: f32/f64 → 1 FP reg (v0-v7); i8..i64/u8..u64/bool/ptr/handle → 1 GP reg (x0-x7)
//   • slices {ptr,len} + lambdas {fn,env}: 2 consecutive GP words
//   • composites ≤ 16 bytes:
//       - HFA: a struct/fixed-array of 1..4 IDENTICAL f32 (or f64) members →
//         that many FP regs (v0-v3 max for an arg)
//       - otherwise (POD int/handle aggregates, packed) → GP regs (up to 2
//         eightbyte-equivalent words; ember structs are alignment-1 packed so
//         they occupy ceil(size/8) GP words)
//   • composites > 16 bytes: INDIRECT (passed/returned by pointer — x8 for the
//     RETURN dest; the caller allocates + passes the pointer for an arg)
//   • NOT yet (deferred, throw): mixed-float+int HFAs that aren't pure HFA,
//     >4-member HFAs, stack args (>8 GP or >8 FP args in one call), variadics.
//     These are rare in ember scripts; expand later if a real script needs them.
//
// AAPCS64 register streams are INDEPENDENT: a GP arg consumes an x reg and an
// FP arg consumes a v reg, in parallel (unlike Win64's slot-parallel). The
// classifier tracks both counts so a caller can place args correctly.
#pragma once
#include "ast.hpp"          // Type, Prim
#include "sema.hpp"         // StructLayoutTable, StructLayout
#include <cstdint>
#include <string>
#include <vector>

namespace ember {

// One classified argument/return slot.
struct Aapcs64Slot {
    enum class Kind : uint8_t {
        GpReg,      // in an x register (x0-x7)
        FpReg,      // in a v register (v0-v7) — an HFA member or a scalar float
        Indirect,   // passed/returned by pointer (>16B composite)
    };
    Kind kind = Kind::GpReg;
    // For GpReg/FpReg: the register INDEX within its stream (0 = x0/v0). For
    // Indirect: unused (the pointer itself occupies a GP reg — the caller
    // allocates + passes it as the next GP arg, or x8 for a return).
    uint8_t reg_index = 0;
    // For an HFA arg spanning multiple FP regs, this is ONE slot per member;
    // the caller emits consecutive v regs starting at reg_index. The number of
    // member slots = hfa_count (below, on the Arg). For a scalar float, 1.
    uint8_t width_bytes = 8;   // the slot's byte width (1/2/4/8) for GP, 4/8 for FP
    bool is_f32 = false;       // FP slot width: true=f32, false=f64
};

// The full classification of one argument (may span multiple slots: a slice =
// 2 GP slots; an HFA = N FP slots). `indirect` means the arg is passed by
// pointer (the pointer is ONE GP slot, recorded separately by the caller).
struct Aapcs64ArgClass {
    std::vector<Aapcs64Slot> slots;
    bool indirect = false;       // >16B composite: pass a pointer
    uint8_t hfa_count = 0;       // # of identical FP members (HFA), 0 if not HFA
    bool is_f32_hfa = false;     // HFA member width (true=f32, false=f64)
    int32_t byte_size = 0;       // the arg's true byte size (for indirect copy)
    const Type* type = nullptr;
};

// Classify a single argument type given the running GP + FP register counts
// (so the slot reg_index is correct). Advances gp_used/fp_used by the registers
// this arg consumes (GP regs for GpReg/Indirect-pointer; FP regs for FpReg/HFA).
// Throws std::runtime_error for unsupported cases (mixed HFAs, >4-member HFAs,
// stack args when the arg would exceed the 8-reg streams — the caller decides
// whether to reject the whole call or fall back).
Aapcs64ArgClass classify_aapcs64_arg(const Type* ty, const StructLayoutTable* structs,
                                     uint8_t gp_used, uint8_t fp_used);

// Classify a RETURN type. AAPCS64 return: scalar/ptr → x0; float → v0; HFA →
// v0..v(N-1); ≤16B composite → x0/x1 (or v0/v1 for a 2-float non-HFA? rare);
// >16B composite → INDIRECT via x8 (the caller passes the dest ptr in x8 as a
// hidden first arg, which the classifier accounts for by also classifying the
// caller's arg list with an implicit x8-dest). Returns the return slots +
// `indirect` (caller allocates dest + passes x8). For indirect returns, the
// FUNCTION receives the dest pointer in x8 (not x0).
Aapcs64ArgClass classify_aapcs64_return(const Type* ty, const StructLayoutTable* structs);

// Helper: is `ty` a pure HFA (1..4 identical f32 or f64 members)? Returns the
// member count + width via out-params; false if not a pure HFA.
bool is_hfa(const Type* ty, const StructLayoutTable* structs,
            uint8_t& member_count, bool& is_f32);

} // namespace ember
