// ember AST + Type. Node shapes per COMPILER_PIPELINE.md Section 3.
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
    // XOR-encrypted with baked_key; codegen emits a __str_decrypt call
    // instead of a raw pointer so raw strings never appear in the JIT'd
    // executable memory (encrypted-rodata string obfuscation).
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
};
struct Ident    : Expr { std::string name; };
struct BinExpr  : Expr { enum class Op {
        Add,Sub,Mul,Div,Mod, And,Or,Xor,Shl,Shr,
        Eq,Neq,Lt,Le,Gt,Ge, LAnd,LOr } op;
    ExprPtr lhs, rhs;
    // operator-overload dispatch (TYPE_SYSTEM.md Section 7): if sema resolves this
    // BinExpr to a registered overload (vec3_add, etc.), codegen emits a native
    // call instead of inline arithmetic.
    bool is_overload = false;
    void* overload_fn = nullptr;
};
struct UnaryExpr: Expr { enum class Op { Neg,Not,BitNot } op; ExprPtr operand; };
struct CastExpr : Expr { ExprPtr operand; std::shared_ptr<Type> to; };
struct CallExpr : Expr { std::string name; std::vector<ExprPtr> args;
                         // sema-resolved target (native fn ptr or script slot)
                         bool is_native = false; void* native_fn = nullptr;
                         int script_slot = -1;
                         // v0.5 cross-module: non-empty module_alias = a `mod::fn()` call.
                         // Sema resolves it against the linked-module export table; if the
                         // module/fn is not yet registered it's an unresolved external (deferred
                         // trap, MODULES.md §5). codegen stamps cross_module_id/slot when
                         // resolved (kind-2 call site); unresolved stays -1 (trap stub).
                         std::string module_alias;       // empty = same-module call
                         int cross_module_id = -1;        // registry module_id (resolved)
                         int cross_module_slot = -1;     // target fn's slot in that module
                         bool cross_module_unresolved = false; // sema couldn't resolve (deferred trap)
                         // method-call sugar: obj.method(args) desugars to
                         // method(obj, args) - receiver becomes arg[0]. Null
                         // for free-function calls. (BINDING_API.md Section 3)
                         ExprPtr receiver;
                         // compile-time assertion folding (sema.cpp's
                         // CallExpr check): set when this call is one of
                         // the assert_eq_* natives AND both arguments folded to
                         // compile-time constants that compare EQUAL - codegen
                         // emits nothing at all for this call (a mismatch is a
                         // sema compile error instead, never reaches codegen).
                         bool elided = false; };
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
struct AssignExpr: Expr { ExprPtr target; std::optional<BinExpr::Op> compound; ExprPtr value; };
struct TernaryExpr: Expr { ExprPtr cond, then_e, else_e; };
struct SizeofExpr: Expr { std::shared_ptr<Type> ty; };
struct StructLit : Expr { std::string type_name; std::vector<std::pair<std::string,ExprPtr>> fields; };

// statements
struct Stmt : Node {};
using StmtPtr = std::unique_ptr<Stmt>;

struct Block { std::vector<StmtPtr> stmts; };

struct ExprStmt : Stmt { ExprPtr expr; };
struct LetStmt  : Stmt { std::string name; std::shared_ptr<Type> ty; ExprPtr init; bool is_auto; bool is_const; };
struct IfStmt   : Stmt { ExprPtr cond; Block then_b; bool has_else=false; Block else_b; };
struct WhileStmt: Stmt { ExprPtr cond; Block body; };
struct ForStmt  : Stmt { std::unique_ptr<LetStmt> init; ExprPtr cond; ExprPtr step; Block body; };
struct DoWhileStmt:Stmt { Block body; ExprPtr cond; };
struct ReturnStmt:Stmt { ExprPtr value; };          // nullptr = void return
struct BreakStmt: Stmt {};
struct ContinueStmt:Stmt {};
struct DeferStmt : Stmt { ExprPtr expr; };
struct BlockStmt: Stmt { Block block; };
// one `case EXPR: stmts...` or `default: stmts...` clause. `value` is null
// iff is_default. No implicit break between clauses (C-style fallthrough) -
// codegen emits case bodies back to back; `break` exits the switch.
struct SwitchCase { ExprPtr value; bool is_default = false; Block body; };
struct SwitchStmt: Stmt { ExprPtr subject; std::vector<SwitchCase> cases; };

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
};

// v0.5 live-module link declaration (MODULES.md §6). `link "foo.em" as foo;`
// loads+registers a .em bundle at init; `link "foo" as foo;` links to an
// already-registered module. `as alias` is optional sugar (defaults to the
// module name / the file stem). Distinct from textual `import "path";` which
// inlines source before lexing (BUNDLING_AND_EM_MODULES.md §1.1).
struct LinkDecl {
    std::string target;      // module name OR .em file path
    std::string alias;       // the name scripts use (foo in foo::bar())
    bool is_file = false;     // true = target is a .em path (load it); false = link to registered
    Loc loc;
};

struct Program {
    std::vector<StructDecl> structs;
    std::vector<GlobalDecl> globals;
    std::vector<FuncDecl> funcs;
    std::vector<LinkDecl> links;   // v0.5 live-module link declarations (MODULES.md §6)
    // type store: owns synthesized Types created by sema (slices, adapted
    // literal types) so the raw `ty` pointers stashed on AST nodes survive
    // until codegen finishes (sema's local Checker would otherwise free them).
    std::vector<std::shared_ptr<Type>> type_store;
    // rodata store: owns the byte buffers for string literals (StringLit::
    // baked_ptr points into these) so they outlive sema, same as type_store.
    std::vector<std::shared_ptr<std::string>> rodata_store;
    // per-compile XOR key for string encryption. Nonzero =
    // strings are XOR-encrypted in rodata; codegen emits __str_decrypt
    // calls. The host sets this before sema; 0 disables encryption.
    uint8_t string_xor_key = 0;
};

} // namespace ember
