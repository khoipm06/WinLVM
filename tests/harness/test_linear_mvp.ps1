# test_linear_mvp.ps1 - Automated verification of Linear-1PV LVM2 volume on Windows
$ErrorActionPreference = 'Continue'
$LogFile = "C:\winlvm\linear_mvp.log"
if (Test-Path $LogFile) { Remove-Item -Force $LogFile }

function Log([string]$msg) {
    $ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'
    $line = "[$ts] $msg"
    Write-Output $line
    Add-Content -Path $LogFile -Value $line
}

Log "=== START WINLVM LINEAR-1PV VERIFICATION ==="

# 1. Certificate Trust
Log "Step 1: Adding certificate to Root and TrustedPublisher stores"
$cerPath = "C:\winlvm\driver\x64\Release\winlvm.cer"
if (-not (Test-Path $cerPath)) {
    $foundCer = Get-ChildItem -Path "C:\winlvm\driver" -Filter "*.cer" -Recurse | Select-Object -First 1
    if ($foundCer) { $cerPath = $foundCer.FullName }
}
if (Test-Path $cerPath) {
    certutil -addstore Root $cerPath | Out-Null
    certutil -addstore TrustedPublisher $cerPath | Out-Null
    Log "CERT-TRUST: PASS ($cerPath)"
} else {
    Log "CERT-TRUST: FAIL (Certificate not found at $cerPath)"
}

# 2. Package and stage winlvm-vol.inf BEFORE installing the bus
Log "Step 2: Packaging and staging winlvm-vol.inf"
$volPkg = "C:\winlvm\vol_pkg"
if (-not (Test-Path $volPkg)) { New-Item -ItemType Directory -Path $volPkg -Force | Out-Null }
Copy-Item -Path "C:\winlvm\driver\winlvm-vol.inf" -Destination $volPkg -Force

$inf2cat = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\Inf2Cat.exe"
$signtool = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"

$catOut = & $inf2cat /driver:$volPkg /os:10_X64 2>&1
Log "INF2CAT-OUTPUT: $($catOut | Out-String)"

$signOut = & $signtool sign /ph /fd sha256 /sha1 0AB516A704EFF016D4DBE37CEEDC255C7C8CF395 "$volPkg\winlvm-vol.cat" 2>&1
Log "SIGNTOOL-OUTPUT: $($signOut | Out-String)"

$addVolOut = pnputil.exe /add-driver "$volPkg\winlvm-vol.inf" /install 2>&1
Log "PNPUTIL-VOL-OUTPUT: $($addVolOut | Out-String)"

# 3. Driver Install (ROOT\winlvm)
Log "Step 3: Installing ROOT\winlvm via devcon"
$devcon = "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.26100.0\x64\devcon.exe"
$infPath = "C:\winlvm\driver\x64\Release\winlvm\winlvm.inf"
if (-not (Test-Path $infPath)) {
    $foundInf = Get-ChildItem -Path "C:\winlvm\driver" -Filter "winlvm.inf" -Recurse | Where-Object { $_.DirectoryName -like "*Release*" } | Select-Object -First 1
    if ($foundInf) { $infPath = $foundInf.FullName }
}

if (Test-Path $devcon) {
    & $devcon remove "ROOT\winlvm" 2>&1 | Out-Null
    $installOut = & $devcon install $infPath "ROOT\winlvm" 2>&1
    Log "DEVCON-INSTALL-OUTPUT: $($installOut | Out-String)"

    $statusOut = & $devcon status "ROOT\winlvm" 2>&1
    Log "DEVCON-BUS-STATUS: $($statusOut | Out-String)"

    $childStatus = & $devcon status "WinLvm\Volume*" 2>&1
    Log "DEVCON-CHILD-STATUS: $($childStatus | Out-String)"
} else {
    Log "DEVCON: FAIL (devcon.exe not found)"
}

# 4. Service Query
$scOut = sc.exe query winlvm 2>&1
Log "SC-QUERY-WINLVM: $($scOut | Out-String)"

# 5. Define WinLvmVolIO Helper
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
    public static extern bool ReadFile(IntPtr hFile, byte[] lpBuffer, uint nBytesToRead,
        out uint lpBytesRead, IntPtr lpOverlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern long SetFilePointer(IntPtr hFile, long lDistanceToMove,
        IntPtr lpDistanceToMoveHigh, uint dwMoveMethod);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeviceIoControl(IntPtr hDevice, uint dwIoControlCode,
        byte[] lpInBuffer, uint nInBufferSize,
        byte[] lpOutBuffer, uint nOutBufferSize,
        out uint lpBytesReturned, IntPtr lpOverlapped);

    public const uint GENERIC_READ = 0x80000000;
    public const uint GENERIC_WRITE = 0x40000000;
    public const uint FILE_SHARE_READ = 0x01;
    public const uint FILE_SHARE_WRITE = 0x02;
    public const uint OPEN_EXISTING = 3;
    public const uint FILE_ATTRIBUTE_NORMAL = 0x80;
    public const uint FILE_BEGIN = 0;

    public static IntPtr OpenVolume(string path) {
        return CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, IntPtr.Zero);
    }

    public static IntPtr OpenControl() {
        return CreateFile(@"\\.\WinLvmControl", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
    }

    public static string GetKernelLog() {
        IntPtr h = OpenControl();
        if (h.ToInt64() == -1) return "OpenControl failed err=" + Marshal.GetLastWin32Error();
        try {
            byte[] outBuf = new byte[65536];
            uint ret = 0;
            if (DeviceIoControl(h, 0x22601c, null, 0, outBuf, (uint)outBuf.Length, out ret, IntPtr.Zero)) {
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
            return DeviceIoControl(h, 0x22e004, inBuf, (uint)inBuf.Length, null, 0, out ret, IntPtr.Zero);
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

        SetFilePointer(hFile, baseOff, IntPtr.Zero, FILE_BEGIN);
        byte[] rawBuf = new byte[readLen];
        uint totalRead = 0;
        while (totalRead < (uint)readLen) {
            uint n;
            if (!ReadFile(hFile, rawBuf, (uint)readLen - totalRead, out n, IntPtr.Zero))
                throw new Exception("ReadFile failed err=" + Marshal.GetLastWin32Error());
            if (n == 0) throw new Exception("EOF prematurely encountered");
            totalRead += n;
        }

        byte[] winBuf = new byte[length];
        Array.Copy(rawBuf, delta, winBuf, 0, length);
        return winBuf;
    }

    public static string StreamHash(IntPtr hFile, long totalBytes) {
        SetFilePointer(hFile, 0, IntPtr.Zero, FILE_BEGIN);
        using (var sha = SHA256.Create()) {
            byte[] chunk = new byte[4194304];
            long totalRead = 0;
            while (totalRead < totalBytes) {
                int remaining = (int)Math.Min((long)chunk.Length, totalBytes - totalRead);
                uint n;
                if (!ReadFile(hFile, chunk, (uint)remaining, out n, IntPtr.Zero))
                    throw new Exception("StreamHash ReadFile failed err=" + Marshal.GetLastWin32Error());
                if (n == 0) throw new Exception("StreamHash unexpected EOF at " + totalRead);
                sha.TransformBlock(chunk, 0, (int)n, chunk, 0);
                totalRead += n;
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

# 6. Wait and Detect Mounted Volume
Log "Step 4: Detecting mounted volume"
$targetPath = $null
for ($i = 0; $i -lt 15; $i++) {
    Start-Sleep -Seconds 1

    $eVol = Get-CimInstance Win32_Volume | Where-Object { $_.DriveLetter -eq 'E:' }
    if ($eVol) {
        Log "VOLUME-DETECT: PASS Found volume on drive E: (DeviceID=$($eVol.DeviceID))"
        $targetPath = '\\.\E:'
        break
    }

    $cimVol = Get-CimInstance Win32_Volume | Where-Object { $_.Capacity -eq 16777216 }
    if ($cimVol) {
        Log "FOUND-CIM-VOLUME: DeviceID=$($cimVol.DeviceID) Capacity=$($cimVol.Capacity) DriveLetter=$($cimVol.DriveLetter)"
        if ($cimVol.DriveLetter) {
            $targetPath = "\\.\" + $cimVol.DriveLetter
        } else {
            $targetPath = $cimVol.DeviceID.TrimEnd('\')
        }
        Log "VOLUME-DETECT: PASS Found volume ($targetPath)"
        break
    }

    $vol = Get-Volume | Where-Object { $_.Size -eq 16777216 -and $_.DriveLetter }
    if ($vol) {
        $targetPath = "\\.\" + $vol.DriveLetter + ":"
        Log "VOLUME-DETECT: PASS Found volume on drive $($vol.DriveLetter): Size: $($vol.Size)"
        break
    }

    if ($i -eq 5) {
        Log "Re-probing: triggering IOCTL_WINLVM_REFRESH..."
        $refOk = [WinLvmVolIO]::TriggerRefresh()
        Log "TRIGGER-REFRESH: $refOk"
    }
}

$klog = [WinLvmVolIO]::GetKernelLog()
Log "KERNEL-LOG:`n$klog"

if (-not $targetPath) {
    Log "VOLUME-DETECT: Volume not yet mounted to drive letter. Listing all volumes and mount points:"
    Get-CimInstance Win32_Volume | ForEach-Object { Log "  CIM-Vol: DeviceID=$($_.DeviceID) DriveLetter=$($_.DriveLetter) Capacity=$($_.Capacity) FileSystem=$($_.FileSystem)" }
    $mvol = mountvol.exe 2>&1
    Log "MOUNTVOL:`n$($mvol | Out-String)"

    $childDev = Get-PnpDevice | Where-Object { $_.InstanceId -like "*6NSQ8P*" }
    if ($childDev) {
        Log "CHILD-DEVNODE-FOUND: $($childDev.InstanceId)"
    }
}

if ($targetPath) {
    Log "Step 5: Testing IO on $targetPath"
    $h = [WinLvmVolIO]::OpenVolume($targetPath)
    if ($h.ToInt64() -eq -1) {
        Log "RAW-OPEN: FAIL (err=$([System.Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    } else {
        Log "RAW-OPEN: PASS (Handle=$($h.ToInt64()))"

        # Check whole volume hash (16777216 bytes)
        $expectedWhole = "4184b3b80f669d5bddd80ec9aaad4d1e6aaa75e4a0fa63c5ec4479cca7b8021b"
        $wholeHash = [WinLvmVolIO]::StreamHash($h, 16777216)
        if ($wholeHash -eq $expectedWhole) {
            Log "WHOLE-HASH: PASS got=$wholeHash"
        } else {
            Log "WHOLE-HASH: FAIL expected=$expectedWhole got=$wholeHash"
        }

        # Check window hashes
        $windows = @(
            @{ Off = 0; Len = 131072; Exp = "e44889364628d980d6554a099af122db2e7826b0e4e233f327faa783ddfd38bb" },
            @{ Off = 5592405; Len = 131072; Exp = "fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471" },
            @{ Off = 8388608; Len = 131072; Exp = "a139ed6d165df05d9456ce1b5e6c3584486ee62d9926cf4a0fa7e8f90955f23e" },
            @{ Off = 16646144; Len = 131072; Exp = "fa43239bcee7b97ca62f007cc68487560a39e19f74f3dde7486db3f98df8e471" }
        )

        foreach ($win in $windows) {
            $data = [WinLvmVolIO]::ReadWindow($h, $win.Off, $win.Len)
            $hsh = [WinLvmVolIO]::Sha256($data)
            if ($hsh -eq $win.Exp) {
                Log "WINDOW-HASH [$($win.Off)+$($win.Len)]: PASS ($hsh)"
            } else {
                Log "WINDOW-HASH [$($win.Off)+$($win.Len)]: FAIL expected=$($win.Exp) got=$hsh"
            }
        }

        # 7. Payload Verification (hello.txt)
        Log "Step 6: Checking Filesystem Payload (hello.txt)"
        $helloPath = "E:\hello.txt"
        if (Test-Path $helloPath) {
            $content = Get-Content -Raw $helloPath
            Log "FILE-HELLO-TXT: PASS content='$($content.Trim())'"
        } else {
            $rawBytes = [WinLvmVolIO]::ReadWindow($h, 9962496, 127)
            $content = [System.Text.Encoding]::ASCII.GetString($rawBytes)
            if ($content -like "*Hello from WinLVM corpus generator*") {
                Log "FILE-HELLO-TXT: PASS content='$($content.Trim())'"
            } else {
                Log "FILE-HELLO-TXT: FAIL (Content mismatch: $content)"
            }
        }

        [WinLvmVolIO]::CloseHandle($h) | Out-Null
    }
}

Log "=== VERIFICATION FINISHED ==="
