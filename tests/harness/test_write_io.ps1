<#
.SYNOPSIS
    Non-destructive raw block write verification for WinLVM volumes.
.DESCRIPTION
    Tests sector-aligned writes, stripe-seam writes (64 KiB boundary), and
    PV-seam writes (80 MiB boundary), restoring original bytes after each test.
.PARAMETER TestCase
    Test case to run: 'all', 'sector', 'stripe-seam', 'pv-seam'. Default: 'all'.
.PARAMETER TargetDevice
    Target device path (e.g. \\.\E:). Default: auto-detect mounted drive E:.
.PARAMETER LogPath
    Log output path. Default: $env:TEMP\winlvm_write_test.log.
#>

param(
    [Parameter(Position = 0, Mandatory = $false)]
    [ValidateSet('all', 'sector', 'stripe-seam', 'pv-seam')]
    [string]$TestCase = 'all',

    [Parameter(Position = 1, Mandatory = $false)]
    [string]$TargetDevice = '',

    [Parameter(Mandatory = $false)]
    [string]$LogPath = (Join-Path $env:TEMP 'winlvm_write_test.log')
)

$ErrorActionPreference = 'Stop'

$logDir = Split-Path -Path $LogPath -Parent
if ($logDir -and -not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}
Set-Content -Path $LogPath -Value '' -Encoding UTF8

function Write-Log {
    param([string]$Message)
    $timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'
    $line = "[$timestamp] $Message"
    Write-Host $line
    Add-Content -Path $LogPath -Value $line -Encoding UTF8
}

function Write-Marker {
    param([string]$TestName, [bool]$Passed)
    $marker = if ($Passed) { 'PASS' } else { 'FAIL' }
    Write-Log "$($TestName): $marker"
}

Write-Log "=== winlvm write I/O verification started (TestCase=$TestCase) ==="

if (-not $TargetDevice) {
    $TargetDevice = "\\.\E:"
    Write-Log "Defaulting to target device: $TargetDevice"
} else {
    Write-Log "Using specified target device: $TargetDevice"
}

$csharpCode = @'
using System;
using System.IO;
using System.Runtime.InteropServices;

public class WinLvmWriteIO : IDisposable
{
    private const uint GENERIC_READ = 0x80000000;
    private const uint GENERIC_WRITE = 0x40000000;
    private const uint FILE_SHARE_READ = 0x00000001;
    private const uint FILE_SHARE_WRITE = 0x00000002;
    private const uint FILE_SHARE_DELETE = 0x00000004;
    private const uint OPEN_EXISTING = 3;
    private const uint FILE_FLAG_NO_BUFFERING = 0x20000000;
    private const uint FILE_FLAG_WRITE_THROUGH = 0x80000000;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateFile(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetFilePointerEx(
        IntPtr hFile, long liDistanceToMove, out long lpNewFilePointer, uint dwMoveMethod);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ReadFile(
        IntPtr hFile, byte[] lpBuffer, uint nNumberOfBytesToRead,
        out uint lpNumberOfBytesRead, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteFile(
        IntPtr hFile, byte[] lpBuffer, uint nNumberOfBytesToWrite,
        out uint lpNumberOfBytesWritten, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(
        IntPtr hDevice, uint dwIoControlCode, IntPtr lpInBuffer, uint nInBufferSize,
        out long lpOutBuffer, uint nOutBufferSize, out uint lpBytesReturned, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FlushFileBuffers(IntPtr hFile);

    private IntPtr _handle;
    private bool _disposed;

    public WinLvmWriteIO(string devicePath)
    {
        uint flags = FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
        _handle = CreateFile(
            devicePath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            IntPtr.Zero, OPEN_EXISTING, flags, IntPtr.Zero);
        if (_handle.ToInt64() == -1)
        {
            int err = Marshal.GetLastWin32Error();
            throw new System.ComponentModel.Win32Exception(err,
                string.Format("CreateFile failed for {0}, error 0x{1:X}", devicePath, err));
        }
    }

    public void Dispose()
    {
        if (!_disposed && _handle != IntPtr.Zero && _handle.ToInt64() != -1)
        {
            CloseHandle(_handle);
            _handle = IntPtr.Zero;
            _disposed = true;
        }
        GC.SuppressFinalize(this);
    }

    ~WinLvmWriteIO() { Dispose(); }

    public long GetLength()
    {
        long length = 0;
        uint bytesReturned;
        const uint IOCTL_DISK_GET_LENGTH_INFO = 0x0007405C;
        if (DeviceIoControl(_handle, IOCTL_DISK_GET_LENGTH_INFO, IntPtr.Zero, 0,
                            out length, 8, out bytesReturned, IntPtr.Zero))
            return length;
        return -1;
    }

    public byte[] ReadBytes(long offset, int count)
    {
        long newPos;
        if (!SetFilePointerEx(_handle, offset, out newPos, 0))
            throw new IOException("SetFilePointerEx failed for read");
        byte[] buffer = new byte[count];
        uint bytesRead;
        if (!ReadFile(_handle, buffer, (uint)count, out bytesRead, IntPtr.Zero) || bytesRead != count)
            throw new IOException("ReadFile failed err=" + Marshal.GetLastWin32Error());
        return buffer;
    }

    public void WriteBytes(long offset, byte[] data)
    {
        long newPos;
        if (!SetFilePointerEx(_handle, offset, out newPos, 0))
            throw new IOException("SetFilePointerEx failed for write");
        uint bytesWritten;
        if (!WriteFile(_handle, data, (uint)data.Length, out bytesWritten, IntPtr.Zero) || bytesWritten != data.Length)
            throw new IOException("WriteFile failed err=" + Marshal.GetLastWin32Error());
    }

    public void Flush()
    {
        if (!FlushFileBuffers(_handle))
            throw new IOException("FlushFileBuffers failed err=" + Marshal.GetLastWin32Error());
    }
}
'@

if (-not ([System.Management.Automation.PSTypeName]'WinLvmWriteIO').Type) {
    Add-Type -TypeDefinition $csharpCode -Language CSharp -ErrorAction Stop
}

$SECTOR_OFFSET = 1048576
$SECTOR_SIZE = 512
$STRIPE_SEAM_OFFSET = 65536
$STRIPE_SEAM_SIZE = 131072
$PV_SEAM_OFFSET = 83886080
$PV_SEAM_SIZE = 131072

function Get-TestPattern {
    param([int]$Size, [int]$Seed)
    $bytes = New-Object byte[] $Size
    (New-Object System.Random $Seed).NextBytes($bytes)
    return $bytes
}

function Compare-Bytes {
    param([byte[]]$Expected, [byte[]]$Actual)
    if ($Expected.Length -ne $Actual.Length) { return $false }
    for ($i = 0; $i -lt $Expected.Length; $i++) {
        if ($Expected[$i] -ne $Actual[$i]) { return $false }
    }
    return $true
}

$allPassed = $true
$restorePassed = $true

try {
    $disk = New-Object WinLvmWriteIO $TargetDevice
    $capacity = $disk.GetLength()
    Write-Log "Volume capacity: $capacity bytes"

    if ($TestCase -eq 'all' -or $TestCase -eq 'sector') {
        $original = $disk.ReadBytes($SECTOR_OFFSET, $SECTOR_SIZE)
        $pattern = Get-TestPattern -Size $SECTOR_SIZE -Seed 0x13572468
        $disk.WriteBytes($SECTOR_OFFSET, $pattern)
        $disk.Flush()
        $verify = Compare-Bytes -Expected $pattern -Actual ($disk.ReadBytes($SECTOR_OFFSET, $SECTOR_SIZE))
        Write-Marker "SECTOR-WRITE" $verify
        if (-not $verify) { $allPassed = $false }
        $disk.WriteBytes($SECTOR_OFFSET, $original)
        $disk.Flush()
        if (-not (Compare-Bytes -Expected $original -Actual ($disk.ReadBytes($SECTOR_OFFSET, $SECTOR_SIZE)))) {
            $restorePassed = $false
        }
    }

    if ($TestCase -eq 'all' -or $TestCase -eq 'stripe-seam') {
        if ($capacity -gt 0 -and $capacity -ge ($STRIPE_SEAM_OFFSET + $STRIPE_SEAM_SIZE)) {
            $original = $disk.ReadBytes($STRIPE_SEAM_OFFSET, $STRIPE_SEAM_SIZE)
            $pattern = Get-TestPattern -Size $STRIPE_SEAM_SIZE -Seed 0x24681357
            $disk.WriteBytes($STRIPE_SEAM_OFFSET, $pattern)
            $disk.Flush()
            $verify = Compare-Bytes -Expected $pattern -Actual ($disk.ReadBytes($STRIPE_SEAM_OFFSET, $STRIPE_SEAM_SIZE))
            Write-Marker "STRIPE-SEAM-WRITE" $verify
            if (-not $verify) { $allPassed = $false }
            $disk.WriteBytes($STRIPE_SEAM_OFFSET, $original)
            $disk.Flush()
            if (-not (Compare-Bytes -Expected $original -Actual ($disk.ReadBytes($STRIPE_SEAM_OFFSET, $STRIPE_SEAM_SIZE)))) {
                $restorePassed = $false
            }
        }
    }

    if ($TestCase -eq 'all' -or $TestCase -eq 'pv-seam') {
        if ($capacity -gt 0 -and $capacity -ge ($PV_SEAM_OFFSET + $PV_SEAM_SIZE)) {
            $original = $disk.ReadBytes($PV_SEAM_OFFSET, $PV_SEAM_SIZE)
            $pattern = Get-TestPattern -Size $PV_SEAM_SIZE -Seed 0x5A5A5A5A
            $disk.WriteBytes($PV_SEAM_OFFSET, $pattern)
            $disk.Flush()
            $verify = Compare-Bytes -Expected $pattern -Actual ($disk.ReadBytes($PV_SEAM_OFFSET, $PV_SEAM_SIZE))
            Write-Marker "PV-SEAM-WRITE" $verify
            if (-not $verify) { $allPassed = $false }
            $disk.WriteBytes($PV_SEAM_OFFSET, $original)
            $disk.Flush()
            if (-not (Compare-Bytes -Expected $original -Actual ($disk.ReadBytes($PV_SEAM_OFFSET, $PV_SEAM_SIZE)))) {
                $restorePassed = $false
            }
        }
    }

    $disk.Dispose()
    Write-Marker "RESTORE-VERIFY" $restorePassed
    if (-not $restorePassed) { $allPassed = $false }
} catch {
    Write-Log "EXCEPTION: $($_.Exception.Message)"
    $allPassed = $false
}

if ($allPassed) {
    Write-Log "RESULT: ALL TESTS PASSED"
    exit 0
}
Write-Log "RESULT: ONE OR MORE TESTS FAILED"
exit 1
