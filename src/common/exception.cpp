#include "slothdb/common/exception.hpp"

namespace slothdb {

SlothDBException::SlothDBException(ErrorCategory category, ErrorCode code,
                                     const std::string &message)
    : category_(category), code_(code), message_(message) {
    // Prepend category for human-readable what().
    message_ = std::string(ErrorCategoryToString(category)) + " Error: " + message;
}

void SlothDBException::SetContext(const std::string &key, const std::string &value) {
    context_[key] = value;
}

std::optional<std::string> SlothDBException::GetContext(const std::string &key) const {
    auto it = context_.find(key);
    if (it == context_.end()) return std::nullopt;
    return it->second;
}

void SlothDBException::SetQueryLocation(uint32_t line, uint32_t column) {
    line_ = line;
    column_ = column;
    context_["line"] = std::to_string(line);
    context_["column"] = std::to_string(column);
}

const char *ErrorCategoryToString(ErrorCategory category) {
    switch (category) {
    case ErrorCategory::INTERNAL: return "Internal";
    case ErrorCategory::PARSER: return "Parser";
    case ErrorCategory::BINDER: return "Binder";
    case ErrorCategory::PLANNER: return "Planner";
    case ErrorCategory::OPTIMIZER: return "Optimizer";
    case ErrorCategory::EXECUTOR: return "Executor";
    case ErrorCategory::CATALOG: return "Catalog";
    case ErrorCategory::STORAGE: return "Storage";
    case ErrorCategory::IO: return "IO";
    case ErrorCategory::TRANSACTION: return "Transaction";
    case ErrorCategory::CONVERSION: return "Conversion";
    case ErrorCategory::CONSTRAINT: return "Constraint";
    case ErrorCategory::INVALID_INPUT: return "Invalid Input";
    case ErrorCategory::NOT_IMPLEMENTED: return "Not Implemented";
    }
    return "Unknown";
}

const char *ErrorCodeToString(ErrorCode code) {
    switch (code) {
    case ErrorCode::UNKNOWN: return "UNKNOWN";
    case ErrorCode::INTERNAL_ERROR: return "INTERNAL_ERROR";
    case ErrorCode::NOT_IMPLEMENTED: return "NOT_IMPLEMENTED";
    case ErrorCode::SYNTAX_ERROR: return "SYNTAX_ERROR";
    case ErrorCode::UNEXPECTED_TOKEN: return "UNEXPECTED_TOKEN";
    case ErrorCode::UNTERMINATED_STRING: return "UNTERMINATED_STRING";
    case ErrorCode::INVALID_NUMERIC_LITERAL: return "INVALID_NUMERIC_LITERAL";
    case ErrorCode::TABLE_NOT_FOUND: return "TABLE_NOT_FOUND";
    case ErrorCode::COLUMN_NOT_FOUND: return "COLUMN_NOT_FOUND";
    case ErrorCode::AMBIGUOUS_COLUMN: return "AMBIGUOUS_COLUMN";
    case ErrorCode::TYPE_MISMATCH: return "TYPE_MISMATCH";
    case ErrorCode::AGGREGATE_IN_WHERE: return "AGGREGATE_IN_WHERE";
    case ErrorCode::TABLE_ALREADY_EXISTS: return "TABLE_ALREADY_EXISTS";
    case ErrorCode::SCHEMA_NOT_FOUND: return "SCHEMA_NOT_FOUND";
    case ErrorCode::SCHEMA_ALREADY_EXISTS: return "SCHEMA_ALREADY_EXISTS";
    case ErrorCode::FILE_NOT_FOUND: return "FILE_NOT_FOUND";
    case ErrorCode::FILE_READ_ERROR: return "FILE_READ_ERROR";
    case ErrorCode::FILE_WRITE_ERROR: return "FILE_WRITE_ERROR";
    case ErrorCode::CONVERSION_ERROR: return "CONVERSION_ERROR";
    case ErrorCode::OUT_OF_RANGE: return "OUT_OF_RANGE";
    case ErrorCode::NULL_VALUE_ACCESS: return "NULL_VALUE_ACCESS";
    case ErrorCode::NOT_NULL_VIOLATION: return "NOT_NULL_VIOLATION";
    case ErrorCode::UNIQUE_VIOLATION: return "UNIQUE_VIOLATION";
    case ErrorCode::TRANSACTION_CONFLICT: return "TRANSACTION_CONFLICT";
    case ErrorCode::WRITE_LOCK_FAILED: return "WRITE_LOCK_FAILED";
    case ErrorCode::STORAGE_ERROR: return "STORAGE_ERROR";
    case ErrorCode::OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    case ErrorCode::CORRUPT_DATA: return "CORRUPT_DATA";
    }
    return "UNKNOWN";
}

} // namespace slothdb
