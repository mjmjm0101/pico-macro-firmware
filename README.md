
# Pico Macro Firmware (auto tag & release)

## 仕組み

- あなたは **main ブランチに push するだけ**。
- GitHub Actions が自動で:
  1. 既存のタグ `vX.Y.Z` を見て
  2. パッチ番号を +1 した新しいタグ（例: `v0.0.1`, `v0.0.2`, ...）を作成
  3. Pico W 向けにファームウェアをビルド
  4. `pico-macro-firmware.uf2` という固定ファイル名にリネーム
  5. そのタグ名で GitHub Release を作成し、`pico-macro-firmware.uf2` を添付

## 常に最新版を取得できる URL

リポジトリを `<OWNER>/<REPO>` とすると:

```
https://github.com/<OWNER>/<REPO>/releases/latest/download/pico-macro-firmware.uf2
```

この URL は、最新の正式リリースに添付された `pico-macro-firmware.uf2` を常に返します。

## ディレクトリ構成

- `firmware/pico_switch_pad/pico_switch_pad.ino`
  - あなたの Pico W 向けファームウェア本体
- `libraries/switch_tinyusb/`
  - CI 用の stub ライブラリ。実機で使うときは、
    あなたの `switch_tinyusb` リポジトリの中身で置き換えてください。
- `.github/workflows/build-auto-release.yml`
  - main push 時に自動でビルド & タグ & リリースする GitHub Actions 定義
