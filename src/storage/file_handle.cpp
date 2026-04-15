#include "slothdb/storage/file_handle.hpp"
#include "slothdb/common/exception.hpp"

namespace slothdb {

FileHandle::~FileHandle() {
    Close();
}

FileHandle::FileHandle(FileHandle &&other) noexcept
    : file_(other.file_), path_(std::move(other.path_)) {
    other.file_ = nullptr;
}

FileHandle &FileHandle::operator=(FileHandle &&other) noexcept {
    if (this != &other) {
        Close();
        file_ = other.file_;
        path_ = std::move(other.path_);
        other.file_ = nullptr;
    }
    return *this;
}

FileHandle FileHandle::Open(const std::string &path, const std::string &mode) {
    FileHandle handle;
    handle.path_ = path;
#ifdef _MSC_VER
    fopen_s(&handle.file_, path.c_str(), mode.c_str());
#else
    handle.file_ = fopen(path.c_str(), mode.c_str());
#endif
    if (!handle.file_) {
        throw IOException(ErrorCode::FILE_NOT_FOUND,
                           "Cannot open file: " + path);
    }
    return handle;
}

void FileHandle::ReadAt(data_ptr_t buffer, idx_t size, idx_t offset) {
    if (!file_) throw IOException("File not open for reading");
#ifdef _MSC_VER
    _fseeki64(file_, static_cast<__int64>(offset), SEEK_SET);
#else
    fseeko(file_, static_cast<off_t>(offset), SEEK_SET);
#endif
    auto read = fread(buffer, 1, size, file_);
    if (read != size) {
        throw IOException(ErrorCode::FILE_READ_ERROR,
                           "Read " + std::to_string(read) + " bytes, expected " + std::to_string(size));
    }
}

void FileHandle::WriteAt(const_data_ptr_t buffer, idx_t size, idx_t offset) {
    if (!file_) throw IOException("File not open for writing");
#ifdef _MSC_VER
    _fseeki64(file_, static_cast<__int64>(offset), SEEK_SET);
#else
    fseeko(file_, static_cast<off_t>(offset), SEEK_SET);
#endif
    auto written = fwrite(buffer, 1, size, file_);
    if (written != size) {
        throw IOException(ErrorCode::FILE_WRITE_ERROR,
                           "Wrote " + std::to_string(written) + " bytes, expected " + std::to_string(size));
    }
}

idx_t FileHandle::GetFileSize() {
    if (!file_) return 0;
#ifdef _MSC_VER
    _fseeki64(file_, 0, SEEK_END);
    auto size = _ftelli64(file_);
#else
    fseeko(file_, 0, SEEK_END);
    auto size = ftello(file_);
#endif
    return static_cast<idx_t>(size);
}

void FileHandle::Sync() {
    if (file_) fflush(file_);
}

void FileHandle::Close() {
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
}

} // namespace slothdb
