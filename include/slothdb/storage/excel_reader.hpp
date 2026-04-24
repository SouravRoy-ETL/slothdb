#pragma once

#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/value.hpp"
#include <string>
#include <vector>

namespace slothdb {

// Excel (.xlsx) reader.
// XLSX is a ZIP containing XML files:
//   xl/worksheets/sheet1.xml - cell data
//   xl/sharedStrings.xml - string pool
//   xl/styles.xml - cell formatting
//
// This is a minimal reader for simple flat tables (no formulas, merged cells, etc.).
// Since implementing full ZIP deflate decompression from scratch is extremely complex,
// this reader handles .xlsx files that use "stored" (uncompressed) ZIP entries,
// and also supports a simpler fallback: CSV-like .xlsx files.
//
// For full .xlsx support with deflate, a compression library would be needed.
// As an alternative, we provide a TabularExcelReader that can read
// simple XML-based spreadsheet formats.

class ExcelReader {
public:
    explicit ExcelReader(const std::string &path, const std::string &sheet = "");

    const std::vector<std::string> &GetColumnNames() const { return column_names_; }
    const std::vector<LogicalType> &GetColumnTypes() const { return column_types_; }
    int64_t NumRows() const { return static_cast<int64_t>(rows_.size()); }

    std::vector<std::vector<Value>> ReadAll();

private:
    void Parse();
    // Try to read as a ZIP-based .xlsx.
    bool TryReadXLSX();
    // Extract a file from a ZIP archive (stored entries only).
    std::string ExtractZipEntry(const std::vector<uint8_t> &zip_data,
                                 const std::string &entry_name);
    // Parse worksheet XML to extract cell values.
    void ParseSheetXML(const std::string &xml, const std::vector<std::string> &shared_strings);
    // Parse shared strings XML.
    std::vector<std::string> ParseSharedStrings(const std::string &xml);

    std::string path_;
    std::string sheet_name_;
    std::vector<std::string> column_names_;
    std::vector<LogicalType> column_types_;
    std::vector<std::vector<Value>> rows_;
    bool parsed_ = false;
};

} // namespace slothdb
