# ==============================================================================
# UNTORN: POWERSHELL TURNKEY LIVE DEFENSE DEMONSTRATION LAUNCHER
# Course: Operating Systems Capstone — Final Defense & Committee Review
# ==============================================================================

param(
    [switch]$NonInteractive
)

$ErrorActionPreference = "Stop"

Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host "     UNTORN: KERNEL-STORAGE CO-DESIGN CAPSTONE LIVE DEFENSE LAUNCHER            " -ForegroundColor Yellow
Write-Host "     Bridging VFS Extents & NVMe AWUN for Zero-Logging Storage Engines          " -ForegroundColor Yellow
Write-Host "================================================================================" -ForegroundColor Cyan

# Verify WSL is available
try {
    $wslCheck = wsl uname -s
    if ($wslCheck -ne "Linux") {
        Write-Error "WSL Linux subsystem not responding."
    }
} catch {
    Write-Error "WSL is required to run the Untorn Linux kernel subsystem. Please ensure WSL2 is installed."
}

# Translate current Windows directory to WSL path
$CurrentDir = (Get-Item -Path ".").FullName.Replace("\", "/").Replace("C:", "/mnt/c")
$NonIntFlag = if ($NonInteractive) { "--non-interactive" } else { "" }

Write-Host "[Launcher] Starting Untorn Linux execution inside WSL at $CurrentDir..." -ForegroundColor Green

wsl bash -c "cd '$CurrentDir' && chmod +x ./run_defense_demo.sh && ./run_defense_demo.sh $NonIntFlag"
