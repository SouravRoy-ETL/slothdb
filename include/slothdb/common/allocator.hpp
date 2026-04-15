#pragma once

#include "slothdb/common/constants.hpp"
#include <cstdlib>
#include <memory>

namespace slothdb {

class Allocator {
public:
    Allocator() = default;
    ~Allocator() = default;

    data_ptr_t AllocateData(idx_t size);
    data_ptr_t ReallocateData(data_ptr_t pointer, idx_t old_size, idx_t new_size);
    void FreeData(data_ptr_t pointer, idx_t size);

    static Allocator &DefaultAllocator();

private:
    // Could track allocated_bytes_ for debugging/limits in the future.
};

// Bump allocator for short-lived, batch allocations (parser, binder).
// Memory is freed all at once when the arena is destroyed.
class ArenaAllocator {
public:
    static constexpr idx_t ARENA_BLOCK_SIZE = 262144; // 256 KB

    ArenaAllocator(Allocator &allocator);
    ~ArenaAllocator();

    data_ptr_t Allocate(idx_t size);
    void Reset();

private:
    struct ArenaBlock {
        std::unique_ptr<data_t[]> data;
        idx_t capacity;
        idx_t used;
        std::unique_ptr<ArenaBlock> next;
    };

    void AllocateNewBlock(idx_t min_size);

    Allocator &allocator_;
    std::unique_ptr<ArenaBlock> head_;
    ArenaBlock *current_;
};

} // namespace slothdb
