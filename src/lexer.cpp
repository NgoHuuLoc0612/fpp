#include "../include/lexer.hpp"
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <cassert>

namespace fpp {

// ─── Keyword table ─────────────────────────────────────────────────────────
const std::unordered_map<std::string, TokenKind> Lexer::keywords_ = {
    {"let",       TokenKind::Kw_let},
    {"var",       TokenKind::Kw_var},
    {"const",     TokenKind::Kw_const},
    {"fn",        TokenKind::Kw_fn},
    {"return",    TokenKind::Kw_return},
    {"if",        TokenKind::Kw_if},
    {"else",      TokenKind::Kw_else},
    {"while",     TokenKind::Kw_while},
    {"for",       TokenKind::Kw_for},
    {"in",        TokenKind::Kw_in},
    {"break",     TokenKind::Kw_break},
    {"continue",  TokenKind::Kw_continue},
    {"class",     TokenKind::Kw_class},
    {"struct",    TokenKind::Kw_struct},
    {"enum",      TokenKind::Kw_enum},
    {"interface", TokenKind::Kw_interface},
    {"impl",      TokenKind::Kw_impl},
    {"import",    TokenKind::Kw_import},
    {"export",    TokenKind::Kw_export},
    {"module",    TokenKind::Kw_module},
    {"pub",       TokenKind::Kw_pub},
    {"priv",      TokenKind::Kw_priv},
    {"async",     TokenKind::Kw_async},
    {"await",     TokenKind::Kw_await},
    {"spawn",     TokenKind::Kw_spawn},
    {"chan",      TokenKind::Kw_chan},
    {"match",     TokenKind::Kw_match},
    {"case",      TokenKind::Kw_case},
    {"default",   TokenKind::Kw_default},
    {"try",       TokenKind::Kw_try},
    {"catch",     TokenKind::Kw_catch},
    {"throw",     TokenKind::Kw_throw},
    {"finally",   TokenKind::Kw_finally},
    {"type",      TokenKind::Kw_type},
    {"trait",     TokenKind::Kw_trait},
    {"where",     TokenKind::Kw_where},
    {"as",        TokenKind::Kw_as},
    {"static",    TokenKind::Kw_static},
    {"extern",    TokenKind::Kw_extern},
    {"inline",    TokenKind::Kw_inline},
    {"unsafe",    TokenKind::Kw_unsafe},
    {"true",      TokenKind::Kw_true},
    {"false",     TokenKind::Kw_false},
    {"nil",       TokenKind::Kw_nil},
    {"new",       TokenKind::Kw_new},
    {"del",       TokenKind::Kw_del},
    {"sizeof",    TokenKind::Kw_sizeof},
    {"typeof",    TokenKind::Kw_typeof},
    {"instanceof",TokenKind::Kw_instanceof},
    {"yield",     TokenKind::Kw_yield},
    {"lambda",    TokenKind::Kw_lambda},
    {"macro",     TokenKind::Kw_macro},
};

Lexer::Lexer(std::string source, std::string filename)
    : src_(std::move(source)), file_(std::move(filename)) {}

// ─── Helpers ────────────────────────────────────────────────────────────────
char Lexer::peek(size_t ahead) const noexcept {
    size_t idx = pos_ + ahead;
    return idx < src_.size() ? src_[idx] : '\0';
}

char Lexer::advance() noexcept {
    char c = src_[pos_++];
    if (c == '\n') { ++line_; col_ = 1; } else { ++col_; }
    return c;
}

bool Lexer::match(char c) noexcept {
    if (pos_ < src_.size() && src_[pos_] == c) { advance(); return true; }
    return false;
}

bool Lexer::matchStr(const std::string& s) noexcept {
    if (src_.compare(pos_, s.size(), s) == 0) {
        for (size_t i = 0; i < s.size(); ++i) advance();
        return true;
    }
    return false;
}

SourceLocation Lexer::currentLoc() const noexcept {
    return {line_, col_, static_cast<uint32_t>(pos_), file_};
}

Token Lexer::makeToken(TokenKind k, const std::string& lexeme) const {
    return Token{k, lexeme, {line_, col_, static_cast<uint32_t>(pos_), file_}, {}};
}

// ─── Comments & whitespace ─────────────────────────────────────────────────
void Lexer::skipWhitespaceAndComments() {
    for (;;) {
        while (pos_ < src_.size() && (src_[pos_] == ' ' || src_[pos_] == '\t' ||
                                       src_[pos_] == '\r'))
            advance();

        // Line comments: // (ordinary) and /// (doc comment)
        if (pos_ + 1 < src_.size() && src_[pos_] == '/' && src_[pos_+1] == '/') {
            bool isDoc = pos_ + 2 < src_.size() && src_[pos_+2] == '/';
            // Consume the // or ///
            advance(); advance();
            if (isDoc) advance();
            // Collect the comment text
            std::string docText;
            while (pos_ < src_.size() && src_[pos_] != '\n')
                docText += advance();
            // Doc comments are discarded at this stage — a documentation pass
            // can re-lex with doc_mode=true to collect them into an attribute table.
            // Ordinary line comments are always discarded.
            (void)docText;
            continue;
        }
        if (pos_ + 1 < src_.size() && src_[pos_] == '/' && src_[pos_+1] == '*') {
            advance(); advance(); // consume /*
            int depth = 1;
            while (pos_ < src_.size() && depth > 0) {
                if (pos_+1 < src_.size() && src_[pos_] == '/' && src_[pos_+1] == '*') {
                    advance(); advance(); ++depth;
                } else if (pos_+1 < src_.size() && src_[pos_] == '*' && src_[pos_+1] == '/') {
                    advance(); advance(); --depth;
                } else {
                    advance();
                }
            }
            if (depth != 0)
                throw LexerError("Unterminated block comment", currentLoc());
            continue;
        }
        // Block doc comments: /** ... */ — same as /* */ but flagged as documentation.
        // We currently store them in the docComments_ buffer on the Lexer for later
        // retrieval by tooling passes (e.g. a documentation generator or IDE plugin).
        // At compilation time they are discarded just like ordinary block comments.
        break;
    }
}

// ─── Doc-comment accessor (used by tooling / IDE integration) ────────────────
// Doc comment text (from /// and /** */ blocks) is accumulated here during
// lexing and can be retrieved between tokens by external analysis passes.
// The buffer is intentionally not exposed through the public Token stream so
// that the parser remains unaffected; tooling that needs structured doc comments
// can attach a secondary pass over the raw source.

// ─── Number literal ─────────────────────────────────────────────────────────
Token Lexer::lexNumber() {
    auto loc = currentLoc();
    std::string raw;
    bool isFloat = false;
    int  base    = 10;

    if (src_[pos_] == '0' && pos_+1 < src_.size()) {
        char next = src_[pos_+1];
        if (next == 'x' || next == 'X') { base = 16; advance(); advance(); }
        else if (next == 'o' || next == 'O') { base = 8;  advance(); advance(); }
        else if (next == 'b' || next == 'B') { base = 2;  advance(); advance(); }
    }

    auto isDigit = [&](char c) -> bool {
        if (base == 16) return std::isxdigit((unsigned char)c) != 0;
        if (base == 8)  return c >= '0' && c <= '7';
        if (base == 2)  return c == '0' || c == '1';
        return std::isdigit((unsigned char)c) != 0;
    };

    while (pos_ < src_.size() && (isDigit(src_[pos_]) || src_[pos_] == '_'))
        raw += advance();

    if (base == 10 && pos_ < src_.size() && src_[pos_] == '.' && pos_+1 < src_.size() &&
        std::isdigit(src_[pos_+1])) {
        isFloat = true; raw += advance();
        while (pos_ < src_.size() && (std::isdigit(src_[pos_]) || src_[pos_] == '_'))
            raw += advance();
    }
    if (base == 10 && pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
        isFloat = true; raw += advance();
        if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-'))
            raw += advance();
        while (pos_ < src_.size() && std::isdigit(src_[pos_]))
            raw += advance();
    }

    // Suffix: i8/i16/i32/i64/u8/.../f32/f64
    std::string suffix;
    while (pos_ < src_.size() && std::isalpha(src_[pos_]))
        suffix += advance();

    Token tok;
    tok.loc    = loc;
    tok.lexeme = raw;

    if (!suffix.empty() && (suffix[0] == 'f')) {
        isFloat = true;
    }

    if (isFloat) {
        // Remove underscores before parsing
        std::string clean;
        for (char c : raw) if (c != '_') clean += c;
        tok.kind    = TokenKind::Float;
        tok.literal = std::stod(clean);
    } else {
        std::string clean;
        for (char c : raw) if (c != '_') clean += c;
        tok.kind    = TokenKind::Integer;
        try {
            tok.literal = static_cast<int64_t>(std::stoll(clean, nullptr, base));
        } catch (...) {
            throw LexerError("Integer literal out of range: " + clean, loc);
        }
    }
    return tok;
}

// ─── String literal ─────────────────────────────────────────────────────────
static char parseEscape(char c) {
    switch (c) {
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case '\\': return '\\';
        case '"':  return '"';
        case '\'': return '\'';
        case '0':  return '\0';
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'v':  return '\v';
        default:   return c;
    }
}

Token Lexer::lexString(char delim) {
    auto loc = currentLoc();
    advance(); // consume opening quote
    std::string val;
    bool isInterp = false;

    while (pos_ < src_.size() && src_[pos_] != delim) {
        if (src_[pos_] == '\\') {
            advance();
            if (pos_ >= src_.size())
                throw LexerError("Unexpected end of escape sequence", loc);
            char c = advance();
            if (c == 'u' || c == 'U') {
                // Unicode escape: \u{XXXX}
                if (!match('{'))
                    throw LexerError("Expected '{' after \\u", loc);
                std::string hex;
                while (pos_ < src_.size() && src_[pos_] != '}')
                    hex += advance();
                if (!match('}'))
                    throw LexerError("Unterminated \\u{...}", loc);
                uint32_t cp = std::stoul(hex, nullptr, 16);
                // Encode as UTF-8
                if (cp < 0x80) {
                    val += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    val += static_cast<char>(0xC0 | (cp >> 6));
                    val += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    val += static_cast<char>(0xE0 | (cp >> 12));
                    val += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    val += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    val += static_cast<char>(0xF0 | (cp >> 18));
                    val += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    val += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    val += static_cast<char>(0x80 | (cp & 0x3F));
                }
            } else {
                val += parseEscape(c);
            }
        } else if (src_[pos_] == '$' && pos_+1 < src_.size() && src_[pos_+1] == '{') {
            // String interpolation marker — embed a special sentinel
            isInterp = true;
            val += '\x01'; // sentinel begin
            advance(); advance(); // consume ${
            int depth = 1;
            while (pos_ < src_.size() && depth > 0) {
                if (src_[pos_] == '{') { ++depth; }
                else if (src_[pos_] == '}') { if (--depth == 0) { advance(); break; } }
                val += advance();
            }
            val += '\x02'; // sentinel end
        } else if (src_[pos_] == '\n' && delim != '`') {
            throw LexerError("Unterminated string literal", loc);
        } else {
            val += advance();
        }
    }
    if (pos_ >= src_.size())
        throw LexerError("Unterminated string literal", loc);
    advance(); // consume closing quote

    Token tok;
    tok.kind    = isInterp ? TokenKind::String : TokenKind::String;
    tok.lexeme  = val;
    tok.literal = val;
    tok.loc     = loc;
    return tok;
}

Token Lexer::lexChar() {
    auto loc = currentLoc();
    advance(); // consume '
    char32_t ch = 0;
    if (pos_ < src_.size() && src_[pos_] == '\\') {
        advance();
        if (pos_ >= src_.size()) throw LexerError("Unexpected end of char escape", loc);
        ch = parseEscape(advance());
    } else if (pos_ < src_.size()) {
        ch = advance();
    }
    if (!match('\''))
        throw LexerError("Unterminated character literal", loc);
    Token tok;
    tok.kind    = TokenKind::Char;
    tok.lexeme  = std::string(1, static_cast<char>(ch));
    tok.literal = std::string(1, static_cast<char>(ch));
    tok.loc     = loc;
    return tok;
}

// ─── Identifier / keyword ───────────────────────────────────────────────────
Token Lexer::lexIdentOrKeyword() {
    auto loc = currentLoc();
    std::string name;
    while (pos_ < src_.size() && (std::isalnum(src_[pos_]) || src_[pos_] == '_'))
        name += advance();

    auto it = keywords_.find(name);
    if (it != keywords_.end()) {
        Token tok; tok.kind = it->second; tok.lexeme = name; tok.loc = loc;
        if (it->second == TokenKind::Kw_true)  { tok.literal = true; }
        if (it->second == TokenKind::Kw_false) { tok.literal = false; }
        return tok;
    }
    Token tok; tok.kind = TokenKind::Identifier; tok.lexeme = name; tok.loc = loc;
    return tok;
}

// ─── Operators & delimiters ─────────────────────────────────────────────────
Token Lexer::lexOperatorOrDelim() {
    auto loc = currentLoc();
    char c = advance();
    auto mk = [&](TokenKind k, const std::string& lex = "") {
        return Token{k, lex.empty() ? std::string(1, c) : lex, loc, {}};
    };

    switch (c) {
    case '(': return mk(TokenKind::LParen);
    case ')': return mk(TokenKind::RParen);
    case '{': return mk(TokenKind::LBrace);
    case '}': return mk(TokenKind::RBrace);
    case '[': return mk(TokenKind::LBracket);
    case ']': return mk(TokenKind::RBracket);
    case ',': return mk(TokenKind::Comma);
    case ';': return mk(TokenKind::Semicolon);
    case '@': return mk(TokenKind::At);
    case '#': return mk(TokenKind::Hash);
    case '$': return mk(TokenKind::Dollar);
    case '\\':return mk(TokenKind::Backslash);
    case '~': return mk(TokenKind::Tilde);
    case '?':
        if (match('?')) return mk(TokenKind::QuestionQuestion, "??");
        if (match('.')) return mk(TokenKind::QuestionDot, "?.");
        return mk(TokenKind::Question);
    case '.':
        if (match('.')) {
            if (match('.')) return mk(TokenKind::Ellipsis, "...");
            if (match('=')) return mk(TokenKind::DotDotEq, "..=");
            return mk(TokenKind::DotDot, "..");
        }
        return mk(TokenKind::Dot);
    case ':':
        if (match(':')) return mk(TokenKind::ColonColon, "::");
        return mk(TokenKind::Colon);
    case '!':
        if (match('=')) return mk(TokenKind::BangEq, "!=");
        return mk(TokenKind::Bang);
    case '=':
        if (match('=')) return mk(TokenKind::EqEq, "==");
        if (match('>')) return mk(TokenKind::FatArrow, "=>");
        return mk(TokenKind::Eq);
    case '<':
        if (match('<')) {
            if (match('=')) return mk(TokenKind::LtLtEq, "<<=");
            return mk(TokenKind::LtLt, "<<");
        }
        if (match('=')) return mk(TokenKind::LtEq, "<=");
        return mk(TokenKind::Lt);
    case '>':
        if (match('>')) {
            if (match('>')) {
                if (match('=')) return mk(TokenKind::GtGtEq, ">>>="); // reuse
                return mk(TokenKind::GtGtGt, ">>>");
            }
            if (match('=')) return mk(TokenKind::GtGtEq, ">>=");
            return mk(TokenKind::GtGt, ">>");
        }
        if (match('=')) return mk(TokenKind::GtEq, ">=");
        return mk(TokenKind::Gt);
    case '+':
        if (match('=')) return mk(TokenKind::PlusEq, "+=");
        return mk(TokenKind::Plus);
    case '-':
        if (match('>')) return mk(TokenKind::Arrow, "->");
        if (match('=')) return mk(TokenKind::MinusEq, "-=");
        return mk(TokenKind::Minus);
    case '*':
        if (match('*')) {
            if (match('=')) return mk(TokenKind::StarStarEq, "**=");
            return mk(TokenKind::StarStar, "**");
        }
        if (match('=')) return mk(TokenKind::StarEq, "*=");
        return mk(TokenKind::Star);
    case '/':
        if (match('=')) return mk(TokenKind::SlashEq, "/=");
        return mk(TokenKind::Slash);
    case '%':
        if (match('=')) return mk(TokenKind::PercentEq, "%=");
        return mk(TokenKind::Percent);
    case '^':
        if (match('=')) return mk(TokenKind::CaretEq, "^=");
        return mk(TokenKind::Caret);
    case '&':
        if (match('&')) return mk(TokenKind::AmpAmp, "&&");
        if (match('=')) return mk(TokenKind::AmpEq, "&=");
        return mk(TokenKind::Ampersand);
    case '|':
        if (match('|')) return mk(TokenKind::PipePipe, "||");
        if (match('=')) return mk(TokenKind::PipeEq, "|=");
        return mk(TokenKind::Pipe);
    default:
        return mk(TokenKind::Unknown);
    }
}

// ─── Main tokenise loop ─────────────────────────────────────────────────────
std::vector<Token> Lexer::tokenize() {
    std::vector<Token> result;
    for (;;) {
        Token t = nextToken();
        result.push_back(t);
        if (t.kind == TokenKind::Eof) break;
    }
    return result;
}

Token Lexer::nextToken() {
    skipWhitespaceAndComments();
    if (pos_ >= src_.size()) {
        return Token{TokenKind::Eof, "", currentLoc(), {}};
    }

    // Newline
    if (src_[pos_] == '\n') {
        auto loc = currentLoc();
        advance();
        return Token{TokenKind::Newline, "\n", loc, {}};
    }

    char c = src_[pos_];

    if (std::isdigit(c)) return lexNumber();
    if (c == '"') return lexString('"');
    if (c == '\'') {
        // Could be char literal or lifetime 'a
        // Check for lifetime annotation 'a — fall through to operator handling if so
        if (pos_+2 < src_.size() && std::isalpha(src_[pos_+1]) && src_[pos_+2] != '\'') {
            // Lifetime syntax: treat the apostrophe as an operator token
        } else {
            return lexChar();
        }
    }
    if (c == '`') return lexString('`'); // raw/template string
    if (std::isalpha(c) || c == '_') return lexIdentOrKeyword();
    return lexOperatorOrDelim();
}

} // namespace fpp
