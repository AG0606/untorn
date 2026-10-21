# ==============================================================================
# UNTORN: POWERSHELL TURNKEY LIVE DEFENSE DEMONSTRATION LAUNCHER
# Course: Operating Systems Capstone — Final Defense & Committee Review
# Repository: https://github.com/AG0606/untorn (Release: v1.0-final)
# ==============================================================================

[CmdletBinding()]
param(
    [Alias("y", "non-interactive", "n")]
    [switch]$NonInteractive,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs
)

$ErrorActionPreference = "Stop"

Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host "         UNTORN: KERNEL-STORAGE CO-DESIGN CAPSTONE LIVE DEFENSE LAUNCHER        " -ForegroundColor Yellow
Write-Host "     Bridging VFS Extents & NVMe AWUN for Zero-Logging Storage Engines          " -ForegroundColor Yellow
Write-Host "================================================================================" -ForegroundColor Cyan

# Check for non-interactive flag in RemainingArgs or environment variable
$IsNonInteractive = $NonInteractive
if ($env:CI -and $env:CI -ne "0") {
    $IsNonInteractive = $true
}
if ($RemainingArgs) {
    foreach ($arg in $RemainingArgs) {
        if ($arg -match "--non-interactive|-y|--batch|-n|--ci") {
            $IsNonInteractive = $true
        }
    }
}

# Verify WSL availability
try {
    $wslCheck = wsl uname -s 2>$null
    if ($wslCheck -ne "Linux") {
        Write-Error "WSL Linux subsystem not responding. Please verify WSL is running."
    }
} catch {
    Write-Error "WSL is required to execute the Untorn Linux kernel subsystem. Please ensure WSL2 is installed."
}

# Translate current Windows working directory to WSL mount path (/mnt/c/...)
$WinPath = (Get-Item -Path ".").FullName
$DriveLetter = $WinPath.Substring(0, 1).ToLower()
$SubPath = $WinPath.Substring(2).Replace("\", "/")
$WslPath = "/mnt/$DriveLetter$SubPath"

$PassFlag = if ($IsNonInteractive) { "--non-interactive" } else { "" }
$ModeStr = if ($IsNonInteractive) { "Automated Non-Interactive Verification" } else { "Interactive Live Defense Presentation" }

Write-Host "[Launcher] Target Directory: $WslPath" -ForegroundColor Green
Write-Host "[Launcher] Execution Mode  : $ModeStr" -ForegroundColor Green
Write-Host "[Launcher] Launching run_defense_demo.sh inside WSL..." -ForegroundColor Cyan

wsl bash -c "cd '$WslPath' && chmod +x ./run_defense_demo.sh && ./run_defense_demo.sh $PassFlag"
