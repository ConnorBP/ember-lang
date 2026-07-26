// x18_avoidance_test — verify the ARM64 JIT (emit_arm64) + the Darwin
// coroutine context switch (darwin_arm64_ctx_switch.S) NEVER reference x18.
//
// x18 is Apple's PLATFORM register (reserved for the OS thread TSD base on
// macOS/iOS). The JIT must NEVER allocate or touch x18 — corrupting it would
// break Apple runtime state. The regalloc pool excludes x18, but emit_arm64
// is frame-only (no regalloc) and hand-selects scratch regs (x9/x10/x11 +
// x19 as the context reg). This test scans the EMITTED BYTES of a
// representative JIT'd function (arithmetic, calls, loads/stores, control
// flow, struct access, returns) for any instruction whose register fields
// encode x18 (register 18) — in every common AArch64 instruction format the
// emitter uses — + asserts NONE found. It also assembles
// darwin_arm64_ctx_switch.S with clang and scans those bytes (the switch
// saves/restores x19-x28 + x29/x30 + SP; by design it does NOT touch x18 —
// this pins that invariant at the byte level).
//
// This is the SAFE option from audit M3 (a runtime probe that reads x18 is
// unsafe: x18 holds OS TSD state; even reading it is reserved, and a JIT'd
// fn that WROTE x18 would corrupt the platform register before any assertion
// could fire). The byte-scan is deterministic + needs no x18 access.
//
// Build & run (macOS arm64): built as a CTest (Apple-gated) in CMakeLists.txt.
#include "../src/thin_emit.hpp"       // emit_arm64
#include "../src/thin_lower.hpp"      // lower_function
#include "../src/thin_ir.hpp"
#include "../src/engine.hpp"          // CompiledFn, finalize, free_executable
#include "../src/dispatch_table.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/sema.hpp"
#include "../src/codegen.hpp"         // CodeGenCtx, g_globals_for_codegen
#include "../src/globals.hpp"
#include "../src/ast.hpp"
#include "../extensions/array/ext_array.hpp"
#include "../extensions/string/ext_string.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ember;

static int g_fail = 0;
static void ck(bool c, const char* m) {
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

// A non-null trap-stub placeholder (never called: the test only scans bytes,
// it does not RUN the JIT'd code). Baked as an imm64 into the trap marshaling
// so that instruction form is exercised; its address bits land in the movz/
// movk imm16 fields (not the register fields), so they do not collide with the
// x18 register-field scan.
extern "C" void x18_test_trap(ember::context_t*, int, const char*) {}

// ---------------------------------------------------------------------------
// Decode one 32-bit little-endian AArch64 instruction + return the set of
// register numbers (0-31) it references in its register operand fields.
// Covers the instruction formats the Arm64Emitter emits (data-processing
// shifted-register, move-wide, load/store + pairs, branch register, cbz/cbnz,
// conditional branch, fp fmov/arith). For an unrecognized opcode, the three
// common register field positions (Rd/Rt [4:0], Rn [9:5], Rm [20:16]) are
// checked conservatively (a rare immediate-only instruction with 18 in those
// exact bits would false-positive; the assertion below prints every hit so a
// false positive is identifiable + the decoder can be refined).
// ---------------------------------------------------------------------------
static void regs_in_insn(uint32_t w, std::vector<int>& out) {
    auto add = [&](int field) { if (field >= 0 && field <= 31) out.push_back(field); };
    uint32_t op0 = (w >> 24) & 0xFF;   // top byte (rough class)
    uint32_t op_hi = (w >> 22) & 0x3FF; // top 10 bits (load/store class)
    int rd  = int(w & 0x1F);
    int rn  = int((w >> 5) & 0x1F);
    int rm  = int((w >> 16) & 0x1F);
    int rt2 = int((w >> 10) & 0x1F);

    // Move-wide (movz/movk/movn): sf 00 100101 hw imm16 Rd  -> Rd only.
    if ((w & 0x1F800000) == 0x12800000 || (w & 0x1F800000) == 0x1A800000) {
        add(rd); return;
    }
    // Data-processing (shifted register): add/sub/orr/mov (shifted reg).
    // top bits 0x8B/0xCB/0xAA/0xEB etc. Rd[4:0], Rn[9:5], Rm[20:16].
    if ((w & 0x1F000000) == 0x0B000000) {  // data-processing (shifted reg)
        add(rd); add(rn); add(rm); return;
    }
    // Data-processing (immediate): add/sub imm (top 0x11/0x31...). Rd, Rn only.
    if ((w & 0x1F000000) == 0x11000000) { add(rd); add(rn); return; }
    // Load/store register (unsigned immediate): 11 111 0 01 01 size opc ...
    // Rt[4:0], Rn[9:5]. (0xF940xxxx = ldr64, 0xF900xxxx = str64, etc.)
    if ((w & 0x3B000000) == 0x39000000) { add(rd); add(rn); return; }
    // Load/store register (unscaled immediate / pair): ldp/stp/ldur/stur.
    // Pair: 10 1010 0_ etc. Rt[4:0], Rn[9:5], Rt2[14:10].
    if ((w & 0x3A000000) == 0x28000000) {  // load/store pair (various)
        add(rd); add(rn); add(rt2); return;
    }
    // Unconditional branch register: blr/br/ret (bits[31:10] == 0xD63F0 etc).
    if ((w & 0xFE000000) == 0xD6000000) { add(rn); return; }
    // Conditional branch (b.eq etc): uses Rt (the compared reg) at [4:0] + cond [15:12].
    if ((w & 0xFF000010) == 0x54000000) { add(int(w & 0x1F)); return; }
    // cbz/cbnz: sf 011 010 0 imm19 Rt[4:0].
    if ((w & 0x7E000000) == 0x34000000) { add(int(w & 0x1F)); return; }
    // tbz/tbnz: b5/b6... Rt[4:0].
    if ((w & 0x7E000000) == 0x36000000) { add(int(w & 0x1F)); return; }
    // FP load/store + fp arith (fmov/fadd/fcmp): v regs (0-31). The emitter
    // uses v0-v7 only; x18-as-a-vreg (v18) is also forbidden in JIT'd code.
    // Treat the same field positions as register refs (covers v regs too).
    if ((w & 0x1F000000) == 0x0E000000 || (w & 0xFF000000) == 0xFD000000 ||
        (w & 0xFF000000) == 0xFC000000 || (w & 0x1F200000) == 0x0EA00000) {
        add(rd); add(rn); add(rm); return;
    }
    // Unconditional immediate branch (b) / branch (no regs): skip.
    if ((w & 0xFC000000) == 0x14000000) return;
    if ((w & 0xFC000000) == 0x94000000) return;  // bl (no register operands)
    // adrp/adr: Rd only.
    if ((w & 0x1F000000) == 0x10000000) { add(rd); return; }
    // Conservative fallback for anything unrecognized: check the three common
    // register field positions so a new instruction form is not silently
    // skipped (a hit here is printed for review).
    add(rd); add(rn); add(rm);
}

// Scan a buffer of 4-byte AArch64 instructions for any reference to register
// 18 (x18/v18). Returns the offset of the first hit (or -1).
static int scan_for_x18(const uint8_t* bytes, size_t n, const char* who) {
    for (size_t off = 0; off + 4 <= n; off += 4) {
        uint32_t w = uint32_t(bytes[off])
                   | (uint32_t(bytes[off+1]) << 8)
                   | (uint32_t(bytes[off+2]) << 16)
                   | (uint32_t(bytes[off+3]) << 24);
        std::vector<int> regs;
        regs_in_insn(w, regs);
        for (int r : regs) {
            if (r == 18) {
                std::printf("[FAIL] %s: x18/v18 reference at byte offset %zu (insn 0x%08x)\n",
                            who, off, w);
                return int(off);
            }
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Compile a representative ember function through the full JIT pipeline
// (lex/parse/sema/lower_function/emit_arm64) + return its emitted bytes.
// The function exercises arithmetic, a script call, loads/stores, a loop,
// struct field access, + a return — so every common emit_arm64 instruction
// form is covered.
// ---------------------------------------------------------------------------
static bool compile_to_bytes(const std::string& src, std::vector<uint8_t>& out_bytes) {
    auto lr = tokenize(src, "<x18-scan>");
    if (!lr.ok) return false;
    auto pr = parse(std::move(lr.toks));
    if (!pr.ok) return false;
    Program prog = std::move(pr.program);
    std::unordered_map<std::string, int> slots;
    int si = 0;
    for (auto& fn : prog.funcs) { slots[fn.name] = si++; fn.slot = si - 1; }
    std::unordered_map<std::string, NativeSig> natives;
    ext_string::register_natives(natives);
    ext_array::register_natives(natives);
    OpOverloadTable ov;
    ext_string::register_overloads(ov);
    StructLayoutTable layouts = build_struct_layouts(prog);
    prog.string_xor_key = 0;
    auto sr = sema(prog, natives, slots, 0, &ov, &layouts);
    if (!sr.ok) return false;
    GlobalsBlock gb; gb.base = 0; g_globals_for_codegen = &gb;
    DispatchTable table(prog.funcs.size());
    CodeGenCtx ctx;
    ctx.globals_base = 0;
    ctx.dispatch_base = int64_t(table.base());
    ctx.natives = &natives;
    ctx.script_slots = &slots;
    ctx.structs = &layouts;
    ctx.use_context_reg = true;   // B1: x19 = context_t* (the reserved context reg)
    ctx.emit_budget_checks = true;
    ctx.emit_depth_checks = true;
    ctx.max_call_depth = 64;
    ctx.trap_stub = (void*)&x18_test_trap;  // non-null placeholder (bytes-only test)
    for (auto& fn : prog.funcs) {
        ThinFunction thf = lower_function(fn, ctx);
        if (thf.blocks.empty()) return false;
        CompiledFn cf = emit_arm64(thf, ctx);
        if (cf.bytes.empty()) return false;
        // Accumulate every fn's bytes (all are JIT'd code that must avoid x18).
        out_bytes.insert(out_bytes.end(), cf.bytes.begin(), cf.bytes.end());
    }
    return true;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== x18-avoidance scan (audit M3) ===\n\n");

    // (1) A representative JIT'd fn: arithmetic, a script call, a loop,
    //     loads/stores, struct field access, + a return. Compiles with
    //     safety ON (x19 = context reg, budget/depth checks, a trap stub) so
    //     the trap-stub marshaling + context-reg loads are exercised too.
    {
        const char* src =
            "struct Pair { a: i64; b: i64; }\n"
            "fn helper(n: i64) -> i64 { return n * 3 + 1; }\n"
            "fn main() -> i64 {\n"
            "    let mut s: i64 = 0;\n"
            "    let mut i: i64 = 0;\n"
            "    while (i < 50) { s = s + helper(i); i = i + 1; }\n"
            "    let p: Pair; p.a = s; p.b = s * 2;\n"
            "    return p.a + p.b;\n"
            "}\n";
        std::vector<uint8_t> bytes;
        bool ok = compile_to_bytes(src, bytes);
        ck(ok, "[1] representative JIT'd fn compiles (arithmetic/call/loop/struct)");
        if (ok) {
            int hit = scan_for_x18(bytes.data(), bytes.size(), "JIT'd fn");
            char msg[128];
            std::snprintf(msg, sizeof msg, "[1] JIT'd fn (%zu bytes) has NO x18/v18 reference", bytes.size());
            ck(hit < 0, msg);
        }
    }

    // (2) Assemble the Darwin coroutine context switch (the hand-written
    //     src/darwin_arm64_ctx_switch.S) with clang + scan the object bytes.
    //     The switch saves/restores x19-x28 + x29/x30 + SP; by design it does
    //     NOT touch x18 (Apple's platform register). This pins that invariant
    //     at the byte level so a future edit that accidentally saves x18 is
    //     caught. Skipped (not a FAIL) if clang is unavailable.
    {
        // Assemble just the .S to a temp .o, then read its __TEXT,__text bytes
        // with otool (the .S is #ifdef __APPLE__ so it assembles on this host).
        // otool -X suppresses the 2-line header; each remaining line is
        // `address hexbytes...` so sed strips the leading address column before
        // xxd -r -p turns the hex into raw bytes.
        const char* src_s = "../src/darwin_arm64_ctx_switch.S";
        FILE* p = popen(
            "clang -c -arch arm64 -x assembler ../src/darwin_arm64_ctx_switch.S "
            "-o /tmp/_x18_ctx_switch.o 2>/dev/null && "
            "/usr/bin/otool -X -s __TEXT __text /tmp/_x18_ctx_switch.o 2>/dev/null "
            "| sed 's/^[0-9a-f]* //' | /usr/bin/xxd -r -p 2>/dev/null",
            "r");
        if (!p) {
            std::printf("[SKIP] [2] ctx_switch scan (popen failed)\n");
        } else {
            std::vector<uint8_t> bytes;
            unsigned char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
                bytes.insert(bytes.end(), buf, buf + n);
            pclose(p);
            if (bytes.empty()) {
                std::printf("[SKIP] [2] ctx_switch scan (clang/otool/xxd unavailable or empty)\n");
            } else {
                int hit = scan_for_x18(bytes.data(), bytes.size(), "ctx_switch");
                char msg[128];
                std::snprintf(msg, sizeof msg, "[2] ctx_switch (%zu bytes) has NO x18 reference", bytes.size());
                ck(hit < 0, msg);
            }
        }
    }

    std::printf("\nx18_avoidance_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
