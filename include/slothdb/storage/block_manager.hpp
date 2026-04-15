#pragma once

#include "slothdb/common/constants.hpp"
#include "slothdb/storage/file_handle.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace slothdb {

// File format constants.
static constexpr uint32_t SLOTHDB_MAGIC = 0x534C4442; // "LTDB"
static constexpr uint32_t SLOTHDB_VERSION = 1;

// File header — stored in the first 64 bytes.
struct FileHeader {
    uint32_t magic = SLOTHDB_MAGIC;
    uint32_t version = SLOTHDB_VERSION;
    uint32_t block_size = BLOCK_SIZE;
    uint32_t reserved = 0;
    uint64_t catalog_offset = 0;    // Byte offset of catalog data.
    uint64_t catalog_size = 0;      // Size of catalog data in bytes.
    uint64_t total_blocks = 0;
    uint64_t data_offset = 0;       // Byte offset where row data starts.
};

static_assert(sizeof(FileHeader) <= 64, "FileHeader must fit in 64 bytes");

// Manages block-level I/O to a .slothdb file.
class BlockManager {
public:
    BlockManager() = default;

    // Create a new database file.
    void CreateFile(const std::string &path);

    // Open an existing database file.
    void OpenFile(const std::string &path);

    // Write the file header.
    void WriteHeader(const FileHeader &header);

    // Read the file header.
    FileHeader ReadHeader();

    // Write raw data at an offset.
    void WriteData(const_data_ptr_t data, idx_t size, idx_t offset);

    // Read raw data at an offset.
    void ReadData(data_ptr_t data, idx_t size, idx_t offset);

    // Get file size.
    idx_t GetFileSize();

    // Flush.
    void Sync();

    bool IsOpen() const { return file_.IsOpen(); }

private:
    FileHandle file_;
    std::string path_;
};

} // namespace slothdb
