#include "slothdb/execution/agg_emit_helpers.hpp"

#include <algorithm>
#include <cmath>

namespace slothdb {

EmitAggKind ResolveEmitAggKind(const std::string& n) {
    if (n == "COUNT") return EmitAggKind::Count;
    if (n == "SUM") return EmitAggKind::Sum;
    if (n == "AVG") return EmitAggKind::Avg;
    if (n == "MIN") return EmitAggKind::Min;
    if (n == "MAX") return EmitAggKind::Max;
    if (n == "STRING_AGG" || n == "LISTAGG" || n == "GROUP_CONCAT")
        return EmitAggKind::StringAgg;
    if (n == "STDDEV" || n == "STDDEV_SAMP") return EmitAggKind::StddevSamp;
    if (n == "STDDEV_POP") return EmitAggKind::StddevPop;
    if (n == "VARIANCE" || n == "VAR_SAMP") return EmitAggKind::VarSamp;
    if (n == "VAR_POP") return EmitAggKind::VarPop;
    if (n == "MEDIAN") return EmitAggKind::Median;
    if (n == "BOOL_AND") return EmitAggKind::BoolAnd;
    if (n == "BOOL_OR") return EmitAggKind::BoolOr;
    return EmitAggKind::Other;
}

bool EmitAggValue(const EmitAggDesc& desc, const EmitAggView& view,
                  std::vector<Value>& out_row) {
    switch (desc.kind) {
    case EmitAggKind::Count:
        out_row.push_back(Value::BIGINT(view.count));
        return true;
    case EmitAggKind::Sum: {
        // Algebraic collapse: SUM(col +/- N) = SUM(col) + N*COUNT(col).
        // 90 SUMs over the same column reduce to ONE scan; each
        // agg's offset adjusts the emitted answer at this point.
        double s = view.sum;
        if (desc.sum_with_offset) s += desc.sum_offset * (double)view.count;
        if (desc.return_type_id == LogicalTypeId::BIGINT) {
            out_row.push_back(Value::BIGINT(static_cast<int64_t>(s)));
        } else {
            out_row.push_back(Value::DOUBLE(s));
        }
        return true;
    }
    case EmitAggKind::Avg:
        if (view.count > 0) {
            out_row.push_back(Value::DOUBLE(view.sum / view.count));
        } else {
            out_row.push_back(Value());
        }
        return true;
    case EmitAggKind::Min:
        if (!view.has_min) {
            out_row.push_back(Value());
        } else if (view.min_val_ptr) {
            out_row.push_back(*view.min_val_ptr);
        } else {
            // Fused numeric path writes only sum_min (double); synthesize
            // a typed Value here using the agg's return type.
            switch (desc.return_type_id) {
            case LogicalTypeId::INTEGER:
                out_row.push_back(Value::INTEGER((int32_t)view.sum_min)); break;
            case LogicalTypeId::BIGINT:
                out_row.push_back(Value::BIGINT((int64_t)view.sum_min)); break;
            case LogicalTypeId::FLOAT:
                out_row.push_back(Value::FLOAT((float)view.sum_min)); break;
            default:
                out_row.push_back(Value::DOUBLE(view.sum_min)); break;
            }
        }
        return true;
    case EmitAggKind::Max:
        if (!view.has_max) {
            out_row.push_back(Value());
        } else if (view.max_val_ptr) {
            out_row.push_back(*view.max_val_ptr);
        } else {
            switch (desc.return_type_id) {
            case LogicalTypeId::INTEGER:
                out_row.push_back(Value::INTEGER((int32_t)view.sum_max)); break;
            case LogicalTypeId::BIGINT:
                out_row.push_back(Value::BIGINT((int64_t)view.sum_max)); break;
            case LogicalTypeId::FLOAT:
                out_row.push_back(Value::FLOAT((float)view.sum_max)); break;
            default:
                out_row.push_back(Value::DOUBLE(view.sum_max)); break;
            }
        }
        return true;
    case EmitAggKind::StringAgg:
        out_row.push_back(view.str_started && view.str_agg
                              ? Value::VARCHAR(*view.str_agg)
                              : Value());
        return true;
    case EmitAggKind::StddevSamp:
        if (view.count > 1) {
            double mean = view.sum / view.count;
            double var = (view.sum_sq - view.count * mean * mean) /
                         (view.count - 1);
            out_row.push_back(Value::DOUBLE(std::sqrt(var)));
        } else {
            out_row.push_back(Value());
        }
        return true;
    case EmitAggKind::StddevPop:
        if (view.count > 0) {
            double mean = view.sum / view.count;
            double var = (view.sum_sq - view.count * mean * mean) / view.count;
            out_row.push_back(Value::DOUBLE(std::sqrt(var)));
        } else {
            out_row.push_back(Value());
        }
        return true;
    case EmitAggKind::VarSamp:
        if (view.count > 1) {
            double mean = view.sum / view.count;
            double var = (view.sum_sq - view.count * mean * mean) /
                         (view.count - 1);
            out_row.push_back(Value::DOUBLE(var));
        } else {
            out_row.push_back(Value());
        }
        return true;
    case EmitAggKind::VarPop:
        if (view.count > 0) {
            double mean = view.sum / view.count;
            double var = (view.sum_sq - view.count * mean * mean) / view.count;
            out_row.push_back(Value::DOUBLE(var));
        } else {
            out_row.push_back(Value());
        }
        return true;
    case EmitAggKind::Median:
        if (view.values && !view.values->empty()) {
            auto& vals = *view.values;
            std::sort(vals.begin(), vals.end());
            size_t mid = vals.size() / 2;
            double median = (vals.size() % 2 == 0)
                ? (vals[mid - 1] + vals[mid]) / 2.0
                : vals[mid];
            out_row.push_back(Value::DOUBLE(median));
        } else {
            out_row.push_back(Value());
        }
        return true;
    case EmitAggKind::BoolAnd:
        out_row.push_back(view.count > 0 ? Value::BOOLEAN(view.bool_and_v) : Value());
        return true;
    case EmitAggKind::BoolOr:
        out_row.push_back(view.count > 0 ? Value::BOOLEAN(view.bool_or_v) : Value());
        return true;
    case EmitAggKind::Other:
        return false;
    }
    return false;
}

} // namespace slothdb
