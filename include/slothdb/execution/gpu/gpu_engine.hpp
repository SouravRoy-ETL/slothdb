#pragma once

#include "slothdb/common/constants.hpp"
#include "slothdb/common/types/value.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace slothdb {

// GPU backend type.
enum class GPUBackend : uint8_t {
    NONE = 0,     // No GPU available - CPU fallback.
    CUDA = 1,     // NVIDIA CUDA.
    METAL = 2,    // Apple Metal.
    OPENCL = 3,   // OpenCL (fallback for other GPUs).
};

// GPU device information.
struct GPUDeviceInfo {
    GPUBackend backend = GPUBackend::NONE;
    std::string name;
    uint64_t memory_bytes = 0;
    int compute_units = 0;
    bool available = false;
};

// GPU buffer: a block of memory on the GPU device.
class GPUBuffer {
public:
    virtual ~GPUBuffer() = default;
    virtual void CopyToDevice(const void *host_data, size_t size) = 0;
    virtual void CopyToHost(void *host_data, size_t size) = 0;
    virtual size_t Size() const = 0;
};

// Abstract GPU execution engine.
// Concrete implementations for CUDA and Metal.
class GPUEngine {
public:
    virtual ~GPUEngine() = default;

    // Detect available GPU.
    virtual GPUDeviceInfo GetDeviceInfo() = 0;

    // Allocate GPU buffer.
    virtual std::unique_ptr<GPUBuffer> AllocateBuffer(size_t size) = 0;

    // ========================================================================
    // GPU-accelerated operations.
    // Each takes input data on host, processes on GPU, returns results on host.
    // Falls back to CPU if GPU is not available.
    // ========================================================================

    // Parallel filter: returns indices where predicate is true.
    virtual std::vector<idx_t> Filter(const int32_t *data, idx_t count,
                                       const std::string &op, int32_t value) = 0;

    // Parallel aggregation: SUM of int32 array.
    virtual int64_t SumInt32(const int32_t *data, idx_t count) = 0;

    // Parallel aggregation: SUM of int64 array.
    virtual int64_t SumInt64(const int64_t *data, idx_t count) = 0;

    // Parallel aggregation: SUM of double array.
    virtual double SumDouble(const double *data, idx_t count) = 0;

    // Parallel sort: returns sorted indices.
    virtual std::vector<idx_t> SortIndices(const int32_t *data, idx_t count,
                                            bool ascending) = 0;
    virtual std::vector<idx_t> SortIndices(const int64_t *data, idx_t count,
                                            bool ascending) = 0;
    virtual std::vector<idx_t> SortIndices(const double *data, idx_t count,
                                            bool ascending) = 0;

    // Hash aggregation: group by int32 keys, SUM int32 values.
    struct AggResult {
        std::vector<int32_t> keys;
        std::vector<int64_t> sums;
        std::vector<int64_t> counts;
    };
    virtual AggResult HashAggregate(const int32_t *keys, const int32_t *values,
                                     idx_t count) = 0;

    // Factory: create the best available GPU engine.
    static std::unique_ptr<GPUEngine> Create();

    // Check if GPU is available.
    static bool IsAvailable();

    // Get the minimum data size (rows) to use GPU (below this, CPU is faster).
    static constexpr idx_t GPU_THRESHOLD = 100000;
};

// CPU fallback engine - implements the same interface using optimized CPU code.
class CPUFallbackEngine : public GPUEngine {
public:
    GPUDeviceInfo GetDeviceInfo() override;
    std::unique_ptr<GPUBuffer> AllocateBuffer(size_t size) override;
    std::vector<idx_t> Filter(const int32_t *data, idx_t count,
                               const std::string &op, int32_t value) override;
    int64_t SumInt32(const int32_t *data, idx_t count) override;
    int64_t SumInt64(const int64_t *data, idx_t count) override;
    double SumDouble(const double *data, idx_t count) override;
    std::vector<idx_t> SortIndices(const int32_t *data, idx_t count, bool ascending) override;
    std::vector<idx_t> SortIndices(const int64_t *data, idx_t count, bool ascending) override;
    std::vector<idx_t> SortIndices(const double *data, idx_t count, bool ascending) override;
    AggResult HashAggregate(const int32_t *keys, const int32_t *values, idx_t count) override;
};

} // namespace slothdb
