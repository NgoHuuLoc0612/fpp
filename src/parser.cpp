#include "../include/parser.hpp"
#include <sstream>
#include <cassert>
#include <algorithm>

namespace fpp {

using namespace ast;

// Tokens that trigger panic-mode recovery
const std::set<TokenKind> Parser::syncSet_ = {
    TokenKind::Semicolon, TokenKind::RBrace, TokenKind::Kw_fn,
    TokenKind::Kw_struct, TokenKind::Kw_class, TokenKind::Kw_enum,
    TokenKind::Kw_impl, TokenKind::Kw_trait, TokenKind::Kw_let,
    TokenKind::Eof,
};

Parser::Parser(std::vector<Token> tokens, std::string filename)
    : toks_(std::move(tokens)), file_(std::move(filename)) {}

// ─── Token access ────────────────────────────────────────────────────────────
const Token& Parser::peek(size_t ahead) const {
    size_t idx = pos_ + ahead;
    return idx < toks_.size() ? toks_[idx] : toks_.back();
}
const Token& Parser::advance() {
    if (pos_ < toks_.size()) ++pos_;
    return toks_[pos_ - 1];
}
bool Parser::check(TokenKind k) const { return peek().kind == k; }
bool Parser::check2(TokenKind k) const { return peek(1).kind == k; }
bool Parser::match(TokenKind k) { if (check(k)) { advance(); return true; } return false; }
bool Parser::matchAny(std::initializer_list<TokenKind> ks) {
    for (auto k : ks) if (check(k)) { advance(); return true; }
    return false;
}
const Token& Parser::expect(TokenKind k, const std::string& msg) {
    if (!check(k)) {
        std::string m = msg.empty()
            ? "Expected token '" + std::to_string((int)k) + "', got '" + peek().lexeme + "'"
            : msg;
        error(m);
        static Token dummy{TokenKind::Unknown, "", {}, {}};
        return dummy;
    }
    return advance();
}
bool Parser::atEnd() const { return check(TokenKind::Eof); }
SourceLocation Parser::loc() const { return peek().loc; }

void Parser::error(const std::string& msg) {
    errors_.emplace_back(msg, loc());
}
void Parser::error(const std::string& msg, SourceLocation l) {
    errors_.emplace_back(msg, l);
}

void Parser::synchronize() {
    while (!atEnd()) {
        if (syncSet_.count(peek().kind)) { if (check(TokenKind::Semicolon)) advance(); return; }
        advance();
    }
}

// ─── Operator tables ─────────────────────────────────────────────────────────
int Parser::infixPrec(TokenKind k) noexcept {
    switch (k) {
    case TokenKind::Eq: case TokenKind::PlusEq: case TokenKind::MinusEq:
    case TokenKind::StarEq: case TokenKind::SlashEq: case TokenKind::PercentEq:
    case TokenKind::AmpEq: case TokenKind::PipeEq: case TokenKind::CaretEq:
    case TokenKind::LtLtEq: case TokenKind::GtGtEq: case TokenKind::StarStarEq:
        return 1;
    case TokenKind::QuestionQuestion: return 3;
    case TokenKind::PipePipe: return 4;
    case TokenKind::AmpAmp: return 5;
    case TokenKind::Pipe: return 6;
    case TokenKind::Caret: return 7;
    case TokenKind::Ampersand: return 8;
    case TokenKind::EqEq: case TokenKind::BangEq: return 9;
    case TokenKind::Lt: case TokenKind::Gt: case TokenKind::LtEq: case TokenKind::GtEq:
        return 10;
    case TokenKind::LtLt: case TokenKind::GtGt: case TokenKind::GtGtGt: return 11;
    case TokenKind::DotDot: case TokenKind::DotDotEq: return 12;
    case TokenKind::Plus: case TokenKind::Minus: return 13;
    case TokenKind::Star: case TokenKind::Slash: case TokenKind::Percent: return 14;
    case TokenKind::StarStar: return 15;
    case TokenKind::Kw_as: return 16;
    case TokenKind::Dot: case TokenKind::QuestionDot:
    case TokenKind::ColonColon: return 20;
    default: return -1;
    }
}

bool Parser::isRightAssoc(TokenKind k) noexcept {
    return k == TokenKind::Eq || k == TokenKind::PlusEq || k == TokenKind::MinusEq ||
           k == TokenKind::StarEq || k == TokenKind::SlashEq || k == TokenKind::StarStar ||
           k == TokenKind::StarStarEq;
}

bool Parser::isAssignOp(TokenKind k) noexcept {
    return k == TokenKind::Eq || k == TokenKind::PlusEq || k == TokenKind::MinusEq ||
           k == TokenKind::StarEq || k == TokenKind::SlashEq || k == TokenKind::PercentEq ||
           k == TokenKind::AmpEq || k == TokenKind::PipeEq || k == TokenKind::CaretEq ||
           k == TokenKind::LtLtEq || k == TokenKind::GtGtEq || k == TokenKind::StarStarEq;
}

bool Parser::isBinaryOp(TokenKind k) noexcept { return infixPrec(k) > 0; }

bool Parser::isPrefixOp(TokenKind k) noexcept {
    return k == TokenKind::Minus || k == TokenKind::Bang || k == TokenKind::Tilde ||
           k == TokenKind::Ampersand || k == TokenKind::Star;
}

// ─── Top-level parsing ────────────────────────────────────────────────────────
Module Parser::parse() {
    Module mod;
    mod.name = file_;
    mod.file = file_;
    // Skip leading newlines
    while (match(TokenKind::Newline)) {}
    while (!atEnd()) {
        try {
            if (check(TokenKind::Newline)) { advance(); continue; }
            auto item = parseItem();
            if (item) mod.items.push_back(std::move(item));
        } catch (const ParseError& e) {
            errors_.push_back(e);
            synchronize();
        }
    }
    return mod;
}

StmtPtr Parser::parseItem() {
    auto attrs = parseAttributes();
    bool isPublic = match(TokenKind::Kw_pub);

    // Modifiers
    bool isAsync   = match(TokenKind::Kw_async);
    bool isExtern  = match(TokenKind::Kw_extern);
    bool isUnsafe  = match(TokenKind::Kw_unsafe);
    bool isStatic  = match(TokenKind::Kw_static);
    bool isInline  = match(TokenKind::Kw_inline);

    if (check(TokenKind::Kw_fn))
        return parseFn(isPublic, isAsync, isExtern, isUnsafe, isStatic, isInline);
    if (check(TokenKind::Kw_struct))    return parseStruct(isPublic);
    if (check(TokenKind::Kw_enum))      return parseEnum(isPublic);
    if (check(TokenKind::Kw_trait))     return parseTrait(isPublic);
    if (check(TokenKind::Kw_impl))      return parseImpl();
    if (check(TokenKind::Kw_class))     return parseClass(isPublic);
    if (check(TokenKind::Kw_interface)) return parseInterface(isPublic);
    if (check(TokenKind::Kw_type))      return parseTypeAlias(isPublic);
    if (check(TokenKind::Kw_module))    return parseModule(isPublic);
    if (check(TokenKind::Kw_import))    return parseImport();
    if (check(TokenKind::Kw_const))     return parseConst(isPublic);
    if (check(TokenKind::Kw_macro))     return parseMacro();
    // If we already consumed static, we're missing the fn/let
    if (isStatic)                       return parseStmt();
    return parseStmt();
}

std::vector<Attribute> Parser::parseAttributes() {
    std::vector<Attribute> attrs;
    while (check(TokenKind::Hash)) {
        advance(); // #
        expect(TokenKind::LBracket, "Expected '[' after '#'");
        Attribute a;
        a.name = expect(TokenKind::Identifier, "Expected attribute name").lexeme;
        if (match(TokenKind::LParen)) {
            while (!check(TokenKind::RParen) && !atEnd()) {
                a.args.push_back(advance().lexeme);
                if (!match(TokenKind::Comma)) break;
            }
            expect(TokenKind::RParen, "Expected ')'");
        }
        expect(TokenKind::RBracket, "Expected ']'");
        attrs.push_back(std::move(a));
    }
    return attrs;
}

std::vector<GenericParam> Parser::parseGenericParams() {
    std::vector<GenericParam> params;
    if (!match(TokenKind::Lt)) return params;
    while (!check(TokenKind::Gt) && !atEnd()) {
        GenericParam p;
        p.name = expect(TokenKind::Identifier, "Expected type parameter name").lexeme;
        if (match(TokenKind::Colon)) {
            p.bounds.push_back(parseBound());
            while (match(TokenKind::Plus))
                p.bounds.push_back(parseBound());
        }
        if (match(TokenKind::Eq))
            p.defaultTy = parseType();
        params.push_back(std::move(p));
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::Gt, "Expected '>'");
    return params;
}

TypePtr Parser::parseBound() {
    return parseType();
}

ParamList Parser::parseParamList(std::optional<std::string>& selfParam) {
    ParamList params;
    expect(TokenKind::LParen, "Expected '('");
    // self parameter
    if (check(TokenKind::Ampersand)) {
        advance();
        bool mut = match(TokenKind::Kw_var);
        if (check(TokenKind::Identifier) && peek().lexeme == "self") {
            advance();
            selfParam = mut ? "&mut self" : "&self";
            if (!match(TokenKind::Comma)) { expect(TokenKind::RParen, "Expected ')'"); return params; }
        } else {
            // Wasn't a self param — backtrack and parse as &T
            --pos_;
        }
    } else if (check(TokenKind::Identifier) && peek().lexeme == "self") {
        advance();
        selfParam = "self";
        if (!match(TokenKind::Comma)) { expect(TokenKind::RParen, "Expected ')'"); return params; }
    }

    while (!check(TokenKind::RParen) && !atEnd()) {
        if (check(TokenKind::Ellipsis)) { advance(); break; } // variadic
        std::string pname = expect(TokenKind::Identifier, "Expected parameter name").lexeme;
        expect(TokenKind::Colon, "Expected ':' after parameter name");
        auto ty = parseType();
        params.emplace_back(pname, std::move(ty));
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RParen, "Expected ')'");
    return params;
}

StmtPtr Parser::parseFn(bool isPublic, bool isAsync, bool isExtern, bool isUnsafe,
                         bool isStatic, bool isInline) {
    advance(); // consume 'fn'
    auto fn = std::make_unique<FnItem>();
    fn->loc      = loc();
    fn->isPublic = isPublic;
    fn->isAsync  = isAsync;
    fn->isExtern = isExtern;
    fn->isUnsafe = isUnsafe;
    fn->isStatic = isStatic;
    fn->isInline = isInline;
    fn->name     = expect(TokenKind::Identifier, "Expected function name").lexeme;
    fn->generics = parseGenericParams();

    std::optional<std::string> selfParam;
    fn->params   = parseParamList(selfParam);
    fn->selfParam = selfParam;

    // Return type
    if (match(TokenKind::Arrow)) {
        fn->retTy = parseType();
    } else {
        auto unit = std::make_unique<NamedType>();
        unit->name = "unit";
        fn->retTy = std::move(unit);
    }

    // Body or ';'
    if (check(TokenKind::LBrace)) {
        StmtList body;
        advance(); // {
        while (!check(TokenKind::RBrace) && !atEnd()) {
            if (match(TokenKind::Newline)) continue;
            body.push_back(parseStmt());
        }
        expect(TokenKind::RBrace, "Expected '}'");
        fn->body = std::move(body);
    } else {
        match(TokenKind::Semicolon);
    }
    return fn;
}

StmtPtr Parser::parseStruct(bool isPublic) {
    advance(); // struct
    auto s = std::make_unique<StructItem>();
    s->loc      = loc();
    s->isPublic = isPublic;
    s->name     = expect(TokenKind::Identifier, "Expected struct name").lexeme;
    s->generics = parseGenericParams();
    expect(TokenKind::LBrace, "Expected '{'");
    while (!check(TokenKind::RBrace) && !atEnd()) {
        if (match(TokenKind::Newline)) continue;
        s->fields.push_back(parseFieldDef());
        matchAny({TokenKind::Comma, TokenKind::Semicolon, TokenKind::Newline});
    }
    expect(TokenKind::RBrace, "Expected '}'");
    return s;
}

FieldDef Parser::parseFieldDef() {
    FieldDef f;
    f.isPublic = match(TokenKind::Kw_pub);
    f.isMut    = match(TokenKind::Kw_var);
    f.name     = expect(TokenKind::Identifier, "Expected field name").lexeme;
    expect(TokenKind::Colon, "Expected ':'");
    f.ty = parseType();
    return f;
}

StmtPtr Parser::parseEnum(bool isPublic) {
    advance(); // enum
    auto e = std::make_unique<EnumItem>();
    e->loc      = loc();
    e->isPublic = isPublic;
    e->name     = expect(TokenKind::Identifier, "Expected enum name").lexeme;
    e->generics = parseGenericParams();
    expect(TokenKind::LBrace, "Expected '{'");
    while (!check(TokenKind::RBrace) && !atEnd()) {
        if (match(TokenKind::Newline)) continue;
        e->variants.push_back(parseVariantDef());
        matchAny({TokenKind::Comma, TokenKind::Newline});
    }
    expect(TokenKind::RBrace, "Expected '}'");
    return e;
}

VariantDef Parser::parseVariantDef() {
    VariantDef v;
    v.name = expect(TokenKind::Identifier, "Expected variant name").lexeme;
    if (check(TokenKind::LParen)) { // tuple variant
        advance();
        std::vector<TypePtr> types;
        while (!check(TokenKind::RParen) && !atEnd()) {
            types.push_back(parseType());
            if (!match(TokenKind::Comma)) break;
        }
        expect(TokenKind::RParen, "Expected ')'");
        v.tuple = std::move(types);
    } else if (check(TokenKind::LBrace)) { // struct variant
        advance();
        std::vector<FieldDef> fields;
        while (!check(TokenKind::RBrace) && !atEnd()) {
            if (match(TokenKind::Newline)) continue;
            fields.push_back(parseFieldDef());
            matchAny({TokenKind::Comma, TokenKind::Newline});
        }
        expect(TokenKind::RBrace, "Expected '}'");
        v.fields = std::move(fields);
    } else if (match(TokenKind::Eq)) { // discriminant
        auto e = parseExpr();
        if (auto* lit = dynamic_cast<LiteralExpr*>(e.get()))
            if (auto* iv = std::get_if<int64_t>(&lit->tok.literal))
                v.discrim = *iv;
    }
    return v;
}

StmtPtr Parser::parseTrait(bool isPublic) {
    advance();
    auto t = std::make_unique<TraitItem>();
    t->isPublic = isPublic;
    t->name     = expect(TokenKind::Identifier, "Expected trait name").lexeme;
    t->generics = parseGenericParams();
    if (match(TokenKind::Colon)) {
        t->superTraits.push_back(parseBound());
        while (match(TokenKind::Plus)) t->superTraits.push_back(parseBound());
    }
    expect(TokenKind::LBrace, "Expected '{'");
    while (!check(TokenKind::RBrace) && !atEnd()) {
        if (match(TokenKind::Newline)) continue;
        t->members.push_back(parseItem());
    }
    expect(TokenKind::RBrace, "Expected '}'");
    return t;
}

StmtPtr Parser::parseImpl() {
    advance(); // impl
    auto i = std::make_unique<ImplItem>();
    i->generics = parseGenericParams();
    // impl Trait for Type  OR  impl Type
    auto ty1 = parseType();
    if (match(TokenKind::Kw_for)) {
        i->trait = std::move(ty1);
        i->ty    = parseType();
    } else {
        i->ty = std::move(ty1);
    }
    expect(TokenKind::LBrace, "Expected '{'");
    while (!check(TokenKind::RBrace) && !atEnd()) {
        if (match(TokenKind::Newline)) continue;
        i->members.push_back(parseItem());
    }
    expect(TokenKind::RBrace, "Expected '}'");
    return i;
}

StmtPtr Parser::parseClass(bool isPublic) {
    advance(); // class
    auto c = std::make_unique<ClassItem>();
    c->isPublic  = isPublic;
    c->isFinal   = false;
    c->name      = expect(TokenKind::Identifier, "Expected class name").lexeme;
    c->generics  = parseGenericParams();
    if (match(TokenKind::Colon)) { // inheritance / interface list
        c->bases.push_back(parseType());
        while (match(TokenKind::Comma)) c->bases.push_back(parseType());
    }
    expect(TokenKind::LBrace, "Expected '{'");
    while (!check(TokenKind::RBrace) && !atEnd()) {
        if (match(TokenKind::Newline)) continue;
        c->members.push_back(parseItem());
    }
    expect(TokenKind::RBrace, "Expected '}'");
    return c;
}

StmtPtr Parser::parseInterface(bool isPublic) {
    advance();
    auto i = std::make_unique<InterfaceItem>();
    i->isPublic = isPublic;
    i->name     = expect(TokenKind::Identifier, "Expected interface name").lexeme;
    i->generics = parseGenericParams();
    if (match(TokenKind::Colon)) {
        i->extends.push_back(parseType());
        while (match(TokenKind::Comma)) i->extends.push_back(parseType());
    }
    expect(TokenKind::LBrace, "Expected '{'");
    while (!check(TokenKind::RBrace) && !atEnd()) {
        if (match(TokenKind::Newline)) continue;
        i->members.push_back(parseItem());
    }
    expect(TokenKind::RBrace, "Expected '}'");
    return i;
}

StmtPtr Parser::parseTypeAlias(bool isPublic) {
    advance(); // type
    auto a = std::make_unique<TypeAliasItem>();
    a->isPublic = isPublic;
    a->name     = expect(TokenKind::Identifier, "Expected type name").lexeme;
    a->generics = parseGenericParams();
    expect(TokenKind::Eq, "Expected '='");
    a->ty       = parseType();
    matchAny({TokenKind::Semicolon, TokenKind::Newline});
    return a;
}

StmtPtr Parser::parseModule(bool isPublic) {
    advance();
    auto m = std::make_unique<ModuleItem>();
    m->isPublic = isPublic;
    m->name     = expect(TokenKind::Identifier, "Expected module name").lexeme;
    if (check(TokenKind::LBrace)) {
        advance();
        StmtList body;
        while (!check(TokenKind::RBrace) && !atEnd()) {
            if (match(TokenKind::Newline)) continue;
            body.push_back(parseItem());
        }
        expect(TokenKind::RBrace, "Expected '}'");
        m->body = std::move(body);
    } else {
        matchAny({TokenKind::Semicolon, TokenKind::Newline});
    }
    return m;
}

StmtPtr Parser::parseImport() {
    advance(); // import
    auto imp = std::make_unique<ImportItem>();
    // Parse path::to::module
    imp->path.push_back(expect(TokenKind::Identifier, "Expected module path").lexeme);
    while (match(TokenKind::ColonColon)) {
        if (check(TokenKind::Star)) { advance(); imp->spec = std::monostate{}; break; }
        if (check(TokenKind::LBrace)) {
            advance();
            std::vector<std::pair<std::string,std::optional<std::string>>> imports;
            while (!check(TokenKind::RBrace) && !atEnd()) {
                std::string name = expect(TokenKind::Identifier, "Expected name").lexeme;
                std::optional<std::string> alias;
                if (match(TokenKind::Kw_as))
                    alias = expect(TokenKind::Identifier, "Expected alias").lexeme;
                imports.emplace_back(name, alias);
                if (!match(TokenKind::Comma)) break;
            }
            expect(TokenKind::RBrace, "Expected '}'");
            imp->spec = std::move(imports);
            break;
        }
        imp->path.push_back(advance().lexeme);
    }
    if (match(TokenKind::Kw_as))
        imp->spec = expect(TokenKind::Identifier, "Expected alias").lexeme;
    matchAny({TokenKind::Semicolon, TokenKind::Newline});
    return imp;
}

StmtPtr Parser::parseConst(bool isPublic) {
    advance();
    auto c = std::make_unique<ConstItem>();
    c->isPublic = isPublic;
    c->name = expect(TokenKind::Identifier, "Expected const name").lexeme;
    expect(TokenKind::Colon, "Expected ':'");
    c->ty = parseType();
    expect(TokenKind::Eq, "Expected '='");
    c->val = parseExpr();
    matchAny({TokenKind::Semicolon, TokenKind::Newline});
    return c;
}

StmtPtr Parser::parseMacro() {
    advance();
    auto m = std::make_unique<MacroItem>();
    m->name = expect(TokenKind::Identifier, "Expected macro name").lexeme;
    expect(TokenKind::LParen, "Expected '('");
    while (!check(TokenKind::RParen) && !atEnd()) {
        m->params.push_back(advance().lexeme);
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RParen, "Expected ')'");
    expect(TokenKind::LBrace, "Expected '{'");
    while (!check(TokenKind::RBrace) && !atEnd()) {
        if (match(TokenKind::Newline)) continue;
        m->body.push_back(parseStmt());
    }
    expect(TokenKind::RBrace, "Expected '}'");
    return m;
}

// ─── Statements ──────────────────────────────────────────────────────────────
StmtPtr Parser::parseStmt() {
    while (match(TokenKind::Newline)) {}
    if (check(TokenKind::Kw_let) || check(TokenKind::Kw_var)) return parseLet();
    if (check(TokenKind::Kw_fn)    || check(TokenKind::Kw_struct) || check(TokenKind::Kw_class) ||
        check(TokenKind::Kw_enum)  || check(TokenKind::Kw_trait)  || check(TokenKind::Kw_impl)  ||
        check(TokenKind::Kw_const) || check(TokenKind::Kw_type)   || check(TokenKind::Kw_pub)   ||
        check(TokenKind::Kw_import)) return parseItem();

    auto e = parseExpr();
    bool hasSemi = matchAny({TokenKind::Semicolon, TokenKind::Newline});
    auto s = std::make_unique<ExprStmt>();
    s->expr    = std::move(e);
    s->hasSemi = hasSemi;
    return s;
}

StmtPtr Parser::parseLet() {
    auto letLoc = loc();
    bool isMut = check(TokenKind::Kw_var);
    advance(); // let/var
    auto stmt = std::make_unique<LetStmt>();
    stmt->loc   = letLoc;
    stmt->isMut = isMut;
    stmt->pat   = parsePattern();
    if (match(TokenKind::Colon)) stmt->ty = parseType();
    if (match(TokenKind::Eq))    stmt->init = parseExpr();
    matchAny({TokenKind::Semicolon, TokenKind::Newline});
    return stmt;
}

// ─── Types ────────────────────────────────────────────────────────────────────
TypePtr Parser::parseType() {
    // &T, &mut T
    if (match(TokenKind::Ampersand)) {
        auto r = std::make_unique<RefType>();
        r->isMut  = match(TokenKind::Kw_var);
        r->inner  = parseType();
        return r;
    }
    // *T, *mut T
    if (match(TokenKind::Star)) {
        auto p = std::make_unique<PtrType>();
        p->isMut  = match(TokenKind::Kw_var);
        p->inner  = parseType();
        return p;
    }
    // [T] or [T; N]
    if (match(TokenKind::LBracket)) {
        auto elem = parseType();
        if (match(TokenKind::Semicolon)) {
            // [T; N]
            auto arr = std::make_unique<ArrayType>();
            arr->elem = std::move(elem);
            auto sizeExpr = parseExpr();
            arr->size = 0; // resolved during semantic analysis
            expect(TokenKind::RBracket, "Expected ']'");
            return arr;
        }
        expect(TokenKind::RBracket, "Expected ']'");
        auto sl = std::make_unique<SliceType>();
        sl->elem = std::move(elem);
        return sl;
    }
    // (T, U, ...)
    if (check(TokenKind::LParen)) return parseTupleType();
    // fn(T) -> R
    if (check(TokenKind::Kw_fn)) return parseFnType();
    // ?T
    if (match(TokenKind::Question)) {
        auto n = std::make_unique<NullableType>();
        n->inner = parseType();
        return n;
    }
    // Named type with generics
    auto name = expect(TokenKind::Identifier, "Expected type name").lexeme;
    auto named = std::make_unique<NamedType>();
    named->name = name;
    if (match(TokenKind::ColonColon) && check(TokenKind::Lt)) {
        // path::type<A,B>
        named->args = parseGenericArgs();
    } else if (check(TokenKind::Lt) && !check2(TokenKind::Eq)) {
        named->args = parseGenericArgs();
    }
    return named;
}

TypePtr Parser::parseTupleType() {
    expect(TokenKind::LParen, "Expected '('");
    auto t = std::make_unique<TupleType>();
    if (!check(TokenKind::RParen)) {
        t->elems.push_back(parseType());
        while (match(TokenKind::Comma) && !check(TokenKind::RParen))
            t->elems.push_back(parseType());
    }
    expect(TokenKind::RParen, "Expected ')'");
    return t;
}

TypePtr Parser::parseFnType() {
    advance(); // fn
    auto ft = std::make_unique<FnType>();
    expect(TokenKind::LParen, "Expected '('");
    while (!check(TokenKind::RParen) && !atEnd()) {
        ft->params.push_back(parseType());
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RParen, "Expected ')'");
    if (match(TokenKind::Arrow)) ft->ret = parseType();
    else { auto u = std::make_unique<NamedType>(); u->name = "unit"; ft->ret = std::move(u); }
    return ft;
}

std::vector<TypePtr> Parser::parseGenericArgs() {
    std::vector<TypePtr> args;
    expect(TokenKind::Lt, "Expected '<'");
    while (!check(TokenKind::Gt) && !atEnd()) {
        args.push_back(parseType());
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::Gt, "Expected '>'");
    return args;
}

// ─── Patterns ─────────────────────────────────────────────────────────────────
PatPtr Parser::parsePattern() {
    // Or-pattern
    auto first = parseLiteralPat();
    if (check(TokenKind::Pipe)) {
        auto op = std::make_unique<OrPat>();
        op->alts.push_back(std::move(first));
        while (match(TokenKind::Pipe))
            op->alts.push_back(parseLiteralPat());
        return op;
    }
    return first;
}

PatPtr Parser::parseLiteralPat() {
    auto l = loc();
    // Wildcard _
    if (check(TokenKind::Identifier) && peek().lexeme == "_") {
        advance();
        return std::make_unique<WildcardPat>();
    }
    // Literal
    if (check(TokenKind::Integer) || check(TokenKind::Float) || check(TokenKind::String) ||
        check(TokenKind::Bool) || check(TokenKind::Kw_true) || check(TokenKind::Kw_false) ||
        check(TokenKind::Kw_nil)) {
        auto lp = std::make_unique<LiteralPat>();
        lp->tok = advance();
        return lp;
    }
    // Tuple (...)
    if (check(TokenKind::LParen)) return parseTuplePat();
    // Name-based: Enum::Variant or binding
    if (check(TokenKind::Identifier)) {
        std::string name = advance().lexeme;
        if (match(TokenKind::ColonColon)) {
            // Enum path
            std::string variant = expect(TokenKind::Identifier, "Expected variant name").lexeme;
            auto ep = std::make_unique<EnumPat>();
            ep->path = name + "::" + variant;
            if (check(TokenKind::LParen)) {
                advance();
                while (!check(TokenKind::RParen) && !atEnd()) {
                    ep->inner.push_back(parsePattern());
                    if (!match(TokenKind::Comma)) break;
                }
                expect(TokenKind::RParen, "Expected ')'");
            }
            return ep;
        }
        if (check(TokenKind::LBrace)) return parseStructPat(name);
        // Range pattern a..=b
        if (check(TokenKind::DotDotEq) || check(TokenKind::DotDot)) {
            bool inc = check(TokenKind::DotDotEq);
            advance();
            auto rp = std::make_unique<RangePat>();
            auto lo = std::make_unique<NamePat>(); lo->name = name; lo->isMut = false;
            rp->lo = std::move(lo);
            rp->hi = parsePattern();
            rp->inclusive = inc;
            return rp;
        }
        // Simple name binding
        bool isMut = false;
        auto np = std::make_unique<NamePat>();
        np->name  = name;
        np->isMut = isMut;
        return np;
    }
    // Fallback: wildcard
    return std::make_unique<WildcardPat>();
}

PatPtr Parser::parseTuplePat() {
    expect(TokenKind::LParen, "Expected '('");
    auto tp = std::make_unique<TuplePat>();
    while (!check(TokenKind::RParen) && !atEnd()) {
        tp->pats.push_back(parsePattern());
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RParen, "Expected ')'");
    return tp;
}

PatPtr Parser::parseStructPat(const std::string& name) {
    expect(TokenKind::LBrace, "Expected '{'");
    auto sp = std::make_unique<StructPat>();
    sp->name = name;
    while (!check(TokenKind::RBrace) && !atEnd()) {
        std::string field = expect(TokenKind::Identifier, "Expected field name").lexeme;
        PatPtr subpat;
        if (match(TokenKind::Colon)) subpat = parsePattern();
        else { auto np = std::make_unique<NamePat>(); np->name = field; np->isMut = false; subpat = std::move(np); }
        sp->fields.emplace_back(field, std::move(subpat));
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RBrace, "Expected '}'");
    return sp;
}

// ─── Expressions ─────────────────────────────────────────────────────────────
ExprPtr Parser::parseExpr(int /*minPrec*/) {
    auto lhs = parsePrefix();
    return parsePostfix(std::move(lhs));
}

ExprPtr Parser::parsePrefix() {
    auto l = loc();
    if (check(TokenKind::Kw_return))   return parseReturn();
    if (check(TokenKind::Kw_break))    return parseBreak();
    if (check(TokenKind::Kw_continue)) return parseContinue();
    if (check(TokenKind::Kw_yield))    return parseYield();
    if (check(TokenKind::Kw_if))       return parseIf();
    if (check(TokenKind::Kw_while))    return parseWhile();
    if (check(TokenKind::Kw_for))      return parseFor();
    if (check(TokenKind::Kw_match))    return parseMatch();
    if (check(TokenKind::LBrace))      return parseBlock();
    if (check(TokenKind::Kw_fn))       return parseClosure();
    if (check(TokenKind::Kw_lambda))   return parseLambda();
    if (check(TokenKind::Kw_spawn)) {
        advance();
        auto se = std::make_unique<SpawnExpr>();
        se->loc  = l;
        se->expr = parseExpr();
        return se;
    }
    if (check(TokenKind::Kw_new)) {
        advance();
        auto ne = std::make_unique<NewExpr>();
        ne->loc  = l;
        ne->ty   = parseType();
        if (check(TokenKind::LParen)) ne->args = parseArgList();
        return ne;
    }
    if (check(TokenKind::Kw_del)) {
        advance();
        auto de = std::make_unique<DeleteExpr>();
        de->loc  = l;
        de->ptr  = parseExpr();
        return de;
    }
    if (check(TokenKind::Kw_sizeof)) {
        advance();
        expect(TokenKind::LParen, "Expected '('");
        auto se = std::make_unique<SizeofExpr>();
        se->loc  = l;
        se->ty   = parseType();
        expect(TokenKind::RParen, "Expected ')'");
        return se;
    }
    if (check(TokenKind::Kw_typeof)) {
        advance();
        expect(TokenKind::LParen, "Expected '('");
        auto te = std::make_unique<TypeofExpr>();
        te->loc  = l;
        te->expr = parseExpr();
        expect(TokenKind::RParen, "Expected ')'");
        return te;
    }
    // Prefix operators
    if (isPrefixOp(peek().kind)) {
        auto op = advance();
        auto e  = std::make_unique<UnaryExpr>();
        e->loc  = l;
        e->op   = op;
        e->expr = parsePrefix();
        return e;
    }
    return parsePrimary();
}

ExprPtr Parser::parsePostfix(ExprPtr lhs) {
    for (;;) {
        auto l = loc();
        int prec = infixPrec(peek().kind);
        if (prec < 0) break;

        TokenKind k = peek().kind;

        // Assignment
        if (isAssignOp(k)) {
            auto op = advance();
            auto rhs = parseExpr(isRightAssoc(op.kind) ? prec : prec + 1);
            auto ae = std::make_unique<AssignExpr>();
            ae->loc = l; ae->op = op;
            ae->lhs = std::move(lhs); ae->rhs = std::move(rhs);
            lhs = std::move(ae);
            continue;
        }

        // Await
        if (k == TokenKind::Dot && pos_+1 < toks_.size() &&
            toks_[pos_+1].lexeme == "await") {
            advance(); advance();
            auto aw = std::make_unique<AwaitExpr>();
            aw->expr = std::move(lhs);
            lhs = std::move(aw);
            continue;
        }

        // Field access / method call
        if (k == TokenKind::Dot || k == TokenKind::QuestionDot) {
            bool isSafe = k == TokenKind::QuestionDot;
            advance();
            std::string field = expect(TokenKind::Identifier, "Expected field/method name").lexeme;
            if (check(TokenKind::LParen)) {
                auto args = parseArgList();
                auto me = std::make_unique<MethodExpr>();
                me->loc = l; me->obj = std::move(lhs);
                me->method = field; me->args = std::move(args);
                me->isSafe = isSafe;
                lhs = std::move(me);
            } else {
                auto fe = std::make_unique<FieldExpr>();
                fe->loc = l; fe->obj = std::move(lhs);
                fe->field = field; fe->isSafe = isSafe;
                lhs = std::move(fe);
            }
            continue;
        }

        // Index
        if (k == TokenKind::LBracket) {
            advance();
            auto idx = parseExpr();
            expect(TokenKind::RBracket, "Expected ']'");
            auto ie = std::make_unique<IndexExpr>();
            ie->loc = l; ie->obj = std::move(lhs); ie->idx = std::move(idx);
            lhs = std::move(ie);
            continue;
        }

        // Call (postfix)
        if (k == TokenKind::LParen) {
            auto args = parseArgList();
            auto ce = std::make_unique<CallExpr>();
            ce->loc = l; ce->callee = std::move(lhs); ce->args = std::move(args);
            lhs = std::move(ce);
            continue;
        }

        // Cast
        if (k == TokenKind::Kw_as) {
            advance();
            auto ty = parseType();
            auto cast = std::make_unique<CastExpr>();
            cast->loc = l; cast->expr = std::move(lhs); cast->ty = std::move(ty);
            lhs = std::move(cast);
            continue;
        }

        // Range
        if (k == TokenKind::DotDot || k == TokenKind::DotDotEq) {
            bool inc = k == TokenKind::DotDotEq;
            advance();
            auto rhs = parseExpr(prec + 1);
            auto re = std::make_unique<RangeExpr>();
            re->loc = l; re->lo = std::move(lhs); re->hi = std::move(rhs); re->inclusive = inc;
            lhs = std::move(re);
            continue;
        }

        // Try operator
        if (k == TokenKind::Question) {
            advance();
            auto te = std::make_unique<TryExpr>();
            te->loc = l; te->expr = std::move(lhs);
            lhs = std::move(te);
            continue;
        }

        // Binary op
        if (isBinaryOp(k)) {
            auto op  = advance();
            auto rhs = parseExpr(isRightAssoc(k) ? prec : prec + 1);
            auto be  = std::make_unique<BinaryExpr>();
            be->loc  = l; be->op = op;
            be->lhs  = std::move(lhs); be->rhs = std::move(rhs);
            lhs = std::move(be);
            continue;
        }

        break;
    }
    return lhs;
}

ExprPtr Parser::parsePrimary() {
    auto l = loc();
    // Literals
    if (check(TokenKind::Integer) || check(TokenKind::Float) ||
        check(TokenKind::Char) || check(TokenKind::Kw_nil) ||
        check(TokenKind::Kw_true) || check(TokenKind::Kw_false)) {
        auto le = std::make_unique<LiteralExpr>();
        le->loc = l; le->tok = advance();
        return le;
    }
    if (check(TokenKind::String)) {
        // Could be interpolated
        auto tok = advance();
        // Check for sentinel interp chars
        if (tok.lexeme.find('\x01') != std::string::npos)
            return parseStringInterp(tok.lexeme, l);
        auto le = std::make_unique<LiteralExpr>();
        le->loc = l; le->tok = tok;
        return le;
    }
    // Array literal
    if (check(TokenKind::LBracket)) return parseArrayLit();
    // Map/Set literal or block
    if (check(TokenKind::LBrace)) {
        // heuristic: if next token is string/ident followed by colon → map
        if (pos_+2 < toks_.size() &&
            (toks_[pos_+1].kind == TokenKind::String || toks_[pos_+1].kind == TokenKind::Identifier) &&
            toks_[pos_+2].kind == TokenKind::Colon)
            return parseMapOrSetLit();
        return parseBlock();
    }
    // Tuple or parenthesised expression
    if (check(TokenKind::LParen)) return parseTupleOrParens();
    // Identifier
    if (check(TokenKind::Identifier)) {
        auto ie = std::make_unique<IdentExpr>();
        ie->loc = l; ie->name = advance().lexeme;
        return ie;
    }
    error("Unexpected token: '" + peek().lexeme + "'");
    advance(); // recover
    auto nil = std::make_unique<LiteralExpr>();
    Token nilTok; nilTok.kind = TokenKind::Kw_nil; nilTok.lexeme = "nil"; nilTok.loc = l;
    nil->tok = nilTok;
    return nil;
}

ExprList Parser::parseArgList() {
    ExprList args;
    expect(TokenKind::LParen, "Expected '('");
    while (!check(TokenKind::RParen) && !atEnd()) {
        if (match(TokenKind::Newline)) continue;
        args.push_back(parseExpr());
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RParen, "Expected ')'");
    return args;
}

ExprPtr Parser::parseBlock() {
    auto l = loc();
    expect(TokenKind::LBrace, "Expected '{'");
    auto be = std::make_unique<BlockExpr>();
    be->loc = l;
    while (!check(TokenKind::RBrace) && !atEnd()) {
        if (match(TokenKind::Newline)) continue;
        auto s = parseStmt();
        // Check if last stmt is a tail expr
        be->stmts.push_back(std::move(s));
    }
    expect(TokenKind::RBrace, "Expected '}'");
    // Detect tail expression (last stmt with no semicolon)
    if (!be->stmts.empty()) {
        if (auto* es = dynamic_cast<ExprStmt*>(be->stmts.back().get())) {
            if (!es->hasSemi) {
                be->tail = std::move(es->expr);
                be->stmts.pop_back();
            }
        }
    }
    return be;
}

ExprPtr Parser::parseIf() {
    auto l = loc();
    advance(); // if
    auto cond = parseExpr();
    auto then = parseBlock();
    std::optional<ExprPtr> els;
    if (match(TokenKind::Kw_else)) {
        if (check(TokenKind::Kw_if)) els = parseIf();
        else els = parseBlock();
    }
    auto ie = std::make_unique<IfExpr>();
    ie->loc = l; ie->cond = std::move(cond);
    ie->then = std::move(then); ie->els = std::move(els);
    return ie;
}

ExprPtr Parser::parseWhile() {
    auto l = loc();
    advance();
    std::optional<ExprPtr> cond;
    if (!check(TokenKind::LBrace)) cond = parseExpr();
    auto body = parseBlock();
    auto we = std::make_unique<WhileExpr>();
    we->loc = l; we->cond = std::move(cond); we->body = std::move(body);
    return we;
}

ExprPtr Parser::parseFor() {
    auto l = loc();
    advance(); // for
    auto pat  = parsePattern();
    expect(TokenKind::Kw_in, "Expected 'in'");
    auto iter = parseExpr();
    auto body = parseBlock();
    auto fe = std::make_unique<ForExpr>();
    fe->loc = l; fe->pat = std::move(pat); fe->iter = std::move(iter); fe->body = std::move(body);
    return fe;
}

ExprPtr Parser::parseMatch() {
    auto l = loc();
    advance(); // match
    auto expr = parseExpr();
    expect(TokenKind::LBrace, "Expected '{'");
    MatchExpr::Arm arms_list_tmp; // workaround
    std::vector<MatchExpr::Arm> arms;
    while (!check(TokenKind::RBrace) && !atEnd()) {
        while (match(TokenKind::Newline)) {}
        if (check(TokenKind::RBrace)) break;
        MatchExpr::Arm arm;
        arm.pat = parsePattern();
        if (match(TokenKind::Kw_if)) arm.guard = parseExpr();
        expect(TokenKind::FatArrow, "Expected '=>'");
        arm.body = parseExpr();
        arms.push_back(std::move(arm));
        matchAny({TokenKind::Comma, TokenKind::Newline});
    }
    expect(TokenKind::RBrace, "Expected '}'");
    auto me = std::make_unique<MatchExpr>();
    me->loc = l; me->expr = std::move(expr); me->arms = std::move(arms);
    return me;
}

ExprPtr Parser::parseClosure() {
    auto l = loc();
    advance(); // fn
    auto cl = std::make_unique<ClosureExpr>();
    cl->loc     = l;
    cl->isAsync = false;
    std::optional<std::string> selfP;
    cl->params  = parseParamList(selfP);
    if (match(TokenKind::Arrow)) cl->retTy = parseType();
    // body
    auto body = parseBlock();
    if (auto* be = dynamic_cast<BlockExpr*>(body.get()))
        cl->body = std::move(be->stmts);
    return cl;
}

ExprPtr Parser::parseLambda() {
    auto l = loc();
    advance(); // lambda
    expect(TokenKind::LParen, "Expected '('");
    ParamList params;
    while (!check(TokenKind::RParen) && !atEnd()) {
        std::string pname = expect(TokenKind::Identifier, "").lexeme;
        TypePtr ty;
        if (match(TokenKind::Colon)) ty = parseType();
        else { auto inf = std::make_unique<InferType>(); ty = std::move(inf); }
        params.emplace_back(pname, std::move(ty));
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RParen, "Expected ')'");
    expect(TokenKind::FatArrow, "Expected '=>'");
    auto body = parseExpr();
    auto la = std::make_unique<LambdaExpr>();
    la->loc = l; la->params = std::move(params); la->body = std::move(body);
    return la;
}

ExprPtr Parser::parseReturn() {
    auto l = loc(); advance();
    auto re = std::make_unique<ReturnExpr>();
    re->loc = l;
    if (!check(TokenKind::Semicolon) && !check(TokenKind::RBrace) &&
        !check(TokenKind::Newline) && !atEnd())
        re->val = parseExpr();
    return re;
}

ExprPtr Parser::parseBreak() {
    auto l = loc(); advance();
    auto be = std::make_unique<BreakExpr>();
    be->loc = l;
    if (!check(TokenKind::Semicolon) && !check(TokenKind::RBrace) &&
        !check(TokenKind::Newline) && !atEnd())
        be->val = parseExpr();
    return be;
}

ExprPtr Parser::parseContinue() {
    auto l = loc(); advance();
    return std::make_unique<ContinueExpr>();
}

ExprPtr Parser::parseYield() {
    auto l = loc(); advance();
    auto ye = std::make_unique<YieldExpr>();
    ye->loc = l;
    if (!check(TokenKind::Semicolon) && !check(TokenKind::RBrace) && !check(TokenKind::Newline))
        ye->val = parseExpr();
    return ye;
}

ExprPtr Parser::parseArrayLit() {
    auto l = loc();
    expect(TokenKind::LBracket, "Expected '['");
    auto ae = std::make_unique<ArrayExpr>();
    ae->loc = l;
    while (!check(TokenKind::RBracket) && !atEnd()) {
        ae->elems.push_back(parseExpr());
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RBracket, "Expected ']'");
    return ae;
}

ExprPtr Parser::parseMapOrSetLit() {
    auto l = loc();
    expect(TokenKind::LBrace, "Expected '{'");
    // Peek: if key: value → map, else set
    if (pos_ < toks_.size() && pos_+1 < toks_.size() && toks_[pos_+1].kind == TokenKind::Colon) {
        auto me = std::make_unique<MapExpr>();
        me->loc = l;
        while (!check(TokenKind::RBrace) && !atEnd()) {
            auto key = parseExpr();
            expect(TokenKind::Colon, "Expected ':'");
            auto val = parseExpr();
            me->pairs.emplace_back(std::move(key), std::move(val));
            if (!match(TokenKind::Comma)) break;
        }
        expect(TokenKind::RBrace, "Expected '}'");
        return me;
    }
    auto se = std::make_unique<SetExpr>();
    se->loc = l;
    while (!check(TokenKind::RBrace) && !atEnd()) {
        se->elems.push_back(parseExpr());
        if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RBrace, "Expected '}'");
    return se;
}

ExprPtr Parser::parseTupleOrParens() {
    auto l = loc();
    expect(TokenKind::LParen, "Expected '('");
    if (check(TokenKind::RParen)) {
        advance();
        auto te = std::make_unique<TupleExpr>();
        te->loc = l;
        return te;
    }
    auto first = parseExpr();
    if (match(TokenKind::Comma)) {
        auto te = std::make_unique<TupleExpr>();
        te->loc = l;
        te->elems.push_back(std::move(first));
        while (!check(TokenKind::RParen) && !atEnd()) {
            te->elems.push_back(parseExpr());
            if (!match(TokenKind::Comma)) break;
        }
        expect(TokenKind::RParen, "Expected ')'");
        return te;
    }
    expect(TokenKind::RParen, "Expected ')'");
    return first; // just parenthesised
}

ExprPtr Parser::parseStringInterp(const std::string& raw, SourceLocation l) {
    auto si = std::make_unique<StringInterp>();
    si->loc = l;
    std::string cur;
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '\x01') {
            if (!cur.empty()) { si->parts.push_back(cur); cur.clear(); }
            ++i;
            std::string inner;
            while (i < raw.size() && raw[i] != '\x02') inner += raw[i++];
            ++i; // skip \x02
            // Re-parse the inner expression
            Lexer lx(inner, "<interp>");
            auto toks = lx.tokenize();
            Parser p(std::move(toks), "<interp>");
            auto m = p.parse();
            if (!m.items.empty()) {
                if (auto* es = dynamic_cast<ExprStmt*>(m.items[0].get()))
                    si->parts.push_back(std::move(es->expr));
            }
        } else {
            cur += raw[i++];
        }
    }
    if (!cur.empty()) si->parts.push_back(cur);
    return si;
}

} // namespace fpp
