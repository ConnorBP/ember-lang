// ember codegen Stage 1 optimization — peephole passes implementation.
//
// docs/spec/CODEGEN_OPTIMIZATION_DESIGN.md §4.5 (interface) + §3.5/§3.6 (W4
// smart-imm, W10 setcc;movzx). See peephole.hpp for the frame.
//
// Each pass is a byte-buffer -> byte-buffer in-place rewrite. The Stage-1
// passes are strictly LOCAL: they rewrite a contiguous sequence in place and
// pad any length delta with trailing NOPs (0x90), so NO label offset ever
// shifts and NO branch rel32 fixup needs re-resolving. This is the property
// that makes them safe to run after resolve_fixups without a second fixup
// pass. (Stage 2's cross-block passes — the rel32->rel8 branch shrink — will
// need to re-resolve label fixups and is therefore NOT in Stage 1.)

#include "peephole.hpp"
#include <cstring>

namespace ember {

void PeepholePipeline::run_all(PeepholeCtx& ctx) {
    for (auto& p : passes) {
        // Fixed point per pass: re-run until it reports no change.
        while (p->run(ctx)) { /* re-run */ }
    }
}

// ---- helpers ----

// Read a little-endian int64 from bytes[pos..pos+8). Caller checks bounds.
static int64_t read_le_i64(const std::vector<uint8_t>& b, size_t pos) {
    int64_t v = 0;
    std::memcpy(&v, b.data() + pos, 8);
    return v;
}
static void write_le_i32(std::vector<uint8_t>& b, size_t pos, int32_t v) {
    std::memcpy(b.data() + pos, &v, 4);
}

// Is the 8-byte region [off, off+8) a guarded imm64 placeholder? The SmartImm
// pass must not shrink a `mov r, imm64` whose 8-byte immediate is recorded as
// an AbsFixup or NativeFixup (those are relocatable; the `.em` serializer reads
// the fixup list by offset). We check whether the START of the 10-byte mov-imm64
// instruction (the REX byte) overlaps any guarded offset's 8-byte window.
static bool imm64_is_guarded(const PeepholeGuardedRegions& g, uint32_t rex_off) {
    // The imm64 bytes are at [rex_off+2, rex_off+10) (REX + opcode-B8+rd + 8 bytes).
    // A guarded offset (an AbsFixup/NativeFixup code_offset) points at the imm64's
    // first byte (rex_off+2). Check the precise offset, then a defensive overlap
    // check (handles a partial-overlap edge case that should not arise but would
    // corrupt the fixup table if it did).
    uint32_t imm_start = rex_off + 2;
    uint32_t imm_end   = imm_start + 8; // [imm_start, imm_end)
    for (uint32_t off : g.imm64_offsets) {
        if (off == imm_start) return true;
        uint32_t g_end = off + 8;
        if (!(imm_end <= off || g_end <= imm_start)) return true; // overlap
    }
    return false;
}

// ---- SmartImmPass (W4) ----
//
// Pattern: `REX.W B8+rd io` (mov r64, imm64), 10 bytes:
//   byte[0] = REX (0x48 | (B<<0)) — W=1, R=0,X=0, B=rd-extended-bit
//   byte[1] = 0xB8 + (rd & 7)
//   byte[2..9] = imm64 (little-endian)
//
// Rewrite candidates (only when NOT a guarded relocatable load):
//   (a) imm64 fits unsigned-32 (0..0xFFFFFFFF) AND dst low3 == 0 (rax/eax):
//       `B8 imm32` (mov eax, imm32, 5 bytes, zero-extends to rax).
//       REX is dropped (32-bit operand size, no REX.W). bytes: [B8, imm32*4].
//       Length delta: 10 -> 5, pad 5 trailing NOPs.
//       Semantics: mov eax, imm32 zero-extends to rax = imm64 (since imm64
//       fits u32, the upper 32 bits are zero, matching the zero-extension).
//   (b) imm64 fits signed-32 (INT32_MIN..INT32_MAX) for ANY dst register:
//       `REX.W C7 /0 id` (mov r64, imm32 sign-extended, 7 bytes).
//       bytes: [REX.W, 0xC7, modrm(11, 0, rd&7), imm32*4].
//       Length delta: 10 -> 7, pad 3 trailing NOPs.
//       Semantics: imm32 is sign-extended to 64 bits, matching imm64 when the
//       value is in the signed-32 range.
//
// (a) is strictly better than (b) for rax (5 vs 7 bytes), so try (a) first for
// rax, then (b) for any reg. For non-rax dsts only (b) applies (writing the
// 32-bit reg zero-extends the 64-bit reg, which is correct only if the upper 32
// bits of the imm64 are zero — i.e. the u32 case — but the 32-bit opcode form
// `B8+rd imm32` for a non-rax reg would be `mov r32d, imm32` which zero-extends;
// we keep this conservative and only use form (a) for rax (low3==0), form (b)
// for all regs including rax as a fallback).
//
// The imm64 value is read from the already-filled bytes (the AbsFixup fill in
// compile_func wrote the real address for guarded loads; for a plain IntLit
// load the imm64 was the literal all along). A guarded load is skipped; a plain
// literal load is rewritten.

bool SmartImmPass::run(PeepholeCtx& ctx) {
    auto& b = ctx.bytes;
    bool changed = false;
    size_t i = 0;
    const size_t n = b.size();
    while (i + 10 <= n) {
        // Match: REX.W (0x48 | B-bit) then B8+rd.
        uint8_t rex = b[i];
        // REX.W with R=0,X=0: 0x48 or 0x49 (B=1). Only these two forms are emitted
        // by mov_reg_imm64 for the rax..r15 dsts (REX = 0x48 for rax..rdi, 0x49 for
        // r8..r15). Accept exactly those (no R/X bits — those would be a different
        // instruction reusing the B8 opcode prefix, which ember never emits).
        if (rex != 0x48 && rex != 0x49) { ++i; continue; }
        uint8_t op = b[i + 1];
        if (op < 0xB8 || op > 0xBF) { ++i; continue; } // 0xB8+rd, rd in 0..7
        uint8_t rd_low3 = op - 0xB8;
        // The imm64 is at [i+2, i+10).
        if (imm64_is_guarded(ctx.guarded, uint32_t(i))) { i += 10; continue; }
        int64_t imm = read_le_i64(b, i + 2);

        // Candidate (a): unsigned-32 fits AND dst is rax (low3==0).
        // mov eax, imm32 = B8 imm32 (5 bytes, zero-extends to rax).
        if (rd_low3 == 0) {
            uint64_t u = uint64_t(imm);
            if (u <= 0xFFFFFFFFu) {
                // Rewrite to `B8 imm32`. Drop REX (32-bit op, zero-extends).
                b[i] = 0xB8;
                write_le_i32(b, i + 1, int32_t(uint32_t(u)));
                // Pad the 5 trailing bytes (was 10, now 5) with NOPs.
                for (int k = 0; k < 5; ++k) b[i + 5 + k] = 0x90;
                changed = true;
                i += 10; // skip the full original 10-byte footprint (the pad is inside it)
                continue;
            }
        }

        // Candidate (b): signed-32 fits for any dst.
        if (imm >= INT32_MIN && imm <= INT32_MAX) {
            // mov r64, imm32 = REX.W C7 /0 id (7 bytes).
            // REX.W = 0x48 | (B-bit for r8..r15). The original REX already encodes
            // the B bit correctly (0x48 for rax..rdi, 0x49 for r8..r15), so reuse it.
            b[i] = rex;                 // REX.W (B-bit preserved)
            b[i + 1] = 0xC7;            // mov r/m64, imm32
            b[i + 2] = uint8_t(0xC0 | rd_low3); // modrm: mod=11, reg=0 (/0), rm=rd
            write_le_i32(b, i + 3, int32_t(imm));
            // Pad the 3 trailing bytes (was 10, now 7) with NOPs.
            for (int k = 0; k < 3; ++k) b[i + 7 + k] = 0x90;
            changed = true;
            i += 10;
            continue;
        }

        i += 10;
    }
    return changed;
}

// ---- SetccMovzxPass (W10) ----
//
// Pattern (7 bytes, the BinExpr comparison-result sequence):
//   byte[0..2] = `0F 9x C0` (setcc al, where 9x = 0x90 | cc, cc in 0..15)
//   byte[3..6] = `48 0F B6 C0` (movzx rax, al)
//
// Rewrite (6 bytes + 1 trailing NOP):
//   byte[0..2] = `48 31 C0` (xor rax, rax — zeroes rax, clears the false dep)
//   byte[3..5] = `0F 9x C0` (setcc al — writes the low byte into the now-zero rax)
//   byte[6]    = `0x90` (NOP pad — length delta 7->6)
//
// Semantics: xor rax,rax sets rax=0 and clears flags, but the immediately-
// following setcc reads the flags set by the cmp THAT PRECEDED the original
// sequence — wait: the cmp is BEFORE the setcc, and setcc reads those flags.
// In the original, `cmp; setcc al; movzx rax,al`: setcc reads cmp's flags,
// movzx does not touch flags. In the rewrite, `cmp; xor rax,rax; setcc al`:
// the xor CLEARS flags (xor always sets ZF=1, clears CF/OF/SF). That would
// destroy the cmp's flags before setcc reads them!
//
// So the naive `xor; setcc` rewrite is INCORRECT — xor clobbers flags. The
// correct zero-the-register-without-clobbering-flags form is to put the zeroing
// BEFORE the cmp (where flags aren't live yet), i.e. `xor rax,rax; cmp; setcc
// al` — but that requires matching back across the cmp, which crosses an
// instruction boundary the peephole would have to recognize.
//
// The design doc §3.6 explicitly flags W10 as "a micro-win; recorded for
// completeness, not a headline" and notes the rewrite is `xor rax,rax; setcc
// al`. Given the flags-clobber hazard, the SAFE rewrite is to SKIP W10 rather
// than ship an incorrect peephole. Stage 1 ships SmartImm only; W10 is
// documented as analyzed-but-deferred (the xor-flags interaction makes the
// naive in-place rewrite unsound; a correct version needs the pre-cmp zeroing
// which is a cross-instruction peephole = Stage 2).
//
// This pass is therefore a NO-OP in Stage 1: it is retained in the pipeline as
// a placeholder for the Stage-2 cross-instruction form, and reports no change.

bool SetccMovzxPass::run(PeepholeCtx& /*ctx*/) {
    // Stage 1: no-op (see file header — the in-place `xor;setcc` rewrite
    // clobbers the cmp's flags, which setcc reads. A correct W10 rewrite needs
    // the zeroing moved before the cmp (a cross-instruction peephole), which is
    // Stage 2. Shipped inert so the pipeline shape matches the design doc §4.5
    // and the Stage-2 upgrade is additive.
    return false;
}

// Build the Stage-1 default pipeline. Order: SmartImm first (the headline W4
// win), SetccMovzx second (inert in Stage 1, placeholder for Stage 2).
PeepholePipeline make_stage1_pipeline() {
    PeepholePipeline p;
    p.add(std::make_unique<SmartImmPass>());
    p.add(std::make_unique<SetccMovzxPass>());
    return p;
}

} // namespace ember
