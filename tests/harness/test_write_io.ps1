<#
.SYNOPSIS
    Black-box write verification for winlvm LVM2/LUKS2 volumes.
    Non-destructive: reads original bytes, writes test pattern, verifies, restores.
.DESCRIPTION
    Tests sector-aligned writes, stripe-seam writes, and PV-seam writes.
    Uses Win32 P/Invoke via embedded C# class for raw disk access.
.PARAMETER TargetDevice
    Target device path (e.g., \\.\E:, \\.\PhysicalDrive1). Default: auto-detect mounted drive E:.
.PARAMETER TestCase
    Test case to run: 'all', 'sector', 'stripe-seam', 'pv-seam'. Default: 'all'.
.OUTPUTS
    Logs to C:\winlvm\write_test.log and stdout with PASS/FAIL markers.
    Exit code 0 on all pass, non-zero on any failure.
#>

param(
    [Parameter(Position=0, Mandatory=$false)]
    [ValidateSet('all','sector','stripe-seam','pv-seam')]
    [string]$TestCase = 'all',

    [Parameter(Position=1, Mandatory=$false)]
    [string]$TargetDevice = ''
)

$ErrorActionPreference = 'Stop'
$LogPath = 'C:\winlvm\write_test.log'

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

# Ensure log directory exists
if (-not (Test-Path 'C:\winlvm')) {
    New-Item -ItemType Directory -Path 'C:\winlvm' -Force | Out-Null
}

# Clear previous log
Set-Content -Path $LogPath -Value '' -Encoding UTF8

Write-Log "=== winlvm write I/O verification started ==="
Write-Log "TestCase: $TestCase"

# Auto-detect target device if not provided
if (-not $TargetDevice) {
    for ($attempt = 1; $attempt -le 5; $attempt++) {
        $cimVol = Get-CimInstance Win32_Volume -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter -match 'E' }
        if ($cimVol) {
            $TargetDevice = "\\.\E:"
            Write-Log "Auto-detected target device via Win32_Volume: $TargetDevice"
            break
        }
        $vol = Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter -eq 'E' }
        if ($vol) {
            $TargetDevice = "\\.\E:"
            Write-Log "Auto-detected target device via Get-Volume: $TargetDevice"
            break
        }
        if ($TargetDevice) { break }
        Start-Sleep -Milliseconds 1000
    }
    if (-not $TargetDevice) {
        # Fall back to \\.\E: directly if exists in Win32_Volume or filesystem
        $TargetDevice = "\\.\E:"
        Write-Log "Defaulting to target device: $TargetDevice"
    }
} else {
    Write-Log "Using specified target device: $TargetDevice"
}

# Embedded C# Win32 P/Invoke helper
$csharpCode = @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public class WinLvmWriteIO : IDisposable
{
    // Win32 constants
    private const uint GENERIC_READ = 0x80000000;
    private const uint GENERIC_WRITE = 0x40000000;
    private const uint FILE_SHARE_READ = 0x00000001;
    private const uint FILE_SHARE_WRITE = 0x00000002;
    private const uint FILE_SHARE_DELETE = 0x00000004;
    private const uint OPEN_EXISTING = 3;
    private const uint FILE_FLAG_NO_BUFFERING = 0x20000000;
    private const uint FILE_FLAG_WRITE_THROUGH = 0x80000000;
    private const int INVALID_HANDLE_VALUE = -1;

    // Win32 API imports
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateFile(
        string lpFileName,
        uint dwDesiredAccess,
        uint dwShareMode,
        IntPtr lpSecurityAttributes,
        uint dwCreationDisposition,
        uint dwFlagsAndAttributes,
        IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetFilePointerEx(
        IntPtr hFile,
        long liDistanceToMove,
        out long lpNewFilePointer,
        uint dwMoveMethod);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ReadFile(
        IntPtr hFile,
        byte[] lpBuffer,
        uint nNumberOfBytesToRead,
        out uint lpNumberOfBytesRead,
        IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteFile(
        IntPtr hFile,
        byte[] lpBuffer,
        uint nNumberOfBytesToWrite,
        out uint lpNumberOfBytesWritten,
        IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(
        IntPtr hDevice,
        uint dwIoControlCode,
        IntPtr lpInBuffer,
        uint nInBufferSize,
        out long lpOutBuffer,
        uint nOutBufferSize,
        out uint lpBytesReturned,
        IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FlushFileBuffers(IntPtr hFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint GetLastError();

    private IntPtr _handle;
    private bool _disposed;

    public WinLvmWriteIO(string devicePath)
    {
        uint flags = FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
        _handle = CreateFile(
            devicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            IntPtr.Zero,
            OPEN_EXISTING,
            flags,
            IntPtr.Zero);

        if (_handle.ToInt32() == INVALID_HANDLE_VALUE)
        {
            uint err = GetLastError();
            throw new System.ComponentModel.Win32Exception((int)err,
                string.Format("CreateFile failed for {0}, error 0x{1:X}", devicePath, err));
        }
    }

    public void Dispose()
    {
        if (!_disposed && _handle != IntPtr.Zero)
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
        {
            return length;
        }
        return -1;
    }

    public bool SetPosition(long offset)
    {
        long newPos;
        return SetFilePointerEx(_handle, offset, out newPos, 0); // FILE_BEGIN = 0
    }

    public byte[] ReadBytes(long offset, int count)
    {
        if (!SetPosition(offset)) throw new IOException("SetFilePointerEx failed for read");
        byte[] buffer = new byte[count];
        uint bytesRead;
        bool ok = ReadFile(_handle, buffer, (uint)count, out bytesRead, IntPtr.Zero);
        if (!ok || bytesRead != count)
        {
            uint err = GetLastError();
            throw new IOException(string.Format("ReadFile failed at offset {0}, count {1}, error 0x{2:X}", offset, count, err));
        }
        return buffer;
    }

    public void WriteBytes(long offset, byte[] data)
    {
        if (!SetPosition(offset)) throw new IOException("SetFilePointerEx failed for write");
        uint bytesWritten;
        bool ok = WriteFile(_handle, data, (uint)data.Length, out bytesWritten, IntPtr.Zero);
        if (!ok || bytesWritten != data.Length)
        {
            uint err = GetLastError();
            throw new IOException(string.Format("WriteFile failed at offset {0}, count {1}, error 0x{2:X}", offset, data.Length, err));
        }
    }

    public void Flush()
    {
        if (!FlushFileBuffers(_handle))
        {
            uint err = GetLastError();
            throw new IOException(string.Format("FlushFileBuffers failed, error 0x{0:X}", err));
        }
    }
}
'@

Write-Log "Compiling C# helper class..."
Add-Type -TypeDefinition $csharpCode -Language CSharp -ErrorAction Stop
Write-Log "C# helper class compiled successfully."

# Test parameters
$SECTOR_OFFSET = 1048576          # 1 MiB
$SECTOR_SIZE = 512
$STRIPE_SEAM_OFFSET = 65536       # 64 KiB boundary
$STRIPE_SEAM_SIZE = 131072        # 128 KiB
$PV_SEAM_OFFSET = 83886080        # 80 MiB boundary
$PV_SEAM_SIZE = 131072            # 128 KiB

# Generate deterministic test pattern
function Get-TestPattern {
    param([int]$Size, [int]$Seed)
    $bytes = New-Object byte[] $Size
    $rnd = New-Object System.Random $Seed
    $rnd.NextBytes($bytes)
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
    Write-Log "Opening device: $TargetDevice"
    $disk = New-Object WinLvmWriteIO $TargetDevice
    $capacity = $disk.GetLength()
    Write-Log "Volume capacity: $capacity bytes ($([math]::Round($capacity / 1MB, 2)) MiB)"

    # Test 1: Sector-aligned write (512 bytes at 1 MiB)
    if ($TestCase -eq 'all' -or $TestCase -eq 'sector') {
        Write-Log "--- Test 1: Sector-aligned write (512 bytes at 1 MiB) ---"
        $original = $disk.ReadBytes($SECTOR_OFFSET, $SECTOR_SIZE)
        $pattern = Get-TestPattern -Size $SECTOR_SIZE -Seed 0xDEADBEEF
        $disk.WriteBytes($SECTOR_OFFSET, $pattern)
        $disk.Flush()
        $readback = $disk.ReadBytes($SECTOR_OFFSET, $SECTOR_SIZE)
        $verify = Compare-Bytes -Expected $pattern -Actual $readback
        Write-Marker "SECTOR-WRITE" $verify
        if (-not $verify) { $allPassed = $false }
        # Restore
        $disk.WriteBytes($SECTOR_OFFSET, $original)
        $disk.Flush()
        $restored = $disk.ReadBytes($SECTOR_OFFSET, $SECTOR_SIZE)
        $restoreOk = Compare-Bytes -Expected $original -Actual $restored
        if (-not $restoreOk) { $restorePassed = $false; Write-Log "SECTOR RESTORE FAILED" }
    }

    # Test 2: Stripe-seam write (128 KiB spanning 64 KiB boundary)
    if ($TestCase -eq 'all' -or $TestCase -eq 'stripe-seam') {
        if ($capacity -gt 0 -and $capacity -lt ($STRIPE_SEAM_OFFSET + $STRIPE_SEAM_SIZE)) {
            Write-Log "SKIP: Stripe-seam write ($STRIPE_SEAM_OFFSET + $STRIPE_SEAM_SIZE) exceeds volume capacity $capacity"
        } else {
            Write-Log "--- Test 2: Stripe-seam write (128 KiB at 64 KiB boundary) ---"
            $original = $disk.ReadBytes($STRIPE_SEAM_OFFSET, $STRIPE_SEAM_SIZE)
            $pattern = Get-TestPattern -Size $STRIPE_SEAM_SIZE -Seed 0xFEEDFACE
            $disk.WriteBytes($STRIPE_SEAM_OFFSET, $pattern)
            $disk.Flush()
            $readback = $disk.ReadBytes($STRIPE_SEAM_OFFSET, $STRIPE_SEAM_SIZE)
            $verify = Compare-Bytes -Expected $pattern -Actual $readback
            Write-Marker "STRIPE-SEAM-WRITE" $verify
            if (-not $verify) { $allPassed = $false }
            # Restore
            $disk.WriteBytes($STRIPE_SEAM_OFFSET, $original)
            $disk.Flush()
            $restored = $disk.ReadBytes($STRIPE_SEAM_OFFSET, $STRIPE_SEAM_SIZE)
            $restoreOk = Compare-Bytes -Expected $original -Actual $restored
            if (-not $restoreOk) { $restorePassed = $false; Write-Log "STRIPE-SEAM RESTORE FAILED" }
        }
    }

    # Test 3: PV-seam write (128 KiB spanning 80 MiB boundary)
    if ($TestCase -eq 'all' -or $TestCase -eq 'pv-seam') {
        if ($capacity -gt 0 -and $capacity -lt ($PV_SEAM_OFFSET + $PV_SEAM_SIZE)) {
            Write-Log "SKIP: PV-seam write ($PV_SEAM_OFFSET + $PV_SEAM_SIZE) exceeds volume capacity $capacity (striped-2pv is 32 MiB, PV-seam is for linear-2pv)"
        } else {
            Write-Log "--- Test 3: PV-seam write (128 KiB at 80 MiB boundary) ---"
            $original = $disk.ReadBytes($PV_SEAM_OFFSET, $PV_SEAM_SIZE)
            $pattern = Get-TestPattern -Size $PV_SEAM_SIZE -Seed 0xCAFEBABE
            $disk.WriteBytes($PV_SEAM_OFFSET, $pattern)
            $disk.Flush()
            $readback = $disk.ReadBytes($PV_SEAM_OFFSET, $PV_SEAM_SIZE)
            $verify = Compare-Bytes -Expected $pattern -Actual $readback
            Write-Marker "PV-SEAM-WRITE" $verify
            if (-not $verify) { $allPassed = $false }
            # Restore
            $disk.WriteBytes($PV_SEAM_OFFSET, $original)
            $disk.Flush()
            $restored = $disk.ReadBytes($PV_SEAM_OFFSET, $PV_SEAM_SIZE)
            $restoreOk = Compare-Bytes -Expected $original -Actual $restored
            if (-not $restoreOk) { $restorePassed = $false; Write-Log "PV-SEAM RESTORE FAILED" }
        }
    }

    $disk.Dispose()

    Write-Marker "RESTORE-VERIFY" $restorePassed
    if (-not $restorePassed) { $allPassed = $false }

} catch {
    Write-Log "EXCEPTION: $($_.Exception.Message)"
    Write-Marker "SECTOR-WRITE" $false
    Write-Marker "STRIPE-SEAM-WRITE" $false
    Write-Marker "PV-SEAM-WRITE" $false
    Write-Marker "RESTORE-VERIFY" $false
    $allPassed = $false
}

Write-Log "=== winlvm write I/O verification finished ==="
if ($allPassed) {
    Write-Log "RESULT: ALL TESTS PASSED"
    exit 0
} else {
    Write-Log "RESULT: ONE OR MORE TESTS FAILED"
    exit 1
}