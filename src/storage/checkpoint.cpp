#include "slothdb/storage/checkpoint.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/types/value.hpp"
#include <cstring>

namespace slothdb {

// ============================================================================
// Serialization helpers
// ============================================================================

void Checkpoint::WriteString(BlockManager &bm, idx_t &offset, const std::string &str) {
    WriteU32(bm, offset, static_cast<uint32_t>(str.size()));
    if (!str.empty()) {
        bm.WriteData(reinterpret_cast<const_data_ptr_t>(str.data()), str.size(), offset);
        offset += str.size();
    }
}

std::string Checkpoint::ReadString(BlockManager &bm, idx_t &offset) {
    auto len = ReadU32(bm, offset);
    if (len == 0) return "";
    std::string str(len, '\0');
    bm.ReadData(reinterpret_cast<data_ptr_t>(str.data()), len, offset);
    offset += len;
    return str;
}

void Checkpoint::WriteU8(BlockManager &bm, idx_t &offset, uint8_t val) {
    bm.WriteData(reinterpret_cast<const_data_ptr_t>(&val), 1, offset);
    offset += 1;
}

uint8_t Checkpoint::ReadU8(BlockManager &bm, idx_t &offset) {
    uint8_t val;
    bm.ReadData(reinterpret_cast<data_ptr_t>(&val), 1, offset);
    offset += 1;
    return val;
}

void Checkpoint::WriteU32(BlockManager &bm, idx_t &offset, uint32_t val) {
    bm.WriteData(reinterpret_cast<const_data_ptr_t>(&val), 4, offset);
    offset += 4;
}

uint32_t Checkpoint::ReadU32(BlockManager &bm, idx_t &offset) {
    uint32_t val;
    bm.ReadData(reinterpret_cast<data_ptr_t>(&val), 4, offset);
    offset += 4;
    return val;
}

void Checkpoint::WriteU64(BlockManager &bm, idx_t &offset, uint64_t val) {
    bm.WriteData(reinterpret_cast<const_data_ptr_t>(&val), 8, offset);
    offset += 8;
}

uint64_t Checkpoint::ReadU64(BlockManager &bm, idx_t &offset) {
    uint64_t val;
    bm.ReadData(reinterpret_cast<data_ptr_t>(&val), 8, offset);
    offset += 8;
    return val;
}

void Checkpoint::WriteI32(BlockManager &bm, idx_t &offset, int32_t val) {
    bm.WriteData(reinterpret_cast<const_data_ptr_t>(&val), 4, offset);
    offset += 4;
}

int32_t Checkpoint::ReadI32(BlockManager &bm, idx_t &offset) {
    int32_t val;
    bm.ReadData(reinterpret_cast<data_ptr_t>(&val), 4, offset);
    offset += 4;
    return val;
}

void Checkpoint::WriteI64(BlockManager &bm, idx_t &offset, int64_t val) {
    bm.WriteData(reinterpret_cast<const_data_ptr_t>(&val), 8, offset);
    offset += 8;
}

int64_t Checkpoint::ReadI64(BlockManager &bm, idx_t &offset) {
    int64_t val;
    bm.ReadData(reinterpret_cast<data_ptr_t>(&val), 8, offset);
    offset += 8;
    return val;
}

void Checkpoint::WriteDouble(BlockManager &bm, idx_t &offset, double val) {
    bm.WriteData(reinterpret_cast<const_data_ptr_t>(&val), 8, offset);
    offset += 8;
}

double Checkpoint::ReadDouble(BlockManager &bm, idx_t &offset) {
    double val;
    bm.ReadData(reinterpret_cast<data_ptr_t>(&val), 8, offset);
    offset += 8;
    return val;
}

// ============================================================================
// Value serialization
// ============================================================================

void Checkpoint::WriteValue(BlockManager &bm, idx_t &offset, const Value &val,
                             const LogicalType &type) {
    // Write null flag.
    WriteU8(bm, offset, val.IsNull() ? 1 : 0);
    if (val.IsNull()) return;

    switch (type.id()) {
    case LogicalTypeId::BOOLEAN:
        WriteU8(bm, offset, val.GetValue<bool>() ? 1 : 0);
        break;
    case LogicalTypeId::TINYINT:
        WriteU8(bm, offset, static_cast<uint8_t>(val.GetValue<int8_t>()));
        break;
    case LogicalTypeId::SMALLINT:
        WriteI32(bm, offset, val.GetValue<int16_t>());
        break;
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::DATE:
        WriteI32(bm, offset, val.GetValue<int32_t>());
        break;
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIME:
        WriteI64(bm, offset, val.GetValue<int64_t>());
        break;
    case LogicalTypeId::FLOAT:
        WriteI32(bm, offset, 0); // placeholder
        {
            float f = val.GetValue<float>();
            int32_t bits;
            std::memcpy(&bits, &f, 4);
            // rewrite
            offset -= 4;
            bm.WriteData(reinterpret_cast<const_data_ptr_t>(&f), 4, offset);
            offset += 4;
        }
        break;
    case LogicalTypeId::DOUBLE:
        WriteDouble(bm, offset, val.GetValue<double>());
        break;
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::BLOB:
        WriteString(bm, offset, val.GetValue<std::string>());
        break;
    default:
        // Fallback: serialize as string.
        WriteString(bm, offset, val.ToString());
        break;
    }
}

Value Checkpoint::ReadValue(BlockManager &bm, idx_t &offset, const LogicalType &type) {
    auto is_null = ReadU8(bm, offset);
    if (is_null) return Value();

    switch (type.id()) {
    case LogicalTypeId::BOOLEAN:
        return Value::BOOLEAN(ReadU8(bm, offset) != 0);
    case LogicalTypeId::TINYINT:
        return Value::TINYINT(static_cast<int8_t>(ReadU8(bm, offset)));
    case LogicalTypeId::SMALLINT:
        return Value::SMALLINT(static_cast<int16_t>(ReadI32(bm, offset)));
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::DATE:
        return Value::INTEGER(ReadI32(bm, offset));
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIME:
        return Value::BIGINT(ReadI64(bm, offset));
    case LogicalTypeId::FLOAT: {
        float f;
        bm.ReadData(reinterpret_cast<data_ptr_t>(&f), 4, offset);
        offset += 4;
        return Value::FLOAT(f);
    }
    case LogicalTypeId::DOUBLE:
        return Value::DOUBLE(ReadDouble(bm, offset));
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::BLOB:
        return Value::VARCHAR(ReadString(bm, offset));
    default:
        return Value::VARCHAR(ReadString(bm, offset));
    }
}

// ============================================================================
// Save: serialize entire database
// ============================================================================

void Checkpoint::Save(Catalog &catalog, const std::string &path) {
    BlockManager bm;
    bm.CreateFile(path);

    // Start writing after the header area (4096 bytes reserved).
    idx_t offset = 4096;

    // Get all tables from the default schema.
    auto &schema = catalog.GetSchema();
    auto table_names = schema.GetTableNames();

    // Write number of tables.
    WriteU32(bm, offset, static_cast<uint32_t>(table_names.size()));

    for (auto &name : table_names) {
        auto *entry = catalog.GetTable(name);
        if (!entry) continue;

        // Write table name.
        WriteString(bm, offset, name);

        // Write column definitions.
        auto &cols = entry->GetColumns();
        WriteU32(bm, offset, static_cast<uint32_t>(cols.size()));
        for (auto &col : cols) {
            WriteString(bm, offset, col.name);
            WriteU8(bm, offset, static_cast<uint8_t>(col.type.id()));
        }

        // Write row data.
        auto &storage = entry->GetStorage();
        auto types = entry->GetTypes();
        WriteU64(bm, offset, storage.Count());

        auto state = storage.InitScan();
        DataChunk chunk;
        while (true) {
            chunk.Initialize(types);
            if (!storage.Scan(state, chunk)) break;
            for (idx_t row = 0; row < chunk.size(); row++) {
                for (idx_t col = 0; col < types.size(); col++) {
                    WriteValue(bm, offset, chunk.GetValue(col, row), types[col]);
                }
            }
        }
    }

    // Update header with catalog info.
    FileHeader header;
    header.catalog_offset = 4096;
    header.catalog_size = offset - 4096;
    header.data_offset = 4096;
    bm.WriteHeader(header);
    bm.Sync();
}

// ============================================================================
// Load: deserialize entire database
// ============================================================================

void Checkpoint::Load(Catalog &catalog, const std::string &path) {
    BlockManager bm;
    bm.OpenFile(path);

    auto header = bm.ReadHeader();
    auto file_size = bm.GetFileSize();

    // Validate header fields.
    if (header.data_offset < sizeof(FileHeader) || header.data_offset > file_size)
        throw IOException(ErrorCode::CORRUPT_DATA, "Invalid data_offset in database file");

    idx_t offset = header.data_offset;

    // Read number of tables.
    auto num_tables = ReadU32(bm, offset);
    if (num_tables > 100000)
        throw IOException(ErrorCode::CORRUPT_DATA, "Unreasonable table count in database file");

    for (uint32_t t = 0; t < num_tables; t++) {
        // Read table name.
        auto table_name = ReadString(bm, offset);

        // Read column definitions.
        auto num_cols = ReadU32(bm, offset);
        std::vector<ColumnDefinition> cols;
        std::vector<LogicalType> types;
        for (uint32_t c = 0; c < num_cols; c++) {
            auto col_name = ReadString(bm, offset);
            auto type_id = static_cast<LogicalTypeId>(ReadU8(bm, offset));
            auto type = LogicalType(type_id);
            cols.emplace_back(col_name, type);
            types.push_back(type);
        }

        // Create table in catalog.
        auto &entry = catalog.CreateTable(table_name, std::move(cols));
        auto storage = std::make_shared<DataTable>(types);
        entry.SetStorage(storage);

        // Read row data.
        auto row_count = ReadU64(bm, offset);
        if (row_count > 1000000000ULL) // 1 billion row sanity limit
            throw IOException(ErrorCode::CORRUPT_DATA, "Unreasonable row count in database file");
        idx_t rows_read = 0;
        while (rows_read < row_count) {
            DataChunk chunk;
            chunk.Initialize(types);
            idx_t batch = std::min(VECTOR_SIZE, row_count - rows_read);
            for (idx_t row = 0; row < batch; row++) {
                for (idx_t col = 0; col < types.size(); col++) {
                    auto val = ReadValue(bm, offset, types[col]);
                    chunk.SetValue(col, row, val);
                }
            }
            storage->Append(chunk);
            rows_read += batch;
        }
    }
}

} // namespace slothdb
