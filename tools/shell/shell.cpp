/*
 * SlothDB CLI Shell
 *
 * Interactive SQL REPL for SlothDB.
 * Usage: slothdb [database_path]
 *        slothdb -c "SELECT ..."
 */

#include "slothdb/api/slothdb.h"
#include "slothdb/ask/nl_to_sql.hpp"
#include "slothdb/ask/embedded_llm.hpp"
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
    printf("  .version                    Print SlothDB version\n");
    printf("  .clear                      Clear the screen\n");
    printf("  .open <path>                Close current DB and open a new one\n");
    printf("                                :memory:        in-process, non-persistent\n");
    printf("                                file.slothdb    single-file persistent DB\n");
    printf("  .tables [PATTERN]           List tables. PATTERN is SQL LIKE ('sales%%').\n");
    printf("  .schema [TABLE]             Show CREATE TABLE statements. No arg = all tables.\n");
    printf("  .ask [question]             Natural language -> SQL. With no argument opens\n");
    printf("                              an interactive NL prompt. Rules parser handles\n");
    printf("                              common shapes (counts, top-N, group by, filters);\n");
    printf("                              builds compiled with -DSLOTHDB_ASK_MODEL=ON fall\n");
    printf("                              back to a local Qwen model (~310 MB, auto-\n");
    printf("                              downloaded on first use).\n");
    printf("                              Every generated SQL is shown and gated by [Y/n].\n");
    printf("                              Docs: docs/ASK.md in the repo.\n");
    printf("\n");
    printf("  Query any file directly (no import step needed)\n");
    printf("  ------------------------------------------------\n");
    printf("  SELECT * FROM 'data.csv';\n");
    printf("  SELECT * FROM 'logs.json';\n");
    printf("  SELECT * FROM read_parquet('events.parquet');\n");
    printf("  SELECT * FROM read_xlsx('report.xlsx');\n");
    printf("  SELECT * FROM read_avro('events.avro');\n");
    printf("  SELECT * FROM read_arrow('stream.arrow');\n");
    printf("  SELECT * FROM sqlite_scan('app.db', 'users');\n");
    printf("  SELECT * FROM 'https://host/data.csv';    -- HTTPS and s3:// work too\n");
    printf("\n");
    printf("  Materialize / transform with CTAS\n");
    printf("  ---------------------------------\n");
    printf("  CREATE TABLE sales AS SELECT * FROM 'sales.csv';\n");
    printf("  CREATE OR REPLACE TABLE top AS\n");
    printf("    SELECT region, SUM(revenue) AS rev FROM sales GROUP BY region;\n");
    printf("  CREATE TABLE IF NOT EXISTS archive AS SELECT * FROM sales WHERE year < 2024;\n");
    printf("\n");
    printf("  Virtual and live views\n");
    printf("  ----------------------\n");
    printf("  CREATE VIEW v AS SELECT ... ;            -- re-executes on every SELECT\n");
    printf("  CREATE LIVE VIEW lv AS SELECT * FROM 'app.log';\n");
    printf("      -- cached; invalidates when the source file's mtime changes.\n");
    printf("\n");
    printf("  Export with COPY\n");
    printf("  ----------------\n");
    printf("  COPY sales TO 'out.csv';\n");
    printf("  COPY (SELECT * FROM sales WHERE region='EU') TO 'eu.parquet' (FORMAT PARQUET);\n");
    printf("\n");
    printf("  Catalog introspection\n");
    printf("  ---------------------\n");
    printf("  PRAGMA table_info('sales');           -- column name/type/notnull/pk\n");
    printf("  PRAGMA database_list;                 -- attached DBs (currently just one)\n");
    printf("  DESCRIBE SELECT ... ;                 -- result schema of a query\n");
    printf("  EXPLAIN SELECT ... ;                  -- logical plan\n");
    printf("\n");
    printf("  Input handling\n");
    printf("  --------------\n");
    printf("  Statements run when they end with a semicolon.\n");
    printf("  Blank line cancels a partial statement.\n");
#ifdef SLOTHDB_HAS_LINENOISE
    printf("  Arrow keys move the cursor; Up/Down browses history.\n");
    printf("  History persists in .slothdb_history in the current directory.\n");
    printf("  Ctrl+C cancels input. Ctrl+D at empty prompt exits.\n");
#else
    printf("  On Windows, use cmd.exe's native line editing. Ctrl+C exits.\n");
#endif
    printf("\n");
    printf("  Docs: https://github.com/SouravRoy-ETL/slothdb\n");
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

// Forward decls - definitions live below (shell dispatchers used by the
// catalog-intent shortcut above the .ask pipeline).
static void handle_tables(slothdb_connection *conn, const std::string &arg);
static void handle_schema(slothdb_connection *conn, const std::string &arg);
static std::string strip_invisible(const std::string &s);

// Lowercase copy for intent matching.
static std::string to_lower(const std::string &s) {
    std::string out; out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Catalog-intent classifier. Returns true if the question is purely
// catalog introspection (show tables, describe X) and routes it to the
// dot-command handlers, which use the C API rather than SQL -
// information_schema isn't a real schema in SlothDB's parser.
static bool handle_catalog_intent(slothdb_connection *conn,
                                   const std::string &question) {
    std::string q = to_lower(question);
    // Strip trailing punctuation / whitespace.
    while (!q.empty() && (q.back() == ' ' || q.back() == '\t' ||
                          q.back() == '?' || q.back() == '.' ||
                          q.back() == '!')) q.pop_back();
    // "show tables" / "list tables" / "what tables" / "which tables" /
    // "all tables" / "tables".
    auto is_tables = [](const std::string &s) {
        return s == "tables" || s == "show tables" || s == "list tables" ||
               s == "what tables" || s == "which tables" ||
               s == "all tables" || s == "display tables" ||
               s == "show all tables" || s == "list all tables" ||
               s == "what tables are there" || s == "what tables exist" ||
               s == "show schemas" || s == "list schemas";
    };
    if (is_tables(q)) {
        handle_tables(conn, "");
        return true;
    }
    // "describe <table>" / "schema of <table>" / "columns in <table>" /
    // "columns of <table>" / "<table> schema" / "<table> columns".
    const char *describe_prefixes[] = {
        "describe ", "desc ", "schema of ", "schema for ",
        "columns in ", "columns of ", "show schema of ",
        "show schema for ", "show columns in ", "show columns of ", nullptr
    };
    for (int i = 0; describe_prefixes[i]; i++) {
        const std::string p = describe_prefixes[i];
        if (q.size() > p.size() && q.compare(0, p.size(), p) == 0) {
            handle_schema(conn, q.substr(p.size()));
            return true;
        }
    }
    return false;
}

// Run one NL question. Rules-first for the common shapes - instant,
// deterministic, no model load. When rules refuse, hand off to the
// embedded GGUF (if compiled in); it loads lazily on first miss and
// handles open-ended NL. Shows the generated SQL with [Y/n] before running.
static bool ask_once(slothdb_connection *conn, const std::string &question) {
    // Catalog-introspection questions don't go through the SQL pipeline -
    // SlothDB's parser doesn't expose information_schema, so we route
    // these to the dot-command handlers directly.
    if (handle_catalog_intent(conn, question)) return true;

    auto schema = build_ask_schema(conn);

    // File-source intents ("load sales.csv", "query events.parquet") work
    // on an empty catalog. Only fall back to the empty-schema message
    // when Translate also refuses.
    auto r = slothdb::ask::Translate(question, schema);
    if (r.status == slothdb::ask::Status::OK) {
        return confirm_and_run(conn, r.sql);
    }
    if (schema.tables.empty()) {
        printf("No tables loaded yet. Try: `.ask load sales.csv` or "
               "`.ask query events.parquet`, or run a CREATE/SELECT in "
               "SQL first.\n");
        return true;
    }

    if (slothdb::ask::EmbeddedAvailable()) {
        printf("-- %s (rules did not match - asking the local model)\n",
               slothdb::ask::DefaultModel().name);
        fflush(stdout);
        auto er = slothdb::ask::GenerateSQLLocal(schema, question);
        if (er.ok && !er.sql.empty()) {
            return confirm_and_run(conn, er.sql);
        }
        printf("  Model couldn't answer: %s\n", er.message.c_str());
        return true;
    }

    // Rules refused and the model isn't compiled in - honest error.
    printf("  Rules didn't match: %s\n", r.message.c_str());
    if (!r.unresolved.empty()) {
        printf("  Unresolved token: '%s'\n", r.unresolved.c_str());
    }
    printf("  Build with -DSLOTHDB_ASK_MODEL=ON to enable the local "
           "Qwen model (handles arbitrary NL).\n");
    return true;
}

static void handle_ask(slothdb_connection *conn, const std::string &arg) {
    // With an argument: single-shot. Pipeline -> confirm -> run -> return.
    if (!arg.empty()) {
        ask_once(conn, arg);
        return;
    }

    // No argument: interactive NL shell. Each non-empty line runs through
    // the NL pipeline. Blank line / `exit` / Ctrl+D exits back to SQL.
    printf("ask mode: natural language -> SQL, inside the shell.\n");
    if (slothdb::ask::EmbeddedAvailable()) {
        printf("  Model: %s (loads on first query).\n",
               slothdb::ask::DefaultModel().name);
    } else {
        printf("  Rules parser only (built without -DSLOTHDB_ASK_MODEL=ON).\n");
    }
    printf("  Docs: https://github.com/SouravRoy-ETL/slothdb/blob/main/docs/ASK.md\n");
    printf("  `exit` / blank line / Ctrl+D to leave.\n\n");

    while (true) {
        printf("ask> ");
        fflush(stdout);
        char buf[1024];
        if (!fgets(buf, sizeof(buf), stdin)) { printf("\n"); break; }
        std::string line = strip_invisible(buf);
        while (!line.empty() &&
               (line.back() == '\n' || line.back() == '\r' ||
                line.back() == ' '  || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty()) break;
        if (line == "exit" || line == "quit" || line == ":q" ||
            line == ":quit" || line == ":exit") break;
        if (!ask_once(conn, line)) break;
        printf("\n");
    }
    printf("Back to SQL mode.\n");
}

// Simple wildcard match: '%' matches any run of chars. Used for the
// optional LIKE pattern on .tables.
static bool like_match(const std::string &name, const std::string &pattern) {
    // Iterative DP-free matcher for '%' only (no '_' / escapes needed here).
    size_t ni = 0, pi = 0, star_n = std::string::npos, star_p = 0;
    while (ni < name.size()) {
        if (pi < pattern.size() && pattern[pi] == '%') {
            star_p = ++pi;
            star_n = ni;
        } else if (pi < pattern.size() && pattern[pi] == name[ni]) {
            pi++; ni++;
        } else if (star_n != std::string::npos) {
            pi = star_p;
            ni = ++star_n;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '%') pi++;
    return pi == pattern.size();
}

// Enumerate tables via the catalog C API - information_schema isn't a real
// schema in SlothDB's parser, so SQL-based introspection doesn't work.
static void handle_tables(slothdb_connection *conn, const std::string &arg) {
    std::string pattern = arg;
    if (pattern.size() >= 2 && (pattern.front() == '\'' || pattern.front() == '"')) {
        pattern = pattern.substr(1, pattern.size() - 2);
    }
    uint64_t n = slothdb_table_count(conn);
    if (n == 0) {
        printf("(no tables)\n");
        return;
    }
    printf("%-32s\n", "table_name");
    printf("--------------------------------\n");
    uint64_t shown = 0;
    for (uint64_t i = 0; i < n; i++) {
        const char *name = slothdb_table_name(conn, i);
        std::string sname(name ? name : "");
        if (!pattern.empty() && !like_match(sname, pattern)) continue;
        printf("%-32s\n", sname.c_str());
        shown++;
    }
    printf("(%llu table%s)\n", (unsigned long long)shown, shown == 1 ? "" : "s");
}

// Show columns for a specific table (or every table when arg is empty)
// via the catalog C API.
static void handle_schema(slothdb_connection *conn, const std::string &arg) {
    std::string name = arg;
    if (name.size() >= 2 && (name.front() == '\'' || name.front() == '"')) {
        name = name.substr(1, name.size() - 2);
    }
    uint64_t n = slothdb_table_count(conn);
    if (n == 0) { printf("(no tables)\n"); return; }

    auto print_table = [&](uint64_t ti) {
        const char *tname = slothdb_table_name(conn, ti);
        if (!tname) return;
        printf("%s (\n", tname);
        uint64_t cn = slothdb_table_column_count(conn, ti);
        for (uint64_t j = 0; j < cn; j++) {
            const char *cname = slothdb_table_column_name(conn, ti, j);
            const char *ctype = slothdb_table_column_type(conn, ti, j);
            printf("  %-24s %s%s\n",
                   cname ? cname : "",
                   ctype ? ctype : "",
                   (j + 1 == cn) ? "" : ",");
        }
        printf(");\n");
    };

    if (name.empty()) {
        for (uint64_t i = 0; i < n; i++) { print_table(i); if (i + 1 < n) printf("\n"); }
        return;
    }
    for (uint64_t i = 0; i < n; i++) {
        const char *tname = slothdb_table_name(conn, i);
        if (tname && name == tname) { print_table(i); return; }
    }
    printf("(no table named '%s')\n", name.c_str());
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
    // Find the last whitespace - we complete the word after it.
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

// Strip invisible UTF-8 characters that sneak into pasted input and break
// parsing silently:
//   U+200B..U+200F  ZWSP / ZWNJ / ZWJ / LRM / RLM
//   U+202A..U+202E  LRE / RLE / PDF / LRO / RLO
//   U+2066..U+2069  LRI / RLI / FSI / PDI
//   U+FEFF          BOM / zero-width no-break space
//   U+00AD          soft hyphen
// Windows File Explorer's "Copy as path" injects U+202A before the drive
// letter; the user sees `'C:\...'` but the shell sees `'<LRE>C:\...'` and
// the CSV reader reports a confusing "file not found." Cheap to skip before
// parsing, zero cost when absent.
static std::string strip_invisible(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    const auto n = s.size();
    for (size_t i = 0; i < n; ) {
        unsigned char b0 = static_cast<unsigned char>(s[i]);
        if (b0 == 0xE2 && i + 2 < n) {
            unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
            if (b1 == 0x80 && ((b2 >= 0x8B && b2 <= 0x8F) ||
                               (b2 >= 0xAA && b2 <= 0xAE))) {
                i += 3; continue;
            }
            if (b1 == 0x81 && b2 >= 0xA6 && b2 <= 0xA9) {
                i += 3; continue;
            }
        }
        if (b0 == 0xEF && i + 2 < n &&
            static_cast<unsigned char>(s[i + 1]) == 0xBB &&
            static_cast<unsigned char>(s[i + 2]) == 0xBF) {
            i += 3; continue;
        }
        if (b0 == 0xC2 && i + 1 < n &&
            static_cast<unsigned char>(s[i + 1]) == 0xAD) {
            i += 2; continue;
        }
        out.push_back(s[i]);
        i++;
    }
    return out;
}

// Read one logical line, falling back to getline on Windows. Returns false on
// EOF / Ctrl+D. Strips invisible BIDI / ZWSP / BOM marks from the result so
// pasted file paths with hidden control characters parse the same as the
// visible text the user sees.
static bool read_line(const char *prompt, std::string &out) {
#ifdef SLOTHDB_HAS_LINENOISE
    char *raw = linenoise(prompt);
    if (!raw) return false;
    out = strip_invisible(raw);
    if (!out.empty()) linenoiseHistoryAdd(out.c_str());
    linenoiseFree(raw);
    return true;
#else
    printf("%s", prompt);
    fflush(stdout);
    if (!std::getline(std::cin, out)) return false;
    out = strip_invisible(out);
    return true;
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
                // handles and `return 1`'d the whole process - exiting
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
                // Success - dispose of the old handles and swap.
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
