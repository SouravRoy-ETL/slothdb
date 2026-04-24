#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <unordered_map>
#include <optional>

namespace slothdb {

// ============================================================================
// STRUCTURED ERROR SYSTEM (SlothDB Differentiator #5)
//
// Unlike DuckDB's free-form string errors, SlothDB provides machine-readable
// error codes, categories, and contextual metadata. This guarantees stable
// programmatic error handling across versions.
// ============================================================================

// Error category - which subsystem produced the error.
enum class ErrorCategory : uint8_t {
    INTERNAL,
    PARSER,
    BINDER,
    PLANNER,
    OPTIMIZER,
    EXECUTOR,
    CATALOG,
    STORAGE,
    IO,
    TRANSACTION,
    CONVERSION,
    CONSTRAINT,
    INVALID_INPUT,
    NOT_IMPLEMENTED
};

// Numeric error codes - stable across versions.
enum class ErrorCode : uint32_t {
    // Generic
    UNKNOWN = 0,
    INTERNAL_ERROR = 1,
    NOT_IMPLEMENTED = 2,

    // Parser (1000-1999)
    SYNTAX_ERROR = 1000,
    UNEXPECTED_TOKEN = 1001,
    UNTERMINATED_STRING = 1002,
    INVALID_NUMERIC_LITERAL = 1003,

    // Binder (2000-2999)
    TABLE_NOT_FOUND = 2000,
    COLUMN_NOT_FOUND = 2001,
    AMBIGUOUS_COLUMN = 2002,
    TYPE_MISMATCH = 2003,
    AGGREGATE_IN_WHERE = 2004,

    // Catalog (3000-3999)
    TABLE_ALREADY_EXISTS = 3000,
    SCHEMA_NOT_FOUND = 3001,
    SCHEMA_ALREADY_EXISTS = 3002,

    // IO (4000-4999)
    FILE_NOT_FOUND = 4000,
    FILE_READ_ERROR = 4001,
    FILE_WRITE_ERROR = 4002,

    // Conversion (5000-5999)
    CONVERSION_ERROR = 5000,
    OUT_OF_RANGE = 5001,
    NULL_VALUE_ACCESS = 5002,

    // Constraint (6000-6999)
    NOT_NULL_VIOLATION = 6000,
    UNIQUE_VIOLATION = 6001,

    // Transaction (7000-7999)
    TRANSACTION_CONFLICT = 7000,
    WRITE_LOCK_FAILED = 7001,

    // Storage (8000-8999)
    STORAGE_ERROR = 8000,
    OUT_OF_MEMORY = 8001,
    CORRUPT_DATA = 8002,
};

const char *ErrorCategoryToString(ErrorCategory category);
const char *ErrorCodeToString(ErrorCode code);

// The structured error base class.
class SlothDBException : public std::exception {
public:
    SlothDBException(ErrorCategory category, ErrorCode code, const std::string &message);

    const char *what() const noexcept override { return message_.c_str(); }

    ErrorCategory GetCategory() const { return category_; }
    ErrorCode GetCode() const { return code_; }
    uint32_t GetNumericCode() const { return static_cast<uint32_t>(code_); }
    const std::string &GetMessage() const { return message_; }

    // Contextual metadata - key-value pairs with extra info.
    void SetContext(const std::string &key, const std::string &value);
    std::optional<std::string> GetContext(const std::string &key) const;
    const std::unordered_map<std::string, std::string> &GetAllContext() const { return context_; }

    // Source location (for parser/binder errors).
    void SetQueryLocation(uint32_t line, uint32_t column);
    uint32_t GetLine() const { return line_; }
    uint32_t GetColumn() const { return column_; }

protected:
    ErrorCategory category_;
    ErrorCode code_;
    std::string message_;
    std::unordered_map<std::string, std::string> context_;
    uint32_t line_ = 0;
    uint32_t column_ = 0;
};

// Derived exception classes - convenience constructors, correct category/code.

class InternalException : public SlothDBException {
public:
    explicit InternalException(const std::string &msg)
        : SlothDBException(ErrorCategory::INTERNAL, ErrorCode::INTERNAL_ERROR, msg) {}
};

class NotImplementedException : public SlothDBException {
public:
    explicit NotImplementedException(const std::string &msg)
        : SlothDBException(ErrorCategory::NOT_IMPLEMENTED, ErrorCode::NOT_IMPLEMENTED, msg) {}
};

class ParserException : public SlothDBException {
public:
    explicit ParserException(const std::string &msg)
        : SlothDBException(ErrorCategory::PARSER, ErrorCode::SYNTAX_ERROR, msg) {}
    ParserException(ErrorCode code, const std::string &msg)
        : SlothDBException(ErrorCategory::PARSER, code, msg) {}
};

class BinderException : public SlothDBException {
public:
    explicit BinderException(const std::string &msg)
        : SlothDBException(ErrorCategory::BINDER, ErrorCode::TABLE_NOT_FOUND, msg) {}
    BinderException(ErrorCode code, const std::string &msg)
        : SlothDBException(ErrorCategory::BINDER, code, msg) {}
};

class CatalogException : public SlothDBException {
public:
    explicit CatalogException(const std::string &msg)
        : SlothDBException(ErrorCategory::CATALOG, ErrorCode::TABLE_ALREADY_EXISTS, msg) {}
    CatalogException(ErrorCode code, const std::string &msg)
        : SlothDBException(ErrorCategory::CATALOG, code, msg) {}
};

class IOException : public SlothDBException {
public:
    explicit IOException(const std::string &msg)
        : SlothDBException(ErrorCategory::IO, ErrorCode::FILE_READ_ERROR, msg) {}
    IOException(ErrorCode code, const std::string &msg)
        : SlothDBException(ErrorCategory::IO, code, msg) {}
};

class ConversionException : public SlothDBException {
public:
    explicit ConversionException(const std::string &msg)
        : SlothDBException(ErrorCategory::CONVERSION, ErrorCode::CONVERSION_ERROR, msg) {}
    ConversionException(ErrorCode code, const std::string &msg)
        : SlothDBException(ErrorCategory::CONVERSION, code, msg) {}
};

class OutOfRangeException : public SlothDBException {
public:
    explicit OutOfRangeException(const std::string &msg)
        : SlothDBException(ErrorCategory::CONVERSION, ErrorCode::OUT_OF_RANGE, msg) {}
};

class InvalidInputException : public SlothDBException {
public:
    explicit InvalidInputException(const std::string &msg)
        : SlothDBException(ErrorCategory::INVALID_INPUT, ErrorCode::UNKNOWN, msg) {}
};

class ConstraintException : public SlothDBException {
public:
    explicit ConstraintException(const std::string &msg)
        : SlothDBException(ErrorCategory::CONSTRAINT, ErrorCode::NOT_NULL_VIOLATION, msg) {}
    ConstraintException(ErrorCode code, const std::string &msg)
        : SlothDBException(ErrorCategory::CONSTRAINT, code, msg) {}
};

class TransactionException : public SlothDBException {
public:
    explicit TransactionException(const std::string &msg)
        : SlothDBException(ErrorCategory::TRANSACTION, ErrorCode::TRANSACTION_CONFLICT, msg) {}
    TransactionException(ErrorCode code, const std::string &msg)
        : SlothDBException(ErrorCategory::TRANSACTION, code, msg) {}
};

} // namespace slothdb
