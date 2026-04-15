#include "slothdb/execution/gpu/gpu_engine.hpp"
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <cstring>

namespace slothdb {

// ============================================================================
// CPU Fallback Engine
// Uses optimized CPU algorithms as baseline.
// When CUDA or Metal is available, those implementations override these.
// ============================================================================

class CPUBuffer : public GPUBuffer {
public:
    explicit CPUBuffer(size_t size) : data_(size) {}
    void CopyToDevice(const void *host_data, size_t size) override {
        std::memcpy(data_.data(), host_data, std::min(size, data_.size()));
    }
    void CopyToHost(void *host_data, size_t size) override {
        std::memcpy(host_data, data_.data(), std::min(size, data_.size()));
    }
    size_t Size() const override { return data_.size(); }
private:
    std::vector<uint8_t> data_;
};

GPUDeviceInfo CPUFallbackEngine::GetDeviceInfo() {
    GPUDeviceInfo info;
    info.backend = GPUBackend::NONE;
    info.name = "CPU Fallback";
    info.available = true;
    return info;
}

std::unique_ptr<GPUBuffer> CPUFallbackEngine::AllocateBuffer(size_t size) {
    return std::make_unique<CPUBuffer>(size);
}

std::vector<idx_t> CPUFallbackEngine::Filter(const int32_t *data, idx_t count,
                                               const std::string &op, int32_t value) {
    std::vector<idx_t> result;
    result.reserve(count / 4); // Estimate 25% selectivity.

    for (idx_t i = 0; i < count; i++) {
        bool match = false;
        if (op == "=") match = data[i] == value;
        else if (op == "!=") match = data[i] != value;
        else if (op == "<") match = data[i] < value;
        else if (op == ">") match = data[i] > value;
        else if (op == "<=") match = data[i] <= value;
        else if (op == ">=") match = data[i] >= value;
        if (match) result.push_back(i);
    }
    return result;
}

int64_t CPUFallbackEngine::SumInt32(const int32_t *data, idx_t count) {
    int64_t sum = 0;
    // Unrolled loop for better CPU utilization.
    idx_t i = 0;
    for (; i + 4 <= count; i += 4) {
        sum += data[i] + data[i + 1] + data[i + 2] + data[i + 3];
    }
    for (; i < count; i++) sum += data[i];
    return sum;
}

int64_t CPUFallbackEngine::SumInt64(const int64_t *data, idx_t count) {
    int64_t sum = 0;
    for (idx_t i = 0; i < count; i++) sum += data[i];
    return sum;
}

double CPUFallbackEngine::SumDouble(const double *data, idx_t count) {
    double sum = 0;
    // Kahan summation for better precision.
    double c = 0;
    for (idx_t i = 0; i < count; i++) {
        double y = data[i] - c;
        double t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return sum;
}

std::vector<idx_t> CPUFallbackEngine::SortIndices(const int32_t *data, idx_t count,
                                                     bool ascending) {
    std::vector<idx_t> indices(count);
    std::iota(indices.begin(), indices.end(), 0);
    if (ascending) {
        std::sort(indices.begin(), indices.end(),
                  [data](idx_t a, idx_t b) { return data[a] < data[b]; });
    } else {
        std::sort(indices.begin(), indices.end(),
                  [data](idx_t a, idx_t b) { return data[a] > data[b]; });
    }
    return indices;
}

std::vector<idx_t> CPUFallbackEngine::SortIndices(const int64_t *data, idx_t count,
                                                     bool ascending) {
    std::vector<idx_t> indices(count);
    std::iota(indices.begin(), indices.end(), 0);
    if (ascending) {
        std::sort(indices.begin(), indices.end(),
                  [data](idx_t a, idx_t b) { return data[a] < data[b]; });
    } else {
        std::sort(indices.begin(), indices.end(),
                  [data](idx_t a, idx_t b) { return data[a] > data[b]; });
    }
    return indices;
}

std::vector<idx_t> CPUFallbackEngine::SortIndices(const double *data, idx_t count,
                                                     bool ascending) {
    std::vector<idx_t> indices(count);
    std::iota(indices.begin(), indices.end(), 0);
    if (ascending) {
        std::sort(indices.begin(), indices.end(),
                  [data](idx_t a, idx_t b) { return data[a] < data[b]; });
    } else {
        std::sort(indices.begin(), indices.end(),
                  [data](idx_t a, idx_t b) { return data[a] > data[b]; });
    }
    return indices;
}

GPUEngine::AggResult CPUFallbackEngine::HashAggregate(const int32_t *keys,
                                                        const int32_t *values,
                                                        idx_t count) {
    AggResult result;
    std::unordered_map<int32_t, std::pair<int64_t, int64_t>> agg; // key -> (sum, count)

    for (idx_t i = 0; i < count; i++) {
        auto &entry = agg[keys[i]];
        entry.first += values[i];
        entry.second++;
    }

    for (auto &[key, val] : agg) {
        result.keys.push_back(key);
        result.sums.push_back(val.first);
        result.counts.push_back(val.second);
    }
    return result;
}

// ============================================================================
// GPU Engine Factory
// ============================================================================

// Try to detect and load CUDA or Metal at runtime.
// For now, return CPU fallback.
// When CUDA toolkit is available, we'll load the CUDA implementation.
// When on macOS, we'll load the Metal implementation.

bool GPUEngine::IsAvailable() {
#if defined(SLOTHDB_CUDA)
    return true; // CUDA available at compile time.
#elif defined(SLOTHDB_METAL)
    return true; // Metal available at compile time.
#else
    return false; // No GPU SDK available.
#endif
}

std::unique_ptr<GPUEngine> GPUEngine::Create() {
#if defined(SLOTHDB_CUDA)
    // TODO: Return CUDAEngine when CUDA SDK is linked.
    return std::make_unique<CPUFallbackEngine>();
#elif defined(SLOTHDB_METAL)
    // TODO: Return MetalEngine when Metal framework is linked.
    return std::make_unique<CPUFallbackEngine>();
#else
    return std::make_unique<CPUFallbackEngine>();
#endif
}

} // namespace slothdb
