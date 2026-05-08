#pragma once

// Minimal open-addressing int64-key int64-count map. 16-byte slots; key 0 is
// reserved as the empty sentinel and tracked separately via has_zero_. Linear
// probing, ankerl hash, 0.5 load-factor grow. Mirrors SimpleI64Set with a
// per-slot counter for COUNT(*)-style aggregation on a single BIGINT GROUP BY
// column. Used for ClickBench Q16 (`GROUP BY UserID` over 17.6M distinct keys
// — ankerl::unordered_dense::map<int64_t, vector<AggState>> at this size is
// memory-bound; the 16-byte direct-slot layout cuts probe cache lines from
// 3-4 down to 1).

#include <cstdint>
#include <utility>
#include <vector>

#include "third_party/unordered_dense.h"

namespace slothdb {

class SimpleI64CountMap {
public:
    struct Slot {
        int64_t key;
        int64_t count;
    };

    SimpleI64CountMap()
        : slots_(64), cap_(64), mask_(63), count_(0),
          has_zero_(false), zero_count_(0) {}

    // Increment the count for `k`. Returns true iff this added a new key.
    bool increment(int64_t k) {
        if (k == 0) {
            zero_count_++;
            bool was_new = !has_zero_;
            has_zero_ = true;
            return was_new;
        }
        if ((count_ + 1) * 2 > cap_) grow();
        size_t i = ankerl::unordered_dense::hash<int64_t>{}(k) & mask_;
        while (true) {
            int64_t s = slots_[i].key;
            if (s == 0) { slots_[i].key = k; slots_[i].count = 1; ++count_; return true; }
            if (s == k) { slots_[i].count++; return false; }
            i = (i + 1) & mask_;
        }
    }

    // Add `delta` to the count for `k`. Returns true iff this added a new key.
    bool add(int64_t k, int64_t delta) {
        if (k == 0) {
            zero_count_ += delta;
            bool was_new = !has_zero_;
            has_zero_ = true;
            return was_new;
        }
        if ((count_ + 1) * 2 > cap_) grow();
        size_t i = ankerl::unordered_dense::hash<int64_t>{}(k) & mask_;
        while (true) {
            int64_t s = slots_[i].key;
            if (s == 0) { slots_[i].key = k; slots_[i].count = delta; ++count_; return true; }
            if (s == k) { slots_[i].count += delta; return false; }
            i = (i + 1) & mask_;
        }
    }

    void reserve(size_t expected) {
        size_t target = 64;
        while ((expected + 1) * 2 > target) target <<= 1;
        if (target > cap_) {
            cap_ = target;
            mask_ = cap_ - 1;
            slots_.assign(cap_, {0, 0});
            count_ = 0;
        }
    }

    size_t size() const noexcept { return count_ + (has_zero_ ? 1 : 0); }
    bool empty() const noexcept { return count_ == 0 && !has_zero_; }

    template <typename Fn>
    void for_each(Fn &&fn) const {
        if (has_zero_) fn(int64_t(0), zero_count_);
        for (auto &s : slots_) if (s.key != 0) fn(s.key, s.count);
    }

private:
    void grow() {
        size_t new_cap = cap_ * 2;
        std::vector<Slot> new_slots(new_cap);
        size_t new_mask = new_cap - 1;
        for (auto &s : slots_) {
            if (s.key == 0) continue;
            size_t i = ankerl::unordered_dense::hash<int64_t>{}(s.key) & new_mask;
            while (new_slots[i].key != 0) i = (i + 1) & new_mask;
            new_slots[i] = s;
        }
        slots_ = std::move(new_slots);
        cap_ = new_cap;
        mask_ = new_mask;
    }

    std::vector<Slot> slots_;
    size_t cap_;
    size_t mask_;
    size_t count_;
    bool has_zero_;
    int64_t zero_count_;
};

} // namespace slothdb
