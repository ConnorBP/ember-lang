// block_merge_pass.cpp — conservative single-predecessor CFG block merging.
#include "custom_passes.hpp"
#include "thin_ir_ser.hpp"

#include <cstddef>
#include <iterator>
#include <unordered_map>

namespace ember::examples::custom_pass {
namespace {

void canonicalize_block_ids(ThinFunction& f) {
    std::unordered_map<uint32_t, uint32_t> remap;
    remap.reserve(f.blocks.size());
    for (size_t i = 0; i < f.blocks.size(); ++i)
        remap[f.blocks[i].id] = static_cast<uint32_t>(i);

    for (size_t i = 0; i < f.blocks.size(); ++i) {
        auto& block = f.blocks[i];
        if (block.term.kind == TermKind::Jmp) {
            block.term.target = remap.at(block.term.target);
        } else if (block.term.kind == TermKind::Branch) {
            block.term.target = remap.at(block.term.target);
            block.term.false_target = remap.at(block.term.false_target);
        }
        block.id = static_cast<uint32_t>(i);
    }
}

} // namespace

EmberPreserved BlockMergePass::run(ThinFunction& f, EmberAnalysisManager&) {
    bool changed = false;

    // Merge one edge, then rebuild predecessor facts. Erasing B can renumber
    // vector positions and can make another formerly non-trivial edge trivial.
    for (;;) {
        std::unordered_map<uint32_t, size_t> id_to_index;
        std::unordered_map<uint32_t, size_t> predecessor_count;
        for (size_t i = 0; i < f.blocks.size(); ++i)
            id_to_index[f.blocks[i].id] = i;
        for (const auto& block : f.blocks) {
            if (block.term.kind == TermKind::Jmp) {
                ++predecessor_count[block.term.target];
            } else if (block.term.kind == TermKind::Branch) {
                ++predecessor_count[block.term.target];
                ++predecessor_count[block.term.false_target];
            }
        }

        bool merged = false;
        for (size_t ai = 0; ai < f.blocks.size(); ++ai) {
            ThinBlock& a = f.blocks[ai];
            if (a.term.kind != TermKind::Jmp) continue;

            const uint32_t target_id = a.term.target;
            auto found = id_to_index.find(target_id);
            if (found == id_to_index.end()) continue; // malformed input: validator rejects it
            const size_t bi = found->second;
            if (bi == 0 || bi == ai) continue;       // never erase entry; no self-edge
            if (predecessor_count[target_id] != 1) continue;
            if (a.instrs.size() + f.blocks[bi].instrs.size() > IR_MAX_INSTRS)
                continue;

            ThinBlock& b = f.blocks[bi];
            a.instrs.insert(a.instrs.end(),
                            std::make_move_iterator(b.instrs.begin()),
                            std::make_move_iterator(b.instrs.end()));
            a.term = b.term; // replace A's jump with B's terminator
            f.blocks.erase(f.blocks.begin() + static_cast<ptrdiff_t>(bi));
            canonicalize_block_ids(f); // rewrite every surviving CFG edge
            f.ra = {};                 // block order/liveness facts are stale
            changed = merged = true;
            break;
        }
        if (!merged) break;
    }

    return changed ? EmberPreserved::none() : EmberPreserved::all();
}

} // namespace ember::examples::custom_pass
