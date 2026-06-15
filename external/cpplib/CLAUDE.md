# CLAUDE.md

## プロジェクト概要
様々なCppの機能を提供するマルチプラットフォームのC++ライブラリ群。dllの形で提供する。
テスト用は実行ファイル形式の.exeファイルはtestフォルダに生成する。

## 環境
- C++ 20
- cmake
  - CMakePresets.jsonを使用
- vcpkgによるパッケージ管理
  - vcpkg.jsonでvcpkgのインストール管理
- windows,mac,linuxでビルド可能なこと。

## プロジェクト構成
- `external`
  - `vcpkg` - vcpkgフォルダ
  - 他にvcpkgでインストールできないものはここに格納
- `build` cmakeの結果はここに
- `out` ビルドの成果物はここにまとめて、Cmakeでコピー
  - `debug` デバックビルドの成果物
  - `release`　リリースビルドの成果物
- `db`    - SQLiteを使用したlib
- `io`    - TIFFの高速読み書き。libTIFFを使用
- `test`  - 各ライブラリのテスト環境。dllごとに実行ファイルができる。

## コーディング規約
- 英語で書く。コード内での日本語は使用禁止
- 変数名・関数名は英語（スネークケース）

## よく使うコマンド
cmake --preset debug
cmake --preset release
cmake --build --preset debug
cmake --build --preset release

## タスクの優先度
- dbフォルダに、logger.cppがあり、jsol形式で、ログを残す機能が提供されている。
これを、jsol出なくてSQLiteを出力先としたプログラムに修正してほしい
- ioフォルダに、tiff_ioがあるが、これを高速書き込みにも対応してほしい
