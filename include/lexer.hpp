#pragma once
#ifdef _WIN32
#  define _CRT_SECURE_NO_WARNINGS
#  define NOMINMAX
#endif
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <optional>
#include <cstdint>
#include <stdexcept>

namespace fpp {

enum class TokenKind : uint16_t {
    // Literals
    Integer, Float, String, Char, Bool, Null,
    // Identifiers & Keywords
    Identifier,
    Kw_let, Kw_var, Kw_const, Kw_fn, Kw_return, Kw_if, Kw_else,
    Kw_while, Kw_for, Kw_in, Kw_break, Kw_continue,
    Kw_class, Kw_struct, Kw_enum, Kw_interface, Kw_impl,
    Kw_import, Kw_export, Kw_module, Kw_pub, Kw_priv,
    Kw_async, Kw_await, Kw_spawn, Kw_chan,
    Kw_match, Kw_case, Kw_default,
    Kw_try, Kw_catch, Kw_throw, Kw_finally,
    Kw_type, Kw_trait, Kw_where, Kw_as,
    Kw_static, Kw_extern, Kw_inline, Kw_unsafe,
    Kw_true, Kw_false, Kw_nil,
    Kw_new, Kw_del, Kw_sizeof, Kw_typeof, Kw_instanceof,
    Kw_yield, Kw_lambda, Kw_macro,
    // Operators
    Plus, Minus, Star, Slash, Percent, Caret, Ampersand, Pipe, Tilde,
    Bang, Lt, Gt, Eq, LtEq, GtEq, EqEq, BangEq,
    LtLt, GtGt, GtGtGt,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    AmpEq, PipeEq, CaretEq, LtLtEq, GtGtEq,
    AmpAmp, PipePipe,
    Arrow, FatArrow, ColonColon, DotDot, DotDotEq,
    Question, QuestionQuestion, QuestionDot,
    At, Hash, Dollar, Backslash,
    // Delimiters
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semicolon, Colon, Dot, Ellipsis,
    // Special
    Eof, Newline, Indent, Dedent, Unknown,
    // Compound assignment
    StarStar, StarStarEq,
};

struct SourceLocation {
    uint32_t line   = 1;
    uint32_t col    = 1;
    uint32_t offset = 0;
    std::string file;
};

struct Token {
    TokenKind        kind;
    std::string      lexeme;
    SourceLocation   loc;

    using LiteralVal = std::variant<std::monostate, int64_t, double, std::string, bool>;
    LiteralVal literal;

    bool is(TokenKind k) const noexcept { return kind == k; }
    bool isKeyword()     const noexcept { return kind >= TokenKind::Kw_let && kind <= TokenKind::Kw_macro; }
    bool isLiteral()     const noexcept { return kind >= TokenKind::Integer && kind <= TokenKind::Null; }
    bool isEof()         const noexcept { return kind == TokenKind::Eof; }
};

class LexerError : public std::runtime_error {
public:
    SourceLocation loc;
    LexerError(const std::string& msg, SourceLocation l)
        : std::runtime_error(msg), loc(std::move(l)) {}
};

class Lexer {
public:
    explicit Lexer(std::string source, std::string filename = "<stdin>");

    std::vector<Token> tokenize();
    Token              nextToken();
    const std::string& source() const noexcept { return src_; }

private:
    std::string src_;
    std::string file_;
    size_t      pos_  = 0;
    uint32_t    line_ = 1;
    uint32_t    col_  = 1;

    static const std::unordered_map<std::string, TokenKind> keywords_;

    char        peek(size_t ahead = 0) const noexcept;
    char        advance() noexcept;
    bool        match(char c) noexcept;
    bool        matchStr(const std::string& s) noexcept;
    void        skipWhitespaceAndComments();
    Token       makeToken(TokenKind k, const std::string& lexeme) const;
    Token       lexNumber();
    Token       lexString(char delim);
    Token       lexChar();
    Token       lexIdentOrKeyword();
    Token       lexOperatorOrDelim();
    SourceLocation currentLoc() const noexcept;
};

} // namespace fpp
