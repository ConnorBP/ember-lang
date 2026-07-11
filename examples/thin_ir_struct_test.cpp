// thin_ir_struct_test — Stage A c1 struct-invariants pin.
//
// Builds a small ThinFunction BY HAND (no parser, no sema, no lowering — this
// is the CONTRACT chunk, not the lowering chunk) and asserts the data-structure
// invariants that c2 (lowering) will produce and c3 (emit) will consume:
//
//   - blocks is non-empty and blocks[0] is the entry block (id 0).
//   - every block has a well-formed terminator (TermKind != None).
//   - a Branch terminator carries a condition vreg + a true-target + a
//     false-target; a Return terminator carries a return vreg (0 = void).
//   - VReg 0 is the invalid/none sentinel (never a real operand).
//   - the frame plan's vreg-bearing fields are populated (params carry types
//     + absolute offsets).
//   - dump(const ThinFunction&) returns a NON-EMPTY string that contains the
//     function name (so tests/debug can grep the dump).
//
// The fib-shaped function built here:
//   fn fib(n: i64, accum: i64) -> i64 {       // 2 params
//     if (n <= 1) return accum;               // Branch (false-target = recurse)
//     return fib(n - 1, n + accum);           // Add + CallScript + Return
//   }
//
// lowered (hand-built, representative — NOT a real lowering impl) to:
//   bb0:
//     %c1 = Cmp  %n, #1, Le                  // n <= 1  -> bool
//     Branch %c1 ? bb1 : bb2
//   bb1:                                      // base case
//     Return %accum
//   bb2:                                      // recursive case
//     %nm1   = Sub  %n, #1                    // n - 1
//     %nplus = Add  %accum, #n(immediate via src2+imm)  // n + accum
//     %r     = CallScript slot=0 args=[%nm1, %nplus] ret:i64
//     Return %r
//
// Links only the core `ember` lib (thin_ir.cpp lives there; dump() is the only
// symbol this test calls, and it touches no frontend code). Independent of the
// extension refactor and of F2 — a pure data-structure pin.

#include "../src/thin_ir.hpp"
#include "../src/ast.hpp"   // Type / Prim (aggregate; data-member access only)

#include <cstdio>
#include <string>
#include <vector>

using namespace ember;

// Local type objects (NOT the type_i64()/type_bool() singletons — those live
// in types.cpp / ember_frontend, and this test links only the core `ember` lib
// to stay disjoint from the frontend, matching thin_ir.cpp's own dependency
// discipline). Type is an aggregate with public members; we set prim and use
// the address. No Type METHOD is called anywhere in this test (those also live
// in ember_frontend), so it links against `ember` alone.

static int g_fail = 0;
static void ck(bool c, const char* m) {
    std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail = 1;
}

int main() {
    // ---- type objects (local; addresses are the vreg-type pointers) ----
    Type ty_i64;   ty_i64.prim   = Prim::I64;
    Type ty_bool;  ty_bool.prim  = Prim::Bool;
    const Type* p_i64  = &ty_i64;
    const Type* p_bool = &ty_bool;

    // ---- build a fib-shaped ThinFunction by hand ----
    ThinFunction f;
    f.name = "fib";
    f.slot = 0;
    f.ret_type = p_i64;

    // Frame plan: 2 i64 params + an i64 local would live here in a real lower.
    // Pin the absolute-offset convention (rbp-negative) so c3 can use verbatim.
    f.frame.frame_size = 48;            // round16(8 rbx + 16 params + slack)
    f.frame.rbx_save_offset = -8;       // fixed -8 (Item E)
    f.frame.struct_ret_ptr_offset = 0;  // not a struct-returning fn
    f.frame.arg_temps_base = -32;
    f.frame.next_local_off = 24;
    f.frame.returns_struct_by_ptr = false;
    f.frame.params.push_back({"n",     p_i64, -16, 0, 1});
    f.frame.params.push_back({"accum", p_i64, -24, 1, 1});

    // VReg allocation (naive TAC, NOT single-assignment — but this fn is
    // straight-line enough that each value gets its own vreg):
    //   v1 = n      (param 0)
    //   v2 = accum  (param 1)
    //   v3 = cmp result (bool)
    //   v4 = n - 1
    //   v5 = n + accum
    //   v6 = call result (i64)
    // VReg 0 stays invalid (the sentinel).
    const VReg v_n      = 1;
    const VReg v_accum  = 2;
    const VReg v_cmp    = 3;
    const VReg v_nm1    = 4;
    const VReg v_nplus  = 5;
    const VReg v_ret    = 6;

    // bb0: entry — compare n <= 1, branch.
    ThinBlock bb0;
    bb0.id = 0;
    {
        ThinInstr cmp;
        cmp.op = ThinOp::Cmp;
        cmp.dst = v_cmp;
        cmp.src1 = v_n;
        cmp.src2 = 0;            // immediate 1 carried in imm.i (a real lower
                                 // might use a ConstInt for the literal; either
                                 // is a legal thin-IR shape — Cmp reads src2 OR
                                 // imm.i. Here we exercise the imm form.)
        cmp.imm.i = 1;
        cmp.meta.cmp = 2;        // Lt is index 2 in (Eq,Neq,Lt,Le,Gt,Ge)
        cmp.meta.type = p_bool;
        cmp.loc = {1, 1};
        bb0.instrs.push_back(std::move(cmp));
    }
    bb0.term.kind = TermKind::Branch;
    bb0.term.cond = v_cmp;
    bb0.term.target = 1;         // true  -> bb1 (base case)
    bb0.term.false_target = 2;   // false -> bb2 (recurse)
    f.blocks.push_back(std::move(bb0));

    // bb1: base case — return accum.
    ThinBlock bb1;
    bb1.id = 1;
    bb1.term.kind = TermKind::Return;
    bb1.term.ret = v_accum;
    f.blocks.push_back(std::move(bb1));

    // bb2: recursive case — n-1, n+accum, call fib, return.
    ThinBlock bb2;
    bb2.id = 2;
    {
        ThinInstr sub;
        sub.op = ThinOp::Sub;
        sub.dst = v_nm1;
        sub.src1 = v_n;
        sub.imm.i = 1;           // n - 1 (imm form)
        sub.meta.width = 8;
        sub.meta.type = p_i64;
        sub.loc = {3, 12};
        bb2.instrs.push_back(std::move(sub));
    }
    {
        ThinInstr add;
        add.op = ThinOp::Add;
        add.dst = v_nplus;
        add.src1 = v_accum;
        add.src2 = v_n;          // n + accum (three-address form)
        add.meta.width = 8;
        add.meta.type = p_i64;
        add.loc = {3, 19};
        bb2.instrs.push_back(std::move(add));
    }
    {
        ThinInstr call;
        call.op = ThinOp::CallScript;
        call.dst = v_ret;
        call.meta.slot = 0;            // fib's own dispatch slot (recursive)
        call.args = {v_nm1, v_nplus};  // arg vregs IN ORDER
        call.arg_frame_offs = {-1, -1}; // plain vregs (not struct-by-value)
        call.arg_types = {p_i64, p_i64};
        call.ret_type = p_i64;
        call.loc = {3, 5};
        bb2.instrs.push_back(std::move(call));
    }
    bb2.term.kind = TermKind::Return;
    bb2.term.ret = v_ret;
    f.blocks.push_back(std::move(bb2));

    // ---- assert the struct invariants ----
    ck(!f.blocks.empty(), "blocks non-empty");
    ck(!f.name.empty(), "function name non-empty");

    // blocks[0] is entry (id 0)
    ck(f.blocks.size() >= 1 && f.blocks[0].id == 0, "blocks[0] is entry (id 0)");

    // every block has a well-formed terminator (TermKind != None)
    bool all_terminated = true;
    for (const auto& b : f.blocks)
        if (b.term.kind == TermKind::None) all_terminated = false;
    ck(all_terminated, "every block has TermKind != None");

    // the Branch carries cond + true/false targets; the Returns carry a ret vreg
    ck(f.blocks[0].term.kind == TermKind::Branch, "bb0 term is Branch");
    ck(f.blocks[0].term.cond != 0, "Branch cond vreg is non-zero (valid)");
    ck(f.blocks[0].term.target == 1 && f.blocks[0].term.false_target == 2,
       "Branch targets are bb1 (true) / bb2 (false)");
    ck(f.blocks[1].term.kind == TermKind::Return && f.blocks[1].term.ret == v_accum,
       "bb1 term is Return(accum)");
    ck(f.blocks[2].term.kind == TermKind::Return && f.blocks[2].term.ret == v_ret,
       "bb2 term is Return(call-result)");

    // VReg 0 is the invalid/none sentinel: no instr uses it as a real operand
    // (src2==0 above means "unused field", the documented sentinel meaning).
    bool no_v0_operand = true;
    for (const auto& b : f.blocks) {
        for (const auto& in : b.instrs) {
            // dst is never 0 (a real def); src1/src2 may be 0 = "unused".
            if (in.dst == 0) no_v0_operand = false;
            for (VReg a : in.args) if (a == 0) no_v0_operand = false;
        }
    }
    ck(no_v0_operand, "no instr defines/uses VReg 0 as a real operand (sentinel)");

    // vreg types populated: the cmp yields bool, the call yields i64
    ck(f.blocks[0].instrs[0].meta.type == p_bool, "Cmp result type is bool");
    ck(f.blocks[2].instrs[2].ret_type == p_i64,   "CallScript ret type is i64");
    ck(f.blocks[2].instrs[2].args.size() == 2,          "CallScript has 2 arg vregs");

    // frame plan: 2 params with absolute rbp-negative offsets + types
    ck(f.frame.params.size() == 2, "frame plan has 2 params");
    ck(f.frame.params[0].off < 0 && f.frame.params[1].off < 0,
       "param offsets are absolute rbp-negative");
    ck(f.frame.params[0].ty == p_i64 && f.frame.params[1].ty == p_i64,
       "param types populated");

    // dump() is non-empty and contains the function name
    std::string d = dump(f);
    ck(!d.empty(), "dump() returns a non-empty string");
    ck(d.find(f.name) != std::string::npos,
       "dump() contains the function name");

    // dump() also reflects the shape (op names + terminators) — a light sanity
    // check that the pretty-printer walked the structures, not a frozen format.
    ck(d.find("Cmp") != std::string::npos,      "dump shows the Cmp instr");
    ck(d.find("CallScript") != std::string::npos, "dump shows the CallScript instr");
    ck(d.find("Branch") != std::string::npos,   "dump shows the Branch terminator");
    ck(d.find("Return") != std::string::npos,   "dump shows a Return terminator");

    if (g_fail) {
        std::printf("\n--- dump ---\n%s-------------\n", d.c_str());
        return 1;
    }
    std::printf("\nthin_ir_struct: all invariants hold\n");
    return 0;
}
