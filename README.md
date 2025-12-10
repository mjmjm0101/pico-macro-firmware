# Pico Macro Firmware

## 仕組み

- **main ブランチに push するだけ**。

## 常に最新版を取得できる URL

リポジトリを `<OWNER>/<REPO>` とすると:

```
https://github.com/<OWNER>/<REPO>/releases/latest/download/pico-macro-firmware.uf2
```

この URL は、最新の正式リリースに添付された `pico-macro-firmware.uf2` を常に返します。

## ディレクトリ構成

- `firmware/pico_switch_pad/pico_switch_pad.ino`
  - Pico W 向けファームウェア本体
- `.github/workflows/build-auto-release.yml`
  - main push 時に自動でビルド & タグ & リリースする GitHub Actions 定義
