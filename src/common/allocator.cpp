#include "slothdb/common/allocator.hpp"
#include "slothdb/common/exception.hpp"
#include <algorithm>
#include <cstring>

namespace slothdb {

data_ptr_t Allocator::AllocateData(idx_t size) {
    auto result = static_cast<data_ptr_t>(std::malloc(size));
    if (!result) {
        throw InternalException("Failed to allocate " + std::to_string(size) + " bytes");
    }
    return result;
}

data_ptr_t Allocator::ReallocateData(data_ptr_t pointer, idx_t old_size, idx_t new_size) {
    auto result = static_cast<data_ptr_t>(std::realloc(pointer, new_size));
    if (!result) {
        throw InternalException("Failed to reallocate to " + std::to_string(new_size) + " bytes");
    }
    return result;
}

void Allocator::FreeData(data_ptr_t pointer, idx_t size) {
    (void)size;
    std::free(pointer);
}

Allocator &Allocator::DefaultAllocator() {
    static Allocator instance;
    return instance;
}

// --- ArenaAllocator ---

ArenaAllocator::ArenaAllocator(Allocator &allocator)
    : allocator_(allocator), head_(nullptr), current_(nullptr) {
}

ArenaAllocator::~ArenaAllocator() {
    // unique_ptr chain handles cleanup
}

void ArenaAllocator::AllocateNewBlock(idx_t min_size) {
    idx_t block_size = std::max(ARENA_BLOCK_SIZE, min_size);
    auto block = std::make_unique<ArenaBlock>();
    block->data = std::make_unique<data_t[]>(block_size);
    block->capacity = block_size;
    block->used = 0;
    block->next = std::move(head_);
    current_ = block.get();
    head_ = std::move(block);
}

data_ptr_t ArenaAllocator::Allocate(idx_t size) {
    if (!current_ || current_->used + size > current_->capacity) {
        AllocateNewBlock(size);
    }
    auto result = current_->data.get() + current_->used;
    current_->used += size;
    return result;
}

void ArenaAllocator::Reset() {
    head_.reset();
    current_ = nullptr;
}

} // namespace slothdb
