#include "slothdb/parser/parser.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/string_util.hpp"

namespace slothdb {

// ============================================================================
// Public API
// ============================================================================

std::vector<ParsedStmtPtr> Parser::Parse(const std::string &sql) {
    Tokenizer tokenizer(sql);
    auto tokens = tokenizer.Tokenize();
    Parser parser(std::move(tokens));

    std::vector<ParsedStmtPtr> statements;
    while (!parser.IsAtEnd()) {
        if (parser.Check(TokenType::SEMICOLON)) {
            parser.Advance();
            continue;
        }
        statements.push_back(parser.ParseStatement());
        // Consume optional trailing semicolon.
        parser.Match(TokenType::SEMICOLON);
    }
    return statements;
}

Parser::Parser(std::vector<Token> tokens)
    : tokens_(std::move(tokens)), pos_(0) {}

// ============================================================================
// Token helpers
// ============================================================================

const Token &Parser::Current() const { return tokens_[pos_]; }

const Token &Parser::Peek() const {
    if (pos_ + 1 < tokens_.size()) return tokens_[pos_ + 1];
    return tokens_.back(); // EOF
}

const Token &Parser::Advance() {
    auto &tok = tokens_[pos_];
    if (!IsAtEnd()) pos_++;
    return tok;
}

bool Parser::Check(TokenType type) const { return Current().type == type; }
bool Parser::CheckKeyword(TokenType kw) const { return Current().type == kw; }

bool Parser::Match(TokenType type) {
    if (Check(type)) { Advance(); return true; }
    return false;
}

bool Parser::MatchKeyword(TokenType kw) {
    if (CheckKeyword(kw)) { Advance(); return true; }
    return false;
}

const Token &Parser::Expect(TokenType type, const std::string &context) {
    if (!Check(type)) {
        ThrowError("Expected " + std::string(TokenTypeToString(type)) +
                   " " + context + ", got '" + Current().value + "'");
    }
    return Advance();
}

bool Parser::IsIdentifierOrNonReserved(TokenType t) {
    if (t == TokenType::IDENTIFIER) return true;
    // Reserved keywords — would be parse-ambiguous if allowed as identifiers.
    // Everything else that tokenizes as a keyword falls through to the default
    // branch and is treated as non-reserved (legal in alias / column / table
    // positions). Matches DuckDB's non-reserved set.
    switch (t) {
    // Statement verbs and clause starters.
    case TokenType::KW_SELECT:
    case TokenType::KW_FROM:
    case TokenType::KW_WHERE:
    case TokenType::KW_GROUP:
    case TokenType::KW_ORDER:
    case TokenType::KW_HAVING:
    case TokenType::KW_LIMIT:
    case TokenType::KW_OFFSET:
    case TokenType::KW_QUALIFY:
    case TokenType::KW_UNION:
    case TokenType::KW_INTERSECT:
    case TokenType::KW_EXCEPT:
    case TokenType::KW_WITH:
    case TokenType::KW_RECURSIVE:
    case TokenType::KW_INSERT:
    case TokenType::KW_INTO:
    case TokenType::KW_UPDATE:
    case TokenType::KW_DELETE:
    case TokenType::KW_SET:
    case TokenType::KW_VALUES:
    case TokenType::KW_CREATE:
    case TokenType::KW_DROP:
    case TokenType::KW_ALTER:
    case TokenType::KW_EXPLAIN:
    case TokenType::KW_COPY:
    case TokenType::KW_TABLE:
    case TokenType::KW_TO:
    // JOIN family.
    case TokenType::KW_JOIN:
    case TokenType::KW_INNER:
    case TokenType::KW_LEFT:
    case TokenType::KW_RIGHT:
    case TokenType::KW_FULL:
    case TokenType::KW_OUTER:
    case TokenType::KW_CROSS:
    case TokenType::KW_ON:
    case TokenType::KW_USING:
    case TokenType::KW_NATURAL:
    // Logical / boolean / comparison operators and modifiers.
    case TokenType::KW_AND:
    case TokenType::KW_OR:
    case TokenType::KW_NOT:
    case TokenType::KW_IS:
    case TokenType::KW_IN:
    case TokenType::KW_LIKE:
    case TokenType::KW_BETWEEN:
    case TokenType::KW_NULL:
    case TokenType::KW_TRUE:
    case TokenType::KW_FALSE:
    case TokenType::KW_DISTINCT:
    case TokenType::KW_DISTINCT_FROM:
    case TokenType::KW_ALL:
    case TokenType::KW_ASC:
    case TokenType::KW_DESC:
    case TokenType::KW_AS:
    case TokenType::KW_BY:
    // Expression control.
    case TokenType::KW_CASE:
    case TokenType::KW_WHEN:
    case TokenType::KW_THEN:
    case TokenType::KW_ELSE:
    case TokenType::KW_END:
    case TokenType::KW_CAST:
    case TokenType::KW_EXISTS:
    // Post-expression anchors (would swallow the next clause if mistaken for alias).
    case TokenType::KW_OVER:
    case TokenType::KW_PARTITION:
    case TokenType::KW_FILTER:
    // Ordering modifiers.
    case TokenType::KW_NULLS:
    case TokenType::KW_FIRST:
    case TokenType::KW_LAST:
    // DDL fragments.
    case TokenType::KW_COLUMN:
    case TokenType::KW_IF:
    case TokenType::KW_ADD:
        return false;
    default:
        // Any other keyword (time units, type names, constraint keywords,
        // window-frame words, aggregate function names, transaction verbs,
        // MERGE / PIVOT / RETURNING / CONFLICT, etc.) is non-reserved —
        // legal as an identifier in alias / column / table-alias position.
        // Guard against non-keyword token types (punctuation, operators, EOF).
        return (static_cast<int>(t) >= static_cast<int>(TokenType::KW_SELECT) &&
                static_cast<int>(t) <= static_cast<int>(TokenType::KW_TRANSACTION));
    }
}

const Token &Parser::ExpectIdentifier(const std::string &context) {
    if (!IsIdentifierOrNonReserved(Current().type)) {
        ThrowError("Expected identifier " + context +
                   ", got '" + Current().value + "'");
    }
    return Advance();
}

bool Parser::IsAtEnd() const { return Current().type == TokenType::END_OF_FILE; }

[[noreturn]] void Parser::ThrowError(const std::string &msg) {
    auto ex = ParserException(ErrorCode::SYNTAX_ERROR, msg);
    ex.SetQueryLocation(Current().line, Current().column);
    throw ex;
}

[[noreturn]] void Parser::ThrowUnexpected(const std::string &context) {
    ThrowError("Unexpected token '" + Current().value + "' " + context);
}

// ============================================================================
// Statement parsers
// ============================================================================

ParsedStmtPtr Parser::ParseStatement() {
    if (CheckKeyword(TokenType::KW_WITH)) return ParseSelectStatement(); // WITH starts a CTE
    if (CheckKeyword(TokenType::KW_SELECT)) return ParseSelectStatement();
    if (CheckKeyword(TokenType::KW_CREATE)) return ParseCreateStatement();
    if (CheckKeyword(TokenType::KW_DROP)) return ParseDropStatement();
    if (CheckKeyword(TokenType::KW_INSERT)) return ParseInsertStatement();
    if (CheckKeyword(TokenType::KW_UPDATE)) return ParseUpdateStatement();
    if (CheckKeyword(TokenType::KW_DELETE)) return ParseDeleteStatement();
    if (CheckKeyword(TokenType::KW_EXPLAIN)) return ParseExplainStatement();
    if (CheckKeyword(TokenType::KW_DESCRIBE)) return ParseDescribeStatement();
    if (CheckKeyword(TokenType::KW_PRAGMA)) return ParsePragmaStatement();
    if (CheckKeyword(TokenType::KW_MERGE)) return ParseMergeStatement();
    // COPY table TO/FROM 'file.csv' [WITH (options)]
    // COPY (SELECT ...) TO 'file' [WITH (options)]
    if (MatchKeyword(TokenType::KW_COPY)) {
        auto stmt = std::make_unique<CopyStatement>();
        if (Match(TokenType::LPAREN)) {
            // Subquery form.
            auto inner = ParseSelectStatement();
            stmt->source_query = std::unique_ptr<SelectStatement>(
                static_cast<SelectStatement *>(inner.release()));
            Expect(TokenType::RPAREN, "after COPY subquery");
        } else {
            stmt->table_name = ExpectIdentifier("for table name").value;
        }

        if (MatchKeyword(TokenType::KW_TO)) {
            stmt->is_from = false;
        } else if (MatchKeyword(TokenType::KW_FROM)) {
            stmt->is_from = true;
        } else {
            ThrowError("Expected TO or FROM after COPY table");
        }

        stmt->file_path = Expect(TokenType::STRING_LITERAL, "for file path").value;

        // Optional WITH (DELIMITER ',', HEADER true)
        if (MatchKeyword(TokenType::KW_WITH)) {
            Expect(TokenType::LPAREN, "after WITH");
            while (!Check(TokenType::RPAREN)) {
                auto opt_name = StringUtil::Upper(Advance().value);
                if (opt_name == "DELIMITER") {
                    auto delim = Expect(TokenType::STRING_LITERAL, "for delimiter").value;
                    if (!delim.empty()) stmt->delimiter = delim[0];
                } else if (opt_name == "HEADER") {
                    auto val = StringUtil::Upper(Advance().value);
                    stmt->header = (val == "TRUE" || val == "1");
                } else if (opt_name == "FORMAT") {
                    stmt->format = StringUtil::Upper(Advance().value);
                }
                Match(TokenType::COMMA);
            }
            Expect(TokenType::RPAREN, "after WITH options");
        }

        return stmt;
    }

    if (MatchKeyword(TokenType::KW_BEGIN)) {
        MatchKeyword(TokenType::KW_TRANSACTION); // optional
        return std::make_unique<BeginStatement>();
    }
    if (MatchKeyword(TokenType::KW_COMMIT)) {
        MatchKeyword(TokenType::KW_TRANSACTION);
        return std::make_unique<CommitStatement>();
    }
    if (MatchKeyword(TokenType::KW_ROLLBACK)) {
        MatchKeyword(TokenType::KW_TRANSACTION);
        return std::make_unique<RollbackStatement>();
    }
    if (CheckKeyword(TokenType::KW_ALTER)) {
        Advance(); // ALTER
        Expect(TokenType::KW_TABLE, "after ALTER");
        auto stmt = std::make_unique<AlterTableStatement>();
        stmt->table_name = ExpectIdentifier("for table name").value;

        if (MatchKeyword(TokenType::KW_ADD)) {
            MatchKeyword(TokenType::KW_COLUMN); // optional
            stmt->action = AlterTableStatement::AlterAction::ADD_COLUMN;
            stmt->column_name = ExpectIdentifier("for column name").value;
            stmt->column_type = ParseTypeName();
        } else if (MatchKeyword(TokenType::KW_DROP)) {
            MatchKeyword(TokenType::KW_COLUMN);
            stmt->action = AlterTableStatement::AlterAction::DROP_COLUMN;
            stmt->column_name = ExpectIdentifier("for column name").value;
        } else if (Check(TokenType::IDENTIFIER) && StringUtil::Upper(Current().value) == "RENAME") {
            Advance();
            MatchKeyword(TokenType::KW_COLUMN);
            stmt->action = AlterTableStatement::AlterAction::RENAME_COLUMN;
            stmt->column_name = ExpectIdentifier("for old column name").value;
            Expect(TokenType::KW_TO, "in RENAME");
            stmt->new_column_name = ExpectIdentifier("for new column name").value;
        } else {
            ThrowError("Expected ADD, DROP, or RENAME after ALTER TABLE name");
        }
        return stmt;
    }
    if (CheckKeyword(TokenType::KW_TRUNCATE)) {
        Advance(); // TRUNCATE
        MatchKeyword(TokenType::KW_TABLE); // optional TABLE
        auto stmt = std::make_unique<TruncateStatement>();
        stmt->table_name = ExpectIdentifier("for table name").value;
        return stmt;
    }
    ThrowUnexpected("at start of statement");
}

// SELECT [DISTINCT] select_list FROM table [WHERE ...] [GROUP BY ...] [HAVING ...]
// [ORDER BY ...] [LIMIT n] [OFFSET n]
ParsedStmtPtr Parser::ParseSelectStatement() {
    auto stmt = std::make_unique<SelectStatement>();

    // Parse optional WITH clause (CTEs).
    if (MatchKeyword(TokenType::KW_WITH)) {
        bool is_recursive = MatchKeyword(TokenType::KW_RECURSIVE);
        do {
            SelectStatement::CTE cte;
            cte.recursive = is_recursive;
            cte.name = ExpectIdentifier("for CTE name").value;
            // Optional column alias list: nums(n) or nums(a, b).
            if (Match(TokenType::LPAREN)) {
                // Skip column names — they're just aliases.
                do { Advance(); } while (Match(TokenType::COMMA));
                Expect(TokenType::RPAREN, "after CTE column list");
            }
            Expect(TokenType::KW_AS, "after CTE name");
            Expect(TokenType::LPAREN, "before CTE query");
            // Parse the inner SELECT.
            auto inner = ParseSelectStatement();
            cte.query = std::unique_ptr<SelectStatement>(
                static_cast<SelectStatement *>(inner.release()));
            Expect(TokenType::RPAREN, "after CTE query");
            stmt->ctes.push_back(std::move(cte));
        } while (Match(TokenType::COMMA));
    }

    Expect(TokenType::KW_SELECT, "");

    // DISTINCT
    if (MatchKeyword(TokenType::KW_DISTINCT)) {
        stmt->is_distinct = true;
    }

    // Select list.
    do {
        auto expr = ParseExpression();
        // Alias: expr AS name, or expr name
        if (MatchKeyword(TokenType::KW_AS)) {
            expr->alias = ExpectIdentifier("after AS").value;
        } else if (IsIdentifierOrNonReserved(Current().type) &&
                   !CheckKeyword(TokenType::KW_FROM) &&
                   !CheckKeyword(TokenType::KW_WHERE) &&
                   !CheckKeyword(TokenType::KW_GROUP) &&
                   !CheckKeyword(TokenType::KW_ORDER) &&
                   !CheckKeyword(TokenType::KW_LIMIT) &&
                   !CheckKeyword(TokenType::KW_HAVING)) {
            expr->alias = Advance().value;
        }
        stmt->select_list.push_back(std::move(expr));
    } while (Match(TokenType::COMMA));

    // FROM
    if (MatchKeyword(TokenType::KW_FROM)) {
        stmt->from_table = ParseTableRef();
    }

    // WHERE
    if (MatchKeyword(TokenType::KW_WHERE)) {
        stmt->where_clause = ParseExpression();
    }

    // GROUP BY
    if (MatchKeyword(TokenType::KW_GROUP)) {
        Expect(TokenType::KW_BY, "after GROUP");
        do {
            stmt->group_by.push_back(ParseExpression());
        } while (Match(TokenType::COMMA));
    }

    // HAVING
    if (MatchKeyword(TokenType::KW_HAVING)) {
        stmt->having_clause = ParseExpression();
    }

    // QUALIFY (Snowflake-style: filter on window function results)
    if (MatchKeyword(TokenType::KW_QUALIFY)) {
        stmt->qualify_clause = ParseExpression();
    }

    // ORDER BY
    if (MatchKeyword(TokenType::KW_ORDER)) {
        Expect(TokenType::KW_BY, "after ORDER");
        do {
            OrderByItem item;
            item.expression = ParseExpression();
            if (MatchKeyword(TokenType::KW_DESC)) {
                item.ascending = false;
            } else {
                MatchKeyword(TokenType::KW_ASC); // optional
            }
            // NULLS FIRST/LAST
            if (MatchKeyword(TokenType::KW_NULLS)) {
                if (MatchKeyword(TokenType::KW_FIRST)) {
                    item.nulls_first = true;
                } else if (MatchKeyword(TokenType::KW_LAST)) {
                    item.nulls_first = false;
                }
            }
            stmt->order_by.push_back(std::move(item));
        } while (Match(TokenType::COMMA));
    }

    // LIMIT
    if (MatchKeyword(TokenType::KW_LIMIT)) {
        stmt->limit = ParseExpression();
    }

    // OFFSET
    if (MatchKeyword(TokenType::KW_OFFSET)) {
        stmt->offset = ParseExpression();
    }

    // Set operations: UNION [ALL], INTERSECT, EXCEPT.
    if (CheckKeyword(TokenType::KW_UNION) || CheckKeyword(TokenType::KW_INTERSECT) ||
        CheckKeyword(TokenType::KW_EXCEPT)) {
        if (MatchKeyword(TokenType::KW_UNION)) {
            if (MatchKeyword(TokenType::KW_ALL)) {
                stmt->set_op = "UNION ALL";
            } else {
                stmt->set_op = "UNION";
            }
        } else if (MatchKeyword(TokenType::KW_INTERSECT)) {
            stmt->set_op = "INTERSECT";
        } else if (MatchKeyword(TokenType::KW_EXCEPT)) {
            stmt->set_op = "EXCEPT";
        }
        auto right = ParseSelectStatement();
        stmt->set_right = std::unique_ptr<SelectStatement>(
            static_cast<SelectStatement *>(right.release()));
    }

    return stmt;
}

// INSERT INTO name [(cols)] VALUES (vals), (vals), ...
ParsedStmtPtr Parser::ParseInsertStatement() {
    auto stmt = std::make_unique<InsertStatement>();
    Expect(TokenType::KW_INSERT, "");
    Expect(TokenType::KW_INTO, "after INSERT");

    stmt->table_name = ExpectIdentifier("for table name").value;

    // Optional column list.
    if (Match(TokenType::LPAREN)) {
        do {
            stmt->column_names.push_back(
                ExpectIdentifier("for column name").value);
        } while (Match(TokenType::COMMA));
        Expect(TokenType::RPAREN, "after column list");
    }

    // INSERT INTO ... SELECT ...
    if (CheckKeyword(TokenType::KW_SELECT) || CheckKeyword(TokenType::KW_WITH)) {
        auto select = ParseSelectStatement();
        stmt->select_source = std::unique_ptr<SelectStatement>(
            static_cast<SelectStatement *>(select.release()));
        return stmt;
    }

    Expect(TokenType::KW_VALUES, "after table name");

    // Value rows.
    do {
        Expect(TokenType::LPAREN, "for VALUES row");
        std::vector<ParsedExprPtr> row;
        do {
            row.push_back(ParseExpression());
        } while (Match(TokenType::COMMA));
        Expect(TokenType::RPAREN, "after VALUES row");
        stmt->values.push_back(std::move(row));
    } while (Match(TokenType::COMMA));

    return stmt;
}

// ============================================================================
// FROM clause
// ============================================================================

std::unique_ptr<TableRef> Parser::ParseTableRef() {
    auto ref = std::make_unique<TableRef>();

    // Handle table functions: read_csv(), read_json(), GENERATE_SERIES().
    if (Check(TokenType::IDENTIFIER) &&
        (StringUtil::Upper(Current().value) == "READ_CSV" ||
         StringUtil::Upper(Current().value) == "READ_CSV_AUTO" ||
         StringUtil::Upper(Current().value) == "READ_JSON" ||
         StringUtil::Upper(Current().value) == "READ_JSON_AUTO" ||
         StringUtil::Upper(Current().value) == "READ_PARQUET" ||
         StringUtil::Upper(Current().value) == "READ_ARROW" ||
         StringUtil::Upper(Current().value) == "READ_AVRO" ||
         StringUtil::Upper(Current().value) == "READ_XLSX" ||
         StringUtil::Upper(Current().value) == "SQLITE_SCAN")) {
        ref->table_name = StringUtil::Upper(Advance().value);
        ref->is_table_function = true;
        Expect(TokenType::LPAREN, "after table function");
        do {
            ref->function_args.push_back(ParseExpression());
        } while (Match(TokenType::COMMA));
        Expect(TokenType::RPAREN, "after table function args");

        if (MatchKeyword(TokenType::KW_AS)) {
            ref->alias = ExpectIdentifier("for alias").value;
        } else if (Check(TokenType::IDENTIFIER) &&
                   !CheckKeyword(TokenType::KW_WHERE) &&
                   !CheckKeyword(TokenType::KW_GROUP) &&
                   !CheckKeyword(TokenType::KW_ORDER) &&
                   !CheckKeyword(TokenType::KW_LIMIT) &&
                   !CheckKeyword(TokenType::KW_JOIN) &&
                   !CheckKeyword(TokenType::KW_INNER) &&
                   !CheckKeyword(TokenType::KW_LEFT) &&
                   !CheckKeyword(TokenType::KW_RIGHT) &&
                   !CheckKeyword(TokenType::KW_FULL) &&
                   !CheckKeyword(TokenType::KW_CROSS) &&
                   !CheckKeyword(TokenType::KW_ON) &&
                   !CheckKeyword(TokenType::KW_HAVING) &&
                   !CheckKeyword(TokenType::KW_QUALIFY)) {
            ref->alias = Advance().value;
        }
    } else if (Check(TokenType::STRING_LITERAL)) {
        // Handle string literal in FROM: SELECT * FROM 'file.csv' (auto-detect format).
        auto file_path = Advance().value;
        ref->table_name = "__FILE__";
        ref->is_table_function = true;
        ref->function_args.push_back(
            std::make_unique<ConstantExpression>(file_path, TokenType::STRING_LITERAL));

        if (MatchKeyword(TokenType::KW_AS)) {
            ref->alias = ExpectIdentifier("for alias").value;
        } else if (Check(TokenType::IDENTIFIER) &&
                   !CheckKeyword(TokenType::KW_WHERE) &&
                   !CheckKeyword(TokenType::KW_GROUP) &&
                   !CheckKeyword(TokenType::KW_ORDER) &&
                   !CheckKeyword(TokenType::KW_LIMIT) &&
                   !CheckKeyword(TokenType::KW_JOIN) &&
                   !CheckKeyword(TokenType::KW_INNER) &&
                   !CheckKeyword(TokenType::KW_LEFT) &&
                   !CheckKeyword(TokenType::KW_RIGHT) &&
                   !CheckKeyword(TokenType::KW_FULL) &&
                   !CheckKeyword(TokenType::KW_CROSS) &&
                   !CheckKeyword(TokenType::KW_ON)) {
            ref->alias = Advance().value;
        }
        // Fall through to JOIN handling.
    } else if (CheckKeyword(TokenType::KW_GENERATE_SERIES)) {
        // Handle GENERATE_SERIES(start, stop[, step]) as a table function.
        Advance();
        ref->table_name = "GENERATE_SERIES";
        ref->is_table_function = true;
        Expect(TokenType::LPAREN, "after GENERATE_SERIES");
        do {
            ref->function_args.push_back(ParseExpression());
        } while (Match(TokenType::COMMA));
        Expect(TokenType::RPAREN, "after GENERATE_SERIES args");

        if (MatchKeyword(TokenType::KW_AS)) {
            ref->alias = ExpectIdentifier("for alias").value;
        } else if (Check(TokenType::IDENTIFIER)) {
            ref->alias = Advance().value;
        }
        // Fall through to JOIN handling.
    } else {
        ref->table_name = ExpectIdentifier("for table name").value;

    // Optional alias.
    if (MatchKeyword(TokenType::KW_AS)) {
        ref->alias = ExpectIdentifier("for table alias").value;
    } else if (Check(TokenType::IDENTIFIER) &&
               !CheckKeyword(TokenType::KW_WHERE) &&
               !CheckKeyword(TokenType::KW_GROUP) &&
               !CheckKeyword(TokenType::KW_ORDER) &&
               !CheckKeyword(TokenType::KW_LIMIT) &&
               !CheckKeyword(TokenType::KW_JOIN) &&
               !CheckKeyword(TokenType::KW_INNER) &&
               !CheckKeyword(TokenType::KW_LEFT) &&
               !CheckKeyword(TokenType::KW_RIGHT) &&
               !CheckKeyword(TokenType::KW_FULL) &&
               !CheckKeyword(TokenType::KW_CROSS) &&
               !CheckKeyword(TokenType::KW_ON) &&
               !CheckKeyword(TokenType::KW_HAVING)) {
        ref->alias = Advance().value;
    }
    } // close else block from regular table name path

    // JOINs.
    while (true) {
        std::string join_type;
        if (MatchKeyword(TokenType::KW_INNER)) {
            join_type = "INNER";
        } else if (MatchKeyword(TokenType::KW_LEFT)) {
            MatchKeyword(TokenType::KW_OUTER);
            join_type = "LEFT";
        } else if (MatchKeyword(TokenType::KW_RIGHT)) {
            MatchKeyword(TokenType::KW_OUTER);
            join_type = "RIGHT";
        } else if (MatchKeyword(TokenType::KW_FULL)) {
            MatchKeyword(TokenType::KW_OUTER);
            join_type = "FULL";
        } else if (MatchKeyword(TokenType::KW_CROSS)) {
            join_type = "CROSS";
        } else if (MatchKeyword(TokenType::KW_NATURAL)) {
            join_type = "NATURAL";
        } else if (CheckKeyword(TokenType::KW_JOIN)) {
            join_type = "INNER"; // plain JOIN = INNER JOIN
        } else {
            break;
        }

        Expect(TokenType::KW_JOIN, "after join type");

        // Parse right side as a full table ref (supports read_csv, etc).
        auto right_table = ParseTableRef();

        // ON condition (not for CROSS JOIN).
        ParsedExprPtr on_cond;
        if (join_type != "CROSS" && MatchKeyword(TokenType::KW_ON)) {
            on_cond = ParseExpression();
        }

        // Attach JOIN to the existing ref (preserve is_table_function, function_args, etc).
        ref->join_type = join_type;
        ref->right = std::move(right_table);
        ref->on_condition = std::move(on_cond);
    }

    return ref;
}

// ============================================================================
// Type name parser
// ============================================================================

std::string Parser::ParseTypeName() {
    std::string type_name;

    // Accept keywords that are type names.
    auto &tok = Current();
    switch (tok.type) {
    case TokenType::KW_INTEGER: case TokenType::KW_INT:
    case TokenType::KW_BIGINT: case TokenType::KW_SMALLINT:
    case TokenType::KW_TINYINT: case TokenType::KW_HUGEINT:
    case TokenType::KW_FLOAT: case TokenType::KW_DOUBLE:
    case TokenType::KW_REAL: case TokenType::KW_BOOLEAN:
    case TokenType::KW_BOOL: case TokenType::KW_VARCHAR:
    case TokenType::KW_TEXT: case TokenType::KW_CHAR:
    case TokenType::KW_BLOB: case TokenType::KW_DATE:
    case TokenType::KW_TIME: case TokenType::KW_TIMESTAMP:
    case TokenType::KW_DECIMAL: case TokenType::KW_NUMERIC:
    case TokenType::IDENTIFIER:
        type_name = StringUtil::Upper(Advance().value);
        break;
    default:
        ThrowError("Expected type name, got '" + tok.value + "'");
    }

    // Handle parameterized types: DECIMAL(10,2), VARCHAR(255).
    if (Match(TokenType::LPAREN)) {
        type_name += "(";
        type_name += Expect(TokenType::INTEGER_LITERAL, "for type parameter").value;
        if (Match(TokenType::COMMA)) {
            type_name += ",";
            type_name += Expect(TokenType::INTEGER_LITERAL, "for type parameter").value;
        }
        Expect(TokenType::RPAREN, "after type parameters");
        type_name += ")";
    }

    return type_name;
}

// ============================================================================
// Expression parser (precedence climbing)
// ============================================================================

ParsedExprPtr Parser::ParseExpression() {
    if (++expr_depth_ > MAX_EXPR_DEPTH)
        ThrowError("Expression nesting too deep (limit: 256)");
    auto result = ParseOr();
    expr_depth_--;
    return result;
}

ParsedExprPtr Parser::ParseOr() {
    auto left = ParseAnd();
    while (MatchKeyword(TokenType::KW_OR)) {
        auto right = ParseAnd();
        left = std::make_unique<ConjunctionExpression>("OR", std::move(left), std::move(right));
    }
    return left;
}

ParsedExprPtr Parser::ParseAnd() {
    auto left = ParseNot();
    while (MatchKeyword(TokenType::KW_AND)) {
        auto right = ParseNot();
        left = std::make_unique<ConjunctionExpression>("AND", std::move(left), std::move(right));
    }
    return left;
}

ParsedExprPtr Parser::ParseNot() {
    if (MatchKeyword(TokenType::KW_NOT)) {
        auto child = ParseNot();
        return std::make_unique<NegationExpression>(std::move(child));
    }
    return ParseComparison();
}

ParsedExprPtr Parser::ParseComparison() {
    auto left = ParseAddSub();

    // IS [NOT] NULL
    if (MatchKeyword(TokenType::KW_IS)) {
        bool is_not = MatchKeyword(TokenType::KW_NOT);
        Expect(TokenType::KW_NULL, "after IS [NOT]");
        return std::make_unique<IsNullExpression>(std::move(left), is_not);
    }

    // Comparison operators.
    if (Check(TokenType::EQUALS) || Check(TokenType::NOT_EQUALS) ||
        Check(TokenType::LESS_THAN) || Check(TokenType::GREATER_THAN) ||
        Check(TokenType::LESS_EQUALS) || Check(TokenType::GREATER_EQUALS)) {
        auto op = Advance().value;
        auto right = ParseAddSub();
        return std::make_unique<ComparisonExpression>(op, std::move(left), std::move(right));
    }

    // LIKE / ILIKE
    if (MatchKeyword(TokenType::KW_LIKE)) {
        auto right = ParseAddSub();
        return std::make_unique<ComparisonExpression>("LIKE", std::move(left), std::move(right));
    }
    if (MatchKeyword(TokenType::KW_ILIKE)) {
        auto right = ParseAddSub();
        return std::make_unique<ComparisonExpression>("ILIKE", std::move(left), std::move(right));
    }

    // BETWEEN x AND y  ->  x >= low AND x <= high
    if (MatchKeyword(TokenType::KW_BETWEEN)) {
        auto low = ParseAddSub();
        Expect(TokenType::KW_AND, "in BETWEEN");
        auto high = ParseAddSub();
        // Desugar: left >= low AND left <= high
        // We need to duplicate 'left'. Since we can't clone, use a function node.
        std::vector<ParsedExprPtr> args;
        args.push_back(std::move(left));
        args.push_back(std::move(low));
        args.push_back(std::move(high));
        return std::make_unique<FunctionExpression>("BETWEEN", std::move(args));
    }

    // NOT BETWEEN
    if (CheckKeyword(TokenType::KW_NOT) && pos_ + 1 < tokens_.size() &&
        tokens_[pos_ + 1].type == TokenType::KW_BETWEEN) {
        Advance(); // NOT
        Advance(); // BETWEEN
        auto low = ParseAddSub();
        Expect(TokenType::KW_AND, "in NOT BETWEEN");
        auto high = ParseAddSub();
        std::vector<ParsedExprPtr> args;
        args.push_back(std::move(left));
        args.push_back(std::move(low));
        args.push_back(std::move(high));
        auto between = std::make_unique<FunctionExpression>("BETWEEN", std::move(args));
        return std::make_unique<NegationExpression>(std::move(between));
    }

    // IN (list or subquery)
    if (MatchKeyword(TokenType::KW_IN)) {
        Expect(TokenType::LPAREN, "after IN");
        // Check if it's a subquery.
        if (CheckKeyword(TokenType::KW_SELECT) || CheckKeyword(TokenType::KW_WITH)) {
            auto inner = ParseSelectStatement();
            Expect(TokenType::RPAREN, "after IN subquery");
            auto sub = std::make_unique<SubqueryExpression>(
                SubqueryType::IN_SUBQUERY,
                std::unique_ptr<SelectStatement>(
                    static_cast<SelectStatement *>(inner.release())));
            sub->child = std::move(left);
            return sub;
        }
        // Value list.
        std::vector<ParsedExprPtr> args;
        args.push_back(std::move(left));
        do {
            args.push_back(ParseExpression());
        } while (Match(TokenType::COMMA));
        Expect(TokenType::RPAREN, "after IN list");
        return std::make_unique<FunctionExpression>("IN", std::move(args));
    }

    return left;
}

ParsedExprPtr Parser::ParseAddSub() {
    auto left = ParseMulDiv();
    while (Check(TokenType::PLUS) || Check(TokenType::MINUS) || Check(TokenType::PIPE)) {
        auto op = Advance().value;
        auto right = ParseMulDiv();
        left = std::make_unique<ArithmeticExpression>(op, std::move(left), std::move(right));
    }
    return left;
}

ParsedExprPtr Parser::ParseMulDiv() {
    auto left = ParseUnary();
    while (Check(TokenType::STAR) || Check(TokenType::SLASH) || Check(TokenType::PERCENT)) {
        auto op = Advance().value;
        auto right = ParseUnary();
        left = std::make_unique<ArithmeticExpression>(op, std::move(left), std::move(right));
    }
    return left;
}

ParsedExprPtr Parser::ParseUnary() {
    if (Match(TokenType::MINUS)) {
        auto child = ParseUnary();
        return std::make_unique<UnaryMinusExpression>(std::move(child));
    }
    return ParsePrimary();
}

ParsedExprPtr Parser::ParsePrimary() {
    // Parenthesized expression.
    if (Match(TokenType::LPAREN)) {
        auto expr = ParseExpression();
        Expect(TokenType::RPAREN, "after expression");
        return expr;
    }

    // NULL.
    if (MatchKeyword(TokenType::KW_NULL)) {
        return std::make_unique<ConstantExpression>();
    }

    // TRUE / FALSE.
    if (MatchKeyword(TokenType::KW_TRUE)) {
        return std::make_unique<ConstantExpression>("true", TokenType::KW_TRUE);
    }
    if (MatchKeyword(TokenType::KW_FALSE)) {
        return std::make_unique<ConstantExpression>("false", TokenType::KW_FALSE);
    }

    // EXISTS (subquery).
    if (MatchKeyword(TokenType::KW_EXISTS)) {
        Expect(TokenType::LPAREN, "after EXISTS");
        auto inner = ParseSelectStatement();
        Expect(TokenType::RPAREN, "after EXISTS subquery");
        return std::make_unique<SubqueryExpression>(
            SubqueryType::EXISTS,
            std::unique_ptr<SelectStatement>(
                static_cast<SelectStatement *>(inner.release())));
    }

    // NOT EXISTS (subquery) — handled via NOT + EXISTS in ParseNot.

    // CAST(expr AS type).
    if (MatchKeyword(TokenType::KW_CAST)) {
        Expect(TokenType::LPAREN, "after CAST");
        auto expr = ParseExpression();
        Expect(TokenType::KW_AS, "in CAST");
        auto type_name = ParseTypeName();
        Expect(TokenType::RPAREN, "after CAST type");
        return std::make_unique<CastExpression>(std::move(expr), type_name);
    }

    // TRY_CAST(expr AS type) — returns NULL on failure instead of error.
    if (Check(TokenType::IDENTIFIER) && StringUtil::Upper(Current().value) == "TRY_CAST") {
        Advance();
        Expect(TokenType::LPAREN, "after TRY_CAST");
        auto expr = ParseExpression();
        Expect(TokenType::KW_AS, "in TRY_CAST");
        auto type_name = ParseTypeName();
        Expect(TokenType::RPAREN, "after TRY_CAST type");
        return std::make_unique<CastExpression>(std::move(expr), type_name, true);
    }

    // CASE WHEN ... THEN ... [ELSE ...] END
    if (MatchKeyword(TokenType::KW_CASE)) {
        // Parse as a function for now: CASE(when1, then1, when2, then2, ..., else)
        std::vector<ParsedExprPtr> args;
        while (MatchKeyword(TokenType::KW_WHEN)) {
            args.push_back(ParseExpression());
            Expect(TokenType::KW_THEN, "after WHEN condition");
            args.push_back(ParseExpression());
        }
        if (MatchKeyword(TokenType::KW_ELSE)) {
            args.push_back(ParseExpression());
        }
        Expect(TokenType::KW_END, "after CASE expression");
        return std::make_unique<FunctionExpression>("CASE", std::move(args));
    }

    // Aggregate functions and keywords that double as function names.
    if (CheckKeyword(TokenType::KW_COUNT) || CheckKeyword(TokenType::KW_SUM) ||
        CheckKeyword(TokenType::KW_AVG) || CheckKeyword(TokenType::KW_MIN) ||
        CheckKeyword(TokenType::KW_MAX) ||
        CheckKeyword(TokenType::KW_LEFT) || CheckKeyword(TokenType::KW_RIGHT)) {
        auto name = StringUtil::Upper(Advance().value);
        if (Check(TokenType::LPAREN)) {
            return ParseFunctionCall(name);
        }
        return std::make_unique<ColumnRefExpression>(name);
    }

    // NOW() function.
    if (CheckKeyword(TokenType::KW_NOW)) {
        Advance();
        if (Check(TokenType::LPAREN)) {
            return ParseFunctionCall("NOW");
        }
        return std::make_unique<FunctionExpression>("NOW", std::vector<ParsedExprPtr>{});
    }

    // CURRENT_TIMESTAMP (no parens required).
    if (MatchKeyword(TokenType::KW_CURRENT_TIMESTAMP)) {
        return std::make_unique<FunctionExpression>("CURRENT_TIMESTAMP", std::vector<ParsedExprPtr>{});
    }

    // CURRENT_DATE.
    if (MatchKeyword(TokenType::KW_CURRENT_DATE)) {
        return std::make_unique<FunctionExpression>("CURRENT_DATE", std::vector<ParsedExprPtr>{});
    }

    // EXTRACT(part FROM expr).
    if (MatchKeyword(TokenType::KW_EXTRACT)) {
        Expect(TokenType::LPAREN, "after EXTRACT");
        // Part name: YEAR, MONTH, DAY, HOUR, MINUTE, SECOND, EPOCH, DOW.
        std::string part = StringUtil::Upper(Advance().value);
        Expect(TokenType::KW_FROM, "in EXTRACT");
        auto expr = ParseExpression();
        Expect(TokenType::RPAREN, "after EXTRACT");
        std::vector<ParsedExprPtr> args;
        args.push_back(std::make_unique<ConstantExpression>(part, TokenType::STRING_LITERAL));
        args.push_back(std::move(expr));
        return std::make_unique<FunctionExpression>("EXTRACT", std::move(args));
    }

    // * (star).
    if (Check(TokenType::STAR)) {
        Advance();
        return std::make_unique<StarExpression>();
    }

    // Numeric literals.
    if (Check(TokenType::INTEGER_LITERAL)) {
        auto tok = Advance();
        return std::make_unique<ConstantExpression>(tok.value, TokenType::INTEGER_LITERAL);
    }
    if (Check(TokenType::FLOAT_LITERAL)) {
        auto tok = Advance();
        return std::make_unique<ConstantExpression>(tok.value, TokenType::FLOAT_LITERAL);
    }

    // String literal.
    if (Check(TokenType::STRING_LITERAL)) {
        auto tok = Advance();
        return std::make_unique<ConstantExpression>(tok.value, TokenType::STRING_LITERAL);
    }

    // Keywords that can also be column names.
    if (CheckKeyword(TokenType::KW_GENERATE_SERIES) ||
        CheckKeyword(TokenType::KW_VIEW) || CheckKeyword(TokenType::KW_FILTER) ||
        CheckKeyword(TokenType::KW_SAMPLE) ||
        CheckKeyword(TokenType::KW_YEAR) || CheckKeyword(TokenType::KW_MONTH) ||
        CheckKeyword(TokenType::KW_DAY) || CheckKeyword(TokenType::KW_HOUR) ||
        CheckKeyword(TokenType::KW_MINUTE) || CheckKeyword(TokenType::KW_SECOND) ||
        CheckKeyword(TokenType::KW_EPOCH) || CheckKeyword(TokenType::KW_DOW) ||
        CheckKeyword(TokenType::KW_DATE) || CheckKeyword(TokenType::KW_TIME) ||
        CheckKeyword(TokenType::KW_TIMESTAMP) ||
        CheckKeyword(TokenType::KW_MATCHED) || CheckKeyword(TokenType::KW_CONFLICT) ||
        CheckKeyword(TokenType::KW_DO) || CheckKeyword(TokenType::KW_NOTHING) ||
        CheckKeyword(TokenType::KW_RETURNING) ||
        CheckKeyword(TokenType::KW_ROWS) || CheckKeyword(TokenType::KW_RANGE) ||
        CheckKeyword(TokenType::KW_PRECEDING) || CheckKeyword(TokenType::KW_FOLLOWING) ||
        CheckKeyword(TokenType::KW_ROW) || CheckKeyword(TokenType::KW_CURRENT) ||
        CheckKeyword(TokenType::KW_UNBOUNDED)) {
        auto name = Advance().value;
        if (Check(TokenType::LPAREN)) return ParseFunctionCall(name);
        if (Match(TokenType::DOT)) {
            if (Match(TokenType::STAR)) return std::make_unique<StarExpression>(name);
            auto col = ExpectIdentifier("after '.'").value;
            return std::make_unique<ColumnRefExpression>(col, name);
        }
        return std::make_unique<ColumnRefExpression>(name);
    }

    // Identifier — could be column, table.column, or function call.
    if (Check(TokenType::IDENTIFIER)) {
        auto name = Advance().value;

        // Function call: name(args...).
        if (Check(TokenType::LPAREN)) {
            return ParseFunctionCall(name);
        }

        // table.column or table.*.
        if (Match(TokenType::DOT)) {
            if (Match(TokenType::STAR)) {
                return std::make_unique<StarExpression>(name);
            }
            auto col = ExpectIdentifier("after '.'").value;
            return std::make_unique<ColumnRefExpression>(col, name);
        }

        return std::make_unique<ColumnRefExpression>(name);
    }

    ThrowUnexpected("in expression");
}

ParsedExprPtr Parser::ParseFunctionCall(const std::string &name) {
    Expect(TokenType::LPAREN, "after function name");
    std::vector<ParsedExprPtr> args;
    bool distinct = false;

    if (!Check(TokenType::RPAREN)) {
        // COUNT(*) special case.
        if (StringUtil::Upper(name) == "COUNT" && Check(TokenType::STAR)) {
            Advance();
        } else {
            if (MatchKeyword(TokenType::KW_DISTINCT)) {
                distinct = true;
            }
            do {
                args.push_back(ParseExpression());
            } while (Match(TokenType::COMMA));
        }
    }
    Expect(TokenType::RPAREN, "after function arguments");

    // Check for OVER clause (window function).
    if (MatchKeyword(TokenType::KW_OVER)) {
        auto window = std::make_unique<WindowExpression>(
            StringUtil::Upper(name), std::move(args));

        Expect(TokenType::LPAREN, "after OVER");

        // PARTITION BY
        if (MatchKeyword(TokenType::KW_PARTITION)) {
            Expect(TokenType::KW_BY, "after PARTITION");
            do {
                window->partition_by.push_back(ParseExpression());
            } while (Match(TokenType::COMMA));
        }

        // ORDER BY
        if (MatchKeyword(TokenType::KW_ORDER)) {
            Expect(TokenType::KW_BY, "after ORDER in OVER");
            do {
                WindowOrderItem item;
                item.expression = ParseExpression();
                if (MatchKeyword(TokenType::KW_DESC)) {
                    item.ascending = false;
                } else {
                    MatchKeyword(TokenType::KW_ASC);
                }
                window->order_by.push_back(std::move(item));
            } while (Match(TokenType::COMMA));
        }

        Expect(TokenType::RPAREN, "after OVER clause");
        return window;
    }

    return std::make_unique<FunctionExpression>(StringUtil::Upper(name),
                                                std::move(args), distinct);
}

// UPDATE table SET col = val, ... [WHERE ...]
ParsedStmtPtr Parser::ParseUpdateStatement() {
    auto stmt = std::make_unique<UpdateStatement>();
    Expect(TokenType::KW_UPDATE, "");
    stmt->table_name = ExpectIdentifier("for table name").value;
    Expect(TokenType::KW_SET, "after table name");

    do {
        UpdateAssignment assign;
        assign.column_name = ExpectIdentifier("for column name").value;
        Expect(TokenType::EQUALS, "in SET clause");
        assign.value = ParseExpression();
        stmt->assignments.push_back(std::move(assign));
    } while (Match(TokenType::COMMA));

    if (MatchKeyword(TokenType::KW_WHERE)) {
        stmt->where_clause = ParseExpression();
    }

    return stmt;
}

// DELETE FROM table [WHERE ...]
ParsedStmtPtr Parser::ParseDeleteStatement() {
    auto stmt = std::make_unique<DeleteStatement>();
    Expect(TokenType::KW_DELETE, "");
    Expect(TokenType::KW_FROM, "after DELETE");
    stmt->table_name = ExpectIdentifier("for table name").value;

    if (MatchKeyword(TokenType::KW_WHERE)) {
        stmt->where_clause = ParseExpression();
    }

    return stmt;
}

// CREATE dispatches to TABLE or VIEW.
ParsedStmtPtr Parser::ParseCreateStatement() {
    Expect(TokenType::KW_CREATE, "");

    // CREATE OR REPLACE VIEW
    bool or_replace = false;
    if (CheckKeyword(TokenType::KW_OR)) {
        Advance();
        if (Check(TokenType::IDENTIFIER) && StringUtil::Upper(Current().value) == "REPLACE") {
            Advance();
            or_replace = true;
        }
    }

    // CREATE [OR REPLACE] [LIVE] VIEW
    bool is_live = false;
    if (CheckKeyword(TokenType::KW_LIVE)) {
        Advance();
        is_live = true;
    }

    if (CheckKeyword(TokenType::KW_VIEW)) {
        Advance(); // consume VIEW
        auto stmt = std::make_unique<CreateViewStatement>();
        stmt->or_replace = or_replace;
        stmt->is_live = is_live;
        stmt->view_name = ExpectIdentifier("for view name").value;
        Expect(TokenType::KW_AS, "after view name");
        auto inner = ParseSelectStatement();
        stmt->query = std::unique_ptr<SelectStatement>(
            static_cast<SelectStatement *>(inner.release()));
        return stmt;
    }

    // Otherwise expect TABLE — put CREATE back (already consumed).
    // We need to parse the rest of CREATE TABLE without the CREATE keyword.
    Expect(TokenType::KW_TABLE, "after CREATE");

    auto stmt = std::make_unique<CreateTableStatement>();
    if (MatchKeyword(TokenType::KW_IF)) {
        Expect(TokenType::KW_NOT, "after IF");
        Expect(TokenType::KW_EXISTS, "after NOT");
        stmt->if_not_exists = true;
    }

    stmt->table_name = ExpectIdentifier("for table name").value;

    // CREATE TABLE ... AS SELECT ...
    if (MatchKeyword(TokenType::KW_AS)) {
        // CTAS: will be handled at execution level.
        // Store the inner select as a special marker.
        // For now, parse columns from the select result.
        // We'll handle this in Connection::Query.
        stmt->table_name = stmt->table_name; // keep name
        // Parse isn't easy here without a proper CTAS node.
        // Skip for now and require column defs.
        ThrowError("CREATE TABLE AS SELECT not yet supported in parser");
    }

    Expect(TokenType::LPAREN, "after table name");
    do {
        ParsedColumnDef col;
        col.name = ExpectIdentifier("for column name").value;
        col.type_name = ParseTypeName();
        while (true) {
            if (MatchKeyword(TokenType::KW_NOT)) {
                Expect(TokenType::KW_NULL, "after NOT");
                col.not_null = true;
            } else if (MatchKeyword(TokenType::KW_PRIMARY)) {
                Expect(TokenType::KW_KEY, "after PRIMARY");
                col.is_primary_key = true;
                col.not_null = true;
            } else {
                break;
            }
        }
        stmt->columns.push_back(std::move(col));
    } while (Match(TokenType::COMMA));
    Expect(TokenType::RPAREN, "after column definitions");
    return stmt;
}

// DROP dispatches to TABLE or VIEW.
ParsedStmtPtr Parser::ParseDropStatement() {
    Expect(TokenType::KW_DROP, "");

    if (CheckKeyword(TokenType::KW_VIEW)) {
        Advance(); // consume VIEW
        auto stmt = std::make_unique<DropTableStatement>(); // reuse for simplicity
        if (MatchKeyword(TokenType::KW_IF)) {
            Expect(TokenType::KW_EXISTS, "after IF");
            stmt->if_exists = true;
        }
        stmt->table_name = ExpectIdentifier("for view name").value;
        return stmt;
    }

    Expect(TokenType::KW_TABLE, "after DROP");
    auto stmt = std::make_unique<DropTableStatement>();
    if (MatchKeyword(TokenType::KW_IF)) {
        Expect(TokenType::KW_EXISTS, "after IF");
        stmt->if_exists = true;
    }
    stmt->table_name = ExpectIdentifier("for table name").value;
    return stmt;
}

// EXPLAIN statement.
ParsedStmtPtr Parser::ParseExplainStatement() {
    Expect(TokenType::KW_EXPLAIN, "");
    auto stmt = std::make_unique<ExplainStatement>();
    stmt->inner = ParseStatement();
    return stmt;
}

// PRAGMA statement. Syntax:
//   PRAGMA <name>              -- e.g. PRAGMA database_list
//   PRAGMA <name>('<arg>')     -- e.g. PRAGMA table_info('t')
ParsedStmtPtr Parser::ParsePragmaStatement() {
    Expect(TokenType::KW_PRAGMA, "");
    auto stmt = std::make_unique<PragmaStatement>();
    stmt->name = ExpectIdentifier("for PRAGMA name").value;
    if (Match(TokenType::LPAREN)) {
        if (!Check(TokenType::RPAREN)) {
            // Single string (or bare identifier) argument is all we support.
            if (Check(TokenType::STRING_LITERAL)) {
                stmt->arg = Advance().value;
            } else {
                stmt->arg = ExpectIdentifier("for PRAGMA argument").value;
            }
        }
        Expect(TokenType::RPAREN, "after PRAGMA argument");
    }
    return stmt;
}

// DESCRIBE statement. Returns the result schema of the inner query as rows.
// Syntax: DESCRIBE <select-or-table-name>
ParsedStmtPtr Parser::ParseDescribeStatement() {
    Expect(TokenType::KW_DESCRIBE, "");
    auto stmt = std::make_unique<DescribeStatement>();
    // DESCRIBE <table_name>  →  desugars to SELECT * FROM <table_name>
    // DESCRIBE SELECT ...    →  inner = SELECT
    // DESCRIBE WITH ...      →  inner = CTE-SELECT
    if (CheckKeyword(TokenType::KW_SELECT) || CheckKeyword(TokenType::KW_WITH)) {
        stmt->inner = ParseSelectStatement();
    } else {
        // Treat bare identifier as `SELECT * FROM <identifier>`.
        auto select = std::make_unique<SelectStatement>();
        auto star = std::make_unique<StarExpression>();
        select->select_list.push_back(std::move(star));
        auto tref = std::make_unique<TableRef>();
        tref->table_name = ExpectIdentifier("for DESCRIBE target").value;
        select->from_table = std::move(tref);
        stmt->inner = std::move(select);
    }
    return stmt;
}

// MERGE INTO target USING source ON condition
// WHEN MATCHED THEN UPDATE SET ... WHEN NOT MATCHED THEN INSERT ...
ParsedStmtPtr Parser::ParseMergeStatement() {
    auto stmt = std::make_unique<MergeStatement>();
    Expect(TokenType::KW_MERGE, "");
    Expect(TokenType::KW_INTO, "after MERGE");

    stmt->target_table = ExpectIdentifier("for target table").value;
    if (MatchKeyword(TokenType::KW_AS) || Check(TokenType::IDENTIFIER)) {
        stmt->target_alias = Advance().value;
    }

    // USING source
    MatchKeyword(TokenType::KW_USING);
    stmt->source_table = ExpectIdentifier("for source table").value;
    if (MatchKeyword(TokenType::KW_AS) || (Check(TokenType::IDENTIFIER) &&
        !CheckKeyword(TokenType::KW_ON))) {
        stmt->source_alias = Advance().value;
    }

    Expect(TokenType::KW_ON, "in MERGE");
    stmt->on_condition = ParseExpression();

    // WHEN MATCHED / WHEN NOT MATCHED clauses.
    while (MatchKeyword(TokenType::KW_WHEN)) {
        if (MatchKeyword(TokenType::KW_NOT)) {
            Expect(TokenType::KW_MATCHED, "after NOT");
            // WHEN NOT MATCHED THEN INSERT
            Expect(TokenType::KW_THEN, "after MATCHED");
            Expect(TokenType::KW_INSERT, "in MERGE");
            stmt->has_insert = true;

            if (Match(TokenType::LPAREN)) {
                do {
                    stmt->insert_columns.push_back(
                        ExpectIdentifier("for column").value);
                } while (Match(TokenType::COMMA));
                Expect(TokenType::RPAREN, "after column list");
            }
            Expect(TokenType::KW_VALUES, "in MERGE INSERT");
            Expect(TokenType::LPAREN, "after VALUES");
            do {
                stmt->insert_values.push_back(ParseExpression());
            } while (Match(TokenType::COMMA));
            Expect(TokenType::RPAREN, "after VALUES");
        } else {
            MatchKeyword(TokenType::KW_MATCHED);
            // WHEN MATCHED THEN UPDATE
            Expect(TokenType::KW_THEN, "after MATCHED");
            Expect(TokenType::KW_UPDATE, "in MERGE");
            Expect(TokenType::KW_SET, "after UPDATE");
            stmt->has_update = true;

            do {
                UpdateAssignment assign;
                assign.column_name = ExpectIdentifier("for column").value;
                Expect(TokenType::EQUALS, "in SET");
                assign.value = ParseExpression();
                stmt->update_assignments.push_back(std::move(assign));
            } while (Match(TokenType::COMMA));
        }
    }

    return stmt;
}

} // namespace slothdb
