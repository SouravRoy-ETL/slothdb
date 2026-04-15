#pragma once

#include "slothdb/storage/block_manager.hpp"
#include "slothdb/catalog/catalog.hpp"
#include "slothdb/storage/data_table.hpp"
#include <string>

namespace slothdb {

// Checkpoint: serialize/deserialize the entire database to/from a .slothdb file.
class Checkpoint {
public:
    // Write the entire database to a file.
    static void Save(Catalog &catalog, const std::string &path);

    // Load a database from a file.
    static void Load(Catalog &catalog, const std::string &path);

private:
    // Serialization helpers.
    static void WriteString(BlockManager &bm, idx_t &offset, const std::string &str);
    static std::string ReadString(BlockManager &bm, idx_t &offset);
    static void WriteU32(BlockManager &bm, idx_t &offset, uint32_t val);
    static uint32_t ReadU32(BlockManager &bm, idx_t &offset);
    static void WriteU64(BlockManager &bm, idx_t &offset, uint64_t val);
    static uint64_t ReadU64(BlockManager &bm, idx_t &offset);
    static void WriteI32(BlockManager &bm, idx_t &offset, int32_t val);
    static int32_t ReadI32(BlockManager &bm, idx_t &offset);
    static void WriteI64(BlockManager &bm, idx_t &offset, int64_t val);
    static int64_t ReadI64(BlockManager &bm, idx_t &offset);
    static void WriteDouble(BlockManager &bm, idx_t &offset, double val);
    static double ReadDouble(BlockManager &bm, idx_t &offset);
    static void WriteU8(BlockManager &bm, idx_t &offset, uint8_t val);
    static uint8_t ReadU8(BlockManager &bm, idx_t &offset);

    // Serialize/deserialize a Value.
    static void WriteValue(BlockManager &bm, idx_t &offset, const Value &val,
                            const LogicalType &type);
    static Value ReadValue(BlockManager &bm, idx_t &offset, const LogicalType &type);
};

} // namespace slothdb
