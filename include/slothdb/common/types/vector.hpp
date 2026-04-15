#pragma once

#include "slothdb/common/constants.hpp"
#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/validity_mask.hpp"
#include "slothdb/common/types/string_type.hpp"
#include "slothdb/common/types/value.hpp"
#include <memory>
#include <vector>

namespace slothdb {

enum class VectorType : uint8_t {
    FLAT,
    CONSTANT,
    DICTIONARY,
    SEQUENCE
};

// Base class for memory buffers owned by a Vector.
class VectorBuffer {
public:
    virtual ~VectorBuffer() = default;
};

// Owns a flat data buffer for a vector.
class VectorChildBuffer : public VectorBuffer {
public:
    explicit VectorChildBuffer(idx_t size) : data_(std::make_unique<data_t[]>(size)) {}
    data_ptr_t GetData() { return data_.get(); }

private:
    std::unique_ptr<data_t[]> data_;
};

// Owns the overflow heap for long strings in a VARCHAR vector.
class VectorStringBuffer : public VectorBuffer {
public:
    // Add a string to the heap and return a pointer to it.
    const char *AddString(const char *data, idx_t len);
    const char *AddString(const std::string &str);

private:
    // Store strings so they stay alive.
    std::vector<std::string> strings_;
};

// The core columnar data vector. Holds VECTOR_SIZE values of a single type.
class Vector {
public:
    // Construct an empty vector of the given type.
    explicit Vector(const LogicalType &type, idx_t capacity = VECTOR_SIZE);

    // Construct a flat vector wrapping external data (no ownership).
    Vector(const LogicalType &type, data_ptr_t data);

    ~Vector() = default;

    // No copy, allow move.
    Vector(const Vector &) = delete;
    Vector &operator=(const Vector &) = delete;
    Vector(Vector &&other) noexcept;
    Vector &operator=(Vector &&other) noexcept;

    // Accessors.
    VectorType GetVectorType() const { return vector_type_; }
    const LogicalType &GetType() const { return type_; }
    data_ptr_t GetData() { return data_; }
    const_data_ptr_t GetData() const { return data_; }
    ValidityMask &GetValidity() { return validity_; }
    const ValidityMask &GetValidity() const { return validity_; }

    void SetVectorType(VectorType type) { vector_type_ = type; }

    // Typed data access.
    template <class T>
    T *GetData() {
        return reinterpret_cast<T *>(data_);
    }

    template <class T>
    const T *GetData() const {
        return reinterpret_cast<const T *>(data_);
    }

    // Get/set a single value (slow path, for initialization and testing).
    Value GetValue(idx_t index) const;
    void SetValue(idx_t index, const Value &val);

    // Convert to FLAT representation.
    void Flatten(idx_t count);

    // Reference another vector (shallow, no copy).
    void Reference(Vector &other);

    // Get the string buffer (for VARCHAR vectors).
    VectorStringBuffer &GetStringBuffer();

private:
    void Initialize(idx_t capacity);

    VectorType vector_type_;
    LogicalType type_;
    data_ptr_t data_;
    ValidityMask validity_;

    // Owns the data buffer.
    std::shared_ptr<VectorBuffer> buffer_;
    // Owns auxiliary data (string heap, child vectors, etc.)
    std::shared_ptr<VectorBuffer> auxiliary_;
};

} // namespace slothdb
