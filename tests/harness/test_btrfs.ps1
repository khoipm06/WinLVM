# test_btrfs.ps1 — Verify a Btrfs filesystem on a WinLVM volume via WinBtrfs.
param (
    [string]$DriveLetter = "F",
    [string]$ExpectedHash = "",
    [string]$LogFile = (Join-Path $env:TEMP "winlvm_test_btrfs.log")
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

Log "=== START BTRFS VERIFICATION ==="

$vols = Get-CimInstance Win32_Volume
$targetVol = $vols | Where-Object {
    $_.Label -eq "WinLVM_Btrfs" -or
    $_.FileSystem -eq "btrfs" -or
    $_.FileSystem -eq "Btrfs"
} | Select-Object -First 1

if (-not $targetVol) {
    Log "ERROR: WinLVM Btrfs volume not found via Win32_Volume."
    exit 1
}

$targetDrive = "$($DriveLetter.TrimEnd(':')):"
if ($targetVol.DriveLetter -ne $targetDrive) {
    $targetVol | Set-CimInstance -Property @{DriveLetter = $targetDrive}
    Start-Sleep -Seconds 1
}

$helloPath = "$targetDrive\hello_btrfs.txt"
if ((Test-Path $helloPath) -and ([System.IO.File]::ReadAllText($helloPath).Trim() -match "Hello from Linux Btrfs")) {
    Log "READ-LINUX-TEXT: PASS"
} else {
    Log "READ-LINUX-TEXT: FAIL"
    $allPassed = $false
}

$nestedPath = "$targetDrive\sub\nested.txt"
if ((Test-Path $nestedPath) -and ([System.IO.File]::ReadAllText($nestedPath).Trim() -eq "Nested directory file on Btrfs")) {
    Log "READ-NESTED-DIR: PASS"
} else {
    Log "READ-NESTED-DIR: FAIL"
    $allPassed = $false
}

$binPath = "$targetDrive\data_2mb.bin"
if (Test-Path $binPath) {
    $bytes = [System.IO.File]::ReadAllBytes($binPath)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $actualHash = [System.BitConverter]::ToString($sha256.ComputeHash($bytes)).Replace("-", "").ToLower()
    if (-not $ExpectedHash -or $actualHash -eq $ExpectedHash.ToLower()) {
        Log "READ-LINUX-BINARY-SHA256: PASS ($actualHash)"
    } else {
        Log "READ-LINUX-BINARY-SHA256: FAIL (expected=$ExpectedHash actual=$actualHash)"
        $allPassed = $false
    }
} else {
    Log "READ-LINUX-BINARY-SHA256: FAIL (file not found: $binPath)"
    $allPassed = $false
}

$winPath = "$targetDrive\win_created.txt"
$winMsg = "Written from Windows via WinBtrfs on WinLVM at $(Get-Date -Format 'o')"
[System.IO.File]::WriteAllText($winPath, $winMsg)
if ([System.IO.File]::ReadAllText($winPath) -eq $winMsg) {
    Log "WRITE-READ-WINDOWS-FILE: PASS"
} else {
    Log "WRITE-READ-WINDOWS-FILE: FAIL"
    $allPassed = $false
}

if ($allPassed) {
    Log "=== OVERALL BTRFS TEST RESULT: PASS ==="
    exit 0
}
Log "=== OVERALL BTRFS TEST RESULT: FAIL ==="
exit 1
