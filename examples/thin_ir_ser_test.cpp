// thin_ir_ser_test.cpp — Stage B c1b: the IR serializer/deserializer unit test.
//
// Pins the ir_blob round-trip: hand-build a small ThinFunction (no
// parser/sema/lowering), serialize it, deserialize it, and assert structural
// equality. Then build a SECOND ThinFunction via the real lowering path
// (lower_function on a parsed+sema'd fib) and assert its round-trip too —
// this proves the serializer works on real lowered IR, not just hand-built
// fixtures. Finally, malformed-blob rejection: bad magic, truncated, invalid
// ThinOp ordinal, out-of-range VReg — each must return false with no partial
// ThinFunction published.
//
// Links the core `ember` lib (thin_ir_ser.cpp / em_type_codec.cpp / thin_ir.cpp
// live there) + ember_frontend (for the lower_function path). Modeled on
// thin_ir_struct_test (the hand-built struct pin) + thin_ir_test (the lowered
// value-equivalence gate).

#include "../src/thin_ir.hpp"
#include "../src/thin_ir_ser.hpp"
#include "../src/thin_lower.hpp"   // lower_function
#include "../src/thin_emit.hpp"    // emit_x64 (for the lowered round-trip)
#include "../src/codegen.hpp"      // CodeGenCtx, compile_func, finalize
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/dispatch_table.hpp"
#include "../src/jit_memory.hpp"
#include "../src/engine.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

static int failures = 0;
static void check(bool ok, const char* msg) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok) failures++;
}

// ─── Part 1: hand-built ThinFunction round-trip ───
//
// Build a tiny function by hand:
//   fn add_one(x: i64) -> i64 { return x + 1; }
// Two instrs in one block: ConstInt(1) -> v2; Add(v1, v2) -> v3; Return v3.
// This exercises: ConstInt, Add, Return, one param, no rodata, no natives.

static ThinFunction build_hand_thinfn() {
    ThinFunction thf;
    thf.name = "add_one";
    thf.slot = 3;
    thf.ret_type = nullptr;  // i64 — use a real Type
    // Make ret_type a real i64 Type in owned_types.
    auto i64_ty = std::make_shared<Type>();
    i64_ty->prim = Prim::I64;
    thf.ret_type = i64_ty.get();
    thf.owned_types.push_back(std::move(i64_ty));

    // Frame plan (minimal: just the rbx save slot).
    thf.frame.frame_size = 16;
    thf.frame.rbx_save_offset = -8;
    thf.frame.struct_ret_ptr_offset = 0;
    thf.frame.arg_temps_base = 0;
    thf.frame.next_local_off = 8;
    thf.frame.returns_struct_by_ptr = false;
    // One param: x: i64 at frame off -16, word 0, 1 word.
    auto p_i64 = std::make_shared<Type>();
    p_i64->prim = Prim::I64;
    ThinFramePlan::ParamSpill p;
    p.name = "x";
    p.ty = p_i64.get();
    p.off = -16;
    p.word0 = 0;
    p.nwords = 1;
    thf.frame.params.push_back(std::move(p));
    thf.owned_types.push_back(std::move(p_i64));

    // Block 0: entry
    ThinBlock blk0;
    blk0.id = 0;
    // v1 = x (param, frame off -16); v2 = ConstInt(1); v3 = Add(v1, v2)
    ThinInstr c1;
    c1.op = ThinOp::ConstInt;
    c1.dst = 2;
    c1.imm.i = 1;
    c1.meta.width = 8;
    blk0.instrs.push_back(c1);

    ThinInstr add;
    add.op = ThinOp::Add;
    add.dst = 3;
    add.src1 = 1;  // param x
    add.src2 = 2;  // const 1
    add.meta.width = 8;
    blk0.instrs.push_back(add);

    blk0.term.kind = TermKind::Return;
    blk0.term.ret = 3;
    thf.blocks.push_back(std::move(blk0));

    thf.non_serializable = false;
    return thf;
}

// Structural equality of two ThinFunctions (the fields the serializer
// round-trips). Compares op, dst/src1/src2, imm, key meta fields, term, and
// block count. Does NOT compare owned_types (they're reconstruction storage)
// or abs_fixups (empty at serialize time) or native_fn (rebound by name).
static bool thinfn_equal(const ThinFunction& a, const ThinFunction& b) {
    if (a.slot != b.slot) return false;
    if (a.name != b.name) return false;
    if (a.blocks.size() != b.blocks.size()) return false;
    for (size_t bi = 0; bi < a.blocks.size(); ++bi) {
        const auto& ba = a.blocks[bi];
        const auto& bb = b.blocks[bi];
        if (ba.id != bb.id) return false;
        if (ba.instrs.size() != bb.instrs.size()) return false;
        for (size_t ii = 0; ii < ba.instrs.size(); ++ii) {
            const auto& ia = ba.instrs[ii];
            const auto& ib = bb.instrs[ii];
            if (ia.op != ib.op) return false;
            if (ia.dst != ib.dst || ia.src1 != ib.src1 || ia.src2 != ib.src2) return false;
            if (ia.imm.i != ib.imm.i) return false;
            if (ia.meta.width != ib.meta.width) return false;
            if (ia.meta.frame_off != ib.meta.frame_off) return false;
            if (ia.meta.native_name != ib.meta.native_name) return false;
            if (ia.args.size() != ib.args.size()) return false;
            for (size_t k = 0; k < ia.args.size(); ++k)
                if (ia.args[k] != ib.args[k]) return false;
        }
        if (ba.term.kind != bb.term.kind) return false;
        if (ba.term.cond != bb.term.cond) return false;
        if (ba.term.target != bb.term.target) return false;
        if (ba.term.false_target != bb.term.false_target) return false;
        if (ba.term.ret != bb.term.ret) return false;
    }
    if (a.frame.frame_size != b.frame.frame_size) return false;
    if (a.frame.params.size() != b.frame.params.size()) return false;
    for (size_t i = 0; i < a.frame.params.size(); ++i) {
        if (a.frame.params[i].name != b.frame.params[i].name) return false;
        if (a.frame.params[i].off != b.frame.params[i].off) return false;
        if (a.frame.params[i].nwords != b.frame.params[i].nwords) return false;
    }
    if (a.rodata != b.rodata) return false;
    return true;
}

// ─── Part 2: real lowered ThinFunction round-trip ───
//
// Parse + sema + lower_function a real fib, serialize the ThinFunction,
// deserialize it, validate it, re-emit via emit_x64, and assert the re-emitted
// code executes correctly (fib(10) == 55). This proves the serializer works
// on real lowered IR AND that the deserialized IR is re-emittable.

static bool lowered_roundtrip() {
    const std::string src =
        "fn fib(n: i64) -> i64 { if (n <= 1) { return n; } return fib(n - 1) + fib(n - 2); }\n";
    auto lr = tokenize(src, "<ser_test>");
    if (!lr.ok) { std::printf("  lex fail\n"); return false; }
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) { std::printf("  parse fail\n"); return false; }
    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : pr.program.funcs) { slots[fn.name] = si++; fn.slot = slots[fn.name]; }
    std::unordered_map<std::string, NativeSig> natives;
    auto layouts = build_struct_layouts(pr.program);
    auto sr = sema(pr.program, natives, slots, 0, nullptr, &layouts);
    if (!sr.ok) { std::printf("  sema fail\n"); return false; }

    // Lower to ThinFunction (the IR path).
    CodeGenCtx ctx;
    ctx.natives = &natives;
    ctx.script_slots = &slots;
    ctx.structs = &layouts;
    ctx.enable_ir_backend = true;
    ThinFunction thf = lower_function(pr.program.funcs[0], ctx);
    if (thf.blocks.empty()) { std::printf("  lower gave empty blocks\n"); return false; }

    // Serialize.
    std::vector<uint8_t> blob;
    std::string serr;
    if (!serialize_thin_function(thf, blob, &serr)) {
        std::printf("  serialize fail: %s\n", serr.c_str());
        return false;
    }
    if (blob.empty()) { std::printf("  empty blob\n"); return false; }

    // Deserialize.
    ThinFunction thf2;
    const uint8_t* cur = blob.data();
    const uint8_t* end = blob.data() + blob.size();
    std::string derr;
    if (!deserialize_thin_function(cur, end, thf.name, thf.slot, thf2, &derr)) {
        std::printf("  deserialize fail: %s\n", derr.c_str());
        return false;
    }
    // Cursor should have consumed exactly the blob.
    if (cur != end) { std::printf("  deserialize did not consume full blob\n"); return false; }

    // Validate.
    std::string verr;
    if (!validate_thin_function(thf2, &verr)) {
        std::printf("  validate fail: %s\n", verr.c_str());
        return false;
    }

    // Structural equality (the IR round-trips).
    if (!thinfn_equal(thf, thf2)) {
        std::printf("  structural equality fail\n");
        return false;
    }

    // Re-emit the deserialized IR and execute it.
    DispatchTable table(1);
    ctx.dispatch_base = int64_t(table.base());
    ctx.globals_base = 0;
    CompiledFn cf = emit_x64(thf2, ctx);
    if (cf.bytes.empty()) { std::printf("  re-emit gave empty bytes\n"); return false; }
    if (!finalize(cf)) { std::printf("  finalize fail\n"); return false; }
    table.set(0, cf.entry);
    // fib(n:i64) takes n in rcx (the first Win64 arg reg). call_i64_i64_i64
    // passes (a,b) in (rcx,rdx); rdx is unused by fib, so the second arg is a
    // dummy. There is no 1-arg call helper in engine.hpp.
    int64_t result = call_i64_i64_i64(table.get(0), 10, 0);
    free_executable(cf.exec);
    if (result != 55) { std::printf("  fib(10) = %lld (expected 55)\n", (long long)result); return false; }

    return true;
}

// ─── Part 3: malformed-blob rejection ───

static bool malformed_rejection() {
    // Build a valid blob first, then corrupt it in several ways.
    auto thf = build_hand_thinfn();
    std::vector<uint8_t> blob;
    std::string err;
    if (!serialize_thin_function(thf, blob, &err)) return false;

    ThinFunction out;
    std::string derr;

    // (a) bad magic: flip the first byte.
    {
        auto bad = blob;
        bad[0] ^= 0xFF;
        const uint8_t* cur = bad.data();
        const uint8_t* end = bad.data() + bad.size();
        if (deserialize_thin_function(cur, end, "x", 0, out, &derr)) return false;
        if (derr.find("magic") == std::string::npos) return false;
    }
    // (b) truncated: cut the blob in half.
    {
        auto bad = blob;
        bad.resize(bad.size() / 2);
        const uint8_t* cur = bad.data();
        const uint8_t* end = bad.data() + bad.size();
        if (deserialize_thin_function(cur, end, "x", 0, out, &derr)) return false;
        // Should report a truncation error.
    }
    // (c) invalid ThinOp: corrupt the first instr's op to a value > CallTargetGuard.
    {
        // The op is at a known offset after the header + frame plan. Rather
        // than compute it, scan for the pattern: we know op=ConstInt(1)=0,
        // dst=2, src1=0, src2=0. Find the first u16==0 that's followed by
        // u32(2), u32(0), u32(0) and bump it to 0xFFFF.
        auto bad = blob;
        bool found = false;
        for (size_t i = 0; i + 18 <= bad.size(); ++i) {
            if (bad[i] == 0 && bad[i+1] == 0 &&
                bad[i+2] == 2 && bad[i+3] == 0 && bad[i+4] == 0 && bad[i+5] == 0 &&
                bad[i+6] == 0 && bad[i+7] == 0 && bad[i+8] == 0 && bad[i+9] == 0 &&
                bad[i+10] == 0 && bad[i+11] == 0 && bad[i+12] == 0 && bad[i+13] == 0) {
                // Set op to 0xFFFF (way past CallTargetGuard).
                bad[i] = 0xFF; bad[i+1] = 0xFF;
                found = true;
                break;
            }
        }
        if (!found) return false;  // test setup bug
        const uint8_t* cur = bad.data();
        const uint8_t* end = bad.data() + bad.size();
        if (deserialize_thin_function(cur, end, "x", 0, out, &derr)) return false;
        if (derr.find("ThinOp") == std::string::npos) return false;
    }
    // (d) out-of-range VReg: bump the Add's dst to a huge value, then validate.
    {
        auto thf_bad = build_hand_thinfn();
        // The Add instr (block 0, instr 1) has dst=3. Bump to 999999 so
        // validate_thin_function catches it (max_vreg will be 1000000, but
        // src1=1 which is < max_vreg, so this passes...). Instead, bump src1
        // to a value > max_vreg. max_vreg = max(2,3,999999)+1 = 1000000.
        // src1=1 < 1000000, so it passes. We need a VReg that's NOT in the
        // max computation. Use a different approach: add a block with a
        // bogus target.
        thf_bad.blocks[0].term.kind = TermKind::Jmp;
        thf_bad.blocks[0].term.target = 99;  // only 1 block
        thf_bad.blocks[0].term.ret = 0;
        std::string verr;
        if (validate_thin_function(thf_bad, &verr)) return false;
        if (verr.find("target") == std::string::npos) return false;
    }
    // (e) no terminator: clear block 0's term kind.
    {
        auto thf_bad = build_hand_thinfn();
        thf_bad.blocks[0].term.kind = TermKind::None;
        std::string verr;
        if (validate_thin_function(thf_bad, &verr)) return false;
        if (verr.find("terminator") == std::string::npos) return false;
    }
    // (f) empty blob (zero bytes).
    {
        std::vector<uint8_t> empty;
        const uint8_t* cur = empty.data();
        const uint8_t* end = empty.data();
        if (deserialize_thin_function(cur, end, "x", 0, out, &derr)) return false;
    }
    return true;
}

// ─── Part 4: validation edge cases (the audit-found checks) ───
//
// Each case builds a hand-constructed ThinFunction that is structurally
// deserializable (it never goes through the blob) but fails a specific
// validate_thin_function check. This pins each audit fix.

static bool validation_edge_cases() {
    auto base = build_hand_thinfn();  // a valid 1-block fn: ConstInt(1), Add, Return

    // C1: block.id out of range (>= num_blocks). emit_x64 uses blk.id as a
    // vector index — an OOB id is a heap OOB.
    {
        auto bad = base;
        bad.blocks[0].id = 5;  // only 1 block (id must be 0)
        std::string verr;
        if (validate_thin_function(bad, &verr)) return false;
        // blocks[0].id != 0 is caught by the entry-block check first.
        if (verr.find("id") == std::string::npos) return false;
    }
    // C1 variant: 2 blocks, second block has id=99 (>= 2).
    {
        auto bad = base;
        bad.blocks[0].term.kind = TermKind::Jmp;
        bad.blocks[0].term.target = 1;
        ThinBlock blk1;
        blk1.id = 99;  // should be 1
        blk1.term.kind = TermKind::Return;
        blk1.term.ret = 3;
        bad.blocks.push_back(blk1);
        std::string verr;
        if (validate_thin_function(bad, &verr)) return false;
        if (verr.find("id out of range") == std::string::npos) return false;
    }
    // C2: CallNative with empty native_name.
    {
        auto bad = base;
        ThinInstr call;
        call.op = ThinOp::CallNative;
        call.dst = 4;
        call.meta.native_name = "";  // empty — must be rejected
        call.ret_type = nullptr;
        bad.blocks[0].instrs.push_back(call);
        std::string verr;
        if (validate_thin_function(bad, &verr)) return false;
        if (verr.find("empty native_name") == std::string::npos) return false;
    }
    // P2: StringDecrypt with rodata OOB (addend + len > rodata.size()).
    {
        auto bad = base;
        bad.rodata.resize(4);  // 4 bytes of rodata
        ThinInstr sd;
        sd.op = ThinOp::StringDecrypt;
        sd.meta.addend = 2;
        sd.meta.len = 10;  // 2 + 10 = 12 > 4
        bad.blocks[0].instrs.push_back(sd);
        std::string verr;
        if (validate_thin_function(bad, &verr)) return false;
        if (verr.find("rodata") == std::string::npos) return false;
    }
    // P3: CallScript slot >= dispatch_size.
    {
        auto bad = base;
        ThinInstr cs;
        cs.op = ThinOp::CallScript;
        cs.dst = 4;
        cs.meta.slot = 99;  // dispatch_size will be 1
        bad.blocks[0].instrs.push_back(cs);
        std::string verr;
        if (validate_thin_function(bad, &verr, 1, 0)) return false;  // dispatch_size=1
        if (verr.find("slot out of range") == std::string::npos) return false;
    }
    // P3: CallCrossModule mod_id >= registry_size.
    {
        auto bad = base;
        ThinInstr cm;
        cm.op = ThinOp::CallCrossModule;
        cm.dst = 4;
        cm.meta.mod_id = 99;  // registry_size will be 1
        bad.blocks[0].instrs.push_back(cm);
        std::string verr;
        if (validate_thin_function(bad, &verr, 0, 1)) return false;  // registry_size=1
        if (verr.find("mod_id out of range") == std::string::npos) return false;
    }
    // P4: Cmp predicate out of range (> 5).
    {
        auto bad = base;
        ThinInstr cmp;
        cmp.op = ThinOp::Cmp;
        cmp.dst = 4;
        cmp.src1 = 1;
        cmp.src2 = 2;
        cmp.meta.cmp = 99;  // must be 0..5
        bad.blocks[0].instrs.push_back(cmp);
        std::string verr;
        if (validate_thin_function(bad, &verr)) return false;
        if (verr.find("predicate") == std::string::npos) return false;
    }
    // P7: frame_size out of range (> 1MB).
    {
        auto bad = base;
        bad.frame.frame_size = (1 << 21);  // 2MB > 1MB limit
        std::string verr;
        if (validate_thin_function(bad, &verr)) return false;
        if (verr.find("frame_size") == std::string::npos) return false;
    }
    // P7: rbx_save_offset out of range (>= 0, should be negative).
    {
        auto bad = base;
        bad.frame.rbx_save_offset = 8;  // positive — should be negative
        std::string verr;
        if (validate_thin_function(bad, &verr)) return false;
        if (verr.find("rbx_save_offset") == std::string::npos) return false;
    }
    // P1: VReg exceeds the declared max_vreg. Set declared_max_vreg to a small
    // value (e.g. 4) and add an instr with a VReg reference (dst=10) that
    // exceeds it. The recomputed max_vreg would be 11 (tautological), but the
    // declared bound (4) catches it.
    {
        auto bad = base;
        bad.declared_max_vreg = 4;  // only VRegs 1..3 are valid
        ThinInstr extra;
        extra.op = ThinOp::ConstInt;
        extra.dst = 10;  // 10 >= 4 — exceeds declared bound
        extra.imm.i = 0;
        extra.meta.width = 8;
        bad.blocks[0].instrs.push_back(extra);
        std::string verr;
        if (validate_thin_function(bad, &verr)) return false;
        if (verr.find("VReg out of range") == std::string::npos) return false;
    }
    // Item 11: negative len for ConstStringRef.
    {
        auto bad = base;
        bad.rodata.resize(16);
        ThinInstr sr;
        sr.op = ThinOp::ConstStringRef;
        sr.meta.addend = 0;
        sr.meta.len = -1;  // negative — must be rejected
        bad.blocks[0].instrs.push_back(sr);
        std::string verr;
        if (validate_thin_function(bad, &verr)) return false;
        if (verr.find("negative len") == std::string::npos) return false;
    }
    // Item 12h: negative len for BoundsCheck.
    {
        auto bad = base;
        ThinInstr bc;
        bc.op = ThinOp::BoundsCheck;
        bc.meta.len = -1;  // negative — disables the bounds check
        bad.blocks[0].instrs.push_back(bc);
        std::string verr;
        if (validate_thin_function(bad, &verr)) return false;
        if (verr.find("negative len") == std::string::npos) return false;
    }
    // Item 12a: frame_off out of frame bounds (positive — above rbp).
    {
        auto bad = base;
        ThinInstr lf;
        lf.op = ThinOp::LoadFrame;
        lf.dst = 4;
        lf.meta.frame_off = 100;  // positive — above rbp (return address area)
        lf.meta.width = 8;
        bad.blocks[0].instrs.push_back(lf);
        std::string verr;
        if (validate_thin_function(bad, &verr)) return false;
        if (verr.find("frame_off") == std::string::npos) return false;
    }
    // Item 12a: frame_off out of frame bounds (too negative — below frame).
    {
        auto bad = base;
        bad.frame.frame_size = 16;
        ThinInstr sf;
        sf.op = ThinOp::StoreFrame;
        sf.src1 = 1;
        sf.meta.frame_off = -100000;  // way below the 16-byte frame
        sf.meta.width = 8;
        bad.blocks[0].instrs.push_back(sf);
        std::string verr;
        if (validate_thin_function(bad, &verr)) return false;
        if (verr.find("frame_off") == std::string::npos) return false;
    }
    // Finding A (EM_FORMAT_RED_TEAM 2026-07-11): frame_size == 0 must NOT
    // bypass the frame_off bounds check. The prior validator gated the
    // frame_off check on `frame_size > 0`, so frame_size == 0 skipped it
    // entirely — a producing op with frame_off = +8 overwrote the return
    // address (the prologue always does `push rbp; mov rbp, rsp` regardless
    // of frame_size, so [rbp+8] = return addr). The fix: ALWAYS check
    // frame_off regardless of frame_size. When frame_size == 0, any non-zero
    // frame_off is rejected.
    //
    // Case A1: frame_size=0, frame_off=+8 (the return-address overwrite
    // primitive from the audit's PoC). MUST be rejected.
    {
        auto bad = base;
        bad.frame.frame_size = 0;  // zero frame — the bypass condition
        ThinInstr ci;
        ci.op = ThinOp::ConstInt;
        ci.dst = 4;
        ci.imm.i = 0x4141414142424242LL;  // the attacker's return addr
        ci.meta.frame_off = 8;  // +8 = return address slot ([rbp+8])
        ci.meta.width = 8;
        bad.blocks[0].instrs.push_back(ci);
        std::string verr;
        if (validate_thin_function(bad, &verr)) return false;
        if (verr.find("frame_off") == std::string::npos) return false;
    }
    // Case A2: frame_size=0, frame_off=-8 (negative, but frame_size is 0 so
    // there's no frame to write to — below rsp, unallocated stack). MUST be
    // rejected. This pins that the fix rejects ANY non-zero frame_off when
    // frame_size == 0, not just positive ones.
    {
        auto bad = base;
        bad.frame.frame_size = 0;
        ThinInstr sf;
        sf.op = ThinOp::StoreFrame;
        sf.src1 = 1;
        sf.meta.frame_off = -8;  // negative, but frame_size=0 so still invalid
        sf.meta.width = 8;
        bad.blocks[0].instrs.push_back(sf);
        std::string verr;
        if (validate_thin_function(bad, &verr)) return false;
        if (verr.find("frame_off") == std::string::npos) return false;
    }
    // Case A3 (contrast / negative control): frame_size=0 with NO non-zero
    // frame_off instrs must still validate clean (a leaf function with no
    // frame slots is legitimate — frame_size=0 alone is not malformed, only
    // frame_size=0 + a non-zero frame_off is).
    {
        auto good = base;
        good.frame.frame_size = 0;
        // base has ConstInt(1) + Add, neither uses frame_off (both are 0).
        // The param at off=-16 is a frame-plan offset, not an instr frame_off,
        // so it doesn't trigger the instr-level check. This should validate.
        std::string verr;
        if (!validate_thin_function(good, &verr)) return false;
    }
    // Positive: the base function validates clean (sanity check).
    {
        std::string verr;
        if (!validate_thin_function(base, &verr)) return false;
    }
    return true;
}

int main() {
    std::printf("=== thin_ir_ser_test: Stage B c1b IR serializer ===\n");

    // Part 1: hand-built round-trip.
    std::printf("Part 1: hand-built ThinFunction round-trip\n");
    {
        auto thf = build_hand_thinfn();
        std::vector<uint8_t> blob;
        std::string serr;
        check(serialize_thin_function(thf, blob, &serr), "serialize hand-built");
        check(!blob.empty(), "blob non-empty");

        ThinFunction thf2;
        const uint8_t* cur = blob.data();
        const uint8_t* end = blob.data() + blob.size();
        std::string derr;
        check(deserialize_thin_function(cur, end, thf.name, thf.slot, thf2, &derr),
              "deserialize hand-built");
        check(cur == end, "deserialize consumed full blob");
        std::string verr;
        check(validate_thin_function(thf2, &verr), "validate hand-built");
        check(thinfn_equal(thf, thf2), "structural equality (hand-built round-trip)");
    }

    // Part 2: real lowered round-trip.
    std::printf("Part 2: real lowered ThinFunction round-trip (fib)\n");
    check(lowered_roundtrip(), "lowered serialize+deserialize+validate+re-emit+execute (fib(10)==55)");

    // Part 3: malformed rejection.
    std::printf("Part 3: malformed-blob rejection\n");
    check(malformed_rejection(), "all malformed blobs rejected (bad magic, truncated, bad ThinOp, bad target, no terminator, empty)");

    // Part 4: validation edge cases (the audit-found checks).
    std::printf("Part 4: validation edge cases (C1/C2/P2/P3/P4/P7)\n");
    check(validation_edge_cases(), "all validation edge cases rejected (bad block.id, empty native_name, rodata OOB, slot OOB, mod_id OOB, bad cmp, bad frame_size, bad rbx_save_offset, VReg exceeds declared bound, negative len rodata, negative len BoundsCheck, frame_off OOB)");

    std::printf("\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
