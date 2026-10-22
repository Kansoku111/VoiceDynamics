# Fetches the pinned VST3 SDK and CMake into this project only. Existing
# third_party/.tools contents are verified, never overwritten. When a sibling
# project already holds the same SDK commit, it is copied instead of cloned.
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$sdk=Join-Path $project 'third_party\vst3sdk'
$sdkCommit='9fad9770f2ae8542ab1a548a68c1ad1ac690abe0'
if (-not (Test-Path -LiteralPath $sdk)) {
    $copied=$false
    foreach ($name in @('VocalCarve','Glaze')) {
        $sibling=Join-Path (Split-Path -Parent $project) "$name\third_party\vst3sdk"
        if ((Test-Path -LiteralPath $sibling) -and ((git -C $sibling rev-parse HEAD) -eq $sdkCommit)) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $sdk) | Out-Null
            Copy-Item -LiteralPath $sibling -Destination $sdk -Recurse
            $copied=$true
            break
        }
    }
    if (-not $copied) {
        git clone --branch v3.8.0_build_66 --depth 1 https://github.com/steinbergmedia/vst3sdk.git $sdk
        if ($LASTEXITCODE -ne 0) { throw 'VST3 SDKの取得に失敗しました。' }
    }
}
$revision=git -C $sdk rev-parse HEAD
if ($revision -ne $sdkCommit) { throw "SDKの版が異なります（$revision）。既存SDKは変更しません。" }
git -C $sdk submodule update --init --depth 1 base cmake pluginterfaces public.sdk
if ($LASTEXITCODE -ne 0) { throw 'SDKの依存ファイルの取得に失敗しました。' }
$python=Join-Path $project '.tools\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python)) {
    python -m venv (Join-Path $project '.tools')
    if ($LASTEXITCODE -ne 0) { throw '開発用Python環境の作成に失敗しました。' }
}
& $python -m pip install --disable-pip-version-check cmake==3.31.10 numpy==2.5.3 soundfile==0.14.0 scipy==1.18.1
if ($LASTEXITCODE -ne 0) { throw 'CMake・評価用パッケージの取得に失敗しました。' }
& (Join-Path $project '.tools\Scripts\cmake.exe') --version
