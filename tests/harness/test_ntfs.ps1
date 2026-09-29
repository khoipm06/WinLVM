# test_ntfs.ps1 - Format WinLVM volume as NTFS, perform real file I/O, verify integrity
param (
    [string]$DriveLetter = "E",
    [string]$LogFile = "C:\winlvm\test_ntfs.log"
)

$ErrorActionPreference = "Continue"
$allPassed = $true

function Log($msg) {
    $line = "[$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')] $msg"
    Write-Host $line
    Add-Content -Path $LogFile -Value $line
}

if (Test-Path $LogFile) { Remove-Item -Force $LogFile }

Log "=== START NTFS FORMAT AND FILE I/O VERIFICATION ==="

# 1. Identify WinLVM volume via Win32_Volume
$targetVol = Get-CimInstance Win32_Volume | Where-Object {
    $_.Label -eq "WinLVM_NTFS" -or
    $_.Label -eq "WinLVM" -or
    ($_.Capacity -ge 160000000 -and $_.Capacity -le 170000000 -and $_.FileSystem -ne "Btrfs" -and $_.FileSystem -ne "btrfs")
} | Select-Object -First 1

if (-not $targetVol) {
    Log "ERROR: WinLVM volume not found via Win32_Volume!"
    exit 1
}

$winlvmVolGuid = $targetVol.DeviceID
Log "Selected WinLVM volume: $winlvmVolGuid (Capacity: $($targetVol.Capacity), Prior FileSystem: $($targetVol.FileSystem))"

# 2. Mount to specified drive letter
$targetDrive = "$($DriveLetter):"
if ($targetVol.DriveLetter -ne $targetDrive) {
    Log "Mounting $winlvmVolGuid to $targetDrive ..."
    $targetVol | Set-CimInstance -Property @{DriveLetter=$targetDrive}
    Start-Sleep -Seconds 1
}

# 3. Format as NTFS
Log "Formatting $targetDrive as NTFS..."
$formatCmd = "format.com $targetDrive /FS:NTFS /V:WinLVM_NTFS /Q /Y"
$fmtOutput = cmd.exe /c $formatCmd
$fmtOutput | ForEach-Object { Log "FORMAT: $_" }

# Verify volume is recognized as NTFS
Start-Sleep -Seconds 2
$vol = Get-CimInstance Win32_Volume | Where-Object { $_.DriveLetter -eq "$($DriveLetter):" } | Select-Object -First 1
if ($vol -and $vol.FileSystem -eq "NTFS") {
    Log "NTFS-FORMAT: PASS (Size: $($vol.Capacity), FileSystem: $($vol.FileSystem), Label: $($vol.Label))"
} else {
    Log "NTFS-FORMAT: FAIL (Volume not recognized as NTFS: $($vol | Out-String))"
    $allPassed = $false
}

# 4. Write real files
Log "Testing File Writes..."
$testDir = "$targetDrive\TestDir"
New-Item -ItemType Directory -Path $testDir -Force | Out-Null

# 4a. Small text file
$smallPath = "$targetDrive\hello.txt"
$smallText = "Hello from WinLVM NTFS volume! Date: $(Get-Date -Format 'o')"
[System.IO.File]::WriteAllText($smallPath, $smallText)
$readText = [System.IO.File]::ReadAllText($smallPath)
if ($readText -eq $smallText) {
    Log "WRITE-SMALL-TEXT: PASS"
} else {
    Log "WRITE-SMALL-TEXT: FAIL"
    $allPassed = $false
}

# 4b. 4MB binary file with deterministic pseudo-random bytes
$binPath = "$testDir\data_4mb.bin"
$binSize = 4 * 1024 * 1024
$rng = New-Object System.Random(42)
$bytes = New-Object byte[] $binSize
$rng.NextBytes($bytes)
[System.IO.File]::WriteAllBytes($binPath, $bytes)

# Calculate expected SHA256 in memory
$sha256 = [System.Security.Cryptography.SHA256]::Create()
$expectedHash = [System.BitConverter]::ToString($sha256.ComputeHash($bytes)).Replace("-", "").ToLower()
Log "Expected 4MB file SHA256: $expectedHash"

# Read back from disk and compute hash
$readBytes = [System.IO.File]::ReadAllBytes($binPath)
$actualHash = [System.BitConverter]::ToString($sha256.ComputeHash($readBytes)).Replace("-", "").ToLower()
Log "Actual   4MB file SHA256: $actualHash"

if ($actualHash -eq $expectedHash) {
    Log "WRITE-READ-4MB-BINARY: PASS"
} else {
    Log "WRITE-READ-4MB-BINARY: FAIL"
    $allPassed = $false
}

# 4c. Deep nested structure
$nestedPath = "$testDir\sub1\sub2\sub3\nested.txt"
New-Item -ItemType Directory -Path (Split-Path $nestedPath) -Force | Out-Null
[System.IO.File]::WriteAllText($nestedPath, "Deep nested content in WinLVM")
$nestedRead = [System.IO.File]::ReadAllText($nestedPath)
if ($nestedRead -eq "Deep nested content in WinLVM") {
    Log "WRITE-NESTED-DIR: PASS"
} else {
    Log "WRITE-NESTED-DIR: FAIL"
    $allPassed = $false
}

# 5. Flush filesystem cache
Log "Flushing filesystem buffers..."
$hVol = [System.IO.File]::OpenWrite("$targetDrive\dummy.tmp")
$hVol.Close()
Remove-Item "$targetDrive\dummy.tmp" -Force -ErrorAction SilentlyContinue

# Final result
if ($allPassed) {
    Log "=== OVERALL NTFS TEST RESULT: PASS ==="
    exit 0
} else {
    Log "=== OVERALL NTFS TEST RESULT: FAIL ==="
    exit 1
}
