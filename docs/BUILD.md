# VoiceDynamics ビルド・検証手順

## 環境（採用したもの）
Windows 11 / Visual Studio 2022（MSVC、静的CRT）/ Windows SDK 10.0.26100 / CMake 3.31.10（`.tools`に取得）/ VST3 SDK 3.8.0 build 66（commit `9fad9770f2ae8542ab1a548a68c1ad1ac690abe0`、`third_party\vst3sdk`）/ Python 3.13.12 + numpy 2.5.3・scipy 1.18.1・soundfile 0.14.0（独立測定用。`.tools`の仮想環境）。JUCEは使わない。

## 手順（リポジトリの直下で、PowerShell）
```powershell
.\tools\setup.ps1    # SDKとCMakeの用意（GitHubから固定版を取得）
.\tools\build.ps1    # 構成→ビルド→ctest（適合・DSP・ホスト・画面）→ Steinberg validator
.\tools\install.ps1  # C:\Program Files\Common Files\VST3 へ新規配置（要管理者権限）
```
- 出力：`build\VST3\Release\VoiceDynamics.vst3`（フォルダー全体で1つ）。
- 既に導入済みなら `-Replace`。使用中（Studio One等が読み込み中）なら何も変えずに止まる。Studio Oneを閉じてから実行する。
- 試験を個別に：`ctest --test-dir build -C Release --output-on-failure`。

## 追加の検証
- **pluginval（strictness 10）**：ASCIIだけのパスへコピーし、`Start-Process -Wait` で実行する。
  `Start-Process -Wait -NoNewWindow -FilePath <pluginval.exe> -ArgumentList '--strictness-level 10 --validate "C:\Temp\vdval\VoiceDynamics.vst3"' -RedirectStandardOutput pluginval.log`（pluginvalは他プラグインの `.tools` にあるものを使った）。
- **独立測定**：`python -I tools\crosscheck.py`（ITU表の係数でIntegratedを測り直す。6条件。結果は `work\crosscheck.json`）。
- **1時間の試験**：`build\Release\vd_tool.exe bench --minutes 60`（再生学習を高速で流して結果が出るまで）、`vd_tool gen x.wav --seconds 3600` → `vd_tool analyze x.wav`（WAV解析の時間）。
- 録音データ・一時ファイルは `work\`（git管理外）。

## 注意
- Pythonの標準出力は `PYTHONIOENCODING=utf-8` で実行する。
- ソース変更後は `build.ps1` を全部通してから `install.ps1 -Replace`。

## 公開（GitHub）
- 公開先：https://github.com/Kansoku111/VoiceDynamics （MIT、Public）。
- ソースの原本はこのプロジェクト。`.\tools\make_oss.ps1 -Zip` で `配布用\VoiceDynamics-oss`（公開リポジトリの作業ツリー。`.git`以外を作り直す）とリリース用ZIP（`work\release\`）を再生成し、そこでコミット・push、`gh release create` する。コミットの作者は `Kansoku111 <302713191+Kansoku111@users.noreply.github.com>`（個人のメールを出さない）。
