// Structural edge-path coverage for all optimization passes in ext_opt.cpp.
#include "thin_ir.hpp"
#include "ember_pass.hpp"
#include "ember_pass_registry.hpp"
#include "ext_opt.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace ember;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static ThinInstr ci(VReg d, int64_t v, int off = 0) {
    ThinInstr i; i.op = ThinOp::ConstInt; i.dst = d; i.imm.i = v;
    i.meta.type = &type_i64(); i.meta.width = 8; i.meta.frame_off = off; return i;
}
static ThinInstr cb(VReg d, bool v) {
    ThinInstr i; i.op = ThinOp::ConstBool; i.dst = d; i.imm.i = v;
    i.meta.type = &type_bool(); i.meta.width = 1; return i;
}
static ThinInstr bin(ThinOp op, VReg d, VReg a, VReg b, int off = 0) {
    ThinInstr i; i.op = op; i.dst = d; i.src1 = a; i.src2 = b;
    i.meta.type = &type_i64(); i.meta.width = 8; i.meta.frame_off = off; return i;
}
static ThinInstr move(VReg d, VReg s) {
    ThinInstr i; i.op = ThinOp::Move; i.dst = d; i.src1 = s; i.meta.type = &type_i64(); return i;
}
static ThinBlock block(uint32_t id, std::vector<ThinInstr> ins, ThinTerm t) {
    ThinBlock b; b.id = id; b.instrs = std::move(ins); b.term = t; return b;
}
static ThinTerm ret(VReg v = 0) { ThinTerm t; t.kind = TermKind::Return; t.ret = v; return t; }
static ThinTerm jmp(uint32_t x) { ThinTerm t; t.kind = TermKind::Jmp; t.target = x; return t; }
static ThinTerm branch(VReg c, uint32_t a, uint32_t b) { ThinTerm t; t.kind = TermKind::Branch; t.cond=c; t.target=a; t.false_target=b; return t; }
static size_t count(const ThinFunction& f, ThinOp op) {
    size_t n = 0; for (const auto& b : f.blocks) for (const auto& i : b.instrs) n += i.op == op; return n;
}
static EmberPreserved run(const EmberPassRegistry& r, const char* name, ThinFunction& f) {
    EmberAnalysisManager am; auto p = r.create(name); CHECK(bool(p));
    return p ? p->run(f, am) : EmberPreserved::all();
}
static ThinFunction one(std::vector<ThinInstr> ins, VReg result = 0) {
    ThinFunction f; f.name = "coverage"; f.ret_type = &type_i64();
    f.blocks.push_back(block(0, std::move(ins), ret(result))); return f;
}

int main() {
    EmberPassRegistry reg; ext_opt::register_passes(reg);
    const char* names[] = {"constprop","dce","simplifycfg","cse","gvn","licm","forward",
        "copyprop","instcombine","dse","bounds-elim","lsr","unroll","sccp",
        "spill_elim","peephole","branch_folding","tailcall"};
    for (const char* n : names) CHECK(reg.has(n));

    // Every pass must tolerate empty and single empty-block functions.
    for (const char* n : names) {
        ThinFunction empty; CHECK(run(reg, n, empty).all_preserved());
        ThinFunction singleton; singleton.blocks.push_back(block(0, {}, ret()));
        run(reg, n, singleton); CHECK(singleton.blocks.size() == 1);
    }

    // Const propagation: full fold, partial immediate conversion, frame fact,
    // and alias barrier invalidation.
    {
        ThinInstr st; st.op=ThinOp::StoreFrame; st.src1=1; st.meta.frame_off=-8; st.meta.width=8;
        ThinInstr ld; ld.op=ThinOp::LoadFrame; ld.dst=2; ld.meta.frame_off=-8; ld.meta.width=8; ld.meta.type=&type_i64();
        ThinInstr call; call.op=ThinOp::CallNative; call.meta.native_name="opaque";
        ThinInstr ld2=ld; ld2.dst=3;
        auto f=one({ci(1,7),st,ld,call,ld2,bin(ThinOp::Add,4,2,3)},4);
        run(reg,"constprop",f);
        CHECK(count(f,ThinOp::ConstInt)>=1); CHECK(count(f,ThinOp::CallNative)==1);
    }
    // DCE retains effects/computed stores but reaches a dead-def fixpoint.
    {
        ThinInstr computed; computed.op=ThinOp::StoreFrame; computed.src1=1; computed.src2=2; computed.meta.frame_off=4;
        ThinInstr effect; effect.op=ThinOp::StoreGlobal; effect.src1=1;
        auto f=one({ci(1,1),ci(2,2),bin(ThinOp::Add,3,1,2),move(4,3),computed,effect});
        run(reg,"dce",f); CHECK(count(f,ThinOp::StoreFrame)==1); CHECK(count(f,ThinOp::StoreGlobal)==1);
    }
    // CFG: constant/equal-target branch folds, dead cycle disappears, IDs compact,
    // and a straight one-predecessor chain merges.
    {
        ThinFunction f; f.blocks={block(7,{cb(1,true)},branch(1,9,11)), block(9,{},jmp(13)),
            block(11,{},jmp(11)), block(13,{ci(2,42)},ret(2))};
        run(reg,"simplifycfg",f); CHECK(f.blocks.size()==1); CHECK(f.blocks[0].id==0); CHECK(f.blocks[0].term.kind==TermKind::Return);
        ThinFunction equal; equal.blocks={block(0,{ci(1,9)},branch(1,1,1)),block(1,{},ret())};
        run(reg,"simplifycfg",equal); CHECK(equal.blocks.size()==1);
    }
    // CSE: equivalent expressions through moves are replaced; memory barriers
    // prevent load CSE across a store/call.
    {
        ThinInstr l1; l1.op=ThinOp::LoadFrame; l1.dst=6; l1.meta.frame_off=-8; l1.meta.width=8;
        ThinInstr st; st.op=ThinOp::StoreFrame; st.src1=1; st.meta.frame_off=-8; st.meta.width=8;
        ThinInstr l2=l1; l2.dst=7;
        auto f=one({ci(1,2),ci(2,3),bin(ThinOp::Mul,3,1,2),move(4,1),bin(ThinOp::Mul,5,4,2),l1,st,l2},7);
        run(reg,"cse",f); CHECK(count(f,ThinOp::Mul)==1); CHECK(count(f,ThinOp::LoadFrame)==2);
    }
    // GVN across dominance and conservative join behavior.
    {
        ThinFunction f; f.blocks={block(0,{ci(1,4),ci(2,5),bin(ThinOp::Add,3,1,2)},jmp(1)),
            block(1,{bin(ThinOp::Add,4,1,2)},ret(4))};
        run(reg,"gvn",f); CHECK(count(f,ThinOp::Add)==1);
        ThinFunction join; join.blocks={block(0,{cb(1,true),ci(2,4),ci(3,5)},branch(1,1,2)),
            block(1,{bin(ThinOp::Add,4,2,3)},jmp(3)),block(2,{},jmp(3)),block(3,{bin(ThinOp::Add,5,2,3)},ret(5))};
        run(reg,"gvn",join); CHECK(count(join,ThinOp::Add)==2);
    }
    // Forward/copyprop including source redefinition and terminator/call args.
    {
        ThinInstr st; st.op=ThinOp::StoreFrame; st.src1=1; st.meta.frame_off=-8; st.meta.width=8;
        ThinInstr ld; ld.op=ThinOp::LoadFrame; ld.dst=2; ld.meta.frame_off=-8; ld.meta.width=8;
        auto f=one({ci(1,9),st,ld,move(3,2)},3); run(reg,"forward",f); CHECK(count(f,ThinOp::Move)>=2);
        run(reg,"copyprop",f); CHECK(f.blocks[0].term.ret==1);
        auto guarded=one({ci(1,1),st,ci(1,2),ld},2); run(reg,"forward",guarded); CHECK(count(guarded,ThinOp::LoadFrame)==1);
    }
    // InstCombine identities on left/right/self operands.
    {
        auto z=ci(2,0), onei=ci(3,1), neg=ci(4,-1);
        auto f=one({ci(1,7),z,onei,neg,bin(ThinOp::Add,5,1,2),bin(ThinOp::Mul,6,3,1),
                    bin(ThinOp::Sub,7,1,1),bin(ThinOp::Xor,8,1,1),bin(ThinOp::And,9,1,4)},9);
        run(reg,"instcombine",f); CHECK(count(f,ThinOp::Move)>=3); CHECK(count(f,ThinOp::ConstInt)>=5);
    }
    // DSE: overwritten exact store is removed, read and computed stores protect it.
    {
        ThinInstr s1; s1.op=ThinOp::StoreFrame; s1.src1=1; s1.meta.frame_off=-8; s1.meta.width=8;
        ThinInstr s2=s1; s2.src1=2;
        ThinInstr ld; ld.op=ThinOp::LoadFrame; ld.dst=3; ld.meta.frame_off=-8; ld.meta.width=8;
        auto f=one({ci(1,1),ci(2,2),s1,s2,ld},3); run(reg,"dse",f); CHECK(count(f,ThinOp::StoreFrame)==1);
        ThinInstr computed=s2; computed.src2=9;
        auto safe=one({s1,ld,computed}); run(reg,"dse",safe); CHECK(count(safe,ThinOp::StoreFrame)==2);
    }
    // SCCP branch folding/unreachable pruning.
    {
        ThinFunction f; f.blocks={block(0,{ci(1,2),ci(2,3),bin(ThinOp::Add,3,1,2),ci(4,5),bin(ThinOp::Cmp,5,3,4)},branch(5,1,2)),
            block(1,{ci(6,42)},ret(6)),block(2,{ci(7,99)},ret(7))};
        f.blocks[0].instrs.back().meta.cmp=0; run(reg,"sccp",f);
        CHECK(f.blocks.size()<=3); CHECK(count(f,ThinOp::ConstInt)>=2);
    }
    // Spill elimination, peephole and equal-target branch folding.
    {
        ThinInstr spill; spill.op=ThinOp::StoreFrame; spill.src1=1; spill.meta.frame_off=-8; spill.meta.width=8;
        auto f=one({ci(1,8,-8),spill,move(2,2)},1); run(reg,"spill_elim",f); CHECK(count(f,ThinOp::StoreFrame)==0);
        run(reg,"peephole",f); CHECK(count(f,ThinOp::Move)==0);
        ThinFunction br; br.blocks={block(0,{},branch(1,1,1)),block(1,{},ret())};
        run(reg,"branch_folding",br); CHECK(br.blocks[0].term.kind==TermKind::Jmp);
    }
    // LICM canonical natural loop: safe constants/arithmetic may move, loads and
    // trapping division remain in-loop. Also exercise loop rejection paths.
    {
        ThinInstr div=bin(ThinOp::Div,8,6,7);
        ThinFunction f; f.blocks={block(0,{ci(1,0)},jmp(1)),
            block(1,{ci(2,10),bin(ThinOp::Cmp,3,1,2)},branch(3,2,3)),
            block(2,{ci(4,6),ci(5,7),bin(ThinOp::Mul,6,4,5),ci(7,2),div,bin(ThinOp::Add,1,1,7)},jmp(1)),
            block(3,{},ret(1))}; f.blocks[1].instrs.back().meta.cmp=2;
        run(reg,"licm",f); CHECK(count(f,ThinOp::Div)==1); CHECK(f.blocks[0].instrs.size()>1);
        ThinFunction irreducible; irreducible.blocks={block(0,{cb(1,true)},branch(1,1,2)),block(1,{},jmp(2)),block(2,{},jmp(1))};
        run(reg,"licm",irreducible); CHECK(irreducible.blocks.size()==3);
    }
    // Loop-specialized passes: malformed/minimal loops are safe no-ops. The
    // full positive canonical shapes are already exercised by ir_passes_test.
    for (const char* n : {"bounds-elim","lsr","unroll"}) {
        ThinFunction f; f.blocks={block(0,{},jmp(1)),block(1,{ci(1,1)},branch(1,1,2)),block(2,{},ret())};
        run(reg,n,f); CHECK(f.blocks.size()==3);
    }
    // Tail-call positive, idempotence, and exception barrier.
    {
        ThinInstr call; call.op=ThinOp::CallScript; call.dst=1; call.meta.slot=3;
        call.ret_type=&type_i64();
        auto f=one({call},1); f.ret_type=&type_i64();
        CHECK(!run(reg,"tailcall",f).all_preserved()); CHECK(f.blocks[0].instrs[0].is_tail_call);
        CHECK(run(reg,"tailcall",f).all_preserved());
        ThinInstr ex; ex.op=ThinOp::Throw; ex.src1=2;
        auto blocked=one({ex,call},1); blocked.ret_type=&type_i64(); run(reg,"tailcall",blocked);
        CHECK(!blocked.blocks[0].instrs.back().is_tail_call);
    }

    std::puts(failures ? "opt pass coverage: FAIL" : "opt pass coverage: PASS");
    return failures != 0;
}
