#include "slothdb/ask/nl_to_sql.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace slothdb {
namespace ask {

namespace {

// Case-fold a string to lowercase for matching.
std::string lower(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Tokenize an NL question. Collapses whitespace + punctuation, preserves
// quoted strings as single tokens (for future "where name = 'alice'"
// support; current patterns use bare literals).
std::vector<std::string> Tokenize(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() { if (!cur.empty()) { out.push_back(cur); cur.clear(); } };
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isspace(uc) || c == ',' || c == '?' || c == '!') {
            flush();
        } else if (c == '.') {
            // Treat "." as separator only between words, not inside numbers.
            if (!cur.empty() && std::isdigit(static_cast<unsigned char>(cur.back()))) {
                cur += c;
            } else {
                flush();
            }
        } else {
            cur += static_cast<char>(std::tolower(uc));
        }
    }
    flush();
    return out;
}

bool IsInteger(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

bool IsYear(const std::string &s) {
    return s.size() == 4 && IsInteger(s);
}

// Stopwords the rule engine routinely skips over. Not exhaustive — we
// only strip what matters for matching patterns, not for proper NL.
const std::unordered_set<std::string> &Stopwords() {
    static const std::unordered_set<std::string> s = {
        // Articles / copulas / politeness
        "the", "a", "an", "of", "for", "is", "are", "was", "were",
        "be", "been", "please",
        // Imperative framings the user often leads with
        "show", "me", "give", "tell", "find", "list", "get",
        "display", "out", "print", "want", "need",
        // Connective filler that doesn't change the query shape
        "all", "any", "some", "that", "which", "who", "whose", "whom",
        "had", "has", "have", "did", "does", "do",
        // Possessives that don't matter for rules
        "their", "its"
    };
    return s;
}

// Column-name synonyms. When the NL mentions "revenue" and the schema
// has a column called "amount" or "total", we accept it. Expandable
// via docs/ASK.md; keep the list small enough to be auditable.
const std::unordered_map<std::string, std::vector<std::string>> &ColumnSynonyms() {
    static const std::unordered_map<std::string, std::vector<std::string>> s = {
        {"revenue", {"revenue", "total", "amount", "value", "price", "sales", "sum"}},
        {"sales",   {"sales", "revenue", "amount", "total"}},
        {"amount",  {"amount", "total", "value", "price"}},
        {"value",   {"value", "amount", "total"}},
        {"total",   {"total", "amount", "sum", "revenue"}},
        {"price",   {"price", "amount", "cost"}},
        {"cost",    {"cost", "price", "amount"}},
        {"customer",{"customer", "client", "buyer", "user"}},
        {"user",    {"user", "customer", "client"}},
        {"product", {"product", "item", "sku"}},
        {"date",    {"date", "day", "time", "timestamp", "created_at", "created", "order_date"}},
        {"month",   {"month", "date", "order_date", "created_at", "timestamp"}},
        {"year",    {"year", "date", "order_date", "created_at", "timestamp"}},
        {"region",  {"region", "country", "location", "area"}},
    };
    return s;
}

// Resolve an English noun to a table in the schema. Tries exact match,
// case-insensitive, and singular↔plural. Returns nullptr if nothing
// matches or multiple tables match ambiguously.
const Table *ResolveTable(const Schema &schema, const std::string &noun) {
    const Table *exact = nullptr;
    for (const auto &t : schema.tables) {
        if (lower(t.name) == noun) { exact = &t; break; }
    }
    if (exact) return exact;

    // Singular → plural and plural → singular.
    std::string variant;
    if (!noun.empty() && noun.back() == 's') {
        variant = noun.substr(0, noun.size() - 1);
    } else {
        variant = noun + "s";
    }
    for (const auto &t : schema.tables) {
        if (lower(t.name) == variant) return &t;
    }
    return nullptr;
}

// Resolve an English noun to a column on a specific table. Synonyms
// are tried in order: exact, lowercase, then the synonym class the noun
// belongs to (if any) against every column. Ties resolve to first-match
// (stable on column-order in the DDL).
const Column *ResolveColumn(const Table &table, const std::string &noun) {
    const std::string n = lower(noun);
    for (const auto &c : table.columns) if (lower(c.name) == n) return &c;

    // Synonym class: pick the key whose synonym list contains `noun`,
    // then match every column against that class.
    const auto &syns = ColumnSynonyms();
    auto it = syns.find(n);
    if (it != syns.end()) {
        for (const auto &syn : it->second) {
            for (const auto &c : table.columns) {
                if (lower(c.name) == syn) return &c;
                if (lower(c.name).find(syn) != std::string::npos) return &c;
            }
        }
    }
    // Substring match as a last resort.
    for (const auto &c : table.columns) {
        if (lower(c.name).find(n) != std::string::npos) return &c;
    }
    return nullptr;
}

// Does the table have at least one numeric column? Needed when the
// NL asks for SUM/AVG without naming a specific column.
bool HasNumericColumn(const Table &table) {
    for (const auto &c : table.columns) {
        const std::string t = lower(c.type);
        if (t.find("int") != std::string::npos ||
            t.find("double") != std::string::npos ||
            t.find("float") != std::string::npos ||
            t.find("decimal") != std::string::npos ||
            t.find("numeric") != std::string::npos) return true;
    }
    return false;
}

const Column *FirstNumericColumn(const Table &table) {
    for (const auto &c : table.columns) {
        const std::string t = lower(c.type);
        if (t.find("int") != std::string::npos ||
            t.find("double") != std::string::npos ||
            t.find("float") != std::string::npos ||
            t.find("decimal") != std::string::npos ||
            t.find("numeric") != std::string::npos) return &c;
    }
    return nullptr;
}

const Column *DateColumn(const Table &table) {
    for (const auto &c : table.columns) {
        const std::string t = lower(c.type);
        if (t.find("date") != std::string::npos ||
            t.find("time") != std::string::npos) return &c;
        const std::string n = lower(c.name);
        if (n.find("date") != std::string::npos ||
            n.find("_at") != std::string::npos) return &c;
    }
    return nullptr;
}

// Strip stopwords but keep positional tokens that matter for patterns
// ("how many", "top N", "per", "by", "in"). Preserves order.
std::vector<std::string> Normalize(const std::vector<std::string> &tokens) {
    const auto &stop = Stopwords();
    std::vector<std::string> out;
    out.reserve(tokens.size());
    for (const auto &t : tokens) {
        if (stop.count(t)) continue;
        out.push_back(t);
    }
    return out;
}

// Build a NO_MATCH result with a specific message.
Result NoMatch(const std::string &msg) {
    Result r;
    r.status = Status::NO_MATCH;
    r.message = msg;
    return r;
}

Result UnresolvedTable(const std::string &noun) {
    Result r;
    r.status = Status::UNRESOLVED_TABLE;
    r.message = "could not find a table matching '" + noun + "'";
    r.unresolved = noun;
    return r;
}

Result UnresolvedColumn(const std::string &noun) {
    Result r;
    r.status = Status::UNRESOLVED_COLUMN;
    r.message = "could not find a column matching '" + noun + "'";
    r.unresolved = noun;
    return r;
}

// Find the position of a specific keyword in a token list.
int FindToken(const std::vector<std::string> &tokens, const std::string &kw) {
    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i] == kw) return static_cast<int>(i);
    }
    return -1;
}

// Extract a 4-digit year from tokens after "in"/"during"/"for" — very
// common NL pattern ("sales in 2024"). Returns empty if absent.
std::string ExtractYear(const std::vector<std::string> &tokens) {
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
        if ((tokens[i] == "in" || tokens[i] == "during" || tokens[i] == "for") &&
            IsYear(tokens[i + 1])) {
            return tokens[i + 1];
        }
    }
    return "";
}

// Render `WHERE EXTRACT(YEAR FROM <col>) = <year>` with fallback to
// string-prefix comparison if the column looks like a VARCHAR. This is
// the least-surprise mapping for "... in 2024" across CSV-inferred
// schemas where the date column may be a string.
std::string RenderYearFilter(const Column &date_col, const std::string &year) {
    std::string t = lower(date_col.type);
    bool is_varchar = (t.find("varchar") != std::string::npos ||
                       t.find("text") != std::string::npos ||
                       t.find("string") != std::string::npos);
    if (is_varchar) {
        // Substring match on the year — works for ISO-8601 and YYYY-first
        // date strings. Not perfect; a follow-up could parse and compare.
        return "\"" + date_col.name + "\" LIKE '" + year + "%'";
    }
    return "EXTRACT(YEAR FROM \"" + date_col.name + "\") = " + year;
}

// Quote an identifier for SQL. We use double-quotes because SlothDB's
// parser accepts them and they're unambiguous across dialects.
std::string Q(const std::string &ident) {
    return "\"" + ident + "\"";
}

// Extract a "top N" limit from tokens. Returns 0 if absent.
int ExtractTopN(const std::vector<std::string> &tokens) {
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
        if ((tokens[i] == "top" || tokens[i] == "biggest" ||
             tokens[i] == "highest" || tokens[i] == "first") &&
            IsInteger(tokens[i + 1])) {
            return std::stoi(tokens[i + 1]);
        }
        if (tokens[i] == "bottom" || tokens[i] == "lowest" ||
            tokens[i] == "smallest") {
            if (i + 1 < tokens.size() && IsInteger(tokens[i + 1])) {
                return -std::stoi(tokens[i + 1]); // negative marks ASC
            }
        }
    }
    return 0;
}

// Extract the "X by|per|grouped-by Y" group column token, if any.
// Returns the noun token or empty.
std::string ExtractGroupBy(const std::vector<std::string> &tokens) {
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
        if (tokens[i] == "by" || tokens[i] == "per") {
            // "grouped by X", "sorted by X" — we want the trailing noun only
            // in contexts where GROUP BY makes sense. Conservative: take
            // the next token that isn't a keyword.
            std::string cand = tokens[i + 1];
            if (cand != "the" && cand != "a") return cand;
        }
    }
    return "";
}

} // namespace

Result Translate(const std::string &nl, const Schema &schema) {
    auto tokens = Tokenize(nl);
    if (tokens.empty()) return NoMatch("empty question");

    auto toks = Normalize(tokens);
    if (toks.empty()) return NoMatch("nothing left after removing stopwords");

    // Common sub-extractions.
    std::string year = ExtractYear(tokens);       // use original tokens (stopwords removed "in")
    std::string group_by = ExtractGroupBy(toks);
    int top_n = ExtractTopN(toks);

    // ---- PATTERN 1: COUNT ----
    // "how many X [in YEAR]"
    // "count (of) X", "count rows (in|from) X"
    // "number (of) X"
    //
    // Stopwords like "of" / "the" are stripped by Normalize before we get
    // here, so match on the post-normalize token stream. "count of sales"
    // arrives as ["count", "sales"] etc.
    bool is_count = false;
    size_t count_noun_idx = 0;
    if (toks.size() >= 2 && toks[0] == "how" && toks[1] == "many") {
        is_count = true;
        count_noun_idx = 2;
    } else if (!toks.empty() && toks[0] == "count") {
        is_count = true;
        count_noun_idx = (toks.size() >= 2 && toks[1] == "rows") ? 2 : 1;
    } else if (!toks.empty() && toks[0] == "number") {
        is_count = true;
        count_noun_idx = 1;
    }
    if (is_count && count_noun_idx < toks.size()) {
        // Skip "in" / "from" before the table noun.
        while (count_noun_idx < toks.size() &&
               (toks[count_noun_idx] == "in" || toks[count_noun_idx] == "from")) {
            count_noun_idx++;
        }
        if (count_noun_idx >= toks.size()) return NoMatch("count without a table");
        const std::string &noun = toks[count_noun_idx];
        const Table *t = ResolveTable(schema, noun);
        if (!t) return UnresolvedTable(noun);

        std::ostringstream sql;
        sql << "SELECT COUNT(*) FROM " << Q(t->name);
        if (!year.empty()) {
            if (const Column *dc = DateColumn(*t)) {
                sql << " WHERE " << RenderYearFilter(*dc, year);
            }
        }
        Result r; r.status = Status::OK; r.sql = sql.str(); return r;
    }

    // ---- PATTERN 1b: running total / cumulative (disabled in 0.1.7) ----
    // The pattern would emit `SUM(x) OVER (ORDER BY date)` which is standard
    // SQL, but SlothDB's current window engine doesn't produce the per-row
    // cumulative result for unbounded-frame windows (it returns the grand
    // total on every row) and doesn't accept the explicit
    // `ROWS UNBOUNDED PRECEDING` frame syntax either. Until both of those
    // land in the engine, `.ask running total of ...` refuses cleanly.
    if ((!toks.empty() && toks[0] == "cumulative") ||
        (toks.size() >= 2 && toks[0] == "running" && toks[1] == "total")) {
        return NoMatch(
            "running / cumulative aggregates need window-frame support "
            "that SlothDB 0.1.7 does not yet execute correctly. Once "
            "SUM OVER (ORDER BY ... ROWS UNBOUNDED PRECEDING) lands, "
            ".ask will generate it automatically. For now write the "
            "SUM OVER directly if you want to test.");
    }

    // ---- PATTERN 2: SUM / AVG / MIN / MAX ----
    // "total X", "sum of X", "average X", "mean X", "min X", "max X"
    // plus optional "per Y" / "by Y" → GROUP BY
    // plus optional "in YYYY" → WHERE year
    struct AggSpec { std::string kw; std::string sql_fn; };
    static const AggSpec aggs[] = {
        {"total",   "SUM"},
        {"sum",     "SUM"},
        {"average", "AVG"},
        {"avg",     "AVG"},
        {"mean",    "AVG"},
        {"min",     "MIN"},
        {"minimum", "MIN"},
        {"max",     "MAX"},
        {"maximum", "MAX"},
    };
    for (const auto &agg : aggs) {
        int idx = FindToken(toks, agg.kw);
        if (idx < 0) continue;
        // After the agg keyword, optional "of", then a noun → column.
        // Then optional "by"/"per" noun → group col.
        size_t i = static_cast<size_t>(idx) + 1;
        if (i < toks.size() && toks[i] == "of") i++;
        if (i >= toks.size()) continue;
        std::string col_noun = toks[i];
        // The table noun usually appears in "per <group> in <table>"
        // or right after the column noun, or implied by context. Try
        // the obvious nouns in order.

        // Build candidate table list: any token after "from"/"in"
        // (other than year) that resolves to a table; else the
        // first table in the schema whose columns include col_noun.
        const Table *table = nullptr;
        // Prefer explicit "from X" / "in X".
        for (size_t j = 0; j + 1 < toks.size(); j++) {
            if (toks[j] == "from" || (toks[j] == "in" && !IsYear(toks[j + 1]))) {
                if (const Table *t = ResolveTable(schema, toks[j + 1])) {
                    table = t; break;
                }
            }
        }
        if (!table) {
            // Implicit table: prefer a table that has an EXACT column-name
            // match before falling back to synonym routing. This matters
            // for e.g. "total price" in a schema with both products.price
            // and sales.amount — without the exact-match pass, synonyms
            // route "price" → "amount" and we pick sales, which is wrong.
            for (const auto &t : schema.tables) {
                for (const auto &c : t.columns) {
                    if (lower(c.name) == col_noun) { table = &t; break; }
                }
                if (table) break;
            }
            if (!table) {
                for (const auto &t : schema.tables) {
                    if (ResolveColumn(t, col_noun)) { table = &t; break; }
                }
            }
        }
        if (!table) return UnresolvedTable(col_noun);

        const Column *col = ResolveColumn(*table, col_noun);
        if (!col) return UnresolvedColumn(col_noun);

        // Group-by column on the chosen table, if NL asked for "by Y".
        const Column *gcol = nullptr;
        if (!group_by.empty()) {
            gcol = ResolveColumn(*table, group_by);
            if (!gcol) return UnresolvedColumn(group_by);
        }

        std::ostringstream sql;
        sql << "SELECT ";
        if (gcol) sql << Q(gcol->name) << ", ";
        sql << agg.sql_fn << "(" << Q(col->name) << ") AS "
            << Q(lower(agg.sql_fn) + "_" + col->name);
        sql << " FROM " << Q(table->name);
        if (!year.empty()) {
            if (const Column *dc = DateColumn(*table)) {
                sql << " WHERE " << RenderYearFilter(*dc, year);
            }
        }
        if (gcol) sql << " GROUP BY " << Q(gcol->name);
        if (top_n != 0) {
            int n = std::abs(top_n);
            sql << " ORDER BY " << agg.sql_fn << "(" << Q(col->name) << ") "
                << (top_n > 0 ? "DESC" : "ASC");
            sql << " LIMIT " << n;
        } else if (gcol) {
            sql << " ORDER BY " << Q(gcol->name);
        }
        Result r; r.status = Status::OK; r.sql = sql.str(); return r;
    }

    // ---- PATTERN 3: top-N plain ----
    // "top N X by Y" (without an agg keyword) — implies SUM of Y.
    if (top_n != 0 && toks.size() >= 3) {
        // Find "top"/"biggest"/"highest" position.
        int kw_idx = -1;
        for (size_t i = 0; i < toks.size(); i++) {
            if (toks[i] == "top" || toks[i] == "biggest" || toks[i] == "highest" ||
                toks[i] == "bottom" || toks[i] == "lowest" || toks[i] == "smallest") {
                kw_idx = static_cast<int>(i); break;
            }
        }
        if (kw_idx >= 0 && static_cast<size_t>(kw_idx) + 2 < toks.size()) {
            // tokens: top N <table> [by <metric>] OR top N <what> from <table> ...
            std::string tbl_noun = toks[kw_idx + 2];
            const Table *table = ResolveTable(schema, tbl_noun);
            // Fall back to looking up a group column inside any table.
            if (!table) {
                for (const auto &t : schema.tables) {
                    if (ResolveColumn(t, tbl_noun)) { table = &t; break; }
                }
                if (!table) return UnresolvedTable(tbl_noun);
            }

            // If the noun after "top N" matches a column, treat it as the
            // grouping column; otherwise it was the table and we need to
            // infer the grouping.
            const Column *group_col = ResolveColumn(*table, tbl_noun);
            // Find the metric: token after "by"/"per".
            std::string metric_noun = group_by;
            const Column *metric = nullptr;
            if (!metric_noun.empty()) {
                metric = ResolveColumn(*table, metric_noun);
                if (!metric) return UnresolvedColumn(metric_noun);
            }
            if (!metric) metric = FirstNumericColumn(*table);
            if (!metric) return NoMatch("no numeric column on " + table->name);

            // If the "top N X" X was the table itself, pick any non-metric
            // column as the group (first text-y column).
            if (!group_col) {
                for (const auto &c : table->columns) {
                    if (&c == metric) continue;
                    const std::string t = lower(c.type);
                    if (t.find("varchar") != std::string::npos ||
                        t.find("text") != std::string::npos ||
                        t.find("string") != std::string::npos) {
                        group_col = &c; break;
                    }
                }
            }

            std::ostringstream sql;
            sql << "SELECT ";
            if (group_col) sql << Q(group_col->name) << ", ";
            sql << "SUM(" << Q(metric->name) << ") AS "
                << Q("sum_" + metric->name);
            sql << " FROM " << Q(table->name);
            if (group_col) sql << " GROUP BY " << Q(group_col->name);
            sql << " ORDER BY SUM(" << Q(metric->name) << ") "
                << (top_n > 0 ? "DESC" : "ASC");
            sql << " LIMIT " << std::abs(top_n);
            Result r; r.status = Status::OK; r.sql = sql.str(); return r;
        }
    }

    // ---- PATTERN 4: 'latest X' / 'most recent X' / 'newest X' ----
    // Picks the single row with the largest value of the table's date-ish
    // column. Symmetrical 'oldest X' / 'earliest X' picks the smallest.
    {
        int dir = 0; // +1 for newest, -1 for oldest
        size_t i = 0;
        if (!toks.empty() &&
            (toks[0] == "latest" || toks[0] == "newest" ||
             toks[0] == "recent")) { dir = 1; i = 1; }
        else if (toks.size() >= 2 && toks[0] == "most" && toks[1] == "recent") {
            dir = 1; i = 2;
        }
        else if (!toks.empty() &&
                 (toks[0] == "oldest" || toks[0] == "earliest")) { dir = -1; i = 1; }

        if (dir != 0 && i < toks.size()) {
            // Optional "find me", "show me" etc. already stripped by Normalize.
            const std::string &noun = toks[i];
            const Table *t = ResolveTable(schema, noun);
            if (!t) return UnresolvedTable(noun);
            const Column *dc = DateColumn(*t);
            if (!dc) {
                return NoMatch("'" + toks[0] + " " + noun +
                               "' needs a date-like column on " + t->name +
                               " — none found. Try `.schema " + t->name +
                               "` to see columns.");
            }
            std::ostringstream sql;
            sql << "SELECT * FROM " << Q(t->name)
                << " ORDER BY " << Q(dc->name)
                << (dir > 0 ? " DESC" : " ASC")
                << " LIMIT 1";
            Result r; r.status = Status::OK; r.sql = sql.str(); return r;
        }
    }

    // ---- PATTERN 5: superlative 'which X has most Y' / 'X with most Y' ----
    // 'which month had most customers'  → SELECT month-expr, COUNT(DISTINCT
    //   customer) FROM t GROUP BY month-expr ORDER BY count DESC LIMIT 1.
    // 'which region has the highest amount' → SUM(amount) variant.
    //
    // The grouping key can be 'month' / 'year' / 'day' / 'week' — those
    // get rendered via EXTRACT / substring depending on the date col's
    // type — or any column on the implicit table.
    {
        int most_idx = -1;
        bool is_least = false;
        for (size_t i = 0; i < toks.size(); i++) {
            if (toks[i] == "most" || toks[i] == "highest" ||
                toks[i] == "biggest" || toks[i] == "largest") {
                most_idx = static_cast<int>(i); break;
            }
            if (toks[i] == "least" || toks[i] == "lowest" ||
                toks[i] == "smallest" || toks[i] == "fewest") {
                most_idx = static_cast<int>(i); is_least = true; break;
            }
        }
        // Need a grouping noun before most/highest/etc. "which X" or "X with".
        if (most_idx > 0 && static_cast<size_t>(most_idx) + 1 < toks.size()) {
            // The noun right before 'most' is the grouping key.
            const std::string &group_noun = toks[static_cast<size_t>(most_idx) - 1];

            // After 'most'/'highest'/etc., the metric noun may have
            // adjectives in front of it ("the most loyal repeat customers").
            // Scan forward for the first token that either matches a column
            // on some table or has a known synonym. Skip unknown adjectives.
            std::string metric_noun;
            const Table *table = nullptr;
            for (size_t j = static_cast<size_t>(most_idx) + 1; j < toks.size(); j++) {
                for (const auto &t : schema.tables) {
                    if (ResolveColumn(t, toks[j])) {
                        metric_noun = toks[j];
                        table = &t;
                        break;
                    }
                }
                if (table) break;
            }
            if (metric_noun.empty()) {
                // Didn't resolve any noun after 'most' — honest refusal.
                return NoMatch(
                    "saw '" + toks[static_cast<size_t>(most_idx)] +
                    "' but couldn't map the following nouns to any column. "
                    "Try naming a column explicitly (e.g. '... most orders', "
                    "'... highest amount').");
            }
            if (!table && !schema.tables.empty()) table = &schema.tables[0];
            if (!table) return NoMatch("no tables loaded");

            // Grouping expression: month/year/week/day on a date column →
            // EXTRACT; otherwise, a direct column match on the table.
            std::string group_expr;
            std::string group_alias;
            if (group_noun == "month" || group_noun == "year" ||
                group_noun == "week" || group_noun == "day") {
                const Column *dc = DateColumn(*table);
                if (!dc) return NoMatch("'" + group_noun +
                    "' needs a date column on " + table->name + " — none found.");
                std::string kw = group_noun;
                for (auto &c : kw) c = static_cast<char>(std::toupper(
                    static_cast<unsigned char>(c)));
                std::string tl = lower(dc->type);
                bool is_varchar = (tl.find("varchar") != std::string::npos ||
                                   tl.find("text") != std::string::npos ||
                                   tl.find("string") != std::string::npos);
                if (is_varchar) {
                    // VARCHAR date columns: function-in-GROUP-BY is currently
                    // unreliable in the engine (planner bug). Fall back to
                    // grouping by the raw column — coarser than the user
                    // asked ("per month" ends up per-day for ISO dates) but
                    // always correct. Promote to EXTRACT once the engine
                    // lands function-aware grouping.
                    group_expr = Q(dc->name);
                    group_alias = dc->name;
                } else {
                    group_expr = "EXTRACT(" + kw + " FROM " + Q(dc->name) + ")";
                    group_alias = group_noun;
                }
            } else {
                const Column *gcol = ResolveColumn(*table, group_noun);
                if (!gcol) return UnresolvedColumn(group_noun);
                group_expr = Q(gcol->name);
                group_alias = gcol->name;
            }

            // Metric: if the NL said "most customers", count distinct of the
            // matching column. If it said "most amount"/"highest revenue",
            // use SUM. Heuristic: a numeric column defaults to SUM, anything
            // else defaults to COUNT(DISTINCT).
            const Column *metric_col = ResolveColumn(*table, metric_noun);
            if (!metric_col) return UnresolvedColumn(metric_noun);
            std::string metric_t = lower(metric_col->type);
            std::string metric_n = lower(metric_col->name);
            // ID-ish columns (named `id` or ending `_id`) are semantically
            // identifiers, not metrics — summing them is nonsense. Use
            // COUNT(DISTINCT) for those. Everything else numeric → SUM.
            bool is_id_col = (metric_n == "id") ||
                             (metric_n.size() > 3 &&
                              metric_n.compare(metric_n.size() - 3, 3, "_id") == 0);
            bool numeric = !is_id_col &&
                           (metric_t.find("int") != std::string::npos ||
                            metric_t.find("double") != std::string::npos ||
                            metric_t.find("float") != std::string::npos ||
                            metric_t.find("decimal") != std::string::npos ||
                            metric_t.find("numeric") != std::string::npos);
            std::string metric_expr = numeric
                ? ("SUM(" + Q(metric_col->name) + ")")
                : ("COUNT(DISTINCT " + Q(metric_col->name) + ")");

            std::ostringstream sql;
            sql << "SELECT " << group_expr << " AS " << Q(group_alias)
                << ", " << metric_expr << " AS " << Q(numeric ? "total" : "n")
                << " FROM " << Q(table->name)
                << " GROUP BY " << group_expr
                << " ORDER BY " << metric_expr << (is_least ? " ASC" : " DESC")
                << " LIMIT 1";
            Result r; r.status = Status::OK; r.sql = sql.str(); return r;
        }
    }

    // ---- PATTERN 6: plain "X from Y" / "Y rows" — SELECT *  ----
    if (toks.size() >= 2 &&
        (toks[0] == "rows" || toks[0] == "data") &&
        toks[1] == "from") {
        if (toks.size() >= 3) {
            const Table *t = ResolveTable(schema, toks[2]);
            if (!t) return UnresolvedTable(toks[2]);
            Result r;
            r.status = Status::OK;
            r.sql = "SELECT * FROM " + Q(t->name) + " LIMIT 100";
            return r;
        }
    }

    return NoMatch(
        "no pattern matched. Try phrasings like 'how many X', "
        "'total X per Y', 'top N X by Y', 'latest X', 'which Y has most Z', "
        "'running total of X', 'average X in YYYY'. "
        "See docs/ASK.md for the full list.");
}

} // namespace ask
} // namespace slothdb
