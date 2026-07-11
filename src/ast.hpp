// ember AST + Type. Node shapes per docs/spec/COMPILER_PIPELINE.md Section 3.
#pragma once
#include "lexer.hpp"
#include <memory>
#include <string>
#include <vector>
#include <variant>
#include <optional>

namespace ember {

// --- types ---
enum class Prim : uint8_t {
    Void, Bool,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
};

struct Type {
    // a value type: primitive, named struct, fixed-array-of, or slice-of
    Prim prim = Prim::Void;
    std::string struct_name;        // non-empty iff this is a named struct type
    bool is_slice = false;          // T[]
    uint32_t array_len = 0;        // 0 = not a fixed array; >0 = T[N]
    std::shared_ptr<Type> elem;     // element type for slice/array
    // Tier 1 typed enums (docs/planning/plan_ENUMS.md §6): a typed enum
    // `enum Color : i32 { ... }` makes `Color` a REAL type. It is represented
    // representationally as its backing integer (prim = backing prim,
    // struct_name empty — so codegen/byte_size/align treat it as the backing
    // int and ALL existing int code paths work unchanged), tagged with
    // enum_name = "Color" so Type::same() distinguishes it from a plain int
    // (int->enum assignment is rejected) and from every other typed enum.
    // is_int()/is_uint() return true for a typed enum (it IS an integer
    // value); sema's is_plain_integer() excludes typed enums (enum_name set)
    // so the int<->int widening matrix does not silently accept int->enum.
    std::string enum_name;          // non-empty iff this is a typed-enum type

    // v1.0 Tier 2 first-class function refs (docs/planning/plan_FUNCTION_REFS.md §2): a
    // function handle is Prim::I64 with is_fn_handle=true. Representation is
    // identical to a plain i64 (8 bytes, GP reg, Win64 i64 ABI); the tag is
    // metadata for the call-target guard (§5) + diagnostics. recorded_* carry
    // the source fn's signature when known (a `&fib`-typed local); a bare `fn`
    // parameter has has_recorded_sig=false (accepts any fn, args unchecked at
    // the type level — the runtime guard still validates the handle, §7).
    bool is_fn_handle = false;
    bool has_recorded_sig = false;
    std::vector<std::shared_ptr<Type>> recorded_params;  // empty unless is_fn_handle
    std::shared_ptr<Type> recorded_ret;                  // null unless is_fn_handle

    bool is_int() const;            // any i*/u*
    bool is_uint() const;          // any u*
    bool is_float() const;          // f32/f64
    bool is_bool() const;
    bool is_void() const { return prim == Prim::Void && struct_name.empty() && !is_slice && array_len==0; }
    bool is_struct() const { return !struct_name.empty(); }
    size_t byte_size() const;       // for sizeof / frame layout
    uint32_t align() const;
    bool same(const Type& o) const; // nominal equality
    std::string to_string() const;
};

// common: shared primitive type singletons
const Type& type_void();
const Type& type_bool();
const Type& type_i64();
const Type& type_u64();
const Type& type_f32();
const Type& type_f64();
Type make_slice(std::shared_ptr<Type> elem);
Type make_array(std::shared_ptr<Type> elem, uint32_t n);
Type make_struct(std::string name);
Type make_prim(Prim p);

// --- source location on every node ---
struct Loc { uint32_t line; uint32_t col; };

// --- AST nodes ---
struct Node {
    Loc loc;
    virtual ~Node() = default;
};
using NodePtr = std::unique_ptr<Node>;

// expressions
struct Expr : Node { const Type* ty = nullptr; };  // ty filled by sema
using ExprPtr = std::unique_ptr<Expr>;

struct IntLit   : Expr { int64_t v; };
struct FloatLit : Expr { double v; bool is_f32 = false; };
struct BoolLit  : Expr { bool v; };
struct StringLit: Expr { std::string s;            // slice<u8> to host rodata
    // baked by sema: stable storage address + length (Program::rodata_store
    // owns the bytes so the pointer stays valid as long as the JIT'd code is
    // callable, same lifetime contract as globals/dispatch table).
    // If string encryption is enabled (default), the stored bytes are
    // XOR-encrypted with baked_key; codegen decrypts inline into a
    // compiler-hidden temp frame slot (see codegen's alloc_str_temp) at
    // each use site, so raw strings never appear in the JIT'd executable
    // memory AND the plaintext is transient - it lives only in the caller's
    // stack frame for the expression's lifetime, reclaimed when the frame
    // is torn down. The encrypted form alone lives in rodata.
    const uint8_t* baked_ptr = nullptr;
    int64_t baked_len = 0;
    uint8_t baked_key = 0;  // XOR key (0 = no encryption)
    bool encrypted = false;
    // Implicit conversion to a `string` handle (sema stamps these when this
    // literal appears where a `string` is expected - see sema.cpp's StringLit
    // check_expr case), mirroring how BinExpr::is_overload/overload_fn already
    // let sema resolve something an AST node can't self-determine and hand
    // codegen a native fn pointer to call. When set, codegen emits a
    // string_from_slice(ptr,len) call right after the slice above and uses
    // ITS result (an i64 string handle) as this expression's value instead.
    bool implicit_to_string = false;
    void* to_string_native_fn = nullptr;
    std::string to_string_native_name;
};
struct Ident    : Expr { std::string name; };
struct BinExpr  : Expr { enum class Op {
        Add,Sub,Mul,Div,Mod, And,Or,Xor,Shl,Shr,
        Eq,Neq,Lt,Le,Gt,Ge, LAnd,LOr } op;
    ExprPtr lhs, rhs;
    // operator-overload dispatch (docs/spec/TYPE_SYSTEM.md Section 7): if sema resolves this
    // BinExpr to a registered overload (vec3_add, etc.), codegen emits a native
    // call instead of inline arithmetic.
    bool is_overload = false;
    void* overload_fn = nullptr;
    // Sema preserves the exact symbolic overload binding selected from the
    // OpOverloadTable. Codegen must not reverse-map overload_fn: aliases can
    // share an address, and overload-only symbols need not exist in natives.
    std::string overload_name;
    Type overload_ret;
    std::vector<Type> overload_params;
};
struct UnaryExpr: Expr { enum class Op { Neg,Not,BitNot } op; ExprPtr operand; };
struct CastExpr : Expr { ExprPtr operand; std::shared_ptr<Type> to; };
// v1.0 Tier 2 first-class function refs (docs/planning/plan_FUNCTION_REFS.md §3.1): `&fn_name`
// produces a function handle. Sema resolves the operand to a script fn slot,
// bakes it as an i64 literal (slot). NOT a runtime computation — a compile-time
// reification. `slot` is filled by sema; codegen emits `mov rax, imm64(slot)`.
struct FnHandleExpr : Expr { ExprPtr operand; int slot = -1; };
struct CallExpr : Expr { std::string name; std::vector<ExprPtr> args;
                         // sema-resolved target (native fn ptr or script slot)
                         bool is_native = false; void* native_fn = nullptr;
                         // Exact native-table key selected by sema. This is the
                         // portable .em identity; never recover it from fn_ptr.
                         std::string native_binding_name;
                         int script_slot = -1;
                         // v0.5 cross-module: non-empty module_alias = a `mod::fn()` call.
                         // Sema resolves it against the linked-module export table; if the
                         // module/fn is not yet registered it's an unresolved external (deferred
                         // trap, docs/MODULES.md §5). codegen stamps cross_module_id/slot when
                         // resolved (kind-2 call site); unresolved stays -1 (trap stub).
                         std::string module_alias;       // empty = same-module call
                         int cross_module_id = -1;        // registry module_id (resolved)
                         int cross_module_slot = -1;     // target fn's slot in that module
                         bool cross_module_unresolved = false; // sema couldn't resolve (deferred trap)
                         // method-call sugar: obj.method(args) desugars to
                         // method(obj, args) - receiver becomes arg[0]. Null
                         // for free-function calls. (docs/spec/BINDING_API.md Section 3)
                         ExprPtr receiver;
                         // compile-time assertion folding (sema.cpp's
                         // CallExpr check): set when this call is one of
                         // the assert_eq_* natives AND both arguments folded to
                         // compile-time constants that compare EQUAL - codegen
                         // emits nothing at all for this call (a mismatch is a
                         // sema compile error instead, never reaches codegen).
                         bool elided = false;
                         // v1.0 Tier 2 first-class call: `indirect_target(args)` — a call
                         // through a RUNTIME i64 handle, not a compile-time-known name. Sema
                         // types indirect_target as a fn handle, sets is_indirect=true;
                         // codegen validates the handle against the registered-fn allowlist
                         // (REDSHELL guard #6, plan §5) before dispatching via
                         // `call [dispatch_base + handle*8]`. Mutually exclusive with
                         // is_native / script_slot / module_alias being set (sema asserts).
                         ExprPtr indirect_target;
                         bool is_indirect = false; };
struct IndexExpr: Expr { ExprPtr base, index;
                         // compile-time bounds folding (sema.cpp's IndexExpr
                         // check): set when `index` folded to a compile-time
                         // constant. Codegen uses this + the base's type (fixed
                         // array vs. slice) to decide whether a runtime bounds
                         // check is still needed - a constant index against a
                         // fixed-size array T[N] was already range-checked by
                         // sema (out-of-range is a compile error, never reaches
                         // codegen), so that ONE combination needs no runtime
                         // check at all. A slice's length is runtime-only even
                         // for a constant index, so this flag alone is not
                         // sufficient - codegen still checks bt->is_slice too.
                         bool index_is_const = false;
                         int64_t index_const_value = 0; };
struct FieldExpr: Expr { ExprPtr base; std::string field; };
struct ViewExpr : Expr { ExprPtr base; };           // arr[..]
struct AssignExpr: Expr { ExprPtr target; std::optional<BinExpr::Op> compound; ExprPtr value; bool postfix = false; };
struct TernaryExpr: Expr { ExprPtr cond, then_e, else_e; };
struct SizeofExpr: Expr { std::shared_ptr<Type> ty; uint64_t resolved = 0; };
struct OffsetofExpr: Expr { std::shared_ptr<Type> ty; std::string field; uint64_t resolved = 0; };
struct StructLit : Expr { std::string type_name; std::vector<std::pair<std::string,ExprPtr>> fields; };
// Array literal `[ expr, expr, ... ]` as a first-class expression (chunk c2).
// Parsed in the PRIMARY position (parse_primary's LBracket case); the postfix
// `[` at parse_postfix stays the index/view operator, so `arr[0]` still indexes
// and `[1,2,3]` at an expression start constructs a literal. The element type
// is carried on `->ty` after sema; for a fixed-array literal the array Type is
// on `->ty`, for a slice literal the slice Type is on `->ty` too. Sema requires
// a declared target type at the let-init / arg site (v1 does not infer array
// types from elements), so an ArrayLit never reaches codegen without `->ty`
// already baked to the full array/slice Type.
struct ArrayLit : Expr { std::vector<ExprPtr> elements; };
// E::A - enum-variant access. Exists only between parse and sema's check_expr:
// sema rewrites this node IN PLACE to an IntLit carrying the variant's i32
// value (see docs/planning/plan_ENUMS.md Section 5 - the switch case-value literal-check
// at sema.cpp's SwitchStmt requires an IntLit by the time check_expr
// returns, so the rewrite happens in sema, not as a codegen pre-pass).
struct EnumAccessExpr : Expr { std::string enum_name; std::string variant; };

// statements
struct Stmt : Node {};
using StmtPtr = std::unique_ptr<Stmt>;

struct Block { std::vector<StmtPtr> stmts; };

struct ExprStmt : Stmt { ExprPtr expr; };
struct LetStmt  : Stmt { std::string name; std::shared_ptr<Type> ty; ExprPtr init; bool is_auto; bool is_const; bool used_auto_kw = false; };
struct IfStmt   : Stmt { ExprPtr cond; Block then_b; bool has_else=false; Block else_b; };
struct WhileStmt: Stmt { ExprPtr cond; Block body; };
struct ForStmt  : Stmt { std::unique_ptr<LetStmt> init; ExprPtr cond; ExprPtr step; Block body; };
struct DoWhileStmt:Stmt { Block body; ExprPtr cond; };
// Tier 1: for-each over a slice T[]. `iter` must be a slice; `var` gets the
// element type. Desugared by codegen to a while loop with indexing.
//
// iterable() hook (Tier 1): for-each generalizes beyond slices to any
// registered iterable. Two iterable kinds are recognized today:
//   1. a slice T[]           -> tree-walker ptr+len indexing (the shipped path)
//   2. an array<T> handle    -> an opaque i64 from the array extension, lowered
//                               to array_length(h) + array_get_*(h, i) natives
// `array_elem_ty` is set by sema ONLY for the array-handle case (case 2): it
// is the inferred element type (u8/f32/i64) used to select the array_get_*
// variant in codegen. It is null for the slice case, where the element type
// comes from iter->ty->elem. Future iterables (map, host collections)
// register through the same hook surface (see sema.cpp's ForEachStmt check +
// the array_elem_ty inference); only the array case is implemented now.
struct ForEachStmt: Stmt { std::string var; ExprPtr iter; Block body; const Type* array_elem_ty = nullptr; };
struct ReturnStmt:Stmt { ExprPtr value; };          // nullptr = void return
struct BreakStmt: Stmt {};
struct ContinueStmt:Stmt {};
struct DeferStmt : Stmt { ExprPtr expr; };
struct BlockStmt: Stmt { Block block; };
// one `case EXPR: stmts...` or `default: stmts...` clause. `value` is null
// iff is_default. Sema requires each nonempty body to end in `break` or
// `return`, so implicit fallthrough never reaches codegen.
struct SwitchCase { ExprPtr value; bool is_default = false; Block body; };
struct SwitchStmt: Stmt { ExprPtr subject; std::vector<SwitchCase> cases; };

// Tier 1: match — pattern matching. Like switch but without break (each arm
// is a separate branch) and with => syntax. Patterns: integer literals,
// bool literals, `_` (wildcard/default). No struct destructure in v1.
struct MatchArm { ExprPtr pattern; bool is_wildcard = false; Block body; };
struct MatchStmt : Stmt { ExprPtr subject; std::vector<MatchArm> arms; };
// Tier 1: static_assert(cond, "msg") — a compile-time assertion. Sema folds
// `cond` (constexpr calls + enum variants are already IntLits by the time the
// statement check runs, so the same try_eval_const_bool that powers assert_eq
// folding applies) and resolves it NOW: true -> elided (codegen emits nothing,
// the statement is a pure compile-time check); false -> a sema compile error
// carrying `msg`; not-foldable -> a sema compile error ("condition must be a
// compile-time constant"). Valid both inside a function body (parsed by
// parse_stmt, checked in check_stmt) and at top level (parsed in parse_program,
// stored on Program::static_asserts, checked in a dedicated sema pass). Produces
// NO runtime code in either position.
struct StaticAssertStmt : Stmt { ExprPtr cond; std::string msg; };

// declarations
// A literal default value for a parameter (v1: literals only - arbitrary
// expression defaults would need either a generic AST-clone mechanism
// (ExprPtr is a unique_ptr, not trivially copyable) or re-evaluating a
// stored expression fresh at each call site, risking out-of-scope names).
// Mirrors IntLit/FloatLit/BoolLit/StringLit's shapes closely enough that
// sema can synthesize a fresh literal AST node directly from this at each
// call site missing the argument.
struct DefaultValue {
    enum class Kind { None, Int, Float, Bool, String } kind = Kind::None;
    int64_t     i = 0;           // Kind::Int
    double      f = 0.0;         // Kind::Float
    bool        f_is_f32 = false;// Kind::Float - mirrors FloatLit::is_f32
    bool        b = false;       // Kind::Bool
    std::string s;                // Kind::String
};
struct Param { std::string name; std::shared_ptr<Type> ty; DefaultValue default_val; Loc loc; };
struct Annotation { std::string name; std::vector<std::string> args; };
struct FieldDecl { std::string name; std::shared_ptr<Type> ty; };

struct FuncDecl {
    std::string name;
    std::vector<Param> params;
    std::shared_ptr<Type> ret;
    std::vector<Annotation> annotations;
    Block body;
    Loc loc;
    int slot = -1;                 // sema-assigned dispatch slot
    // F1 visibility (docs/spec/SPEC_AUDIT_2026-07-10.md F1): is this fn part of
    // the module's EXPORTED surface (published to the .em name table / the JIT
    // export table, callable cross-module via `mod::fn()`), or a module-private
    // internal helper? Backward-compat decision: a bare `fn` is EXPORTED by
    // default (preserves every existing cross-module test/demo that uses bare
    // `fn` and calls across modules); `priv fn` opts OUT of the export surface.
    // Intra-module visibility is unchanged - a `priv fn` is still callable from
    // its own module; it is simply not published to other modules. The CLI /
    // native `engine.define` surface is unaffected (pub/priv is a script-module
    // bundling concern, not a native-binding concern).
    bool is_exported = true;
    // constexpr fn (Tier 1 feature): a fn declared `constexpr` CAN be
    // const-evaluated at sema time when called with all-constant arguments.
    // The call site is then rewritten IN PLACE to an IntLit carrying the
    // folded result (mirroring lower_enum_access_expr's EnumAccessExpr ->
    // IntLit rewrite), so downstream consumers (case labels, global inits,
    // codegen const-fold) see the literal directly. A constexpr fn may ALSO
    // be called at runtime with non-constant args — it is a normal fn that
    // CAN be const-evaluated, not one that MUST be. If any arg is not
    // constant, the call is left as a normal runtime call (no error).
    bool is_constexpr = false;
};

struct StructDecl {
    std::string name;
    std::vector<FieldDecl> fields;
    Loc loc;
};

struct GlobalDecl {
    std::string name;
    std::shared_ptr<Type> ty;
    ExprPtr init;
    Loc loc;
    bool is_const = false;
};

// v0.5 live-module link declaration (docs/MODULES.md §6). `link "foo.em" as foo;`
// loads+registers a .em bundle at init; `link "foo" as foo;` links to an
// already-registered module. `as alias` is optional sugar (defaults to the
// module name / the file stem). Distinct from textual `import "path";` which
// inlines source before lexing (docs/BUNDLING_AND_EM_MODULES.md §1.1).
struct LinkDecl {
    std::string target;      // module name OR .em file path
    std::string alias;       // the name scripts use (foo in foo::bar())
    bool is_file = false;     // true = target is a .em path (load it); false = link to registered
    Loc loc;
};

// Tier 1 script-side `enum` (docs/planning/plan_ENUMS.md). An enum is a set of named i32
// compile-time constants; an enum value IS an i32 (no new Type shape).
// `explicit_value` is nullptr iff the variant has no `= value` (auto-
// increment from the previous variant). It is a constexpr-foldable integer
// expression (IntLit / -IntLit / BinExpr-of-literals - the restricted set
// try_eval_const_i64 already folds); the parser stores the raw expr, sema
// folds + range-checks + fills `resolved`. `resolved` is the variant's final
// i32 value, available to EnumAccessExpr resolution (sema's enum-resolution
// pass runs before any function body is checked).
struct EnumVariant {
    std::string name;
    ExprPtr explicit_value;
    Loc loc;
    int64_t resolved = 0;
};
struct EnumDecl {
    std::string name;
    std::vector<EnumVariant> variants;
    Loc loc;
    // Tier 1 typed enums (docs/planning/plan_ENUMS.md §6): `enum E : T { ... }`
    // declares E as a real type backed by the integer T. null = untyped (the
    // original v1 shape — variants are plain i32 constants, E is NOT a type);
    // non-null = typed (E is a type, a Color-typed binding accepts only
    // Color::Variant values, int->enum is rejected, enum->int widens).
    std::shared_ptr<Type> backing_type;
    bool is_typed() const { return backing_type != nullptr; }
};

struct Program {
    std::vector<StructDecl> structs;
    std::vector<GlobalDecl> globals;
    std::vector<FuncDecl> funcs;
    std::vector<LinkDecl> links;   // v0.5 live-module link declarations (docs/MODULES.md §6)
    std::vector<EnumDecl> enums;   // Tier 1 script-side enums (docs/planning/plan_ENUMS.md)
    std::vector<StaticAssertStmt> static_asserts;  // Tier 1 top-level static_assert (in-body ones live inside their fn's Block)
    // type store: owns synthesized Types created by sema (slices, adapted
    // literal types) so the raw `ty` pointers stashed on AST nodes survive
    // until codegen finishes (sema's local Checker would otherwise free them).
    std::vector<std::shared_ptr<Type>> type_store;
    // rodata store: owns the byte buffers for string literals (StringLit::
    // baked_ptr points into these) so they outlive sema, same as type_store.
    std::vector<std::shared_ptr<std::string>> rodata_store;
    // per-compile XOR key for string encryption. Nonzero =
    // strings are XOR-encrypted in rodata; codegen decrypts inline into a
    // compiler-hidden temp frame slot at each use site (no host native)
    // calls. The host sets this before sema; 0 disables encryption.
    uint8_t string_xor_key = 0;
};

} // namespace ember
