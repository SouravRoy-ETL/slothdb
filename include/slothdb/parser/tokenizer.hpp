#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace slothdb {

enum class TokenType : uint8_t {
    // Literals
    IDENTIFIER,
    INTEGER_LITERAL,
    FLOAT_LITERAL,
    STRING_LITERAL,

    // Operators
    PLUS,           // +
    MINUS,          // -
    STAR,           // *
    SLASH,          // /
    PERCENT,        // %
    EQUALS,         // =
    NOT_EQUALS,     // != or <>
    LESS_THAN,      // <
    GREATER_THAN,   // >
    LESS_EQUALS,    // <=
    GREATER_EQUALS, // >=
    PIPE,           // ||  (string concat)

    // Punctuation
    LPAREN,         // (
    RPAREN,         // )
    COMMA,          // ,
    SEMICOLON,      // ;
    DOT,            // .

    // Keywords
    KW_SELECT,
    KW_FROM,
    KW_WHERE,
    KW_AND,
    KW_OR,
    KW_NOT,
    KW_AS,
    KW_ORDER,
    KW_BY,
    KW_ASC,
    KW_DESC,
    KW_LIMIT,
    KW_OFFSET,
    KW_GROUP,
    KW_HAVING,
    KW_DISTINCT,
    KW_ALL,
    KW_JOIN,
    KW_INNER,
    KW_LEFT,
    KW_RIGHT,
    KW_FULL,
    KW_OUTER,
    KW_CROSS,
    KW_ON,
    KW_USING,
    KW_CREATE,
    KW_TABLE,
    KW_DROP,
    KW_INSERT,
    KW_INTO,
    KW_VALUES,
    KW_UPDATE,
    KW_SET,
    KW_DELETE,
    KW_ALTER,
    KW_ADD,
    KW_COLUMN,
    KW_IF,
    KW_EXISTS,
    KW_NULL,
    KW_IS,
    KW_IN,
    KW_BETWEEN,
    KW_LIKE,
    KW_CASE,
    KW_WHEN,
    KW_THEN,
    KW_ELSE,
    KW_END,
    KW_CAST,
    KW_TRUE,
    KW_FALSE,
    KW_INTEGER,
    KW_INT,
    KW_BIGINT,
    KW_SMALLINT,
    KW_TINYINT,
    KW_FLOAT,
    KW_DOUBLE,
    KW_REAL,
    KW_BOOLEAN,
    KW_BOOL,
    KW_VARCHAR,
    KW_TEXT,
    KW_CHAR,
    KW_BLOB,
    KW_DATE,
    KW_TIME,
    KW_TIMESTAMP,
    KW_DECIMAL,
    KW_NUMERIC,
    KW_HUGEINT,
    KW_PRIMARY,
    KW_KEY,
    KW_UNIQUE,
    KW_DEFAULT,
    KW_CHECK,
    KW_REFERENCES,
    KW_FOREIGN,
    KW_CONSTRAINT,
    KW_INDEX,
    KW_WITH,
    KW_RECURSIVE,
    KW_UNION,
    KW_EXCEPT,
    KW_INTERSECT,
    KW_EXPLAIN,
    KW_DESCRIBE,
    KW_PRAGMA,
    KW_LIVE,
    KW_COPY,
    KW_TO,
    KW_NULLS,
    KW_FIRST,
    KW_LAST,
    KW_OVER,
    KW_PARTITION,
    KW_ROWS,
    KW_RANGE,
    KW_UNBOUNDED,
    KW_PRECEDING,
    KW_FOLLOWING,
    KW_CURRENT,
    KW_ROW,
    KW_COUNT,
    KW_SUM,
    KW_AVG,
    KW_MIN,
    KW_MAX,
    KW_QUALIFY,
    KW_NOW,
    KW_CURRENT_TIMESTAMP,
    KW_CURRENT_DATE,
    KW_EXTRACT,
    KW_YEAR,
    KW_MONTH,
    KW_DAY,
    KW_HOUR,
    KW_MINUTE,
    KW_SECOND,
    KW_EPOCH,
    KW_DOW,
    KW_TRUNCATE,
    KW_VIEW,
    KW_ILIKE,
    KW_DISTINCT_FROM,
    KW_NATURAL,
    KW_GENERATE_SERIES,
    KW_PIVOT,
    KW_UNPIVOT,
    KW_MERGE,
    KW_MATCHED,
    KW_RETURNING,
    KW_CONFLICT,
    KW_DO,
    KW_NOTHING,
    KW_FILTER,
    KW_SAMPLE,
    KW_BEGIN,
    KW_COMMIT,
    KW_ROLLBACK,
    KW_TRANSACTION,

    // Special
    END_OF_FILE,
    INVALID
};

struct Token {
    TokenType type;
    std::string value;      // Raw text of the token.
    uint32_t line;
    uint32_t column;

    Token() : type(TokenType::INVALID), line(0), column(0) {}
    Token(TokenType type, const std::string &value, uint32_t line, uint32_t column)
        : type(type), value(value), line(line), column(column) {}
};

const char *TokenTypeToString(TokenType type);

// SQL Tokenizer. Converts a SQL string into a sequence of tokens.
class Tokenizer {
public:
    explicit Tokenizer(const std::string &sql);

    std::vector<Token> Tokenize();

private:
    char Peek() const;
    char PeekNext() const;
    char Advance();
    bool IsAtEnd() const;
    void SkipWhitespace();
    void SkipLineComment();
    void SkipBlockComment();

    Token ReadIdentifierOrKeyword();
    Token ReadNumber();
    Token ReadString();
    Token MakeToken(TokenType type, const std::string &value);

    static TokenType LookupKeyword(const std::string &word);

    std::string sql_;
    size_t pos_;
    uint32_t line_;
    uint32_t column_;
};

} // namespace slothdb
