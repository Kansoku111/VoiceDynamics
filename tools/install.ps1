# Copies a built bundle into the standard VST3 folder.
# Without -Replace an existing VoiceDynamics is never touched. With -Replace the installed bundle
# is first checked (not loaded by Studio One, not locked), then copied to BackupDir
# (default: work\installed-backup-<time>) and verified; the new files are written over the
# installed ones, verified, and the backup is copied back if anything fails.
# Nothing is changed while Studio One (or another process) uses the installed plug-in.
param([string]$Bundle, [switch]$Replace, [string]$BackupDir)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
if (-not $Bundle) { $Bundle=Join-Path $project 'build\VST3\Release\VoiceDynamics.vst3' }
$source=(Resolve-Path -LiteralPath $Bundle).Path
$binary=Join-Path $source 'Contents\x86_64-win\VoiceDynamics.vst3'
if (-not (Test-Path -LiteralPath $binary)) { throw 'Windows x64用VST3バンドルが見つかりません。' }
$destination=Join-Path $env:CommonProgramFiles 'VST3\VoiceDynamics.vst3'
$installed=Join-Path $destination 'Contents\x86_64-win\VoiceDynamics.vst3'
function Copy-Verified([string]$from, [string]$to) {
    foreach ($file in Get-ChildItem -LiteralPath $from -Recurse -File -Force) {
        $relative=$file.FullName.Substring($from.Length + 1)
        $target=[IO.Path]::GetFullPath((Join-Path $to $relative))
        if (-not $target.StartsWith($to + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '配置先の範囲外です。' }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
        if ((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath $target).Hash) { throw "照合に失敗しました: $relative" }
    }
}
if (-not (Test-Path -LiteralPath $destination)) {
    Copy-Verified $source $destination
    Write-Output "新規に配置し、SHA256を照合しました: $destination ($((Get-FileHash -LiteralPath $installed).Hash))"
    Write-Output 'Studio Oneのプラグイン一覧でVoiceDynamicsを確認してください。表示されない場合は作業を保存して再スキャンまたは再起動します。'
    return
}
if (-not $Replace) { throw "既存版を保護しました。導入先が存在します: $destination（入れ替える場合は -Replace）" }
# 1. In use? (loaded by any process, or opened without sharing)
$users=@(Get-Process -ErrorAction SilentlyContinue | Where-Object {
    try { $_.Modules | Where-Object { $_.FileName -ieq $installed } } catch { $null }
} | ForEach-Object { "$($_.ProcessName) ($($_.Id))" })
if ($users.Count) { throw "使用中のため変更しませんでした: $($users -join ', ')。Studio Oneを終了してから実行してください。" }
try {
    $probe=[IO.File]::Open($installed, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $probe.Dispose()
} catch { throw "導入済みのファイルを開けないため変更しませんでした（使用中の可能性）: $installed" }
# 2. Verified copy of the installed bundle.
if (-not $BackupDir) { $BackupDir=Join-Path $project ("work\installed-backup-" + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$backup=Join-Path $BackupDir 'VoiceDynamics.vst3'
if (Test-Path -LiteralPath $backup) { throw "退避先が既に存在します: $backup" }
Copy-Verified $destination $backup
$old=(Get-FileHash -LiteralPath $installed).Hash
Write-Output "旧版を退避しました: $backup ($old)"
# 3. New files over the installed ones; restore on any failure.
try {
    Copy-Verified $source $destination
    $stale=@(Get-ChildItem -LiteralPath $destination -Recurse -File -Force | Where-Object { -not (Test-Path -LiteralPath (Join-Path $source $_.FullName.Substring($destination.Length + 1))) })
    foreach ($file in $stale) { Remove-Item -LiteralPath $file.FullName -Force }
} catch {
    Write-Output '失敗したため旧版を戻します。'
    Copy-Verified $backup $destination
    throw
}
Write-Output "入れ替えとSHA256照合が完了しました: $destination ($((Get-FileHash -LiteralPath $installed).Hash))"
Write-Output 'Studio Oneを起動し、プラグイン一覧でVoiceDynamicsを確認してください。'
