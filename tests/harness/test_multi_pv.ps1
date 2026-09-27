# test_multi_pv.ps1 - Automated verification of Multi-PV LVM2 volumes on Windows (Linear-2PV and Striped-2PV)
# Black-box test harness operating strictly against ground truth oracles without internal driver implementation assumptions.

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('auto', 'linear-2pv', 'striped-2pv', 'all')]
    [string]$TestCase = 'auto',

    [Parameter()]
    [string]$TargetDevice = $null,

    [Parameter()]
    [switch]$SkipInstall = $false,

    [Parameter()]
    [switch]$ForceReinstall = $false,

    [Parameter()]
    [string]$LogFile = "C:\winlvm\multi_pv_test.log",

    [Parameter()]
    [int]$DetectTimeoutSeconds = 20
)

$ErrorActionPreference = 'Continue'

# Initialize Log File
if (Test-Path $LogFile) {
    try { Remove-Item -Force $LogFile -ErrorAction SilentlyContinue } catch {}
}
$logDir = Split-Path $LogFile -Parent
if ($logDir -and -not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}

function Log([string]$msg) {
    $ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'
    $line = "[$ts] $msg"
    Write-Output $line
    try {
        Add-Content -Path $LogFile -Value $line -ErrorAction SilentlyContinue
    } catch {}
}

Log "=== START WINLVM MULTI-PV VERIFICATION HARNESS ==="
Log "Parameters: TestCase='$TestCase', TargetDevice='$TargetDevice', SkipInstall=$SkipInstall, ForceReinstall=$ForceReinstall"

# 1. Driver Installation / Verification
$scOut = sc.exe query winlvm 2>&1 | Out-String
$driverRunning = ($scOut -match "STATE\s+:\s+4\s+RUNNING")

if ($ForceReinstall -or (-not $driverRunning -and -not $SkipInstall)) {
    Log "Step 1: Installing / re-installing WinLVM driver..."

    # Certificate Trust
    $cerPath = "C:\winlvm\driver\x64\Release\winlvm.cer"
    if (-not (Test-Path $cerPath)) {
        $foundCer = Get-ChildItem -Path "C:\winlvm\driver" -Filter "*.cer" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($foundCer) { $cerPath = $foundCer.FullName }
    }
    if (Test-Path $cerPath) {
        certutil -addstore Root $cerPath | Out-Null
        certutil -addstore TrustedPublisher $cerPath | Out-Null
        Log "CERT-TRUST: PASS ($cerPath)"
    } else {
        Log "CERT-TRUST: SKIP (Certificate not found at $cerPath)"
    }

    # Package and stage winlvm-vol.inf BEFORE installing the bus
    $volPkg = "C:\winlvm\vol_pkg"
    $volInfSource = "C:\winlvm\driver\winlvm-vol.inf"
    if (Test-Path $volInfSource) {
        if (-not (Test-Path $volPkg)) { New-Item -ItemType Directory -Path $volPkg -Force | Out-Null }
        Copy-Item -Path $volInfSource -Destination $volPkg -Force

        $inf2cat = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\Inf2Cat.exe"
        $signtool = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"

        if ((Test-Path $inf2cat) -and (Test-Path $signtool)) {
            $catOut = & $inf2cat /driver:$volPkg /os:10_X64 2>&1
            Log "INF2CAT-OUTPUT: $($catOut | Out-String)"

            $signOut = & $signtool sign /ph /fd sha256 /sha1 0AB516A704EFF016D4DBE37CEEDC255C7C8CF395 "$volPkg\winlvm-vol.cat" 2>&1
            Log "SIGNTOOL-OUTPUT: $($signOut | Out-String)"

            $addVolOut = pnputil.exe /add-driver "$volPkg\winlvm-vol.inf" /install 2>&1
            Log "PNPUTIL-VOL-OUTPUT: $($addVolOut | Out-String)"
        }
    }

    # Driver Install (ROOT\winlvm)
    $devcon = "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe"
    $infPath = "C:\winlvm\driver\x64\Release\winlvm\winlvm.inf"
    if (-not (Test-Path $infPath)) {
        $foundInf = Get-ChildItem -Path "C:\winlvm\driver" -Filter "winlvm.inf" -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.DirectoryName -like "*Release*" } | Select-Object -First 1
        if ($foundInf) { $infPath = $foundInf.FullName }
    }

    if ((Test-Path $devcon) -and (Test-Path $infPath)) {
        & $devcon remove "ROOT\winlvm" 2>&1 | Out-Null
        $installOut = & $devcon install $infPath "ROOT\winlvm" 2>&1
        Log "DEVCON-INSTALL-OUTPUT: $($installOut | Out-String)"

        $statusOut = & $devcon status "ROOT\winlvm" 2>&1
        Log "DEVCON-BUS-STATUS: $($statusOut | Out-String)"

        $childStatus = & $devcon status "WinLvm\Volume*" 2>&1
        Log "DEVCON-CHILD-STATUS: $($childStatus | Out-String)"
    }
} else {
    Log "Step 1: Driver service 'winlvm' is already RUNNING (or install skipped)."
}

# 2. Define WinLvmVolIO Helper
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
        byte[] lpInBuffer, uint nInBufferSize,
        byte[] lpOutBuffer, uint nOutBufferSize,
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
        if (path.EndsWith("\\") && !path.EndsWith(":\\")) {
            path = path.TrimEnd('\\');
        } else if (path.EndsWith(":\\")) {
            path = path.Substring(0, path.Length - 1);
        }
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
        if (DeviceIoControl(hDevice, IOCTL_DISK_GET_LENGTH_INFO, null, 0, outBuf, (uint)outBuf.Length, out ret, IntPtr.Zero)) {
            return BitConverter.ToInt64(outBuf, 0);
        }
        return -1;
    }

    public static string GetKernelLog() {
        IntPtr h = OpenControl();
        if (h.ToInt64() == -1) return "OpenControl failed err=" + Marshal.GetLastWin32Error();
        try {
            byte[] outBuf = new byte[65536];
            uint ret = 0;
            if (DeviceIoControl(h, IOCTL_WINLVM_GET_LOG, null, 0, outBuf, (uint)outBuf.Length, out ret, IntPtr.Zero)) {
                return Encoding.ASCII.GetString(outBuf, 0, (int)ret);
            }
            return "DeviceIoControl failed err=" + Marshal.GetLastWin32Error();
        } finally {
            CloseHandle(h);
        }
    }

    public static bool TriggerRefresh() {
        IntPtr h = OpenControl();
        if (h.ToInt64() == -1) return false;
        try {
            byte[] inBuf = new byte[16];
            BitConverter.GetBytes(1).CopyTo(inBuf, 0);  // version = 1
            BitConverter.GetBytes(16).CopyTo(inBuf, 4); // size = 16
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
        if (readLen % sectorSize != 0) {
            readLen += sectorSize - (readLen % sectorSize);
        }

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
                if (n == 0) throw new Exception("EOF prematurely encountered");
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
            int bufferSize = 4194304; // 4 MiB chunks
            byte[] chunk = new byte[bufferSize];
            long totalRead = 0;

            while (totalRead < totalBytes) {
                int toRead = (int)Math.Min((long)bufferSize, totalBytes - totalRead);
                int readLen = toRead;
                int rem = readLen % 512;
                if (rem != 0) {
                    readLen += (512 - rem);
                }

                byte[] buf = (readLen == bufferSize) ? chunk : new byte[readLen];
                uint n;
                if (!ReadFile(hFile, buf, (uint)readLen, out n, IntPtr.Zero))
                    throw new Exception("StreamHash ReadFile failed err=" + Marshal.GetLastWin32Error() + " at offset " + totalRead);
                if (n < (uint)toRead)
                    throw new Exception(string.Format("StreamHash unexpected short read: expected {0}, got {1} at {2}", toRead, n, totalRead));

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

# 3. Ground Truth Test Oracle Definitions
$TestCases = @{
    'linear-2pv' = @{
        Name = 'linear-2pv'
        Capacity = 167772160 # 160 MiB
        Description = 'Linear LVM2 volume spanning 2 PVs (160 MiB)'
        ExpectedWholeHash = 'a4a504e587019178f5217e7f450efb26e683b4672f1a4026802ca67d82bf66ed'
        Windows = @(
            @{ Off = 0;         Len = 131072; Exp = 'f82cff7d52b46e275e159fb2bef5dc65336e135289ae31bf24a152e9588b09f9' },
            @{ Off = 55924053;  Len = 131072; Exp = 'fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471' },
            @{ Off = 83886080;  Len = 131072; Exp = 'fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471' },
            @{ Off = 167641088; Len = 131072; Exp = 'fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471' }
        )
        PayloadToken = 'Case: linear-2pv'
        PayloadRawOffset = 9962496
        PayloadRawLength = 127
    }
    'striped-2pv' = @{
        Name = 'striped-2pv'
        Capacity = 33554432 # 32 MiB
        Description = 'Striped LVM2 volume across 2 PVs (32 MiB)'
        ExpectedWholeHash = '381507f9e93c945612c8c9b50434e745f55dc7cfec5ebf306bb1b45596a9fc8e'
        Windows = @(
            @{ Off = 0;         Len = 131072; Exp = 'b3bdea8ac49d325b2b66c82c6de6e5970f78a4b17c2ce14ec0295c38c31f4333' },
            @{ Off = 11184810;  Len = 131072; Exp = 'da5a088cd6d0ea038b0bee73fa3d7cc37b33ce820cc71c4ae87d0dd227a1a447' },
            @{ Off = 16777216;  Len = 131072; Exp = 'fc72e66bf1bfd2b0f9b0ab04f85471223e4f6a3e7eb355676e3251863f8d1c9d' },
            @{ Off = 33423360;  Len = 131072; Exp = 'fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471' }
        )
        PayloadToken = 'Case: striped-2pv'
        PayloadRawOffset = 9962496
        PayloadRawLength = 127
    }
}

function Format-DevicePath([string]$rawPath) {
    if (-not $rawPath) { return $null }
    $p = $rawPath.Trim()
    if ($p -match '^[A-Za-z]:?$') {
        $dl = $p.TrimEnd(':')
        return "\\.\" + $dl + ":"
    }
    if ($p.StartsWith("\\?\Volume{") -or $p.StartsWith("\\.\Volume{")) {
        return $p.TrimEnd('\')
    }
    return $p
}

function Find-VolumeInfo([long]$expectedCapacity) {
    # 1. CimInstance Win32_Volume
    try {
        $cimVols = Get-CimInstance Win32_Volume -ErrorAction SilentlyContinue | Where-Object { $_.Capacity -eq $expectedCapacity }
        if ($cimVols) {
            $vol = $cimVols | Select-Object -First 1
            $drive = if ($vol.DriveLetter) { $vol.DriveLetter.TrimEnd('\') } else { $null }
            $path = if ($drive) { "\\.\" + $drive } else { $vol.DeviceID.TrimEnd('\') }
            return @{
                Path = $path
                DriveLetter = $drive
                DeviceID = $vol.DeviceID
                Capacity = $vol.Capacity
                Source = "Win32_Volume"
            }
        }
    } catch {}

    # 2. Get-Volume
    try {
        $getVols = Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.Size -eq $expectedCapacity }
        if ($getVols) {
            $vol = $getVols | Select-Object -First 1
            $drive = if ($vol.DriveLetter) { $vol.DriveLetter + ":" } else { $null }
            $path = if ($drive) { "\\.\" + $drive } elseif ($vol.UniqueId) { $vol.UniqueId.TrimEnd('\') } else { $null }
            if ($path) {
                return @{
                    Path = $path
                    DriveLetter = $drive
                    DeviceID = $vol.UniqueId
                    Capacity = $vol.Size
                    Source = "Get-Volume"
                }
            }
        }
    } catch {}

    # 3. Direct probe on candidate drive letters via IOCTL_DISK_GET_LENGTH_INFO
    $candidateDrives = @('E:', 'F:', 'G:', 'D:', 'H:', 'I:')
    foreach ($dl in $candidateDrives) {
        $devPath = "\\.\" + $dl
        $h = [WinLvmVolIO]::OpenVolume($devPath)
        if ($h.ToInt64() -ne -1) {
            try {
                $len = [WinLvmVolIO]::GetVolumeLength($h)
                if ($len -eq $expectedCapacity) {
                    return @{
                        Path = $devPath
                        DriveLetter = $dl
                        DeviceID = $devPath
                        Capacity = $len
                        Source = "Direct-Probe ($dl)"
                    }
                }
            } finally {
                [WinLvmVolIO]::CloseHandle($h) | Out-Null
            }
        }
    }

    return $null
}

# 4. Volume Detection and Test Selection
Log "Step 2: Detecting target multi-PV volume(s)..."

$casesToRun = @()

if ($TargetDevice) {
    $devPath = Format-DevicePath $TargetDevice
    $h = [WinLvmVolIO]::OpenVolume($devPath)
    if ($h.ToInt64() -eq -1) {
        Log "RAW-OPEN: FAIL (err=$([System.Runtime.InteropServices.Marshal]::GetLastWin32Error()) Path=$devPath)"
        Log "VOLUME-DETECT: FAIL (Unable to open specified TargetDevice $devPath)"
        exit 1
    }

    $detectedLen = [WinLvmVolIO]::GetVolumeLength($h)
    [WinLvmVolIO]::CloseHandle($h) | Out-Null
    Log "TargetDevice $devPath reported capacity: $detectedLen bytes"

    $selectedCase = $null
    if ($TestCase -eq 'auto') {
        if ($detectedLen -eq 167772160) {
            $selectedCase = 'linear-2pv'
        } elseif ($detectedLen -eq 33554432) {
            $selectedCase = 'striped-2pv'
        } else {
            Log "VOLUME-DETECT: FAIL (TargetDevice capacity $detectedLen does not match linear-2pv (167772160) or striped-2pv (33554432))"
            exit 1
        }
    } else {
        $selectedCase = $TestCase
    }

    $tcDef = $TestCases[$selectedCase]
    $volInfo = @{
        Path = $devPath
        DriveLetter = if ($devPath -match '^\\\\\.\\([A-Za-z]:)$') { $Matches[1] } else { $null }
        DeviceID = $devPath
        Capacity = $detectedLen
        Source = "TargetDevice-Argument"
    }
    $casesToRun += @{ Definition = $tcDef; VolumeInfo = $volInfo }
} else {
    # Discovery loop
    $startTime = Get-Date
    $foundLinear = $null
    $foundStriped = $null

    for ($iter = 0; $iter -lt $DetectTimeoutSeconds; $iter++) {
        if ($TestCase -eq 'auto' -or $TestCase -eq 'linear-2pv' -or $TestCase -eq 'all') {
            if (-not $foundLinear) {
                $foundLinear = Find-VolumeInfo 167772160
            }
        }

        if ($TestCase -eq 'auto' -or $TestCase -eq 'striped-2pv' -or $TestCase -eq 'all') {
            if (-not $foundStriped) {
                $foundStriped = Find-VolumeInfo 33554432
            }
        }

        # Check termination condition for discovery
        if ($TestCase -eq 'linear-2pv' -and $foundLinear) { break }
        if ($TestCase -eq 'striped-2pv' -and $foundStriped) { break }
        if ($TestCase -eq 'all' -and $foundLinear -and $foundStriped) { break }
        if ($TestCase -eq 'auto' -and ($foundLinear -or $foundStriped)) { break }

        if ($iter -eq 0 -or $iter -eq 4) {
            Log "Probing: triggering IOCTL_WINLVM_REFRESH on \Device\WinLvmControl..."
            $refOk = [WinLvmVolIO]::TriggerRefresh()
            Log "TRIGGER-REFRESH: $refOk"
        }

        Start-Sleep -Seconds 1
    }

    if ($TestCase -eq 'linear-2pv' -or ($TestCase -eq 'auto' -and $foundLinear -and -not $foundStriped)) {
        if ($foundLinear) {
            $casesToRun += @{ Definition = $TestCases['linear-2pv']; VolumeInfo = $foundLinear }
        }
    } elseif ($TestCase -eq 'striped-2pv' -or ($TestCase -eq 'auto' -and $foundStriped -and -not $foundLinear)) {
        if ($foundStriped) {
            $casesToRun += @{ Definition = $TestCases['striped-2pv']; VolumeInfo = $foundStriped }
        }
    } elseif ($TestCase -eq 'all' -or ($TestCase -eq 'auto' -and $foundLinear -and $foundStriped)) {
        if ($foundLinear) {
            $casesToRun += @{ Definition = $TestCases['linear-2pv']; VolumeInfo = $foundLinear }
        }
        if ($foundStriped) {
            $casesToRun += @{ Definition = $TestCases['striped-2pv']; VolumeInfo = $foundStriped }
        }
    }
}

if ($casesToRun.Count -eq 0) {
    Log "VOLUME-DETECT: FAIL (No matching volume found for TestCase='$TestCase')"
    Log "Listing all volumes and mount points currently recognized by the system:"
    Get-CimInstance Win32_Volume -ErrorAction SilentlyContinue | ForEach-Object {
        Log "  CIM-Vol: DeviceID=$($_.DeviceID) DriveLetter=$($_.DriveLetter) Capacity=$($_.Capacity) FileSystem=$($_.FileSystem)"
    }
    $mvol = mountvol.exe 2>&1
    Log "MOUNTVOL:`n$($mvol | Out-String)"

    $childDev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "*WinLvm*" }
    if ($childDev) {
        $childDev | ForEach-Object { Log "  DEVNODE: $($_.InstanceId) Status=$($_.Status)" }
    }

    $klog = [WinLvmVolIO]::GetKernelLog()
    Log "KERNEL-LOG:`n$klog"

    Log "=== MULTI-PV TEST SUITE FAILED (NO MATCHING VOLUMES) ==="
    exit 1
}

# 5. Run Verification on Selected Cases
Log "Step 3: Running black-box verification on $($casesToRun.Count) volume case(s)..."

$allPassed = $true

foreach ($entry in $casesToRun) {
    $tc = $entry.Definition
    $volInfo = $entry.VolumeInfo
    $targetPath = $volInfo.Path
    $casePassed = $true

    Log "------------------------------------------------------------"
    Log "CASE: $($tc.Name) ($($tc.Description))"
    Log "VOLUME-DETECT: PASS Found volume at $targetPath (Capacity=$($volInfo.Capacity), Source=$($volInfo.Source))"

    # Open Volume Handle
    $h = [WinLvmVolIO]::OpenVolume($targetPath)
    if ($h.ToInt64() -eq -1) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Log "RAW-OPEN: FAIL (err=$err Path=$targetPath)"
        $casePassed = $false
        $allPassed = $false
        continue
    }

    Log "RAW-OPEN: PASS (Handle=$($h.ToInt64()) Path=$targetPath)"

    try {
        # 5a. Whole-Volume Streaming SHA256 Hash
        Log "Verifying whole volume SHA256 (streaming $($tc.Capacity) bytes)..."
        $wholeHash = [WinLvmVolIO]::StreamHash($h, $tc.Capacity)
        if ($wholeHash -eq $tc.ExpectedWholeHash) {
            Log "WHOLE-HASH: PASS got=$wholeHash"
        } else {
            Log "WHOLE-HASH: FAIL expected=$($tc.ExpectedWholeHash) got=$wholeHash"
            $casePassed = $false
        }

        # 5b. Window Hashes (Cross-Device Seam & Extent Boundaries)
        Log "Verifying $($tc.Windows.Count) specific window hashes..."
        foreach ($win in $tc.Windows) {
            $data = [WinLvmVolIO]::ReadWindow($h, $win.Off, $win.Len)
            $hsh = [WinLvmVolIO]::Sha256($data)
            if ($hsh -eq $win.Exp) {
                Log "WINDOW-HASH [$($win.Off)+$($win.Len)]: PASS ($hsh)"
            } else {
                Log "WINDOW-HASH [$($win.Off)+$($win.Len)]: FAIL expected=$($win.Exp) got=$hsh"
                $casePassed = $false
            }
        }

        # 5c. Filesystem / Payload Verification (hello.txt)
        Log "Verifying payload data (token: '$($tc.PayloadToken)')..."
        $payloadMatched = $false
        $payloadDetail = ""

        # Probe filesystem file via drive letters
        $probeDrives = @()
        if ($volInfo.DriveLetter) {
            $probeDrives += $volInfo.DriveLetter.TrimEnd('\')
        }
        $probeDrives += @("E:", "F:", "G:", "D:")
        $probeDrives = $probeDrives | Select-Object -Unique

        foreach ($dl in $probeDrives) {
            $helloPath = "$dl\hello.txt"
            if (Test-Path $helloPath) {
                try {
                    $content = Get-Content -Raw $helloPath -ErrorAction Stop
                    if ($content -like "*$($tc.PayloadToken)*") {
                        $payloadMatched = $true
                        $payloadDetail = "file '$helloPath' content='$($content.Trim())'"
                        break
                    }
                } catch {}
            }
        }

        # Fallback to direct raw sector read window
        if (-not $payloadMatched) {
            try {
                $rawBytes = [WinLvmVolIO]::ReadWindow($h, $tc.PayloadRawOffset, $tc.PayloadRawLength)
                $content = [System.Text.Encoding]::ASCII.GetString($rawBytes)
                if ($content -like "*$($tc.PayloadToken)*") {
                    $payloadMatched = $true
                    $payloadDetail = "raw offset $($tc.PayloadRawOffset) content='$($content.Trim())'"
                } else {
                    $payloadDetail = "raw offset $($tc.PayloadRawOffset) content='$content'"
                }
            } catch {
                $payloadDetail = "raw read exception: $_"
            }
        }

        if ($payloadMatched) {
            Log "PAYLOAD: PASS ($payloadDetail)"
        } else {
            Log "PAYLOAD: FAIL (expected '$($tc.PayloadToken)', got $payloadDetail)"
            $casePassed = $false
        }
    } catch {
        Log "IO-ERROR: FAIL (Unexpected exception: $_)"
        $casePassed = $false
    } finally {
        [WinLvmVolIO]::CloseHandle($h) | Out-Null
    }

    if ($casePassed) {
        Log "CASE-RESULT [$($tc.Name)]: PASS"
    } else {
        Log "CASE-RESULT [$($tc.Name)]: FAIL"
        $allPassed = $false
    }
}

# 6. Capture Kernel Log
$klog = [WinLvmVolIO]::GetKernelLog()
Log "KERNEL-LOG:`n$klog"

Log "------------------------------------------------------------"
if ($allPassed) {
    Log "ALL-TESTS: PASS (All multi-PV test cases succeeded)"
    Log "=== VERIFICATION FINISHED: SUCCESS ==="
    exit 0
} else {
    Log "ALL-TESTS: FAIL (One or more multi-PV test cases failed)"
    Log "=== VERIFICATION FINISHED: FAILURE ==="
    exit 1
}
