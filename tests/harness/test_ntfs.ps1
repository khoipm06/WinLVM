# test_ntfs.ps1 — Format a mounted WinLVM volume as NTFS and verify file I/O integrity.
param (
    [string]$DriveLetter = "E",
    [string]$LogFile = (Join-Path $env:TEMP "winlvm_test_ntfs.log")
)

$ErrorActionPreference = "Continue"
$allPassed = $true

$logDir = Split-Path -Path $LogFile -Parent
if ($logDir -and -not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}
if (Test-Path $LogFile) { Remove-Item -Force $LogFile }

function Log([string]$msg) {
    $line = "[$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')] $msg"
    Write-Host $line
    Add-Content -Path $LogFile -Value $line
}

Log "=== START NTFS FORMAT AND FILE I/O VERIFICATION ==="

$targetVol = Get-CimInstance Win32_Volume | Where-Object {
    $_.Label -eq "WinLVM_NTFS" -or
    $_.Label -eq "WinLVM" -or
    ($_.Capacity -ge 160000000 -and $_.Capacity -le 170000000 -and $_.FileSystem -ne "Btrfs" -and $_.FileSystem -ne "btrfs")
} | Select-Object -First 1

if (-not $targetVol) {
    Log "ERROR: WinLVM volume not found via Win32_Volume."
    exit 1
}

$winlvmVolGuid = $targetVol.DeviceID
Log "Selected WinLVM volume: $winlvmVolGuid (Capacity: $($targetVol.Capacity), Prior FileSystem: $($targetVol.FileSystem))"

$targetDrive = "$($DriveLetter.TrimEnd(':')):"
if ($targetVol.DriveLetter -ne $targetDrive) {
    Log "Mounting $winlvmVolGuid to $targetDrive ..."
    $targetVol | Set-CimInstance -Property @{DriveLetter = $targetDrive}
    Start-Sleep -Seconds 1
}

Log "Formatting $targetDrive as NTFS..."
$fmtOutput = cmd.exe /c "format.com $targetDrive /FS:NTFS /V:WinLVM_NTFS /Q /Y"
$fmtOutput | ForEach-Object { Log "FORMAT: $_" }

Start-Sleep -Seconds 2
$vol = Get-CimInstance Win32_Volume | Where-Object { $_.DriveLetter -eq $targetDrive } | Select-Object -First 1
if ($vol -and $vol.FileSystem -eq "NTFS") {
    Log "NTFS-FORMAT: PASS (Size: $($vol.Capacity), FileSystem: $($vol.FileSystem), Label: $($vol.Label))"
} else {
    Log "NTFS-FORMAT: FAIL"
    $allPassed = $false
}

$testDir = "$targetDrive\TestDir"
New-Item -ItemType Directory -Path $testDir -Force | Out-Null

$smallPath = "$targetDrive\hello.txt"
$smallText = "Hello from WinLVM NTFS volume! Date: $(Get-Date -Format 'o')"
[System.IO.File]::WriteAllText($smallPath, $smallText)
if ([System.IO.File]::ReadAllText($smallPath) -eq $smallText) {
    Log "WRITE-SMALL-TEXT: PASS"
} else {
    Log "WRITE-SMALL-TEXT: FAIL"
    $allPassed = $false
}

$binPath = "$testDir\data_4mb.bin"
$bytes = New-Object byte[] (4 * 1024 * 1024)
(New-Object System.Random(42)).NextBytes($bytes)
[System.IO.File]::WriteAllBytes($binPath, $bytes)

$sha256 = [System.Security.Cryptography.SHA256]::Create()
$expectedHash = [System.BitConverter]::ToString($sha256.ComputeHash($bytes)).Replace("-", "").ToLower()
$actualHash = [System.BitConverter]::ToString($sha256.ComputeHash([System.IO.File]::ReadAllBytes($binPath))).Replace("-", "").ToLower()
if ($actualHash -eq $expectedHash) {
    Log "WRITE-READ-4MB-BINARY: PASS ($actualHash)"
} else {
    Log "WRITE-READ-4MB-BINARY: FAIL (expected=$expectedHash actual=$actualHash)"
    $allPassed = $false
}

$nestedPath = "$testDir\sub1\sub2\sub3\nested.txt"
New-Item -ItemType Directory -Path (Split-Path $nestedPath) -Force | Out-Null
[System.IO.File]::WriteAllText($nestedPath, "Deep nested content in WinLVM")
if ([System.IO.File]::ReadAllText($nestedPath) -eq "Deep nested content in WinLVM") {
    Log "WRITE-NESTED-DIR: PASS"
} else {
    Log "WRITE-NESTED-DIR: FAIL"
    $allPassed = $false
}

if ($allPassed) {
    Log "=== OVERALL NTFS TEST RESULT: PASS ==="
    exit 0
}
Log "=== OVERALL NTFS TEST RESULT: FAIL ==="
exit 1
