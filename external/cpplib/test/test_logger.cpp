#include "db/logger.h"
#include <chrono>
#include <cstdio>

int main()
{
    EnvironmentHeader env{
        .app_name      = "my_app",
        .app_version   = "1.0.0",
        .host_name     = "dev-machine-01",
        .model_name    = "dummy-model",
        .model_version = "v0.1"
    };

    // Extra typed columns — add more ColumnDef entries here to extend the schema.
    std::vector<ColumnDef> cols = {
        {"frame_id", "INTEGER", "frame_id"},
        {"score",    "REAL",    "score"},
    };

    AsyncSqliteLogger logger("app_log.db", cols, env);

    // ---- Run 1 ----
    logger.start_run("run_2025_1118_01", {
        {"description", "sobel threshold test"},
        {"sobel_ksize", 3},
        {"sobel_th",    128}
    });

    logger.log_event("info",  "program started");
    logger.log_event("debug", "sobel start", {{"stage", "sobel"}, {"frame_id", 123}});
    logger.log_event("debug", "sobel done",  {{"stage", "sobel"}, {"frame_id", 123}, {"elapsed_ms", 3.4}});

    logger.end_run();

    // ---- Run 2 ----
    logger.start_run("run_2025_1118_02", {
        {"description", "fft test"},
        {"fft_size",    1024}
    });

    double elapsed_ms = 0.0;
    for (int i = 0; i < 1000; ++i) {
        auto t0 = std::chrono::high_resolution_clock::now();
        logger.log_event("info", "frame processed", {
            {"frame_id",   i},
            {"status",     "ok"},
            {"elapsed_ms", elapsed_ms},
            {"defect",     false},
            {"score",      0.97 * i}
        });
        auto t1 = std::chrono::high_resolution_clock::now();
        elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }

    std::puts("logger test done -> app_log.db");
}
