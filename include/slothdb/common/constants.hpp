#pragma once

#include <cstdint>
#include <cstddef>

namespace slothdb {

using idx_t = uint64_t;
using sel_t = uint32_t;
using data_t = uint8_t;
using data_ptr_t = data_t *;
using const_data_ptr_t = const data_t *;
using validity_t = uint64_t;
using hash_t = uint64_t;

static constexpr idx_t VECTOR_SIZE = 2048;
static constexpr idx_t STANDARD_VECTOR_SIZE = VECTOR_SIZE;
static constexpr idx_t ROW_GROUP_SIZE = 122880;
static constexpr idx_t BLOCK_SIZE = 262144; // 256 KB

static constexpr idx_t INVALID_INDEX = static_cast<idx_t>(-1);

} // namespace slothdb
