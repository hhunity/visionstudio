# cpplib

マルチプラットフォーム（Windows / macOS / Linux）の C++20 ライブラリ群。DLL として提供。

---

## ライブラリ一覧

| ライブラリ | 概要 |
|-----------|------|
| `db` | SQLite を使った非同期ロガー |
| `io` | TIFF の高速並列読み書き |

---

## ビルド

### 前提

- CMake 3.25 以上
- Ninja
- C++20 対応コンパイラ

### vcpkg のセットアップ（初回のみ）

```sh
git clone https://github.com/microsoft/vcpkg.git external/vcpkg

# macOS / Linux
./external/vcpkg/bootstrap-vcpkg.sh

# Windows
.\external\vcpkg\bootstrap-vcpkg.bat
```

### ビルドコマンド

```sh
# configure（初回 / CMakeLists.txt 変更時）
cmake --preset debug
cmake --preset release

# ビルド
cmake --build --preset debug
cmake --build --preset release
```

成果物は `out/debug/` または `out/release/` に出力されます。

---

## db — 非同期 SQLite ロガー

### ヘッダ

```cpp
#include "db/logger.h"
```

### 基本的な使い方

```cpp
// 環境情報（全ログに付与される）
EnvironmentHeader env{
    .app_name      = "my_app",
    .app_version   = "1.0.0",
    .host_name     = "server-01",
    .model_name    = "model-v1",
    .model_version = "1.0",
};

// ロガー生成（DBファイルが存在しない場合は自動作成）
AsyncSqliteLogger logger("app_log.db", {}, env);

// Run の開始（実験条件などをメタとして記録）
logger.start_run("run_2025_01", {
    {"description", "threshold test"},
    {"threshold",   128}
});

// ログ書き込み
logger.log_event("info",  "processing started");
logger.log_event("debug", "frame done", {{"frame_id", 42}, {"elapsed_ms", 3.4}});
logger.log_event("warn",  "low score",  {{"score", 0.3}});

// Run の終了
logger.end_run();

// デストラクタでキューをフラッシュして DB クローズ
```

### スキーマの拡張（型付きカラムの追加）

よく使うフィールドを独立したカラムとして定義できます。
定義したフィールドは専用カラムに格納され、SQL でそのまま集計できます。

```cpp
std::vector<ColumnDef> cols = {
    {"frame_id", "INTEGER", "frame_id"},  // name / SQLite型 / jsonキー
    {"score",    "REAL",    "score"},
};

AsyncSqliteLogger logger("app_log.db", cols, env);
```

生成されるテーブル：

```sql
CREATE TABLE logs (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id     TEXT,
    type       TEXT,
    time       TEXT,
    level      TEXT,
    msg        TEXT,
    env_json   TEXT,
    extra_json TEXT,   -- 定義外のフィールドはここに格納
    frame_id   INTEGER,
    score      REAL
);

CREATE TABLE runs (
    run_id     TEXT PRIMARY KEY,
    started_at TEXT,
    meta_json  TEXT
);
```

### JSONL へのエクスポート

```cpp
// run_id を指定して JSONL ファイルに書き出す
// ロガーが動いている最中でも呼べる（WAL モード）
int rows = export_run_to_jsonl("app_log.db", "run_2025_01", "run_2025_01.jsonl");
// 戻り値: 書き出した行数 / -1 はエラー
```

出力例（1行1JSON）：

```json
{"run_id":"run_2025_01","type":"event","time":"2025-01-18T12:34:56.000000000","level":"debug","msg":"frame done","frame_id":42,"elapsed_ms":3.4}
```

### SQL での集計例

```sql
-- あるRunのフレーム平均スコア
SELECT AVG(score) FROM logs WHERE run_id = 'run_2025_01';

-- Run一覧とメタ情報
SELECT run_id, started_at, meta_json FROM runs;
```

---

## io — TIFF 高速読み書き

### ヘッダ

```cpp
#include "io/tiff_io.h"
#include "util/image_data.h"
```

### 読み込み

```cpp
image_data img;
bool ok = tiff_io::read("input.tiff", img);

// img.width  : 幅（ピクセル）
// img.height : 高さ（ピクセル）
// img.pixels : RGBA 8bit, row-major, w * h * 4 バイト
```

進捗コールバック付き：

```cpp
std::atomic<float> progress{0.0f};
tiff_io::read("input.tiff", img, &progress);
// progress は 0.0 → 1.0 に更新される
```

スレッド数の指定：

```cpp
// デフォルトは CPU コア数フル活用
tiff_io::read("input.tiff", img, nullptr, {.max_threads = 4});
```

### 書き込み

```cpp
image_data img;
// img.width / img.height / img.pixels を設定...

bool ok = tiff_io::write("output.tiff", img);
```

オプション指定：

```cpp
// 速度優先
tiff_io::write("output.tiff", img, {.tile_size = 512, .compression_level = 1});

// バランス（デフォルト）
tiff_io::write("output.tiff", img, {.tile_size = 512, .compression_level = 6});
```

### WriteOptions / ReadOptions

```cpp
struct WriteOptions {
    uint32_t tile_size         = 512; // タイルサイズ（2の累乗推奨）
    int      compression_level = 6;   // zlib: 1=最速, 9=最高圧縮, 0=無圧縮
};

struct ReadOptions {
    int max_threads = 0; // 0=CPUコア数フル活用, 正数=上限指定
};
```

### 速度の目安（4096×4096 / 64MB / Apple M2）

| 方式 | 書き込み | 読み込み |
|------|---------|---------|
| sequential（無圧縮） | ~450 ms | ~380 ms |
| parallel tile=512 level=1 | ~200 ms | ~120 ms |
| parallel tile=512 level=6 | ~900 ms | ~95 ms |

---

## 他プロジェクトから使う

### 方法1: git submodule（推奨）

使う側のプロジェクトで：

```sh
git submodule add <cpplib の git URL> external/cpplib
git submodule update --init
```

CMakeLists.txt に追加：

```cmake
add_subdirectory(external/cpplib)

target_link_libraries(your_app PRIVATE db io)
target_include_directories(your_app PRIVATE external/cpplib)
```

### 方法2: CMake FetchContent（clone 不要）

CMakeLists.txt に追加：

```cmake
include(FetchContent)

FetchContent_Declare(cpplib
    GIT_REPOSITORY <cpplib の git URL>
    GIT_TAG        main
)
FetchContent_MakeAvailable(cpplib)

target_link_libraries(your_app PRIVATE db io)
```

configure 時に自動で clone されます。

### 方法の比較

| | submodule | FetchContent |
|--|-----------|--------------|
| バージョン管理 | コミット単位で固定 | TAG で固定可 |
| 初回 clone | 手動 | 自動 |
| オフライン作業 | ✅ | ❌（初回はネット必要） |

### vcpkg の設定（使う側で必要）

使う側のプロジェクトの `vcpkg.json` に以下を追加してください：

```json
{
  "dependencies": [
    "nlohmann-json",
    "sqlite3",
    "tiff",
    "zlib"
  ]
}
```

### ビルド済みファイルをコピーして使う場合

ビルド後、`out/release/` に必要なファイルがすべてまとまります。

```
out/release/
  libdb.dylib / db.dll / libdb.so
  libio.dylib / io.dll / libio.so
  include/
    db/logger.h
    io/tiff_io.h
    util/image_data.h
```

`out/release/` フォルダごとコピーして使う側の CMakeLists.txt に追加：

```cmake
set(CPPLIB_DIR "/path/to/cpplib/out/release")

find_library(DB_LIB db PATHS ${CPPLIB_DIR} NO_DEFAULT_PATH)
find_library(IO_LIB io PATHS ${CPPLIB_DIR} NO_DEFAULT_PATH)

target_link_libraries(your_app PRIVATE ${DB_LIB} ${IO_LIB})
target_include_directories(your_app PRIVATE "${CPPLIB_DIR}/include")
```

---

## テスト

```sh
cmake --build --preset debug

./out/debug/test_logger     # db ロガーの動作確認
./out/debug/test_tiff_io    # tiff 読み書き正常系テスト
./out/debug/test_db_export  # JSONL エクスポートテスト
./out/debug/test_compare    # sequential vs parallel 速度比較
```
