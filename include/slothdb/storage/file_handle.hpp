#pragma once

#include "slothdb/common/constants.hpp"
#include <cstdio>
#include <string>
#include <memory>

namespace slothdb {

// Cross-platform file I/O abstraction.
class FileHandle {
public:
    FileHandle() : file_(nullptr) {}
    ~FileHandle();

    // No copy, allow move.
    FileHandle(const FileHandle &) = delete;
    FileHandle &operator=(const FileHandle &) = delete;
    FileHandle(FileHandle &&other) noexcept;
    FileHandle &operator=(FileHandle &&other) noexcept;

    // Open a file. Mode: "rb", "wb", "r+b", "w+b", etc.
    static FileHandle Open(const std::string &path, const std::string &mode);

    // Read/Write at a specific offset.
    void ReadAt(data_ptr_t buffer, idx_t size, idx_t offset);
    void WriteAt(const_data_ptr_t buffer, idx_t size, idx_t offset);

    // Get file size.
    idx_t GetFileSize();

    // Flush to disk.
    void Sync();

    // Check if open.
    bool IsOpen() const { return file_ != nullptr; }

    // Close.
    void Close();

private:
    FILE *file_;
    std::string path_;
};

} // namespace slothdb
