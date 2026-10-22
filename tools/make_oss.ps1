# Regenerates the public repository tree (and optionally the release ZIP) from
# this project and docs\voice-dynamics. The sources stay here; the export is a copy.
# The export folder keeps its own .git, everything else in it is replaced.
param([string]$Out, [switch]$Zip)
$ErrorActionPreference='Stop'
$project=Split-Path -Parent $PSScriptRoot
$lab=Split-Path -Parent (Split-Path -Parent $project)
if (-not $Out) { $Out=Join-Path $lab '配布用\VoiceDynamics-oss' }
$docs=Join-Path $lab 'docs\voice-dynamics'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Get-ChildItem -LiteralPath $Out -Force | Where-Object Name -ne '.git' | Remove-Item -Recurse -Force
foreach ($item in 'src','tests','licenses','CMakeLists.txt','README.md','LICENSE','THIRD_PARTY_NOTICES.md','.gitignore') {
    Copy-Item -LiteralPath (Join-Path $project $item) -Destination $Out -Recurse
}
New-Item -ItemType Directory -Force -Path (Join-Path $Out 'tools'),(Join-Path $Out 'docs') | Out-Null
foreach ($item in 'setup.ps1','build.ps1','install.ps1','crosscheck.py','vd_tool.cpp','make_oss.ps1') {
    Copy-Item -LiteralPath (Join-Path $project "tools\$item") -Destination (Join-Path $Out 'tools')
}
foreach ($item in 'BUILD.md','DECISIONS.md','USER-TEST.md') {
    Copy-Item -LiteralPath (Join-Path $docs $item) -Destination (Join-Path $Out 'docs')
}
Copy-Item -LiteralPath (Join-Path $docs 'evidence\0.2.0\editor-done-100.png') -Destination (Join-Path $Out 'docs\screenshot.png')
if ($Zip) {
    $version=(Select-String -LiteralPath (Join-Path $project 'src\version.h') -Pattern 'VD_VERSION "([^"]+)"').Matches[0].Groups[1].Value
    $stage=Join-Path $project "work\release\VoiceDynamics-$version-win64"
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item -LiteralPath (Join-Path $project 'build\VST3\Release\VoiceDynamics.vst3') -Destination $stage -Recurse
    foreach ($item in 'LICENSE','THIRD_PARTY_NOTICES.md','licenses','README.md') { Copy-Item -LiteralPath (Join-Path $project $item) -Destination $stage -Recurse }
    $zipPath="$stage.zip"
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath
    Write-Output $zipPath
}
Write-Output $Out
