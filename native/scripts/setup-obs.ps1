# Setup OBS DLLs for recorder_core
# Run this script in PowerShell on Windows

param(
    [Parameter(Mandatory=$true)]
    [string]$ObsDir,
    [Parameter(Mandatory=$true)]
    [string]$DistDir
)

$ErrorActionPreference = "Stop"

# Resolve paths
$ObsDir = (Resolve-Path $ObsDir).Path
$DistDir = (Resolve-Path $DistDir).Path

Write-Host "OBS Directory: $ObsDir"
Write-Host "Dist Directory: $DistDir"

# Create directory structure
$PluginsDir = Join-Path $DistDir "obs-plugins\64bit"
$DataDir = Join-Path $DistDir "data"
$DataPluginsDir = Join-Path $DataDir "obs-plugins"

if (-not (Test-Path $DistDir)) {
    New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
}
if (-not (Test-Path $PluginsDir)) {
    New-Item -ItemType Directory -Path $PluginsDir -Force | Out-Null
}
if (-not (Test-Path $DataPluginsDir)) {
    New-Item -ItemType Directory -Path $DataPluginsDir -Force | Out-Null
}

# Core DLLs to copy (root level)
$CoreDlls = @(
    "obs.dll",
    "libobs-d3d11.dll",
    "w32-pthreads.dll",
    "vcruntime140.dll",
    "msvcp140.dll",
    "msvcp140_1.dll",
    "msvcp140_2.dll",
    "msvcp140_codecvt_ids.dll"
)

Write-Host "`n=== Copying core DLLs ==="
foreach ($dll in $CoreDlls) {
    $src = Join-Path $ObsDir $dll
    if (Test-Path $src) {
        Copy-Item $src -Destination $DistDir -Force
        Write-Host "  Copied: $dll"
    } else {
        Write-Host "  Not found: $dll"
    }
}

# FFmpeg DLLs
$FFmpegDlls = @(
    "avcodec-60.dll",
    "avformat-60.dll",
    "avutil-58.dll",
    "swscale-7.dll",
    "swresample-4.dll",
    "zlib.dll",
    "libx264.dll",
    "libx265.dll",
    "libvpx.dll",
    "libmp3lame.dll",
    "libopus.dll",
    "libvorbis.dll",
    "libvorbisenc.dll"
)

Write-Host "`n=== Copying FFmpeg DLLs ==="
foreach ($dll in $FFmpegDlls) {
    $src = Join-Path $ObsDir $dll
    if (Test-Path $src) {
        Copy-Item $src -Destination $DistDir -Force
        Write-Host "  Copied: $dll"
    } else {
        Write-Host "  Not found: $dll"
    }
}

# Required plugins (64bit)
$RequiredPlugins = @(
    "win-capture.dll",
    "obs-ffmpeg.dll",
    "win-wasapi.dll",
    "obs-x264.dll",
    "obs-outputs.dll",
    "obs-av-ffmpeg.dll"
)

Write-Host "`n=== Copying plugins ==="
$Obs64Dir = Join-Path $ObsDir "obs-plugins\64bit"
if (Test-Path $Obs64Dir) {
    foreach ($plugin in $RequiredPlugins) {
        $src = Join-Path $Obs64Dir $plugin
        if (Test-Path $src) {
            Copy-Item $src -Destination $PluginsDir -Force
            Write-Host "  Copied: $plugin"
        } else {
            Write-Host "  Not found: $plugin"
        }
    }
} else {
    Write-Host "  Warning: obs-plugins\64bit not found, checking root..."
    foreach ($plugin in $RequiredPlugins) {
        $src = Join-Path $ObsDir $plugin
        if (Test-Path $src) {
            Copy-Item $src -Destination $PluginsDir -Force
            Write-Host "  Copied: $plugin"
        }
    }
}

# Data files
Write-Host "`n=== Copying data files ==="
$ObsDataDir = Join-Path $ObsDir "data\obs-plugins"
if (Test-Path $ObsDataDir) {
    # Copy plugin data folders
    $PluginFolders = @("win-capture", "obs-ffmpeg", "win-wasapi")
    foreach ($folder in $PluginFolders) {
        $src = Join-Path $ObsDataDir $folder
        if (Test-Path $src) {
            $dst = Join-Path $DataPluginsDir $folder
            Copy-Item $src -Destination $DataPluginsDir -Recurse -Force
            Write-Host "  Copied data: $folder"
        }
    }
}

# Copy libobs data
$LibobsDataSrc = Join-Path $ObsDir "data\libobs"
if (Test-Path $LibobsDataSrc) {
    $LibobsDataDst = Join-Path $DataDir "libobs"
    Copy-Item $LibobsDataSrc -Destination $DataDir -Recurse -Force
    Write-Host "  Copied data: libobs"
}

Write-Host "`n=== Setup complete ==="
Write-Host "Files copied to: $DistDir"
Write-Host ""
Write-Host "Directory structure:"
Get-ChildItem $DistDir -Recurse -File | Select-Object -First 20 | ForEach-Object {
    Write-Host "  $($_.FullName.Replace($DistDir, ''))"
}
