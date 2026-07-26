// thin_interp.hpp — WASM W0: the ThinIR interpreter API.
//
// WASM has no user-allocated executable memory (no PROT_EXEC pages, no JIT),
// so the WASM backend is a C++ INTERPRETER that walks a lowered ThinFunction
// instead of emitting native code. This is "emit_arm64 but in C++" — strictly
// easier (no ABI marshaling, no regalloc, no relocations, no encodings). The
// interpreter mirrors src/thin_emit_arm64.cpp's SEMANTICS exactly: every
// ThinOp produces the same value emit_arm64 would compute.
//
// W0 builds the interpreter NATIVELY (macOS ARM64, Apple Clang) ALONGSIDE the
// JIT and validates it produces the same results as emit_arm64. Emscripten/
// wasi-sdk comes in W1 (a later task) — do NOT add Emscripten now. The
// interpreter is ADDITIVE: it does not touch the JIT path (no existing src/
// files are modified to add it).
//
// DESIGN (see the W0 section of docs/planning/WASM_PROGRESS.md for the full
// write-up):
//
// • VReg model — frame-buffer, mirroring emit_arm64. The interpreter maintains
//   a `vregs[VReg] -> {frame_off, type}` map (built lazily as producing
//   instrs execute: a producing instr with dst != 0 + meta.frame_off != 0
//   records the dst's home frame slot). Reading a src vreg reads from its
//   frame slot (width-normalized per its type); writing a dst vreg writes to
//   its frame slot. VRegs with frame_off == 0 (emit_arm64's "trust x9"
//   fallback — rare in well-formed lowering) fall back to a vreg→value table.
//   This is emit_arm64's load_int_vreg / pin_int_dst model translated to C++.
//
// • Frame — a `frame_size` byte buffer. LoadFrame/StoreFrame access it at
//   meta.frame_off (a negative offset; use frame_base + frame_off).
//   CopyBytes memmoves within it. Struct/array temps live in it at their
//   frame offsets. Scalar slots are 8 bytes (mirrors emit_arm64's
//   frame_load64/frame_store64); narrow element loads/stores honor meta.width.
//
// • Calls — recursive interpret_thin. CallScript: look up dispatch[slot] ->
//   ThinFunction*, marshal args from the caller's vregs/frame into the
//   callee's arg-word array, recurse, place the result. CallNative: resolve
//   by meta.native_name from ctx.natives, marshal, call the fn_ptr via a
//   typed dispatcher (inspects the NativeSig), return. CallIndirect /
//   CallCrossModule: the handle-based dispatch (bit-63 cross-module), mirroring
//   emit_arm64's emit_indirect_call / emit_cross_module_call.
//
// • Traps — a C++ exception (struct InterpTrap { TrapReason reason; }) thrown
//   on a trap, caught at the top-level interpret_thin call (or at TryCatch
//   boundaries for in-language catch). The WASM build (-fno-exceptions) will
//   swap this to setjmp/longjmp or error-code return; for the NATIVE
//   validation build, C++ exceptions are fine. The trap policy is isolated
//   behind EMBER_INTERP_TRAP_THROW / EMBER_INTERP_TRAP_RAISE macros so it can
//   be swapped without touching the interpreter loop.
//
// • Try/catch — pc-restore + call_depth-restore, NOT libc longjmp (the audit's
//   CRITICAL correction). The interpreter saves (block index, instr index,
//   call_depth) at TryCatch; Throw restores them + jumps to the catch entry
//   (the block at meta.slot). CatchCleanup pops the catch stack. CatchEntry
//   loads the thrown value into the catch_name slot (meta.frame_off). A
//   per-invocation catch-stack is maintained. Unhandled throw -> trap
//   (UnhandledThrow).
//
// • GC shadow-stack linkage (W2 testing, but the HOOK is built now) — on entry
//   to a fn with thf.frame.gc_ptr_frame_offs non-empty, the interpreter
//   allocates a GcFrameRecord, sets its map to the gc_ptr_frame_offs, links it
//   onto ctx->gc_frame_head; on exit, unlinks. This lets gc_collect() during
//   an interpreted lambda-using script find the roots (the collector walks the
//   gc_frame_head chain via ext_gc's trace callback).
//
// • Coroutines — STUB for W0. The design (paused interpreter frame
//   {ThinFunction*, pc, frame copy, call_depth}; yield = save + return; resume
//   = re-enter) is noted in the progress doc; full coroutine support is deferred.
//
// See src/thin_interp.cpp for the implementation + src/thin_emit_arm64.cpp for
// the reference semantics being mirrored.
#pragma once
#include "thin_ir.hpp"       // ThinFunction, ThinOp, ThinMeta, ...
#include "context.hpp"       // context_t, TrapReason
#include "codegen.hpp"       // CodeGenCtx (for natives/structs/globals/...)
#include "sema.hpp"          // NativeSig, StructLayoutTable
#include "gc_roots.hpp"      // gc::GcFrameRecord, GcFrameMap

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ember {

// A trap raised by the interpreter. Thrown as a C++ exception in the native
// build (EMBER_INTERP_TRAP_THROW); the WASM build will swap the policy to
// setjmp/longjmp or error-code return. Caught at the top-level interpret_thin
// call (sets ctx->last_trap + returns a sentinel) or at TryCatch boundaries
// (restores to the catch entry). The `reason` matches TrapReason.
struct InterpTrap {
    TrapReason reason = TrapReason::None;
    std::string detail;
    InterpTrap() = default;
    InterpTrap(TrapReason r, std::string d = {}) : reason(r), detail(std::move(d)) {}
};

// The interpreter's dispatch table: slot -> ThinFunction*. For CallScript /
// CallIndirect (intra-module), the interpreter indexes this by slot. For
// CallCrossModule, see interpret_thin's `cross_module_tables` param (a
// mod_id -> dispatch-table map); null = no cross-module calls.
using InterpDispatch = std::vector<const ThinFunction*>;

// Per-module handle-records for CallIndirect cross-module handles (bit 63).
// Each record is {dispatch_base (InterpDispatch*), allowlist bytes, slot_count}.
// Mirrors emit_arm64's module_handle_records_base table (the JIT bakes raw
// ptrs; the interpreter uses this struct). Null entries = module not wired.
struct InterpHandleRecord {
    const InterpDispatch* dispatch = nullptr;  // target module's dispatch table
    const std::vector<uint8_t>* allowlist = nullptr;  // bit per slot (set = registered)
    int64_t slot_count = 0;
};
using InterpHandleRecords = std::vector<InterpHandleRecord>;

// The cross-module dispatch map: mod_id -> the target module's InterpDispatch.
// Used by CallCrossModule (meta.mod_id + meta.slot). Null = no cross-module.
using InterpCrossModuleTables = std::vector<const InterpDispatch*>;

// ─── the interpreter entry points ───
//
// All overloads allocate a frame_size byte buffer, place the incoming args
// into the callee's frame per thf.frame.params (one int64_t word per param;
// slices consume 2 consecutive words {ptr, len}; struct/array params consume
// 1 word holding a const void* to the struct bytes — the interpreter memcpys
// value_bytes(ty) bytes into the frame slot; floats are bit-cast into the
// word), walk blocks from block 0, execute instrs, follow the term, and
// return the result.
//
// `struct_ret_dest` — when thf.frame.returns_struct_by_ptr, the caller
// provides a buffer the callee writes the struct result into (the hidden
// return-ptr). Null = no struct return (the fn does not return a struct).
//
// Traps: a trap throws InterpTrap (native build) which is caught at the
// top-level call; the caught trap sets ctx->last_trap + ctx->last_error and
// returns 0 (for the i64/f64 overloads). A host that wants recoverable traps
// should set ctx->has_checkpoint + use the _safe variants (below) OR catch
// InterpTrap itself.

// Call a script fn returning i64 (int/bool/fn-handle/string-handle). Returns
// the i64 result, or 0 if a trap was caught at the top level (ctx->last_trap
// is set). `args` is one int64_t word per param (slices = 2 words, structs =
// 1 word = const void*). `nargs` is the word count.
int64_t interpret_thin_i64(const ThinFunction& thf,
                           const InterpDispatch& dispatch,
                           const CodeGenCtx& ctx,
                           context_t* ectx,
                           const int64_t* args, size_t nargs,
                           void* struct_ret_dest = nullptr,
                           const InterpCrossModuleTables* cross_module_tables = nullptr,
                           const InterpHandleRecords* handle_records = nullptr);

// Call a script fn returning f64 (float). The float result is returned as a
// double. Same arg convention.
double interpret_thin_f64(const ThinFunction& thf,
                          const InterpDispatch& dispatch,
                          const CodeGenCtx& ctx,
                          context_t* ectx,
                          const int64_t* args, size_t nargs,
                          void* struct_ret_dest = nullptr,
                          const InterpCrossModuleTables* cross_module_tables = nullptr,
                          const InterpHandleRecords* handle_records = nullptr);

// Call a script fn returning void. Same arg convention.
void interpret_thin_void(const ThinFunction& thf,
                         const InterpDispatch& dispatch,
                         const CodeGenCtx& ctx,
                         context_t* ectx,
                         const int64_t* args, size_t nargs,
                         void* struct_ret_dest = nullptr,
                         const InterpCrossModuleTables* cross_module_tables = nullptr,
                         const InterpHandleRecords* handle_records = nullptr);

// Recover a slice return {ptr, len} from a fn that returns a slice. After
// interpret_thin_i64 returns for a slice-returning fn, the slice {ptr, len}
// is in the fn's return vreg's frame slot; this helper reads it out. Returns
// {ptr, len} in the out params. (Slice returns are rare in the test probes;
// this is the convenience accessor.)
void interpret_thin_slice_result(const ThinFunction& thf,
                                 int64_t* out_ptr, int64_t* out_len);

// A recoverable-trap variant: sets up a host setjmp checkpoint (mirrors
// emit_arm64_test's call0_safe) so a trap is recoverable + observable. Sets
// *trapped = true if the trap stub fired (longjmp). Returns the i64 result
// (0 if trapped). The ctx must have has_checkpoint handling; this helper
// manages it.
int64_t interpret_thin_i64_safe(const ThinFunction& thf,
                                const InterpDispatch& dispatch,
                                const CodeGenCtx& ctx,
                                context_t* ectx,
                                const int64_t* args, size_t nargs,
                                bool* trapped,
                                void* struct_ret_dest = nullptr,
                                const InterpCrossModuleTables* cross_module_tables = nullptr,
                                const InterpHandleRecords* handle_records = nullptr);

} // namespace ember
