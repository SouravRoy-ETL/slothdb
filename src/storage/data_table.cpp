#include "slothdb/storage/data_table.hpp"
#include <algorithm>

namespace slothdb {

DataTable::DataTable(const std::vector<LogicalType> &types)
    : types_(types), total_rows_(0) {
}

void DataTable::Append(DataChunk &chunk) {
    std::lock_guard<std::mutex> guard(lock_);

    idx_t offset = 0;
    while (offset < chunk.size()) {
        // Get or create the last row group.
        if (row_groups_.empty() || row_groups_.back()->IsFull()) {
            row_groups_.push_back(
                std::make_unique<RowGroup>(types_, total_rows_));
        }

        auto &last_rg = row_groups_.back();
        idx_t appended = last_rg->Append(chunk, offset);
        offset += appended;
        total_rows_ += appended;
    }
}

void DataTable::AppendRowGroups(std::vector<std::unique_ptr<RowGroup>> groups) {
    std::lock_guard<std::mutex> guard(lock_);
    for (auto &g : groups) {
        if (!g || g->Count() == 0) continue;
        g->SetStartRow(total_rows_);
        total_rows_ += g->Count();
        row_groups_.push_back(std::move(g));
    }
}

TableScanState DataTable::InitScan() const {
    return TableScanState{0, 0};
}

bool DataTable::Scan(TableScanState &state, DataChunk &result) const {
    result.Reset();

    while (state.current_row_group < row_groups_.size()) {
        auto &rg = row_groups_[state.current_row_group];
        idx_t remaining = rg->Count() - state.current_row_in_group;
        if (remaining == 0) {
            state.current_row_group++;
            state.current_row_in_group = 0;
            continue;
        }

        idx_t scan_count = std::min(remaining, VECTOR_SIZE);
        rg->Scan(result, state.current_row_in_group, scan_count);
        state.current_row_in_group += scan_count;
        return true;
    }

    return false; // No more data.
}

std::unique_ptr<DataTable::ParallelScanState> DataTable::InitParallelScan() const {
    auto state = std::make_unique<ParallelScanState>();
    state->next_morsel.store(0);
    // Compute total morsels: each row group produces ceil(count/VECTOR_SIZE) morsels.
    idx_t total = 0;
    for (auto &rg : row_groups_) {
        total += (rg->Count() + VECTOR_SIZE - 1) / VECTOR_SIZE;
    }
    state->total_morsels = total;
    return state;
}

bool DataTable::ParallelScan(ParallelScanState &state, DataChunk &result) const {
    while (true) {
        idx_t morsel_idx = state.next_morsel.fetch_add(1);
        if (morsel_idx >= state.total_morsels) return false;

        // Find which row group and offset this morsel maps to.
        idx_t current = 0;
        for (auto &rg : row_groups_) {
            idx_t rg_morsels = (rg->Count() + VECTOR_SIZE - 1) / VECTOR_SIZE;
            if (morsel_idx < current + rg_morsels) {
                idx_t local_morsel = morsel_idx - current;
                idx_t start = local_morsel * VECTOR_SIZE;
                idx_t scan_count = std::min(VECTOR_SIZE, rg->Count() - start);
                if (scan_count == 0) continue;

                result.Initialize(types_);
                rg->Scan(result, start, scan_count);
                return true;
            }
            current += rg_morsels;
        }
        return false;
    }
}

} // namespace slothdb
