#pragma once
// Per-thread radix-shard VARCHAR distinct build for Q6
// (`SELECT COUNT(DISTINCT SearchPhrase) FROM hits`).
//
// Mirrors the INT scatter pattern at physical_planner.cpp:6266 (Q5):
//   - Each thread holds 16 shards keyed on low-4 ankerl hash bits.
//   - Each shard is `set<string_view>` over a per-thread bump arena
//     so insertions allocate only on cache misses, not on duplicates.
//   - Final merge unions thread-disjoint shards in parallel — no rehash,
//     no malloc, no memcpy in the merge phase.
//
// Lives in a separate TU to keep physical_planner.cpp's .text section
// stable across this change (see feedback_text_icache_shift.md).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace slothdb {

struct Q6PerThread;

struct Q6BuildState {
    // Sized to MAX_THREADS at construction. Pointer-of-array layout so
    // moving/copying the outer state never invalidates the per-thread
    // string_views (they reference arena chunks owned by Q6PerThread).
    std::vector<std::unique_ptr<Q6PerThread>> threads;
    explicit Q6BuildState(int max_threads);
    ~Q6BuildState();
    Q6BuildState(const Q6BuildState&) = delete;
    Q6BuildState& operator=(const Q6BuildState&) = delete;
};

// Insert (data, size) into thread `tid`'s appropriate shard. On a miss
// the bytes are copied into the per-thread arena before the string_view
// is stored, so the keys remain valid across the merge phase.
void q6_emplace(Q6BuildState& st, int tid, const char* data, std::size_t size);

// Sum the per-shard distinct counts. Each shard is unioned across
// threads in its own worker thread (NSHARDS == 16) — disjoint by hash
// partition, so no rehash needed during merge.
std::int64_t q6_total_distinct(Q6BuildState& st);

} // namespace slothdb
