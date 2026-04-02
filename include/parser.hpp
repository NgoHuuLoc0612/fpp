#pragma once
#include "lexer.hpp"
#include "ast.hpp"
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <optional>
#include <set>

namespace fpp {

class ParseError : public std::runtime_error {
public:
    SourceLocation loc;
    ParseError(const std::string& msg, SourceLocation l)
        : std::runtime_error(msg), loc(std::move(l)) {}
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens, std::string filename = "<stdin>");

    ast::Module parse();
    const std::vector<ParseError>& errors() const { return errors_; }

private:
    std::vector<Token>     toks_;
    size_t                 pos_ = 0;
    std::string            file_;
    std::vector<ParseError> errors_;
    // Panic-mode recovery set
    static const std::set<TokenKind> syncSet_;

    // ── Token access ──────────────────────────────────────────────────────
    const Token& peek(size_t ahead = 0) const;
    const Token& advance();
    bool         check(TokenKind k) const;
    bool         check2(TokenKind k) const; // peek ahead by 1
    bool         match(TokenKind k);
    bool         matchAny(std::initializer_list<TokenKind> ks);
    const Token& expect(TokenKind k, const std::string& msg = "");
    void         synchronize();
    bool         atEnd() const;

    // ── Top-level parsing ─────────────────────────────────────────────────
    ast::StmtPtr  parseItem();
    ast::StmtPtr  parseFn(bool isPublic, bool isAsync, bool isExtern, bool isUnsafe, bool isStatic, bool isInline);
    ast::StmtPtr  parseStruct(bool isPublic);
    ast::StmtPtr  parseEnum(bool isPublic);
    ast::StmtPtr  parseTrait(bool isPublic);
    ast::StmtPtr  parseImpl();
    ast::StmtPtr  parseClass(bool isPublic);
    ast::StmtPtr  parseInterface(bool isPublic);
    ast::StmtPtr  parseTypeAlias(bool isPublic);
    ast::StmtPtr  parseModule(bool isPublic);
    ast::StmtPtr  parseImport();
    ast::StmtPtr  parseConst(bool isPublic);
    ast::StmtPtr  parseStatic(bool isPublic);
    ast::StmtPtr  parseMacro();

    // ── Generics ─────────────────────────────────────────────────────────
    std::vector<ast::GenericParam> parseGenericParams();
    std::vector<ast::TypePtr>      parseGenericArgs();
    ast::TypePtr                   parseBound();

    // ── Attributes ───────────────────────────────────────────────────────
    std::vector<ast::Attribute>    parseAttributes();

    // ── Statements ────────────────────────────────────────────────────────
    ast::StmtPtr  parseStmt();
    ast::StmtPtr  parseLet();

    // ── Expressions (Pratt parser) ────────────────────────────────────────
    ast::ExprPtr  parseExpr(int minPrec = 0);
    ast::ExprPtr  parsePrefix();
    ast::ExprPtr  parsePostfix(ast::ExprPtr lhs);
    ast::ExprPtr  parsePrimary();
    ast::ExprPtr  parseBlock();
    ast::ExprPtr  parseIf();
    ast::ExprPtr  parseWhile();
    ast::ExprPtr  parseFor();
    ast::ExprPtr  parseMatch();
    ast::ExprPtr  parseClosure();
    ast::ExprPtr  parseLambda();
    ast::ExprPtr  parseReturn();
    ast::ExprPtr  parseBreak();
    ast::ExprPtr  parseContinue();
    ast::ExprPtr  parseYield();
    ast::ExprPtr  parseAwait(ast::ExprPtr expr);
    ast::ExprPtr  parseStringInterp(const std::string& raw, SourceLocation loc);
    ast::ExprList parseArgList();
    ast::ExprPtr  parseSpread();
    ast::ExprPtr  parseArrayLit();
    ast::ExprPtr  parseMapOrSetLit();
    ast::ExprPtr  parseTupleOrParens();

    // ── Types ─────────────────────────────────────────────────────────────
    ast::TypePtr  parseType();
    ast::TypePtr  parsePrimitiveType(const std::string& name);
    ast::TypePtr  parseFnType();
    ast::TypePtr  parseTupleType();

    // ── Patterns ─────────────────────────────────────────────────────────
    ast::PatPtr   parsePattern();
    ast::PatPtr   parseLiteralPat();
    ast::PatPtr   parseTuplePat();
    ast::PatPtr   parseStructPat(const std::string& name);

    // ── Struct/Enum helpers ───────────────────────────────────────────────
    ast::FieldDef   parseFieldDef();
    ast::VariantDef parseVariantDef();
    ast::ParamList  parseParamList(std::optional<std::string>& selfParam);

    // ── Operators / Precedence ────────────────────────────────────────────
    static int  infixPrec(TokenKind k) noexcept;
    static bool isRightAssoc(TokenKind k) noexcept;
    static bool isAssignOp(TokenKind k) noexcept;
    static bool isBinaryOp(TokenKind k) noexcept;
    static bool isPrefixOp(TokenKind k) noexcept;

    // ── Utilities ─────────────────────────────────────────────────────────
    SourceLocation loc() const;
    void error(const std::string& msg);
    void error(const std::string& msg, SourceLocation l);
};

} // namespace fpp
