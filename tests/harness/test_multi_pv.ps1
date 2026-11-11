# test_multi_pv.ps1 — Black-box verification of multi-PV corpus volumes on Windows.
# Verifies whole-volume and cross-boundary window SHA-256 digests against
# corpus/expected/<case>/lv-hashes.txt without hardcoded signing or SDK paths.

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('auto', 'linear-2pv', 'striped-2pv', 'all')]
    [string]$TestCase = 'auto',

    [Parameter()]
    [string]$TargetDevice = $null,

    [Parameter()]
    [string]$ExpectedDir = $null,

    [Parameter()]
    [string]$LogFile = (Join-Path $env:TEMP "winlvm_multi_pv_test.log"),

    [Parameter()]
    [int]$DetectTimeoutSeconds = 20
)

$ErrorActionPreference = 'Continue'

$logDir = Split-Path -Path $LogFile -Parent
if ($logDir -and -not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}
if (Test-Path $LogFile) { Remove-Item -Force $LogFile -ErrorAction SilentlyContinue }

function Log([string]$msg) {
    $ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'
    $line = "[$ts] $msg"
    Write-Output $line
    Add-Content -Path $LogFile -Value $line -ErrorAction SilentlyContinue
}

if (-not ([System.Management.Automation.PSTypeName]'WinLvmVolIO').Type) {
    Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

public class WinLvmVolIO {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    public static extern IntPtr CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr hFile);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ReadFile(IntPtr hFile, [Out] byte[] lpBuffer, uint nBytesToRead,
        out uint lpBytesRead, IntPtr lpOverlapped);
    [DllImport("kernel32.dll", EntryPoint = "ReadFile", SetLastError = true)]
    public static extern bool ReadFilePtr(IntPtr hFile, IntPtr lpBuffer, uint nBytesToRead,
        out uint lpBytesRead, IntPtr lpOverlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool SetFilePointerEx(IntPtr hFile, long liDistanceToMove,
        out long lpNewFilePointer, uint dwMoveMethod);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeviceIoControl(IntPtr hDevice, uint dwIoControlCode,
        byte[] lpInBuffer, uint nInBufferSize, byte[] lpOutBuffer, uint nOutBufferSize,
        out uint lpBytesReturned, IntPtr lpOverlapped);

    public const uint GENERIC_READ = 0x80000000;
    public const uint GENERIC_WRITE = 0x40000000;
    public const uint FILE_SHARE_READ = 0x01;
    public const uint FILE_SHARE_WRITE = 0x02;
    public const uint FILE_SHARE_DELETE = 0x04;
    public const uint OPEN_EXISTING = 3;
    public const uint FILE_ATTRIBUTE_NORMAL = 0x80;
    public const uint FILE_BEGIN = 0;

    public const uint IOCTL_DISK_GET_LENGTH_INFO = 0x0007405c;
    public const uint IOCTL_WINLVM_REFRESH       = 0x0022e004;
    public const uint IOCTL_WINLVM_GET_LOG       = 0x0022601c;

    public static IntPtr OpenVolume(string path) {
        if (path.EndsWith("\\") && !path.EndsWith(":\\")) path = path.TrimEnd('\\');
        else if (path.EndsWith(":\\")) path = path.Substring(0, path.Length - 1);
        return CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            IntPtr.Zero, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);
    }

    public static IntPtr OpenControl() {
        return CreateFile(@"\\.\WinLvmControl", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
    }

    public static long GetVolumeLength(IntPtr hDevice) {
        byte[] outBuf = new byte[8];
        uint ret = 0;
        if (DeviceIoControl(hDevice, IOCTL_DISK_GET_LENGTH_INFO, null, 0, outBuf, (uint)outBuf.Length, out ret, IntPtr.Zero))
            return BitConverter.ToInt64(outBuf, 0);
        return -1;
    }

    public static string GetKernelLog() {
        IntPtr h = OpenControl();
        if (h.ToInt64() == -1) return "OpenControl failed err=" + Marshal.GetLastWin32Error();
        try {
            byte[] outBuf = new byte[65536];
            uint ret = 0;
            if (DeviceIoControl(h, IOCTL_WINLVM_GET_LOG, null, 0, outBuf, (uint)outBuf.Length, out ret, IntPtr.Zero))
                return Encoding.ASCII.GetString(outBuf, 0, (int)ret);
            return "DeviceIoControl failed err=" + Marshal.GetLastWin32Error();
        } finally {
            CloseHandle(h);
        }
    }

    public static bool TriggerRefresh() {
        IntPtr h = OpenControl();
        if (h.ToInt64() == -1) return false;
        try {
            byte[] inBuf = new byte[8];
            BitConverter.GetBytes(1u).CopyTo(inBuf, 0);
            BitConverter.GetBytes(8u).CopyTo(inBuf, 4);
            uint ret = 0;
            return DeviceIoControl(h, IOCTL_WINLVM_REFRESH, inBuf, (uint)inBuf.Length, null, 0, out ret, IntPtr.Zero);
        } finally {
            CloseHandle(h);
        }
    }

    public static byte[] ReadWindow(IntPtr hFile, long offset, int length) {
        int sectorSize = 512;
        long baseOff = offset - (offset % sectorSize);
        int delta = (int)(offset - baseOff);
        int readLen = length + delta;
        if (readLen % sectorSize != 0) readLen += sectorSize - (readLen % sectorSize);
        long dummy;
        if (!SetFilePointerEx(hFile, baseOff, out dummy, FILE_BEGIN))
            throw new Exception("SetFilePointerEx failed err=" + Marshal.GetLastWin32Error());
        byte[] rawBuf = new byte[readLen];
        GCHandle handle = GCHandle.Alloc(rawBuf, GCHandleType.Pinned);
        try {
            IntPtr ptr = handle.AddrOfPinnedObject();
            uint totalRead = 0;
            while (totalRead < (uint)readLen) {
                uint n;
                IntPtr dest = new IntPtr(ptr.ToInt64() + totalRead);
                if (!ReadFilePtr(hFile, dest, (uint)readLen - totalRead, out n, IntPtr.Zero))
                    throw new Exception("ReadFile failed err=" + Marshal.GetLastWin32Error());
                if (n == 0) throw new Exception("Unexpected EOF");
                totalRead += n;
            }
        } finally {
            handle.Free();
        }
        byte[] winBuf = new byte[length];
        Array.Copy(rawBuf, delta, winBuf, 0, length);
        return winBuf;
    }

    public static string StreamHash(IntPtr hFile, long totalBytes) {
        long dummy;
        if (!SetFilePointerEx(hFile, 0, out dummy, FILE_BEGIN))
            throw new Exception("SetFilePointerEx(0) failed err=" + Marshal.GetLastWin32Error());
        using (var sha = SHA256.Create()) {
            int bufferSize = 4194304;
            byte[] chunk = new byte[bufferSize];
            long totalRead = 0;
            while (totalRead < totalBytes) {
                int toRead = (int)Math.Min((long)bufferSize, totalBytes - totalRead);
                int readLen = toRead;
                int rem = readLen % 512;
                if (rem != 0) readLen += (512 - rem);
                byte[] buf = (readLen == bufferSize) ? chunk : new byte[readLen];
                uint n;
                if (!ReadFile(hFile, buf, (uint)readLen, out n, IntPtr.Zero))
                    throw new Exception("StreamHash ReadFile failed err=" + Marshal.GetLastWin32Error());
                if (n < (uint)toRead) throw new Exception("StreamHash short read");
                sha.TransformBlock(buf, 0, toRead, buf, 0);
                totalRead += toRead;
            }
            sha.TransformFinalBlock(new byte[0], 0, 0);
            return BitConverter.ToString(sha.Hash).Replace("-", "").ToLower();
        }
    }

    public static string Sha256(byte[] data) {
        using (var sha = SHA256.Create()) {
            return BitConverter.ToString(sha.ComputeHash(data)).Replace("-", "").ToLower();
        }
    }
}
"@
}

function Load-OracleFromFile([string]$caseName, [string]$lvName, [string]$filePath) {
    $cap = 0L
    $whole = $null
    $wins = @()
    foreach ($line in (Get-Content $filePath)) {
        if ($line -match "^#\s+\S+/$lvName\s+size=(\d+)$") {
            $cap = [long]$Matches[1]
        } elseif ($line -match "^$lvName\s+offset=(\d+)\s+length=(\d+)\s+sha256=([0-9a-fA-F]{64})$") {
            $wins += @{ Off = [long]$Matches[1]; Len = [int]$Matches[2]; Exp = $Matches[3].ToLower() }
        } elseif ($line -match "^$lvName\s+whole\s+sha256=([0-9a-fA-F]{64})$") {
            $whole = $Matches[1].ToLower()
        }
    }
    return @{ Name = $caseName; Capacity = $cap; ExpectedWholeHash = $whole; Windows = $wins }
}

$TestCases = @{
    'linear-2pv' = @{
        Name = 'linear-2pv'
        Capacity = 167772160
        ExpectedWholeHash = 'c21a07e21aefc661b564cd5a6fb7d3f8f1a867093b42184c71bf91d4fd75356f'
        Windows = @(
            @{ Off = 0;         Len = 131072; Exp = '2ec21306483a13b6772098e00f73a4f99350ccf03110546c27abd65ff08f070a' },
            @{ Off = 55924053;  Len = 131072; Exp = 'fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471' },
            @{ Off = 83886080;  Len = 131072; Exp = 'fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471' },
            @{ Off = 167641088; Len = 131072; Exp = 'fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471' }
        )
    }
    'striped-2pv' = @{
        Name = 'striped-2pv'
        Capacity = 33554432
        ExpectedWholeHash = 'adf2465d48ad6a9fe15fad0df3eed54785b7ea23a2b712831dad3f8271e81f10'
        Windows = @(
            @{ Off = 0;         Len = 131072; Exp = '95f8387680885639f013f904bc6f9ece422787abe22848aff520e54139a24f8f' },
            @{ Off = 11184810;  Len = 131072; Exp = 'fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471' },
            @{ Off = 16777216;  Len = 131072; Exp = '0a2e9560e3617e71a1543e05c3596afd825bbf5feaea26d790cd4494e9284e9e' },
            @{ Off = 33423360;  Len = 131072; Exp = 'fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471' }
        )
    }
}

if ($ExpectedDir) {
    $linFile = Join-Path $ExpectedDir "linear-2pv\lv-hashes.txt"
    $strFile = Join-Path $ExpectedDir "striped-2pv\lv-hashes.txt"
    if (Test-Path $linFile) { $TestCases['linear-2pv'] = Load-OracleFromFile 'linear-2pv' 'lvspan' $linFile }
    if (Test-Path $strFile) { $TestCases['striped-2pv'] = Load-OracleFromFile 'striped-2pv' 'lvstr' $strFile }
}

function Find-VolumeInfo([long]$expectedCapacity) {
    foreach ($v in (Get-CimInstance Win32_Volume -ErrorAction SilentlyContinue)) {
        if ($v.Capacity -eq $expectedCapacity) {
            $drive = if ($v.DriveLetter) { $v.DriveLetter.TrimEnd('\') } else { $null }
            $path = if ($drive) { "\\.\" + $drive } else { $v.DeviceID.TrimEnd('\') }
            return @{ Path = $path; Capacity = $v.Capacity }
        }
    }
    return $null
}

$casesToRun = @()
if ($TargetDevice) {
    $h = [WinLvmVolIO]::OpenVolume($TargetDevice)
    if ($h.ToInt64() -eq -1) { Log "RAW-OPEN: FAIL ($TargetDevice)"; exit 1 }
    $len = [WinLvmVolIO]::GetVolumeLength($h)
    [WinLvmVolIO]::CloseHandle($h) | Out-Null
    $sel = if ($TestCase -ne 'auto') { $TestCase } elseif ($len -eq 167772160) { 'linear-2pv' } elseif ($len -eq 33554432) { 'striped-2pv' } else { $null }
    if (-not $sel) { Log "VOLUME-DETECT: FAIL (unrecognized length $len)"; exit 1 }
    $casesToRun += @{ Definition = $TestCases[$sel]; VolumeInfo = @{ Path = $TargetDevice; Capacity = $len } }
} else {
    $foundLinear = $null; $foundStriped = $null
    for ($iter = 0; $iter -lt $DetectTimeoutSeconds; $iter++) {
        if (($TestCase -in 'auto', 'linear-2pv', 'all') -and -not $foundLinear) { $foundLinear = Find-VolumeInfo 167772160 }
        if (($TestCase -in 'auto', 'striped-2pv', 'all') -and -not $foundStriped) { $foundStriped = Find-VolumeInfo 33554432 }
        if (($TestCase -eq 'linear-2pv' -and $foundLinear) -or ($TestCase -eq 'striped-2pv' -and $foundStriped) -or ($TestCase -eq 'all' -and $foundLinear -and $foundStriped) -or ($TestCase -eq 'auto' -and ($foundLinear -or $foundStriped))) { break }
        if ($iter -eq 0 -or $iter -eq 4) { [WinLvmVolIO]::TriggerRefresh() | Out-Null }
        Start-Sleep -Seconds 1
    }
    if ($foundLinear -and ($TestCase -in 'auto', 'linear-2pv', 'all')) { $casesToRun += @{ Definition = $TestCases['linear-2pv']; VolumeInfo = $foundLinear } }
    if ($foundStriped -and ($TestCase -in 'auto', 'striped-2pv', 'all')) { $casesToRun += @{ Definition = $TestCases['striped-2pv']; VolumeInfo = $foundStriped } }
}

if ($casesToRun.Count -eq 0) {
    Log "VOLUME-DETECT: FAIL (No matching multi-PV volume found)"
    Log "KERNEL-LOG:`n$([WinLvmVolIO]::GetKernelLog())"
    exit 1
}

$allPassed = $true
foreach ($entry in $casesToRun) {
    $tc = $entry.Definition
    $path = $entry.VolumeInfo.Path
    $h = [WinLvmVolIO]::OpenVolume($path)
    if ($h.ToInt64() -eq -1) { Log "RAW-OPEN: FAIL ($path)"; $allPassed = $false; continue }
    try {
        $wholeHash = [WinLvmVolIO]::StreamHash($h, $tc.Capacity)
        if ($wholeHash -eq $tc.ExpectedWholeHash) {
            Log "$($tc.Name) WHOLE-HASH: PASS ($wholeHash)"
        } else {
            Log "$($tc.Name) WHOLE-HASH: FAIL expected=$($tc.ExpectedWholeHash) got=$wholeHash"
            $allPassed = $false
        }
        foreach ($win in $tc.Windows) {
            $hsh = [WinLvmVolIO]::Sha256([WinLvmVolIO]::ReadWindow($h, $win.Off, $win.Len))
            if ($hsh -eq $win.Exp) {
                Log "$($tc.Name) WINDOW [$($win.Off)+$($win.Len)]: PASS"
            } else {
                Log "$($tc.Name) WINDOW [$($win.Off)+$($win.Len)]: FAIL expected=$($win.Exp) got=$hsh"
                $allPassed = $false
            }
        }
    } finally {
        [WinLvmVolIO]::CloseHandle($h) | Out-Null
    }
}

if ($allPassed) { Log "MULTI-PV: PASS"; exit 0 }
Log "MULTI-PV: FAIL"
exit 1
