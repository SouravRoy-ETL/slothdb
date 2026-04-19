#include "slothdb/storage/column_data.hpp"
#include "slothdb/common/enums.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/types/string_type.hpp"
#include <cstring>

namespace slothdb {

ColumnData::ColumnData(const LogicalType &type, idx_t capacity)
    : type_(type), capacity_(capacity), count_(0) {
    idx_t type_size = TypeSize();
    if (type_size > 0) {
        data_ = std::make_unique<data_t[]>(capacity * type_size);
    }
}

idx_t ColumnData::TypeSize() const {
    return GetTypeIdSize(type_.GetInternalType());
}

void ColumnData::AppendSlice(Vector &source, idx_t offset, idx_t count) {
    if (count_ + count > capacity_) {
        throw InternalException("ColumnData::AppendSlice exceeds capacity");
    }
    auto physical = type_.GetInternalType();
    idx_t type_size = TypeSize();
    if (physical == PhysicalType::VARCHAR) {
        // Zero-copy: memcpy the string_t slice and hold the source's string
        // buffer via shared_ptr so the pointers inside string_t stay valid.
        auto *src_strs = source.GetData<string_t>();
        auto *dst_strs = reinterpret_cast<string_t *>(data_.get());
        std::memcpy(dst_strs + count_, src_strs + offset, count * sizeof(string_t));
        if (auto aux = source.GetAuxiliaryPtr()) {
            held_bufs_.push_back(std::move(aux));
        }
    } else if (type_size > 0) {
        auto *src_data = source.GetData();
        auto *dst_data = data_.get() + count_ * type_size;
        std::memcpy(dst_data, src_data + offset * type_size, count * type_size);
    }
    if (!source.GetValidity().AllValid()) {
        for (idx_t i = 0; i < count; i++) {
            if (!source.GetValidity().RowIsValid(offset + i)) {
                validity_.SetInvalid(count_ + i);
            }
        }
    }
    count_ += count;
}

void ColumnData::Append(Vector &source, idx_t source_count) {
    if (count_ + source_count > capacity_) {
        throw InternalException("ColumnData::Append exceeds capacity");
    }

    auto physical = type_.GetInternalType();
    idx_t type_size = TypeSize();

    if (physical == PhysicalType::VARCHAR) {
        auto *src_data = source.GetData<string_t>();
        auto *dst_data = reinterpret_cast<string_t *>(data_.get());

        for (idx_t i = 0; i < source_count; i++) {
            if (!source.GetValidity().RowIsValid(i)) {
                validity_.SetInvalid(count_ + i);
                dst_data[count_ + i] = string_t();
            } else {
                // Copy string to our heap.
                std::string str_val = src_data[i].GetString();
                string_heap_.push_back(str_val);
                const char *heap_ptr = string_heap_.back().c_str();
                dst_data[count_ + i] = string_t(heap_ptr, static_cast<uint32_t>(str_val.size()));
            }
        }
    } else if (type_size > 0) {
        auto *src_data = source.GetData();
        auto *dst_data = data_.get() + count_ * type_size;
        std::memcpy(dst_data, src_data, source_count * type_size);

        // Copy validity.
        if (!source.GetValidity().AllValid()) {
            for (idx_t i = 0; i < source_count; i++) {
                if (!source.GetValidity().RowIsValid(i)) {
                    validity_.SetInvalid(count_ + i);
                }
            }
        }
    }

    count_ += source_count;
}

void ColumnData::Scan(Vector &target, idx_t start, idx_t scan_count) const {
    auto physical = type_.GetInternalType();
    idx_t type_size = TypeSize();

    if (physical == PhysicalType::VARCHAR) {
        auto *src_data = reinterpret_cast<const string_t *>(data_.get());
        for (idx_t i = 0; i < scan_count; i++) {
            if (!validity_.RowIsValid(start + i)) {
                target.GetValidity().SetInvalid(i);
            } else {
                auto &src_str = src_data[start + i];
                std::string str_val = src_str.GetString();
                if (str_val.size() <= string_t::INLINE_LENGTH) {
                    target.GetData<string_t>()[i] = string_t(str_val);
                } else {
                    const char *heap_ptr = target.GetStringBuffer().AddString(str_val);
                    target.GetData<string_t>()[i] = string_t(heap_ptr, static_cast<uint32_t>(str_val.size()));
                }
            }
        }
    } else if (type_size > 0) {
        auto *src_data = data_.get() + start * type_size;
        std::memcpy(target.GetData(), src_data, scan_count * type_size);

        // Copy validity.
        if (!validity_.AllValid()) {
            for (idx_t i = 0; i < scan_count; i++) {
                if (!validity_.RowIsValid(start + i)) {
                    target.GetValidity().SetInvalid(i);
                }
            }
        }
    }
}

} // namespace slothdb
