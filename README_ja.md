# WZH 製品用 XiaoZhi ファームウェア

[中文](README_zh.md) · [English](README.md)

[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) をベースに、ESP32-S3 搭載の WZH 製品 3 機種を管理しています。タッチ操作、オフライン音声コマンド、Wi-Fi 設定、オンライン会話に対応します。恐竜モデルは ML307 による 4G 接続にも対応します。

| 製品 | ボードディレクトリ | ビルド名 |
| --- | --- | --- |
| [パンダ](main/boards/wzh_crawlPanda/README.md) | `wzh_crawlPanda` | `wzh-crawlpanda` |
| [恐竜](main/boards/wzh_dinosaur/README.md) | `wzh_dinosaur` | `wzh-dinosaur-270-servo` |
| [レッサーパンダ](main/boards/wzh_tailRedPanda/README.md) | `wzh_tailRedPanda` | `wzh-tailredpanda` |

ESP-IDF v6.0.2 を推奨します。SDK の `export.sh` を読み込み、`python3 scripts/build.py --list-boards` でビルド設定を確認してください。ビルドには `python3 scripts/build.py <board-directory> --name <build-name>`、ホストテストには `python3 scripts/run_host_tests.py` を使用します。

- [WZH 製品ドキュメント（中国語）](docs/wzh/README.md)
- [XiaoZhi 開発ドキュメント](docs/README.md)
- [ライセンス](LICENSE)
