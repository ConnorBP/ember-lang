// gc_roots.hpp — precise GC root-scanning data structures (shadow stack).
//
// The shared, frontend-free POD/POD-ish types used by the precise GC root
// scanning introduced for lambda environments (#20) and the tracing collector
// (src/gc.{hpp,cpp}). These describe WHERE GC pointers live at runtime so the
// collector can trace them EXACTLY instead of conservatively scanning arbitrary
// stack words.
//
// Three participants:
//
//   GcFrameMap      — a COMPILE-TIME description of one function's frame: the
//                     rbp-relative (negative) byte offsets of every frame slot
//                     that holds a GC object pointer (a lambda env_ptr, the
//                     second word of a lambda-valued local, or a compiler-
//                     hidden GC env-pointer temp). Produced by both backends
//                     (src/codegen.cpp tree-walker + src/thin_lower.cpp Thin
//                     IR) and owned by the CompiledFn (src/engine.hpp) for the
//                     function's lifetime. The collector iterates `offs`.
//
//   GcFrameRecord   — a RUNTIME record, one per ACTIVE JIT'd frame, living
//                     INSIDE the JIT'd stack frame (a 24-byte region reserved
//                     by the prologue when precise GC is enabled). It forms a
//                     singly-linked list (the "shadow stack") whose head lives
//                     in context_t (gc_frame_head). Each record carries:
//                       prev        — the previous head (the caller's record)
//                       frame_base  — this frame's rbp value (so the collector
//                                     can compute each slot's absolute address
//                                     as frame_base + off)
//                       map         — pointer to this frame's GcFrameMap
//                     The prologue links a new record; the epilogue unlinks it
//                     (restores the head to `prev`). Collection walks the chain
//                     from context_t::gc_frame_head and reports each mapped
//                     slot's value as a root candidate.
//
//   GcGlobalRoots   — a RUNTIME descriptor of the typed global block's GC-
//                     pointer words: the globals base address + the byte
//                     offsets of every global slot that holds a GC pointer
//                     (notably the env_ptr half of a lambda-typed global at
//                     offset+8). Owned by the host/module and attached to
//                     context_t::gc_global_roots. The collector reports each
//                     *(base + off) as a root candidate.
//
// Why a SEPARATE header (not in gc.hpp or context.hpp): gc.hpp's collector core
// is intentionally frontend-free AND context-free (the gc_test unit test drives
// it with no context_t). context.hpp is the lowest core layer (csetjmp/mutex).
// Putting these shared types in their own header lets context.hpp, codegen,
// thin_ir, ext_gc, and (optionally) gc.cpp all reference them without creating
// a cycle or dragging std::vector into context.hpp's minimal include set.
//
// THREAD-SAFETY: under the concurrent-entry model a context_t (and its
// gc_frame_head / gc_global_roots) is per-thread — each concurrently-entering
// OS thread owns its own context_t, so each shadow-stack head is single-thread
// mutated by its owning thread's JIT prologue/epilogue. A GcFrameMap is read-
// only after compile. A GcGlobalRoots is read-only after the host attaches it
// and is SHARED across the workers of one module (the same typed-global block
// descriptor). The cooperative stop-the-world collector (ext_gc) walks every
// registered participant's per-thread gc_frame_head plus the shared immutable
// global roots while the participants are parked at a safepoint or exited, so
// the collector never races a participant's JIT root-chain mutation.
#pragma once
#include <cstdint>
#include <vector>

namespace ember::gc {

// Compile-time description of one function's GC-pointer frame slots.
// `offs` holds rbp-relative ABSOLUTE (negative) byte offsets of every 8-byte
// frame slot that stores a GC object's user-byte pointer. Empty = this frame
// holds no GC pointers (a leaf with no lambda envs / no lambda-valued locals).
// Owned by CompiledFn (a std::shared_ptr<GcFrameMap>); its ADDRESS is baked
// into the function's prologue as the frame record's `map` pointer, so it must
// live at a stable address for the function's lifetime (shared_ptr guarantees
// that). The backing std::vector's data pointer is stable once filling is
// complete (compile finishes before any call), so the collector may iterate it
// directly.
struct GcFrameMap {
    std::vector<int32_t> offs;  // rbp-relative (negative) byte offsets of GC ptr slots
    bool empty() const { return offs.empty(); }
};

// One active JIT frame's record on the shadow-stack chain (lives in the frame).
// Layout is POD (three pointers) so JIT'd code can write it with plain stores.
// The collector walks: for (rec = head; rec; rec = rec->prev)
//                          for (off : rec->map->offs) report(*(rec->frame_base + off));
struct GcFrameRecord {
    GcFrameRecord* prev;       // previous head (caller's record), nullptr if outermost
    const void* frame_base;    // this frame's rbp value (slot addr = frame_base + off)
    const GcFrameMap* map;     // this frame's compile-time GC slot map (nullptr = no slots)
};

// Runtime descriptor of the typed global block's GC-pointer words. `base` is
// the globals block base address (the same value codegen bakes as GlobalsBase);
// `offs` are the byte offsets within the block of every 8-byte slot holding a
// GC object pointer (e.g. the env_ptr half of a lambda-typed global, which
// lives at the global's byte offset + 8). The host builds this from
// GlobalsBlock (src/codegen.hpp) base/offset/type info and attaches it to
// context_t::gc_global_roots. Recursively GC-bearing words where supported
// (struct globals with lambda fields, etc.) are flattened into `offs`.
struct GcGlobalRoots {
    uint64_t base = 0;          // globals block base address
    std::vector<int32_t> offs;  // byte offsets of GC-pointer words within the block
    bool empty() const { return offs.empty(); }
};

} // namespace ember::gc
