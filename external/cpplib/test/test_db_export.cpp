#include "db/logger.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char* DB_PATH    = "_test_export.db";
static const char* JSONL_PATH = "_test_export.jsonl";

// Delete the DB files (WAL + SHM included) so each test starts fresh.
// JSONL is kept so the user can inspect the output.
static void reset_db()
{
    std::remove(DB_PATH);
    std::remove((std::string(DB_PATH) + "-wal").c_str());
    std::remove((std::string(DB_PATH) + "-shm").c_str());
}

static std::vector<nlohmann::ordered_json> read_jsonl(const std::string& path)
{
    std::vector<nlohmann::ordered_json> rows;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        try { rows.push_back(nlohmann::ordered_json::parse(line)); }
        catch (...) {}
    }
    return rows;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static bool test_basic_export()
{
    reset_db();

    {
        EnvironmentHeader env{
            .app_name    = "test_app",
            .app_version = "0.1",
            .host_name   = "localhost",
        };
        AsyncSqliteLogger logger(DB_PATH, {}, env);

        logger.start_run("run_001", {{"desc", "basic test"}});
        logger.log_event("info",  "step 1");
        logger.log_event("debug", "step 2", {{"value", 42}});
        logger.log_event("warn",  "step 3", {{"flag", true}});
        logger.end_run();
    } // destructor flushes

    const int n = export_run_to_jsonl(DB_PATH, "run_001", JSONL_PATH);
    if (n < 0) {
        std::fputs("FAIL [basic_export] export returned error\n", stderr);
        return false;
    }

    // run_start + 3 log_events = 4 rows
    if (n != 4) {
        std::fprintf(stderr, "FAIL [basic_export] expected 4 rows, got %d\n", n);
        return false;
    }

    const auto rows = read_jsonl(JSONL_PATH);
    if (rows.size() != 4) {
        std::fprintf(stderr, "FAIL [basic_export] jsonl has %zu lines\n", rows.size());
        return false;
    }

    if (rows[0].value("type", "") != "run_start") {
        std::fputs("FAIL [basic_export] first row is not run_start\n", stderr);
        return false;
    }

    for (const auto& row : rows) {
        if (row.value("run_id", "") != "run_001") {
            std::fputs("FAIL [basic_export] wrong run_id in row\n", stderr);
            return false;
        }
    }

    // "value":42 must be merged into top-level (not buried in extra_json)
    if (!rows[2].contains("value") || rows[2]["value"] != 42) {
        std::fputs("FAIL [basic_export] extra field 'value' not merged\n", stderr);
        return false;
    }

    std::puts("PASS [basic_export]");
    return true;
}

static bool test_extra_columns()
{
    reset_db();

    {
        std::vector<ColumnDef> cols = {
            {"frame_id", "INTEGER", "frame_id"},
            {"score",    "REAL",    "score"},
        };
        AsyncSqliteLogger logger(DB_PATH, cols, {});

        logger.start_run("run_002");
        for (int i = 0; i < 5; ++i) {
            logger.log_event("info", "frame", {
                {"frame_id", i},
                {"score",    0.9 + i * 0.01},
                {"status",   "ok"}
            });
        }
        logger.end_run();
    }

    const int n = export_run_to_jsonl(DB_PATH, "run_002", JSONL_PATH);
    if (n != 6) {
        std::fprintf(stderr, "FAIL [extra_columns] expected 6 rows, got %d\n", n);
        return false;
    }

    const auto rows = read_jsonl(JSONL_PATH);
    for (int i = 1; i <= 5; ++i) {
        if (!rows[i].contains("frame_id")) {
            std::fprintf(stderr, "FAIL [extra_columns] row %d missing frame_id\n", i);
            return false;
        }
        if (!rows[i].contains("score")) {
            std::fprintf(stderr, "FAIL [extra_columns] row %d missing score\n", i);
            return false;
        }
    }

    std::puts("PASS [extra_columns]");
    return true;
}

static bool test_multiple_runs()
{
    reset_db();

    {
        AsyncSqliteLogger logger(DB_PATH, {}, {});

        logger.start_run("run_A");
        logger.log_event("info", "in run A");
        logger.log_event("info", "in run A again");
        logger.end_run();

        logger.start_run("run_B");
        logger.log_event("info", "in run B");
        logger.end_run();
    }

    const int nA = export_run_to_jsonl(DB_PATH, "run_A", JSONL_PATH);
    if (nA != 3) {
        std::fprintf(stderr, "FAIL [multiple_runs] run_A: expected 3 rows, got %d\n", nA);
        return false;
    }

    for (const auto& row : read_jsonl(JSONL_PATH)) {
        if (row.value("run_id", "") != "run_A") {
            std::fputs("FAIL [multiple_runs] run_B row leaked into run_A export\n", stderr);
            return false;
        }
    }

    const int nB = export_run_to_jsonl(DB_PATH, "run_B", JSONL_PATH);
    if (nB != 2) {
        std::fprintf(stderr, "FAIL [multiple_runs] run_B: expected 2 rows, got %d\n", nB);
        return false;
    }

    std::puts("PASS [multiple_runs]");
    return true;
}

static bool test_bulk_performance()
{
    reset_db();

    {
        std::vector<ColumnDef> cols = {
            {"frame_id", "INTEGER", "frame_id"},
            {"score",    "REAL",    "score"},
        };
        AsyncSqliteLogger logger(DB_PATH, cols, {});
        logger.start_run("run_bulk");
        for (int i = 0; i < 10000; ++i) {
            logger.log_event("info", "frame", {
                {"frame_id", i},
                {"score",    static_cast<double>(i) / 10000.0}
            });
        }
        logger.end_run();
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    const int n = export_run_to_jsonl(DB_PATH, "run_bulk", JSONL_PATH);
    auto t1 = std::chrono::high_resolution_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (n != 10001) {
        std::fprintf(stderr, "FAIL [bulk_performance] expected 10001 rows, got %d\n", n);
        return false;
    }

    std::printf("PASS [bulk_performance] exported %d rows in %.1f ms (%.0f rows/s)\n",
                n, ms, n / (ms / 1000.0));
    std::printf("     -> %s\n", JSONL_PATH);
    return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    int failed = 0;
    failed += !test_basic_export();
    failed += !test_extra_columns();
    failed += !test_multiple_runs();
    failed += !test_bulk_performance();

    // DB from last test remains for inspection alongside the JSONL.
    std::printf("\n%s  (%d test(s) failed)\n",
                failed == 0 ? "ALL PASSED" : "SOME FAILED", failed);
    return failed == 0 ? 0 : 1;
}
