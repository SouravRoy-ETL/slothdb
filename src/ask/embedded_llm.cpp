#include "slothdb/ask/embedded_llm.hpp"
#include "slothdb/storage/http_client.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef SLOTHDB_ASK_MODEL
#include "llama.h"
#endif

namespace slothdb {
namespace ask {

namespace {

std::string HomeDir() {
#ifdef _MSC_VER
    char *buf = nullptr; size_t sz = 0;
    if (_dupenv_s(&buf, &sz, "USERPROFILE") == 0 && buf) {
        std::string v(buf); free(buf); return v;
    }
    return ".";
#else
    const char *h = std::getenv("HOME");
    return h ? std::string(h) : std::string(".");
#endif
}

// Wrap an identifier in double quotes if it contains any character that
// would need quoting in SQL (spaces, punctuation, starts with a digit, or
// is a reserved word the user might hit). Otherwise emit as-is. We always
// quote when in doubt - the model then mimics the form it sees.
std::string QuoteIdentIfNeeded(const std::string &name) {
    auto needs_quote = [&]() {
        if (name.empty()) return true;
        if (std::isdigit(static_cast<unsigned char>(name[0]))) return true;
        for (char c : name) {
            bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_';
            if (!ok) return true;
        }
        return false;
    };
    if (needs_quote()) return "\"" + name + "\"";
    return name;
}

// Render a DDL block for the prompt. Capped at 20 tables - enough for
// realistic shell sessions, prevents the prompt from exploding if the
// user has an enormous catalog attached. Identifiers containing spaces
// or other non-word characters are rendered with surrounding double
// quotes so the model learns by example that multi-word column names
// MUST be quoted in the generated SQL. File-source tables (produced by
// the shell's schema-peek for un-loaded files) are tagged with a
// comment that tells the model to use `FROM '<path>'` instead of
// treating the alias as a regular table name.
std::string RenderDDL(const Schema &schema) {
    std::ostringstream ss;
    size_t limit = schema.tables.size() < 20 ? schema.tables.size() : 20;
    for (size_t i = 0; i < limit; i++) {
        const auto &t = schema.tables[i];
        if (!t.source_file.empty()) {
            ss << "-- File source: query via FROM '" << t.source_file
               << "' (single quotes). Schema below shows its columns:\n";
        }
        ss << "CREATE TABLE " << QuoteIdentIfNeeded(t.name) << " (";
        for (size_t j = 0; j < t.columns.size(); j++) {
            if (j > 0) ss << ", ";
            ss << QuoteIdentIfNeeded(t.columns[j].name) << " "
               << t.columns[j].type;
        }
        ss << ");\n";
    }
    return ss.str();
}

// Build the chat-formatted prompt. Qwen2.5 uses the ChatML format
// (<|im_start|>...<|im_end|>). We render that directly - llama.cpp's
// chat-template API would work too but this is explicit and easier to
// debug when output looks wrong. The quoting rule is spelled out
// explicitly because small models drop it without the reminder.
std::string RenderPromptQwen(const std::string &ddl, const std::string &question) {
    std::ostringstream ss;
    ss << "<|im_start|>system\n"
       << "You are a SQL generator for SlothDB (DuckDB-compatible dialect). "
       << "Given the schema below and a natural-language question, output "
       << "ONE SQL SELECT statement that answers the question. No commentary, "
       << "no markdown fences, no explanations.\n"
       << "Rules:\n"
       << "  1. Use only tables and columns from the schema below.\n"
       << "  2. Prefer explicit column names over SELECT *.\n"
       << "  3. Any identifier that is quoted in the schema (wrapped in "
       << "double quotes) MUST also be quoted in the SQL you generate. "
       << "This includes every column name that contains a space "
       << "(e.g. `\"Customer Id\"`, `\"Subscription Date\"`).\n"
       << "  4. String literals use single quotes. Identifiers use double "
       << "quotes. Never confuse the two.\n"
       << "  5. When an alias contains a space, quote it with double "
       << "quotes too: `COUNT(*) AS \"Total Customers\"`.\n"
       << "\nAnalytic patterns:\n"
       << "  - \"top N per group\" / \"rank within each group\" -> use\n"
       << "    ROW_NUMBER() OVER (PARTITION BY grp ORDER BY metric DESC) AS rn\n"
       << "    wrap in a subquery and filter WHERE rn <= N.\n"
       << "    Example: top 3 customers by revenue per region:\n"
       << "      SELECT * FROM (\n"
       << "        SELECT region, customer_id, revenue,\n"
       << "               ROW_NUMBER() OVER (PARTITION BY region ORDER BY revenue DESC) AS rn\n"
       << "        FROM sales\n"
       << "      ) WHERE rn <= 3;\n"
       << "  - \"compare to previous / next row\" -> use LAG() / LEAD()\n"
       << "    OVER (PARTITION BY ... ORDER BY ...).\n"
       << "  - \"running total / cumulative sum\" -> NOT SUPPORTED by\n"
       << "    SlothDB's executor yet. Respond with REFUSE_RUNNING_TOTAL\n"
       << "    so the shell can tell the user cleanly instead of running\n"
       << "    a query that returns wrong numbers.\n"
       << "\nSchema:\n"
       << ddl
       << "<|im_end|>\n"
       << "<|im_start|>user\n"
       << question
       << "<|im_end|>\n"
       << "<|im_start|>assistant\n";
    return ss.str();
}

// Strip markdown fences + trim + drop trailing semicolons from model output.
std::string ExtractSQL(const std::string &text) {
    std::string s = text;
    auto fence = s.find("```");
    if (fence != std::string::npos) {
        size_t start = fence + 3;
        while (start < s.size() && s[start] != '\n') start++;
        if (start < s.size()) start++;
        auto end = s.find("```", start);
        if (end != std::string::npos) {
            s = s.substr(start, end - start);
        }
    }
    auto is_ws = [](char c){ return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && is_ws(s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws(s.back())) s.pop_back();
    while (!s.empty() && s.back() == ';') s.pop_back();
    return s;
}

// Wrap multi-word AS aliases in double quotes. Qwen sometimes emits
// `COUNT(*) AS Total Customers` which the parser rejects because
// `Customers` starts a new statement. Scan for `AS <ident> <ident>...`
// at the top level (not inside strings or already-quoted idents) and
// merge the run of bare identifier-like tokens into a single quoted
// alias. Stops at a non-identifier character (comma, FROM, WHERE, ...).
std::string QuoteMultiWordAliases(const std::string &sql) {
    std::string out;
    out.reserve(sql.size() + 8);
    bool in_str = false, in_dq = false;
    size_t i = 0;

    auto is_id_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    auto is_space = [](char c) { return c == ' ' || c == '\t'; };

    // Reserved words we should not swallow into an alias.
    static const char *boundary_words[] = {
        "FROM", "WHERE", "GROUP", "ORDER", "LIMIT", "HAVING", "QUALIFY",
        "UNION", "INTERSECT", "EXCEPT", "JOIN", "ON", "INNER", "LEFT",
        "RIGHT", "FULL", "CROSS", "USING", "AS", "WITH", "SELECT",
        "AND", "OR", "NOT", "ASC", "DESC", nullptr
    };
    auto is_boundary = [&](const std::string &w) {
        std::string up;
        for (char c : w) up.push_back(static_cast<char>(std::toupper(
            static_cast<unsigned char>(c))));
        for (int k = 0; boundary_words[k]; k++)
            if (up == boundary_words[k]) return true;
        return false;
    };

    while (i < sql.size()) {
        char c = sql[i];
        if (in_str) {
            out.push_back(c);
            if (c == '\'' && (i + 1 >= sql.size() || sql[i + 1] != '\'')) in_str = false;
            i++; continue;
        }
        if (in_dq) {
            out.push_back(c);
            if (c == '"') in_dq = false;
            i++; continue;
        }
        if (c == '\'') { in_str = true; out.push_back(c); i++; continue; }
        if (c == '"')  { in_dq  = true; out.push_back(c); i++; continue; }

        // Look for "AS" (case-insensitive) at a word boundary.
        bool as_match = false;
        if (i + 2 <= sql.size()) {
            char a = std::tolower(static_cast<unsigned char>(sql[i]));
            char b = std::tolower(static_cast<unsigned char>(sql[i + 1]));
            if (a == 'a' && b == 's') {
                bool left_ok = (i == 0) || !is_id_char(sql[i - 1]);
                bool right_ok = (i + 2 >= sql.size()) || !is_id_char(sql[i + 2]);
                if (left_ok && right_ok) as_match = true;
            }
        }
        if (!as_match) { out.push_back(c); i++; continue; }

        // Consume `AS`.
        size_t p = i + 2;
        // Skip spaces after AS.
        while (p < sql.size() && is_space(sql[p])) p++;
        // If the next char is a double quote (already-quoted alias),
        // leave everything alone.
        if (p < sql.size() && sql[p] == '"') {
            out.append(sql, i, p - i);
            i = p; continue;
        }
        // Collect first identifier token.
        size_t t1_start = p;
        while (p < sql.size() && is_id_char(sql[p])) p++;
        if (p == t1_start) { out.push_back(c); i++; continue; }
        std::string first = sql.substr(t1_start, p - t1_start);

        // Try to append more identifier tokens separated by single spaces,
        // stopping at any boundary word, punctuation, or line break.
        std::vector<std::string> tokens = {first};
        size_t try_p = p;
        while (try_p < sql.size() && sql[try_p] == ' ') {
            size_t probe = try_p + 1;
            if (probe >= sql.size() || !is_id_char(sql[probe])) break;
            size_t tok_start = probe;
            while (probe < sql.size() && is_id_char(sql[probe])) probe++;
            std::string tok = sql.substr(tok_start, probe - tok_start);
            if (is_boundary(tok)) break;
            tokens.push_back(tok);
            try_p = probe;
        }
        if (tokens.size() <= 1) {
            // Single-word alias - leave as-is.
            out.append(sql, i, p - i);
            i = p; continue;
        }

        // Emit `AS "<multi word alias>"`.
        out += "AS \"";
        for (size_t k = 0; k < tokens.size(); k++) {
            if (k) out += ' ';
            out += tokens[k];
        }
        out += "\"";
        i = try_p;
    }
    return out;
}

// Rewrite FROM/JOIN references to a file-source alias back to the real
// path as a single-quoted file literal. Qwen reads the "-- File source"
// hint in the DDL but still sometimes emits `FROM file_stem` or
// `FROM "C:\...\file.csv"` instead of `FROM 'C:\...\file.csv'`.
// This pass corrects both, so questions over files that were peeked
// into the schema actually execute.
std::string RewriteFileAliases(const std::string &sql, const Schema &schema) {
    // Collect (alias, path) pairs for file-source tables.
    std::vector<std::pair<std::string, std::string>> pairs;
    for (const auto &t : schema.tables) {
        if (!t.source_file.empty()) pairs.emplace_back(t.name, t.source_file);
    }
    if (pairs.empty()) return sql;

    // Longest alias first.
    std::sort(pairs.begin(), pairs.end(),
              [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });

    std::string out;
    out.reserve(sql.size() + 32);
    bool in_str = false, in_dq = false;
    size_t i = 0;
    while (i < sql.size()) {
        char c = sql[i];
        if (in_str) {
            out.push_back(c);
            if (c == '\'' && (i + 1 >= sql.size() || sql[i + 1] != '\'')) in_str = false;
            i++; continue;
        }
        if (in_dq) {
            out.push_back(c);
            if (c == '"') in_dq = false;
            i++; continue;
        }
        if (c == '\'') { in_str = true; out.push_back(c); i++; continue; }
        if (c == '"')  { in_dq  = true; out.push_back(c); i++; continue; }

        bool matched = false;
        for (const auto &[alias, path] : pairs) {
            if (i + alias.size() > sql.size()) continue;
            bool eq = true;
            for (size_t k = 0; k < alias.size(); k++) {
                char a = std::tolower(static_cast<unsigned char>(sql[i + k]));
                char b = std::tolower(static_cast<unsigned char>(alias[k]));
                if (a != b) { eq = false; break; }
            }
            if (!eq) continue;
            if (i > 0) {
                char p = sql[i - 1];
                if (std::isalnum(static_cast<unsigned char>(p)) || p == '_' || p == '"' || p == '\'') continue;
            }
            size_t after = i + alias.size();
            if (after < sql.size()) {
                char n = sql[after];
                if (std::isalnum(static_cast<unsigned char>(n)) || n == '_' || n == '"' || n == '\'') continue;
            }
            out.push_back('\'');
            out.append(path);
            out.push_back('\'');
            i = after;
            matched = true;
            break;
        }
        if (!matched) {
            out.push_back(c);
            i++;
        }
    }
    return out;
}

// Post-process the model's SQL: wrap any schema identifier that contains
// a space with double quotes, unless it is already quoted or is inside a
// string literal. Small models (0.5B Qwen) drop quotes on multi-word
// identifiers even with an explicit prompt rule, so we enforce it after
// the fact. Only multi-word identifiers are rewritten - single-word
// names don't need quoting and rewriting them would risk breaking
// keywords like SELECT / FROM.
std::string QuoteSchemaIdents(const std::string &sql, const Schema &schema) {
    // Collect identifiers that contain a space. Tables and columns both.
    std::vector<std::string> targets;
    auto add_if_multiword = [&](const std::string &s) {
        if (s.find(' ') != std::string::npos) targets.push_back(s);
    };
    for (const auto &t : schema.tables) {
        add_if_multiword(t.name);
        for (const auto &c : t.columns) add_if_multiword(c.name);
    }
    if (targets.empty()) return sql;

    // Longest-first so "Subscription Date End" rewrites before "Subscription
    // Date" (which is a prefix of the longer name).
    std::sort(targets.begin(), targets.end(),
              [](const std::string &a, const std::string &b) {
                  return a.size() > b.size();
              });

    // Walk the SQL. Track whether we're inside a string literal ('...') or
    // already-quoted identifier ("..."). Both are left untouched.
    std::string out;
    out.reserve(sql.size() + 16);
    bool in_str = false;   // inside '...'
    bool in_dq  = false;   // inside "..."
    size_t i = 0;
    while (i < sql.size()) {
        char c = sql[i];
        if (in_str) {
            out.push_back(c);
            if (c == '\'' && (i + 1 >= sql.size() || sql[i + 1] != '\'')) in_str = false;
            i++; continue;
        }
        if (in_dq) {
            out.push_back(c);
            if (c == '"') in_dq = false;
            i++; continue;
        }
        if (c == '\'') { in_str = true; out.push_back(c); i++; continue; }
        if (c == '"')  { in_dq  = true; out.push_back(c); i++; continue; }

        // Try to match a target at this position (case-insensitive).
        bool matched = false;
        for (const auto &t : targets) {
            if (i + t.size() > sql.size()) continue;
            bool eq = true;
            for (size_t k = 0; k < t.size(); k++) {
                char a = std::tolower(static_cast<unsigned char>(sql[i + k]));
                char b = std::tolower(static_cast<unsigned char>(t[k]));
                if (a != b) { eq = false; break; }
            }
            if (!eq) continue;
            // Left boundary: must not be alnum or underscore (otherwise
            // we'd match a middle of a longer identifier).
            if (i > 0) {
                char p = sql[i - 1];
                if (std::isalnum(static_cast<unsigned char>(p)) || p == '_' || p == '"') continue;
            }
            // Right boundary: same check.
            size_t after = i + t.size();
            if (after < sql.size()) {
                char n = sql[after];
                if (std::isalnum(static_cast<unsigned char>(n)) || n == '_' || n == '"') continue;
            }
            // Rewrite: emit the schema-canonical form wrapped in quotes.
            out.push_back('"');
            out.append(t);
            out.push_back('"');
            i = after;
            matched = true;
            break;
        }
        if (!matched) {
            out.push_back(c);
            i++;
        }
    }
    return out;
}

} // namespace

std::string ModelDir() {
    auto p = std::filesystem::path(HomeDir()) / ".slothdb" / "models";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p.string();
}

const ModelSpec &SmallModel() {
    static const ModelSpec spec = {
        /*name=*/          "Qwen2.5-Coder-0.5B-Instruct-Q4_K_M",
        /*url=*/           "https://huggingface.co/Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF/"
                           "resolve/main/qwen2.5-coder-0.5b-instruct-q4_k_m.gguf",
        /*sha256=*/        "", // TODO: pin on first release
        /*local_file=*/    "qwen2.5-coder-0.5b-instruct.Q4_K_M.gguf",
        /*chat_template=*/ "qwen",
        /*expected_bytes=*/ 310ULL * 1024ULL * 1024ULL
    };
    return spec;
}

const ModelSpec &LargeModel() {
    static const ModelSpec spec = {
        /*name=*/          "Qwen2.5-Coder-1.5B-Instruct-Q4_K_M",
        /*url=*/           "https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF/"
                           "resolve/main/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf",
        /*sha256=*/        "", // TODO: pin on first release
        /*local_file=*/    "qwen2.5-coder-1.5b-instruct.Q4_K_M.gguf",
        /*chat_template=*/ "qwen",
        /*expected_bytes=*/ 986ULL * 1024ULL * 1024ULL
    };
    return spec;
}

// Backwards-compatible default - the small tier. Routing now happens
// per-question via PickModelForQuestion(). Keep this pointing at the
// small tier so any old caller still works.
const ModelSpec &DefaultModel() { return SmallModel(); }

const ModelSpec &PickModelForQuestion(const std::string &question) {
    std::string lo;
    lo.reserve(question.size());
    for (char c : question)
        lo.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    // Signals that analytic / window-function SQL is probably needed.
    // Matching ANY of these routes to the 1.5B tier. False positives are
    // cheap (just a slower / larger model that is already downloaded);
    // false negatives matter more - the 0.5B produces wrong-shape SQL.
    static const char *analytic_markers[] = {
        " per ", " within each ", " in each ", " for each ",
        " rank ", "ranking", "top-", "top_",
        " biggest ", " smallest ", " largest ", " highest ", " lowest ",
        "appears most", "appears least",
        " most common", " least common", " most frequent", " least frequent",
        "most common", "least common", "most frequent", "least frequent",
        "row_number", "partition by", "over (", "over(",
        "lag(", "lead(", "ntile", "percentile",
        " join ", "join ", " inner join", " left join", " right join",
        " correlat", " regression", " median ", " percentile ",
        " distinct ", " unique ",
        nullptr
    };
    for (int i = 0; analytic_markers[i]; i++) {
        if (lo.find(analytic_markers[i]) != std::string::npos) {
            return LargeModel();
        }
    }
    return SmallModel();
}

// Helpers for the background-download coordination below.
static std::string SpecPath(const ModelSpec &spec) {
    return (std::filesystem::path(ModelDir()) / spec.local_file).string();
}

static bool SpecAlreadyOnDisk(const ModelSpec &spec) {
    std::error_code ec;
    auto path = SpecPath(spec);
    if (!std::filesystem::exists(path, ec)) return false;
    auto sz = std::filesystem::file_size(path, ec);
    // Sanity: must be at least half the expected size to avoid trusting
    // a truncated partial download from a previous attempt.
    return !ec && sz > spec.expected_bytes / 2;
}

// Tracks downloads in flight so StartBackgroundDownloads is idempotent
// and EnsureModelAvailable can wait on a thread that's already running.
// Keyed by local_file (unique per tier).
namespace {
struct DownloadRegistry {
    std::mutex m;
    std::set<std::string> in_flight;
} g_dl;
} // namespace

static void TryBackgroundDownload(const ModelSpec &spec) {
    {
        std::lock_guard<std::mutex> lk(g_dl.m);
        if (g_dl.in_flight.count(spec.local_file)) return; // already running
        if (SpecAlreadyOnDisk(spec)) return;
        g_dl.in_flight.insert(spec.local_file);
    }
    std::thread([spec]() {
        auto path = SpecPath(spec);
        HTTPClient::DownloadToFile(spec.url, path);
        std::lock_guard<std::mutex> lk(g_dl.m);
        g_dl.in_flight.erase(spec.local_file);
    }).detach();
}

void StartBackgroundDownloads() {
    // Both tiers; idempotent, non-blocking.
    TryBackgroundDownload(SmallModel());
    TryBackgroundDownload(LargeModel());
}

std::string DefaultModelPath() {
    return (std::filesystem::path(ModelDir()) / DefaultModel().local_file).string();
}

bool EmbeddedAvailable() {
#ifdef SLOTHDB_ASK_MODEL
    return true;
#else
    return false;
#endif
}

bool EnsureModelAvailable(const ModelSpec &spec, bool verbose, std::string &err) {
    auto path = SpecPath(spec);

    // Fast path.
    if (SpecAlreadyOnDisk(spec)) return true;

    // Is a background download running for this spec already? If so,
    // wait for it; user doesn't care whether the bytes came from here or
    // from the detached thread kicked off at .ask start.
    bool was_in_flight = false;
    {
        std::lock_guard<std::mutex> lk(g_dl.m);
        was_in_flight = g_dl.in_flight.count(spec.local_file) > 0;
    }
    if (was_in_flight) {
        if (verbose) {
            fprintf(stderr, "  Waiting for %s background download...\n",
                    spec.name);
        }
        while (true) {
            {
                std::lock_guard<std::mutex> lk(g_dl.m);
                if (!g_dl.in_flight.count(spec.local_file)) break;
            }
            if (SpecAlreadyOnDisk(spec)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (SpecAlreadyOnDisk(spec)) return true;
        // Background thread finished but file not usable - fall through
        // and retry inline so the user gets a proper error if the URL
        // itself is broken.
    }

    if (verbose) {
        fprintf(stderr, "  Downloading %s (~%zu MB) from:\n    %s\n  to: %s\n",
                spec.name,
                static_cast<size_t>(spec.expected_bytes / (1024ULL * 1024ULL)),
                spec.url, path.c_str());
        fprintf(stderr, "  (one-time; lives in ~/.slothdb/models/)\n");
    }
    {
        std::lock_guard<std::mutex> lk(g_dl.m);
        g_dl.in_flight.insert(spec.local_file);
    }
    bool ok = HTTPClient::DownloadToFile(spec.url, path);
    {
        std::lock_guard<std::mutex> lk(g_dl.m);
        g_dl.in_flight.erase(spec.local_file);
    }
    if (!ok) {
        err = "Download failed. Check network and retry, or grab the file "
              "manually and put it at: " + path;
        return false;
    }
    if (verbose) fprintf(stderr, "  Model downloaded.\n");
    return true;
}

bool EnsureModelDownloaded(bool verbose, std::string &err) {
    return EnsureModelAvailable(DefaultModel(), verbose, err);
}

#ifdef SLOTHDB_ASK_MODEL

// Silent log callback so llama.cpp / ggml don't flood stderr with
// "llama_model_loader: ..." lines during every .ask call. Warnings and
// errors still reach us - we just drop info/debug.
static void silent_log(ggml_log_level level, const char *text, void *) {
    if (level >= GGML_LOG_LEVEL_ERROR) {
        fputs(text, stderr);
    }
}

EmbeddedResult GenerateSQLLocal(const Schema &schema,
                                 const std::string &question) {
    EmbeddedResult r;

    // Kick both tiers' downloads in background. Idempotent; a tier that
    // is already on disk or already downloading is a no-op. This means
    // simple questions served by the 0.5B don't pay the 1.5B's cold-
    // download latency - the large model streams in while the user is
    // asking, and is ready for the next analytic question.
    StartBackgroundDownloads();

    // Per-question routing: analytic shapes go to the large tier; simple
    // shapes stay on small. Deterministic based on lowercase keywords.
    const ModelSpec &spec = PickModelForQuestion(question);

    std::string err;
    if (!EnsureModelAvailable(spec, /*verbose=*/true, err)) {
        r.message = err; return r;
    }
    auto path = SpecPath(spec);

    // Silence llama.cpp + ggml chatter before any call that could log.
    llama_log_set(silent_log, nullptr);
    ggml_log_set(silent_log, nullptr);

    // Initialise llama.cpp backend (idempotent; cheap to call each time).
    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0; // CPU-only; GPU would require platform-specific
                              // backends which we don't link.
    llama_model *model = llama_model_load_from_file(path.c_str(), mparams);
    if (!model) {
        r.message = "llama_model_load_from_file failed for: " + path;
        return r;
    }

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx     = 2048;
    cparams.n_batch   = 512;
    cparams.n_threads = 4;
    llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx) {
        llama_model_free(model);
        r.message = "llama_init_from_model failed";
        return r;
    }

    const std::string prompt = RenderPromptQwen(RenderDDL(schema), question);

    // Tokenize.
    const llama_vocab *vocab = llama_model_get_vocab(model);
    const int max_tokens = 4096;
    std::vector<llama_token> tokens(max_tokens);
    int n_tokens = llama_tokenize(vocab, prompt.c_str(), (int)prompt.size(),
                                  tokens.data(), max_tokens,
                                  /*add_special=*/true, /*parse_special=*/true);
    if (n_tokens < 0) {
        llama_free(ctx); llama_model_free(model);
        r.message = "tokenize failed";
        return r;
    }
    tokens.resize(n_tokens);

    // Decode prompt.
    llama_batch batch = llama_batch_init((int)tokens.size(), 0, 1);
    batch.n_tokens = (int)tokens.size();
    for (int i = 0; i < batch.n_tokens; i++) {
        batch.token[i]    = tokens[i];
        batch.pos[i]      = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i]   = (i == batch.n_tokens - 1) ? 1 : 0;
    }
    if (llama_decode(ctx, batch) != 0) {
        llama_batch_free(batch);
        llama_free(ctx); llama_model_free(model);
        r.message = "llama_decode (prompt) failed";
        return r;
    }

    // Greedy decode - simple and deterministic. The question space here
    // (single SQL query) doesn't benefit much from sampling.
    std::string out;
    int n_predict = 512;
    int cur_pos = batch.n_tokens;
    for (int step = 0; step < n_predict; step++) {
        float *logits = llama_get_logits_ith(ctx, batch.n_tokens - 1);
        int vocab_size = llama_vocab_n_tokens(vocab);
        // Argmax.
        int best = 0;
        float best_v = logits[0];
        for (int i = 1; i < vocab_size; i++) {
            if (logits[i] > best_v) { best_v = logits[i]; best = i; }
        }
        llama_token tok = best;
        if (llama_vocab_is_eog(vocab, tok)) break;

        // Detokenize incrementally.
        char piece[256];
        int np = llama_token_to_piece(vocab, tok, piece, sizeof(piece),
                                       /*lstrip=*/0, /*special=*/false);
        if (np > 0) out.append(piece, np);

        // Stop on </assistant> marker or ChatML end.
        if (out.find("<|im_end|>") != std::string::npos) break;

        // Feed back.
        batch.n_tokens = 1;
        batch.token[0]    = tok;
        batch.pos[0]      = cur_pos++;
        batch.n_seq_id[0] = 1;
        batch.seq_id[0][0] = 0;
        batch.logits[0]   = 1;
        if (llama_decode(ctx, batch) != 0) break;
    }

    llama_batch_free(batch);
    llama_free(ctx);
    llama_model_free(model);

    // Strip any trailing <|im_end|> and friends.
    auto end_marker = out.find("<|im_end|>");
    if (end_marker != std::string::npos) out = out.substr(0, end_marker);

    r.sql = RewriteFileAliases(
        QuoteMultiWordAliases(QuoteSchemaIdents(ExtractSQL(out), schema)),
        schema);
    if (r.sql.empty()) {
        r.message = "model returned empty SQL";
        return r;
    }
    r.ok = true;
    return r;
}

#else // !SLOTHDB_ASK_MODEL

EmbeddedResult GenerateSQLLocal(const Schema &, const std::string &) {
    EmbeddedResult r;
    r.message = "embedded model not compiled in - "
                "rebuild with -DSLOTHDB_ASK_MODEL=ON (see docs/ASK.md)";
    return r;
}

#endif

} // namespace ask
} // namespace slothdb
