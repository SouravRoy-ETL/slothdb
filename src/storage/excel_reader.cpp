#include "slothdb/storage/excel_reader.hpp"
#include "slothdb/common/exception.hpp"
#include <fstream>
#include <cstring>
#include <algorithm>

namespace slothdb {

ExcelReader::ExcelReader(const std::string &path, const std::string &sheet)
    : path_(path), sheet_name_(sheet) {}

std::string ExcelReader::ExtractZipEntry(const std::vector<uint8_t> &zip_data,
                                          const std::string &entry_name) {
    // Scan for local file headers (PK\x03\x04).
    for (size_t pos = 0; pos + 30 < zip_data.size(); ) {
        if (zip_data[pos] != 0x50 || zip_data[pos + 1] != 0x4B ||
            zip_data[pos + 2] != 0x03 || zip_data[pos + 3] != 0x04) {
            pos++;
            continue;
        }

        uint16_t compression;
        std::memcpy(&compression, &zip_data[pos + 8], 2);
        uint32_t compressed_size, uncompressed_size;
        std::memcpy(&compressed_size, &zip_data[pos + 18], 4);
        std::memcpy(&uncompressed_size, &zip_data[pos + 22], 4);
        uint16_t name_len, extra_len;
        std::memcpy(&name_len, &zip_data[pos + 26], 2);
        std::memcpy(&extra_len, &zip_data[pos + 28], 2);

        auto name = std::string(reinterpret_cast<const char *>(&zip_data[pos + 30]), name_len);
        auto data_start = pos + 30 + name_len + extra_len;

        if (name == entry_name) {
            if (compression == 0) {
                // Stored (no compression).
                return std::string(reinterpret_cast<const char *>(&zip_data[data_start]),
                                    uncompressed_size);
            }
            // Deflate compressed — we cannot decompress without a deflate implementation.
            // Return empty and fall through to error handling.
            return "";
        }

        pos = data_start + compressed_size;
    }
    return "";
}

std::vector<std::string> ExcelReader::ParseSharedStrings(const std::string &xml) {
    std::vector<std::string> strings;
    size_t pos = 0;
    while (pos < xml.size()) {
        auto si_start = xml.find("<si>", pos);
        if (si_start == std::string::npos) break;
        auto si_end = xml.find("</si>", si_start);
        if (si_end == std::string::npos) break;

        // Extract text from <t>...</t> within <si>.
        auto t_start = xml.find("<t", si_start);
        if (t_start != std::string::npos && t_start < si_end) {
            auto content_start = xml.find('>', t_start) + 1;
            auto content_end = xml.find("</t>", content_start);
            if (content_end != std::string::npos && content_end <= si_end) {
                strings.push_back(xml.substr(content_start, content_end - content_start));
            } else {
                strings.push_back("");
            }
        } else {
            strings.push_back("");
        }

        pos = si_end + 5;
    }
    return strings;
}

void ExcelReader::ParseSheetXML(const std::string &xml,
                                 const std::vector<std::string> &shared_strings) {
    // Parse <row> elements containing <c> (cell) elements.
    size_t pos = 0;
    bool first_row = true;

    while (pos < xml.size()) {
        auto row_start = xml.find("<row", pos);
        if (row_start == std::string::npos) break;
        auto row_end = xml.find("</row>", row_start);
        if (row_end == std::string::npos) break;

        auto row_xml = xml.substr(row_start, row_end - row_start + 6);
        std::vector<std::string> cell_values;

        // Parse cells.
        size_t cpos = 0;
        while (cpos < row_xml.size()) {
            auto c_start = row_xml.find("<c ", cpos);
            if (c_start == std::string::npos) break;

            // Get cell type attribute: t="s" (shared string), t="n" or no t (number).
            std::string cell_type;
            auto t_attr = row_xml.find("t=\"", c_start);
            if (t_attr != std::string::npos && t_attr < row_xml.find(">", c_start)) {
                auto t_end = row_xml.find("\"", t_attr + 3);
                cell_type = row_xml.substr(t_attr + 3, t_end - t_attr - 3);
            }

            // Get value from <v>...</v>.
            auto v_start = row_xml.find("<v>", c_start);
            auto c_end = row_xml.find("</c>", c_start);
            if (c_end == std::string::npos) break;

            std::string value;
            if (v_start != std::string::npos && v_start < c_end) {
                auto v_end = row_xml.find("</v>", v_start);
                value = row_xml.substr(v_start + 3, v_end - v_start - 3);
            }

            // Resolve shared strings.
            if (cell_type == "s" && !value.empty()) {
                auto idx = std::stoul(value);
                if (idx < shared_strings.size()) {
                    value = shared_strings[idx];
                }
            }

            cell_values.push_back(value);
            cpos = c_end + 4;
        }

        if (first_row) {
            // Use first row as header.
            column_names_ = cell_values;
            column_types_.resize(cell_values.size(), LogicalType::VARCHAR());
            first_row = false;
        } else if (!cell_values.empty()) {
            std::vector<Value> row;
            for (size_t i = 0; i < column_types_.size(); i++) {
                if (i < cell_values.size() && !cell_values[i].empty()) {
                    // Try numeric.
                    try {
                        auto d = std::stod(cell_values[i]);
                        if (cell_values[i].find('.') != std::string::npos)
                            row.push_back(Value::DOUBLE(d));
                        else
                            row.push_back(Value::INTEGER(static_cast<int32_t>(d)));
                    } catch (...) {
                        row.push_back(Value::VARCHAR(cell_values[i]));
                    }
                } else {
                    row.push_back(Value());
                }
            }
            rows_.push_back(std::move(row));
        }

        pos = row_end + 6;
    }
}

bool ExcelReader::TryReadXLSX() {
    std::ifstream file(path_, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    auto size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char *>(data.data()), size);

    // Check ZIP magic (PK).
    if (size < 4 || data[0] != 0x50 || data[1] != 0x4B) return false;

    // Extract shared strings.
    auto ss_xml = ExtractZipEntry(data, "xl/sharedStrings.xml");
    auto shared_strings = ParseSharedStrings(ss_xml);

    // Extract worksheet.
    std::string sheet_xml;
    if (!sheet_name_.empty()) {
        sheet_xml = ExtractZipEntry(data, "xl/worksheets/" + sheet_name_ + ".xml");
    }
    if (sheet_xml.empty()) {
        sheet_xml = ExtractZipEntry(data, "xl/worksheets/sheet1.xml");
    }

    if (sheet_xml.empty()) {
        // Worksheets might be deflate-compressed.
        throw IOException("Cannot read .xlsx: worksheets are compressed. "
                           "SlothDB's built-in Excel reader supports uncompressed "
                           ".xlsx files. Use CSV or JSON format instead.");
    }

    ParseSheetXML(sheet_xml, shared_strings);
    return true;
}

void ExcelReader::Parse() {
    if (parsed_) return;
    parsed_ = true;

    if (!TryReadXLSX()) {
        throw IOException(ErrorCode::FILE_READ_ERROR,
                           "Cannot read Excel file: " + path_);
    }
}

std::vector<std::vector<Value>> ExcelReader::ReadAll() {
    Parse();
    return rows_;
}

} // namespace slothdb
