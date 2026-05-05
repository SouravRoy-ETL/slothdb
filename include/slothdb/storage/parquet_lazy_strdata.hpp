#pragma once

// Helper to lazily materialise ParquetColumnData::str_data when a caller
// asked to skip it but a PLAIN page forces full string bytes. Lives in its
// own translation unit so parquet.cpp's hot decode loops keep their existing
// .text layout.

#include "slothdb/storage/parquet.hpp"

namespace slothdb {

void MaterialiseStrDataLazy(ParquetColumnData &out,
                            const char *const *dict_ptr,
                            const uint32_t *dict_len,
                            uint32_t dict_size,
                            idx_t row_offset);

}  // namespace slothdb
