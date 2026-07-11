// ember codegen Stage 1 optimization — peephole framework.
//
// docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4.5 (the pass interface).
//
// Stage 1 LAYERS optimization over the existing tree-walker WITHOUT a
// flag-day rewrite: after the tree-walker emits a function's bytes (and
// resolve_fixups + the AbsFixup/native-fixup patching is done), a peephole
// pipeline runs over the final `vector<uint8_t>` byte buffer, pattern-
// matching redundant instruction sequences and rewriting them in-place.
//
// The peephole operates on the FINAL emitted bytes (post-resolve_fixups,
// post-AbsFixup-fill). It is therefore a byte-buffer -> byte-buffer transform.
// It must NOT relocate the 8-byte imm64 regions recorded as AbsFixups or
// NativeFixups — the `.em` serializer reads `abs_fixups()`/`native_fixups()`
// by `code_offset` and those offsets must stay valid against the (rewritten)
// buffer. Concretely: a `mov r, imm64` emitted by `mov_reg_imm64_external`
// (a dispatch-base/globals-base/registry/rodata load) is recorded as an
// AbsFixup and its 8-byte immediate was just patched with the real address
// — the peephole must not shrink it. A plain `mov rax, imm64` from an
// `IntLit` is NOT an AbsFixup and IS safe to shrink. The SmartImm pass takes
// the set of "do-not-touch" offsets (the AbsFixup + NativeFixup code_offsets)
// so it skips the relocatable loads.
//
// Behind a flag (CodeGenCtx::enable_peephole, default false). When off, the
// peephole does not run and the codegen is byte-identical to today (the 24/24
// ctest gate + 268/0/0 lang gate hold unchanged). The per-peephole-rewrite
// value-equivalence is pinned by codegen_opt_test (design §4.7).

#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>

namespace ember {

// A "do-not-touch" byte offset: an 8-byte imm64 region whose code_offset is
// recorded by an AbsFixup or NativeFixup. The peephole must not rewrite
// (shrink/relocate) any instruction whose bytes overlap one of these,
// because the `.em` serializer reads the fixup list by offset.
struct PeepholeGuardedRegions {
    // The 8-byte imm64 placeholders (AbsFixup.code_offset, NativeFixup.code_offset).
    // Each guards the 8 bytes [off, off+8).
    std::unordered_set<uint32_t> imm64_offsets;
};

struct PeepholeCtx {
    std::vector<uint8_t>& bytes;
    // Resolved label offsets (post-resolve_fixups): label_id -> byte offset.
    // A peephole that rewrites bytes before a label target must adjust this map
    // AND the label's branch rel32 fixups (none of the Stage-1 peepholes move
    // bytes across a label — they are strictly local in-place rewrites of equal-
    // or-shorter-length sequences, with nop padding for the length delta — so
    // label offsets never shift). Kept here for the Stage-2 cross-block passes.
    const std::unordered_map<uint32_t, uint32_t>& resolved_labels;
    PeepholeGuardedRegions guarded;
};

// A pass operates on the emitted byte buffer. Stage 1 ships two concrete
// passes (SmartImmPass, SetccMovzxPass); the interface is extensible for the
// later redundant-guard peepholes (W5/W6/W7) and the rel32->rel8 branch
// shrink (CODEGEN_SPEC §4 deferral).
struct PeepholePass {
    virtual ~PeepholePass() = default;
    virtual const char* name() const = 0;
    // Returns true iff any bytes were rewritten. Idempotent: a second run is a no-op.
    virtual bool run(PeepholeCtx& ctx) = 0;
};

// The manager, additive. Runs each pass to a fixed point (a pass that made a
// change is re-run until it reports no change), then proceeds to the next.
struct PeepholePipeline {
    std::vector<std::unique_ptr<PeepholePass>> passes;
    void add(std::unique_ptr<PeepholePass> p) { passes.push_back(std::move(p)); }
    void run_all(PeepholeCtx& ctx);
};

// --- Stage 1 concrete passes ---

// SmartImmPass (design W4): `mov r64, imm64` (REX.W B8+rd io, 10 bytes) where
// the imm64 fits a cheaper form -> rewrite in place:
//   - imm64 fits signed-32 (INT32_MIN..INT32_MAX): `mov r64, imm32`
//     (REX.W C7 /0 id, 7 bytes) — sign-extended to 64. Saves 3 bytes.
//   - imm64 fits unsigned-32 (0..UINT32_MAX) AND dst is rax/eax (the accumulator,
//     where every IntLit lands): `mov eax, imm32` (B8 imm32, 5 bytes,
//     zero-extends to rax). Saves 5 bytes. Only safe for rax because the rest of
//     the codegen assumes rax is the full 64-bit accumulator; writing eax zero-
//     extends rax which is the correct semantics for a non-negative literal.
// Skips any imm64 load whose offset is a guarded region (an AbsFixup or
// NativeFixup placeholder — those are relocatable and must stay 10 bytes).
// Pads the length delta with trailing NOPs so subsequent label offsets never
// shift (the pass is strictly local; no cross-block offset adjustment needed).
struct SmartImmPass : PeepholePass {
    const char* name() const override { return "smart-imm"; }
    bool run(PeepholeCtx& ctx) override;
};

// SetccMovzxPass (design W10): the comparison-result sequence
//   `0F 9x C0` (setcc al, 3 bytes) + `48 0F B6 C0` (movzx rax, al, 4 bytes)
// -> `48 31 C0` (xor rax,rax, 3 bytes) + `0F 9x C0` (setcc al, 3 bytes).
// Same total length (7 -> 6, saves 1 byte), and removes the false-dependency
// on rax's upper bytes that `movzx` also breaks (xor rax,rax fully zeroes rax
// first, so setcc writes the low byte into a known-zero register). Pads the
// 1-byte length delta with a trailing NOP. Idempotent (the xor+setcc form is
// not matched by the pattern on a second run).
struct SetccMovzxPass : PeepholePass {
    const char* name() const override { return "setcc-movzx"; }
    bool run(PeepholeCtx& ctx) override;
};

// Build the Stage-1 default pipeline (the two shipped passes, in order).
PeepholePipeline make_stage1_pipeline();

} // namespace ember
