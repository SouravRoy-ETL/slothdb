#include "slothdb/execution/q4_dict_histogram.hpp"
#include "slothdb/storage/parquet.hpp"

#include <atomic>
#include <thread>
#include <vector>

namespace slothdb {

bool TryQ4DictHistogram(ParquetReader &reader, idx_t col_idx,
                        int64_t &out_count, double &out_sum) {
    const auto &meta = reader.GetMeta();
    const idx_t num_rgs = static_cast<idx_t>(meta.row_groups.size());
    if (num_rgs == 0) {
        out_count = 0;
        out_sum = 0.0;
        return true;
    }

    // Quick gate: every RG must have a dict page on this column. Skip the
    // parallel spawn cost when we'd just bail on the first RG.
    for (idx_t rg = 0; rg < num_rgs; rg++) {
        if (col_idx >= meta.row_groups[rg].columns.size()) return false;
        const auto &cmeta = meta.row_groups[rg].columns[col_idx];
        if (cmeta.parquet_type != ParquetType::INT64) return false;
        if (cmeta.dict_page_offset < 0) return false;
    }

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    if (hw > 8) hw = 8;
    unsigned int nt = hw;
    if ((idx_t)nt > num_rgs) nt = static_cast<unsigned int>(num_rgs);

    std::vector<int64_t> tl_count(nt, 0);
    std::vector<double>  tl_sum(nt, 0.0);
    std::atomic<idx_t> next{0};
    std::atomic<bool> abort_flag{false};

    auto worker = [&](unsigned int tid) {
        while (true) {
            if (abort_flag.load(std::memory_order_relaxed)) return;
            idx_t rg = next.fetch_add(1, std::memory_order_relaxed);
            if (rg >= num_rgs) return;
            int64_t c = 0;
            double s = 0.0;
            if (!reader.DecodeBigintColumnHistogram(rg, col_idx, c, s)) {
                abort_flag.store(true, std::memory_order_relaxed);
                return;
            }
            tl_count[tid] += c;
            tl_sum[tid] += s;
        }
    };

    if (nt <= 1) {
        worker(0);
    } else {
        std::vector<std::thread> threads;
        threads.reserve(nt);
        for (unsigned int t = 0; t < nt; t++) {
            threads.emplace_back(worker, t);
        }
        for (auto &th : threads) th.join();
    }

    if (abort_flag.load()) return false;

    int64_t total_count = 0;
    double total_sum = 0.0;
    for (unsigned int t = 0; t < nt; t++) {
        total_count += tl_count[t];
        total_sum += tl_sum[t];
    }
    out_count = total_count;
    out_sum = total_sum;
    return true;
}

} // namespace slothdb
