// Formula.cpp — formula parser, block bytecode evaluator and presets.
// See Formula.h for the grammar and the variable rules.
#include "Formula.h"

#include <cctype>
#include <memory>
#include <optional>

namespace tincture::wtedit
{
namespace
{
enum Op : uint8_t
{
    OpConst = 0, OpVar, OpNeg, OpNot,
    OpAdd, OpSub, OpMul, OpDiv, OpMod, OpPow, OpLt, OpGt, OpLe, OpGe, OpEq, OpNe, OpAnd, OpOr,
    OpSelect, OpFunc1, OpMin, OpMax, OpSum, OpAvg
};

enum Fn1 : uint8_t
{
    FSin = 0, FCos, FTan, FAsin, FAcos, FAtan, FSinh, FCosh, FTanh, FAsinh, FAcosh, FAtanh,
    FLog2, FLog10, FLn, FExp, FSqrt, FSign, FRint, FAbs
};

struct FuncInfo { const char* name; int argc; uint8_t op; uint8_t fn; };   // argc -1 = variadic
const FuncInfo funcs[] {
    { "sin", 1, OpFunc1, FSin }, { "cos", 1, OpFunc1, FCos }, { "tan", 1, OpFunc1, FTan },
    { "asin", 1, OpFunc1, FAsin }, { "acos", 1, OpFunc1, FAcos }, { "atan", 1, OpFunc1, FAtan },
    { "sinh", 1, OpFunc1, FSinh }, { "cosh", 1, OpFunc1, FCosh }, { "tanh", 1, OpFunc1, FTanh },
    { "asinh", 1, OpFunc1, FAsinh }, { "acosh", 1, OpFunc1, FAcosh }, { "atanh", 1, OpFunc1, FAtanh },
    { "log2", 1, OpFunc1, FLog2 }, { "log10", 1, OpFunc1, FLog10 }, { "log", 1, OpFunc1, FLog10 },
    { "ln", 1, OpFunc1, FLn }, { "exp", 1, OpFunc1, FExp }, { "sqrt", 1, OpFunc1, FSqrt },
    { "sign", 1, OpFunc1, FSign }, { "rint", 1, OpFunc1, FRint }, { "abs", 1, OpFunc1, FAbs },
    { "min", -1, OpMin, 0 }, { "max", -1, OpMax, 0 }, { "sum", -1, OpSum, 0 }, { "avg", -1, OpAvg, 0 },
};

struct VarInfo { const char* name; int var; float value; };   // var -1 = constant
const VarInfo vars[] {
    { "pi", -1, 3.14159265358979323846f }, { "e", -1, 2.71828182845904523536f },
    { "x", Formula::X, 0 }, { "w", Formula::W, 0 }, { "y", Formula::Y, 0 }, { "z", Formula::Z, 0 },
    { "q", Formula::Q, 0 }, { "in", Formula::In, 0 }, { "sel", Formula::Sel, 0 }, { "rand", Formula::Rand, 0 },
};

inline float apply1 (uint8_t fn, float v) noexcept
{
    switch (fn)
    {
        case FSin:   return std::sin (v);
        case FCos:   return std::cos (v);
        case FTan:   return std::tan (v);
        case FAsin:  return std::asin (v);
        case FAcos:  return std::acos (v);
        case FAtan:  return std::atan (v);
        case FSinh:  return std::sinh (v);
        case FCosh:  return std::cosh (v);
        case FTanh:  return std::tanh (v);
        case FAsinh: return std::asinh (v);
        case FAcosh: return std::acosh (v);
        case FAtanh: return std::atanh (v);
        case FLog2:  return std::log2 (v);
        case FLog10: return std::log10 (v);
        case FLn:    return std::log (v);
        case FExp:   return std::exp (v);
        case FSqrt:  return std::sqrt (v);
        case FSign:  return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f);
        case FRint:  return std::floor (v + 0.5f);
        case FAbs:   return std::abs (v);
        default:     return 0.0f;
    }
}

inline float apply2 (uint8_t op, float a, float b) noexcept
{
    switch (op)
    {
        case OpAdd: return a + b;
        case OpSub: return a - b;
        case OpMul: return a * b;
        case OpDiv: return a / b;
        case OpMod: return std::fmod (a, b);
        case OpPow: return std::pow (a, b);
        case OpLt:  return a < b ? 1.0f : 0.0f;
        case OpGt:  return a > b ? 1.0f : 0.0f;
        case OpLe:  return a <= b ? 1.0f : 0.0f;
        case OpGe:  return a >= b ? 1.0f : 0.0f;
        case OpEq:  return a == b ? 1.0f : 0.0f;
        case OpNe:  return a != b ? 1.0f : 0.0f;
        case OpAnd: return (a != 0.0f && b != 0.0f) ? 1.0f : 0.0f;
        case OpOr:  return (a != 0.0f || b != 0.0f) ? 1.0f : 0.0f;
        default:    return 0.0f;
    }
}

// Standard per-sample inputs.
struct Ramps
{
    std::array<float, kFrameSize> x, w, q;
    Ramps()
    {
        for (int i = 0; i < kFrameSize; ++i)
        {
            x[(size_t) i] = -1.0f + 2.0f * (float) i / (float) kFrameSize;
            w[(size_t) i] = (float) i / (float) kFrameSize;
            q[(size_t) i] = (float) (i + 1);
        }
    }
};
const Ramps& ramps() { static const Ramps r; return r; }
const std::array<float, kFrameSize>& zeros() { static const std::array<float, kFrameSize> z {}; return z; }

// ---------------------------------------------------------------------------
// Lexer + parser -> AST
// ---------------------------------------------------------------------------
struct Ast
{
    enum Kind { Num, Var, Unary, Binary, Ternary, Call } kind = Num;
    uint8_t op = 0, fn = 0;
    float value = 0.0f;
    int var = 0, pos = 0;
    std::vector<std::unique_ptr<Ast>> kids;
};
using AstPtr = std::unique_ptr<Ast>;

struct Tok
{
    enum Kind { End, Number, Ident, Sym } kind = End;
    std::string text;
    float value = 0.0f;
    int pos = 0;
};

struct ParseFail { int pos; std::string msg; };

class Parser
{
public:
    explicit Parser (const std::string& s) : src (s) { advance(); }

    std::optional<ParseFail> fail;

    AstPtr parseAll()
    {
        if (tok.kind == Tok::End) { error (0, "empty formula"); return nullptr; }
        auto e = parseTernary();
        if (! e) return nullptr;
        if (tok.kind != Tok::End) { error (tok.pos, "unexpected '" + tok.text + "'"); return nullptr; }
        return e;
    }

private:
    const std::string& src;
    size_t p = 0;
    Tok tok;

    void error (int pos, std::string m) { if (! fail) fail = ParseFail { pos, std::move (m) }; }

    void advance()
    {
        while (p < src.size() && std::isspace ((unsigned char) src[p])) ++p;
        tok = Tok {};
        tok.pos = (int) p;
        if (p >= src.size()) { tok.kind = Tok::End; tok.text = "end of formula"; return; }
        const char c = src[p];
        if (std::isdigit ((unsigned char) c) || (c == '.' && p + 1 < src.size() && std::isdigit ((unsigned char) src[p + 1])))
        {
            size_t e = p;
            while (e < src.size() && (std::isdigit ((unsigned char) src[e]) || src[e] == '.')) ++e;
            // Exponent only when followed by digits ("2e" alone is 2 then the constant e).
            if (e < src.size() && (src[e] == 'e' || src[e] == 'E'))
            {
                size_t k = e + 1;
                if (k < src.size() && (src[k] == '+' || src[k] == '-')) ++k;
                if (k < src.size() && std::isdigit ((unsigned char) src[k]))
                {
                    while (k < src.size() && std::isdigit ((unsigned char) src[k])) ++k;
                    e = k;
                }
            }
            tok.kind = Tok::Number;
            tok.text = src.substr (p, e - p);
            if (std::count (tok.text.begin(), tok.text.end(), '.') > 1)
            {
                error (tok.pos, "malformed number '" + tok.text + "'");
            }
            tok.value = std::strtof (tok.text.c_str(), nullptr);
            p = e;
            return;
        }
        if (std::isalpha ((unsigned char) c) || c == '_')
        {
            size_t e = p;
            while (e < src.size() && (std::isalnum ((unsigned char) src[e]) || src[e] == '_')) ++e;
            tok.kind = Tok::Ident;
            tok.text = src.substr (p, e - p);
            for (auto& ch : tok.text) ch = (char) std::tolower ((unsigned char) ch);
            p = e;
            return;
        }
        static const char* two[] { "<=", ">=", "!=", "==", "&&", "||" };
        for (auto* t : two)
        {
            if (src.compare (p, 2, t) == 0)
            {
                tok.kind = Tok::Sym;
                tok.text = t;
                p += 2;
                return;
            }
        }
        if (std::string ("+-*/%^()<>,?:!").find (c) != std::string::npos)
        {
            tok.kind = Tok::Sym;
            tok.text = std::string (1, c);
            ++p;
            return;
        }
        error (tok.pos, std::string ("unexpected character '") + c + "'");
        tok.kind = Tok::End;
        tok.text = std::string (1, c);
    }

    bool isSym (const char* s) const { return tok.kind == Tok::Sym && tok.text == s; }

    static int binaryPrec (const std::string& s, uint8_t& op)
    {
        struct B { const char* s; uint8_t op; int prec; };
        static const B ops[] {
            { "&&", OpAnd, 1 }, { "||", OpOr, 2 },
            { "<=", OpLe, 4 }, { ">=", OpGe, 4 }, { "!=", OpNe, 4 }, { "==", OpEq, 4 }, { "<", OpLt, 4 }, { ">", OpGt, 4 },
            { "+", OpAdd, 5 }, { "-", OpSub, 5 },
            { "*", OpMul, 6 }, { "/", OpDiv, 6 }, { "%", OpMod, 6 },
            { "^", OpPow, 7 },
        };
        for (auto& b : ops) if (s == b.s) { op = b.op; return b.prec; }
        return -1;
    }

    AstPtr parseTernary()
    {
        auto c = parseBinary (1);
        if (! c) return nullptr;
        if (! isSym ("?")) return c;
        const int qpos = tok.pos;
        advance();
        auto a = parseTernary();
        if (! a) return nullptr;
        if (! isSym (":")) { error (tok.pos, "expected ':' for the '?' at position " + std::to_string (qpos)); return nullptr; }
        advance();
        auto b = parseTernary();
        if (! b) return nullptr;
        auto n = std::make_unique<Ast>();
        n->kind = Ast::Ternary;
        n->pos = qpos;
        n->kids.push_back (std::move (c));
        n->kids.push_back (std::move (a));
        n->kids.push_back (std::move (b));
        return n;
    }

    AstPtr parseBinary (int minPrec)
    {
        auto lhs = parseUnary();
        if (! lhs) return nullptr;
        for (;;)
        {
            if (tok.kind != Tok::Sym) break;
            uint8_t op = 0;
            const int prec = binaryPrec (tok.text, op);
            if (prec < 0 || prec < minPrec) break;
            const int opos = tok.pos;
            advance();
            auto rhs = parseBinary (op == OpPow ? prec : prec + 1);
            if (! rhs) return nullptr;
            auto n = std::make_unique<Ast>();
            n->kind = Ast::Binary;
            n->op = op;
            n->pos = opos;
            n->kids.push_back (std::move (lhs));
            n->kids.push_back (std::move (rhs));
            lhs = std::move (n);
        }
        return lhs;
    }

    AstPtr parseUnary()
    {
        if (isSym ("-") || isSym ("+") || isSym ("!"))
        {
            const char c = tok.text[0];
            const int upos = tok.pos;
            advance();
            auto operand = parseBinary (7);    // binds looser than ^
            if (! operand) return nullptr;
            if (c == '+') return operand;
            auto n = std::make_unique<Ast>();
            n->kind = Ast::Unary;
            n->op = c == '-' ? OpNeg : OpNot;
            n->pos = upos;
            n->kids.push_back (std::move (operand));
            return n;
        }
        return parsePrimary();
    }

    AstPtr parsePrimary()
    {
        if (fail) return nullptr;
        if (tok.kind == Tok::Number)
        {
            auto n = std::make_unique<Ast>();
            n->kind = Ast::Num;
            n->value = tok.value;
            n->pos = tok.pos;
            advance();
            return n;
        }
        if (tok.kind == Tok::Ident)
        {
            const std::string name = tok.text;
            const int ipos = tok.pos;
            advance();
            for (auto& f : funcs)
            {
                if (name != f.name) continue;
                if (! isSym ("(")) { error (tok.pos, "expected '(' after '" + name + "'"); return nullptr; }
                advance();
                auto n = std::make_unique<Ast>();
                n->kind = Ast::Call;
                n->op = f.op;
                n->fn = f.fn;
                n->pos = ipos;
                if (! isSym (")"))
                {
                    for (;;)
                    {
                        auto a = parseTernary();
                        if (! a) return nullptr;
                        n->kids.push_back (std::move (a));
                        if (isSym (",")) { advance(); continue; }
                        break;
                    }
                }
                if (! isSym (")")) { error (tok.pos, "expected ')' to close '" + name + "('"); return nullptr; }
                advance();
                const int argc = (int) n->kids.size();
                if (f.argc >= 0 && argc != f.argc)
                {
                    error (ipos, "'" + name + "' takes " + std::to_string (f.argc) + " argument" + (f.argc == 1 ? "" : "s"));
                    return nullptr;
                }
                if (f.argc < 0 && argc < 1) { error (ipos, "'" + name + "' needs at least one argument"); return nullptr; }
                return n;
            }
            for (auto& v : vars)
            {
                if (name != v.name) continue;
                auto n = std::make_unique<Ast>();
                n->pos = ipos;
                if (v.var < 0) { n->kind = Ast::Num; n->value = v.value; }
                else { n->kind = Ast::Var; n->var = v.var; }
                return n;
            }
            error (ipos, "unknown name '" + name + "'");
            return nullptr;
        }
        if (isSym ("("))
        {
            const int open = tok.pos;
            advance();
            auto e = parseTernary();
            if (! e) return nullptr;
            if (! isSym (")")) { error (tok.pos, "expected ')' to close '(' at position " + std::to_string (open)); return nullptr; }
            advance();
            return e;
        }
        if (tok.kind == Tok::End) error (tok.pos, "unexpected end of formula");
        else error (tok.pos, "unexpected '" + tok.text + "'");
        return nullptr;
    }
};
} // namespace

// ---------------------------------------------------------------------------
// Code generation (with constant folding)
// ---------------------------------------------------------------------------
struct FormulaCompiler
{
    Formula& f;
    int cur = 0;

    void push (uint8_t op, uint8_t arg, uint16_t argc, float value, int delta)
    {
        f.code.push_back ({ op, arg, argc, value });
        cur += delta;
        f.depth = std::max (f.depth, cur);
    }

    static float evalConst (const Ast& n)
    {
        switch (n.kind)
        {
            case Ast::Num:     return n.value;
            case Ast::Var:     return 0.0f;
            case Ast::Unary:   { const float v = evalConst (*n.kids[0]); return n.op == OpNeg ? -v : (v == 0.0f ? 1.0f : 0.0f); }
            case Ast::Binary:  return apply2 (n.op, evalConst (*n.kids[0]), evalConst (*n.kids[1]));
            case Ast::Ternary: return evalConst (*n.kids[0]) != 0.0f ? evalConst (*n.kids[1]) : evalConst (*n.kids[2]);
            case Ast::Call:
            {
                if (n.op == OpFunc1) return apply1 (n.fn, evalConst (*n.kids[0]));
                float acc = evalConst (*n.kids[0]);
                for (size_t i = 1; i < n.kids.size(); ++i)
                {
                    const float v = evalConst (*n.kids[i]);
                    if (n.op == OpMin) acc = std::min (acc, v);
                    else if (n.op == OpMax) acc = std::max (acc, v);
                    else acc += v;
                }
                return n.op == OpAvg ? acc / (float) n.kids.size() : acc;
            }
        }
        return 0.0f;
    }

    // Returns true if the subtree is constant (folds it to a Num node).
    bool fold (Ast& n)
    {
        if (n.kind == Ast::Num) return true;
        if (n.kind == Ast::Var) { f.usedVars |= 1u << n.var; return false; }
        bool allConst = true;
        for (auto& k : n.kids) allConst &= fold (*k);
        if (! allConst) return false;
        n.value = evalConst (n);
        n.kind = Ast::Num;
        n.kids.clear();
        return true;
    }

    void emit (const Ast& n)
    {
        switch (n.kind)
        {
            case Ast::Num: push (OpConst, 0, 0, n.value, 1); break;
            case Ast::Var: push (OpVar, (uint8_t) n.var, 0, 0.0f, 1); break;
            case Ast::Unary: emit (*n.kids[0]); push (n.op, 0, 0, 0.0f, 0); break;
            case Ast::Binary: emit (*n.kids[0]); emit (*n.kids[1]); push (n.op, 0, 0, 0.0f, -1); break;
            case Ast::Ternary:
                for (auto& k : n.kids) emit (*k);
                push (OpSelect, 0, 0, 0.0f, -2);
                break;
            case Ast::Call:
                for (auto& k : n.kids) emit (*k);
                if (n.op == OpFunc1) push (OpFunc1, n.fn, 1, 0.0f, 0);
                else push (n.op, 0, (uint16_t) n.kids.size(), 0.0f, 1 - (int) n.kids.size());
                break;
        }
    }
};

bool Formula::compile (const std::string& text, FormulaError& error)
{
    code.clear();
    stack.clear();
    depth = 0;
    usedVars = 0;
    isConstant = false;
    source = text;
    error = {};

    Parser parser (text);
    auto ast = parser.parseAll();
    if (parser.fail || ! ast)
    {
        error.position = parser.fail ? parser.fail->pos : 0;
        error.message = parser.fail ? parser.fail->msg : "syntax error";
        code.clear();
        return false;
    }
    FormulaCompiler c { *this };
    isConstant = c.fold (*ast);
    c.emit (*ast);
    stack.assign ((size_t) std::max (1, depth) * kFrameSize, 0.0f);
    return true;
}

void Formula::evaluate (const Inputs& inp, float* out) const noexcept
{
    const int n = std::clamp (inp.n, 1, kFrameSize);
    if (code.empty()) { std::fill (out, out + n, 0.0f); return; }
    const auto& R = ramps();
    const float* src[NumVars] {
        inp.x ? inp.x : R.x.data(), inp.w ? inp.w : R.w.data(), nullptr, nullptr,
        inp.q ? inp.q : R.q.data(), inp.in ? inp.in : zeros().data(),
        inp.sel ? inp.sel : zeros().data(), inp.rand ? inp.rand : zeros().data()
    };
    const float yv = inp.y, zv = 2.0f * inp.y - 1.0f;
    float* const base = stack.data();
    int sp = 0;     // number of live slots
    auto slot = [base] (int i) { return base + (size_t) i * kFrameSize; };

    for (const Ins& ins : code)
    {
        switch (ins.op)
        {
            case OpConst: { float* d = slot (sp++); const float v = ins.value; for (int i = 0; i < n; ++i) d[i] = v; break; }
            case OpVar:
            {
                float* d = slot (sp++);
                if (ins.arg == Y || ins.arg == Z)
                {
                    const float v = ins.arg == Y ? yv : zv;
                    for (int i = 0; i < n; ++i) d[i] = v;
                }
                else
                {
                    std::memcpy (d, src[ins.arg], sizeof (float) * (size_t) n);
                }
                break;
            }
            case OpNeg: { float* a = slot (sp - 1); for (int i = 0; i < n; ++i) a[i] = -a[i]; break; }
            case OpNot: { float* a = slot (sp - 1); for (int i = 0; i < n; ++i) a[i] = a[i] == 0.0f ? 1.0f : 0.0f; break; }

#define TINCTURE_BINOP(OPC, EXPR) \
            case OPC: { float* a = slot (sp - 2); const float* b = slot (sp - 1); \
                        for (int i = 0; i < n; ++i) { const float l = a[i], r = b[i]; a[i] = (EXPR); } --sp; break; }
            TINCTURE_BINOP (OpAdd, l + r)
            TINCTURE_BINOP (OpSub, l - r)
            TINCTURE_BINOP (OpMul, l * r)
            TINCTURE_BINOP (OpDiv, l / r)
            TINCTURE_BINOP (OpMod, std::fmod (l, r))
            TINCTURE_BINOP (OpPow, std::pow (l, r))
            TINCTURE_BINOP (OpLt, l < r ? 1.0f : 0.0f)
            TINCTURE_BINOP (OpGt, l > r ? 1.0f : 0.0f)
            TINCTURE_BINOP (OpLe, l <= r ? 1.0f : 0.0f)
            TINCTURE_BINOP (OpGe, l >= r ? 1.0f : 0.0f)
            TINCTURE_BINOP (OpEq, l == r ? 1.0f : 0.0f)
            TINCTURE_BINOP (OpNe, l != r ? 1.0f : 0.0f)
            TINCTURE_BINOP (OpAnd, (l != 0.0f && r != 0.0f) ? 1.0f : 0.0f)
            TINCTURE_BINOP (OpOr, (l != 0.0f || r != 0.0f) ? 1.0f : 0.0f)
#undef TINCTURE_BINOP

            case OpSelect:
            {
                float* c = slot (sp - 3);
                const float* a = slot (sp - 2);
                const float* b = slot (sp - 1);
                for (int i = 0; i < n; ++i) c[i] = c[i] != 0.0f ? a[i] : b[i];
                sp -= 2;
                break;
            }
            case OpFunc1:
            {
                float* a = slot (sp - 1);
                switch (ins.arg)     // one loop per function so each one is a tight loop
                {
#define TINCTURE_F1(ID) case ID: for (int i = 0; i < n; ++i) a[i] = apply1 (ID, a[i]); break;
                    TINCTURE_F1 (FSin) TINCTURE_F1 (FCos) TINCTURE_F1 (FTan) TINCTURE_F1 (FAsin) TINCTURE_F1 (FAcos)
                    TINCTURE_F1 (FAtan) TINCTURE_F1 (FSinh) TINCTURE_F1 (FCosh) TINCTURE_F1 (FTanh) TINCTURE_F1 (FAsinh)
                    TINCTURE_F1 (FAcosh) TINCTURE_F1 (FAtanh) TINCTURE_F1 (FLog2) TINCTURE_F1 (FLog10) TINCTURE_F1 (FLn)
                    TINCTURE_F1 (FExp) TINCTURE_F1 (FSqrt) TINCTURE_F1 (FSign) TINCTURE_F1 (FRint) TINCTURE_F1 (FAbs)
#undef TINCTURE_F1
                    default: break;
                }
                break;
            }
            case OpMin: case OpMax: case OpSum: case OpAvg:
            {
                const int k = ins.argc;
                float* a = slot (sp - k);
                for (int j = 1; j < k; ++j)
                {
                    const float* b = slot (sp - k + j);
                    if (ins.op == OpMin)      for (int i = 0; i < n; ++i) a[i] = std::min (a[i], b[i]);
                    else if (ins.op == OpMax) for (int i = 0; i < n; ++i) a[i] = std::max (a[i], b[i]);
                    else                      for (int i = 0; i < n; ++i) a[i] += b[i];
                }
                if (ins.op == OpAvg) { const float s = 1.0f / (float) k; for (int i = 0; i < n; ++i) a[i] *= s; }
                sp -= k - 1;
                break;
            }
            default: break;
        }
    }
    const float* r = slot (0);
    for (int i = 0; i < n; ++i) out[i] = std::isfinite (r[i]) ? r[i] : 0.0f;
}

float Formula::evaluateScalar (float x, float y) const noexcept
{
    Inputs in;
    in.n = 1;
    const float w = (x + 1.0f) * 0.5f, q = 1.0f;
    in.x = &x; in.w = &w; in.q = &q; in.y = y;
    float out = 0.0f;
    evaluate (in, &out);
    return out;
}

// ---------------------------------------------------------------------------
// Applying to a table
// ---------------------------------------------------------------------------
namespace
{
inline float magToFormula (float m) noexcept { return m < 0.0f ? -std::sqrt (-m) : std::sqrt (m); }

void binsOf (const Spectrum& s, float* bars)
{
    for (int q = 1; q <= kFftBins; ++q) bars[q - 1] = magToFormula (s.mag[(size_t) q]);
}
} // namespace

FormulaResult applyFormula (const Formula& f, FrameSet& set, Selection& sel, uint32_t seed)
{
    FormulaResult res;
    if (! f.valid()) { res.error.position = 0; res.error.message = "empty formula"; return res; }
    if (set.frames.empty()) set.frames.push_back (Frame {});
    sel.clamp (set.size());

    const bool spectral = f.spectral();
    const bool multi = f.multiFrame();
    const int n = spectral ? kFftBins : kFrameSize;
    const int count = multi ? kMaxFrames : 1;

    std::vector<float> rnd ((size_t) kFrameSize);
    Rng rng (seed);
    for (auto& v : rnd) v = rng.bi();

    const std::vector<Frame> old = set.frames;
    const Frame& selFrame = old[(size_t) sel.current];
    const int oldN = (int) old.size();

    std::array<float, kFrameSize> inBuf {}, selBuf {}, outBuf {};
    Spectrum selSpec, inSpec;
    if (spectral && f.uses (Formula::Sel))
    {
        analyse (selFrame, selSpec);
        binsOf (selSpec, selBuf.data());
    }

    std::vector<Frame> out ((size_t) count);
    for (int fi = 0; fi < count; ++fi)
    {
        const float y = multi ? (float) fi / (float) (kMaxFrames - 1) : 0.0f;
        const int src = multi ? (oldN > 1 ? (int) std::lround (y * (float) (oldN - 1)) : 0) : sel.current;
        const Frame& inFrame = old[(size_t) src];

        Formula::Inputs in;
        in.n = n;
        in.y = y;
        in.rand = rnd.data();
        if (spectral)
        {
            analyse (inFrame, inSpec);
            binsOf (inSpec, inBuf.data());
            in.in = inBuf.data();
            in.sel = selBuf.data();
            f.evaluate (in, outBuf.data());
            Spectrum s = inSpec;              // phases are kept
            s.mag[0] = s.mag[kNyquist] = 0.0f;
            for (int k = kFftBins + 1; k < kNyquist; ++k) s.mag[(size_t) k] = 0.0f;
            for (int q = 1; q <= kFftBins; ++q)
            {
                const float v = outBuf[(size_t) q - 1];
                float m = v * std::abs (v);
                if (m < 0.0f) { m = -m; s.phase[(size_t) q] += kPi; }
                s.mag[(size_t) q] = m;
            }
            synthesise (s, out[(size_t) fi]);
            if (peakOf (out[(size_t) fi]) > 1.0f) normalise (out[(size_t) fi], 1.0f);
        }
        else
        {
            in.in = inFrame.data();
            in.sel = selFrame.data();
            f.evaluate (in, out[(size_t) fi].data());
        }
    }

    if (multi)
    {
        set.frames = std::move (out);
        set.keyFrames.clear();
        sel.clamp (set.size());
    }
    else
    {
        set.frames[(size_t) sel.current] = out[0];
    }
    set.modified = true;
    res.ok = true;
    res.frames = count;
    res.spectral = spectral;
    return res;
}

FormulaResult applyFormula (const std::string& text, FrameSet& set, Selection& sel, uint32_t seed)
{
    Formula f;
    FormulaResult res;
    if (! f.compile (text, res.error)) return res;
    return applyFormula (f, set, sel, seed);
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------
const std::vector<FormulaPreset>& factorySingles()
{
    static const std::vector<FormulaPreset> p {
        { "Sine", "sin(x*pi)" },
        { "Saw Up", "x" },
        { "Saw Down", "-x" },
        { "Square", "x<0?-1:1" },
        { "Triangle", "1-2*abs(x)" },
        { "Pulse 25%", "w<0.25?1:-1" },
        { "Pulse 10%", "w<0.1?1:-1" },
        { "Rectified Sine", "2*abs(sin(x*pi))-1" },
        { "Half Sine", "max(sin(x*pi),0)*2-0.6366" },
        { "Stepped Triangle", "(rint(16*abs(x))/8.0)-1" },
        { "Sine Octaves", "(sin(x*pi)+sin(2*x*pi)/2+sin(4*x*pi)/4)/1.4" },
        { "Soft Square", "tanh(4*sin(x*pi))" },
        { "Exp Ramp", "(exp(3*w)-1)/(exp(3)-1)*2-1" },
        { "Noise", "rand" },
        { "Saw (Spectral)", "(1/q)^0.5" },
        { "Bright Saw (Spectral)", "(1/q)^0.25" },
        { "Dull Saw (Spectral)", "(1/q)^0.75" },
        { "First 16 Harmonics", "q<17" },
        { "Square (Spectral)", "(q%2)*(1/q)^0.5" },
        { "Proc:Squarify", "((q%2)==1)?in:0" },
        { "Proc:Remove Fundamental", "(q>1)?in:0" },
        { "Proc:Even Only", "((q%2)==0)?in:0" },
        { "Proc:Brighten", "in*q^0.15" },
        { "Proc:Darken", "in/q^0.15" },
        { "Proc:Half Gain", "in*0.5" },
        { "Proc:Invert", "-in" },
        { "Proc:Rectify", "abs(in)" },
        { "Proc:Soft Clip", "tanh(2*in)" },
        { "Proc:Bit Crush", "rint(in*8)/8" },
        { "Proc:Mix Selected", "(in+sel)/2" },
    };
    return p;
}

const std::vector<FormulaPreset>& factoryMultis()
{
    static const std::vector<FormulaPreset> p {
        { "Saw Brightness", "(1/q)^(1.5-y)" },
        { "Harmonic Ladder", "q<y*255+2" },
        { "Spectral Saw Sweep", "(1/q)^z" },
        { "PWM", "w<(0.5-0.48*y)?1:-1" },
        { "Sine Sync", "sin(x*pi*(1+7*y))" },
        { "Sine Fold", "sin((1+6*y)*sin(x*pi))" },
        { "Saw to Square", "(1-y)*x+y*(x<0?-1:1)" },
        { "Sine to Triangle", "(1-y)*sin(x*pi)+y*(1-2*abs(x))" },
        { "Tanh Drive", "tanh((1+15*y)*sin(x*pi))/tanh(1+15*y)" },
        { "Bit Reduce", "rint(sin(x*pi)*(33-31*y))/(33-31*y)" },
        { "Phase Squeeze", "sin(pi*(w^(1+3*y))*2)" },
        { "Odd to Even", "((q%2)==1?(1-y):y)*(1/q)^0.5" },
        { "Comb Sweep", "(1/q)^0.5*abs(cos(q*pi*y*0.5))" },
        { "Formant Sweep", "exp(-((q-(2+60*y))^2)/40)" },
        { "Proc:Fade Out Table", "in*(1-y)" },
    };
    return p;
}

const std::vector<const char*>& formulaMenuItems()
{
    static const std::vector<const char*> m { "Singles", "Multis", "User Singles", "User Multis", "-", "Save Formula..." };
    return m;
}

std::vector<FormulaPreset> parseFormulaFile (const std::string& text)
{
    std::vector<FormulaPreset> out;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find ('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr (start, end - start);
        start = end + 1;
        while (! line.empty() && std::isspace ((unsigned char) line.back())) line.pop_back();
        size_t a = line.find ('[');
        if (a == std::string::npos) continue;
        size_t b = line.find (']', a + 1);
        if (b == std::string::npos) continue;
        FormulaPreset p;
        p.formula = line.substr (a + 1, b - a - 1);
        size_t c = line.find ('[', b + 1);
        size_t d = c == std::string::npos ? std::string::npos : line.find (']', c + 1);
        p.name = (d != std::string::npos) ? line.substr (c + 1, d - c - 1) : p.formula;
        if (! p.formula.empty()) out.push_back (std::move (p));
        if (end == text.size()) break;
    }
    return out;
}

std::string writeFormulaFile (const std::vector<FormulaPreset>& presets)
{
    std::string s;
    for (auto& p : presets) s += "[" + p.formula + "][" + p.name + "]\n";
    return s;
}

bool isMultiFormula (const std::string& formula)
{
    Formula f;
    FormulaError e;
    return f.compile (formula, e) && f.multiFrame();
}

std::string addToFormulaFile (const std::string& fileText, const FormulaPreset& p)
{
    auto list = parseFormulaFile (fileText);
    bool replaced = false;
    for (auto& e : list) if (e.name == p.name) { e.formula = p.formula; replaced = true; }
    if (! replaced) list.push_back (p);
    return writeFormulaFile (list);
}
} // namespace tincture::wtedit
