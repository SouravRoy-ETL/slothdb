#pragma once

#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/value.hpp"
#include <fstream>
#include <string>
#include <vector>

namespace slothdb {

class DataTable;

struct CSVOptions {
    char delimiter = ',';
    char quote = '"';
    char escape = '"';
    bool header = true;
    std::string null_string = "";
};

// CSV Reader: parse a CSV file into rows of Values.
class CSVReader {
public:
    CSVReader(const std::string &path, CSVOptions options = {});

    // Read header row (column names).
    std::vector<std::string> ReadHeader();

    // Auto-detect column types from first N rows.
    std::vector<LogicalType> DetectTypes(idx_t sample_size = 100);

    // Read all rows as Values. Each inner vector is one row.
    std::vector<std::vector<Value>> ReadAll(const std::vector<LogicalType> &types);

    // Read next batch of rows (up to batch_size).
    std::vector<std::vector<Value>> ReadBatch(const std::vector<LogicalType> &types,
                                                idx_t batch_size);

    // Read directly into a DataChunk (up to VECTOR_SIZE rows). Returns rows read.
    // This is the fast path - avoids intermediate Value/vector allocations.
    idx_t ReadChunk(DataChunk &chunk, const std::vector<LogicalType> &types);

    // Stream all data directly into a DataTable in chunks. Fastest bulk load.
    void ReadIntoTable(DataTable &table, const std::vector<LogicalType> &types);

    bool IsEOF() const { return eof_; }

private:
    std::vector<std::string> ParseLine();
    void SetValueDirect(DataChunk &chunk, idx_t col, idx_t row,
                        const std::string &str, const LogicalType &type);
    Value ConvertValue(const std::string &str, const LogicalType &type);

    std::ifstream file_;
    CSVOptions options_;
    bool eof_ = false;
    bool header_read_ = false;
};

// CSV Writer: write rows to a CSV file.
class CSVWriter {
public:
    CSVWriter(const std::string &path, CSVOptions options = {});

    void WriteHeader(const std::vector<std::string> &columns);
    void WriteRow(const std::vector<Value> &row);
    void Flush();

private:
    std::ofstream file_;
    CSVOptions options_;
};

} // namespace slothdb
