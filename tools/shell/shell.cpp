/*
 * SlothDB CLI Shell
 *
 * Interactive SQL REPL for SlothDB.
 * Usage: slothdb_shell [database_path]
 */

#include "slothdb/api/slothdb.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <iostream>
#include <iomanip>

static void print_result(slothdb_result *result) {
    uint64_t cols = slothdb_column_count(result);
    uint64_t rows = slothdb_row_count(result);

    if (cols == 0) {
        printf("OK\n");
        return;
    }

    // Print header.
    for (uint64_t c = 0; c < cols; c++) {
        if (c > 0) printf(" | ");
        printf("%-15s", slothdb_column_name(result, c));
    }
    printf("\n");

    // Separator.
    for (uint64_t c = 0; c < cols; c++) {
        if (c > 0) printf("-+-");
        printf("---------------");
    }
    printf("\n");

    // Rows.
    for (uint64_t r = 0; r < rows; r++) {
        for (uint64_t c = 0; c < cols; c++) {
            if (c > 0) printf(" | ");
            printf("%-15s", slothdb_value_varchar(result, r, c));
        }
        printf("\n");
    }

    printf("(%llu row%s)\n", (unsigned long long)rows, rows == 1 ? "" : "s");
}

int main(int argc, char *argv[]) {
    // Parse command-line args.
    const char *db_path = "";
    const char *cmd_query = nullptr;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cmd_query = argv[++i];
        } else {
            db_path = argv[i];
        }
    }

    // If running a one-shot command, skip the banner.
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

    // One-shot command mode: run query and exit.
    if (cmd_query) {
        slothdb_result *result = nullptr;
        slothdb_status status = slothdb_query(conn, cmd_query, &result);
        if (status != SLOTHDB_OK) {
            fprintf(stderr, "Error: %s\n", slothdb_result_error(result));
            slothdb_free_result(result);
            slothdb_disconnect(conn);
            slothdb_close(db);
            return 1;
        }
        print_result(result);
        slothdb_free_result(result);
        slothdb_disconnect(conn);
        slothdb_close(db);
        return 0;
    }

    if (strlen(db_path) > 0) {
        printf("Connected to: %s\n", db_path);
    } else {
        printf("Connected to in-memory database\n");
    }

    std::string buffer;
    while (true) {
        printf(buffer.empty() ? "slothdb> " : "   ...> ");
        fflush(stdout);

        std::string line;
        if (!std::getline(std::cin, line)) break;

        // Dot commands.
        if (buffer.empty() && !line.empty() && line[0] == '.') {
            if (line == ".quit" || line == ".exit") break;
            if (line == ".help") {
                printf(".help     Show this help\n");
                printf(".quit     Exit\n");
                printf(".tables   List all tables\n");
                printf(".schema   Show table schemas\n");
                printf(".version  Show version\n");
                continue;
            }
            if (line == ".tables") {
                slothdb_result *result = nullptr;
                // Use a hack: query catalog via SQL.
                if (slothdb_query(conn, "SELECT 'tables' AS info", &result) == SLOTHDB_OK) {
                    printf("(Use SQL: SELECT * FROM table_name)\n");
                }
                slothdb_free_result(result);
                continue;
            }
            if (line == ".version") {
                printf("%s\n", slothdb_version());
                continue;
            }
            printf("Unknown command: %s\n", line.c_str());
            continue;
        }

        buffer += line + " ";

        // Check if statement is complete (ends with semicolon).
        auto trimmed = buffer;
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
            trimmed.pop_back();

        if (trimmed.empty()) {
            buffer.clear();
            continue;
        }

        if (trimmed.back() != ';') continue; // Multi-line input.

        // Execute.
        slothdb_result *result = nullptr;
        auto status = slothdb_query(conn, buffer.c_str(), &result);

        if (status == SLOTHDB_OK) {
            print_result(result);
        } else {
            fprintf(stderr, "Error: %s\n", slothdb_result_error(result));
        }

        slothdb_free_result(result);
        buffer.clear();
    }

    printf("\nBye!\n");
    slothdb_disconnect(conn);
    slothdb_close(db);
    return 0;
}
