# CLAUDE.md

## プロジェクト概要
C++ImGuiライブラリで、巨大画像の高速画像ビューワーを提供する。
画像ビューワーは、拡大縮小、座標表示、グリッドのオーバレイを持ったTIFF画像のビューワと、
2画像比較ビュワー（左右同期）を提供
また、画像ビューワ以外に、Tiffの読み書きなどのIO周りも、他のプロジェクトで使用できるようにする。

## 環境
- C++ 20
- ImGuiライブラリを使用
- cmake
  - CMakePresets.jsonを使用
- vcpkgによるパッケージ管理
  - vcpkg.jsonでvcpkgのインストール管理
  
## プロジェクト構成
- `external`
  - `vcpkg` - vcpkgフォルダ
  - 他にvcpkgでインストールできないものはここに格納
- `build` cmakeの結果はここに
- `out` ビルドの成果物はここにまとめて、実行できるようにする
  - `debug` デバックビルドの成果物
  - `release`　リリースビルドの成果物
- `gui` - Ui関係のものはこちら
- `io` -TIFF, 読み書き
- `util` -ユーティリテの置き場  
- `api` -main.cppの置き場。アプリケーションの入り口
- `capture` - キャプチャモードの HTTP クライアント（capture_config, capture_client）


## アプリケーションモード設計

起動時に `--mode` で動作モードを固定する。実行中のモード切り替えは行わない。
コードの複雑化（フラグの絡み合い）を避けるため、この方針を維持すること。
また、モード指定がない場合は、起動時にUIで選択できるようにする

| モード | 引数例 | 概要 |
|---|---|---|
| single | `--mode single image.tiff` | 1画像ビューア |
| compare | `--mode compare -l left.tiff -r right.tiff` | 2画像比較（Diffも可） |
| split | `--mode split image.tiff` | 1画像を左右分割比較（Diffも可） |
| capture | `--mode capture` | ラインカメラ取得モード |

将来モードを追加する場合は `app_mode` enum に追加し、drop_callback・poll・View メニューの switch/if をそれぞれ拡張する。

## キャプチャモード通信仕様

### 概要
ラインカメラの制御は別プロセスのコンソールアプリが担当する。
VisionStudio は HTTP クライアントとしてコンソールアプリの HTTP サーバと通信する。

```
VisionStudio (GUI)
    ↕ HTTP (cpp-httplib)
コンソールアプリ (カメラ制御サーバ)
    ↕ REST API
ラインカメラ
```

### エンドポイント

| 操作 | メソッド | パス（デフォルト） |
|---|---|---|
| サーバ接続 | POST | `/connect` |
| 撮影開始 | POST | `/start` |
| 撮影停止 | POST | `/stop` |
| 完了通知 | GET (SSE) | `/events` |
| サーバ切断 | POST | `/disconnect` |


### SSE イベント形式
完了通知は Server-Sent Events (SSE) で受け取る。
`data:` フィールドに JSON を含み、`path` キーで画像ファイルパスを返す。

```
data: {"path": "/path/to/captured_image.tiff"}
```

受信後、VisionStudio は `async_loader` 経由で TIFF を自動ロードする。

### 設定ファイル
プロジェクトルートの `visionstudio.json` で一元管理する。
ImGui のウィンドウレイアウト（従来の `imgui.ini`）も同じファイルに保存される。

```json
{
  "capture": {
    "host": "localhost",
    "port": 8080,
    "connect_path": "/connect",
    "start_path": "/start",
    "stop_path": "/stop",
    "disconnect_path": "/disconnect",
    "sse_path": "/events",
    "timeout_ms": 5000
  }
}
```

- `imgui.ini` の自動生成は無効化済み（`io.IniFilename = nullptr`）
- 起動時に `imgui_ini` を読み込み、終了時に上書き保存する

### 実装ファイル
- `capture/capture_config.h/.cpp` - INI 読み込み・設定構造体
- `capture/capture_client.h/.cpp` - HTTP クライアント・SSE スレッド管理
- `capture.ini` - デフォルト設定

### 注意事項
- SSE リスナーは別スレッドで動作する。完了パスは `mutex` 保護のキューで main スレッドに渡す
- SSE 接続が切れた場合の自動再接続は未実装（初版）。再接続が必要な場合は `capture_client::sse_thread_func()` にループを追加する
- start/stop の POST には `timeout_ms` を適用。SSE の read timeout は無制限（ストリーミングのため）

## コーディング規約
- 英語で書く。コード内での日本語は使用禁止
- 変数名・関数名は英語（スネークケース）

## よく使うコマンド


## タスクの優先度

