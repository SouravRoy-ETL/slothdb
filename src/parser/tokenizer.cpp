#include "slothdb/parser/tokenizer.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/string_util.hpp"
#include <unordered_map>

namespace slothdb {

static const std::unordered_map<std::string, TokenType> KEYWORDS = {
    {"SELECT", TokenType::KW_SELECT}, {"FROM", TokenType::KW_FROM},
    {"WHERE", TokenType::KW_WHERE}, {"AND", TokenType::KW_AND},
    {"OR", TokenType::KW_OR}, {"NOT", TokenType::KW_NOT},
    {"AS", TokenType::KW_AS}, {"ORDER", TokenType::KW_ORDER},
    {"BY", TokenType::KW_BY}, {"ASC", TokenType::KW_ASC},
    {"DESC", TokenType::KW_DESC}, {"LIMIT", TokenType::KW_LIMIT},
    {"OFFSET", TokenType::KW_OFFSET}, {"GROUP", TokenType::KW_GROUP},
    {"HAVING", TokenType::KW_HAVING}, {"DISTINCT", TokenType::KW_DISTINCT},
    {"ALL", TokenType::KW_ALL}, {"JOIN", TokenType::KW_JOIN},
    {"INNER", TokenType::KW_INNER}, {"LEFT", TokenType::KW_LEFT},
    {"RIGHT", TokenType::KW_RIGHT}, {"FULL", TokenType::KW_FULL},
    {"OUTER", TokenType::KW_OUTER}, {"CROSS", TokenType::KW_CROSS},
    {"ON", TokenType::KW_ON}, {"USING", TokenType::KW_USING},
    {"CREATE", TokenType::KW_CREATE}, {"TABLE", TokenType::KW_TABLE},
    {"DROP", TokenType::KW_DROP}, {"INSERT", TokenType::KW_INSERT},
    {"INTO", TokenType::KW_INTO}, {"VALUES", TokenType::KW_VALUES},
    {"UPDATE", TokenType::KW_UPDATE}, {"SET", TokenType::KW_SET},
    {"DELETE", TokenType::KW_DELETE}, {"ALTER", TokenType::KW_ALTER},
    {"ADD", TokenType::KW_ADD}, {"COLUMN", TokenType::KW_COLUMN},
    {"IF", TokenType::KW_IF}, {"EXISTS", TokenType::KW_EXISTS},
    {"NULL", TokenType::KW_NULL}, {"IS", TokenType::KW_IS},
    {"IN", TokenType::KW_IN}, {"BETWEEN", TokenType::KW_BETWEEN},
    {"LIKE", TokenType::KW_LIKE}, {"CASE", TokenType::KW_CASE},
    {"WHEN", TokenType::KW_WHEN}, {"THEN", TokenType::KW_THEN},
    {"ELSE", TokenType::KW_ELSE}, {"END", TokenType::KW_END},
    {"CAST", TokenType::KW_CAST}, {"TRUE", TokenType::KW_TRUE},
    {"FALSE", TokenType::KW_FALSE},
    {"INTEGER", TokenType::KW_INTEGER}, {"INT", TokenType::KW_INT},
    {"BIGINT", TokenType::KW_BIGINT}, {"SMALLINT", TokenType::KW_SMALLINT},
    {"TINYINT", TokenType::KW_TINYINT}, {"FLOAT", TokenType::KW_FLOAT},
    {"DOUBLE", TokenType::KW_DOUBLE}, {"REAL", TokenType::KW_REAL},
    {"BOOLEAN", TokenType::KW_BOOLEAN}, {"BOOL", TokenType::KW_BOOL},
    {"VARCHAR", TokenType::KW_VARCHAR}, {"TEXT", TokenType::KW_TEXT},
    {"CHAR", TokenType::KW_CHAR}, {"BLOB", TokenType::KW_BLOB},
    {"DATE", TokenType::KW_DATE}, {"TIME", TokenType::KW_TIME},
    {"TIMESTAMP", TokenType::KW_TIMESTAMP},
    {"DECIMAL", TokenType::KW_DECIMAL}, {"NUMERIC", TokenType::KW_NUMERIC},
    {"HUGEINT", TokenType::KW_HUGEINT},
    {"PRIMARY", TokenType::KW_PRIMARY}, {"KEY", TokenType::KW_KEY},
    {"UNIQUE", TokenType::KW_UNIQUE}, {"DEFAULT", TokenType::KW_DEFAULT},
    {"CHECK", TokenType::KW_CHECK}, {"REFERENCES", TokenType::KW_REFERENCES},
    {"FOREIGN", TokenType::KW_FOREIGN}, {"CONSTRAINT", TokenType::KW_CONSTRAINT},
    {"INDEX", TokenType::KW_INDEX},
    {"WITH", TokenType::KW_WITH}, {"RECURSIVE", TokenType::KW_RECURSIVE},
    {"UNION", TokenType::KW_UNION}, {"EXCEPT", TokenType::KW_EXCEPT},
    {"INTERSECT", TokenType::KW_INTERSECT},
    {"EXPLAIN", TokenType::KW_EXPLAIN}, {"DESCRIBE", TokenType::KW_DESCRIBE},
    {"PRAGMA", TokenType::KW_PRAGMA},
    {"COPY", TokenType::KW_COPY},
    {"TO", TokenType::KW_TO},
    {"NULLS", TokenType::KW_NULLS}, {"FIRST", TokenType::KW_FIRST},
    {"LAST", TokenType::KW_LAST},
    {"OVER", TokenType::KW_OVER}, {"PARTITION", TokenType::KW_PARTITION},
    {"ROWS", TokenType::KW_ROWS}, {"RANGE", TokenType::KW_RANGE},
    {"UNBOUNDED", TokenType::KW_UNBOUNDED},
    {"PRECEDING", TokenType::KW_PRECEDING}, {"FOLLOWING", TokenType::KW_FOLLOWING},
    {"CURRENT", TokenType::KW_CURRENT}, {"ROW", TokenType::KW_ROW},
    {"COUNT", TokenType::KW_COUNT}, {"SUM", TokenType::KW_SUM},
    {"AVG", TokenType::KW_AVG}, {"MIN", TokenType::KW_MIN},
    {"MAX", TokenType::KW_MAX},
    {"QUALIFY", TokenType::KW_QUALIFY},
    {"NOW", TokenType::KW_NOW},
    {"CURRENT_TIMESTAMP", TokenType::KW_CURRENT_TIMESTAMP},
    {"CURRENT_DATE", TokenType::KW_CURRENT_DATE},
    {"EXTRACT", TokenType::KW_EXTRACT},
    {"YEAR", TokenType::KW_YEAR},
    {"MONTH", TokenType::KW_MONTH},
    {"DAY", TokenType::KW_DAY},
    {"HOUR", TokenType::KW_HOUR},
    {"MINUTE", TokenType::KW_MINUTE},
    {"SECOND", TokenType::KW_SECOND},
    {"EPOCH", TokenType::KW_EPOCH},
    {"DOW", TokenType::KW_DOW},
    {"TRUNCATE", TokenType::KW_TRUNCATE},
    {"VIEW", TokenType::KW_VIEW},
    {"ILIKE", TokenType::KW_ILIKE},
    {"NATURAL", TokenType::KW_NATURAL},
    {"GENERATE_SERIES", TokenType::KW_GENERATE_SERIES},
    {"PIVOT", TokenType::KW_PIVOT},
    {"UNPIVOT", TokenType::KW_UNPIVOT},
    {"MERGE", TokenType::KW_MERGE},
    {"MATCHED", TokenType::KW_MATCHED},
    {"RETURNING", TokenType::KW_RETURNING},
    {"CONFLICT", TokenType::KW_CONFLICT},
    {"DO", TokenType::KW_DO},
    {"NOTHING", TokenType::KW_NOTHING},
    {"FILTER", TokenType::KW_FILTER},
    {"SAMPLE", TokenType::KW_SAMPLE},
    {"BEGIN", TokenType::KW_BEGIN},
    {"COMMIT", TokenType::KW_COMMIT},
    {"ROLLBACK", TokenType::KW_ROLLBACK},
    {"TRANSACTION", TokenType::KW_TRANSACTION},
};

Tokenizer::Tokenizer(const std::string &sql)
    : sql_(sql), pos_(0), line_(1), column_(1) {}

char Tokenizer::Peek() const {
    if (IsAtEnd()) return '\0';
    return sql_[pos_];
}

char Tokenizer::PeekNext() const {
    if (pos_ + 1 >= sql_.size()) return '\0';
    return sql_[pos_ + 1];
}

char Tokenizer::Advance() {
    char c = sql_[pos_++];
    if (c == '\n') { line_++; column_ = 1; }
    else { column_++; }
    return c;
}

bool Tokenizer::IsAtEnd() const {
    return pos_ >= sql_.size();
}

void Tokenizer::SkipWhitespace() {
    while (!IsAtEnd()) {
        char c = Peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            Advance();
        } else if (c == '-' && PeekNext() == '-') {
            SkipLineComment();
        } else if (c == '/' && PeekNext() == '*') {
            SkipBlockComment();
        } else {
            break;
        }
    }
}

void Tokenizer::SkipLineComment() {
    while (!IsAtEnd() && Peek() != '\n') Advance();
}

void Tokenizer::SkipBlockComment() {
    Advance(); Advance(); // skip /*
    while (!IsAtEnd()) {
        if (Peek() == '*' && PeekNext() == '/') {
            Advance(); Advance();
            return;
        }
        Advance();
    }
    auto ex = ParserException(ErrorCode::SYNTAX_ERROR, "Unterminated block comment");
    ex.SetQueryLocation(line_, column_);
    throw ex;
}

Token Tokenizer::MakeToken(TokenType type, const std::string &value) {
    return Token(type, value, line_, column_);
}

// An identifier "continue" byte: ASCII alnum / underscore, OR any high-bit byte
// (>= 0x80). Accepting high-bit bytes lets UTF-8 characters (Chinese, Japanese,
// Korean, etc.) appear in unquoted identifiers, so `SELECT 区域 FROM sales` works.
static inline bool IsIdentContinue(char c) {
    auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || u == '_' || u >= 0x80;
}

Token Tokenizer::ReadIdentifierOrKeyword() {
    uint32_t start_col = column_;
    size_t start = pos_;
    while (!IsAtEnd() && IsIdentContinue(Peek())) {
        Advance();
    }
    std::string word = sql_.substr(start, pos_ - start);
    auto type = LookupKeyword(StringUtil::Upper(word));
    return Token(type, word, line_, start_col);
}

Token Tokenizer::ReadNumber() {
    uint32_t start_col = column_;
    size_t start = pos_;
    bool is_float = false;
    while (!IsAtEnd() && std::isdigit(Peek())) Advance();
    if (!IsAtEnd() && Peek() == '.' && std::isdigit(PeekNext())) {
        is_float = true;
        Advance(); // consume '.'
        while (!IsAtEnd() && std::isdigit(Peek())) Advance();
    }
    // Scientific notation.
    if (!IsAtEnd() && (Peek() == 'e' || Peek() == 'E')) {
        is_float = true;
        Advance();
        if (!IsAtEnd() && (Peek() == '+' || Peek() == '-')) Advance();
        if (IsAtEnd() || !std::isdigit(Peek())) {
            auto ex = ParserException(ErrorCode::INVALID_NUMERIC_LITERAL,
                                       "Invalid numeric literal");
            ex.SetQueryLocation(line_, start_col);
            throw ex;
        }
        while (!IsAtEnd() && std::isdigit(Peek())) Advance();
    }
    std::string num = sql_.substr(start, pos_ - start);
    return Token(is_float ? TokenType::FLOAT_LITERAL : TokenType::INTEGER_LITERAL,
                 num, line_, start_col);
}

Token Tokenizer::ReadString() {
    uint32_t start_col = column_;
    char quote = Advance(); // consume opening quote
    std::string result;
    while (!IsAtEnd()) {
        char c = Peek();
        if (c == quote) {
            Advance();
            // Double-quote escape: '' -> '
            if (!IsAtEnd() && Peek() == quote) {
                result += quote;
                Advance();
            } else {
                return Token(TokenType::STRING_LITERAL, result, line_, start_col);
            }
        } else {
            result += Advance();
        }
    }
    auto ex = ParserException(ErrorCode::UNTERMINATED_STRING, "Unterminated string literal");
    ex.SetQueryLocation(line_, start_col);
    throw ex;
}

TokenType Tokenizer::LookupKeyword(const std::string &word) {
    auto it = KEYWORDS.find(word);
    if (it != KEYWORDS.end()) return it->second;
    return TokenType::IDENTIFIER;
}

std::vector<Token> Tokenizer::Tokenize() {
    std::vector<Token> tokens;

    while (true) {
        SkipWhitespace();
        if (IsAtEnd()) {
            tokens.push_back(MakeToken(TokenType::END_OF_FILE, ""));
            break;
        }

        char c = Peek();

        // Identifiers and keywords. High-bit bytes (>= 0x80) start a UTF-8
        // multi-byte sequence — treat them as identifier starts so Chinese /
        // Japanese / Korean column names work unquoted.
        auto uc = static_cast<unsigned char>(c);
        if (std::isalpha(uc) || c == '_' || uc >= 0x80) {
            tokens.push_back(ReadIdentifierOrKeyword());
            continue;
        }

        // Numbers.
        if (std::isdigit(c)) {
            tokens.push_back(ReadNumber());
            continue;
        }

        // Strings.
        if (c == '\'') {
            tokens.push_back(ReadString());
            continue;
        }

        // Double-quoted identifiers.
        if (c == '"') {
            uint32_t start_col = column_;
            Advance(); // consume "
            std::string id;
            while (!IsAtEnd() && Peek() != '"') {
                id += Advance();
            }
            if (IsAtEnd()) {
                auto ex = ParserException(ErrorCode::UNTERMINATED_STRING,
                                           "Unterminated quoted identifier");
                ex.SetQueryLocation(line_, start_col);
                throw ex;
            }
            Advance(); // consume closing "
            tokens.push_back(Token(TokenType::IDENTIFIER, id, line_, start_col));
            continue;
        }

        // Operators and punctuation.
        uint32_t tok_col = column_;
        Advance();
        switch (c) {
        case '(': tokens.push_back(Token(TokenType::LPAREN, "(", line_, tok_col)); break;
        case ')': tokens.push_back(Token(TokenType::RPAREN, ")", line_, tok_col)); break;
        case ',': tokens.push_back(Token(TokenType::COMMA, ",", line_, tok_col)); break;
        case ';': tokens.push_back(Token(TokenType::SEMICOLON, ";", line_, tok_col)); break;
        case '.': tokens.push_back(Token(TokenType::DOT, ".", line_, tok_col)); break;
        case '+': tokens.push_back(Token(TokenType::PLUS, "+", line_, tok_col)); break;
        case '-': tokens.push_back(Token(TokenType::MINUS, "-", line_, tok_col)); break;
        case '*': tokens.push_back(Token(TokenType::STAR, "*", line_, tok_col)); break;
        case '/': tokens.push_back(Token(TokenType::SLASH, "/", line_, tok_col)); break;
        case '%': tokens.push_back(Token(TokenType::PERCENT, "%", line_, tok_col)); break;
        case '=': tokens.push_back(Token(TokenType::EQUALS, "=", line_, tok_col)); break;
        case '<':
            if (!IsAtEnd() && Peek() == '=') {
                Advance();
                tokens.push_back(Token(TokenType::LESS_EQUALS, "<=", line_, tok_col));
            } else if (!IsAtEnd() && Peek() == '>') {
                Advance();
                tokens.push_back(Token(TokenType::NOT_EQUALS, "<>", line_, tok_col));
            } else {
                tokens.push_back(Token(TokenType::LESS_THAN, "<", line_, tok_col));
            }
            break;
        case '>':
            if (!IsAtEnd() && Peek() == '=') {
                Advance();
                tokens.push_back(Token(TokenType::GREATER_EQUALS, ">=", line_, tok_col));
            } else {
                tokens.push_back(Token(TokenType::GREATER_THAN, ">", line_, tok_col));
            }
            break;
        case '!':
            if (!IsAtEnd() && Peek() == '=') {
                Advance();
                tokens.push_back(Token(TokenType::NOT_EQUALS, "!=", line_, tok_col));
            } else {
                auto ex = ParserException(ErrorCode::UNEXPECTED_TOKEN,
                                           "Unexpected character '!'");
                ex.SetQueryLocation(line_, tok_col);
                throw ex;
            }
            break;
        case '|':
            if (!IsAtEnd() && Peek() == '|') {
                Advance();
                tokens.push_back(Token(TokenType::PIPE, "||", line_, tok_col));
            } else {
                auto ex = ParserException(ErrorCode::UNEXPECTED_TOKEN,
                                           "Unexpected character '|'");
                ex.SetQueryLocation(line_, tok_col);
                throw ex;
            }
            break;
        default: {
            auto ex = ParserException(ErrorCode::UNEXPECTED_TOKEN,
                                       "Unexpected character '" + std::string(1, c) + "'");
            ex.SetQueryLocation(line_, tok_col);
            throw ex;
        }
        }
    }

    return tokens;
}

const char *TokenTypeToString(TokenType type) {
    switch (type) {
    case TokenType::IDENTIFIER: return "IDENTIFIER";
    case TokenType::INTEGER_LITERAL: return "INTEGER_LITERAL";
    case TokenType::FLOAT_LITERAL: return "FLOAT_LITERAL";
    case TokenType::STRING_LITERAL: return "STRING_LITERAL";
    case TokenType::PLUS: return "PLUS";
    case TokenType::MINUS: return "MINUS";
    case TokenType::STAR: return "STAR";
    case TokenType::SLASH: return "SLASH";
    case TokenType::PERCENT: return "PERCENT";
    case TokenType::EQUALS: return "EQUALS";
    case TokenType::NOT_EQUALS: return "NOT_EQUALS";
    case TokenType::LESS_THAN: return "LESS_THAN";
    case TokenType::GREATER_THAN: return "GREATER_THAN";
    case TokenType::LESS_EQUALS: return "LESS_EQUALS";
    case TokenType::GREATER_EQUALS: return "GREATER_EQUALS";
    case TokenType::PIPE: return "PIPE";
    case TokenType::LPAREN: return "LPAREN";
    case TokenType::RPAREN: return "RPAREN";
    case TokenType::COMMA: return "COMMA";
    case TokenType::SEMICOLON: return "SEMICOLON";
    case TokenType::DOT: return "DOT";
    case TokenType::END_OF_FILE: return "EOF";
    case TokenType::INVALID: return "INVALID";
    default: return "KEYWORD";
    }
}

} // namespace slothdb
