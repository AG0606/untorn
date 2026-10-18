# PowerShell Runner for Untorn Engine
Write-Host "========================================================" -ForegroundColor Cyan
Write-Host " Launching Untorn Linux Kernel-Storage Subsystem via WSL" -ForegroundColor Yellow
Write-Host "========================================================" -ForegroundColor Cyan

$CurrentDir = (Get-Item -Path ".").FullName.Replace("\", "/").Replace("C:", "/mnt/c")
wsl bash -c "cd '$CurrentDir' && chmod +x ./run_demo.sh && ./run_demo.sh"
