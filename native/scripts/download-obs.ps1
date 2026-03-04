# Download OBS Studio Portable Version
# Run this script in PowerShell on Windows

param(
    [string]$OutputDir = ".\obs-download",
    [string]$Version = "30.2.3"
)

$ErrorActionPreference = "Stop"

# Create output directory
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir | Out-Null
}

# OBS Studio portable URL (GitHub releases)
$ObsUrl = "https://github.com/obsproject/obs-studio/releases/download/$Version/OBS-Studio-$Version-portable.zip"

Write-Host "Downloading OBS Studio $Version..."
Write-Host "URL: $ObsUrl"

# Download file
$ZipPath = Join-Path $OutputDir "obs-studio-portable.zip"
try {
    Invoke-WebRequest -Uri $ObsUrl -OutFile $ZipPath -UseBasicParsing
    Write-Host "Downloaded to: $ZipPath"
} catch {
    Write-Host "Error downloading: $_"
    Write-Host "Trying alternative URL..."

    # Try alternative URL format
    $AltUrl = "https://github.com/obsproject/obs-studio/releases/download/$Version/OBS-Studio-$Version-full-portable.zip"
    try {
        Invoke-WebRequest -Uri $AltUrl -OutFile $ZipPath -UseBasicParsing
        Write-Host "Downloaded to: $ZipPath"
    } catch {
        Write-Host "Failed to download. Please manually download from:"
        Write-Host "https://github.com/obsproject/obs-studio/releases"
        exit 1
    }
}

# Extract
Write-Host "Extracting..."
$ExtractDir = Join-Path $OutputDir "extracted"
Expand-Archive -Path $ZipPath -DestinationPath $ExtractDir -Force

# Find the OBS folder
$ObsFolder = Get-ChildItem -Path $ExtractDir -Directory | Where-Object { $_.Name -like "*OBS*" } | Select-Object -First 1

if ($ObsFolder) {
    Write-Host "OBS extracted to: $($ObsFolder.FullName)"
    Write-Host ""
    Write-Host "Next steps:"
    Write-Host "1. Run: .\setup-obs.ps1 -ObsDir '$($ObsFolder.FullName)' -DistDir '..\dist'"
} else {
    Write-Host "Error: Could not find OBS folder in extracted files"
    exit 1
}
