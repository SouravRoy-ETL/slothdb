#include "slothdb/storage/parquet_lazy_strdata.hpp"

namespace slothdb {

void MaterialiseStrDataLazy(ParquetColumnData &out,
                            const char *const *dict_ptr,
                            const uint32_t *dict_len,
                            uint32_t dict_size,
                            idx_t row_offset) {
    out.str_data.resize(out.count);
    if (out.str_dict_encoded && !out.str_dict_indices.empty() && row_offset > 0) {
        const uint32_t *bidx = out.str_dict_indices.data();
        for (idx_t i = 0; i < row_offset; i++) {
            uint32_t di = bidx[i];
            out.str_data[i] = (di < dict_size) ? string_t(dict_ptr[di], dict_len[di])
                                                : string_t();
        }
    }
    out.str_data_skipped = false;
}

}  // namespace slothdb
