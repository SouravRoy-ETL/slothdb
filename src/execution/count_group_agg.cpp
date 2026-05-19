#include "slothdb/execution/count_group_agg.hpp"

#include "slothdb/common/types/data_chunk.hpp"
#include "slothdb/common/types/logical_type.hpp"
#include "slothdb/common/types/vector.hpp"
#include "slothdb/common/types/string_type.hpp"
#include "slothdb/common/types/hugeint.hpp"
#include "slothdb/execution/physical_operator.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "third_party/unordered_dense.h"

namespace slothdb {

namespace {

// Append the raw little-endian bytes of a trivially-copyable scalar.
template <class T>
inline void AppendRaw(std::string &key, T v) {
    key.append(reinterpret_cast<const char *>(&v), sizeof(T));
}

// Encode one group-column cell into `key` as a length-self-describing
// binary blob: a 1-byte presence tag, then a fixed- or length-prefixed
// payload determined by the column's logical type. Two rows produce the
// same byte sequence iff they belong to the same logical group, so the
// concatenation of all group columns is a collision-free composite key.
inline void EncodeCell(std::string &key, const Vector &vec,
                       LogicalTypeId tid, idx_t row) {
    if (!vec.GetValidity().RowIsValid(row)) {
        key.push_back('\0');  // NULL
        return;
    }
    key.push_back('\1');  // present
    switch (tid) {
    case LogicalTypeId::BOOLEAN:
        AppendRaw(key, vec.GetData<bool>()[row]);
        break;
    case LogicalTypeId::TINYINT:
        AppendRaw(key, vec.GetData<int8_t>()[row]);
        break;
    case LogicalTypeId::UTINYINT:
        AppendRaw(key, vec.GetData<uint8_t>()[row]);
        break;
    case LogicalTypeId::SMALLINT:
        AppendRaw(key, vec.GetData<int16_t>()[row]);
        break;
    case LogicalTypeId::USMALLINT:
        AppendRaw(key, vec.GetData<uint16_t>()[row]);
        break;
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::DATE:
        AppendRaw(key, vec.GetData<int32_t>()[row]);
        break;
    case LogicalTypeId::UINTEGER:
        AppendRaw(key, vec.GetData<uint32_t>()[row]);
        break;
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIMESTAMP_TZ:
    case LogicalTypeId::TIME:
        AppendRaw(key, vec.GetData<int64_t>()[row]);
        break;
    case LogicalTypeId::UBIGINT:
        AppendRaw(key, vec.GetData<uint64_t>()[row]);
        break;
    case LogicalTypeId::FLOAT:
        AppendRaw(key, vec.GetData<float>()[row]);
        break;
    case LogicalTypeId::DOUBLE:
        AppendRaw(key, vec.GetData<double>()[row]);
        break;
    case LogicalTypeId::HUGEINT:
        AppendRaw(key, vec.GetData<hugeint_t>()[row]);
        break;
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::BLOB: {
        const auto &s = vec.GetData<string_t>()[row];
        uint32_t n = s.GetSize();
        AppendRaw(key, n);
        key.append(s.GetData(), n);
        break;
    }
    default:
        // Unreachable: the dispatcher in physical_planner.cpp only routes a
        // GROUP BY here when every group column is one of the encodable
        // types above.
        break;
    }
}

// Decode a fixed-width scalar and advance the read cursor.
template <class T>
inline T ReadRaw(const char *&p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
}

// Reverse of EncodeCell: rebuild a typed Value from the binary blob.
inline Value DecodeCell(const char *&p, LogicalTypeId tid) {
    char tag = *p++;
    if (tag == '\0') return Value();  // NULL
    switch (tid) {
    case LogicalTypeId::BOOLEAN: return Value::BOOLEAN(ReadRaw<bool>(p));
    case LogicalTypeId::TINYINT: return Value::TINYINT(ReadRaw<int8_t>(p));
    case LogicalTypeId::UTINYINT: return Value::UTINYINT(ReadRaw<uint8_t>(p));
    case LogicalTypeId::SMALLINT: return Value::SMALLINT(ReadRaw<int16_t>(p));
    case LogicalTypeId::USMALLINT: return Value::USMALLINT(ReadRaw<uint16_t>(p));
    case LogicalTypeId::INTEGER: return Value::INTEGER(ReadRaw<int32_t>(p));
    case LogicalTypeId::DATE: return Value::DATE(ReadRaw<int32_t>(p));
    case LogicalTypeId::UINTEGER: return Value::UINTEGER(ReadRaw<uint32_t>(p));
    case LogicalTypeId::BIGINT: return Value::BIGINT(ReadRaw<int64_t>(p));
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIMESTAMP_TZ:
        return Value::TIMESTAMP(ReadRaw<int64_t>(p));
    case LogicalTypeId::TIME: return Value::TIME(ReadRaw<int64_t>(p));
    case LogicalTypeId::UBIGINT: return Value::UBIGINT(ReadRaw<uint64_t>(p));
    case LogicalTypeId::FLOAT: return Value::FLOAT(ReadRaw<float>(p));
    case LogicalTypeId::DOUBLE: return Value::DOUBLE(ReadRaw<double>(p));
    case LogicalTypeId::HUGEINT: return Value::HUGEINT(ReadRaw<hugeint_t>(p));
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::BLOB: {
        uint32_t n = ReadRaw<uint32_t>(p);
        std::string s(p, n);
        p += n;
        return tid == LogicalTypeId::BLOB ? Value::BLOB(s) : Value::VARCHAR(s);
    }
    default:
        return Value();
    }
}

} // namespace

std::vector<std::vector<Value>> RunCountGroupAggregate(
    PhysicalOperator *child,
    const std::vector<idx_t> &group_col_indices,
    idx_t num_count_aggs,
    bool topn_active,
    bool topn_count_order,
    bool topn_ascending,
    idx_t topn_limit) {

    const auto &child_types = child->GetTypes();
    std::vector<LogicalTypeId> gtypes;
    gtypes.reserve(group_col_indices.size());
    for (idx_t gc : group_col_indices)
        gtypes.push_back(child_types[gc].id());

    // key -> group ordinal; count per ordinal kept in a flat vector so each
    // group costs one int64 rather than a heap-allocated state vector.
    ankerl::unordered_dense::map<std::string, uint32_t> key2ord;
    std::vector<int64_t> counts;

    DataChunk chunk;
    chunk.Initialize(child_types);
    std::string key;
    key.reserve(64);

    while (child->GetData(chunk)) {
        idx_t n = chunk.size();
        for (idx_t i = 0; i < n; i++) {
            key.clear();
            for (size_t g = 0; g < group_col_indices.size(); g++) {
                EncodeCell(key, chunk.GetVector(group_col_indices[g]),
                           gtypes[g], i);
            }
            auto [it, inserted] =
                key2ord.try_emplace(key, (uint32_t)counts.size());
            if (inserted) counts.push_back(1);
            else counts[it->second]++;
        }
    }

    // ordinal -> stored key. The map is no longer mutated, so the addresses
    // of its keys are stable for the remainder of this function.
    std::vector<const std::string *> ord2key(counts.size(), nullptr);
    for (const auto &kv : key2ord) ord2key[kv.second] = &kv.first;

    // Decode group ordinal `ord` into a result row.
    auto emit_row = [&](uint32_t ord) {
        const char *p = ord2key[ord]->data();
        std::vector<Value> row;
        row.reserve(group_col_indices.size() + num_count_aggs);
        for (size_t g = 0; g < gtypes.size(); g++)
            row.push_back(DecodeCell(p, gtypes[g]));
        for (idx_t a = 0; a < num_count_aggs; a++)
            row.push_back(Value::BIGINT(counts[ord]));
        return row;
    };

    std::vector<std::vector<Value>> rows;

    // ORDER BY <count column> LIMIT k: materialize only the k extreme-count
    // groups. A GROUP BY with 50M+ groups would otherwise build 50M output
    // rows of boxed Values purely for the upstream LIMIT to drop them.
    if (topn_active && topn_count_order && topn_limit > 0 &&
        topn_limit < counts.size()) {
        std::vector<uint32_t> idx(counts.size());
        for (uint32_t i = 0; i < idx.size(); i++) idx[i] = i;
        auto better = [&](uint32_t a, uint32_t b) {
            // Partition so the wanted side comes first: largest counts for
            // DESC, smallest for ASC.
            return topn_ascending ? counts[a] < counts[b]
                                  : counts[a] > counts[b];
        };
        std::nth_element(idx.begin(), idx.begin() + topn_limit, idx.end(),
                         better);
        idx.resize(topn_limit);
        std::sort(idx.begin(), idx.end(), better);
        rows.reserve(idx.size());
        for (uint32_t ord : idx) rows.push_back(emit_row(ord));
        return rows;
    }

    // No usable count-order LIMIT — emit every group in insertion order.
    rows.resize(counts.size());
    for (uint32_t ord = 0; ord < counts.size(); ord++)
        rows[ord] = emit_row(ord);
    return rows;
}

} // namespace slothdb
