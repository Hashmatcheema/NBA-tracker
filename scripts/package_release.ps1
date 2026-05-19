# Build a clean public-release ZIP from this repo. Excludes vendored runtimes,
# build outputs, debug logs, captured gameplay clips, and local AI/tooling files.
#
# Usage (from a fresh, clean working tree):
#     pwsh scripts/package_release.ps1 -Version 1.0.0
#
# The result is written to dist\j2k_vision_<Version>.zip relative to the repo root.

param(
    [Parameter(Mandatory = $true)] [string]$Version,
    [string]$OutDir = "dist"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $repoRoot

# Always-excluded paths (relative to repo root). Keep this in sync with .gitignore.
$excludePatterns = @(
    "\.git",
    "\.github",        # repo-internal CI definitions
    "\.claude",
    "\.cursor",
    "_backup_before_cleanup",
    "cpp\\build($|\\)",
    "cpp\\build_dll($|\\)",
    "cpp\\build_dll[0-9]+($|\\)",
    "cpp\\build_dll_agent($|\\)",
    "cpp\\onnxruntime-win-x64-gpu-",   # large vendored runtime tree
    "python\\bin\\onnxruntime_providers_(cuda|tensorrt)\.dll$",
    "python\\__pycache__($|\\)",
    "python\\debug-.*\.log$",
    "python\\bin\\debug-.*\.log$",
    "python\\runs($|\\)",
    "python\\reports($|\\)",
    "python\\models\\j2k_vendor($|\\)",
    "debug-.*\.log$",
    "build_trace\.txt$",
    "analysis_frame_.*\.png$",
    "recent_frame_.*\.png$",
    "verify_frame_.*\.png$",
    "video_audit_frames($|\\)",
    "_det_test_.*\.log$",
    ".*\\.webm$",
    ".*\\.mov$",
    "NBA.*\.mp4$",
    ".*_score\.txt$",
    ".*_frames\.csv$",
    ".*_annotated\.mp4$",
    "j2k_panel_python\.path$",
    "j2k_error\.txt$",
    "j2k_crash\.txt$"
)

function Test-Excluded {
    param([string]$relPath)
    foreach ($pat in $excludePatterns) {
        if ($relPath -match $pat) { return $true }
    }
    return $false
}

# Stage release tree under a temp directory.
$stage = Join-Path $env:TEMP "j2k_vision_release_$Version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$repoRootStr = $repoRoot.Path
$copied = 0
Get-ChildItem -Recurse -File . | ForEach-Object {
    $rel = $_.FullName.Substring($repoRootStr.Length).TrimStart('\','/')
    if (Test-Excluded $rel) { return }
    $dst = Join-Path $stage $rel
    $dstDir = Split-Path $dst -Parent
    if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Force -Path $dstDir | Out-Null }
    Copy-Item -LiteralPath $_.FullName -Destination $dst -Force
    $copied++
}

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
$zipPath = Join-Path $OutDir "j2k_vision_$Version.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zipPath -CompressionLevel Optimal
Remove-Item -Recurse -Force $stage

Pop-Location

Write-Host "[package_release] copied $copied files"
Write-Host "[package_release] wrote $zipPath"
