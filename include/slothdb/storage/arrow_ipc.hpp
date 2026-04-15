#pragma once

#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/value.hpp"
#include <string>
#include <vector>

namespace slothdb {

// Arrow IPC (Feather v2) reader/writer.
// Simplified implementation for flat (non-nested) columnar data.
// Format: [ARROW1 magic][schema][record batches][footer][footer_size][ARROW1]

class ArrowIPCWriter {
public:
    ArrowIPCWriter(const std::string &path,
                   const std::vector<std::string> &column_names,
                   const std::vector<LogicalType> &column_types);

    void WriteBatch(const std::vector<std::vector<Value>> &rows);
    void Finish();

private:
    std::string path_;
    std::vector<std::string> column_names_;
    std::vector<LogicalType> column_types_;
    std::vector<std::vector<std::vector<Value>>> batches_;
};

class ArrowIPCReader {
public:
    explicit ArrowIPCReader(const std::string &path);

    const std::vector<std::string> &GetColumnNames() const { return column_names_; }
    const std::vector<LogicalType> &GetColumnTypes() const { return column_types_; }
    int64_t NumRows() const;

    std::vector<std::vector<Value>> ReadAll();

private:
    std::string path_;
    std::vector<std::string> column_names_;
    std::vector<LogicalType> column_types_;
    std::vector<std::vector<Value>> rows_;
    bool parsed_ = false;
    void Parse();
};

} // namespace slothdb
