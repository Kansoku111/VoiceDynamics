# Configure, build, run the DSP/host/editor tests and the Steinberg validator.
# -Quick builds only the DSP libraries and their tests (no plug-in, no validator).
param([ValidateSet('Release','Debug')][string]$Configuration='Release',[string]$SdkRoot,[string]$CMake,[string]$BuildDir,[switch]$Quick)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
if (-not $SdkRoot) { $SdkRoot=Join-Path $project 'third_party\vst3sdk' }
if (-not $CMake) { $CMake=Join-Path $project '.tools\Scripts\cmake.exe' }
if (-not $BuildDir) { $BuildDir=Join-Path $project 'build' }
if (-not (Test-Path -LiteralPath $CMake)) { throw 'tools/setup.ps1を実行するか、-CMakeで実行ファイルを指定してください。' }
if (-not (Test-Path -LiteralPath (Join-Path $SdkRoot 'CMakeLists.txt'))) { throw 'VST3 SDKがありません。tools/setup.ps1を実行してください。' }
$revision=git -C $SdkRoot rev-parse HEAD
if ($revision -ne '9fad9770f2ae8542ab1a548a68c1ad1ac690abe0') { throw '想定する固定版SDK（3.8.0 build 66）ではありません。' }
$work=Join-Path $project 'work'
New-Item -ItemType Directory -Force -Path $work | Out-Null
& $CMake -S $project -B $BuildDir -G 'Visual Studio 17 2022' -A x64 "-DVST3_SDK_ROOT=$SdkRoot" *> (Join-Path $work 'configure.log')
if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $work 'configure.log') -Tail 30; throw '構成に失敗しました。' }
if ($Quick) { $targets='vd_conformance_tests','vd_dsp_tests','vd_tool' }
else { $targets='VoiceDynamics','vd_conformance_tests','vd_dsp_tests','vd_host_tests','vd_editor_tests','vd_tool','validator' }
& $CMake --build $BuildDir --config $Configuration --target $targets --parallel 8 *> (Join-Path $work 'build.log')
if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $work 'build.log') -Tail 60; throw 'ビルドに失敗しました。' }
$ctest=Join-Path (Split-Path -Parent $CMake) 'ctest.exe'
& $ctest --test-dir $BuildDir -C $Configuration -V --output-on-failure *> (Join-Path $work 'ctest.log')
$testExit=$LASTEXITCODE
Get-Content (Join-Path $work 'ctest.log') -Tail 14
if ($testExit -ne 0) { throw '自動検査に失敗しました。work/ctest.logを確認してください。' }
if ($Quick) { return }
$bundle=Join-Path $BuildDir "VST3\$Configuration\VoiceDynamics.vst3"
& (Join-Path $BuildDir "bin\$Configuration\validator.exe") $bundle *> (Join-Path $work 'validator.log')
$validatorExit=$LASTEXITCODE
Get-Content (Join-Path $work 'validator.log') -Tail 4
if ($validatorExit -ne 0) { throw 'VST3 validatorに失敗しました。work/validator.logを確認してください。' }
Write-Output "VST3: $bundle"
