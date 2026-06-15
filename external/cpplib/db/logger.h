#pragma once
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// ---------------------------------------------------------------------------
// DLL export macro
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#  ifdef DB_BUILD_DLL
#    define DB_API __declspec(dllexport)
#  else
#    define DB_API __declspec(dllimport)
#  endif
#else
#  define DB_API
#endif

// ---------------------------------------------------------------------------
// Environment header — written to every log row
// ---------------------------------------------------------------------------
struct DB_API EnvironmentHeader
{
    std::string app_name;
    std::string app_version;
    std::string host_name;
    std::string model_name;
    std::string model_version;
};

// ---------------------------------------------------------------------------
// Extra column definition
// ---------------------------------------------------------------------------
// Pass a list at construction to add typed columns to the logs table.
// json_key: which key to extract from the log json into its own column.
// Any remaining json fields are stored in extra_json.
//
// Example:
//   ColumnDef{"frame_id", "INTEGER", "frame_id"}
//   ColumnDef{"score",    "REAL",    "score"}
// ---------------------------------------------------------------------------
struct DB_API ColumnDef
{
    std::string name;     // column name in SQLite
    std::string type;     // SQLite type: "INTEGER" / "REAL" / "TEXT"
    std::string json_key; // key to extract from the log json
};

// ---------------------------------------------------------------------------
// AsyncSqliteLogger
// ---------------------------------------------------------------------------
// Thread-safe, asynchronous logger that writes structured log records to a
// SQLite database.  A background worker thread drains the queue in batches
// inside a single transaction, which keeps write throughput high even at
// thousands of entries per second.
//
// Schema (always present):
//   runs(run_id TEXT PK, started_at TEXT, meta_json TEXT)
//   logs(id INTEGER PK, run_id TEXT, type TEXT, time TEXT,
//        level TEXT, msg TEXT, env_json TEXT, extra_json TEXT
//        [, <extra columns from ColumnDef>])
// ---------------------------------------------------------------------------
class DB_API AsyncSqliteLogger
{
public:
    using json = nlohmann::ordered_json;

    // db_path       : path to the SQLite file (created if absent)
    // extra_columns : additional typed columns appended to the logs table
    // env_header    : environment info stored in every log row
    AsyncSqliteLogger(const std::string& db_path,
                      std::vector<ColumnDef> extra_columns = {},
                      const EnvironmentHeader& env_header = {});

    ~AsyncSqliteLogger();

    AsyncSqliteLogger(const AsyncSqliteLogger&)            = delete;
    AsyncSqliteLogger& operator=(const AsyncSqliteLogger&) = delete;

    // Begin a new run (writes a run_start row and records run metadata).
    void start_run(const std::string& run_id,
                   const json& run_meta = json::object());

    // Clear the current run_id (subsequent log rows will have an empty run_id).
    void end_run();

    // Replace the environment header for all subsequent log rows.
    void set_environment(const EnvironmentHeader& env_header);

    // Log an arbitrary json object.
    // Fields "type", "time", "level", "msg" are lifted to dedicated columns.
    // Fields matching a ColumnDef are lifted to their typed column.
    // Everything else ends up in extra_json.
    void log(json j);

    // Convenience: build a standard event entry and call log().
    void log_event(const std::string& level,
                   const std::string& msg,
                   const json& extra = json::object());

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Export utility
// ---------------------------------------------------------------------------

// Read all log rows for run_id from db_path and write them as JSONL
// (one JSON object per line) to out_path.
// extra_json fields are merged into the top-level object.
// Returns the number of rows written, or -1 on error.
DB_API int export_run_to_jsonl(const std::string& db_path,
                                const std::string& run_id,
                                const std::string& out_path);
