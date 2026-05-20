#include "slothdb/execution/expression_executor.hpp"
#include "slothdb/binder/binder.hpp"
#include "slothdb/planner/planner.hpp"
#include "slothdb/execution/physical_planner.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/string_util.hpp"
#include "slothdb/common/types/string_type.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <limits>
#include <optional>
#include <regex>

namespace slothdb {

static bool LikeMatch(const std::string &str, const std::string &pattern);

void ExpressionExecutor::Execute(const BoundExpression &expr, DataChunk &input,
                                  Vector &result, idx_t count) {
    switch (expr.GetExpressionType()) {
    case BoundExpressionType::COLUMN_REF:
        ExecuteColumnRef(static_cast<const BoundColumnRef &>(expr), input, result, count);
        break;
    case BoundExpressionType::CONSTANT:
        ExecuteConstant(static_cast<const BoundConstant &>(expr), result, count);
        break;
    case BoundExpressionType::COMPARISON:
        ExecuteComparison(static_cast<const BoundComparison &>(expr), input, result, count);
        break;
    case BoundExpressionType::CONJUNCTION:
        ExecuteConjunction(static_cast<const BoundConjunction &>(expr), input, result, count);
        break;
    case BoundExpressionType::ARITHMETIC:
        ExecuteArithmetic(static_cast<const BoundArithmetic &>(expr), input, result, count);
        break;
    case BoundExpressionType::NEGATION:
        ExecuteNegation(static_cast<const BoundNegation &>(expr), input, result, count);
        break;
    case BoundExpressionType::IS_NULL:
        ExecuteIsNull(static_cast<const BoundIsNull &>(expr), input, result, count);
        break;
    case BoundExpressionType::IS_BOOL:
        ExecuteIsBool(static_cast<const BoundIsBool &>(expr), input, result, count);
        break;
    case BoundExpressionType::UNARY_MINUS:
        ExecuteUnaryMinus(static_cast<const BoundUnaryMinus &>(expr), input, result, count);
        break;
    case BoundExpressionType::FUNCTION:
        ExecuteFunction(static_cast<const BoundFunction &>(expr), input, result, count);
        break;
    case BoundExpressionType::CAST:
        ExecuteCast(static_cast<const BoundCast &>(expr), input, result, count);
        break;
    case BoundExpressionType::SUBQUERY:
        ExecuteSubquery(static_cast<const BoundSubqueryExpression &>(expr), input, result, count);
        break;
    default:
        throw NotImplementedException("Expression executor for type");
    }
}

Value ExpressionExecutor::ExecuteScalar(const BoundExpression &expr) {
    if (expr.GetExpressionType() == BoundExpressionType::CONSTANT) {
        return static_cast<const BoundConstant &>(expr).value;
    }
    // Handle unary minus on constant (e.g., -5 in INSERT VALUES).
    if (expr.GetExpressionType() == BoundExpressionType::UNARY_MINUS) {
        auto &um = static_cast<const BoundUnaryMinus &>(expr);
        auto child_val = ExecuteScalar(*um.child);
        if (child_val.IsNull()) return child_val;
        switch (child_val.type().id()) {
        case LogicalTypeId::INTEGER:
            return Value::INTEGER(-child_val.GetValue<int32_t>());
        case LogicalTypeId::BIGINT:
            return Value::BIGINT(-child_val.GetValue<int64_t>());
        case LogicalTypeId::DOUBLE:
            return Value::DOUBLE(-child_val.GetValue<double>());
        case LogicalTypeId::FLOAT:
            return Value::FLOAT(-child_val.GetValue<float>());
        default:
            break;
        }
    }
    // Handle function on constants (e.g., CAST in INSERT).
    if (expr.GetExpressionType() == BoundExpressionType::CAST) {
        auto &cast = static_cast<const BoundCast &>(expr);
        auto child_val = ExecuteScalar(*cast.child);
        if (child_val.IsNull()) return child_val;
        auto str = child_val.ToString();
        switch (cast.GetReturnType().id()) {
        case LogicalTypeId::INTEGER: return Value::INTEGER(std::stoi(str));
        case LogicalTypeId::BIGINT: return Value::BIGINT(std::stoll(str));
        case LogicalTypeId::DOUBLE: return Value::DOUBLE(std::stod(str));
        case LogicalTypeId::VARCHAR: return Value::VARCHAR(str);
        case LogicalTypeId::DATE: {
            int32_t days;
            if (!Value::TryParseDateStringEpochDays(str.data(), str.size(), days)) {
                throw ConversionException(
                    "Could not convert string '" + str + "' to DATE");
            }
            return Value::DATE(days);
        }
        case LogicalTypeId::TIMESTAMP:
        case LogicalTypeId::TIMESTAMP_TZ: {
            int64_t micros;
            if (!Value::TryParseTimestampMicros(str.data(), str.size(), micros)) {
                int32_t days;
                if (!Value::TryParseDateStringEpochDays(str.data(), str.size(), days)) {
                    throw ConversionException(
                        "Could not convert string '" + str + "' to TIMESTAMP");
                }
                micros = static_cast<int64_t>(days) * 86400LL * 1000000LL;
            }
            return Value::TIMESTAMP(micros);
        }
        default: return Value::VARCHAR(str);
        }
    }
    throw InternalException("ExecuteScalar only supports constants and unary minus");
}

void ExpressionExecutor::ExecuteColumnRef(const BoundColumnRef &expr, DataChunk &input,
                                           Vector &result, idx_t count) {
    auto &src = input.GetVector(expr.column_index);
    auto physical = src.GetType().GetInternalType();
    idx_t type_size = GetTypeIdSize(physical);

    if (physical == PhysicalType::VARCHAR) {
        // Vectorized memcpy of string_t entries (16 bytes each) plus aux
        // ptr share so the destination vector keeps the source's string
        // heap alive. Falls back to per-row SetValue only when the source
        // has no auxiliary buffer (constructed-by-Value path). Drops the
        // per-row Value-boxing cost that dominated wall time on
        // filtered SELECT VARCHAR ORDER BY — 75 s -> sub-second on a
        // 13 M-row scan.
        std::memcpy(result.GetData(), src.GetData(), count * sizeof(string_t));
        if (auto aux = src.GetAuxiliaryPtr()) {
            result.SetAuxiliaryPtr(std::move(aux));
        }
        if (!src.GetValidity().AllValid()) {
            for (idx_t i = 0; i < count; i++) {
                if (!src.GetValidity().RowIsValid(i)) {
                    result.GetValidity().SetInvalid(i);
                }
            }
        }
    } else if (type_size > 0) {
        std::memcpy(result.GetData(), src.GetData(), count * type_size);
        if (!src.GetValidity().AllValid()) {
            for (idx_t i = 0; i < count; i++) {
                if (!src.GetValidity().RowIsValid(i)) {
                    result.GetValidity().SetInvalid(i);
                }
            }
        }
    }
}

void ExpressionExecutor::ExecuteConstant(const BoundConstant &expr, Vector &result,
                                          idx_t count) {
    for (idx_t i = 0; i < count; i++) {
        result.SetValue(i, expr.value);
    }
}

// Compare dispatch.
//
// Two important micro-optimizations:
//
//   1. The op string ("=", ">", etc.) is invariant for the whole call but
//      the loop used to compare it per row, costing 6+ string ops per
//      element. Hoist op selection outside via an enum + switch over
//      function-pointer, so each row is one branch + one typed compare.
//
//   2. When both inputs are all-valid (the common case for column data
//      with no nulls), skip the per-row validity check entirely. The
//      compiler then auto-vectorises the comparison loop.
//
// On the 10M-row Parquet `WHERE quantity > 50` benchmark this dropped
// CompareTyped<int64_t> from being the dominant cost (~600 ms) to
// effectively negligible.
template <typename T>
void ExpressionExecutor::CompareTyped(const std::string &op, Vector &left, Vector &right,
                                       Vector &result, idx_t count) {
    auto *ldata = left.GetData<T>();
    auto *rdata = right.GetData<T>();
    auto *out = result.GetData<bool>();

    enum class CmpOp : uint8_t { EQ, NE, LT, GT, LE, GE, UNKNOWN };
    CmpOp cop = CmpOp::UNKNOWN;
    if      (op == "=")  cop = CmpOp::EQ;
    else if (op == "!=" || op == "<>") cop = CmpOp::NE;
    else if (op == "<")  cop = CmpOp::LT;
    else if (op == ">")  cop = CmpOp::GT;
    else if (op == "<=") cop = CmpOp::LE;
    else if (op == ">=") cop = CmpOp::GE;

    auto &lvalid = left.GetValidity();
    auto &rvalid = right.GetValidity();
    auto &ovalid = result.GetValidity();
    bool both_all_valid = lvalid.AllValid() && rvalid.AllValid();

    auto run = [&](auto cmp) {
        if (both_all_valid) {
            for (idx_t i = 0; i < count; i++) out[i] = cmp(ldata[i], rdata[i]);
        } else {
            for (idx_t i = 0; i < count; i++) {
                if (!lvalid.RowIsValid(i) || !rvalid.RowIsValid(i)) {
                    ovalid.SetInvalid(i);
                    out[i] = false;
                } else {
                    out[i] = cmp(ldata[i], rdata[i]);
                }
            }
        }
    };

    switch (cop) {
    case CmpOp::EQ: run([](T a, T b) { return a == b; }); break;
    case CmpOp::NE: run([](T a, T b) { return a != b; }); break;
    case CmpOp::LT: run([](T a, T b) { return a <  b; }); break;
    case CmpOp::GT: run([](T a, T b) { return a >  b; }); break;
    case CmpOp::LE: run([](T a, T b) { return a <= b; }); break;
    case CmpOp::GE: run([](T a, T b) { return a >= b; }); break;
    default:
        for (idx_t i = 0; i < count; i++) out[i] = false;
        break;
    }
}

void ExpressionExecutor::ExecuteComparison(const BoundComparison &expr, DataChunk &input,
                                            Vector &result, idx_t count) {
    Vector left(expr.left->GetReturnType(), count);
    Vector right(expr.right->GetReturnType(), count);
    Execute(*expr.left, input, left, count);
    Execute(*expr.right, input, right, count);

    // SQLNULL-typed operand short-circuit. Any normal comparison op with
    // a literal NULL on either side is NULL per SQL three-valued logic.
    // Without this branch, ExecuteComparison fell through to the typed
    // dispatch switch and threw "NotImplemented: Comparison for type NULL"
    // because PhysicalType::INVALID isn't in the switch.
    //
    // Order matters: this runs BEFORE the IS DISTINCT FROM branch below
    // would be wrong (IS DISTINCT FROM has null-safe semantics — handled
    // there). LIKE / ILIKE with NULL pattern is handled in its own block.
    bool left_is_sqlnull = (expr.left->GetReturnType().id() == LogicalTypeId::SQLNULL);
    bool right_is_sqlnull = (expr.right->GetReturnType().id() == LogicalTypeId::SQLNULL);
    if ((left_is_sqlnull || right_is_sqlnull) &&
        expr.op != "IS DISTINCT FROM" && expr.op != "IS NOT DISTINCT FROM") {
        auto *out = result.GetData<bool>();
        auto &ovalid = result.GetValidity();
        for (idx_t i = 0; i < count; i++) {
            ovalid.SetInvalid(i);
            out[i] = false;
        }
        return;
    }

    // IS [NOT] DISTINCT FROM — null-safe (in)equality. Result is never NULL:
    //   NULL IS NOT DISTINCT FROM NULL -> true
    //   NULL IS DISTINCT FROM 5         -> true
    //   5 IS NOT DISTINCT FROM 5        -> true
    if (expr.op == "IS DISTINCT FROM" || expr.op == "IS NOT DISTINCT FROM") {
        bool not_distinct = (expr.op == "IS NOT DISTINCT FROM");
        auto *out = result.GetData<bool>();
        auto &lvalid = left.GetValidity();
        auto &rvalid = right.GetValidity();
        auto &ovalid = result.GetValidity();
        for (idx_t i = 0; i < count; i++) {
            ovalid.SetValid(i);
            bool l_null = !lvalid.RowIsValid(i);
            bool r_null = !rvalid.RowIsValid(i);
            bool eq;
            if (l_null && r_null) {
                eq = true;
            } else if (l_null || r_null) {
                eq = false;
            } else {
                eq = (left.GetValue(i) == right.GetValue(i));
            }
            out[i] = not_distinct ? eq : !eq;
        }
        return;
    }

    // Handle LIKE / ILIKE / NOT LIKE / NOT ILIKE specially.
    if (expr.op == "LIKE" || expr.op == "ILIKE" ||
        expr.op == "NOT LIKE" || expr.op == "NOT ILIKE") {
        bool negate = (expr.op == "NOT LIKE" || expr.op == "NOT ILIKE");
        bool case_insensitive = (expr.op == "ILIKE" || expr.op == "NOT ILIKE");
        auto *out = result.GetData<bool>();
        // Constant-pattern hoist: when the RHS is a literal pattern, the
        // existing per-row path allocates a fresh std::string for the pattern
        // on every row. Lift that allocation out of the loop. For shape
        // '%literal%' (case-sensitive, no _, no escapes) skip the backtracker
        // entirely and call std::search on string_t::GetData() directly --
        // no GetString() heap alloc on the haystack either.
        bool pat_is_constant =
            (expr.right->GetExpressionType() == BoundExpressionType::CONSTANT);
        if (pat_is_constant && count > 0 && right.GetValidity().RowIsValid(0)) {
            std::string pattern = right.GetData<string_t>()[0].GetString();
            if (case_insensitive) {
                for (auto &c : pattern) c = static_cast<char>(std::tolower(c));
            }
            bool contains_shape = false;
            std::string needle;
            if (!case_insensitive && pattern.size() >= 2 &&
                pattern.front() == '%' && pattern.back() == '%') {
                needle = pattern.substr(1, pattern.size() - 2);
                bool ok = true;
                for (char c : needle) {
                    if (c == '%' || c == '_' || c == '\\') { ok = false; break; }
                }
                contains_shape = ok;
            }
            if (contains_shape) {
                for (idx_t i = 0; i < count; i++) {
                    if (!left.GetValidity().RowIsValid(i)) {
                        result.GetValidity().SetInvalid(i);
                        out[i] = false;
                        continue;
                    }
                    const auto &s = left.GetData<string_t>()[i];
                    const char *hs = s.GetData();
                    uint32_t hl = s.GetSize();
                    bool m;
                    if (needle.empty()) {
                        m = true;
                    } else if (hl < needle.size()) {
                        m = false;
                    } else {
                        auto end = hs + hl;
                        m = (std::search(hs, end, needle.begin(), needle.end()) != end);
                    }
                    out[i] = negate ? !m : m;
                }
                return;
            }
            for (idx_t i = 0; i < count; i++) {
                if (!left.GetValidity().RowIsValid(i)) {
                    result.GetValidity().SetInvalid(i);
                    out[i] = false;
                } else {
                    auto str = left.GetData<string_t>()[i].GetString();
                    if (case_insensitive) {
                        for (auto &c : str) c = static_cast<char>(std::tolower(c));
                    }
                    bool m = LikeMatch(str, pattern);
                    out[i] = negate ? !m : m;
                }
            }
            return;
        }
        for (idx_t i = 0; i < count; i++) {
            if (!left.GetValidity().RowIsValid(i) || !right.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
                out[i] = false;
            } else {
                auto str = left.GetData<string_t>()[i].GetString();
                auto pattern = right.GetData<string_t>()[i].GetString();
                if (case_insensitive) {
                    for (auto &c : str) c = static_cast<char>(std::tolower(c));
                    for (auto &c : pattern) c = static_cast<char>(std::tolower(c));
                }
                bool m = LikeMatch(str, pattern);
                out[i] = negate ? !m : m;
            }
        }
        return;
    }

    auto left_phys = left.GetType().GetInternalType();
    auto right_phys = right.GetType().GetInternalType();

    // If types match, use typed comparison for speed.
    if (left_phys == right_phys) {
        switch (left_phys) {
        case PhysicalType::BOOL:
            CompareTyped<bool>(expr.op, left, right, result, count); break;
        case PhysicalType::INT8:
            CompareTyped<int8_t>(expr.op, left, right, result, count); break;
        case PhysicalType::INT16:
            CompareTyped<int16_t>(expr.op, left, right, result, count); break;
        case PhysicalType::INT32:
            CompareTyped<int32_t>(expr.op, left, right, result, count); break;
        case PhysicalType::INT64:
            CompareTyped<int64_t>(expr.op, left, right, result, count); break;
        case PhysicalType::UINT8:
            CompareTyped<uint8_t>(expr.op, left, right, result, count); break;
        case PhysicalType::UINT16:
            CompareTyped<uint16_t>(expr.op, left, right, result, count); break;
        case PhysicalType::UINT32:
            CompareTyped<uint32_t>(expr.op, left, right, result, count); break;
        case PhysicalType::UINT64:
            CompareTyped<uint64_t>(expr.op, left, right, result, count); break;
        case PhysicalType::FLOAT:
            CompareTyped<float>(expr.op, left, right, result, count); break;
        case PhysicalType::DOUBLE:
            CompareTyped<double>(expr.op, left, right, result, count); break;
        case PhysicalType::VARCHAR:
            CompareTyped<string_t>(expr.op, left, right, result, count); break;
        default:
            throw NotImplementedException("Comparison for type " + left.GetType().ToString());
        }
    } else {
        // Mixed types: numeric coercion via stod. Non-numeric VARCHAR
        // operands previously leaked std::invalid_argument /
        // std::out_of_range from stod up the stack — every WHERE clause
        // mixing a VARCHAR column with an integer literal crashed when
        // any row's string wasn't numeric. Now: catch and raise
        // ConversionException with a clear message; SQL-92 says
        // unparseable values should be a typed error, not a process
        // crash.
        auto *out = result.GetData<bool>();
        for (idx_t i = 0; i < count; i++) {
            auto lv = left.GetValue(i), rv = right.GetValue(i);
            if (lv.IsNull() || rv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                out[i] = false;
                continue;
            }
            double ld = 0.0, rd = 0.0;
            try {
                ld = std::stod(lv.ToString());
                rd = std::stod(rv.ToString());
            } catch (const std::exception &) {
                throw ConversionException(
                    "Cannot compare values '" + lv.ToString() + "' and '" +
                    rv.ToString() + "' as numeric");
            }
            if (expr.op == "=") out[i] = ld == rd;
            else if (expr.op == "!=" || expr.op == "<>") out[i] = ld != rd;
            else if (expr.op == "<") out[i] = ld < rd;
            else if (expr.op == ">") out[i] = ld > rd;
            else if (expr.op == "<=") out[i] = ld <= rd;
            else if (expr.op == ">=") out[i] = ld >= rd;
            else out[i] = false;
        }
    }
}

void ExpressionExecutor::ExecuteConjunction(const BoundConjunction &expr, DataChunk &input,
                                             Vector &result, idx_t count) {
    Vector left(LogicalType::BOOLEAN(), count);
    Vector right(LogicalType::BOOLEAN(), count);
    Execute(*expr.left, input, left, count);
    Execute(*expr.right, input, right, count);

    auto *ldata = left.GetData<bool>();
    auto *rdata = right.GetData<bool>();
    auto *out = result.GetData<bool>();
    auto &lvalid = left.GetValidity();
    auto &rvalid = right.GetValidity();
    auto &ovalid = result.GetValidity();

    // SQL-92 Kleene three-valued logic:
    //   AND: FALSE wins over NULL  (FALSE AND NULL = FALSE)
    //        TRUE AND NULL = NULL, NULL AND NULL = NULL
    //   OR : TRUE wins over NULL   (TRUE OR NULL = TRUE)
    //        FALSE OR NULL = NULL, NULL OR NULL = NULL
    // The previous code read ldata/rdata without consulting validity,
    // so `NULL AND TRUE` (left invalid, right=true) computed `0 && 1`
    // and returned FALSE — a silent wrong-result bug in every WHERE
    // clause that mixed NULL-producing predicates with TRUE/FALSE.
    bool is_and = (expr.op == "AND");
    for (idx_t i = 0; i < count; i++) {
        bool l_valid = lvalid.RowIsValid(i);
        bool r_valid = rvalid.RowIsValid(i);
        bool l = l_valid && ldata[i];
        bool r = r_valid && rdata[i];

        if (is_and) {
            // FALSE on either side wins regardless of the other's validity.
            if ((l_valid && !l) || (r_valid && !r)) {
                ovalid.SetValid(i);
                out[i] = false;
            } else if (!l_valid || !r_valid) {
                ovalid.SetInvalid(i);
                out[i] = false;
            } else {
                ovalid.SetValid(i);
                out[i] = l && r;
            }
        } else {  // OR
            // TRUE on either side wins regardless of the other's validity.
            if ((l_valid && l) || (r_valid && r)) {
                ovalid.SetValid(i);
                out[i] = true;
            } else if (!l_valid || !r_valid) {
                ovalid.SetInvalid(i);
                out[i] = false;
            } else {
                ovalid.SetValid(i);
                out[i] = l || r;
            }
        }
    }
}

template <typename T>
void ExpressionExecutor::ArithmeticTyped(const std::string &op, Vector &left, Vector &right,
                                          Vector &result, idx_t count) {
    auto *ldata = left.GetData<T>();
    auto *rdata = right.GetData<T>();
    auto *out = result.GetData<T>();

    for (idx_t i = 0; i < count; i++) {
        if (!left.GetValidity().RowIsValid(i) || !right.GetValidity().RowIsValid(i)) {
            result.GetValidity().SetInvalid(i);
            out[i] = T{};
            continue;
        }
        if constexpr (std::is_integral_v<T>) {
            // Signed integer overflow is undefined behaviour in C++ and
            // silently wraps on x86 — `2147483647 + 1` produced
            // `-2147483648` even though the user got no error. SQL
            // standard says overflow is a runtime error; we return NULL
            // to match the divide-by-zero / modulo-by-zero behaviour
            // already established by the +/-/* paths.
            T a = ldata[i], b = rdata[i];
            T r{};
            bool overflow = false;
            if (op == "+") {
                if ((b > 0 && a > std::numeric_limits<T>::max() - b) ||
                    (b < 0 && a < std::numeric_limits<T>::min() - b)) {
                    overflow = true;
                } else r = a + b;
            } else if (op == "-") {
                if ((b < 0 && a > std::numeric_limits<T>::max() + b) ||
                    (b > 0 && a < std::numeric_limits<T>::min() + b)) {
                    overflow = true;
                } else r = a - b;
            } else if (op == "*") {
                if (a != 0 && b != 0) {
                    T m = a * b;
                    // Detect overflow via re-division check (portable;
                    // MSVC has no __builtin_mul_overflow).
                    if (m / a != b) overflow = true;
                    else r = m;
                } // else r = 0 (already)
            } else if (op == "/") {
                if (b == 0) {
                    overflow = true;  // -> NULL
                } else if (a == std::numeric_limits<T>::min() && b == -1) {
                    overflow = true;  // INT_MIN / -1 overflows
                } else {
                    r = a / b;
                }
            } else {
                r = T{};
            }
            if (overflow) {
                result.GetValidity().SetInvalid(i);
                out[i] = T{};
            } else {
                out[i] = r;
            }
        } else {
            // Float / double: IEEE-754 inf/nan is the standard behaviour
            // for overflow; SQL queries expect that, so don't NULL it.
            if (op == "+")       out[i] = ldata[i] + rdata[i];
            else if (op == "-")  out[i] = ldata[i] - rdata[i];
            else if (op == "*")  out[i] = ldata[i] * rdata[i];
            else if (op == "/") {
                if (rdata[i] == T{}) {
                    result.GetValidity().SetInvalid(i);
                    out[i] = T{};
                } else {
                    out[i] = ldata[i] / rdata[i];
                }
            }
            else out[i] = T{};
        }
    }
}

// Cast a numeric Vector to `target_type` if its current type doesn't match.
// Returns the original Vector by-move if no cast is needed; otherwise builds
// a new Vector of target_type with converted values. Falls back to Value
// boxing for uncommon conversions; the typed branches are inlined for the
// hot int32/int64 -> double path that hits whenever a numeric literal is
// mixed with a DOUBLE-typed aggregate (e.g. AVG(x) + 1, AVG(x)/COUNT(*)).
static Vector CoerceVector(Vector &&src, const LogicalType &target_type, idx_t count) {
    if (src.GetType().id() == target_type.id()) return std::move(src);
    Vector out(target_type, count);
    auto src_id = src.GetType().id();
    auto dst_id = target_type.id();

    auto copy_validity = [&]() {
        for (idx_t i = 0; i < count; i++) {
            if (!src.GetValidity().RowIsValid(i)) out.GetValidity().SetInvalid(i);
        }
    };

    if (dst_id == LogicalTypeId::DOUBLE) {
        auto *o = out.GetData<double>();
        if (src_id == LogicalTypeId::INTEGER) {
            auto *s = src.GetData<int32_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<double>(s[i]);
            copy_validity(); return out;
        }
        if (src_id == LogicalTypeId::BIGINT) {
            auto *s = src.GetData<int64_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<double>(s[i]);
            copy_validity(); return out;
        }
        if (src_id == LogicalTypeId::FLOAT) {
            auto *s = src.GetData<float>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<double>(s[i]);
            copy_validity(); return out;
        }
    } else if (dst_id == LogicalTypeId::BIGINT) {
        auto *o = out.GetData<int64_t>();
        if (src_id == LogicalTypeId::INTEGER) {
            auto *s = src.GetData<int32_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<int64_t>(s[i]);
            copy_validity(); return out;
        }
    } else if (dst_id == LogicalTypeId::FLOAT) {
        auto *o = out.GetData<float>();
        if (src_id == LogicalTypeId::INTEGER) {
            auto *s = src.GetData<int32_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<float>(s[i]);
            copy_validity(); return out;
        }
        if (src_id == LogicalTypeId::BIGINT) {
            auto *s = src.GetData<int64_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<float>(s[i]);
            copy_validity(); return out;
        }
    }
    // Fallback: per-row boxed conversion.
    for (idx_t i = 0; i < count; i++) {
        if (!src.GetValidity().RowIsValid(i)) {
            out.GetValidity().SetInvalid(i); continue;
        }
        out.SetValue(i, src.GetValue(i));
    }
    return out;
}

void ExpressionExecutor::ExecuteArithmetic(const BoundArithmetic &expr, DataChunk &input,
                                            Vector &result, idx_t count) {
    Vector left(expr.left->GetReturnType(), count);
    Vector right(expr.right->GetReturnType(), count);
    Execute(*expr.left, input, left, count);
    Execute(*expr.right, input, right, count);

    // Promote operands to result type so ArithmeticTyped<T> below reads the
    // right physical layout. Without this, mixing AVG (DOUBLE) with an
    // INTEGER literal turns into a reinterpret-cast on raw int bytes.
    //
    // Modulo used to skip this coercion. That caused
    //   SELECT null % null
    // to dereference a SQLNULL-typed buffer as int32, segfaulting on
    // operator.slt L268 and any user query with NULL % NULL. Including
    // modulo in the coercion makes the operand layout consistent with
    // +/-/*//; the existing modulo branch then reads the correct types.
    // String concatenation (||) still skips because it converts via
    // Value::ToString and doesn't need typed-buffer layout.
    if (expr.op != "||") {
        left = CoerceVector(std::move(left), result.GetType(), count);
        right = CoerceVector(std::move(right), result.GetType(), count);
    }

    // Handle % modulo for integers. NULL propagates: if either operand
    // is invalid (NULL), the result row is NULL. The pre-fix code wrote
    // garbage into out[i] for those rows; now they're explicitly invalid.
    if (expr.op == "%") {
        auto physical = result.GetType().GetInternalType();
        auto &lv = left.GetValidity();
        auto &rv = right.GetValidity();
        auto &ov = result.GetValidity();
        bool lvalid_all = lv.AllValid();
        bool rvalid_all = rv.AllValid();
        if (physical == PhysicalType::INT32) {
            auto *ld = left.GetData<int32_t>(), *rd = right.GetData<int32_t>();
            auto *out = result.GetData<int32_t>();
            for (idx_t i = 0; i < count; i++) {
                bool li = lvalid_all || lv.RowIsValid(i);
                bool ri = rvalid_all || rv.RowIsValid(i);
                if (!li || !ri || rd[i] == 0) { ov.SetInvalid(i); out[i] = 0; }
                else out[i] = ld[i] % rd[i];
            }
        } else if (physical == PhysicalType::INT64) {
            auto *ld = left.GetData<int64_t>(), *rd = right.GetData<int64_t>();
            auto *out = result.GetData<int64_t>();
            for (idx_t i = 0; i < count; i++) {
                bool li = lvalid_all || lv.RowIsValid(i);
                bool ri = rvalid_all || rv.RowIsValid(i);
                if (!li || !ri || rd[i] == 0) { ov.SetInvalid(i); out[i] = 0; }
                else out[i] = ld[i] % rd[i];
            }
        } else {
            // Float modulo via fmod.
            auto *ld = left.GetData<double>(), *rd = right.GetData<double>();
            auto *out = result.GetData<double>();
            for (idx_t i = 0; i < count; i++) {
                bool li = lvalid_all || lv.RowIsValid(i);
                bool ri = rvalid_all || rv.RowIsValid(i);
                if (!li || !ri || rd[i] == 0.0) { ov.SetInvalid(i); out[i] = 0.0; }
                else out[i] = std::fmod(ld[i], rd[i]);
            }
        }
        return;
    }

    // Handle || for string concatenation.
    if (expr.op == "||") {
        for (idx_t i = 0; i < count; i++) {
            auto lv = left.GetValue(i), rv = right.GetValue(i);
            if (lv.IsNull() || rv.IsNull()) {
                result.GetValidity().SetInvalid(i);
            } else {
                result.SetValue(i, Value::VARCHAR(lv.ToString() + rv.ToString()));
            }
        }
        return;
    }

    auto physical = result.GetType().GetInternalType();
    switch (physical) {
    case PhysicalType::INT32:
        ArithmeticTyped<int32_t>(expr.op, left, right, result, count); break;
    case PhysicalType::INT64:
        ArithmeticTyped<int64_t>(expr.op, left, right, result, count); break;
    case PhysicalType::FLOAT:
        ArithmeticTyped<float>(expr.op, left, right, result, count); break;
    case PhysicalType::DOUBLE:
        ArithmeticTyped<double>(expr.op, left, right, result, count); break;
    default:
        throw NotImplementedException("Arithmetic for type " + result.GetType().ToString());
    }
}

void ExpressionExecutor::ExecuteNegation(const BoundNegation &expr, DataChunk &input,
                                          Vector &result, idx_t count) {
    Vector child(LogicalType::BOOLEAN(), count);
    Execute(*expr.child, input, child, count);
    auto *cdata = child.GetData<bool>();
    auto *out = result.GetData<bool>();
    auto &cv = child.GetValidity();
    auto &ov = result.GetValidity();
    for (idx_t i = 0; i < count; i++) {
        // SQL Kleene logic: NOT UNKNOWN = UNKNOWN. Propagate the
        // child's invalid bit so NOT (x IN (1, 2, NULL)) returns
        // NULL/UNKNOWN when x doesn't equal any concrete element,
        // matching SQL three-valued logic.
        if (!cv.RowIsValid(i)) {
            ov.SetInvalid(i);
            out[i] = false;
        } else {
            out[i] = !cdata[i];
        }
    }
}

void ExpressionExecutor::ExecuteIsNull(const BoundIsNull &expr, DataChunk &input,
                                        Vector &result, idx_t count) {
    Vector child(expr.child->GetReturnType(), count);
    Execute(*expr.child, input, child, count);
    auto *out = result.GetData<bool>();
    for (idx_t i = 0; i < count; i++) {
        bool is_null = !child.GetValidity().RowIsValid(i);
        out[i] = expr.is_not ? !is_null : is_null;
    }
}

// SQL-92 three-valued logic predicates: x IS [NOT] {TRUE | FALSE | UNKNOWN}.
// Result is always BOOLEAN, never NULL. The child may be NULL — that's
// the whole point of the predicate. UNKNOWN is the SQL spelling for
// "the boolean expression evaluated to NULL", i.e. equivalent to IS NULL
// applied to a boolean operand.
void ExpressionExecutor::ExecuteIsBool(const BoundIsBool &expr, DataChunk &input,
                                        Vector &result, idx_t count) {
    Vector child(expr.child->GetReturnType(), count);
    Execute(*expr.child, input, child, count);
    auto *out = result.GetData<bool>();
    auto &validity = child.GetValidity();
    bool is_null_child_type = (expr.child->GetReturnType().id() == LogicalTypeId::SQLNULL);
    auto pred = expr.pred;
    bool negate = expr.is_not;

    for (idx_t i = 0; i < count; i++) {
        bool valid = !is_null_child_type && validity.RowIsValid(i);
        bool match;
        if (pred == BoundIsBool::Predicate::UNKNOWN_) {
            match = !valid;
        } else if (!valid) {
            // NULL IS TRUE/FALSE — both false (NULL is neither TRUE nor FALSE).
            match = false;
        } else {
            bool b = child.GetData<bool>()[i];
            match = (pred == BoundIsBool::Predicate::TRUE_) ? b : !b;
        }
        out[i] = negate ? !match : match;
        // Result is BOOLEAN, never NULL — IS-predicates close the
        // three-valued logic to two-valued.
    }
}

void ExpressionExecutor::ExecuteUnaryMinus(const BoundUnaryMinus &expr, DataChunk &input,
                                            Vector &result, idx_t count) {
    Vector child(expr.child->GetReturnType(), count);
    Execute(*expr.child, input, child, count);

    // Propagate NULL: -NULL is NULL in SQL. Previously the per-element
    // loop unconditionally negated the underlying byte (so -CAST(NULL AS
    // DOUBLE) printed as "-0" instead of NULL). Set the result's invalid
    // bits up-front from the child's; the typed loop still runs but the
    // garbage value is masked.
    auto &cvalid = child.GetValidity();
    auto &ovalid = result.GetValidity();
    auto physical = result.GetType().GetInternalType();
    switch (physical) {
    case PhysicalType::INT16: {
        auto *src = child.GetData<int16_t>();
        auto *dst = result.GetData<int16_t>();
        for (idx_t i = 0; i < count; i++) {
            if (!cvalid.RowIsValid(i)) { ovalid.SetInvalid(i); dst[i] = 0; }
            else dst[i] = static_cast<int16_t>(-src[i]);
        }
        break;
    }
    case PhysicalType::INT32: {
        auto *src = child.GetData<int32_t>();
        auto *dst = result.GetData<int32_t>();
        for (idx_t i = 0; i < count; i++) {
            if (!cvalid.RowIsValid(i)) { ovalid.SetInvalid(i); dst[i] = 0; }
            else dst[i] = -src[i];
        }
        break;
    }
    case PhysicalType::INT64: {
        auto *src = child.GetData<int64_t>();
        auto *dst = result.GetData<int64_t>();
        for (idx_t i = 0; i < count; i++) {
            if (!cvalid.RowIsValid(i)) { ovalid.SetInvalid(i); dst[i] = 0; }
            else dst[i] = -src[i];
        }
        break;
    }
    case PhysicalType::FLOAT: {
        auto *src = child.GetData<float>();
        auto *dst = result.GetData<float>();
        for (idx_t i = 0; i < count; i++) {
            if (!cvalid.RowIsValid(i)) { ovalid.SetInvalid(i); dst[i] = 0.0f; }
            else dst[i] = -src[i];
        }
        break;
    }
    case PhysicalType::DOUBLE: {
        auto *src = child.GetData<double>();
        auto *dst = result.GetData<double>();
        for (idx_t i = 0; i < count; i++) {
            if (!cvalid.RowIsValid(i)) { ovalid.SetInvalid(i); dst[i] = 0.0; }
            else dst[i] = -src[i];
        }
        break;
    }
    default:
        throw NotImplementedException("Unary minus for type " +
                                       result.GetType().ToString());
    }
}

// ============================================================================
// Function execution (CASE, IN, scalar functions)
// ============================================================================

static bool LikeMatch(const std::string &str, const std::string &pattern) {
    // SQL LIKE pattern matching:
    //   %       any sequence of zero or more characters
    //   _       any single character
    //   \%, \_  literal % or _ (backslash escape)
    //   \\      literal backslash
    // ESCAPE clause is desugared at parse time into backslash escapes,
    // so the executor only needs to recognise the standard `\` form.
    size_t si = 0, pi = 0;
    size_t star_p = std::string::npos, star_s = 0;
    auto pattern_char_at = [&](size_t i, char &out, bool &is_meta) -> size_t {
        // Returns the number of pattern chars consumed (1 or 2). For an
        // escape sequence `\X`, returns 2 with `is_meta=false` and the
        // literal X. For a metachar (`%` or `_`), returns 1 with
        // `is_meta=true`. For a plain literal, returns 1 with
        // `is_meta=false`.
        if (i + 1 < pattern.size() && pattern[i] == '\\') {
            out = pattern[i + 1];
            is_meta = false;
            return 2;
        }
        out = pattern[i];
        is_meta = (out == '%' || out == '_');
        return 1;
    };
    while (si < str.size()) {
        if (pi < pattern.size()) {
            char pc;
            bool is_meta;
            size_t step = pattern_char_at(pi, pc, is_meta);
            if (is_meta && pc == '%') {
                star_p = pi;
                star_s = si;
                pi += step;
                continue;
            }
            if (is_meta && pc == '_') {
                si++;
                pi += step;
                continue;
            }
            // Literal (possibly via escape).
            if (pc == str[si]) {
                si++;
                pi += step;
                continue;
            }
        }
        if (star_p != std::string::npos) {
            // Backtrack: advance one in str, reset pi to the char after
            // the % (which is just star_p + 1 since `%` is always one
            // pattern char wide — backslash-escaped `%` can't be a star
            // anyway).
            pi = star_p + 1;
            si = ++star_s;
        } else {
            return false;
        }
    }
    // Skip any trailing `%` in the pattern.
    while (pi < pattern.size()) {
        if (pattern[pi] == '%') {
            pi++;
        } else {
            break;
        }
    }
    return pi == pattern.size();
}

void ExpressionExecutor::ExecuteFunction(const BoundFunction &expr, DataChunk &input,
                                          Vector &result, idx_t count) {
    auto &name = expr.function_name;

    // CASE(when1, then1, when2, then2, ..., [else])
    // IF(c, t, f) and IIF(c, t, f) reach the executor with the same arg
    // layout (cond, then, else) so we route them through this path.
    //
    // Vectorized form: evaluate each branch's when/then over the FULL chunk
    // once, then select per-row. The earlier per-row implementation was
    // O(count^2) — it re-Executed when_vec for every row, so a 2048-row
    // chunk paid 2048× the cost of a single Execute. A CASE-heavy query timed
    // out >30s entirely in that loop. Now we Execute once per branch.
    if (name == "CASE" || name == "IF" || name == "IIF") {
        const size_t nargs = expr.arguments.size();
        const bool has_else = (nargs % 2 == 1);
        // Track which output rows are still unfilled (haven't matched a WHEN).
        std::vector<uint8_t> need(count, 1);
        idx_t remaining = count;
        for (size_t a = 0; a + 1 < nargs && remaining > 0; a += 2) {
            Vector when_vec(LogicalType::BOOLEAN(), count);
            Execute(*expr.arguments[a], input, when_vec, count);
            // Find rows that match THIS branch (when==true and not yet filled).
            std::vector<idx_t> hits;
            hits.reserve(remaining);
            const bool *wd = when_vec.GetData<bool>();
            const auto &wv = when_vec.GetValidity();
            for (idx_t i = 0; i < count; i++) {
                if (need[i] && wv.RowIsValid(i) && wd[i]) hits.push_back(i);
            }
            if (hits.empty()) continue;
            Vector then_vec(expr.GetReturnType(), count);
            Execute(*expr.arguments[a + 1], input, then_vec, count);
            for (idx_t i : hits) {
                result.SetValue(i, then_vec.GetValue(i));
                need[i] = 0;
            }
            remaining -= hits.size();
        }
        if (remaining > 0) {
            if (has_else) {
                Vector else_vec(expr.GetReturnType(), count);
                Execute(*expr.arguments.back(), input, else_vec, count);
                for (idx_t i = 0; i < count; i++) {
                    if (need[i]) result.SetValue(i, else_vec.GetValue(i));
                }
            } else {
                for (idx_t i = 0; i < count; i++) {
                    if (need[i]) result.GetValidity().SetInvalid(i);
                }
            }
        }
        return;
    }

    // BETWEEN(value, low, high)
    if (name == "BETWEEN") {
        auto *out = result.GetData<bool>();
        for (idx_t i = 0; i < count; i++) {
            Vector val_vec(expr.arguments[0]->GetReturnType(), count);
            Vector low_vec(expr.arguments[1]->GetReturnType(), count);
            Vector high_vec(expr.arguments[2]->GetReturnType(), count);
            Execute(*expr.arguments[0], input, val_vec, count);
            Execute(*expr.arguments[1], input, low_vec, count);
            Execute(*expr.arguments[2], input, high_vec, count);
            auto v = val_vec.GetValue(i);
            auto lo = low_vec.GetValue(i);
            auto hi = high_vec.GetValue(i);
            if (v.IsNull() || lo.IsNull() || hi.IsNull()) {
                result.GetValidity().SetInvalid(i);
                out[i] = false;
            } else {
                out[i] = !(v < lo) && !(hi < v);
            }
        }
        return;
    }

    // IN(value, list...)
    if (name == "IN") {
        // SQL three-valued logic for IN:
        //   x IN (a, b, ..., NULL)
        //     true   if x equals any non-null element
        //     NULL   if x is NULL, or any element is NULL and no
        //            non-null equality matched
        //     false  otherwise
        // The pre-fix code returned false for NULL-in-list cases,
        // which (when wrapped by NOT) produced wrong-result rows for
        // any query using NOT IN with a NULL value in the list.
        auto *out = result.GetData<bool>();
        // Pre-evaluate the LHS and list args once for the chunk; the
        // previous code re-ran Execute inside the per-row loop, which
        // was both slow and produced wrong validity for chained args.
        Vector val_vec(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, val_vec, count);
        std::vector<Vector> list_vecs;
        list_vecs.reserve(expr.arguments.size() - 1);
        for (size_t a = 1; a < expr.arguments.size(); a++) {
            list_vecs.emplace_back(expr.arguments[a]->GetReturnType(), count);
            Execute(*expr.arguments[a], input, list_vecs.back(), count);
        }
        for (idx_t i = 0; i < count; i++) {
            auto val = val_vec.GetValue(i);
            if (val.IsNull()) {
                // NULL IN anything is NULL.
                result.GetValidity().SetInvalid(i);
                out[i] = false;
                continue;
            }
            bool found = false;
            bool saw_null = false;
            for (size_t a = 0; a < list_vecs.size(); a++) {
                auto e = list_vecs[a].GetValue(i);
                if (e.IsNull()) {
                    saw_null = true;
                    continue;
                }
                if (val == e) {
                    found = true;
                    break;
                }
            }
            if (found) {
                out[i] = true;
            } else if (saw_null) {
                result.GetValidity().SetInvalid(i);
                out[i] = false;
            } else {
                out[i] = false;
            }
        }
        return;
    }

    // ---- Scalar string functions ----

    if (name == "LENGTH" || name == "STRLEN") {
        auto *out = result.GetData<int32_t>();
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            if (!arg.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
            } else {
                out[i] = static_cast<int32_t>(arg.GetData<string_t>()[i].GetSize());
            }
        }
        return;
    }

    // ASCII(s) — code point of first byte (1-byte semantics; multibyte
    // returns the first byte's value, matching slothdb's byte-oriented
    // LEFT/RIGHT/SUBSTRING). NULL -> NULL; empty -> 0.
    if (name == "ASCII") {
        auto *out = result.GetData<int32_t>();
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            auto s = v.GetValue<std::string>();
            out[i] = s.empty() ? 0 : static_cast<int32_t>(static_cast<unsigned char>(s[0]));
        }
        return;
    }

    // CHR(n) — character with byte value n. NULL -> NULL. Out-of-range
    // values are clamped to 0..127 (7-bit ASCII) for now — full Unicode
    // codepoint handling deferred to a multibyte-aware string layer.
    if (name == "CHR") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            int32_t n = v.GetValue<int32_t>();
            if (n == 0) {
                result.SetValue(i, Value::VARCHAR(""));
            } else {
                // Clip to 7-bit ASCII byte until multibyte support lands.
                unsigned char byte = static_cast<unsigned char>(n & 0xFF);
                result.SetValue(i, Value::VARCHAR(std::string(1, static_cast<char>(byte))));
            }
        }
        return;
    }

    if (name == "UPPER" || name == "LOWER") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            if (!arg.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
            } else {
                auto s = arg.GetData<string_t>()[i].GetString();
                for (auto &c : s) {
                    c = name == "UPPER" ? static_cast<char>(std::toupper(c))
                                        : static_cast<char>(std::tolower(c));
                }
                result.SetValue(i, Value::VARCHAR(s));
            }
        }
        return;
    }

    if (name == "SUBSTRING" || name == "SUBSTR") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector start_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, start_vec, count);
        bool has_len = expr.arguments.size() > 2;
        Vector len_vec(LogicalType::INTEGER(), count);
        if (has_len) Execute(*expr.arguments[2], input, len_vec, count);

        for (idx_t i = 0; i < count; i++) {
            // NULL propagation: if string OR start OR len is NULL the
            // result is NULL. Previously a NULL start crashed with
            // "Cannot get value from NULL" from the typed GetValue.
            bool null_in = !str_vec.GetValidity().RowIsValid(i) ||
                           !start_vec.GetValidity().RowIsValid(i) ||
                           (has_len && !len_vec.GetValidity().RowIsValid(i));
            if (null_in) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto sv = str_vec.GetValue(i);
            if (sv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            int32_t start_raw = start_vec.GetValue(i).GetValue<int32_t>();
            int32_t len_raw = has_len ? len_vec.GetValue(i).GetValue<int32_t>()
                                       : static_cast<int32_t>(s.size());
            // SQL standard: positions are 1-based; positions <1 are
            // clipped but the "missing" characters STILL count against
            // len. So SUBSTRING('hello', -2, 5) covers logical
            // positions -2,-1,0,1,2 → physical "he". A negative len
            // produces empty. Out-of-range start produces empty.
            if (len_raw <= 0) {
                result.SetValue(i, Value::VARCHAR(""));
                continue;
            }
            int64_t effective_start = std::max<int32_t>(1, start_raw);
            int64_t effective_end =
                static_cast<int64_t>(start_raw) + static_cast<int64_t>(len_raw);
            if (effective_end <= 1) {
                result.SetValue(i, Value::VARCHAR(""));
                continue;
            }
            int64_t out_start = effective_start - 1;
            int64_t out_len = effective_end - effective_start;
            int64_t s_size = static_cast<int64_t>(s.size());
            if (out_start >= s_size || out_len <= 0) {
                result.SetValue(i, Value::VARCHAR(""));
                continue;
            }
            out_len = std::min<int64_t>(out_len, s_size - out_start);
            result.SetValue(i, Value::VARCHAR(
                s.substr(static_cast<size_t>(out_start),
                         static_cast<size_t>(out_len))));
        }
        return;
    }

    if (name == "REPLACE") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector from_vec(expr.arguments[1]->GetReturnType(), count);
        Vector to_vec(expr.arguments[2]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, from_vec, count);
        Execute(*expr.arguments[2], input, to_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto fv = from_vec.GetValue(i);
            auto tv = to_vec.GetValue(i);
            if (sv.IsNull() || fv.IsNull() || tv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto from = fv.GetValue<std::string>();
            auto to = tv.GetValue<std::string>();
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.length(), to);
                pos += to.length();
            }
            result.SetValue(i, Value::VARCHAR(s));
        }
        return;
    }

    if (name == "CONCAT") {
        for (idx_t i = 0; i < count; i++) {
            std::string concat_result;
            for (auto &arg : expr.arguments) {
                Vector v(arg->GetReturnType(), count);
                Execute(*arg, input, v, count);
                auto val = v.GetValue(i);
                if (!val.IsNull()) concat_result += val.ToString();
            }
            result.SetValue(i, Value::VARCHAR(concat_result));
        }
        return;
    }

    // CONCAT_WS(separator, val1, val2, ...) — concatenate with a
    // separator. NULL separator -> NULL result for that row (PG rule).
    // NULL values are skipped (no separator emitted for them). All-NULL
    // values with a non-NULL separator -> empty string.
    if (name == "CONCAT_WS") {
        // Need at least the separator; binder enforces arity.
        if (expr.arguments.empty()) {
            for (idx_t i = 0; i < count; i++) {
                result.SetValue(i, Value::VARCHAR(""));
            }
            return;
        }
        Vector sep_vec(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, sep_vec, count);
        // Pre-evaluate value args once.
        std::vector<Vector> val_vecs;
        val_vecs.reserve(expr.arguments.size() - 1);
        for (size_t a = 1; a < expr.arguments.size(); a++) {
            val_vecs.emplace_back(expr.arguments[a]->GetReturnType(), count);
            Execute(*expr.arguments[a], input, val_vecs.back(), count);
        }
        for (idx_t i = 0; i < count; i++) {
            auto sv = sep_vec.GetValue(i);
            if (sv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            std::string sep = sv.GetValue<std::string>();
            std::string out;
            bool first = true;
            for (size_t a = 0; a < val_vecs.size(); a++) {
                auto v = val_vecs[a].GetValue(i);
                if (v.IsNull()) continue;
                if (!first) out += sep;
                out += v.ToString();
                first = false;
            }
            result.SetValue(i, Value::VARCHAR(out));
        }
        return;
    }

    if (name == "TRIM" || name == "LTRIM" || name == "RTRIM") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = v.GetValue<std::string>();
            if (name == "TRIM" || name == "LTRIM") {
                s.erase(0, s.find_first_not_of(" \t\n\r"));
            }
            if (name == "TRIM" || name == "RTRIM") {
                size_t last = s.find_last_not_of(" \t\n\r");
                if (last == std::string::npos) s.clear();
                else s.erase(last + 1);
            }
            result.SetValue(i, Value::VARCHAR(s));
        }
        return;
    }

    // ---- Scalar math functions ----

    if (name == "ABS") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        auto physical = arg.GetType().GetInternalType();
        for (idx_t i = 0; i < count; i++) {
            if (!arg.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            switch (physical) {
            case PhysicalType::INT32:
                result.GetData<int32_t>()[i] = std::abs(arg.GetData<int32_t>()[i]); break;
            case PhysicalType::INT64:
                result.GetData<int64_t>()[i] = std::abs(arg.GetData<int64_t>()[i]); break;
            case PhysicalType::DOUBLE:
                result.GetData<double>()[i] = std::abs(arg.GetData<double>()[i]); break;
            case PhysicalType::FLOAT:
                result.GetData<float>()[i] = std::abs(arg.GetData<float>()[i]); break;
            default: break;
            }
        }
        return;
    }

    if (name == "CEIL" || name == "CEILING" || name == "FLOOR" || name == "ROUND") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        // ROUND can take an optional second arg `precision`: positive
        // values control fractional digits, negative round to powers
        // of 10, zero is integer-round. Previously the second arg was
        // silently dropped — ROUND(2.345, 2) returned 2 instead of 2.35.
        bool has_precision = (name == "ROUND" && expr.arguments.size() >= 2);
        Vector prec_vec(LogicalType::INTEGER(), count);
        if (has_precision) Execute(*expr.arguments[1], input, prec_vec, count);
        for (idx_t i = 0; i < count; i++) {
            if (!arg.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            if (has_precision && !prec_vec.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            double val = 0;
            auto phys = arg.GetType().GetInternalType();
            if (phys == PhysicalType::DOUBLE) val = arg.GetData<double>()[i];
            else if (phys == PhysicalType::FLOAT) val = arg.GetData<float>()[i];
            else if (phys == PhysicalType::INT32) val = arg.GetData<int32_t>()[i];
            else if (phys == PhysicalType::INT64) val = static_cast<double>(arg.GetData<int64_t>()[i]);

            if (name == "CEIL" || name == "CEILING") val = std::ceil(val);
            else if (name == "FLOOR") val = std::floor(val);
            else if (name == "ROUND") {
                if (has_precision) {
                    int32_t p = prec_vec.GetValue(i).GetValue<int32_t>();
                    double factor = std::pow(10.0, static_cast<double>(p));
                    val = std::round(val * factor) / factor;
                } else {
                    val = std::round(val);
                }
            }

            result.GetData<double>()[i] = val;
        }
        return;
    }

    if (name == "SQRT" || name == "POWER" || name == "MOD") {
        Vector arg1(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg1, count);
        if (name == "SQRT") {
            for (idx_t i = 0; i < count; i++) {
                auto val = arg1.GetValue(i);
                if (val.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
                double d = (val.type().id() == LogicalTypeId::INTEGER)
                    ? val.GetValue<int32_t>() : val.GetValue<double>();
                // SQRT of a negative produces nan; SQL standard says
                // this is a domain error -> NULL (matches POWER's
                // negative-base-with-fractional-exponent guard).
                if (d < 0.0) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                result.GetData<double>()[i] = std::sqrt(d);
            }
        } else {
            Vector arg2(expr.arguments[1]->GetReturnType(), count);
            Execute(*expr.arguments[1], input, arg2, count);
            for (idx_t i = 0; i < count; i++) {
                auto v1 = arg1.GetValue(i), v2 = arg2.GetValue(i);
                if (v1.IsNull() || v2.IsNull()) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                // GetValue<double> handles all numeric input types; the
                // prior std::stod(ToString()) round-trip burned CPU and
                // crashed on non-numeric strings.
                double d1, d2;
                try {
                    d1 = v1.type().id() == LogicalTypeId::DOUBLE
                            ? v1.GetValue<double>()
                            : std::stod(v1.ToString());
                    d2 = v2.type().id() == LogicalTypeId::DOUBLE
                            ? v2.GetValue<double>()
                            : std::stod(v2.ToString());
                } catch (...) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                if (name == "POWER") {
                    // Domain errors -> NULL per SQL standard:
                    //   POWER(0, -k)         -> +inf  (was)
                    //   POWER(negative, frac) -> nan
                    if ((d1 == 0.0 && d2 < 0.0) || (d1 < 0.0 && std::floor(d2) != d2)) {
                        result.GetValidity().SetInvalid(i);
                        continue;
                    }
                    double r = std::pow(d1, d2);
                    if (std::isnan(r) || std::isinf(r)) {
                        result.GetValidity().SetInvalid(i);
                    } else {
                        result.GetData<double>()[i] = r;
                    }
                } else {
                    // MOD(x, 0) -> NULL (was nan). Integer % already
                    // returns NULL via the ExecuteArithmetic %-branch;
                    // function-form MOD now matches.
                    if (d2 == 0.0) {
                        result.GetValidity().SetInvalid(i);
                        continue;
                    }
                    result.GetData<double>()[i] = std::fmod(d1, d2);
                }
            }
        }
        return;
    }

    // ---- Additional string functions ----

    if (name == "POSITION" || name == "STRPOS") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector sub_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, sub_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto subv = sub_vec.GetValue(i);
            if (sv.IsNull() || subv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto sub = subv.GetValue<std::string>();
            auto pos = s.find(sub);
            result.SetValue(i, Value::INTEGER(pos == std::string::npos ? 0 : static_cast<int32_t>(pos + 1)));
        }
        return;
    }

    if (name == "LEFT" || name == "RIGHT") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector n_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, n_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto nv = n_vec.GetValue(i);
            if (sv.IsNull() || nv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto n = nv.GetValue<int32_t>();
            // Postgres semantics for negative n:
            //   LEFT(s, -k)  = s[: len(s)-k] (drop last k)
            //   RIGHT(s, -k) = s[k :]        (drop first k)
            // Clamps to empty when k >= len(s).
            if (n < 0) {
                int64_t drop = -static_cast<int64_t>(n);
                int64_t keep = static_cast<int64_t>(s.size()) - drop;
                if (keep <= 0) {
                    result.SetValue(i, Value::VARCHAR(""));
                } else {
                    auto k = static_cast<size_t>(keep);
                    result.SetValue(i, Value::VARCHAR(
                        name == "LEFT" ? s.substr(0, k) : s.substr(s.size() - k)));
                }
                continue;
            }
            auto un = static_cast<size_t>(n);
            result.SetValue(i, Value::VARCHAR(
                name == "LEFT" ? s.substr(0, un) : s.substr(s.size() > un ? s.size() - un : 0)));
        }
        return;
    }

    if (name == "LPAD" || name == "RPAD") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector len_vec(expr.arguments[1]->GetReturnType(), count);
        Vector pad_vec(expr.arguments[2]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, len_vec, count);
        Execute(*expr.arguments[2], input, pad_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto nv = len_vec.GetValue(i);
            auto pv = pad_vec.GetValue(i);
            if (sv.IsNull() || nv.IsNull() || pv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto target_len = static_cast<size_t>(nv.GetValue<int32_t>());
            auto pad = pv.GetValue<std::string>();
            while (s.size() < target_len && !pad.empty()) {
                if (name == "LPAD") s = pad + s; else s = s + pad;
            }
            if (s.size() > target_len) s = s.substr(name == "LPAD" ? s.size() - target_len : 0, target_len);
            result.SetValue(i, Value::VARCHAR(s));
        }
        return;
    }

    if (name == "REVERSE") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = v.GetValue<std::string>();
            std::reverse(s.begin(), s.end());
            result.SetValue(i, Value::VARCHAR(s));
        }
        return;
    }

    if (name == "REPEAT") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector n_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, n_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto nv = n_vec.GetValue(i);
            if (sv.IsNull() || nv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto n = nv.GetValue<int32_t>();
            if (n < 0) n = 0;
            if (n > 65536) throw InvalidInputException("REPEAT count too large (max: 65536)");
            std::string r;
            for (int j = 0; j < n; j++) r += s;
            result.SetValue(i, Value::VARCHAR(r));
        }
        return;
    }

    // INSTR(haystack, needle) — Oracle/MySQL/SQLite spelling of STRPOS.
    // 1-based position of first occurrence, 0 if not found. NULL on
    // either side propagates.
    if (name == "INSTR") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector sub_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, sub_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto subv = sub_vec.GetValue(i);
            if (sv.IsNull() || subv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto sub = subv.GetValue<std::string>();
            auto pos = s.find(sub);
            result.SetValue(i, Value::INTEGER(pos == std::string::npos
                                              ? 0 : static_cast<int32_t>(pos + 1)));
        }
        return;
    }

    if (name == "STARTS_WITH" || name == "PREFIX") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector pre_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, pre_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto pv = pre_vec.GetValue(i);
            if (sv.IsNull() || pv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto p = pv.GetValue<std::string>();
            result.SetValue(i, Value::BOOLEAN(s.substr(0, p.size()) == p));
        }
        return;
    }

    if (name == "ENDS_WITH" || name == "SUFFIX") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector suf_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, suf_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto sufv = suf_vec.GetValue(i);
            if (sv.IsNull() || sufv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto sf = sufv.GetValue<std::string>();
            bool match = s.size() >= sf.size() && s.substr(s.size() - sf.size()) == sf;
            result.SetValue(i, Value::BOOLEAN(match));
        }
        return;
    }

    if (name == "CONTAINS") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector sub_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, sub_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto subv = sub_vec.GetValue(i);
            if (sv.IsNull() || subv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto sub = subv.GetValue<std::string>();
            result.SetValue(i, Value::BOOLEAN(s.find(sub) != std::string::npos));
        }
        return;
    }

    if (name == "SPLIT_PART") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector delim_vec(expr.arguments[1]->GetReturnType(), count);
        Vector idx_vec(expr.arguments[2]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, delim_vec, count);
        Execute(*expr.arguments[2], input, idx_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto dv = delim_vec.GetValue(i);
            auto iv = idx_vec.GetValue(i);
            if (sv.IsNull() || dv.IsNull() || iv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto d = dv.GetValue<std::string>();
            auto idx = iv.GetValue<int32_t>();
            // Multi-character delimiter: split via s.find(d). The
            // previous StringUtil::Split with d[0] silently truncated
            // multi-char delimiters to one byte, so SPLIT_PART(
            // 'aXXbXXc', 'XX', 2) returned '' instead of 'b'.
            std::vector<std::string> parts;
            if (d.empty()) {
                parts.push_back(s);
            } else {
                size_t pos = 0;
                while (pos <= s.size()) {
                    auto next = s.find(d, pos);
                    if (next == std::string::npos) {
                        parts.push_back(s.substr(pos));
                        break;
                    }
                    parts.push_back(s.substr(pos, next - pos));
                    pos = next + d.size();
                }
            }
            if (idx >= 1 && idx <= static_cast<int32_t>(parts.size()))
                result.SetValue(i, Value::VARCHAR(parts[idx - 1]));
            else
                result.SetValue(i, Value::VARCHAR(""));
        }
        return;
    }

    // ---- Additional math functions ----

    if (name == "LOG" || name == "LN") {
        // 1-arg form computes the natural log (matches existing
        // behaviour). 2-arg LOG(base, value) computes log base `base`
        // of `value`, previously silently dropped the second arg.
        // Domain errors (non-positive arg, base==1) return NULL.
        bool two_arg = (name == "LOG" && expr.arguments.size() >= 2);
        Vector arg0(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg0, count);
        Vector arg1(two_arg ? expr.arguments[1]->GetReturnType()
                            : LogicalType::DOUBLE(), count);
        if (two_arg) Execute(*expr.arguments[1], input, arg1, count);
        auto val_to_dbl = [](const Value &v) -> double {
            return v.type().id() == LogicalTypeId::INTEGER
                ? v.GetValue<int32_t>() : v.GetValue<double>();
        };
        for (idx_t i = 0; i < count; i++) {
            auto v0 = arg0.GetValue(i);
            if (v0.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            if (two_arg) {
                auto v1 = arg1.GetValue(i);
                if (v1.IsNull()) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                double base = val_to_dbl(v0);
                double val = val_to_dbl(v1);
                if (base <= 0.0 || base == 1.0 || val <= 0.0) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                double r = std::log(val) / std::log(base);
                if (std::isnan(r) || std::isinf(r)) {
                    result.GetValidity().SetInvalid(i);
                } else {
                    result.GetData<double>()[i] = r;
                }
            } else {
                double d = val_to_dbl(v0);
                if (d <= 0.0) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                result.GetData<double>()[i] = std::log(d);
            }
        }
        return;
    }

    if (name == "LOG2") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::INTEGER) ? v.GetValue<int32_t>() : v.GetValue<double>();
            if (d <= 0.0) { result.GetValidity().SetInvalid(i); continue; }
            result.GetData<double>()[i] = std::log2(d);
        }
        return;
    }

    if (name == "LOG10") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::INTEGER) ? v.GetValue<int32_t>() : v.GetValue<double>();
            if (d <= 0.0) { result.GetValidity().SetInvalid(i); continue; }
            result.GetData<double>()[i] = std::log10(d);
        }
        return;
    }

    if (name == "EXP") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::INTEGER) ? v.GetValue<int32_t>() : v.GetValue<double>();
            double r = std::exp(d);
            if (std::isnan(r) || std::isinf(r)) {
                result.GetValidity().SetInvalid(i);
            } else {
                result.GetData<double>()[i] = r;
            }
        }
        return;
    }

    if (name == "SIGN") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::INTEGER) ? v.GetValue<int32_t>() : v.GetValue<double>();
            result.SetValue(i, Value::INTEGER(d > 0 ? 1 : (d < 0 ? -1 : 0)));
        }
        return;
    }

    if (name == "PI") {
        for (idx_t i = 0; i < count; i++) {
            result.GetData<double>()[i] = 3.14159265358979323846;
        }
        return;
    }

    if (name == "RANDOM" || name == "RAND") {
        for (idx_t i = 0; i < count; i++) {
            result.GetData<double>()[i] = static_cast<double>(std::rand()) / RAND_MAX;
        }
        return;
    }

    if (name == "LEAST") {
        for (idx_t i = 0; i < count; i++) {
            Value min_val;
            bool first = true;
            for (auto &a : expr.arguments) {
                Vector v(a->GetReturnType(), count);
                Execute(*a, input, v, count);
                auto val = v.GetValue(i);
                if (val.IsNull()) continue;
                if (first || val < min_val) { min_val = val; first = false; }
            }
            if (first) result.GetValidity().SetInvalid(i);
            else result.SetValue(i, min_val);
        }
        return;
    }

    if (name == "GREATEST") {
        for (idx_t i = 0; i < count; i++) {
            Value max_val;
            bool first = true;
            for (auto &a : expr.arguments) {
                Vector v(a->GetReturnType(), count);
                Execute(*a, input, v, count);
                auto val = v.GetValue(i);
                if (val.IsNull()) continue;
                if (first || val > max_val) { max_val = val; first = false; }
            }
            if (first) result.GetValidity().SetInvalid(i);
            else result.SetValue(i, max_val);
        }
        return;
    }

    if (name == "INITCAP") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto s = arg.GetValue(i).GetValue<std::string>();
            bool cap_next = true;
            for (auto &c : s) {
                if (std::isalpha(c)) {
                    c = cap_next ? static_cast<char>(std::toupper(c)) : static_cast<char>(std::tolower(c));
                    cap_next = false;
                } else {
                    cap_next = true;
                }
            }
            result.SetValue(i, Value::VARCHAR(s));
        }
        return;
    }

    if (name == "SIN" || name == "COS" || name == "TAN" ||
        name == "ASIN" || name == "ACOS" || name == "ATAN" ||
        name == "SINH" || name == "COSH" || name == "TANH" ||
        name == "ASINH" || name == "ACOSH" || name == "ATANH") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::INTEGER) ? v.GetValue<int32_t>() :
                       (v.type().id() == LogicalTypeId::BIGINT)  ? static_cast<double>(v.GetValue<int64_t>()) :
                       (v.type().id() == LogicalTypeId::FLOAT)   ? v.GetValue<float>() :
                                                                   v.GetValue<double>();
            // Domain guards: out-of-range inputs would produce nan/inf
            // which leak into projection results, sort keys, and hash
            // groups. Match the LN/LOG/SQRT pattern of returning NULL
            // on domain error.
            if ((name == "ASIN" || name == "ACOS") && (d < -1.0 || d > 1.0)) {
                result.GetValidity().SetInvalid(i); continue;
            }
            if (name == "ACOSH" && d < 1.0) {
                result.GetValidity().SetInvalid(i); continue;
            }
            if (name == "ATANH" && (d <= -1.0 || d >= 1.0)) {
                result.GetValidity().SetInvalid(i); continue;
            }
            double r;
            if (name == "SIN") r = std::sin(d);
            else if (name == "COS") r = std::cos(d);
            else if (name == "TAN") r = std::tan(d);
            else if (name == "ASIN") r = std::asin(d);
            else if (name == "ACOS") r = std::acos(d);
            else if (name == "ATAN") r = std::atan(d);
            else if (name == "SINH") r = std::sinh(d);
            else if (name == "COSH") r = std::cosh(d);
            else if (name == "TANH") r = std::tanh(d);
            else if (name == "ASINH") r = std::asinh(d);
            else if (name == "ACOSH") r = std::acosh(d);
            else r = std::atanh(d);  // ATANH
            if (std::isnan(r) || std::isinf(r)) {
                result.GetValidity().SetInvalid(i);
            } else {
                result.GetData<double>()[i] = r;
            }
        }
        return;
    }

    if (name == "ISNAN" || name == "ISINF" || name == "ISFINITE") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::FLOAT) ? v.GetValue<float>() : v.GetValue<double>();
            bool r;
            if (name == "ISNAN")    r = std::isnan(d);
            else if (name == "ISINF") r = std::isinf(d);
            else                      r = std::isfinite(d);
            result.GetData<bool>()[i] = r;
        }
        return;
    }

    if (name == "ATAN2") {
        Vector a1(expr.arguments[0]->GetReturnType(), count);
        Vector a2(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, a1, count);
        Execute(*expr.arguments[1], input, a2, count);
        for (idx_t i = 0; i < count; i++) {
            auto v1 = a1.GetValue(i), v2 = a2.GetValue(i);
            if (v1.IsNull() || v2.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d1 = (v1.type().id() == LogicalTypeId::INTEGER) ? v1.GetValue<int32_t>() : v1.GetValue<double>();
            double d2 = (v2.type().id() == LogicalTypeId::INTEGER) ? v2.GetValue<int32_t>() : v2.GetValue<double>();
            result.GetData<double>()[i] = std::atan2(d1, d2);
        }
        return;
    }

    if (name == "DEGREES") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::INTEGER) ? v.GetValue<int32_t>() : v.GetValue<double>();
            result.GetData<double>()[i] = d * 180.0 / 3.14159265358979323846;
        }
        return;
    }

    if (name == "RADIANS") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::INTEGER) ? v.GetValue<int32_t>() : v.GetValue<double>();
            result.GetData<double>()[i] = d * 3.14159265358979323846 / 180.0;
        }
        return;
    }

    if (name == "TRUNC" || name == "TRUNCATE") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        // Optional second arg controls fractional digits, same shape
        // as ROUND. Previously the second arg was silently dropped.
        bool has_precision = expr.arguments.size() >= 2;
        Vector prec_vec(LogicalType::INTEGER(), count);
        if (has_precision) Execute(*expr.arguments[1], input, prec_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            if (has_precision && !prec_vec.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            double d = v.GetValue<double>();
            if (has_precision) {
                int32_t p = prec_vec.GetValue(i).GetValue<int32_t>();
                double factor = std::pow(10.0, static_cast<double>(p));
                d = std::trunc(d * factor) / factor;
            } else {
                d = std::trunc(d);
            }
            result.GetData<double>()[i] = d;
        }
        return;
    }

    // ---- Additional date functions ----

    if (name == "DATE_DIFF" || name == "DATEDIFF") {
        auto part = StringUtil::Upper(
            ExpressionExecutor::ExecuteScalar(*expr.arguments[0]).GetValue<std::string>());
        Vector ts1(expr.arguments[1]->GetReturnType(), count);
        Vector ts2(expr.arguments[2]->GetReturnType(), count);
        Execute(*expr.arguments[1], input, ts1, count);
        Execute(*expr.arguments[2], input, ts2, count);
        for (idx_t i = 0; i < count; i++) {
            auto v1 = ts1.GetValue(i), v2 = ts2.GetValue(i);
            if (v1.IsNull() || v2.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            int64_t m1 = v1.GetValue<int64_t>(), m2 = v2.GetValue<int64_t>();
            int64_t diff_sec = (m2 - m1) / 1000000;
            int64_t r = 0;
            if (part == "SECOND") r = diff_sec;
            else if (part == "MINUTE") r = diff_sec / 60;
            else if (part == "HOUR") r = diff_sec / 3600;
            else if (part == "DAY") r = diff_sec / 86400;
            result.SetValue(i, Value::BIGINT(r));
        }
        return;
    }

    if (name == "DATE_ADD" || name == "DATEADD") {
        auto part = StringUtil::Upper(
            ExpressionExecutor::ExecuteScalar(*expr.arguments[0]).GetValue<std::string>());
        Vector n_vec(expr.arguments[1]->GetReturnType(), count);
        Vector ts_vec(expr.arguments[2]->GetReturnType(), count);
        Execute(*expr.arguments[1], input, n_vec, count);
        Execute(*expr.arguments[2], input, ts_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto n_val = n_vec.GetValue(i);
            int64_t n = (n_val.type().id() == LogicalTypeId::INTEGER)
                ? n_val.GetValue<int32_t>() : n_val.GetValue<int64_t>();
            auto ts_val = ts_vec.GetValue(i);
            int64_t ts = (ts_val.type().id() == LogicalTypeId::INTEGER)
                ? ts_val.GetValue<int32_t>() : ts_val.GetValue<int64_t>();
            int64_t add_micros = 0;
            if (part == "SECOND") add_micros = n * 1000000;
            else if (part == "MINUTE") add_micros = n * 60 * 1000000;
            else if (part == "HOUR") add_micros = n * 3600 * 1000000;
            else if (part == "DAY") add_micros = n * 86400LL * 1000000;
            result.SetValue(i, Value::BIGINT(ts + add_micros));
        }
        return;
    }

    if (name == "STRFTIME" || name == "FORMAT_TIMESTAMP") {
        // Simple format: return ISO-like string.
        Vector ts_vec(expr.arguments.size() > 1 ? expr.arguments[1]->GetReturnType()
                                                 : expr.arguments[0]->GetReturnType(), count);
        Execute(expr.arguments.size() > 1 ? *expr.arguments[1] : *expr.arguments[0], input, ts_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = ts_vec.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            int64_t micros = v.GetValue<int64_t>();
            auto seconds = static_cast<time_t>(micros / 1000000);
            struct tm tm_buf;
#ifdef _MSC_VER
            gmtime_s(&tm_buf, &seconds);
#else
            gmtime_r(&seconds, &tm_buf);
#endif
            char buf[64];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
            result.SetValue(i, Value::VARCHAR(std::string(buf)));
        }
        return;
    }

    // ---- Null handling functions ----

    // IFNULL(a, b) and NVL(a, b) reach this dispatcher with the same arg
    // layout as COALESCE — route them to the same loop.
    if (name == "COALESCE" || name == "IFNULL" || name == "NVL") {
        for (idx_t i = 0; i < count; i++) {
            bool found = false;
            for (auto &arg : expr.arguments) {
                Vector v(arg->GetReturnType(), count);
                Execute(*arg, input, v, count);
                auto val = v.GetValue(i);
                if (!val.IsNull()) {
                    result.SetValue(i, val);
                    found = true;
                    break;
                }
            }
            if (!found) result.GetValidity().SetInvalid(i);
        }
        return;
    }

    if (name == "NULLIF") {
        Vector arg1(expr.arguments[0]->GetReturnType(), count);
        Vector arg2(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg1, count);
        Execute(*expr.arguments[1], input, arg2, count);
        for (idx_t i = 0; i < count; i++) {
            auto v1 = arg1.GetValue(i), v2 = arg2.GetValue(i);
            if (v1 == v2) result.GetValidity().SetInvalid(i);
            else result.SetValue(i, v1);
        }
        return;
    }

    // ---- Regex functions ----

    if (name == "REGEXP_MATCHES" || name == "REGEXP_MATCH") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector pat_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, pat_vec, count);
        auto *out = result.GetData<bool>();
        for (idx_t i = 0; i < count; i++) {
            if (!str_vec.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = str_vec.GetValue(i).GetValue<std::string>();
            auto p = pat_vec.GetValue(i).GetValue<std::string>();
            try {
                std::regex re(p);
                out[i] = std::regex_search(s, re);
            } catch (...) {
                out[i] = false;
            }
        }
        return;
    }

    if (name == "REGEXP_REPLACE") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector pat_vec(expr.arguments[1]->GetReturnType(), count);
        Vector rep_vec(expr.arguments[2]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, pat_vec, count);
        Execute(*expr.arguments[2], input, rep_vec, count);
        // Translate SQL-style replacement (\1..\9 backrefs, \\ literal) into
        // std::regex_replace ECMAScript form ($1..$9, $$ literal $). Bare $
        // in the input must be escaped to $$ so it isn't reinterpreted.
        auto translate_replacement = [](const std::string &r) {
            std::string out;
            out.reserve(r.size());
            for (size_t k = 0; k < r.size(); ++k) {
                char c = r[k];
                if (c == '\\' && k + 1 < r.size()) {
                    char n = r[k + 1];
                    if (n >= '0' && n <= '9') {
                        out += '$';
                        out += n;
                        ++k;
                        continue;
                    }
                    if (n == '\\') {
                        out += '\\';
                        ++k;
                        continue;
                    }
                    // Unknown escape: keep backslash + char as-is.
                    out += c;
                    out += n;
                    ++k;
                    continue;
                }
                if (c == '$') {
                    out += "$$";
                    continue;
                }
                out += c;
            }
            return out;
        };
        std::optional<std::regex> compiled_re;
        std::string cached_pattern;
        bool have_cached = false;
        for (idx_t i = 0; i < count; i++) {
            auto s = str_vec.GetValue(i).GetValue<std::string>();
            auto p = pat_vec.GetValue(i).GetValue<std::string>();
            auto r = rep_vec.GetValue(i).GetValue<std::string>();
            try {
                if (!have_cached || cached_pattern != p) {
                    have_cached = false;
                    compiled_re.emplace(p);
                    cached_pattern = p;
                    have_cached = true;
                }
                auto rep = translate_replacement(r);
                result.SetValue(i, Value::VARCHAR(std::regex_replace(s, *compiled_re, rep)));
            } catch (...) {
                result.SetValue(i, Value::VARCHAR(s));
            }
        }
        return;
    }

    if (name == "REGEXP_EXTRACT") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector pat_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, pat_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto s = str_vec.GetValue(i).GetValue<std::string>();
            auto p = pat_vec.GetValue(i).GetValue<std::string>();
            try {
                std::regex re(p);
                std::smatch m;
                if (std::regex_search(s, m, re) && m.size() > 1) {
                    result.SetValue(i, Value::VARCHAR(m[1].str()));
                } else if (std::regex_search(s, m, re)) {
                    result.SetValue(i, Value::VARCHAR(m[0].str()));
                } else {
                    result.GetValidity().SetInvalid(i);
                }
            } catch (...) {
                result.GetValidity().SetInvalid(i);
            }
        }
        return;
    }

    // ---- Timestamp/Date functions ----

    if (name == "NOW" || name == "CURRENT_TIMESTAMP") {
        auto now = std::chrono::system_clock::now();
        auto epoch = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        for (idx_t i = 0; i < count; i++) {
            result.SetValue(i, Value::BIGINT(epoch));
        }
        return;
    }

    if (name == "CURRENT_DATE") {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf;
#ifdef _MSC_VER
        localtime_s(&tm_buf, &time_t_now);
#else
        localtime_r(&time_t_now, &tm_buf);
#endif
        // Encode as YYYYMMDD integer.
        int32_t date_val = (tm_buf.tm_year + 1900) * 10000 +
                           (tm_buf.tm_mon + 1) * 100 +
                           tm_buf.tm_mday;
        for (idx_t i = 0; i < count; i++) {
            result.SetValue(i, Value::INTEGER(date_val));
        }
        return;
    }

    if (name == "EXTRACT" || name == "DATE_PART") {
        // EXTRACT(part, timestamp_expr)
        // part is a string constant. The argument's unit is inferred per row:
        //   |v| >= 1e13  -> microseconds since epoch (matches NOW/TO_TIMESTAMP)
        //   otherwise    -> seconds since epoch (common epoch-second timestamps)
        // Time-of-day parts (HOUR/MINUTE/SECOND/DOW/EPOCH) use direct integer
        // arithmetic and avoid the gmtime round-trip; calendar parts
        // (YEAR/MONTH/DAY) still go through gmtime_s/gmtime_r.
        auto part_str = ExpressionExecutor::ExecuteScalar(*expr.arguments[0])
                            .GetValue<std::string>();
        auto part = StringUtil::Upper(part_str);

        Vector ts_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[1], input, ts_vec, count);

        // Bucket parts: arithmetic (no gmtime) vs calendar (gmtime).
        bool is_arith = (part == "HOUR" || part == "MINUTE" || part == "SECOND" ||
                         part == "EPOCH" || part == "DOW");
        bool is_calendar = (part == "YEAR" || part == "MONTH" || part == "DAY");

        auto floor_mod = [](int64_t a, int64_t m) -> int64_t {
            int64_t r = a % m;
            if (r < 0) r += m;
            return r;
        };
        auto floor_div = [](int64_t a, int64_t m) -> int64_t {
            int64_t q = a / m;
            if ((a % m) != 0 && ((a < 0) != (m < 0))) q--;
            return q;
        };

        // Vectorized fast path: timestamps are commonly BIGINT epoch seconds,
        // and the arithmetic parts (HOUR/MINUTE/SECOND/EPOCH/DOW) need only
        // direct integer arithmetic. Skip Value-boxing each row — go straight
        // from int64_t input slot to int64_t output slot. Cuts per-row cost
        // from ~hundreds of ns to a handful of cycles. A minute extract
        // dropped from >30s timeout to a few seconds.
        auto ts_tid = ts_vec.GetType().id();
        if (is_arith && ts_tid == LogicalTypeId::BIGINT) {
            const int64_t *ts_data = ts_vec.GetData<int64_t>();
            int64_t *out_data = result.GetData<int64_t>();
            const auto &ts_valid = ts_vec.GetValidity();
            auto &out_valid = result.GetValidity();
            for (idx_t i = 0; i < count; i++) {
                if (!ts_valid.RowIsValid(i)) { out_valid.SetInvalid(i); continue; }
                int64_t raw = ts_data[i];
                int64_t abs_raw = raw < 0 ? -raw : raw;
                int64_t seconds = (abs_raw >= 10000000000000LL) ? raw / 1000000 : raw;
                int64_t extracted = 0;
                if (part == "SECOND") extracted = floor_mod(seconds, 60);
                else if (part == "MINUTE") extracted = floor_mod(floor_div(seconds, 60), 60);
                else if (part == "HOUR") extracted = floor_mod(floor_div(seconds, 3600), 24);
                else if (part == "EPOCH") extracted = seconds;
                else if (part == "DOW") extracted = floor_mod(floor_div(seconds, 86400) + 4, 7);
                out_data[i] = extracted;
            }
            return;
        }

        auto to_seconds = [](const Value &v) -> int64_t {
            int64_t raw = 0;
            if (v.type().id() == LogicalTypeId::TIMESTAMP)
                return v.GetValue<int64_t>() / 1000000;  // micros -> seconds
            else if (v.type().id() == LogicalTypeId::DATE)
                return static_cast<int64_t>(v.GetValue<int32_t>()) * 86400;  // days -> seconds
            else if (v.type().id() == LogicalTypeId::BIGINT) raw = v.GetValue<int64_t>();
            else if (v.type().id() == LogicalTypeId::INTEGER)
                raw = static_cast<int64_t>(v.GetValue<int32_t>());
            else return 0;
            // Heuristic: values with magnitude >= 1e13 are microseconds; the
            // INTEGER path can never reach that, so it stays in seconds.
            int64_t abs_raw = raw < 0 ? -raw : raw;
            return (abs_raw >= 10000000000000LL) ? raw / 1000000 : raw;
        };

        for (idx_t i = 0; i < count; i++) {
            auto ts_val = ts_vec.GetValue(i);
            if (ts_val.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            int64_t seconds = to_seconds(ts_val);

            int64_t extracted = 0;
            if (is_arith) {
                if (part == "SECOND") extracted = floor_mod(seconds, 60);
                else if (part == "MINUTE") extracted = floor_mod(floor_div(seconds, 60), 60);
                else if (part == "HOUR") extracted = floor_mod(floor_div(seconds, 3600), 24);
                else if (part == "EPOCH") extracted = seconds;
                else if (part == "DOW") extracted = floor_mod(floor_div(seconds, 86400) + 4, 7);
            } else if (is_calendar) {
                if (part == "YEAR") {
                    // Hinnant days-from-civil reverse: avoid per-row gmtime.
                    int64_t days = seconds / 86400;
                    if (seconds < 0 && (seconds % 86400) != 0) --days;
                    days += 719468;
                    int era = (int)((days >= 0 ? days : days - 146096) / 146097);
                    unsigned doe = (unsigned)(days - (int64_t)era * 146097);
                    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
                    int year = (int)yoe + era * 400;
                    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
                    unsigned mp = (5*doy + 2) / 153;
                    if (mp >= 10) ++year;
                    extracted = year;
                } else {
                    auto time_t_val = static_cast<time_t>(seconds);
                    struct tm tm_buf;
#ifdef _MSC_VER
                    gmtime_s(&tm_buf, &time_t_val);
#else
                    gmtime_r(&time_t_val, &tm_buf);
#endif
                    if (part == "MONTH") extracted = tm_buf.tm_mon + 1;
                    else if (part == "DAY") extracted = tm_buf.tm_mday;
                }
            }

            result.SetValue(i, Value::BIGINT(extracted));
        }
        return;
    }

    if (name == "DATE_TRUNC") {
        auto part = StringUtil::Upper(
            ExpressionExecutor::ExecuteScalar(*expr.arguments[0]).GetValue<std::string>());
        Vector ts_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[1], input, ts_vec, count);

        for (idx_t i = 0; i < count; i++) {
            auto ts_val = ts_vec.GetValue(i);
            if (ts_val.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            // Auto-detect input scale (matches DATE_PART/EXTRACT logic):
            //   |raw| >= 1e13  -> microseconds since epoch (NOW/TO_TIMESTAMP)
            //   otherwise      -> seconds since epoch (common BIGINT epoch-second timestamps)
            // Output preserves the input scale so downstream GROUP BY/ORDER BY
            // remain self-consistent against the original column.
            int64_t raw = 0;
            bool input_is_micros = false;
            if (ts_val.type().id() == LogicalTypeId::TIMESTAMP) {
                raw = ts_val.GetValue<int64_t>();
                input_is_micros = true;
            } else if (ts_val.type().id() == LogicalTypeId::DATE) {
                raw = static_cast<int64_t>(ts_val.GetValue<int32_t>()) * 86400;
                input_is_micros = false;
            } else if (ts_val.type().id() == LogicalTypeId::INTEGER) {
                raw = static_cast<int64_t>(ts_val.GetValue<int32_t>());
                input_is_micros = false;
            } else {
                raw = ts_val.GetValue<int64_t>();
                int64_t abs_raw = raw < 0 ? -raw : raw;
                input_is_micros = (abs_raw >= 10000000000000LL);
            }
            int64_t micros = input_is_micros ? raw : raw * 1000000;

            // Sub-second truncation needs no tm round-trip.
            if (part == "MICROSECOND" || part == "MICROSECONDS") {
                int64_t out = input_is_micros ? micros : micros / 1000000;
                result.SetValue(i, Value::BIGINT(out));
                continue;
            }
            if (part == "MILLISECOND" || part == "MILLISECONDS") {
                int64_t out_micros = (micros / 1000) * 1000;
                int64_t out = input_is_micros ? out_micros : out_micros / 1000000;
                result.SetValue(i, Value::BIGINT(out));
                continue;
            }

            auto seconds = micros / 1000000;
            auto time_t_val = static_cast<time_t>(seconds);
            struct tm tm_buf;
#ifdef _MSC_VER
            gmtime_s(&tm_buf, &time_t_val);
#else
            gmtime_r(&time_t_val, &tm_buf);
#endif
            if (part == "SECOND" || part == "SECONDS") { /* already second-precise */ }
            else if (part == "MINUTE" || part == "MINUTES") { tm_buf.tm_sec = 0; }
            else if (part == "HOUR" || part == "HOURS") { tm_buf.tm_min = 0; tm_buf.tm_sec = 0; }
            else if (part == "DAY" || part == "DAYS") { tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0; }
            else if (part == "WEEK" || part == "WEEKS") {
                // Truncate to the Monday of that week (ISO 8601).
                int days_back = (tm_buf.tm_wday == 0) ? 6 : tm_buf.tm_wday - 1;
                tm_buf.tm_mday -= days_back;
                tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            }
            else if (part == "MONTH" || part == "MONTHS") {
                tm_buf.tm_mday = 1; tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            }
            else if (part == "QUARTER" || part == "QUARTERS") {
                tm_buf.tm_mon = (tm_buf.tm_mon / 3) * 3;
                tm_buf.tm_mday = 1; tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            }
            else if (part == "YEAR" || part == "YEARS") {
                tm_buf.tm_mon = 0; tm_buf.tm_mday = 1; tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            }
            else if (part == "DECADE" || part == "DECADES") {
                int year = tm_buf.tm_year + 1900;
                tm_buf.tm_year = (year - (year % 10)) - 1900;
                tm_buf.tm_mon = 0; tm_buf.tm_mday = 1; tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            }
            else if (part == "CENTURY" || part == "CENTURIES") {
                int year = tm_buf.tm_year + 1900;
                tm_buf.tm_year = (year - (year % 100)) - 1900;
                tm_buf.tm_mon = 0; tm_buf.tm_mday = 1; tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            }
            else if (part == "MILLENNIUM" || part == "MILLENNIA") {
                int year = tm_buf.tm_year + 1900;
                tm_buf.tm_year = (year - (year % 1000)) - 1900;
                tm_buf.tm_mon = 0; tm_buf.tm_mday = 1; tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            }

            int64_t trunc_secs = static_cast<int64_t>(
#ifdef _MSC_VER
                _mkgmtime(&tm_buf)
#else
                timegm(&tm_buf)
#endif
            );
            int64_t truncated = input_is_micros ? trunc_secs * 1000000 : trunc_secs;
            result.SetValue(i, Value::BIGINT(truncated));
        }
        return;
    }

    if (name == "TO_TIMESTAMP" || name == "MAKE_TIMESTAMP") {
        // Convert epoch seconds to timestamp (microseconds).
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto val = arg.GetValue(i);
            if (val.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            int64_t epoch_sec = 0;
            if (val.type().id() == LogicalTypeId::INTEGER)
                epoch_sec = val.GetValue<int32_t>();
            else if (val.type().id() == LogicalTypeId::BIGINT)
                epoch_sec = val.GetValue<int64_t>();
            else if (val.type().id() == LogicalTypeId::DOUBLE)
                epoch_sec = static_cast<int64_t>(val.GetValue<double>());
            result.SetValue(i, Value::BIGINT(epoch_sec * 1000000));
        }
        return;
    }

    if (name == "EPOCH_MS") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto val = arg.GetValue(i);
            if (val.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            int64_t micros = val.GetValue<int64_t>();
            result.SetValue(i, Value::DOUBLE(static_cast<double>(micros) / 1000.0));
        }
        return;
    }

    if (name == "MONTHNAME" || name == "DAYNAME") {
        static const char* const month_names[] = {
            "January", "February", "March", "April", "May", "June",
            "July", "August", "September", "October", "November", "December"
        };
        static const char* const day_names[] = {
            "Sunday", "Monday", "Tuesday", "Wednesday",
            "Thursday", "Friday", "Saturday"
        };
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto val = arg.GetValue(i);
            if (val.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            int64_t micros = val.GetValue<int64_t>();
            auto seconds = micros / 1000000;
            auto time_t_val = static_cast<time_t>(seconds);
            struct tm tm_buf;
#ifdef _MSC_VER
            gmtime_s(&tm_buf, &time_t_val);
#else
            gmtime_r(&time_t_val, &tm_buf);
#endif
            if (name == "MONTHNAME") {
                int m = tm_buf.tm_mon;
                if (m < 0) m = 0; if (m > 11) m = 11;
                result.SetValue(i, Value::VARCHAR(month_names[m]));
            } else {
                int d = tm_buf.tm_wday;
                if (d < 0) d = 0; if (d > 6) d = 6;
                result.SetValue(i, Value::VARCHAR(day_names[d]));
            }
        }
        return;
    }

    if (name == "LAST_DAY") {
        // Return the timestamp (microseconds) of the last day of the month at 00:00 UTC.
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto val = arg.GetValue(i);
            if (val.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            int64_t micros = val.GetValue<int64_t>();
            auto seconds = micros / 1000000;
            auto time_t_val = static_cast<time_t>(seconds);
            struct tm tm_buf;
#ifdef _MSC_VER
            gmtime_s(&tm_buf, &time_t_val);
#else
            gmtime_r(&time_t_val, &tm_buf);
#endif
            // Day 0 of the following month is the last day of this month (mkgmtime normalizes).
            tm_buf.tm_mon += 1;
            tm_buf.tm_mday = 0;
            tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            auto last = static_cast<int64_t>(
#ifdef _MSC_VER
                _mkgmtime(&tm_buf)
#else
                timegm(&tm_buf)
#endif
            ) * 1000000;
            result.SetValue(i, Value::BIGINT(last));
        }
        return;
    }

    if (name == "MAKE_DATE") {
        // MAKE_DATE(year, month, day) -> INTEGER (YYYYMMDD, matches CURRENT_DATE encoding).
        if (expr.arguments.size() != 3) {
            throw NotImplementedException("MAKE_DATE requires 3 arguments: year, month, day");
        }
        Vector y_vec(expr.arguments[0]->GetReturnType(), count);
        Vector m_vec(expr.arguments[1]->GetReturnType(), count);
        Vector d_vec(expr.arguments[2]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, y_vec, count);
        Execute(*expr.arguments[1], input, m_vec, count);
        Execute(*expr.arguments[2], input, d_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto yv = y_vec.GetValue(i);
            auto mv = m_vec.GetValue(i);
            auto dv = d_vec.GetValue(i);
            if (yv.IsNull() || mv.IsNull() || dv.IsNull()) {
                result.GetValidity().SetInvalid(i); continue;
            }
            int32_t y = yv.type().id() == LogicalTypeId::BIGINT
                ? static_cast<int32_t>(yv.GetValue<int64_t>()) : yv.GetValue<int32_t>();
            int32_t m = mv.type().id() == LogicalTypeId::BIGINT
                ? static_cast<int32_t>(mv.GetValue<int64_t>()) : mv.GetValue<int32_t>();
            int32_t d = dv.type().id() == LogicalTypeId::BIGINT
                ? static_cast<int32_t>(dv.GetValue<int64_t>()) : dv.GetValue<int32_t>();
            result.SetValue(i, Value::INTEGER(y * 10000 + m * 100 + d));
        }
        return;
    }

    throw NotImplementedException("Function execution for: " + name);
}

// ============================================================================
// CAST execution
// ============================================================================

// Parse a complete integer from a string. Rejects trailing junk
// ('42abc' -> error), decimal fractions ('1.5' -> error), and SQL
// scientific notation ('1e3' -> reject as int but allow as double).
// Accepts surrounding whitespace + optional sign. Returns false on
// any parse failure so the caller can surface a clean error.
static bool ParseIntStrict(const std::string &s, int64_t &out) {
    if (s.empty()) return false;
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= s.size()) return false;
    size_t start = i;
    if (s[i] == '+' || s[i] == '-') i++;
    size_t digit_start = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
    if (i == digit_start) return false;
    size_t digit_end = i;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i != s.size()) return false;
    try {
        out = std::stoll(s.substr(start, digit_end - start));
        return true;
    } catch (...) {
        return false;
    }
}

// Parse a complete double from a string. Rejects trailing junk and
// empty/whitespace-only input. Accepts SQL scientific notation.
static bool ParseDoubleStrict(const std::string &s, double &out) {
    if (s.empty()) return false;
    size_t first = s.find_first_not_of(" \t");
    if (first == std::string::npos) return false;
    try {
        size_t consumed = 0;
        out = std::stod(s.substr(first), &consumed);
        size_t end = first + consumed;
        while (end < s.size() && (s[end] == ' ' || s[end] == '\t')) end++;
        return end == s.size();
    } catch (...) {
        return false;
    }
}

void ExpressionExecutor::ExecuteCast(const BoundCast &expr, DataChunk &input,
                                      Vector &result, idx_t count) {
    Vector child(expr.child->GetReturnType(), count);
    Execute(*expr.child, input, child, count);

    auto to_type = expr.GetReturnType().id();
    auto from_type = expr.child->GetReturnType().id();
    bool from_is_double = (from_type == LogicalTypeId::DOUBLE ||
                           from_type == LogicalTypeId::FLOAT);

    for (idx_t i = 0; i < count; i++) {
        auto val = child.GetValue(i);
        if (val.IsNull()) {
            result.GetValidity().SetInvalid(i);
            continue;
        }
        try {
            // Floating -> integer goes through the typed double, not
            // ToString (which renders 1e10 as "1e+10" and then
            // std::stoi truncates at 'e' producing 1).
            if (from_is_double && (to_type == LogicalTypeId::TINYINT ||
                                    to_type == LogicalTypeId::SMALLINT ||
                                    to_type == LogicalTypeId::INTEGER ||
                                    to_type == LogicalTypeId::BIGINT ||
                                    to_type == LogicalTypeId::UTINYINT ||
                                    to_type == LogicalTypeId::USMALLINT ||
                                    to_type == LogicalTypeId::UINTEGER ||
                                    to_type == LogicalTypeId::UBIGINT)) {
                double d = val.GetValue<double>();
                if (!std::isfinite(d) ||
                    d > static_cast<double>(std::numeric_limits<int64_t>::max()) ||
                    d < static_cast<double>(std::numeric_limits<int64_t>::min())) {
                    throw ConversionException("Value " + val.ToString() +
                        " is out of range for the destination type");
                }
                int64_t v = static_cast<int64_t>(std::trunc(d));
                switch (to_type) {
                case LogicalTypeId::TINYINT:
                    if (v < std::numeric_limits<int8_t>::min() || v > std::numeric_limits<int8_t>::max())
                        throw ConversionException("Value " + val.ToString() + " out of range for TINYINT");
                    result.SetValue(i, Value::TINYINT((int8_t)v)); break;
                case LogicalTypeId::SMALLINT:
                    if (v < std::numeric_limits<int16_t>::min() || v > std::numeric_limits<int16_t>::max())
                        throw ConversionException("Value " + val.ToString() + " out of range for SMALLINT");
                    result.SetValue(i, Value::SMALLINT((int16_t)v)); break;
                case LogicalTypeId::INTEGER:
                    if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max())
                        throw ConversionException("Value " + val.ToString() + " out of range for INTEGER");
                    result.SetValue(i, Value::INTEGER((int32_t)v)); break;
                case LogicalTypeId::BIGINT:
                    result.SetValue(i, Value::BIGINT(v)); break;
                case LogicalTypeId::UTINYINT:
                    if (v < 0 || v > std::numeric_limits<uint8_t>::max())
                        throw ConversionException("Value " + val.ToString() + " out of range for UTINYINT");
                    result.SetValue(i, Value::UTINYINT((uint8_t)v)); break;
                case LogicalTypeId::USMALLINT:
                    if (v < 0 || v > std::numeric_limits<uint16_t>::max())
                        throw ConversionException("Value " + val.ToString() + " out of range for USMALLINT");
                    result.SetValue(i, Value::USMALLINT((uint16_t)v)); break;
                case LogicalTypeId::UINTEGER:
                    if (v < 0 || v > (int64_t)std::numeric_limits<uint32_t>::max())
                        throw ConversionException("Value " + val.ToString() + " out of range for UINTEGER");
                    result.SetValue(i, Value::UINTEGER((uint32_t)v)); break;
                case LogicalTypeId::UBIGINT:
                    if (v < 0)
                        throw ConversionException("Value " + val.ToString() + " is negative; cannot cast to UBIGINT");
                    result.SetValue(i, Value::UBIGINT((uint64_t)v)); break;
                default: break;
                }
                continue;
            }
            auto str = val.ToString();
            switch (to_type) {
            case LogicalTypeId::TINYINT:
            case LogicalTypeId::SMALLINT:
            case LogicalTypeId::INTEGER:
            case LogicalTypeId::BIGINT:
            case LogicalTypeId::UTINYINT:
            case LogicalTypeId::USMALLINT:
            case LogicalTypeId::UINTEGER:
            case LogicalTypeId::UBIGINT: {
                int64_t v;
                if (!ParseIntStrict(str, v)) {
                    throw ConversionException("Could not convert string '" +
                        str + "' to " + expr.GetReturnType().ToString());
                }
                switch (to_type) {
                case LogicalTypeId::TINYINT:
                    if (v < std::numeric_limits<int8_t>::min() || v > std::numeric_limits<int8_t>::max())
                        throw ConversionException("Value " + str + " out of range for TINYINT");
                    result.SetValue(i, Value::TINYINT((int8_t)v)); break;
                case LogicalTypeId::SMALLINT:
                    if (v < std::numeric_limits<int16_t>::min() || v > std::numeric_limits<int16_t>::max())
                        throw ConversionException("Value " + str + " out of range for SMALLINT");
                    result.SetValue(i, Value::SMALLINT((int16_t)v)); break;
                case LogicalTypeId::INTEGER:
                    if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max())
                        throw ConversionException("Value " + str + " out of range for INTEGER");
                    result.SetValue(i, Value::INTEGER((int32_t)v)); break;
                case LogicalTypeId::BIGINT:
                    result.SetValue(i, Value::BIGINT(v)); break;
                case LogicalTypeId::UTINYINT:
                    if (v < 0 || v > std::numeric_limits<uint8_t>::max())
                        throw ConversionException("Value " + str + " out of range for UTINYINT");
                    result.SetValue(i, Value::UTINYINT((uint8_t)v)); break;
                case LogicalTypeId::USMALLINT:
                    if (v < 0 || v > std::numeric_limits<uint16_t>::max())
                        throw ConversionException("Value " + str + " out of range for USMALLINT");
                    result.SetValue(i, Value::USMALLINT((uint16_t)v)); break;
                case LogicalTypeId::UINTEGER:
                    if (v < 0 || v > (int64_t)std::numeric_limits<uint32_t>::max())
                        throw ConversionException("Value " + str + " out of range for UINTEGER");
                    result.SetValue(i, Value::UINTEGER((uint32_t)v)); break;
                case LogicalTypeId::UBIGINT:
                    if (v < 0)
                        throw ConversionException("Value " + str + " is negative; cannot cast to UBIGINT");
                    result.SetValue(i, Value::UBIGINT((uint64_t)v)); break;
                default: break;
                }
                break;
            }
            case LogicalTypeId::DOUBLE: {
                double d;
                if (!ParseDoubleStrict(str, d)) {
                    throw ConversionException("Could not convert string '" +
                        str + "' to DOUBLE");
                }
                result.SetValue(i, Value::DOUBLE(d)); break;
            }
            case LogicalTypeId::FLOAT: {
                double d;
                if (!ParseDoubleStrict(str, d)) {
                    throw ConversionException("Could not convert string '" +
                        str + "' to FLOAT");
                }
                result.SetValue(i, Value::FLOAT((float)d)); break;
            }
            case LogicalTypeId::VARCHAR:
                result.SetValue(i, Value::VARCHAR(str)); break;
            case LogicalTypeId::DATE: {
                int32_t days;
                if (!Value::TryParseDateStringEpochDays(str.data(), str.size(), days)) {
                    throw ConversionException(
                        "Could not convert string '" + str + "' to DATE");
                }
                result.SetValue(i, Value::DATE(days));
                break;
            }
            case LogicalTypeId::TIMESTAMP:
            case LogicalTypeId::TIMESTAMP_TZ: {
                int64_t micros;
                if (!Value::TryParseTimestampMicros(str.data(), str.size(), micros)) {
                    // Fall back to DATE-only string (midnight TIMESTAMP).
                    int32_t days;
                    if (!Value::TryParseDateStringEpochDays(str.data(), str.size(), days)) {
                        throw ConversionException(
                            "Could not convert string '" + str + "' to TIMESTAMP");
                    }
                    micros = static_cast<int64_t>(days) * 86400LL * 1000000LL;
                }
                result.SetValue(i, Value::TIMESTAMP(micros));
                break;
            }
            case LogicalTypeId::BOOLEAN: {
                // Case-insensitive bool parse matching DuckDB/Postgres:
                // 't', 'true', 'yes', 'y', '1' -> true
                // 'f', 'false', 'no', 'n', '0' -> false
                // anything else -> error (not silently false)
                std::string lower;
                lower.reserve(str.size());
                for (char c : str) {
                    lower += (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
                }
                if (lower == "true" || lower == "t" || lower == "yes" ||
                    lower == "y" || lower == "1") {
                    result.SetValue(i, Value::BOOLEAN(true));
                } else if (lower == "false" || lower == "f" || lower == "no" ||
                           lower == "n" || lower == "0") {
                    result.SetValue(i, Value::BOOLEAN(false));
                } else {
                    throw ConversionException("Could not convert string '" +
                        str + "' to BOOLEAN");
                }
                break;
            }
            default:
                result.SetValue(i, Value::VARCHAR(str)); break;
            }
        } catch (...) {
            if (expr.is_try) {
                result.GetValidity().SetInvalid(i); // TRY_CAST returns NULL
            } else {
                throw ConversionException("Cannot cast '" + val.ToString() + "' to " +
                                           expr.GetReturnType().ToString());
            }
        }
    }
}

// Static member.
Catalog *ExpressionExecutor::catalog_ = nullptr;

// ============================================================================
// Subquery execution
// ============================================================================

void ExpressionExecutor::ExecuteSubquery(const BoundSubqueryExpression &expr,
                                          DataChunk &input, Vector &result, idx_t count) {
    if (!catalog_) {
        throw InternalException("Catalog not set for subquery execution");
    }

    auto *parsed_query = static_cast<SelectStatement *>(expr.parsed_query.get());
    if (!parsed_query) {
        throw InternalException("Subquery has no parsed query");
    }

    // Bind and execute the subquery.
    Binder binder(*catalog_);
    auto bound = binder.Bind(*parsed_query);
    auto logical = Planner::Plan(*bound);
    PhysicalPlanner pp(*catalog_);
    auto physical = pp.Plan(*logical);
    physical->Init();

    // Collect subquery results.
    std::vector<std::vector<Value>> sub_rows;
    DataChunk chunk;
    while (true) {
        if (!physical->GetData(chunk)) break;
        for (idx_t i = 0; i < chunk.size(); i++) {
            std::vector<Value> row;
            for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                row.push_back(chunk.GetValue(c, i));
            sub_rows.push_back(std::move(row));
        }
    }

    auto *out = result.GetData<bool>();

    switch (expr.subtype) {
    case BoundSubqueryExpression::Type::EXISTS:
        for (idx_t i = 0; i < count; i++) {
            out[i] = !sub_rows.empty();
        }
        break;

    case BoundSubqueryExpression::Type::NOT_EXISTS:
        for (idx_t i = 0; i < count; i++) {
            out[i] = sub_rows.empty();
        }
        break;

    case BoundSubqueryExpression::Type::IN_SUBQUERY: {
        // Check if child value exists in subquery first column.
        if (expr.child) {
            Vector child_vec(expr.child->GetReturnType(), count);
            Execute(*expr.child, input, child_vec, count);
            for (idx_t i = 0; i < count; i++) {
                auto val = child_vec.GetValue(i);
                bool found = false;
                for (auto &row : sub_rows) {
                    if (!row.empty() && row[0].ToString() == val.ToString()) {
                        found = true;
                        break;
                    }
                }
                out[i] = found;
            }
        }
        break;
    }

    case BoundSubqueryExpression::Type::SCALAR:
        // Return the first value of the first row.
        for (idx_t i = 0; i < count; i++) {
            if (!sub_rows.empty() && !sub_rows[0].empty()) {
                result.SetValue(i, sub_rows[0][0]);
            } else {
                result.GetValidity().SetInvalid(i);
            }
        }
        break;
    }
}

} // namespace slothdb
