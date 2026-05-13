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

// DuckDB-style needle-size-specialized substring search. Mirrors
// _private/duckdb/src/function/scalar/string/contains.cpp::FindStrInStr.
// For 4-8 byte needles, loads the first 4/8 bytes as a single uint32/uint64
// and compares with one cmp instead of byte-by-byte memcmp. Failing
// candidates exit fast; only the small fraction that match the prefix
// pay for a tail memcmp. Empirically ~1.5-2x faster on 6-byte needles
// like "google" vs the generic memcmp loop.
template <typename U>
static inline U LoadU(const char* p) {
    U v;
    std::memcpy(&v, p, sizeof(U));
    return v;
}

static inline bool ContainsFast4(const char* h, std::size_t hlen, std::uint32_t needle32) {
    if (hlen < 4) return false;
    const char* end = h + (hlen - 3);
    const unsigned char first = static_cast<unsigned char>(needle32 & 0xFF);
    while (h < end) {
        const char* p = static_cast<const char*>(
            std::memchr(h, first, static_cast<std::size_t>(end - h)));
        if (!p) return false;
        if (LoadU<std::uint32_t>(p) == needle32) return true;
        h = p + 1;
    }
    return false;
}

template <std::size_t TAIL>
static inline bool ContainsFastN(const char* h, std::size_t hlen,
                                  std::uint32_t needle32, const char* needle_tail) {
    constexpr std::size_t NLEN = 4 + TAIL;
    if (hlen < NLEN) return false;
    const char* end = h + (hlen - (NLEN - 1));
    const unsigned char first = static_cast<unsigned char>(needle32 & 0xFF);
    while (h < end) {
        const char* p = static_cast<const char*>(
            std::memchr(h, first, static_cast<std::size_t>(end - h)));
        if (!p) return false;
        if (LoadU<std::uint32_t>(p) == needle32) {
            if (std::memcmp(p + 4, needle_tail, TAIL) == 0) return true;
        }
        h = p + 1;
    }
    return false;
}

static inline bool ContainsFast8(const char* h, std::size_t hlen, std::uint64_t needle64) {
    if (hlen < 8) return false;
    const char* end = h + (hlen - 7);
    const unsigned char first = static_cast<unsigned char>(needle64 & 0xFF);
    while (h < end) {
        const char* p = static_cast<const char*>(
            std::memchr(h, first, static_cast<std::size_t>(end - h)));
        if (!p) return false;
        if (LoadU<std::uint64_t>(p) == needle64) return true;
        h = p + 1;
    }
    return false;
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

std::int64_t CountPlainLikeContains(
    const string_t* str_data, std::size_t nrows,
    const char* needle, std::size_t nlen,
    bool like_negated) {
    if (nlen == 0) {
        // LIKE '%%' matches everything; NOT LIKE '%%' nothing.
        return like_negated ? 0 : static_cast<std::int64_t>(nrows);
    }
    std::int64_t local = 0;
    // Dispatch on needle size for the inner loop. We instantiate the
    // 4/8-byte aligned cases directly and 5/6/7/<other> via the tail
    // template. Outer loop over rows; inner contains-check is the
    // tight specialized path. Negation is folded post-check to keep the
    // hot inner loops branch-light.
    if (nlen == 4) {
        std::uint32_t n32 = LoadU<std::uint32_t>(needle);
        for (std::size_t r = 0; r < nrows; r++) {
            const auto& s = str_data[r];
            bool m = ContainsFast4(s.GetData(), s.GetSize(), n32);
            local += (m != like_negated) ? 1 : 0;
        }
    } else if (nlen == 5) {
        std::uint32_t n32 = LoadU<std::uint32_t>(needle);
        const char* tail = needle + 4;
        for (std::size_t r = 0; r < nrows; r++) {
            const auto& s = str_data[r];
            bool m = ContainsFastN<1>(s.GetData(), s.GetSize(), n32, tail);
            local += (m != like_negated) ? 1 : 0;
        }
    } else if (nlen == 6) {
        std::uint32_t n32 = LoadU<std::uint32_t>(needle);
        const char* tail = needle + 4;
        for (std::size_t r = 0; r < nrows; r++) {
            const auto& s = str_data[r];
            bool m = ContainsFastN<2>(s.GetData(), s.GetSize(), n32, tail);
            local += (m != like_negated) ? 1 : 0;
        }
    } else if (nlen == 7) {
        std::uint32_t n32 = LoadU<std::uint32_t>(needle);
        const char* tail = needle + 4;
        for (std::size_t r = 0; r < nrows; r++) {
            const auto& s = str_data[r];
            bool m = ContainsFastN<3>(s.GetData(), s.GetSize(), n32, tail);
            local += (m != like_negated) ? 1 : 0;
        }
    } else if (nlen == 8) {
        std::uint64_t n64 = LoadU<std::uint64_t>(needle);
        for (std::size_t r = 0; r < nrows; r++) {
            const auto& s = str_data[r];
            bool m = ContainsFast8(s.GetData(), s.GetSize(), n64);
            local += (m != like_negated) ? 1 : 0;
        }
    } else {
        // Generic path for nlen in {1, 2, 3, 9+}. Falls back to the
        // memchr+memcmp loop. nlen=1 still has memchr only (memcmp
        // is no-op-ish).
        for (std::size_t r = 0; r < nrows; r++) {
            const auto& s = str_data[r];
            const char* hs = s.GetData();
            std::uint32_t hl = s.GetSize();
            bool m = (hl >= nlen) &&
                     (FindSubstrLocal(hs, hl, needle, nlen) != nullptr);
            local += (m != like_negated) ? 1 : 0;
        }
    }
    return local;
}

}  // namespace slothdb
