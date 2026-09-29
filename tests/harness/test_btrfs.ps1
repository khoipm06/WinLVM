# test_btrfs.ps1 - Verify Btrfs filesystem on WinLVM volume via WinBtrfs driver
param (
    [string]$DriveLetter = "F",
    [string]$ExpectedHash = "22e7c31453dd59764b8d4d8c23cd1d1f62958919125630da9592a4134278e9f1",
    [string]$LogFile = "C:\winlvm\test_btrfs.log"
)

$ErrorActionPreference = "Continue"
$allPassed = $true

function Log($msg) {
    $line = "[$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')] $msg"
    Write-Host $line
    Add-Content -Path $LogFile -Value $line
}

if (Test-Path $LogFile) { Remove-Item -Force $LogFile }

Log "=== START BTRFS AND PARTITIONED PV VERIFICATION ==="

# 1. Identify Btrfs / WinLVM volume
$vols = Get-CimInstance Win32_Volume
$targetVol = $vols | Where-Object {
    $_.Label -eq "WinLVM_Btrfs" -or
    $_.FileSystem -eq "btrfs" -or
    $_.FileSystem -eq "Btrfs" -or
    ($_.Capacity -ge 150000000 -and $_.Capacity -le 175000000 -and $_.FileSystem -ne "NTFS")
} | Select-Object -First 1

if (-not $targetVol) {
    Log "All detected volumes:"
    $vols | ForEach-Object { Log "  $($_.DeviceID) - Drive: $($_.DriveLetter) - Label: $($_.Label) - FS: $($_.FileSystem) - Cap: $($_.Capacity)" }
    Log "ERROR: WinLVM Btrfs volume not found via Win32_Volume!"
    exit 1
}

$volGuid = $targetVol.DeviceID
Log "Target Volume found: $volGuid (Label: $($targetVol.Label), FS: $($targetVol.FileSystem), Capacity: $($targetVol.Capacity))"

# 2. Assign drive letter if not already assigned
$targetDrive = "$($DriveLetter):"
if ($targetVol.DriveLetter -ne $targetDrive) {
    Log "Mounting $volGuid to $targetDrive ..."
    $targetVol | Set-CimInstance -Property @{DriveLetter=$targetDrive}
    Start-Sleep -Seconds 1
}

# 3. Read files created on Linux
# 3a. Text file
$helloPath = "$targetDrive\hello_btrfs.txt"
if (Test-Path $helloPath) {
    $content = [System.IO.File]::ReadAllText($helloPath).Trim()
    Log "Read hello_btrfs.txt: '$content'"
    if ($content -match "Hello from Linux Btrfs") {
        Log "READ-LINUX-TEXT: PASS"
    } else {
        Log "READ-LINUX-TEXT: FAIL (unexpected content: $content)"
        $allPassed = $false
    }
} else {
    Log "READ-LINUX-TEXT: FAIL (file not found: $helloPath)"
    $allPassed = $false
}

# 3b. Nested text file
$nestedPath = "$targetDrive\sub\nested.txt"
if (Test-Path $nestedPath) {
    $nestedContent = [System.IO.File]::ReadAllText($nestedPath).Trim()
    if ($nestedContent -eq "Nested directory file on Btrfs") {
        Log "READ-NESTED-DIR: PASS"
    } else {
        Log "READ-NESTED-DIR: FAIL"
        $allPassed = $false
    }
} else {
    Log "READ-NESTED-DIR: FAIL (file not found: $nestedPath)"
    $allPassed = $false
}

# 3c. 2MB binary file SHA256
$binPath = "$targetDrive\data_2mb.bin"
if (Test-Path $binPath) {
    $bytes = [System.IO.File]::ReadAllBytes($binPath)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $actualHash = [System.BitConverter]::ToString($sha256.ComputeHash($bytes)).Replace("-", "").ToLower()
    Log "Expected SHA256: $ExpectedHash"
    Log "Actual   SHA256: $actualHash"
    if ($actualHash -eq $ExpectedHash) {
        Log "READ-LINUX-BINARY-SHA256: PASS"
    } else {
        Log "READ-LINUX-BINARY-SHA256: FAIL (hash mismatch)"
        $allPassed = $false
    }
} else {
    Log "READ-LINUX-BINARY-SHA256: FAIL (file not found: $binPath)"
    $allPassed = $false
}

# 4. Write new file from Windows
Log "Testing write to Btrfs from Windows..."
$winPath = "$targetDrive\win_created.txt"
$winMsg = "Written from Windows 10 via WinBtrfs on WinLVM at $(Get-Date -Format 'o')"
[System.IO.File]::WriteAllText($winPath, $winMsg)
$readWinMsg = [System.IO.File]::ReadAllText($winPath)

if ($readWinMsg -eq $winMsg) {
    Log "WRITE-READ-WINDOWS-FILE: PASS"
} else {
    Log "WRITE-READ-WINDOWS-FILE: FAIL"
    $allPassed = $false
}

# Final result
if ($allPassed) {
    Log "=== OVERALL BTRFS TEST RESULT: PASS ==="
    exit 0
} else {
    Log "=== OVERALL BTRFS TEST RESULT: FAIL ==="
    exit 1
}
