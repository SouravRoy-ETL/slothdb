#include "slothdb/execution/q21_helper.hpp"

#include <cstring>
#include <vector>

namespace slothdb {

// Local copy of physical_planner.cpp::FindSubstr — same memchr-anchored
// memcmp loop, kept here to avoid pulling the planner header into this
// side TU. ~6.8x faster than std::search on URL-shaped strings.
static inline const char* FindSubstrLocal(const char* h, std::size_t hlen,
                                          const char* n, std::size_t nlen) {
    if (nlen == 0) return h;
    if (nlen > hlen) return nullptr;
    const char first = n[0];
    const char* end = h + (hlen - nlen) + 1;
    while (h < end) {
        const char* p = static_cast<const char*>(
            std::memchr(h, static_cast<unsigned char>(first),
                        static_cast<std::size_t>(end - h)));
        if (!p) return nullptr;
        if (std::memcmp(p, n, nlen) == 0) return p;
        h = p + 1;
    }
    return nullptr;
}

std::int64_t CountDictLikeContains(
    const string_t* dict_values, std::size_t dict_size,
    const std::uint32_t* dict_indices, std::size_t nrows,
    const char* needle, std::size_t nlen,
    bool like_negated) {
    // Precompute match per dict entry — 1 FindSubstr per unique URL instead
    // of per row. Dict is typically thousands; rows can be ~1M.
    const std::uint8_t hit_v = like_negated ? 0u : 1u;
    const std::uint8_t miss_v = like_negated ? 1u : 0u;
    std::vector<std::uint8_t> dict_match(dict_size);
    if (nlen == 0) {
        // LIKE '%%' matches everything; NOT LIKE '%%' nothing.
        for (std::size_t di = 0; di < dict_size; di++) dict_match[di] = hit_v;
    } else {
        for (std::size_t di = 0; di < dict_size; di++) {
            const auto& dv = dict_values[di];
            const char* hs = dv.GetData();
            std::uint32_t hl = dv.GetSize();
            if (hl < nlen) { dict_match[di] = miss_v; continue; }
            dict_match[di] =
                FindSubstrLocal(hs, hl, needle, nlen) ? hit_v : miss_v;
        }
    }
    // Single-pass reduction: skip the out_mask intermediate that
    // BuildTypedKeepMask allocates + writes + re-reads.
    std::int64_t local = 0;
    for (std::size_t r = 0; r < nrows; r++) {
        local += dict_match[dict_indices[r]];
    }
    return local;
}

}  // namespace slothdb
