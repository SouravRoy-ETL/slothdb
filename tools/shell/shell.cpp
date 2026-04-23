/*
 * SlothDB CLI Shell
 *
 * Interactive SQL REPL for SlothDB.
 * Usage: slothdb [database_path]
 *        slothdb -c "SELECT ..."
 */

#include "slothdb/api/slothdb.h"
#include "slothdb/ask/nl_to_sql.hpp"
#include "slothdb/ask/llm_provider.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <iostream>

#ifdef SLOTHDB_HAS_LINENOISE
extern "C" {
#include "linenoise.h"
}
#endif

static const char *kHistoryFile = ".slothdb_history";

static void print_result(slothdb_result *result) {
    uint64_t cols = slothdb_column_count(result);
    uint64_t rows = slothdb_row_count(result);

    if (cols == 0) {
        printf("OK\n");
        return;
    }

    for (uint64_t c = 0; c < cols; c++) {
        if (c > 0) printf(" | ");
        printf("%-15s", slothdb_column_name(result, c));
    }
    printf("\n");

    for (uint64_t c = 0; c < cols; c++) {
        if (c > 0) printf("-+-");
        printf("---------------");
    }
    printf("\n");

    for (uint64_t r = 0; r < rows; r++) {
        for (uint64_t c = 0; c < cols; c++) {
            if (c > 0) printf(" | ");
            printf("%-15s", slothdb_value_varchar(result, r, c));
        }
        printf("\n");
    }

    printf("(%llu row%s)\n", (unsigned long long)rows, rows == 1 ? "" : "s");
}

static void print_help() {
    printf("\n");
    printf("  Dot commands\n");
    printf("  ------------\n");
    printf("  .help                       Show this help\n");
    printf("  .quit / .exit               Exit the shell\n");
    printf("  .tables [PATTERN]           List tables (LIKE pattern optional)\n");
    printf("  .schema [TABLE]             Show CREATE statements for tables\n");
    printf("  .open <path>                Close current DB and open a new one\n");
    printf("  .ask <question>             Translate NL question to SQL and run it\n");
    printf("  .version                    Print SlothDB version\n");
    printf("  .clear                      Clear the screen\n");
    printf("\n");
    printf("  Query any file directly\n");
    printf("  -----------------------\n");
    printf("  SELECT * FROM 'data.csv';\n");
    printf("  SELECT * FROM read_parquet('events.parquet');\n");
    printf("  SELECT * FROM 'logs.json';\n");
    printf("  SELECT * FROM read_xlsx('report.xlsx');\n");
    printf("  SELECT * FROM read_avro('events.avro');\n");
    printf("  SELECT * FROM sqlite_scan('app.db', 'users');\n");
    printf("\n");
    printf("  Persistent tables (single-file .slothdb database)\n");
    printf("  -------------------------------------------------\n");
    printf("  CREATE TABLE sales AS SELECT * FROM 'sales.csv';\n");
    printf("  SELECT region, SUM(revenue) FROM sales GROUP BY region;\n");
    printf("\n");
    printf("  Multi-line input\n");
    printf("  ----------------\n");
    printf("  Statements run when they end with a semicolon.\n");
    printf("  Enter by itself cancels a partial statement.\n");
    printf("\n");
    printf("  Shell features\n");
    printf("  --------------\n");
#ifdef SLOTHDB_HAS_LINENOISE
    printf("  Arrow keys move the cursor. Up/Down browses history.\n");
    printf("  History persists in .slothdb_history in the current directory.\n");
    printf("  Ctrl+C cancels input. Ctrl+D at empty prompt exits.\n");
#else
    printf("  On Windows, use cmd.exe's native line editing.\n");
    printf("  Ctrl+C exits.\n");
#endif
    printf("\n");
    printf("  Full documentation: https://github.com/SouravRoy-ETL/slothdb\n");
    printf("\n");
}

// Run a query string, print its result (or error). Returns true on success.
static bool run_query(slothdb_connection *conn, const std::string &sql) {
    slothdb_result *result = nullptr;
    auto status = slothdb_query(conn, sql.c_str(), &result);
    if (status == SLOTHDB_OK) {
        print_result(result);
        slothdb_free_result(result);
        return true;
    }
    fprintf(stderr, "Error: %s\n", slothdb_result_error(result));
    slothdb_free_result(result);
    return false;
}

// Build a NL-to-SQL schema snapshot using the catalog-introspection C API.
// Straight ports the catalog contents into the engine's Schema struct;
// each string is copied immediately since the API's returned pointers
// only live until the next call to the same function.
static slothdb::ask::Schema build_ask_schema(slothdb_connection *conn) {
    slothdb::ask::Schema schema;
    uint64_t n_tables = slothdb_table_count(conn);
    for (uint64_t i = 0; i < n_tables; i++) {
        std::string tname = slothdb_table_name(conn, i);
        slothdb::ask::Table t;
        t.name = tname;
        uint64_t n_cols = slothdb_table_column_count(conn, i);
        for (uint64_t j = 0; j < n_cols; j++) {
            slothdb::ask::Column c;
            c.name = slothdb_table_column_name(conn, i, j);
            c.type = slothdb_table_column_type(conn, i, j);
            t.columns.push_back(std::move(c));
        }
        schema.tables.push_back(std::move(t));
    }
    return schema;
}

// Execute a generated SQL string with the usual [Y/n] confirm prompt.
// Shared between the rules path and the LLM fallback so the safety rail
// is identical: user sees the SQL, types y/n, nothing runs without
// confirmation.
static bool confirm_and_run(slothdb_connection *conn, const std::string &sql) {
    printf("-- %s\n", sql.c_str());
    printf("Run? [Y/n] ");
    fflush(stdout);
    char buf[16];
    if (!fgets(buf, sizeof(buf), stdin)) { printf("\n"); return false; }
    char ch = buf[0];
    if (ch == 'n' || ch == 'N') { printf("Skipped.\n"); return true; }

    slothdb_result *exec_result = nullptr;
    auto status = slothdb_query(conn, sql.c_str(), &exec_result);
    if (status == SLOTHDB_OK) {
        print_result(exec_result);
    } else {
        fprintf(stderr, "Error: %s\n", slothdb_result_error(exec_result));
    }
    slothdb_free_result(exec_result);
    return true;
}

// Run one NL question against .ask. `force_ai` skips the rules path and
// goes straight to the LLM (the `--ai` flag). Otherwise tries rules
// first; on refusal, offers the LLM fallback if a provider is configured.
static bool ask_once(slothdb_connection *conn, const std::string &question,
                     bool force_ai = false) {
    auto schema = build_ask_schema(conn);
    if (schema.tables.empty()) {
        printf("No tables found. Load data first (create a table or "
               "query a file) before using .ask.\n");
        return true;
    }

    if (!force_ai) {
        auto r = slothdb::ask::Translate(question, schema);
        if (r.status == slothdb::ask::Status::OK) {
            return confirm_and_run(conn, r.sql);
        }
        // Rule refusal — print, then try LLM if available.
        printf("Rules didn't match: %s\n", r.message.c_str());
        if (!r.unresolved.empty()) {
            printf("  Unresolved token: '%s'\n", r.unresolved.c_str());
        }
    }

    auto cfg = slothdb::ask::ConfigFromEnvironment();
    if (cfg.provider == slothdb::ask::Provider::None) {
        printf("  No LLM provider configured. Set SLOTHDB_ASK_PROVIDER=ollama "
               "(or openai/anthropic) to enable AI fallback. .ask stays offline "
               "by default.\n");
        return true;
    }

    const char *prov_name = "unknown";
    switch (cfg.provider) {
        case slothdb::ask::Provider::Ollama:    prov_name = "ollama";    break;
        case slothdb::ask::Provider::OpenAI:    prov_name = "openai";    break;
        case slothdb::ask::Provider::Anthropic: prov_name = "anthropic"; break;
        default: break;
    }
    printf("-- trying %s (%s)...\n", prov_name, cfg.model.c_str());
    fflush(stdout);

    auto lr = slothdb::ask::GenerateSQL(cfg, schema, question);
    if (!lr.ok) {
        printf("  LLM error: %s\n", lr.message.c_str());
        printf("  Try .schema to see available columns, or rephrase.\n");
        return true;
    }
    return confirm_and_run(conn, lr.sql);
}

// Extract `--ai` from a question string. Returns true if present and
// strips the flag out of `q` in-place. Supports `--ai` anywhere.
static bool strip_ai_flag(std::string &q) {
    size_t p = q.find("--ai");
    if (p == std::string::npos) return false;
    // Must be a whole token (preceded by start/space, followed by end/space).
    bool left_ok = (p == 0 || q[p - 1] == ' ' || q[p - 1] == '\t');
    bool right_ok = (p + 4 == q.size() || q[p + 4] == ' ' || q[p + 4] == '\t');
    if (!left_ok || !right_ok) return false;
    // Remove the flag + the space before or after it.
    size_t end = p + 4;
    if (p > 0 && q[p - 1] == ' ') p--;
    else if (end < q.size() && q[end] == ' ') end++;
    q.erase(p, end - p);
    // Trim any new leading/trailing space.
    while (!q.empty() && (q.front() == ' ' || q.front() == '\t')) q.erase(q.begin());
    while (!q.empty() && (q.back()  == ' ' || q.back()  == '\t')) q.pop_back();
    return true;
}

static void handle_ask(slothdb_connection *conn, const std::string &arg) {
    std::string question = arg;
    bool force_ai = strip_ai_flag(question);

    // With an argument: single-shot. Translate → confirm → run → return
    // to the main SQL prompt.
    if (!question.empty()) {
        ask_once(conn, question, force_ai);
        return;
    }

    // No argument: enter interactive ask-mode. Keeps reading NL lines
    // (no ".ask" prefix required) until the user types exit / quit /
    // ":q" / ":quit", hits an empty line, or sends EOF. Each non-empty
    // line runs through the NL→SQL pipeline, shows the generated SQL,
    // and prompts [Y/n] before executing.
    //
    // Inside ask-mode, a line can still include `--ai` to force the
    // LLM path, or the user can set it globally with `:ai` / `:rules`.
    printf("ask mode — type a question in English.\n");
    printf("  Commands: `:ai` force-LLM / `:rules` force-rules / "
           "`exit` / `quit` / blank line / Ctrl+D to leave.\n");
    {
        auto cfg = slothdb::ask::ConfigFromEnvironment();
        if (cfg.provider == slothdb::ask::Provider::None) {
            printf("  (rules-only mode — set SLOTHDB_ASK_PROVIDER for AI fallback)\n");
        } else {
            const char *prov_name = "?";
            switch (cfg.provider) {
                case slothdb::ask::Provider::Ollama:    prov_name = "ollama";    break;
                case slothdb::ask::Provider::OpenAI:    prov_name = "openai";    break;
                case slothdb::ask::Provider::Anthropic: prov_name = "anthropic"; break;
                default: break;
            }
            printf("  (AI fallback: %s / %s)\n", prov_name, cfg.model.c_str());
        }
    }
    printf("\n");

    bool mode_force_ai = force_ai; // sticky across interactive lines
    while (true) {
        printf("ask> ");
        fflush(stdout);
        char buf[1024];
        if (!fgets(buf, sizeof(buf), stdin)) { printf("\n"); break; }
        std::string line(buf);
        while (!line.empty() &&
               (line.back() == '\n' || line.back() == '\r' ||
                line.back() == ' '  || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty()) break;
        if (line == "exit" || line == "quit" || line == ":q" ||
            line == ":quit" || line == ":exit") break;
        if (line == ":ai")    { mode_force_ai = true;  printf("  (AI mode on)\n"); continue; }
        if (line == ":rules") { mode_force_ai = false; printf("  (rules mode — AI is fallback only)\n"); continue; }

        bool line_force_ai = mode_force_ai;
        if (strip_ai_flag(line)) line_force_ai = true;
        if (!ask_once(conn, line, line_force_ai)) break;
        printf("\n");
    }
    printf("Back to SQL mode.\n");
}

static void handle_tables(slothdb_connection *conn, const std::string &arg) {
    std::string sql = "SELECT table_name FROM information_schema.tables";
    if (!arg.empty()) {
        std::string pattern = arg;
        // Strip surrounding quotes if the user gave them.
        if (pattern.size() >= 2 && (pattern.front() == '\'' || pattern.front() == '"')) {
            pattern = pattern.substr(1, pattern.size() - 2);
        }
        sql += " WHERE table_name LIKE '" + pattern + "'";
    }
    sql += " ORDER BY table_name";
    // Best-effort: if information_schema isn't implemented, fall through with a hint.
    slothdb_result *result = nullptr;
    auto status = slothdb_query(conn, sql.c_str(), &result);
    if (status == SLOTHDB_OK) {
        print_result(result);
        slothdb_free_result(result);
        return;
    }
    slothdb_free_result(result);
    printf("(no tables, or catalog introspection unavailable — try \n"
           " SELECT * FROM information_schema.tables)\n");
}

static void handle_schema(slothdb_connection *conn, const std::string &arg) {
    std::string sql;
    if (arg.empty()) {
        sql = "SELECT table_name, column_name, data_type "
              "FROM information_schema.columns ORDER BY table_name, ordinal_position";
    } else {
        std::string name = arg;
        if (name.size() >= 2 && (name.front() == '\'' || name.front() == '"')) {
            name = name.substr(1, name.size() - 2);
        }
        sql = "SELECT column_name, data_type FROM information_schema.columns "
              "WHERE table_name = '" + name + "' ORDER BY ordinal_position";
    }
    run_query(conn, sql);
}

// SQL keyword completion (for linenoise TAB).
#ifdef SLOTHDB_HAS_LINENOISE
static const char *kSqlKeywords[] = {
    "SELECT", "FROM", "WHERE", "GROUP BY", "ORDER BY", "HAVING", "LIMIT",
    "INSERT INTO", "UPDATE", "DELETE FROM", "CREATE TABLE", "CREATE VIEW",
    "DROP TABLE", "ALTER TABLE", "JOIN", "LEFT JOIN", "RIGHT JOIN",
    "INNER JOIN", "CROSS JOIN", "UNION", "INTERSECT", "EXCEPT",
    "WITH", "QUALIFY", "OVER", "PARTITION BY",
    "read_csv(", "read_parquet(", "read_json(", "read_xlsx(", "read_avro(",
    "sqlite_scan(",
    ".help", ".quit", ".tables", ".schema", ".version", ".open", ".clear", ".ask",
    nullptr,
};

static void slothdb_complete(const char *buf, linenoiseCompletions *lc) {
    size_t buf_len = strlen(buf);
    if (buf_len == 0) return;
    // Find the last whitespace — we complete the word after it.
    const char *word = buf;
    for (size_t i = buf_len; i-- > 0; ) {
        if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n') {
            word = buf + i + 1;
            break;
        }
    }
    size_t word_len = strlen(word);
    if (word_len == 0) return;
    for (int i = 0; kSqlKeywords[i]; i++) {
        const char *kw = kSqlKeywords[i];
        if (strncasecmp(word, kw, word_len) == 0) {
            // Rebuild: prefix (everything before the word) + keyword.
            std::string candidate(buf, word - buf);
            candidate += kw;
            linenoiseAddCompletion(lc, candidate.c_str());
        }
    }
}
#endif

// Read one logical line, falling back to getline on Windows. Returns false on
// EOF / Ctrl+D.
static bool read_line(const char *prompt, std::string &out) {
#ifdef SLOTHDB_HAS_LINENOISE
    char *raw = linenoise(prompt);
    if (!raw) return false;
    out = raw;
    if (!out.empty()) linenoiseHistoryAdd(raw);
    linenoiseFree(raw);
    return true;
#else
    printf("%s", prompt);
    fflush(stdout);
    return static_cast<bool>(std::getline(std::cin, out));
#endif
}

int main(int argc, char *argv[]) {
    const char *db_path = "";
    const char *cmd_query = nullptr;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cmd_query = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: slothdb [database_path]\n");
            printf("       slothdb -c \"SELECT ...\"\n");
            printf("Opens an in-memory database if no path is given.\n");
            return 0;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("%s\n", slothdb_version());
            return 0;
        } else {
            db_path = argv[i];
        }
    }

    if (!cmd_query) {
        printf("SlothDB Shell v%s\n", slothdb_version());
        printf("Type .help for help, .quit to exit.\n\n");
    }

    slothdb_database *db = nullptr;
    slothdb_connection *conn = nullptr;

    if (slothdb_open(db_path, &db) != SLOTHDB_OK) {
        fprintf(stderr, "Failed to open database: %s\n", db_path);
        return 1;
    }

    if (slothdb_connect(db, &conn) != SLOTHDB_OK) {
        fprintf(stderr, "Failed to create connection\n");
        slothdb_close(db);
        return 1;
    }

    // One-shot command: -c "..."
    if (cmd_query) {
        bool ok = run_query(conn, cmd_query);
        slothdb_disconnect(conn);
        slothdb_close(db);
        return ok ? 0 : 1;
    }

    if (strlen(db_path) > 0) printf("Connected to: %s\n", db_path);
    else                      printf("Connected to in-memory database\n");

#ifdef SLOTHDB_HAS_LINENOISE
    linenoiseHistorySetMaxLen(1000);
    linenoiseHistoryLoad(kHistoryFile);
    linenoiseSetCompletionCallback(slothdb_complete);
#endif

    std::string buffer;
    while (true) {
        const char *prompt = buffer.empty() ? "slothdb> " : "   ...> ";

        std::string line;
        if (!read_line(prompt, line)) break; // EOF / Ctrl+D.

        // Dot commands (only at start of a fresh buffer).
        if (buffer.empty() && !line.empty() && line[0] == '.') {
            // Split on first whitespace: command + optional argument.
            size_t sp = line.find_first_of(" \t");
            std::string cmd = (sp == std::string::npos) ? line : line.substr(0, sp);
            std::string arg = (sp == std::string::npos) ? "" : line.substr(sp + 1);
            // Trim leading whitespace from arg.
            while (!arg.empty() && (arg.front() == ' ' || arg.front() == '\t'))
                arg.erase(arg.begin());

            if (cmd == ".quit" || cmd == ".exit") break;
            if (cmd == ".help") { print_help(); continue; }
            if (cmd == ".version") { printf("%s\n", slothdb_version()); continue; }
            if (cmd == ".tables") { handle_tables(conn, arg); continue; }
            if (cmd == ".schema") { handle_schema(conn, arg); continue; }
            if (cmd == ".ask")    { handle_ask(conn, arg); continue; }
            if (cmd == ".clear") { printf("\033[2J\033[H"); fflush(stdout); continue; }
            if (cmd == ".open") {
                if (arg.empty()) { printf("Usage: .open <path>\n"); continue; }
                // Open the new database FIRST; swap in only on success.
                // Previously this closed the old db before attempting the
                // new open, so a failure left the shell with invalid
                // handles and `return 1`'d the whole process — exiting
                // the REPL instead of returning to the prompt.
                slothdb_database   *new_db   = nullptr;
                slothdb_connection *new_conn = nullptr;
                if (slothdb_open(arg.c_str(), &new_db) != SLOTHDB_OK ||
                    slothdb_connect(new_db, &new_conn) != SLOTHDB_OK) {
                    fprintf(stderr, "Failed to open: %s\n", arg.c_str());
                    // Hint if the user handed us what looks like a CSV/Parquet.
                    auto ends_with = [&](const char *suf) {
                        size_t n = strlen(suf);
                        return arg.size() >= n &&
                               arg.compare(arg.size() - n, n, suf) == 0;
                    };
                    if (ends_with(".csv") || ends_with(".tsv") ||
                        ends_with(".parquet") || ends_with(".json") ||
                        ends_with(".arrow") || ends_with(".xlsx")) {
                        fprintf(stderr,
                            "Hint: .open is for persistent .slothdb files. "
                            "To query a data file, use:\n"
                            "  SELECT * FROM '%s';\n", arg.c_str());
                    }
                    if (new_db) slothdb_close(new_db);
                    continue;  // back to the prompt, old db still active
                }
                // Success — dispose of the old handles and swap.
                slothdb_disconnect(conn);
                slothdb_close(db);
                db = new_db;
                conn = new_conn;
                printf("Connected to: %s\n", arg.c_str());
                continue;
            }
            printf("Unknown command: %s  (type .help for help)\n", line.c_str());
            continue;
        }

        buffer += line + " ";

        auto trimmed = buffer;
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
            trimmed.pop_back();
        if (trimmed.empty()) { buffer.clear(); continue; }
        if (trimmed.back() != ';') continue; // Keep reading multi-line input.

        run_query(conn, buffer);
        buffer.clear();
    }

#ifdef SLOTHDB_HAS_LINENOISE
    linenoiseHistorySave(kHistoryFile);
#endif

    printf("\nBye!\n");
    slothdb_disconnect(conn);
    slothdb_close(db);
    return 0;
}
