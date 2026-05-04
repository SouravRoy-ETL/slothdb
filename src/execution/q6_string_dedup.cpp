#include "slothdb/execution/q6_string_dedup.hpp"
#include "third_party/unordered_dense.h"
#include <array>
#include <cstring>
#include <cstdint>
#include <string_view>
#include <thread>
#include <vector>

namespace slothdb {

namespace {

constexpr int NSHARDS = 16;
constexpr std::size_t ARENA_CHUNK = 256 * 1024; // 256 KB per chunk

// Per-thread shards + bump arena. Kept here (anonymous namespace) so the
// TLDistinct struct in physical_planner.cpp does not grow (the 960-byte
// static_assert pins its layout — see feedback_struct_growth_cache_shifts.md).
struct ArenaChunk {
    std::vector<char> bytes;
    std::size_t used = 0;
};

} // anonymous namespace

struct Q6PerThread {
    std::array<ankerl::unordered_dense::set<std::string_view>, NSHARDS> shards;
    std::vector<ArenaChunk> arena;

    Q6PerThread() = default;

    // Reserve `n` stable bytes in the arena, returns the destination ptr.
    // Strings larger than ARENA_CHUNK get their own chunk.
    char* alloc(std::size_t n) {
        if (arena.empty() || arena.back().bytes.size() - arena.back().used < n) {
            arena.emplace_back();
            std::size_t cap = (n > ARENA_CHUNK) ? n : ARENA_CHUNK;
            arena.back().bytes.resize(cap);
        }
        char* p = arena.back().bytes.data() + arena.back().used;
        arena.back().used += n;
        return p;
    }
};

Q6BuildState::Q6BuildState(int max_threads) {
    threads.reserve(max_threads);
    for (int i = 0; i < max_threads; i++) {
        threads.emplace_back(std::make_unique<Q6PerThread>());
    }
}

Q6BuildState::~Q6BuildState() = default;

void q6_emplace(Q6BuildState& st, int tid, const char* data, std::size_t size) {
    auto& pt = *st.threads[tid];
    ankerl::unordered_dense::hash<std::string_view> H;
    std::string_view probe(data, size);
    std::size_t h = H(probe);
    auto& shard = pt.shards[h & (NSHARDS - 1)];
    // Heterogeneous lookup: try the probe view first; only on miss copy
    // bytes into the arena and insert a stable view.
    if (shard.find(probe) != shard.end()) return;
    char* dst = pt.alloc(size);
    if (size) std::memcpy(dst, data, size);
    shard.insert(std::string_view(dst, size));
}

std::int64_t q6_total_distinct(Q6BuildState& st) {
    const int n_threads = (int)st.threads.size();
    std::array<std::size_t, NSHARDS> shard_sz{};

    auto run_shard = [&](int s) {
        // Disjoint by hash partition: no thread t's shard[s] entry can
        // collide with thread t'!=t's shard[s'!=s] entry. So unioning
        // shard `s` across all threads gives the total distinct in shard s.
        ankerl::unordered_dense::set<std::string_view> out;
        // Pre-reserve based on the largest single-thread shard to skip
        // most rehashes during the union.
        std::size_t max_single = 0;
        for (int t = 0; t < n_threads; t++) {
            std::size_t sz = st.threads[t]->shards[s].size();
            if (sz > max_single) max_single = sz;
        }
        out.reserve(max_single * 2);
        for (int t = 0; t < n_threads; t++) {
            const auto& src = st.threads[t]->shards[s];
            for (auto v : src) out.insert(v);
        }
        shard_sz[s] = out.size();
    };

    std::vector<std::thread> mts;
    mts.reserve(NSHARDS - 1);
    for (int s = 1; s < NSHARDS; s++) mts.emplace_back(run_shard, s);
    run_shard(0);
    for (auto& m : mts) m.join();

    std::int64_t total = 0;
    for (int s = 0; s < NSHARDS; s++) total += (std::int64_t)shard_sz[s];
    return total;
}

} // namespace slothdb
