#include "slothdb/execution/dictionary_vector.hpp"
#include <unordered_set>

namespace slothdb {

void DictionaryVector::Build(const std::vector<Value> &values) {
    dictionary_.clear();
    encode_map_.clear();
    codes_.clear();
    null_flags_.clear();

    codes_.reserve(values.size());
    null_flags_.reserve(values.size());

    for (auto &val : values) {
        if (val.IsNull()) {
            codes_.push_back(INVALID_CODE);
            null_flags_.push_back(true);
            continue;
        }

        null_flags_.push_back(false);
        auto key = val.ToString();
        auto it = encode_map_.find(key);
        if (it != encode_map_.end()) {
            codes_.push_back(it->second);
        } else {
            auto code = static_cast<uint32_t>(dictionary_.size());
            encode_map_[key] = code;
            dictionary_.push_back(val);
            codes_.push_back(code);
        }
    }
}

uint32_t DictionaryVector::Encode(const Value &val) const {
    if (val.IsNull()) return INVALID_CODE;
    auto it = encode_map_.find(val.ToString());
    return (it != encode_map_.end()) ? it->second : INVALID_CODE;
}

const Value &DictionaryVector::Decode(uint32_t code) const {
    static Value null_val;
    if (code >= dictionary_.size()) return null_val;
    return dictionary_[code];
}

std::unordered_map<uint32_t, std::vector<idx_t>>
DictionaryVector::GroupByCodes() const {
    std::unordered_map<uint32_t, std::vector<idx_t>> groups;
    for (idx_t i = 0; i < codes_.size(); i++) {
        if (!null_flags_[i]) {
            groups[codes_[i]].push_back(i);
        }
    }
    return groups;
}

std::vector<idx_t> DictionaryVector::FilterEquals(uint32_t target_code) const {
    std::vector<idx_t> result;
    for (idx_t i = 0; i < codes_.size(); i++) {
        if (!null_flags_[i] && codes_[i] == target_code) {
            result.push_back(i);
        }
    }
    return result;
}

// ============================================================================
// Dictionary Encoder
// ============================================================================

DictionaryEncoder::EncodingDecision
DictionaryEncoder::Analyze(const std::vector<Value> &values) {
    EncodingDecision decision;
    decision.num_values = static_cast<idx_t>(values.size());

    std::unordered_set<std::string> unique;
    for (auto &v : values) {
        if (!v.IsNull()) unique.insert(v.ToString());
    }

    decision.cardinality = static_cast<idx_t>(unique.size());
    decision.cardinality_ratio = values.empty() ? 1.0
        : static_cast<double>(unique.size()) / values.size();
    decision.should_encode = DictionaryVector::ShouldEncode(
        decision.num_values, decision.cardinality);

    return decision;
}

std::unique_ptr<DictionaryVector>
DictionaryEncoder::TryEncode(const std::vector<Value> &values) {
    auto decision = Analyze(values);
    if (!decision.should_encode) return nullptr;

    auto dv = std::make_unique<DictionaryVector>();
    dv->Build(values);
    return dv;
}

} // namespace slothdb
