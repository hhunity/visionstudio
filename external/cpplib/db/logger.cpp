#include "db/logger.h"
#include <sqlite3.h>

#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

// ===========================================================================
// Impl — contains all private state and logic
// ===========================================================================
struct AsyncSqliteLogger::Impl
{
    using json = nlohmann::ordered_json;

    // -----------------------------------------------------------------------
    // Internal log-entry type
    // -----------------------------------------------------------------------
    struct LogEntry
    {
        std::string run_id;
        std::string type;
        std::string time;
        std::string level;
        std::string msg;
        std::string env_json;
        std::string extra_json;
        std::string meta_json;
        std::vector<std::string> extra_col_values; // indexed by extra_columns_
        bool is_run_record = false;
    };

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    std::vector<ColumnDef>  extra_columns_;
    json                    env_header_json_;
    std::string             current_run_id_;

    sqlite3*      db_       = nullptr;
    sqlite3_stmt* stmt_log_ = nullptr;
    sqlite3_stmt* stmt_run_ = nullptr;

    std::mutex              queue_mutex_;
    std::condition_variable cv_;
    std::deque<LogEntry>    queue_;
    bool                    stop_ = false;
    std::thread             worker_;

    // -----------------------------------------------------------------------
    // Constructor / destructor
    // -----------------------------------------------------------------------
    Impl(const std::string& db_path,
         std::vector<ColumnDef> cols,
         const EnvironmentHeader& env)
        : extra_columns_(std::move(cols))
        , env_header_json_(make_env_json(env))
    {
        open_db(db_path);
        worker_ = std::thread(&Impl::worker_loop, this);
    }

    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lk(queue_mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();

        if (stmt_log_) sqlite3_finalize(stmt_log_);
        if (stmt_run_) sqlite3_finalize(stmt_run_);
        if (db_)       sqlite3_close(db_);
    }

    // -----------------------------------------------------------------------
    // Public-method implementations (called from AsyncSqliteLogger wrappers)
    // -----------------------------------------------------------------------
    void start_run(const std::string& run_id, const json& run_meta)
    {
        {
            std::lock_guard<std::mutex> lk(queue_mutex_);
            current_run_id_ = run_id;
        }

        LogEntry e;
        e.run_id        = run_id;
        e.type          = "run_start";
        e.time          = current_time_iso8601();
        e.meta_json     = run_meta.dump();
        e.is_run_record = true;
        enqueue(std::move(e));
    }

    void end_run()
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        current_run_id_.clear();
    }

    void set_environment(const EnvironmentHeader& env_header)
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        env_header_json_ = make_env_json(env_header);
    }

    void log(json j)
    {
        std::string run_id_snap;
        json        env_snap;
        {
            std::lock_guard<std::mutex> lk(queue_mutex_);
            run_id_snap = current_run_id_;
            env_snap    = env_header_json_;
        }

        LogEntry e;
        e.run_id   = run_id_snap;
        e.env_json = env_snap.dump();

        if (j.contains("type"))  { e.type  = j["type"];  j.erase("type");  }
        if (j.contains("time"))  { e.time  = j["time"];  j.erase("time");  }
        if (j.contains("level")) { e.level = j["level"]; j.erase("level"); }
        if (j.contains("msg"))   { e.msg   = j["msg"];   j.erase("msg");   }

        // Lift extra-column fields into typed slots; the rest goes to extra_json.
        e.extra_col_values.resize(extra_columns_.size());
        for (std::size_t i = 0; i < extra_columns_.size(); ++i) {
            const auto& col = extra_columns_[i];
            if (j.contains(col.json_key)) {
                e.extra_col_values[i] = j[col.json_key].dump();
                j.erase(col.json_key);
            }
        }

        if (!j.empty()) e.extra_json = j.dump();

        enqueue(std::move(e));
    }

    void log_event(const std::string& level,
                   const std::string& msg,
                   const json& extra)
    {
        json j;
        j["type"]  = "event";
        j["time"]  = current_time_iso8601();
        j["level"] = level;
        j["msg"]   = msg;
        for (auto it = extra.begin(); it != extra.end(); ++it)
            j[it.key()] = it.value();
        log(std::move(j));
    }

    // -----------------------------------------------------------------------
    // DB setup
    // -----------------------------------------------------------------------
    void open_db(const std::string& db_path)
    {
        if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK)
            throw std::runtime_error("AsyncSqliteLogger: cannot open DB: " + db_path);

        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;",   nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "PRAGMA cache_size=-8000;",   nullptr, nullptr, nullptr);

        sqlite3_exec(db_,
            "CREATE TABLE IF NOT EXISTS runs ("
            "  run_id     TEXT PRIMARY KEY,"
            "  started_at TEXT,"
            "  meta_json  TEXT"
            ");",
            nullptr, nullptr, nullptr);

        // Build logs DDL; extra columns are appended at the end so the base
        // schema stays stable — adding a new ColumnDef only adds a new column.
        std::string ddl =
            "CREATE TABLE IF NOT EXISTS logs ("
            "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  run_id     TEXT,"
            "  type       TEXT,"
            "  time       TEXT,"
            "  level      TEXT,"
            "  msg        TEXT,"
            "  env_json   TEXT,"
            "  extra_json TEXT";
        for (const auto& col : extra_columns_)
            ddl += ", " + col.name + " " + col.type;
        ddl += ");";
        sqlite3_exec(db_, ddl.c_str(), nullptr, nullptr, nullptr);

        prepare_statements();
    }

    void prepare_statements()
    {
        std::string sql =
            "INSERT INTO logs (run_id,type,time,level,msg,env_json,extra_json";
        std::string vals = "VALUES (?,?,?,?,?,?,?";
        for (const auto& col : extra_columns_) {
            sql  += ", " + col.name;
            vals += ",?";
        }
        sql += ") " + vals + ");";
        sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_log_, nullptr);

        sqlite3_prepare_v2(db_,
            "INSERT OR REPLACE INTO runs (run_id, started_at, meta_json)"
            " VALUES (?,?,?);",
            -1, &stmt_run_, nullptr);
    }

    // -----------------------------------------------------------------------
    // Queue & worker
    // -----------------------------------------------------------------------
    void enqueue(LogEntry entry)
    {
        {
            std::lock_guard<std::mutex> lk(queue_mutex_);
            queue_.push_back(std::move(entry));
        }
        cv_.notify_one();
    }

    void worker_loop()
    {
        for (;;) {
            std::deque<LogEntry> batch;
            {
                std::unique_lock<std::mutex> lk(queue_mutex_);
                cv_.wait(lk, [&]{ return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) break;
                batch.swap(queue_); // drain the whole queue at once
            }
            sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
            for (auto& e : batch) write_entry(e);
            sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
        }
    }

    void write_entry(const LogEntry& e)
    {
        if (e.is_run_record) {
            sqlite3_bind_text(stmt_run_, 1, e.run_id.c_str(),    -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt_run_, 2, e.time.c_str(),      -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt_run_, 3, e.meta_json.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt_run_);
            sqlite3_reset(stmt_run_);
        }

        sqlite3_bind_text(stmt_log_, 1, e.run_id.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_log_, 2, e.type.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_log_, 3, e.time.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_log_, 4, e.level.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_log_, 5, e.msg.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_log_, 6, e.env_json.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_log_, 7, e.extra_json.c_str(),-1, SQLITE_TRANSIENT);

        for (std::size_t i = 0; i < extra_columns_.size(); ++i) {
            const int idx = static_cast<int>(i) + 8;
            if (i < e.extra_col_values.size() && !e.extra_col_values[i].empty())
                sqlite3_bind_text(stmt_log_, idx,
                                  e.extra_col_values[i].c_str(), -1, SQLITE_TRANSIENT);
            else
                sqlite3_bind_null(stmt_log_, idx);
        }

        sqlite3_step(stmt_log_);
        sqlite3_reset(stmt_log_);
    }

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------
    static json make_env_json(const EnvironmentHeader& env)
    {
        json j;
        j["app_name"]      = env.app_name;
        j["app_version"]   = env.app_version;
        j["host_name"]     = env.host_name;
        j["model_name"]    = env.model_name;
        j["model_version"] = env.model_version;
        return j;
    }

    static std::string current_time_iso8601()
    {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto ns  = duration_cast<nanoseconds>(now.time_since_epoch()) % 1'000'000'000LL;
        std::time_t t = system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
        char out[96];
        std::snprintf(out, sizeof(out), "%s.%09lld", buf,
                      static_cast<long long>(ns.count()));
        return std::string(out);
    }
};

// ===========================================================================
// AsyncSqliteLogger — thin public wrappers that delegate to Impl
// ===========================================================================

AsyncSqliteLogger::AsyncSqliteLogger(const std::string& db_path,
                                     std::vector<ColumnDef> extra_columns,
                                     const EnvironmentHeader& env_header)
    : impl_(std::make_unique<Impl>(db_path, std::move(extra_columns), env_header))
{}

AsyncSqliteLogger::~AsyncSqliteLogger() = default;

void AsyncSqliteLogger::start_run(const std::string& run_id, const json& run_meta)
{ impl_->start_run(run_id, run_meta); }

void AsyncSqliteLogger::end_run()
{ impl_->end_run(); }

void AsyncSqliteLogger::set_environment(const EnvironmentHeader& env_header)
{ impl_->set_environment(env_header); }

void AsyncSqliteLogger::log(json j)
{ impl_->log(std::move(j)); }

void AsyncSqliteLogger::log_event(const std::string& level,
                                   const std::string& msg,
                                   const json& extra)
{ impl_->log_event(level, msg, extra); }

// ===========================================================================
// export_run_to_jsonl
// ===========================================================================
int export_run_to_jsonl(const std::string& db_path,
                         const std::string& run_id,
                         const std::string& out_path)
{
    using json = nlohmann::ordered_json;

    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        return -1;
    }
    // WAL mode: readers can run concurrently with the writer.
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT * FROM logs WHERE run_id = ? ORDER BY id;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);

    FILE* fp = std::fopen(out_path.c_str(), "w");
    if (!fp) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }

    const int ncols = sqlite3_column_count(stmt);
    int rows = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        json row;

        for (int c = 0; c < ncols; ++c) {
            const char* name = sqlite3_column_name(stmt, c);
            if (!name) continue;

            const std::string col_name(name);

            // Skip the auto-increment id column.
            if (col_name == "id") continue;

            // extra_json: merge its fields into the top-level object.
            if (col_name == "extra_json") {
                const char* val = reinterpret_cast<const char*>(
                    sqlite3_column_text(stmt, c));
                if (val && val[0] != '\0') {
                    try {
                        auto extra = json::parse(val);
                        if (extra.is_object()) {
                            for (auto& [k, v] : extra.items())
                                row[k] = v;
                            continue;
                        }
                    } catch (...) {}
                    row[col_name] = val; // fallback: keep as-is
                }
                continue;
            }

            // All other columns: store by type.
            switch (sqlite3_column_type(stmt, c)) {
                case SQLITE_INTEGER:
                    row[col_name] = sqlite3_column_int64(stmt, c);
                    break;
                case SQLITE_FLOAT:
                    row[col_name] = sqlite3_column_double(stmt, c);
                    break;
                case SQLITE_NULL:
                    // Omit null columns from output.
                    break;
                default: {
                    const char* val = reinterpret_cast<const char*>(
                        sqlite3_column_text(stmt, c));
                    if (val) row[col_name] = val;
                    break;
                }
            }
        }

        std::fprintf(fp, "%s\n", row.dump().c_str());
        ++rows;
    }

    std::fclose(fp);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rows;
}
