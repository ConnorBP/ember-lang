#include "parser.hpp"
#include <cassert>
#include <sstream>

namespace ember {

namespace {

struct P {
    std::vector<Token> toks;
    size_t i = 0;
    Program prog;
    std::vector<std::string> errs;
    std::vector<ParseErrorEntry> err_entries;  // same errors as `errs`, individually positioned

    const Token& peek() const { return toks[i]; }
    const Token& at(size_t k) const { return toks[std::min(k, toks.size()-1)]; }
    const Token& adv() { return toks[i < toks.size()-1 ? i++ : i]; }
    bool at(Tk k) const { return toks[i].kind == k; }
    bool accept(Tk k) { if (at(k)) { adv(); return true; } return false; }
    // End position (line/col right after the last character) of a token,
    // walking its text so multi-line tokens (e.g. strings with embedded
    // newlines) land on the line they actually end on.
    static void end_of(const Token& t, uint32_t& line, uint32_t& col) {
        line = t.line; col = t.col;
        for (char c : t.text) { if (c == '\n') { ++line; col = 1; } else ++col; }
    }

    const Token& expect(Tk k, const char* what) {
        if (!at(k)) {
            // Anchor the error right after the last successfully-consumed
            // token, not at whatever unrelated token the parser next
            // stumbles on - a missing ';' can otherwise get reported many
            // lines away, at the next token that finally fails to fit.
            if (i > 0) {
                uint32_t line, col;
                end_of(toks[i-1], line, col);
                throw ParseError(std::string("expected ")+what, line, col);
            }
            throw ParseError(std::string("expected ")+what, peek().line, peek().col);
        }
        return adv();
    }

    Loc loc(const Token& t) { return {t.line, t.col}; }

    // --- types ---
    std::shared_ptr<Type> parse_type() {
        std::shared_ptr<Type> t = std::make_shared<Type>();
        const Token& tk = peek();
        auto prim = [&](Prim p){ t->prim = p; adv(); };
        switch (tk.kind) {
        case Tk::Kw_void: prim(Prim::Void); break;
        case Tk::Kw_bool: prim(Prim::Bool); break;
        case Tk::Kw_i8: prim(Prim::I8); break;  case Tk::Kw_i16: prim(Prim::I16); break;
        case Tk::Kw_i32: prim(Prim::I32); break; case Tk::Kw_i64: prim(Prim::I64); break;
        case Tk::Kw_u8: prim(Prim::U8); break;  case Tk::Kw_u16: prim(Prim::U16); break;
        case Tk::Kw_u32: prim(Prim::U32); break; case Tk::Kw_u64: prim(Prim::U64); break;
        case Tk::Kw_f32: prim(Prim::F32); break; case Tk::Kw_f64: prim(Prim::F64); break;
        case Tk::Kw_fn:
            // v1.0 Tier 2 (plan_FUNCTION_REFS.md §2): a bare `fn` is the
            // function-handle type (i64 with is_fn_handle=true). A parameterized
            // `fn(i64)->i64` form is v2+; Tier 2's bare `fn` accepts any fn (the
            // call-target guard validates the handle at the call site, §5).
            // Unambiguous: parse_type is only called in type positions (param/
            // ret/let annotations, as/sizeof operands), never where a `fn`
            // declaration could start (that path consumes Kw_fn at parse_top).
            t->prim = Prim::I64; t->is_fn_handle = true; adv(); break;
        case Tk::Ident:
            // named struct type (resolved in sema) - or a registered handle
            // type alias (vec3, vec2, mat4 - i64 handles with operator overloads)
            t->prim = Prim::I64;  // handle types are i64 under the hood
            t->struct_name = tk.text;
            adv();
            break;
        default:
            throw ParseError("expected type", tk.line, tk.col);
        }
        // postfix array/slice suffixes
        while (at(Tk::LBracket)) {
            adv(); // '['
            if (accept(Tk::RBracket)) {
                auto e = std::make_shared<Type>(*t);
                *t = Type{};
                t->is_slice = true; t->elem = e;
            } else {
                const Token& n = expect(Tk::IntLit, "integer array size");
                auto e = std::make_shared<Type>(*t);
                *t = Type{};
                t->array_len = uint32_t(n.ivalue);
                t->elem = e;
                expect(Tk::RBracket, "']'");
            }
        }
        return t;
    }

    // --- annotations ---
    Annotation parse_annotation() {
        Annotation a;
        expect(Tk::At, "'@'");
        a.name = expect(Tk::Ident, "annotation name").text;
        if (accept(Tk::LParen)) {
            if (!at(Tk::RParen)) {
                do {
                    const Token& t = peek();
                    if (t.kind==Tk::IntLit || t.kind==Tk::FloatLit || t.kind==Tk::StringLit || t.kind==Tk::RawStringLit || t.kind==Tk::BoolLit) {
                        a.args.push_back(t.text); adv();
                    } else throw ParseError("expected literal in annotation args", t.line, t.col);
                } while (accept(Tk::Comma));
            }
            expect(Tk::RParen, "')'");
        }
        return a;
    }

    // --- declarations ---
    // parses `NAME: TYPE [= LITERAL]` - a default value must be a bare
    // literal (int/float/bool/string), not an arbitrary expression (see
    // DefaultValue's doc comment in ast.hpp for why).
    Param parse_param() {
        Param p;
        p.loc = loc(peek());
        p.name = expect(Tk::Ident, "parameter name").text;
        expect(Tk::Colon, "':'");
        p.ty = parse_type();
        if (accept(Tk::Assign)) {
            const Token& lt = peek();
            switch (lt.kind) {
            case Tk::IntLit:    p.default_val.kind = DefaultValue::Kind::Int;    p.default_val.i = lt.ivalue; adv(); break;
            case Tk::FloatLit:  p.default_val.kind = DefaultValue::Kind::Float;  p.default_val.f = lt.fvalue; p.default_val.f_is_f32 = lt.f32_suffix; adv(); break;
            case Tk::BoolLit:   p.default_val.kind = DefaultValue::Kind::Bool;   p.default_val.b = lt.bvalue; adv(); break;
            case Tk::StringLit:
            case Tk::RawStringLit: p.default_val.kind = DefaultValue::Kind::String; p.default_val.s = lt.text;   adv(); break;
            default:
                throw ParseError("default parameter value must be a literal (int/float/bool/string)", lt.line, lt.col);
            }
        }
        return p;
    }

    std::unique_ptr<FuncDecl> parse_func(std::vector<Annotation> anns) {
        auto f = std::make_unique<FuncDecl>();
        expect(Tk::Kw_fn, "'fn'");
        f->name = expect(Tk::Ident, "function name").text;
        f->annotations = std::move(anns);
        f->loc = loc(toks[i-1 > 0 ? 0 : 0]); // placeholder; real loc set below
        expect(Tk::LParen, "'('");
        if (!at(Tk::RParen)) {
            do { f->params.push_back(parse_param()); } while (accept(Tk::Comma));
        }
        expect(Tk::RParen, "')'");
        // trailing-defaults-only (once a defaulted param appears, every
        // subsequent one must be too) - checked here (structural shape),
        // not in sema, using each param's own loc for a precise error.
        {
            bool seen_default = false;
            for (auto& p : f->params) {
                if (p.default_val.kind != DefaultValue::Kind::None) seen_default = true;
                else if (seen_default) {
                    throw ParseError("parameter '" + p.name + "' without a default cannot follow "
                        "a parameter with a default value", p.loc.line, p.loc.col);
                }
            }
        }
        if (accept(Tk::Arrow)) {
            f->ret = parse_type();
        } else { f->ret = std::make_shared<Type>(type_void()); }
        f->loc = loc(peek());
        f->body = parse_block();
        return f;
    }

    std::unique_ptr<StructDecl> parse_struct() {
        auto s = std::make_unique<StructDecl>();
        expect(Tk::Kw_struct, "'struct'");
        s->name = expect(Tk::Ident, "struct name").text;
        s->loc = loc(toks[i-1]);
        expect(Tk::LBrace, "'{'");
        while (!at(Tk::RBrace) && !at(Tk::Eof)) {
            FieldDecl fd;
            fd.name = expect(Tk::Ident, "field name").text;
            expect(Tk::Colon, "':'");
            fd.ty = parse_type();
            s->fields.push_back(std::move(fd));
            expect(Tk::Semicolon, "';'");
        }
        expect(Tk::RBrace, "'}'");
        return s;
    }

    std::unique_ptr<GlobalDecl> parse_global() {
        auto g = std::make_unique<GlobalDecl>();
        expect(Tk::Kw_global, "'global'");
        g->is_const = false;
        g->name = expect(Tk::Ident, "global name").text;
        expect(Tk::Colon, "':'");
        g->ty = parse_type();
        expect(Tk::Assign, "'='");
        g->init = parse_expr();
        g->loc = loc(peek());
        expect(Tk::Semicolon, "';'");
        return g;
    }

    // v0.5 live-module link declaration (MODULES.md §6):
    //   link "foo.em" as foo;    -> load+register the .em bundle (is_file=true)
    //   link "foo" as foo;       -> link to an already-registered module (is_file=false)
    //   link "foo.em";           -> alias defaults to the file stem
    //   link "foo";              -> alias defaults to the module name
    // Distinct from textual `import "path";` (which inlines source pre-lex).
    std::unique_ptr<LinkDecl> parse_link() {
        auto ld = std::make_unique<LinkDecl>();
        const Token& t = peek();
        ld->loc = loc(t);
        expect(Tk::Kw_link, "'link'");
        const Token& target = expect(Tk::StringLit, "module name or .em path");
        ld->target = target.text;   // StringLit.text holds the decoded literal
        ld->is_file = ld->target.size() >= 3 &&
                      ld->target.compare(ld->target.size()-3, 3, ".em") == 0;
        if (accept(Tk::Kw_as)) {
            ld->alias = expect(Tk::Ident, "module alias").text;
        } else {
            // default alias: file stem (strip .em) or the module name itself
            if (ld->is_file) {
                auto pos = ld->target.find_last_of("/\\");
                std::string stem = (pos == std::string::npos) ? ld->target : ld->target.substr(pos+1);
                if (stem.size() >= 3 && stem.compare(stem.size()-3,3,".em")==0) stem.resize(stem.size()-3);
                ld->alias = stem;
            } else {
                ld->alias = ld->target;
            }
        }
        expect(Tk::Semicolon, "';'");
        return ld;
    }

    // Tier 1 script-side enum (plan_ENUMS.md Section 3.3):
    //   enum_decl := 'enum' IDENT '{' (variant (',' variant)* ','?)? '}'
    //   variant   := IDENT ('=' constexpr_int_expr)?
    // The explicit-value expr is parsed as a full parse_expr(); the
    // restriction to a compile-time integer constant is enforced by sema
    // (try_eval_const_i64), identical to how GlobalDecl::init and array
    // sizes are handled - the parser stays dumb. Trailing comma allowed.
    std::unique_ptr<EnumDecl> parse_enum() {
        auto e = std::make_unique<EnumDecl>();
        expect(Tk::Kw_enum, "'enum'");
        e->name = expect(Tk::Ident, "enum name").text;
        e->loc = loc(toks[i-1]);
        expect(Tk::LBrace, "'{'");
        if (!at(Tk::RBrace) && !at(Tk::Eof)) {
            do {
                const Token& v = expect(Tk::Ident, "enum variant name");
                EnumVariant ev;
                ev.name = v.text;
                ev.loc = loc(v);
                if (accept(Tk::Assign)) {
                    ev.explicit_value = parse_expr();
                }
                e->variants.push_back(std::move(ev));
            } while (accept(Tk::Comma) && !at(Tk::RBrace) && !at(Tk::Eof));
        }
        expect(Tk::RBrace, "'}'");
        return e;
    }

    // --- expressions (precedence climbing) ---
    ExprPtr parse_expr() { return parse_assign(); }

    ExprPtr parse_assign() {
        ExprPtr lhs = parse_ternary();
        Tk k = peek().kind;
        std::optional<BinExpr::Op> cop;
        switch (k) {
        case Tk::Assign: break;
        case Tk::PlusAssign: cop = BinExpr::Op::Add; break;
        case Tk::MinusAssign: cop = BinExpr::Op::Sub; break;
        case Tk::StarAssign: cop = BinExpr::Op::Mul; break;
        case Tk::SlashAssign: cop = BinExpr::Op::Div; break;
        case Tk::PercentAssign: cop = BinExpr::Op::Mod; break;
        // bitwise / shift compound assignments (new): these let @obf("mba")
        // be exercised on the compound-assignment path for And/Or/Xor/Shl/Shr.
        case Tk::AmpAssign: cop = BinExpr::Op::And; break;
        case Tk::PipeAssign: cop = BinExpr::Op::Or; break;
        case Tk::CaretAssign: cop = BinExpr::Op::Xor; break;
        case Tk::ShlAssign: cop = BinExpr::Op::Shl; break;
        case Tk::ShrAssign: cop = BinExpr::Op::Shr; break;
        default: return lhs;
        }
        adv();
        ExprPtr rhs = parse_assign();
        auto a = std::make_unique<AssignExpr>();
        a->loc = lhs->loc;
        a->target = std::move(lhs);
        a->compound = cop;
        a->value = std::move(rhs);
        return a;
    }

    ExprPtr parse_ternary() {
        ExprPtr c = parse_or();
        if (!accept(Tk::Question)) return c;
        ExprPtr t = parse_expr();
        expect(Tk::Colon, "':'");
        ExprPtr e = parse_ternary();
        auto r = std::make_unique<TernaryExpr>();
        r->loc = c->loc; r->cond = std::move(c); r->then_e = std::move(t); r->else_e = std::move(e);
        return r;
    }

    static BinExpr::Op bin_of(Tk k) {
        switch (k) {
        case Tk::Plus: return BinExpr::Op::Add; case Tk::Minus: return BinExpr::Op::Sub;
        case Tk::Star: return BinExpr::Op::Mul; case Tk::Slash: return BinExpr::Op::Div;
        case Tk::Percent: return BinExpr::Op::Mod;
        case Tk::Amp: return BinExpr::Op::And; case Tk::Pipe: return BinExpr::Op::Or;
        case Tk::Caret: return BinExpr::Op::Xor;
        case Tk::Shl: return BinExpr::Op::Shl; case Tk::Shr: return BinExpr::Op::Shr;
        case Tk::Eq: return BinExpr::Op::Eq; case Tk::Neq: return BinExpr::Op::Neq;
        case Tk::Lt: return BinExpr::Op::Lt; case Tk::Le: return BinExpr::Op::Le;
        case Tk::Gt: return BinExpr::Op::Gt; case Tk::Ge: return BinExpr::Op::Ge;
        case Tk::AndAnd: return BinExpr::Op::LAnd; case Tk::OrOr: return BinExpr::Op::LOr;
        default: return BinExpr::Op::Add; // unreachable; caller checks
        }
    }

    // generic binary-left-assoc level helper
    ExprPtr binlevel(ExprPtr (P::*lower)(), bool (P::*pred)(Tk) const, Tk) {
        ExprPtr lhs = (this->*lower)();
        while ((this->*pred)(peek().kind)) {
            Tk op = adv().kind;
            ExprPtr rhs = (this->*lower)();
            auto b = std::make_unique<BinExpr>();
            b->loc = lhs->loc; b->op = bin_of(op); b->lhs = std::move(lhs); b->rhs = std::move(rhs);
            lhs = std::move(b);
        }
        return lhs;
    }
    bool p_or(Tk k)const   { return k==Tk::OrOr; }
    bool p_and(Tk k)const  { return k==Tk::AndAnd; }
    bool p_bor(Tk k)const  { return k==Tk::Pipe; }
    bool p_bxor(Tk k)const { return k==Tk::Caret; }
    bool p_band(Tk k)const { return k==Tk::Amp; }
    bool p_eq(Tk k)const   { return k==Tk::Eq||k==Tk::Neq; }
    bool p_rel(Tk k)const  { return k==Tk::Lt||k==Tk::Le||k==Tk::Gt||k==Tk::Ge; }
    bool p_shift(Tk k)const{ return k==Tk::Shl||k==Tk::Shr; }
    bool p_add(Tk k)const  { return k==Tk::Plus||k==Tk::Minus; }
    bool p_mul(Tk k)const  { return k==Tk::Star||k==Tk::Slash||k==Tk::Percent; }

    ExprPtr parse_or()    { return binlevel(&P::parse_and, &P::p_or, Tk::OrOr); }
    ExprPtr parse_and()   { return binlevel(&P::parse_bxor, &P::p_and, Tk::AndAnd); }
    ExprPtr parse_bxor()  { return binlevel(&P::parse_bor, &P::p_bxor, Tk::Caret); }
    ExprPtr parse_bor()   { return binlevel(&P::parse_band, &P::p_bor, Tk::Pipe); }
    ExprPtr parse_band()  { return binlevel(&P::parse_eq, &P::p_band, Tk::Amp); }
    ExprPtr parse_eq()    { return binlevel(&P::parse_rel, &P::p_eq, Tk::Eq); }
    ExprPtr parse_rel()   { return binlevel(&P::parse_shift, &P::p_rel, Tk::Lt); }
    ExprPtr parse_shift() { return binlevel(&P::parse_add, &P::p_shift, Tk::Shl); }
    ExprPtr parse_add()   { return binlevel(&P::parse_mul, &P::p_add, Tk::Plus); }
    ExprPtr parse_mul()   { return binlevel(&P::parse_cast, &P::p_mul, Tk::Star); }

    ExprPtr parse_cast() {
        ExprPtr e = parse_unary();
        while (at(Tk::Kw_as)) {
            adv();
            auto t = parse_type();
            auto c = std::make_unique<CastExpr>();
            c->loc = e->loc; c->operand = std::move(e); c->to = std::move(t);
            e = std::move(c);
        }
        // sizeof(T) / offsetof(T, field) handled in primary (prefix kw), but also allow
        // trailing sizeof? No - sizeof is prefix-only per grammar. Skip.
        return e;
    }

    ExprPtr parse_unary() {
        Tk k = peek().kind;
        if (k==Tk::Minus || k==Tk::Not || k==Tk::Tilde) {
            adv();
            ExprPtr e = parse_unary();
            auto u = std::make_unique<UnaryExpr>();
            u->loc = e->loc;
            u->op = (k==Tk::Minus)?UnaryExpr::Op::Neg : (k==Tk::Not)?UnaryExpr::Op::Not : UnaryExpr::Op::BitNot;
            u->operand = std::move(e);
            return u;
        }
        // v1.0 Tier 2 (plan_FUNCTION_REFS.md §3.1): prefix `&` takes a function
        // handle. `&fn_name` is a compile-time reification (sema bakes the slot
        // as an i64 literal), not a runtime deref. Parse the operand as unary so
        // `&&fib`-style nesting is structurally parseable then rejected by sema.
        if (k==Tk::Amp) {
            adv();
            ExprPtr e = parse_unary();
            auto h = std::make_unique<FnHandleExpr>();
            h->loc = e->loc;
            h->operand = std::move(e);
            return h;
        }
        // prefix ++/-- desugars to assign
        if (k==Tk::Inc || k==Tk::Dec) {
            adv();
            ExprPtr e = parse_unary();
            auto a = std::make_unique<AssignExpr>();
            a->loc = e->loc; a->target = std::move(e);
            a->compound = (k==Tk::Inc)?BinExpr::Op::Add:BinExpr::Op::Sub;
            auto one = std::make_unique<IntLit>(); one->loc = a->loc; one->v = 1;
            a->value = std::move(one);
            return a;
        }
        return parse_postfix();
    }

    ExprPtr parse_postfix() {
        ExprPtr e = parse_primary();
        for (;;) {
            Tk k = peek().kind;
            if (k==Tk::DoubleColon) {
                // v0.5 cross-module selector `mod::fn`. The base `e` must be an
                // Ident (the module alias); build a CallExpr with module_alias
                // set + the fn name, args filled by the following `(` case.
                //
                // Tier 1 enums (plan_ENUMS.md Section 3.5): `E::A` (a value, no
                // parens) is a NEW postfix `::` outcome that is NOT a call. The
                // one-token lookahead split is: `::` + Ident + not-`(` = enum
                // variant access (EnumAccessExpr); `::` + Ident + `(` = the
                // existing `mod::fn(args)` cross-module call path, unchanged.
                auto id = dynamic_cast<Ident*>(e.get());
                if (!id) throw ParseError("'::' must follow a module alias name", peek().line, peek().col);
                adv(); // '::'
                const Token& fn = expect(Tk::Ident, "name after '::'");
                if (at(Tk::LParen)) {
                    // mod::fn(args) - cross-module call. Hand the half-built
                    // CallExpr to the next iteration's '(' case below.
                    auto c = std::make_unique<CallExpr>();
                    c->loc = e->loc;
                    c->module_alias = id->name;
                    c->name = fn.text;
                    e = std::move(c);
                    continue;  // the next iteration handles '(' (args)
                }
                // E::A - enum variant access (a value, no call). sema rewrites
                // this to an IntLit in place (plan_ENUMS.md Section 4.2/5).
                auto ea = std::make_unique<EnumAccessExpr>();
                ea->loc = e->loc;
                ea->enum_name = id->name;
                ea->variant = fn.text;
                e = std::move(ea);
                continue;
            }
            if (k==Tk::LParen) {
                // call: e(args). Three forms:
                //   name(args)            - free function call (name is an Ident)
                //   obj.method(args)      - method-call sugar; e is a FieldExpr,
                //                          desugars to method(obj, args) with the
                //                          receiver as arg[0] (BINDING_API.md Section 3)
                //   mod::fn(args)         - cross-module call; e is already a CallExpr
                //                          with module_alias set (built by the '::' case above)
                std::unique_ptr<CallExpr> c;
                if (auto existing = dynamic_cast<CallExpr*>(e.get())) {
                    // mod::fn(args): take the half-built CallExpr, fill its args.
                    c = std::unique_ptr<CallExpr>(static_cast<CallExpr*>(e.release()));
                } else if (auto id = dynamic_cast<Ident*>(e.get())) {
                    c = std::make_unique<CallExpr>();
                    c->name = id->name; c->loc = e->loc;
                } else if (auto fld = dynamic_cast<FieldExpr*>(e.get())) {
                    c = std::make_unique<CallExpr>();
                    c->name = fld->field; c->loc = e->loc;
                    c->receiver = std::move(fld->base);
                } else {
                    // v1.0 Tier 2 first-class call (plan_FUNCTION_REFS.md §3.2):
                    // <expr>(args) where <expr> is none of the three named forms
                    // above — a call through a RUNTIME i64 handle. Sema types the
                    // target as a fn handle; codegen validates it against the
                    // registered-fn allowlist before dispatch (REDSHELL guard #6).
                    // Lifts the old `throw "call target must be..."` that V2
                    // (i64-as-call-target) turned into a sema error.
                    c = std::make_unique<CallExpr>();
                    c->loc = e->loc;
                    c->name.clear();                  // empty name = not a named call
                    c->indirect_target = std::move(e); // the runtime handle
                }
                adv(); // '('
                if (!at(Tk::RParen)) {
                    do { c->args.push_back(parse_expr()); } while (accept(Tk::Comma));
                }
                expect(Tk::RParen, "')'");
                e = std::move(c);
            } else if (k==Tk::LBracket) {
                adv(); // '['
                if (at(Tk::DotDot)) {
                    // arr[..] whole-array view (COMPILER_PIPELINE.md Section 2 view_suffix)
                    adv(); // '..'
                    expect(Tk::RBracket, "']'");
                    auto v = std::make_unique<ViewExpr>(); v->loc = e->loc; v->base = std::move(e);
                    e = std::move(v);
                } else {
                    ExprPtr idx = parse_expr();
                    expect(Tk::RBracket, "']'");
                    auto x = std::make_unique<IndexExpr>(); x->loc = e->loc; x->base = std::move(e); x->index = std::move(idx);
                    e = std::move(x);
                }
            } else if (k==Tk::Dot) {
                adv();
                std::string f = expect(Tk::Ident, "field name").text;
                auto fl = std::make_unique<FieldExpr>(); fl->loc = e->loc; fl->base = std::move(e); fl->field = f;
                e = std::move(fl);
            } else if (k==Tk::Inc || k==Tk::Dec) {
                // postfix ++/-- shares assignment lowering but preserves the old value.
                adv();
                auto a = std::make_unique<AssignExpr>();
                a->loc = e->loc; a->target = std::move(e); a->postfix = true;
                a->compound = (k==Tk::Inc)?BinExpr::Op::Add:BinExpr::Op::Sub;
                auto one = std::make_unique<IntLit>(); one->loc = a->loc; one->v = 1;
                a->value = std::move(one);
                e = std::move(a);
            } else break;
        }
        return e;
    }

    // Maps a (line,col) reported by a sub-lex/sub-parse of an f-string
    // interpolation's substring (numbered from 1,1 for that substring) back
    // into the outer file's coordinates, given the substring's own starting
    // position (base_line,base_col) in the outer file.
    static Loc remap_loc(uint32_t base_line, uint32_t base_col, uint32_t sub_line, uint32_t sub_col) {
        if (sub_line <= 1) return {base_line, uint32_t(base_col + sub_col - 1)};
        return {base_line + (sub_line - 1), sub_col};
    }

    // Splits a raw (brace/quote-aware, un-decoded - see the lexer's
    // FStringLit scan) f-string body into alternating literal-text and
    // {expr} segments, desugaring to a left-associative '+' chain over
    // StringLit segments and __fstring_to_string-wrapped interpolated
    // segments (Item D). Reuses the fully-working `string + string` concat
    // pipeline (sema's BinExpr overload dispatch, codegen's overload-call
    // emission) with zero sema/codegen changes for the "glue" itself - see
    // sema.cpp's __fstring_to_string special case for how each wrapper gets
    // resolved to the right string_from_* native once its inner
    // sub-expression's type is known.
    ExprPtr build_fstring_expr(const std::string& body, uint32_t ftok_line, uint32_t ftok_col) {
        std::vector<ExprPtr> segments;
        size_t n = body.size(), pos = 0;
        uint32_t line = ftok_line, col = ftok_col + 2; // +2 skips the leading f"
        std::string lit;
        uint32_t lit_line = line, lit_col = col;

        auto step = [&](char ch) { if (ch == '\n') { ++line; col = 1; } else { ++col; } ++pos; };
        auto flush_literal = [&]() {
            auto s = std::make_unique<StringLit>();
            s->loc = {lit_line, lit_col};
            s->s = lit;
            segments.push_back(std::move(s));
            lit.clear();
        };

        while (pos < n) {
            char ch = body[pos];
            if (ch != '{') {
                if (lit.empty()) { lit_line = line; lit_col = col; }
                lit.push_back(ch);
                step(ch);
                continue;
            }
            if (!lit.empty()) flush_literal();
            step(ch); // consume '{' (already de-doubled by the lexer at depth 0, so this is always a real interpolation start)
            uint32_t expr_line = line, expr_col = col; // first char of the sub-expression
            int depth = 1;
            size_t expr_start = pos;
            while (pos < n && depth > 0) {
                char c2 = body[pos];
                if (c2 == '"') {
                    // skip a nested string literal's raw span verbatim
                    step(c2);
                    while (pos < n && body[pos] != '"') {
                        if (body[pos] == '\\' && pos + 1 < n) { step(body[pos]); step(body[pos]); }
                        else step(body[pos]);
                    }
                    if (pos < n) step(body[pos]); // closing quote
                    continue;
                }
                if (c2 == '{') { ++depth; step(c2); continue; }
                if (c2 == '}') { --depth; if (depth == 0) break; step(c2); continue; }
                step(c2);
            }
            if (pos >= n || depth != 0)
                throw ParseError("unterminated interpolation in f-string", expr_line, expr_col);
            std::string sub = body.substr(expr_start, pos - expr_start);
            step(body[pos]); // consume the matching '}'

            auto lr = tokenize(sub, "<fstring>");
            if (!lr.ok) {
                Loc rl = remap_loc(expr_line, expr_col, lr.err_line, lr.err_col);
                throw ParseError(std::string("in f-string interpolation: ") + lr.error, rl.line, rl.col);
            }
            P sub_p; sub_p.toks = std::move(lr.toks);
            ExprPtr sub_expr;
            try {
                sub_expr = sub_p.parse_expr();
            } catch (ParseError& pe) {
                Loc rl = remap_loc(expr_line, expr_col, pe.line, pe.col);
                throw ParseError(pe.what(), rl.line, rl.col);
            }
            if (sub_p.i != sub_p.toks.size() - 1) {
                const Token& trailing = sub_p.peek();
                Loc rl = remap_loc(expr_line, expr_col, trailing.line, trailing.col);
                throw ParseError("unexpected trailing content in f-string interpolation", rl.line, rl.col);
            }

            // Wrapped unconditionally (even if already a `string`) - sema
            // resolves this sentinel to a real converter or a trivial
            // identity passthrough once the sub-expression's type is known;
            // this keeps the parser from needing to know types at all.
            auto call = std::make_unique<CallExpr>();
            call->name = "__fstring_to_string";
            call->loc = {expr_line, expr_col};
            call->args.push_back(std::move(sub_expr));
            segments.push_back(std::move(call));
        }
        if (!lit.empty()) flush_literal();
        if (segments.empty()) {
            auto s = std::make_unique<StringLit>();
            s->loc = {ftok_line, ftok_col};
            s->s = "";
            return s;
        }
        ExprPtr result = std::move(segments[0]);
        for (size_t k = 1; k < segments.size(); ++k) {
            auto b = std::make_unique<BinExpr>();
            b->loc = result->loc; b->op = BinExpr::Op::Add;
            b->lhs = std::move(result); b->rhs = std::move(segments[k]);
            result = std::move(b);
        }
        return result;
    }

    ExprPtr parse_primary() {
        const Token& t = peek();
        switch (t.kind) {
        case Tk::IntLit: { auto e=std::make_unique<IntLit>(); e->loc=loc(t); e->v=t.ivalue; adv(); return e; }
        case Tk::FloatLit: { auto e=std::make_unique<FloatLit>(); e->loc=loc(t); e->v=t.fvalue; e->is_f32=t.f32_suffix; adv(); return e; }
        case Tk::BoolLit: { auto e=std::make_unique<BoolLit>(); e->loc=loc(t); e->v=t.bvalue; adv(); return e; }
        case Tk::StringLit:
        case Tk::RawStringLit: { auto e=std::make_unique<StringLit>(); e->loc=loc(t); e->s=t.text; adv(); return e; }
        case Tk::FStringLit: { uint32_t l=t.line, cc=t.col; std::string body=t.text; adv(); return build_fstring_expr(body, l, cc); }
        case Tk::Ident: {
            std::string name = t.text;
            Loc l = loc(t);
            adv();
            if (at(Tk::LBrace)) {
                // struct literal: TypeName { field: expr, field: expr, ... }
                // Unambiguous in this grammar: every other place an
                // identifier precedes a block (if/while/for/switch bodies)
                // has an intervening ')' that breaks the adjacency.
                adv(); // '{'
                auto sl = std::make_unique<StructLit>();
                sl->loc = l;
                sl->type_name = name;
                if (!at(Tk::RBrace)) {
                    do {
                        std::string fname = expect(Tk::Ident, "field name").text;
                        expect(Tk::Colon, "':'");
                        ExprPtr fval = parse_expr();
                        sl->fields.emplace_back(std::move(fname), std::move(fval));
                    } while (accept(Tk::Comma));
                }
                expect(Tk::RBrace, "'}'");
                return sl;
            }
            auto e = std::make_unique<Ident>(); e->loc = l; e->name = name;
            return e;
        }
        case Tk::LParen: { adv(); ExprPtr e=parse_expr(); expect(Tk::RParen,"')'"); return e; }
        case Tk::Kw_sizeof: { adv(); expect(Tk::LParen,"'('"); auto ty=parse_type(); expect(Tk::RParen,"')'"); auto e=std::make_unique<SizeofExpr>(); e->loc=loc(t); e->ty=std::move(ty); return e; }
        case Tk::Kw_offsetof: {
            adv(); expect(Tk::LParen,"'('"); auto ty=parse_type(); expect(Tk::Comma,"','");
            std::string field = expect(Tk::Ident, "field name").text; expect(Tk::RParen,"')'");
            auto e=std::make_unique<OffsetofExpr>(); e->loc=loc(t); e->ty=std::move(ty); e->field=std::move(field); return e;
        }
        default:
            throw ParseError(std::string("unexpected token '")+tok_spelling(t.kind)+"'", t.line, t.col);
        }
    }

    // --- statements ---
    // parses `let|auto|const [mut] NAME [: TYPE] [= EXPR] ;` (the leading
    // keyword token `t` has already been peeked but not consumed). Shared by
    // the top-level let-statement and a `for` loop's init clause.
    // Bindings are immutable by default (Rust-style) - `mut` opts a `let`/
    // `auto` into reassignment; `const` is already immutable, so `const mut`
    // is a contradiction and rejected.
    std::unique_ptr<LetStmt> parse_let_stmt(const Token& t) {
        auto s = std::make_unique<LetStmt>();
        s->is_auto = (t.kind==Tk::Kw_auto);
        bool explicit_const = (t.kind==Tk::Kw_const);
        adv();
        bool is_mut = accept(Tk::Kw_mut);
        if (explicit_const && is_mut)
            throw ParseError("'const' cannot be combined with 'mut' - a const binding is already immutable", t.line, t.col);
        s->is_const = !is_mut;
        s->name = expect(Tk::Ident, "variable name").text;
        if (!s->is_auto) {
            if (accept(Tk::Colon)) {
                s->ty = parse_type();
            } else {
                // no ':TYPE' given on a plain `let`/`const` - infer from the
                // initializer exactly like `auto` does (sema branches
                // correctly on is_auto already, no further changes needed).
                // The keyword actually typed was `let`/`const`, not `auto` -
                // is_auto here only means "no declared type; infer it".
                s->is_auto = true;
            }
        }
        s->loc = loc(t);
        // explicitly-typed `let`/`const` may omit the initializer (default
        // zero-fill - see sema/codegen) - useful for a fixed array local
        // that's filled in by subsequent indexed assignment. `auto` still
        // requires one (nothing to infer the type from otherwise).
        if (accept(Tk::Assign)) {
            s->init = parse_expr();
        } else if (s->is_auto) {
            const char* kw = (t.kind==Tk::Kw_auto) ? "auto" : (t.kind==Tk::Kw_const) ? "const" : "let";
            throw ParseError(
                std::string("'") + kw + "' declaration with no type annotation requires an initializer "
                "to infer from (add ': TYPE' or an initializer)", t.line, t.col);
        }
        expect(Tk::Semicolon, "';'");
        return s;
    }

    StmtPtr parse_stmt() {
        const Token& t = peek();
        switch (t.kind) {
        case Tk::Kw_let: case Tk::Kw_auto: case Tk::Kw_const:
            return parse_let_stmt(t);
        case Tk::Kw_defer: { adv(); auto s=std::make_unique<DeferStmt>(); s->loc=loc(t); s->expr=parse_expr(); expect(Tk::Semicolon,"';'"); return s; }
        case Tk::Kw_if: { adv(); auto s=std::make_unique<IfStmt>(); s->loc=loc(t); expect(Tk::LParen,"'('"); s->cond=parse_expr(); expect(Tk::RParen,"')'"); s->then_b=parse_block(); if (accept(Tk::Kw_else)) { s->has_else=true; if (at(Tk::Kw_if)) { auto sub=parse_stmt(); // else if
            // wrap single stmt in block
            IfStmt* is = dynamic_cast<IfStmt*>(sub.get());
            if (is) { s->else_b.stmts.push_back(std::move(sub)); }
            else { s->else_b.stmts.push_back(std::move(sub)); }
        } else { s->else_b = parse_block(); } } return s; }
        case Tk::Kw_while: { adv(); auto s=std::make_unique<WhileStmt>(); s->loc=loc(t); expect(Tk::LParen,"'('"); s->cond=parse_expr(); expect(Tk::RParen,"')'"); s->body=parse_block(); return s; }
        case Tk::Kw_do: { adv(); auto s=std::make_unique<DoWhileStmt>(); s->loc=loc(t); s->body=parse_block(); expect(Tk::Kw_while,"'while'"); expect(Tk::LParen,"'('"); s->cond=parse_expr(); expect(Tk::RParen,"')'"); expect(Tk::Semicolon,"';'"); return s; }
        case Tk::Kw_for: {
            // for ( [let NAME [: TYPE] [= EXPR]] ; [COND] ; [STEP] ) { BODY }
            adv();
            auto s = std::make_unique<ForStmt>();
            s->loc = loc(t);
            expect(Tk::LParen, "'('");
            if (at(Tk::Kw_let) || at(Tk::Kw_auto) || at(Tk::Kw_const)) {
                const Token& initTok = peek();
                s->init = parse_let_stmt(initTok);   // consumes its own trailing ';'
            } else {
                expect(Tk::Semicolon, "';'");
            }
            if (!at(Tk::Semicolon)) s->cond = parse_expr();
            expect(Tk::Semicolon, "';'");
            if (!at(Tk::RParen)) s->step = parse_expr();
            expect(Tk::RParen, "')'");
            s->body = parse_block();
            return s;
        }
        case Tk::Kw_switch: {
            // switch (expr) { case LIT: stmts...  default: stmts... }
            // Sema requires each nonempty case to terminate with break/return;
            // parsing still preserves each body independently for diagnostics.
            adv();
            auto s = std::make_unique<SwitchStmt>();
            s->loc = loc(t);
            expect(Tk::LParen, "'('");
            s->subject = parse_expr();
            expect(Tk::RParen, "')'");
            expect(Tk::LBrace, "'{'");
            while (!at(Tk::RBrace) && !at(Tk::Eof)) {
                SwitchCase c;
                if (accept(Tk::Kw_default)) {
                    c.is_default = true;
                } else {
                    expect(Tk::Kw_case, "'case' or 'default'");
                    c.value = parse_expr();
                }
                expect(Tk::Colon, "':'");
                while (!at(Tk::Kw_case) && !at(Tk::Kw_default) && !at(Tk::RBrace) && !at(Tk::Eof)) {
                    c.body.stmts.push_back(parse_stmt());
                }
                s->cases.push_back(std::move(c));
            }
            expect(Tk::RBrace, "'}'");
            return s;
        }
        case Tk::Kw_return: { adv(); auto s=std::make_unique<ReturnStmt>(); s->loc=loc(t); if (!at(Tk::Semicolon)) s->value=parse_expr(); expect(Tk::Semicolon,"';'"); return s; }
        case Tk::Kw_break: { adv(); expect(Tk::Semicolon,"';'"); auto s=std::make_unique<BreakStmt>(); s->loc=loc(t); return s; }
        case Tk::Kw_continue: { adv(); expect(Tk::Semicolon,"';'"); auto s=std::make_unique<ContinueStmt>(); s->loc=loc(t); return s; }
        case Tk::LBrace: { auto s=std::make_unique<BlockStmt>(); s->loc=loc(t); s->block=parse_block(); return s; }
        default: break;
        }
        // expression statement
        auto s = std::make_unique<ExprStmt>();
        s->loc = loc(t);
        s->expr = parse_expr();
        expect(Tk::Semicolon, "';'");
        return s;
    }

    Block parse_block() {
        Block b;
        expect(Tk::LBrace, "'{'");
        while (!at(Tk::RBrace) && !at(Tk::Eof)) {
            try { b.stmts.push_back(parse_stmt()); }
            catch (const ParseError& e) {
                errs.push_back(e.what() + std::string(" @ line ") + std::to_string(e.line));
                err_entries.push_back({e.what(), e.line, e.col});
                // sync to next ';' or '}'
                while (!at(Tk::Semicolon) && !at(Tk::RBrace) && !at(Tk::Eof)) adv();
                accept(Tk::Semicolon);
            }
        }
        expect(Tk::RBrace, "'}'");
        return b;
    }

    void parse_program() {
        while (!at(Tk::Eof)) {
            try {
                // gather annotations
                std::vector<Annotation> anns;
                while (at(Tk::At)) anns.push_back(parse_annotation());
                if (at(Tk::Kw_fn)) {
                    prog.funcs.push_back(std::move(*parse_func(std::move(anns))));
                } else if (at(Tk::Kw_struct)) {
                    if (!anns.empty()) throw ParseError("annotations on structs not supported v1", peek().line, peek().col);
                    prog.structs.push_back(std::move(*parse_struct()));
                } else if (at(Tk::Kw_global) || at(Tk::Kw_const)) {
                    if (!anns.empty()) throw ParseError("annotations on globals not supported v1", peek().line, peek().col);
                    bool is_const = accept(Tk::Kw_const);
                    if (!is_const) accept(Tk::Kw_global);
                    auto g = std::make_unique<GlobalDecl>();
                    g->name = expect(Tk::Ident, "global name").text;
                    expect(Tk::Colon, "':'");
                    g->ty = parse_type();
                    expect(Tk::Assign, "'='");
                    g->init = parse_expr();
                    g->loc = loc(peek());
                    g->is_const = is_const;
                    expect(Tk::Semicolon, "';'");
                    prog.globals.push_back(std::move(*g));
                } else if (at(Tk::Kw_link)) {
                    if (!anns.empty()) throw ParseError("annotations on link not supported", peek().line, peek().col);
                    prog.links.push_back(std::move(*parse_link()));
                } else if (at(Tk::Kw_enum)) {
                    if (!anns.empty()) throw ParseError("annotations on enums not supported v1", peek().line, peek().col);
                    prog.enums.push_back(std::move(*parse_enum()));
                } else if (at(Tk::Semicolon)) {
                    adv(); // empty top-level stmt
                } else {
                    throw ParseError("expected fn/struct/global at top level", peek().line, peek().col);
                }
            } catch (const ParseError& e) {
                errs.push_back(std::string(e.what()) + " @ line " + std::to_string(e.line));
                err_entries.push_back({e.what(), e.line, e.col});
                // sync: skip to next ';' or '}', consuming it; guarantee progress
                // so a stray '}' (from a body we synced into) can't stall the loop.
                while (!at(Tk::Semicolon) && !at(Tk::RBrace) && !at(Tk::Eof)) adv();
                if (at(Tk::Semicolon) || at(Tk::RBrace)) adv();
                else if (!at(Tk::Eof)) adv();
            }
        }
    }
};

} // namespace

ParseResult parse(std::vector<Token> toks) {
    P p; p.toks = std::move(toks);
    try { p.parse_program(); }
    catch (const ParseError& e) {
        p.errs.push_back(std::string(e.what()) + " @ line " + std::to_string(e.line));
        p.err_entries.push_back({e.what(), e.line, e.col});
    }
    ParseResult r;
    r.program = std::move(p.prog);
    if (!p.errs.empty()) {
        r.ok = false;
        std::ostringstream os; for (auto& e : p.errs) os << e << "\n";
        r.error = os.str();
        r.errors = std::move(p.err_entries);
        r.err_line = r.errors.front().line;
        r.err_col = r.errors.front().col;
    }
    return r;
}

} // namespace ember
