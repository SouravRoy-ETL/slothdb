#include "slothdb/storage/block_manager.hpp"
#include "slothdb/common/exception.hpp"
#include <cstring>

namespace slothdb {

void BlockManager::CreateFile(const std::string &path) {
    path_ = path;
    file_ = FileHandle::Open(path, "w+b");

    // Write initial header.
    FileHeader header;
    header.data_offset = 4096; // Reserve 4KB for header area.
    WriteHeader(header);
}

void BlockManager::OpenFile(const std::string &path) {
    path_ = path;
    file_ = FileHandle::Open(path, "r+b");

    auto header = ReadHeader();
    if (header.magic != SLOTHDB_MAGIC) {
        throw IOException(ErrorCode::CORRUPT_DATA,
                           "Invalid database file: bad magic number");
    }
    if (header.version != SLOTHDB_VERSION) {
        throw IOException(ErrorCode::CORRUPT_DATA,
                           "Unsupported database version: " + std::to_string(header.version));
    }
}

void BlockManager::WriteHeader(const FileHeader &header) {
    file_.WriteAt(reinterpret_cast<const_data_ptr_t>(&header), sizeof(FileHeader), 0);
    file_.Sync();
}

FileHeader BlockManager::ReadHeader() {
    FileHeader header;
    file_.ReadAt(reinterpret_cast<data_ptr_t>(&header), sizeof(FileHeader), 0);
    return header;
}

void BlockManager::WriteData(const_data_ptr_t data, idx_t size, idx_t offset) {
    file_.WriteAt(data, size, offset);
}

void BlockManager::ReadData(data_ptr_t data, idx_t size, idx_t offset) {
    file_.ReadAt(data, size, offset);
}

idx_t BlockManager::GetFileSize() {
    return file_.GetFileSize();
}

void BlockManager::Sync() {
    file_.Sync();
}

} // namespace slothdb
