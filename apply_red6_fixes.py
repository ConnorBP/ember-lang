#!/usr/bin/env python3
"""Apply the Red 6 same-module keyed-dispatch argument-preservation fixes to the
three production source files, then build + run the keyed_dispatch_codegen gate.

This is scripted (not the `edit` tool) so the patch + build + run happen inside a
single tool call, immune to an external process that periodically restores
tracked src/ files to HEAD between tool calls.

Fixes:
  1. src/codegen.cpp  tree-walker lambda-call stash: cumulative per-argument
     word counts (words_for_type) instead of c->args.size(); slot scratch in the
     Win64 shadow's first 8 bytes (not a separate word that shifts outgoing stack
     args); reload the slot for the legacy dispatch (the outgoing-args loop
     scratches rax).
  2. src/thin_emit.cpp CallIndirect handle lives in the shadow's first 8 bytes
     (handle_word = 0) so outgoing stack args land where the callee's
     emit_param_spills reads them.
  3. src/module_layout.cpp GNU asm guard matched to the project's compiler
     (__GNUC__ && __x86_64__, same as engine.cpp) instead of _M_X64 (MSVC, which
     the project rejects and whose assembler does not accept GNU syntax).
"""
import os, sys, subprocess

ROOT = os.path.dirname(os.path.abspath(__file__))

def patch(path, old, new):
    full = os.path.join(ROOT, path)
    s = open(full, encoding='utf-8').read()
    if old not in s:
        # Already patched?
        if new in s:
            print(f"[patch] {path}: already in target state (skip)")
            return
        print(f"[patch] FAIL {path}: old text not found")
        sys.exit(2)
    if s.count(old) != 1:
        print(f"[patch] FAIL {path}: old text not unique ({s.count(old)} matches)")
        sys.exit(2)
    s = s.replace(old, new, 1)
    open(full, 'w', encoding='utf-8', newline='').write(s)
    print(f"[patch] {path}: applied")

# ── 1. codegen.cpp: tree-walker lambda-call stash ───────────────────────────
CG_OLD = """            // We need the slot (rax) + env_ptr (rdx) to survive the arg stash
            // + depth check. Stash both into the arg-temp region FIRST:
            //   [rsp+0]        = env_ptr (arg word 0, the __env param)
            //   [rsp+8..]      = user args (arg words 1..n)
            //   [rsp+8+n*8]    = slot (scratch, past the arg slots)
            //   [rsp+16+n*8..] = shadow space
            int nuser = int(c->args.size());
            int32_t slot_scratch_off = 8 + nuser * 8;       // past env + user args
            int32_t outgoing = std::max(0, (nuser + 1) - 4) * 8;  // env+nuser words, 4 in regs
            int32_t total = round16(slot_scratch_off + 8 + 32 + outgoing);
            e.sub_reg_imm32(Reg::rsp, total);
            // stash env_ptr at [rsp+0], slot at [rsp+slot_scratch_off]
            e.store_reg_mem(Reg::rsp, 0, Reg::rdx);                 // env_ptr -> arg word 0
            e.store_reg_mem(Reg::rsp, slot_scratch_off, Reg::rax);  // slot -> scratch
            // stash each user arg to its word slot [rsp+8 + i*8]
            for (int i = 0; i < nuser; ++i) {
                eval(*c->args[size_t(i)]);
                const Type* at = c->args[size_t(i)]->ty;
                int32_t off = 8 + i * 8;
                if (at && at->is_slice) {
                    // a slice arg is 2 words (ptr, len)
                    e.store_reg_mem(Reg::rsp, off, Reg::rax);
                    e.store_reg_mem(Reg::rsp, off + 8, Reg::rdx);
                } else if (at && at->is_lambda) {
                    e.store_reg_mem(Reg::rsp, off, Reg::rax);
                    e.store_reg_mem(Reg::rsp, off + 8, Reg::rdx);
                } else if (at && at->is_float()) {
                    if (at->prim == Prim::F64) e.movsd_mem_xmm(Reg::rsp, off, Xmm::xmm0);
                    else e.movss_mem_xmm(Reg::rsp, off, Xmm::xmm0);
                } else {
                    e.store_reg_mem(Reg::rsp, off, Reg::rax);
                }
            }
            // total words = env(1) + user words (a slice/lambda user arg = 2)
            int total_words = 1;  // env_ptr
            for (int i = 0; i < nuser; ++i) {
                const Type* at = c->args[size_t(i)]->ty;
                total_words += (at && (at->is_slice || at->is_lambda)) ? 2 : 1;
            }
            // Build a flat word list mapping (which stash offset each word loads).
            // word_stash_off[w] = stash offset for word w; word_is_float[w], word_is_f64[w]
            std::vector<int32_t> word_stash_off;
            std::vector<bool> w_is_float, w_is_f64;
            word_stash_off.push_back(0); w_is_float.push_back(false); w_is_f64.push_back(false);  // env_ptr
            for (int i = 0; i < nuser; ++i) {
                const Type* at = c->args[size_t(i)]->ty;
                int32_t base = 8 + i * 8;
                if (at && (at->is_slice || at->is_lambda)) {
                    word_stash_off.push_back(base);     w_is_float.push_back(false); w_is_f64.push_back(false);
                    word_stash_off.push_back(base + 8); w_is_float.push_back(false); w_is_f64.push_back(false);
                } else if (at && at->is_float()) {
                    word_stash_off.push_back(base); w_is_float.push_back(true); w_is_f64.push_back(at->prim == Prim::F64);
                } else {
                    word_stash_off.push_back(base); w_is_float.push_back(false); w_is_f64.push_back(false);
                }
            }
            int nwords = int(word_stash_off.size());
            (void)total_words;
            // reload the slot into rax, run the provenance guard BEFORE placing
            // args in registers (the guard clobbers rcx + r11; args are stashed
            // on the stack + reloaded into regs AFTER the guard, like the normal
            // indirect-call path).
            emit_depth_check();
            e.load_reg_mem(Reg::rax, Reg::rsp, slot_scratch_off);  // reload slot
            emit_call_target_guard();
            // Red 6: keyed same-module lambda call resolution. After the guard,
            // rax = the logical slot (survives the guard). Resolve it through
            // ember_resolve_keyed_dispatch(record, slot, r15) → r11 = entry.
            const bool keyed_lambda =
                ctx.keyed_dispatch && ctx.keyed_dispatch->module_record;
            if (keyed_lambda) {
                emit_keyed_resolve(0, /*logical_slot_in_rax=*/true);
            }
            // place words 0..3 into registers. word 0 = env_ptr (always int).
            static const Reg int_regs[4] = {Reg::rcx, Reg::rdx, Reg::r8, Reg::r9};
            static const Xmm flt_regs[4] = {Xmm::xmm0, Xmm::xmm1, Xmm::xmm2, Xmm::xmm3};
            for (int w = 0; w < nwords && w < 4; ++w) {
                if (w_is_float[size_t(w)]) {
                    if (w_is_f64[size_t(w)]) e.movsd_xmm_mem(flt_regs[w], Reg::rsp, word_stash_off[size_t(w)]);
                    else e.movss_xmm_mem(flt_regs[w], Reg::rsp, word_stash_off[size_t(w)]);
                } else {
                    e.load_reg_mem(int_regs[w], Reg::rsp, word_stash_off[size_t(w)]);
                }
            }
            // words 4+ -> outgoing stack args
            for (int w = 4; w < nwords; ++w) {
                int32_t dst = slot_scratch_off + 8 + 32 + (w - 4) * 8;
                if (w_is_float[size_t(w)]) {
                    if (w_is_f64[size_t(w)]) { e.movsd_xmm_mem(Xmm::xmm4, Reg::rsp, word_stash_off[size_t(w)]); e.movsd_mem_xmm(Reg::rsp, dst, Xmm::xmm4); }
                    else { e.movss_xmm_mem(Xmm::xmm4, Reg::rsp, word_stash_off[size_t(w)]); e.movss_mem_xmm(Reg::rsp, dst, Xmm::xmm4); }
                } else {
                    e.load_reg_mem(Reg::rax, Reg::rsp, word_stash_off[size_t(w)]);
                    e.store_reg_mem(Reg::rsp, dst, Reg::rax);
                }
            }
            // r11 = [dispatch_base + slot*8]; call r11 (slot still in rax).
            // Red 6: keyed lambda — the entry is already in r11 (resolved above).
            if (keyed_lambda) {
                e.call_reg(Reg::r11);
            } else {
                e.mov_reg_imm64_external(Reg::r11, AbsFixup::DispatchTableBase);
                e.lea_reg_mem_sib(Reg::r11, Reg::r11, Reg::rax, 3);  // lea r11, [r11 + rax*8]
                e.load_reg_mem(Reg::r11, Reg::r11, 0);               // mov r11, [r11] (entry)
                e.call_reg(Reg::r11);
            }"""

CG_NEW = """            // We need the slot (rax) + env_ptr (rdx) to survive the arg stash
            // + depth check. Stash both into the arg-temp region FIRST. The
            // stash layout uses CUMULATIVE per-argument word counts (a slice or
            // lambda arg occupies 2 words; a registered struct occupies
            // ceil(size/8) words — see words_for_type), NOT c->args.size(), so a
            // multiword argument never overlaps the next argument or the saved
            // logical slot. This mirrors the normal call path's
            // add_operand/next_slot scheme exactly (proven correct for mixed
            // GP/XMM, slice, lambda, struct, and stack-passed args):
            //   [rsp+0 .. stash_size)     = arg words (env_ptr word 0, then each
            //                               user arg at its cumulative word slot)
            //   [rsp+stash_size]          = slot scratch (in the Win64 shadow's
            //                               first 8 bytes; reloaded before call)
            //   [rsp+stash_size+32 ..+out] = 32-byte shadow, then outgoing
            //                               stack args (words 4+)
            struct LOperand { const Expr* e; const Type* ty; int32_t slot0; int words; bool is_struct; };
            std::vector<LOperand> lops;
            int32_t next_slot = 0;
            // env_ptr is word 0 (the hidden __env param, always 1 integer word).
            lops.push_back({nullptr, nullptr, next_slot, 1, false});
            next_slot += 1;
            auto add_loperand = [&](const Expr* oe) {
                const Type* t = oe->ty;
                int w = words_for_type(t, ctx.structs);
                bool is_struct = t && !t->struct_name.empty() && ctx.structs && ctx.structs->count(t->struct_name) != 0;
                lops.push_back({oe, t, next_slot, w, is_struct});
                next_slot += w;
            };
            for (auto& a : c->args) add_loperand(a.get());
            int n = next_slot;                  // total physical word count
            int32_t stash_size = n * 8;         // env_ptr + all user arg words
            // The slot scratch lives in the FIRST 8 bytes of the Win64 shadow
            // space (immediately past the arg stash), NOT as a separate word
            // between the stash and the shadow. A separate scratch word would
            // shift the outgoing stack args by 8 bytes relative to where the
            // CALLEE's param-spill reads them (spill_word expects outgoing
            // args at [rbp+48+stash_size+(w-4)*8] = caller rsp + stash_size +
            // 32 + (w-4)*8, i.e. shadow immediately after the stash, outgoing
            // immediately after the 32-byte shadow). The slot is reloaded
            // BEFORE the call (for the guard + keyed resolve), so the callee
            // reusing its shadow after the call never clobbers a live value.
            int32_t slot_off = stash_size;      // slot scratch (shadow byte 0)
            int32_t outgoing = std::max(0, n - 4) * 8;
            int32_t total = round16(stash_size + 32 + outgoing);
            e.sub_reg_imm32(Reg::rsp, total);
            // stash env_ptr at [rsp+0], slot at [rsp+slot_off]
            e.store_reg_mem(Reg::rsp, 0, Reg::rdx);                // env_ptr -> word 0
            e.store_reg_mem(Reg::rsp, slot_off, Reg::rax);         // slot -> scratch
            // stash each user arg operand to its cumulative word slot(s)
            for (size_t li = 1; li < lops.size(); ++li) {
                const auto& op = lops[li];
                int32_t off = op.slot0 * 8;
                if (op.is_struct) {
                    auto sit = (op.ty && !op.ty->struct_name.empty() && ctx.structs)
                               ? ctx.structs->find(op.ty->struct_name) : ctx.structs->end();
                    if (sit != ctx.structs->end())
                        stash_struct_arg(op.e, op.ty, off, sit->second);
                } else {
                    eval(*op.e);
                    if (op.words == 2) {
                        // slice/lambda: rax=word0, rdx=word1 (the slice ABI)
                        e.store_reg_mem(Reg::rsp, off, Reg::rax);
                        e.store_reg_mem(Reg::rsp, off + 8, Reg::rdx);
                    } else if (op.ty && op.ty->is_float()) {
                        if (op.ty->prim == Prim::F64) e.movsd_mem_xmm(Reg::rsp, off, Xmm::xmm0);
                        else e.movss_mem_xmm(Reg::rsp, off, Xmm::xmm0);
                    } else {
                        e.store_reg_mem(Reg::rsp, off, Reg::rax);
                    }
                }
            }
            // per-word float width (env_ptr/slice/struct words are always int)
            std::vector<bool> word_is_float(size_t(n), false), word_is_f64(size_t(n), false);
            for (const auto& op : lops)
                if (op.words == 1 && !op.is_struct && op.ty && op.ty->is_float()) {
                    word_is_float[size_t(op.slot0)] = true;
                    word_is_f64[size_t(op.slot0)] = op.ty->prim == Prim::F64;
                }
            // reload the slot into rax, run the provenance guard BEFORE placing
            // args in registers (the guard clobbers rcx + r11; args are stashed
            // on the stack + reloaded into regs AFTER the guard, like the normal
            // indirect-call path).
            emit_depth_check();
            e.load_reg_mem(Reg::rax, Reg::rsp, slot_off);          // reload slot
            emit_call_target_guard();
            // Red 6: keyed same-module lambda call resolution. After the guard,
            // rax = the logical slot (survives the guard). Resolve it through
            // ember_resolve_keyed_dispatch(record, slot, r15) → r11 = entry.
            const bool keyed_lambda =
                ctx.keyed_dispatch && ctx.keyed_dispatch->module_record;
            if (keyed_lambda) {
                emit_keyed_resolve(0, /*logical_slot_in_rax=*/true);
            }
            // place words 0..3 into registers. word 0 = env_ptr (always int).
            static const Reg int_regs[4] = {Reg::rcx, Reg::rdx, Reg::r8, Reg::r9};
            static const Xmm flt_regs[4] = {Xmm::xmm0, Xmm::xmm1, Xmm::xmm2, Xmm::xmm3};
            for (int w = 0; w < n && w < 4; ++w) {
                int32_t off = w * 8;
                if (word_is_float[size_t(w)]) {
                    if (word_is_f64[size_t(w)]) e.movsd_xmm_mem(flt_regs[w], Reg::rsp, off);
                    else e.movss_xmm_mem(flt_regs[w], Reg::rsp, off);
                } else {
                    e.load_reg_mem(int_regs[w], Reg::rsp, off);
                }
            }
            // words 4+ -> outgoing stack args (immediately after the 32-byte
            // shadow, so the callee's param-spill reads them at the Win64-
            // standard [rbp+48+stash_size+(w-4)*8] offset)
            for (int w = 4; w < n; ++w) {
                int32_t src = w * 8;
                int32_t dst = stash_size + 32 + (w - 4) * 8;
                if (word_is_float[size_t(w)]) {
                    if (word_is_f64[size_t(w)]) { e.movsd_xmm_mem(Xmm::xmm4, Reg::rsp, src); e.movsd_mem_xmm(Reg::rsp, dst, Xmm::xmm4); }
                    else { e.movss_xmm_mem(Xmm::xmm4, Reg::rsp, src); e.movss_mem_xmm(Reg::rsp, dst, Xmm::xmm4); }
                } else {
                    e.load_reg_mem(Reg::rax, Reg::rsp, src);
                    e.store_reg_mem(Reg::rsp, dst, Reg::rax);
                }
            }
            // r11 = [dispatch_base + slot*8]; call r11.
            // Red 6: keyed lambda — the entry is already in r11 (resolved above,
            // before arg placement clobbered rax). Legacy lambda — RELOAD the
            // slot into rax here (the outgoing-args loop above used rax as a
            // scratch copy register, so the slot reloaded for the guard is no
            // longer live in rax). The slot is still intact at [rsp+slot_off]
            // (the shadow's first 8 bytes; no arg/outgoing store touches it).
            if (keyed_lambda) {
                e.call_reg(Reg::r11);
            } else {
                e.load_reg_mem(Reg::rax, Reg::rsp, slot_off);   // reload slot (rax was scratched)
                e.mov_reg_imm64_external(Reg::r11, AbsFixup::DispatchTableBase);
                e.lea_reg_mem_sib(Reg::r11, Reg::r11, Reg::rax, 3);  // lea r11, [r11 + rax*8]
                e.load_reg_mem(Reg::r11, Reg::r11, 0);               // mov r11, [r11] (entry)
                e.call_reg(Reg::r11);
            }"""

# ── 2. thin_emit.cpp: handle_word ───────────────────────────────────────────
TE_OLD = """    // `lea rax, [rbp+hidden_dest_off]`). For regular calls both are 0 and
    // `ret_struct` is false. The handle word (for indirect calls) is at
    // [rsp+stash_size] if `is_indirect`.
    void emit_call_arg_stash(const ThinInstr& in, std::vector<CallArg>& ops,
                             bool ret_struct, VReg hidden_dest_vreg,
                             int32_t hidden_dest_off, bool is_indirect) {
        int n = 0;
        for (auto& op : ops) n += op.words;
        if (ret_struct) n += 1;  // word 0 = hidden dest ptr
        int32_t stash_size = n * 8;
        int32_t handle_word = is_indirect ? 8 : 0;
        int32_t outgoing = std::max(0, n - 4) * 8;
        int32_t total = round16(stash_size + handle_word + 32 + outgoing);"""

TE_NEW = """    // `lea rax, [rbp+hidden_dest_off]`). For regular calls both are 0 and
    // `ret_struct` is false. The handle word (for indirect calls) is stashed at
    // [rsp+stash_size] (the first 8 bytes of the Win64 shadow space) if
    // `is_indirect`.
    void emit_call_arg_stash(const ThinInstr& in, std::vector<CallArg>& ops,
                             bool ret_struct, VReg hidden_dest_vreg,
                             int32_t hidden_dest_off, bool is_indirect) {
        int n = 0;
        for (auto& op : ops) n += op.words;
        if (ret_struct) n += 1;  // word 0 = hidden dest ptr
        int32_t stash_size = n * 8;
        // The indirect-call handle lives in the FIRST 8 bytes of the Win64
        // shadow space ([rsp+stash_size]), NOT as a separate word between the
        // stash and the shadow. A separate handle word would shift outgoing
        // stack args by 8 bytes relative to where the callee's param-spill
        // reads them (emit_param_spills expects outgoing at [rbp+48+stash_size+
        // (w-4)*8] = caller rsp + stash_size + 32 + (w-4)*8, i.e. shadow
        // immediately after the stash, outgoing immediately after the 32-byte
        // shadow). The handle is reloaded BEFORE the call (for the keyed
        // resolve or the lea+load dispatch), so the callee reusing its shadow
        // after the call never clobbers a live value. handle_word stays 0 so
        // `total` + the outgoing-arg offsets match that callee expectation.
        int32_t handle_word = 0;
        int32_t outgoing = std::max(0, n - 4) * 8;
        int32_t total = round16(stash_size + handle_word + 32 + outgoing);"""

# ── 3. module_layout.cpp: GNU asm guard ─────────────────────────────────────
ML_OLD = "#if defined(__x86_64__) || defined(_M_X64)\nextern \"C\" int64_t ember_keyed_padding_trap_r14(ember::context_t* /*unused*/) noexcept;\nasm("
ML_NEW = "#if defined(__GNUC__) && defined(__x86_64__)\nextern \"C\" int64_t ember_keyed_padding_trap_r14(ember::context_t* /*unused*/) noexcept;\nasm("

patch('src/codegen.cpp', CG_OLD, CG_NEW)
patch('src/thin_emit.cpp', TE_OLD, TE_NEW)
patch('src/module_layout.cpp', ML_OLD, ML_NEW)

print("[patch] all done")
