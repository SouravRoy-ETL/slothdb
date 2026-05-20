#include "slothdb/common/types/vector.hpp"
#include "slothdb/common/enums.hpp"
#include "slothdb/common/exception.hpp"
#include <cstring>

namespace slothdb {

// --- VectorStringBuffer ---

const char *VectorStringBuffer::AddString(const char *data, idx_t len) {
    strings_.emplace_back(data, len);
    return strings_.back().c_str();
}

const char *VectorStringBuffer::AddString(const std::string &str) {
    strings_.push_back(str);
    return strings_.back().c_str();
}

// --- Vector ---

Vector::Vector(const LogicalType &type, idx_t capacity)
    : vector_type_(VectorType::FLAT), type_(type), data_(nullptr) {
    Initialize(capacity);
}

Vector::Vector(const LogicalType &type, data_ptr_t data)
    : vector_type_(VectorType::FLAT), type_(type), data_(data) {
}

Vector::Vector(Vector &&other) noexcept
    : vector_type_(other.vector_type_), type_(std::move(other.type_)),
      data_(other.data_), validity_(std::move(other.validity_)),
      buffer_(std::move(other.buffer_)), auxiliary_(std::move(other.auxiliary_)) {
    other.data_ = nullptr;
}

Vector &Vector::operator=(Vector &&other) noexcept {
    if (this != &other) {
        vector_type_ = other.vector_type_;
        type_ = std::move(other.type_);
        data_ = other.data_;
        validity_ = std::move(other.validity_);
        buffer_ = std::move(other.buffer_);
        auxiliary_ = std::move(other.auxiliary_);
        other.data_ = nullptr;
    }
    return *this;
}

void Vector::Initialize(idx_t capacity) {
    auto physical = type_.GetInternalType();
    idx_t type_size = GetTypeIdSize(physical);
    if (type_size > 0) {
        auto child_buf = std::make_shared<VectorChildBuffer>(capacity * type_size);
        buffer_ = child_buf;
        data_ = child_buf->GetData();
    }
    if (physical == PhysicalType::VARCHAR) {
        auxiliary_ = std::make_shared<VectorStringBuffer>();
    }
}

void Vector::SetValue(idx_t index, const Value &val) {
    if (val.IsNull()) {
        validity_.SetInvalid(index);
        return;
    }
    // SQLNULL-typed Vector can only hold NULL — anything else is a
    // non-NULL Value trying to flow into a NULL-typed slot. Either the
    // type was inferred wrong upstream (VALUES (NULL) for example
    // produces SQLNULL columns until widened by a set-op or cast) or
    // the caller has a bug. Treat as a NULL write so downstream paths
    // don't crash; the upstream type-inference is the proper long-term
    // fix and an unrelated piece of work.
    if (type_.id() == LogicalTypeId::SQLNULL) {
        validity_.SetInvalid(index);
        return;
    }
    validity_.SetValid(index);

    auto physical = type_.GetInternalType();
    switch (physical) {
    case PhysicalType::BOOL:
        GetData<bool>()[index] = val.GetValue<bool>();
        break;
    case PhysicalType::INT8:
        GetData<int8_t>()[index] = val.GetValue<int8_t>();
        break;
    case PhysicalType::INT16:
        GetData<int16_t>()[index] = val.GetValue<int16_t>();
        break;
    case PhysicalType::INT32:
        GetData<int32_t>()[index] = val.GetValue<int32_t>();
        break;
    case PhysicalType::INT64:
        GetData<int64_t>()[index] = val.GetValue<int64_t>();
        break;
    case PhysicalType::INT128:
        GetData<hugeint_t>()[index] = val.GetValue<hugeint_t>();
        break;
    case PhysicalType::UINT8:
        GetData<uint8_t>()[index] = val.GetValue<uint8_t>();
        break;
    case PhysicalType::UINT16:
        GetData<uint16_t>()[index] = val.GetValue<uint16_t>();
        break;
    case PhysicalType::UINT32:
        GetData<uint32_t>()[index] = val.GetValue<uint32_t>();
        break;
    case PhysicalType::UINT64:
        GetData<uint64_t>()[index] = val.GetValue<uint64_t>();
        break;
    case PhysicalType::FLOAT:
        GetData<float>()[index] = val.GetValue<float>();
        break;
    case PhysicalType::DOUBLE:
        GetData<double>()[index] = val.GetValue<double>();
        break;
    case PhysicalType::VARCHAR: {
        auto str = val.GetValue<std::string>();
        auto &str_buf = GetStringBuffer();
        if (str.size() <= string_t::INLINE_LENGTH) {
            GetData<string_t>()[index] = string_t(str);
        } else {
            const char *heap_ptr = str_buf.AddString(str);
            GetData<string_t>()[index] = string_t(heap_ptr, static_cast<uint32_t>(str.size()));
        }
        break;
    }
    default:
        throw NotImplementedException("SetValue for type " + type_.ToString());
    }
}

Value Vector::GetValue(idx_t index) const {
    if (!validity_.RowIsValid(index)) {
        return Value();
    }
    // SQLNULL-typed Vector: every row IS NULL regardless of validity bit.
    // The underlying data buffer is undefined (or absent), so reading via
    // typed dispatch would access garbage / unmapped memory. Return NULL
    // unconditionally. Fixes:
    //   SELECT x, COUNT(*) FROM (VALUES (NULL),(NULL)) t(x) GROUP BY x
    // which previously crashed when the GROUP BY tried to extract the
    // key value from the SQLNULL column.
    if (type_.id() == LogicalTypeId::SQLNULL) {
        return Value();
    }

    // Logical-type-aware path: keep DATE/TIMESTAMP/TIME annotated
    // so ToString renders ISO strings instead of raw integers.
    switch (type_.id()) {
    case LogicalTypeId::DATE:
        return Value::DATE(GetData<int32_t>()[index]);
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIMESTAMP_TZ:
        return Value::TIMESTAMP(GetData<int64_t>()[index]);
    case LogicalTypeId::TIME:
        return Value::TIME(GetData<int64_t>()[index]);
    default:
        break;
    }

    auto physical = type_.GetInternalType();
    switch (physical) {
    case PhysicalType::BOOL:
        return Value::BOOLEAN(GetData<bool>()[index]);
    case PhysicalType::INT8:
        return Value::TINYINT(GetData<int8_t>()[index]);
    case PhysicalType::INT16:
        return Value::SMALLINT(GetData<int16_t>()[index]);
    case PhysicalType::INT32:
        return Value::INTEGER(GetData<int32_t>()[index]);
    case PhysicalType::INT64:
        return Value::BIGINT(GetData<int64_t>()[index]);
    case PhysicalType::INT128:
        return Value::HUGEINT(GetData<hugeint_t>()[index]);
    case PhysicalType::UINT8:
        return Value::UTINYINT(GetData<uint8_t>()[index]);
    case PhysicalType::UINT16:
        return Value::USMALLINT(GetData<uint16_t>()[index]);
    case PhysicalType::UINT32:
        return Value::UINTEGER(GetData<uint32_t>()[index]);
    case PhysicalType::UINT64:
        return Value::UBIGINT(GetData<uint64_t>()[index]);
    case PhysicalType::FLOAT:
        return Value::FLOAT(GetData<float>()[index]);
    case PhysicalType::DOUBLE:
        return Value::DOUBLE(GetData<double>()[index]);
    case PhysicalType::VARCHAR: {
        auto &str = GetData<string_t>()[index];
        return Value::VARCHAR(str.GetString());
    }
    default:
        throw NotImplementedException("GetValue for type " + type_.ToString());
    }
}

void Vector::Flatten(idx_t count) {
    if (vector_type_ == VectorType::FLAT) return;

    if (vector_type_ == VectorType::CONSTANT) {
        auto physical = type_.GetInternalType();
        idx_t type_size = GetTypeIdSize(physical);

        bool is_null = !validity_.RowIsValid(0);

        // Save the constant value.
        std::vector<data_t> const_val(type_size);
        if (!is_null && type_size > 0) {
            std::memcpy(const_val.data(), data_, type_size);
        }

        // Save string if needed.
        std::string const_str;
        if (!is_null && physical == PhysicalType::VARCHAR) {
            const_str = GetData<string_t>()[0].GetString();
        }

        // Re-initialize as flat.
        Initialize(count);
        vector_type_ = VectorType::FLAT;

        if (is_null) {
            for (idx_t i = 0; i < count; i++) {
                validity_.SetInvalid(i);
            }
        } else {
            if (physical == PhysicalType::VARCHAR) {
                auto &str_buf = GetStringBuffer();
                for (idx_t i = 0; i < count; i++) {
                    if (const_str.size() <= string_t::INLINE_LENGTH) {
                        GetData<string_t>()[i] = string_t(const_str);
                    } else {
                        const char *heap_ptr = str_buf.AddString(const_str);
                        GetData<string_t>()[i] = string_t(heap_ptr, static_cast<uint32_t>(const_str.size()));
                    }
                }
            } else {
                for (idx_t i = 0; i < count; i++) {
                    std::memcpy(data_ + i * type_size, const_val.data(), type_size);
                }
            }
        }
    }
}

void Vector::Reference(Vector &other) {
    vector_type_ = other.vector_type_;
    type_ = other.type_;
    data_ = other.data_;
    validity_ = other.validity_;
    buffer_ = other.buffer_;
    auxiliary_ = other.auxiliary_;
}

VectorStringBuffer &Vector::GetStringBuffer() {
    if (!auxiliary_) {
        auxiliary_ = std::make_shared<VectorStringBuffer>();
    }
    return static_cast<VectorStringBuffer &>(*auxiliary_);
}

} // namespace slothdb
