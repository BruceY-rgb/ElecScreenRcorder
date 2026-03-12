# Download OBS Studio Portable Version
# Run this script in PowerShell on Windows

param(
    [string]$OutputDir = ".\obs-download"
)

$ErrorActionPreference = "Stop"

# Create output directory
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir | Out-Null
}

# Use the latest OBS Studio version
$Version = "32.1.0"
$ObsUrl = "https://github.com/obsproject/obs-studio/releases/download/$Version/OBS-Studio-$Version-Windows-x64.zip"

Write-Host "Downloading OBS Studio $Version..."
Write-Host "URL: $ObsUrl"

# Download file
$ZipPath = Join-Path $OutputDir "obs-studio-portable.zip"
try {
    Invoke-WebRequest -Uri $ObsUrl -OutFile $ZipPath -UseBasicParsing
    Write-Host "Downloaded to: $ZipPath"
    
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
} catch {
    Write-Host "Error downloading: $_"
    Write-Host "Please manually download from: https://github.com/obsproject/obs-studio/releases"
    exit 1
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
