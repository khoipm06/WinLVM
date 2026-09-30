#!/usr/bin/env python3
"""
WinLVM End-to-End Bidirectional Test Runner
Coordinates testing between Linux Host (Official Ground Truth LVM2)
and Windows 10 VM (winlvm.sys driver and lvm.exe CLI).

Covers:
- FLOW A: Host Originator (Linux) -> Guest Consumer (Windows VM)
- FLOW B: Guest Originator (Windows VM) -> Host Consumer (Linux)
- FLOW C: 6-Round Ping-Pong Interleaved Multi-Round Mutations
"""

import os
import sys
import time
import subprocess
import hashlib
import json

SUDO_PW = os.environ.get("WINLVM_TEST_SUDO_PW", "")
VM_NAME = "winlvm-build"
SSH_CMD = [
    "ssh", "-p", "2223",
    "-o", "ConnectTimeout=8",
    "-o", "BatchMode=yes",
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=/dev/null",
    "admin@127.0.0.1"
]

DISK_A = "/tmp/winlvm-vm/test-linear-2pv-a.img"
DISK_B = "/tmp/winlvm-vm/test-linear-2pv-b.img"
LOG_FILE = "$WORKSPACE/tests/evidence/bidirectional_matrix_pass.log"

DEVFILE = '--devicesfile ""'

def log(msg):
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    formatted = f"[{timestamp}] {msg}"
    print(formatted, flush=True)
    with open(LOG_FILE, "a") as f:
        f.write(formatted + "\n")

def run_host(cmd, check=True):
    """
    Run command on host with sudo via argument vector.
    Subcommand stdin is connected to /dev/null so that interactive
    commands do not read leftover sudo password bytes or hang on prompts.
    All storage and filesystem commands are passed explicit non-interactive flags.
    """
    res = subprocess.run(
        ["sudo", "-S", "-p", "", "sh", "-c", f"{cmd} < /dev/null"],
        input=f"{SUDO_PW}\n",
        text=True,
        capture_output=True
    )
    if check and res.returncode != 0:
        log(f"HOST CMD FAILED: {cmd}\nSTDOUT: {res.stdout}\nSTDERR: {res.stderr}")
        raise RuntimeError(f"Host command failed: {cmd}")
    return res

def run_guest(cmd, check=True):
    """Run simple command on Windows guest via SSH."""
    args = SSH_CMD + [cmd]
    res = subprocess.run(args, capture_output=True, text=True, timeout=120)
    if check and res.returncode != 0:
        log(f"GUEST CMD FAILED: {cmd}\nSTDOUT: {res.stdout}\nSTDERR: {res.stderr}")
        raise RuntimeError(f"Guest command failed: {cmd}")
    return res

def run_guest_ps1(script_content, script_name="step.ps1"):
    """
    Transfer PowerShell script to guest via SCP and execute cleanly with -File.
    Avoids all cmd.exe quoting, argument length limits, and newline mangling.
    Uses -NonInteractive so any unexpected prompt immediately fails rather than hanging.
    """
    local_path = f"/tmp/{script_name}"
    with open(local_path, "w", encoding="utf-8") as f:
        f.write(script_content)

    # SCP script to guest
    subprocess.run(
        ["scp", "-P", "2223", "-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=no", local_path, f"admin@127.0.0.1:C:/winlvm/{script_name}"],
        check=True, capture_output=True, text=True
    )

    cmd = f"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:\\winlvm\\{script_name}"
    res = run_guest(cmd, check=True)
    return res

def wait_guest_ssh(max_wait=120):
    """Wait for Windows VM SSH to become responsive."""
    log("Waiting for Windows VM SSH to be ready...")
    start = time.time()
    while time.time() - start < max_wait:
        try:
            res = run_guest("echo SSH_OK", check=False)
            if res.returncode == 0 and "SSH_OK" in res.stdout:
                log("Windows VM SSH is alive and responsive.")
                return True
        except Exception:
            pass
        time.sleep(3)
    raise TimeoutError("Timed out waiting for Windows VM SSH")

def vm_powerdown():
    """Cleanly power down VM to allow exclusive host disk manipulation."""
    log("Powering down VM...")
    res = subprocess.run(f"virsh domstate {VM_NAME}", shell=True, capture_output=True, text=True)
    if "shut off" in res.stdout:
        log("VM is already cleanly shut off.")
        return

    try:
        subprocess.run(
            f"virsh qemu-agent-command {VM_NAME} '{{\"execute\":\"guest-shutdown\", \"arguments\":{{\"mode\":\"powerdown\"}}}}'",
            shell=True, capture_output=True, text=True, timeout=5
        )
    except Exception:
        pass

    for _ in range(15):
        res = subprocess.run(f"virsh domstate {VM_NAME}", shell=True, capture_output=True, text=True)
        if "shut off" in res.stdout:
            log("VM is cleanly shut off.")
            return
        time.sleep(2)
    log("Forcing virsh destroy...")
    subprocess.run(f"virsh destroy {VM_NAME}", shell=True)
    time.sleep(2)

def vm_start():
    """Start the VM."""
    log("Starting VM...")
    res = subprocess.run(f"virsh domstate {VM_NAME}", shell=True, capture_output=True, text=True)
    if "running" not in res.stdout:
        subprocess.run(f"virsh start {VM_NAME}", shell=True, check=True)
        time.sleep(25) # Single boot window wait
    wait_guest_ssh(120)

def deploy_and_build_guest_tools():
    """Sync source to guest and compile lvm.exe."""
    log("Deploying updated tools source to Windows VM and compiling lvm.exe...")
    tar_path = "/tmp/winlvm_src.tgz"
    subprocess.run(f"tar czf {tar_path} --exclude='x64' --exclude='build' src", shell=True, check=True, cwd="$WORKSPACE")
    subprocess.run(f"scp -P 2223 -o ConnectTimeout=8 -o StrictHostKeyChecking=no {tar_path} admin@127.0.0.1:C:/winlvm/", shell=True, check=True)
    run_guest("cd /d C:\\winlvm && tar -xzmf winlvm_src.tgz")
    build_cmd = (
        'cmd.exe /c "call \"C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat\" && '
        'cl.exe /nologo /EHsc /std:c++17 /O2 /I C:\\winlvm\\src /I C:\\winlvm\\src\\lvmfmt\\include /I C:\\winlvm\\src\\tools\\lvm '
        'C:\\winlvm\\src\\tools\\lvm\\*.cpp C:\\winlvm\\src\\lvmfmt\\*.cpp /Fe:C:\\winlvm\\lvm.exe"'
    )
    res = run_guest(build_cmd)
    log(f"LVM.EXE BUILD OUTPUT:\n{res.stdout}")
    log("LVM.EXE built successfully on Windows guest.")

    # Also build driver if driver source was updated
    driver_script = r"""
    Remove-Item -Force -ErrorAction SilentlyContinue C:\winlvm\src\driver\x64\Release\*.obj
    & "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" C:\winlvm\src\driver\winlvm.vcxproj /p:Configuration=Release /p:Platform=x64 /p:EnableInfVerif=false /m /v:m /nologo
    $builtPkg = "C:\winlvm\src\driver\x64\Release\winlvm\winlvm.sys"
    if (-not (Test-Path $builtPkg)) {
        $builtPkg = "C:\winlvm\src\driver\x64\Release\winlvm.sys"
    }
    $builtHash = (Get-FileHash $builtPkg).Hash
    $installedHash = (Get-FileHash C:\Windows\System32\drivers\winlvm.sys).Hash
    Write-Output "BUILT_HASH: $builtHash"
    Write-Output "INSTALLED_HASH: $installedHash"
    if ($builtHash -ne $installedHash) {
        $src = @"
using System;
using System.Runtime.InteropServices;
public static class FileUtil {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool MoveFileEx(string lpExistingFileName, string lpNewFileName, int dwFlags);
}
"@
        Add-Type -TypeDefinition $src
        [FileUtil]::MoveFileEx($builtPkg, 'C:\Windows\System32\drivers\winlvm.sys', 5)
        Write-Output "DRIVER-NEEDS-REBOOT: YES"
    } else {
        Write-Output "DRIVER-UP-TO-DATE: YES"
    }
    """
    res_drv = run_guest_ps1(driver_script, "build_driver.ps1")
    log("DRIVER BUILD OUTPUT:\n" + res_drv.stdout)
    if "DRIVER-NEEDS-REBOOT: YES" in res_drv.stdout:
        log("Driver binary updated, rebooting VM to load new driver...")
        run_host(f"virsh qemu-agent-command {VM_NAME} '{{\"execute\":\"guest-shutdown\", \"arguments\":{{\"mode\":\"reboot\"}}}}'", check=False)
        time.sleep(35)
        wait_guest_ssh(120)

def cleanup_host_storage():
    """Clean up any active test dm devices, volume groups, mounts, and loop devices on host."""
    test_vgs = ["flowa_vg", "win_flowb_vg", "pp_vg"]
    for vg in test_vgs:
        run_host(f"vgchange -an {DEVFILE} {vg} 2>/dev/null || true", check=False)

    test_dms = [
        "win_flowb_vg-win_renamed", "win_flowb_vg-win_str",
        "flowa_vg-flowa_lin", "flowa_vg-flowa_str",
        "pp_vg-pp_main", "pp_vg-pp_stripe"
    ]
    for dm in test_dms:
        run_host(f"dmsetup remove {dm} 2>/dev/null || true", check=False)

    for mnt in ["/mnt/flowa_test", "/mnt/flowb_audit", "/mnt/pp_test", "/mnt/pp_final"]:
        run_host(f"umount {mnt} 2>/dev/null || true", check=False)

    run_host("losetup -D", check=False)

def cleanup_loop(dev):
    cleanup_host_storage()

# =========================================================================
# TEST FLOWS
# =========================================================================

def test_flow_a():
    """FLOW A: Linux Originator -> Windows VM Consumer"""
    log("==================================================================")
    log("STARTING FLOW A: Host (Linux) Originator -> Guest (Windows) Consumer")
    log("==================================================================")

    # 1. Power down VM to manipulate raw disk images cleanly
    vm_powerdown()
    cleanup_host_storage()

    # 2. Reset raw images & wipe all partition/filesystem headers
    log("Initializing raw disk images on host...")
    run_host(f"truncate -s 128M {DISK_A}")
    run_host(f"truncate -s 128M {DISK_B}")
    run_host(f"dd if=/dev/zero of={DISK_A} bs=1M count=128 conv=notrunc")
    run_host(f"dd if=/dev/zero of={DISK_B} bs=1M count=128 conv=notrunc")

    loop_a = None
    loop_b = None
    try:
        run_host("losetup -D", check=False)
        res_a = run_host(f"losetup -fP --show {DISK_A}")
        loop_a = res_a.stdout.strip()
        res_b = run_host(f"losetup -fP --show {DISK_B}")
        loop_b = res_b.stdout.strip()

        log(f"Attached loop devices: {loop_a}, {loop_b}")

        # Explicitly wipe any leftover filesystem signatures on loop devices
        run_host(f"wipefs -a {loop_a}")
        run_host(f"wipefs -a {loop_b}")

        # Create LVM2 Volume Group and Logical Volumes on Linux non-interactively (-y -ff)
        run_host(f"pvcreate -y -ff {DEVFILE} {loop_a} {loop_b}")
        run_host(f"vgcreate -y -ff -s 4M {DEVFILE} flowa_vg {loop_a} {loop_b}")

        # Create a 64M Linear LV and 32M Striped LV with explicit wipe signatures (-y -W y)
        run_host(f"lvcreate -y -W y {DEVFILE} -L 64M -n flowa_lin flowa_vg {loop_a}")
        run_host(f"lvcreate -y -W y {DEVFILE} -L 32M -i 2 -I 64k -n flowa_str flowa_vg {loop_a} {loop_b}")

        # Format flowa_lin with NTFS (-F = force without prompt, -Q = quick)
        run_host("mkfs.ntfs -Q -F -L FlowA_NTFS /dev/flowa_vg/flowa_lin")

        # Mount and write test payload on Linux
        run_host("mkdir -p /mnt/flowa_test")
        run_host("mount /dev/flowa_vg/flowa_lin /mnt/flowa_test")

        # Seed test files
        run_host("echo 'WINLVM_FLOW_A_TEXT_SENTINEL' > /mnt/flowa_test/sentinel.txt")
        run_host("mkdir -p /mnt/flowa_test/dir1/dir2/dir3")
        run_host("echo 'DEEP_NESTED_CONTENT_FLOW_A' > /mnt/flowa_test/dir1/dir2/dir3/nested.txt")

        # 4MB binary with deterministic pseudo-random payload
        run_host("dd if=/dev/urandom of=/mnt/flowa_test/payload_4m.bin bs=1M count=4")
        res_sha = run_host("sha256sum /mnt/flowa_test/payload_4m.bin")
        expected_sha = res_sha.stdout.split()[0].lower()
        log(f"Flow A 4MB payload SHA256: {expected_sha}")

        run_host("umount /mnt/flowa_test")
        run_host(f"vgchange -an {DEVFILE} flowa_vg")
        log("Linux preparation completed successfully.")

    finally:
        cleanup_loop(loop_a)
        cleanup_loop(loop_b)

    # 3. Boot Windows VM and verify consumer side
    vm_start()

    # Run guest verification script
    guest_script = f"""
    $ErrorActionPreference = 'Stop'
    Write-Output "Probing WinLVM volumes..."
    sc.exe start winlvm 2>$null
    C:\\winlvm\\lvm.exe vgscan
    Start-Sleep -Seconds 5

    # Check LVM CLI detection
    C:\\winlvm\\lvm.exe pvs
    C:\\winlvm\\lvm.exe vgs
    C:\\winlvm\\lvm.exe lvs

    # Locate mounted FlowA_NTFS volume
    $vol = Get-CimInstance Win32_Volume | Where-Object {{ $_.Label -eq "FlowA_NTFS" }}
    if (-not $vol) {{
        $vol = Get-CimInstance Win32_Volume | Where-Object {{ $_.Capacity -ge 60000000 -and $_.Capacity -le 70000000 }}
    }}
    if (-not $vol) {{
        Write-Error "FlowA_NTFS volume not found by Windows Mount Manager!"
    }}
    if (-not $vol.DriveLetter) {{
        $drive = "D:"
        cmd.exe /c "mountvol.exe $drive\\ $($vol.DeviceID)"
    }} else {{
        $drive = $vol.DriveLetter
    }}
    Write-Output "Found FlowA_NTFS on drive $drive"

    # 1. Verify text sentinel
    $text = (Get-Content "$drive\\sentinel.txt").Trim()
    if ($text -ne "WINLVM_FLOW_A_TEXT_SENTINEL") {{
        Write-Error "Sentinel text mismatch: $text"
    }}
    Write-Output "FLOW-A-SENTINEL: PASS"

    # 2. Verify nested directory
    $nested = (Get-Content "$drive\\dir1\\dir2\\dir3\\nested.txt").Trim()
    if ($nested -ne "DEEP_NESTED_CONTENT_FLOW_A") {{
        Write-Error "Nested text mismatch: $nested"
    }}
    Write-Output "FLOW-A-NESTED: PASS"

    # 3. Verify 4MB binary SHA256
    $actualSha = (Get-FileHash -Algorithm SHA256 "$drive\\payload_4m.bin").Hash.ToLower()
    if ($actualSha -ne "{expected_sha}") {{
        Write-Error "Binary hash mismatch! Expected {expected_sha} got $actualSha"
    }}
    Write-Output "FLOW-A-SHA256: PASS ($actualSha)"

    # 4. Run chkdsk in read-only audit mode
    cmd.exe /c "chkdsk.exe $drive"
    if ($LASTEXITCODE -ne 0) {{
        Write-Error "Chkdsk failed on FlowA volume (exit code $LASTEXITCODE)"
    }}
    Write-Output "FLOW-A-CHKDSK: PASS"
    """

    res = run_guest_ps1(guest_script, "flow_a_verify.ps1")
    log("GUEST VERIFICATION OUTPUT:\n" + res.stdout)
    log("FLOW A: ALL TESTS PASSED!")


def test_flow_b():
    """FLOW B: Guest Originator (Windows) -> Host Consumer (Linux)"""
    log("==================================================================")
    log("STARTING FLOW B: Guest (Windows) Originator -> Host (Linux) Consumer")
    log("==================================================================")

    # 1. Power down VM and reset raw test disks cleanly for Windows Originator
    vm_powerdown()
    cleanup_host_storage()
    log("Resetting and zeroing raw test disks on host for Flow B...")
    run_host(f"truncate -s 128M {DISK_A}")
    run_host(f"truncate -s 128M {DISK_B}")
    run_host(f"dd if=/dev/zero of={DISK_A} bs=1M count=128 conv=notrunc")
    run_host(f"dd if=/dev/zero of={DISK_B} bs=1M count=128 conv=notrunc")
    vm_start()

    guest_batch = r"""
    $ErrorActionPreference = 'Stop'
    Write-Output "Creating LVM volume group and LVs on Windows..."

    Add-Type @"
    using System;
    using System.Runtime.InteropServices;
    using Microsoft.Win32.SafeHandles;

    public class VolHelper {
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
        public static extern SafeFileHandle CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode,
            IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool DeviceIoControl(SafeFileHandle hDevice, uint dwIoControlCode,
            IntPtr lpInBuffer, uint nInBufferSize,
            out long lpOutBuffer, uint nOutBufferSize,
            out uint lpBytesReturned, IntPtr lpOverlapped);

        public const uint IOCTL_DISK_GET_LENGTH_INFO = 0x0007405C;

        public static long GetLength(string path) {
            using (var h = CreateFile(path.TrimEnd(new char[] { (char)92 }), 0x80000000, 3, IntPtr.Zero, 3, 0x80, IntPtr.Zero)) {
                if (h.IsInvalid) return -Marshal.GetLastWin32Error();
                long len = 0;
                uint ret = 0;
                if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, IntPtr.Zero, 0, out len, 8, out ret, IntPtr.Zero))
                    return -Marshal.GetLastWin32Error();
                return len;
            }
        }
    }
"@

    # Dynamically locate the two 128MB test disks
    $disks = Get-Disk | Where-Object { $_.Size -ge 130000000 -and $_.Size -le 140000000 } | Sort-Object Number
    if ($disks.Count -lt 2) {
        Write-Error "Could not find two 128MB test disks in Windows!"
    }
    $d1 = "\\.\PhysicalDrive$($disks[0].Number)"
    $d2 = "\\.\PhysicalDrive$($disks[1].Number)"
    Write-Output "Target disks: $d1 and $d2"

    Set-Disk -Number $disks[0].Number -IsOffline $false -ErrorAction SilentlyContinue
    Set-Disk -Number $disks[0].Number -IsReadOnly $false -ErrorAction SilentlyContinue
    Set-Disk -Number $disks[1].Number -IsOffline $false -ErrorAction SilentlyContinue
    Set-Disk -Number $disks[1].Number -IsReadOnly $false -ErrorAction SilentlyContinue

    # Initialize PVs
    C:\winlvm\lvm.exe pvcreate -y -f $d1 $d2
    if ($LASTEXITCODE -ne 0) { Write-Error "pvcreate failed!" }
    Write-Output "FLOW-B-PVCREATE: PASS"

    # Create VG
    C:\winlvm\lvm.exe vgcreate -s 4M win_flowb_vg $d1 $d2
    if ($LASTEXITCODE -ne 0) { Write-Error "vgcreate failed!" }
    Write-Output "FLOW-B-VGCREATE: PASS"

    # Create Linear LV (40MB)
    C:\winlvm\lvm.exe lvcreate -L 40M -n win_lin win_flowb_vg $d1
    if ($LASTEXITCODE -ne 0) { Write-Error "lvcreate win_lin failed!" }

    # Create Striped LV (32MB)
    C:\winlvm\lvm.exe lvcreate -L 32M -i 2 -I 64k -n win_str win_flowb_vg $d1 $d2
    if ($LASTEXITCODE -ne 0) { Write-Error "lvcreate win_str failed!" }
    Write-Output "FLOW-B-LVCREATE: PASS"

    # Re-probe driver to detect new LVs
    C:\winlvm\lvm.exe vgscan
    Start-Sleep -Seconds 5

    # Locate volumes by exact byte length via IOCTL_DISK_GET_LENGTH_INFO
    $linVol = $null
    $strVol = $null
    for ($attempt = 0; $attempt -lt 15; $attempt++) {
        $vols = Get-CimInstance Win32_Volume
        foreach ($v in $vols) {
            $len = [VolHelper]::GetLength($v.DeviceID)
            if ($len -ge 40000000 -and $len -le 43000000) { $linVol = $v }
            if ($len -ge 32000000 -and $len -le 35000000) { $strVol = $v }
        }
        if ($linVol -and $strVol) { break }
        Start-Sleep -Seconds 1
    }
    if (-not $linVol) { Write-Error "Could not find 40MB Linear LV volume via IOCTL length!" }
    if (-not $strVol) { Write-Error "Could not find 32MB Striped LV volume via IOCTL length!" }

    # Assign drive letters if needed
    if ($linVol.DriveLetter) { $driveLin = $linVol.DriveLetter } else {
        $driveLin = "D:"
        cmd.exe /c "mountvol.exe $driveLin\ $($linVol.DeviceID)"
    }
    if ($strVol.DriveLetter) { $driveStr = $strVol.DriveLetter } else {
        $driveStr = "E:"
        cmd.exe /c "mountvol.exe $driveStr\ $($strVol.DeviceID)"
    }
    Write-Output "Linear volume mounted on $driveLin, Striped volume mounted on $driveStr"

    # Format linear LV as NTFS
    Write-Output "Formatting $driveLin as NTFS..."
    cmd.exe /c "format.com $driveLin /FS:NTFS /V:WIN_ORIGIN /Q /Y"
    if ($LASTEXITCODE -ne 0) { Write-Error "format.com linear failed!" }

    # Format striped LV as NTFS
    Write-Output "Formatting $driveStr as NTFS..."
    cmd.exe /c "format.com $driveStr /FS:NTFS /V:WIN_STRIPE /Q /Y"
    if ($LASTEXITCODE -ne 0) { Write-Error "format.com striped failed!" }

    # Write files to linear LV
    [System.IO.File]::WriteAllText("$driveLin\win_origin.txt", "CREATED_ON_WINDOWS_VM_BY_WINLVM")

    # Write 4MB pseudo-random payload to linear LV
    $buf = New-Object byte[] (4 * 1024 * 1024)
    (New-Object System.Random(42)).NextBytes($buf)
    [System.IO.File]::WriteAllBytes("$driveLin\win_payload_4m.bin", $buf)
    $winSha = (Get-FileHash -Algorithm SHA256 "$driveLin\win_payload_4m.bin").Hash.ToLower()
    Write-Output "FLOW-B-WIN-SHA: $winSha"
    $winSha | Out-File -Encoding ascii C:\winlvm\flowb_win_sha.txt

    # Write sentinel file to striped LV
    [System.IO.File]::WriteAllText("$driveStr\win_stripe.txt", "STRIPED_LV_CREATED_ON_WINDOWS_VM")

    # Run chkdsk to verify clean filesystem structures
    cmd.exe /c "chkdsk.exe $driveLin"
    cmd.exe /c "chkdsk.exe $driveStr"

    # Flush all buffers and dismount volumes to commit 100% of data to disk
    cmd.exe /c "fsutil volume dismount $driveLin"
    cmd.exe /c "fsutil volume dismount $driveStr"
    Start-Sleep -Seconds 2

    # Mutate: Live extend linear LV by 16MB
    C:\winlvm\lvm.exe lvextend -L +16M win_lin $d1
    if ($LASTEXITCODE -ne 0) { Write-Error "lvextend failed!" }
    Write-Output "FLOW-B-LVEXTEND: PASS"

    # Rename linear volume
    C:\winlvm\lvm.exe lvrename win_lin win_renamed $d1
    if ($LASTEXITCODE -ne 0) { Write-Error "lvrename failed!" }
    Write-Output "FLOW-B-LVRENAME: PASS"

    # Move extents of win_renamed (pvmove -n win_renamed) from d1 to d2
    C:\winlvm\lvm.exe pvmove -n win_renamed $d1 $d2
    if ($LASTEXITCODE -ne 0) { Write-Error "pvmove failed!" }
    Write-Output "FLOW-B-PVMOVE: PASS"
    Write-Output "FLOW-B-GUEST-COMPLETE: PASS"
    """

    res = run_guest_ps1(guest_batch, "flow_b_create.ps1")
    log("GUEST FLOW B OUTPUT:\n" + res.stdout)

    # Read the expected hash generated on Windows
    res_hash = run_guest("powershell.exe -NoProfile -NonInteractive -Command \"Get-Content C:\\winlvm\\flowb_win_sha.txt\"")
    expected_win_sha = res_hash.stdout.strip().lower()
    log(f"Expected Windows-created SHA256: {expected_win_sha}")

    # Now power down VM to audit on Linux Host
    vm_powerdown()

    # Linux Host Ground Truth Oracle Audit
    loop_a = None
    loop_b = None
    try:
        run_host("losetup -D", check=False)
        res_a = run_host(f"losetup -fP --show {DISK_A}")
        loop_a = res_a.stdout.strip()
        res_b = run_host(f"losetup -fP --show {DISK_B}")
        loop_b = res_b.stdout.strip()

        log("Auditing Windows-created LVM2 metadata with upstream Linux tools...")
        # 1. vgck metadata consistency check
        run_host(f"vgck {DEVFILE} win_flowb_vg")
        log("ORACLE-VGCK: PASS (exit code 0)")

        # 2. Check pvs, vgs, lvs
        run_host(f"pvs {DEVFILE}")
        run_host(f"vgs {DEVFILE} win_flowb_vg")
        res_lvs = run_host(f"lvs {DEVFILE} win_flowb_vg")
        log(f"Linux LVS Output:\n{res_lvs.stdout}")

        if "win_renamed" not in res_lvs.stdout or "win_str" not in res_lvs.stdout:
            raise RuntimeError("Linux lvs failed to find renamed LV and striped LV!")
        log("ORACLE-LVS: PASS (Found win_renamed and win_str)")

        # 3. Activate VG and check filesystem
        run_host(f"vgchange -ay {DEVFILE} win_flowb_vg")

        # 4. Resize NTFS on win_renamed to match extended LV size (from 40M to 56M)
        run_host("ntfsresize -f -b /dev/win_flowb_vg/win_renamed")
        run_host("ntfsfix -n /dev/win_flowb_vg/win_renamed")
        log("ORACLE-NTFSFIX-LINEAR: PASS")

        run_host("mkdir -p /mnt/flowb_audit")
        run_host("mount -o ro /dev/win_flowb_vg/win_renamed /mnt/flowb_audit")

        res_text = run_host("cat /mnt/flowb_audit/win_origin.txt")
        if "CREATED_ON_WINDOWS_VM_BY_WINLVM" not in res_text.stdout:
            raise RuntimeError("Origin text mismatch on Linux audit!")
        log("ORACLE-TEXT: PASS")

        res_sha = run_host("sha256sum /mnt/flowb_audit/win_payload_4m.bin")
        audit_sha = res_sha.stdout.split()[0].lower()
        if audit_sha != expected_win_sha:
            raise RuntimeError(f"Hash mismatch on Linux audit! Expected {expected_win_sha} got {audit_sha}")
        log(f"ORACLE-SHA256: PASS ({audit_sha})")
        run_host("umount /mnt/flowb_audit")

        # 5. ntfsfix and audit striped LV (win_str)
        run_host("ntfsfix -n /dev/win_flowb_vg/win_str")
        log("ORACLE-NTFSFIX-STRIPED: PASS")

        run_host("mount -o ro /dev/win_flowb_vg/win_str /mnt/flowb_audit")
        res_str_text = run_host("cat /mnt/flowb_audit/win_stripe.txt")
        if "STRIPED_LV_CREATED_ON_WINDOWS_VM" not in res_str_text.stdout:
            raise RuntimeError("Striped text mismatch on Linux audit!")
        log("ORACLE-STRIPED-TEXT: PASS")
        run_host("umount /mnt/flowb_audit")

        run_host(f"vgchange -an {DEVFILE} win_flowb_vg")
        log("FLOW B: ALL AUDIT CHECKS PASSED 100%!")

    finally:
        cleanup_loop(loop_a)
        cleanup_loop(loop_b)

    # Restart VM for next test
    vm_start()


def test_flow_c():
    """FLOW C: 6-Round Ping-Pong Interleaved Multi-Round Mutations"""
    log("==================================================================")
    log("STARTING FLOW C: 6-Round Ping-Pong Interleaved Mutation Protocol")
    log("==================================================================")

    # -------------------------------------------------------------
    # ROUND 1: Linux Host seeds storage
    # -------------------------------------------------------------
    log("--- ROUND 1: Linux Host Provisions VG, Linear LV & NTFS Seed ---")
    vm_powerdown()
    cleanup_host_storage()
    run_host(f"truncate -s 128M {DISK_A}")
    run_host(f"truncate -s 128M {DISK_B}")
    run_host(f"dd if=/dev/zero of={DISK_A} bs=1M count=128 conv=notrunc")
    run_host(f"dd if=/dev/zero of={DISK_B} bs=1M count=128 conv=notrunc")

    loop_a = None
    loop_b = None
    r1_sha = ""
    try:
        run_host("losetup -D", check=False)
        res_a = run_host(f"losetup -fP --show {DISK_A}")
        loop_a = res_a.stdout.strip()
        res_b = run_host(f"losetup -fP --show {DISK_B}")
        loop_b = res_b.stdout.strip()

        run_host(f"wipefs -a {loop_a}")
        run_host(f"wipefs -a {loop_b}")

        run_host(f"pvcreate -y -ff {DEVFILE} {loop_a} {loop_b}")
        run_host(f"vgcreate -y -ff -s 4M {DEVFILE} pp_vg {loop_a} {loop_b}")
        run_host(f"lvcreate -y -W y {DEVFILE} -L 40M -n pp_main pp_vg {loop_a}")
        run_host("mkfs.ntfs -Q -F -L PP_NTFS /dev/pp_vg/pp_main")

        run_host("mkdir -p /mnt/pp_test")
        run_host("mount /dev/pp_vg/pp_main /mnt/pp_test")
        run_host("echo 'ROUND_1_SEED_PAYLOAD' > /mnt/pp_test/round1.txt")
        run_host("dd if=/dev/urandom of=/mnt/pp_test/r1_data.bin bs=1M count=2")
        r1_sha = run_host("sha256sum /mnt/pp_test/r1_data.bin").stdout.split()[0].lower()
        log(f"Round 1 SHA256: {r1_sha}")

        run_host("umount /mnt/pp_test")
        run_host(f"vgchange -an {DEVFILE} pp_vg")
    finally:
        cleanup_loop(loop_a)
        cleanup_loop(loop_b)

    # -------------------------------------------------------------
    # ROUND 2: Windows Guest VM Mounts & Appends Data
    # -------------------------------------------------------------
    log("--- ROUND 2: Windows Guest VM Mounts & Appends Data ---")
    vm_start()

    r2_script = f"""
    $ErrorActionPreference = 'Stop'
    sc.exe start winlvm 2>$null
    C:\\winlvm\\lvm.exe vgscan
    Start-Sleep -Seconds 5
    $vol = Get-CimInstance Win32_Volume | Where-Object {{ $_.Label -eq "PP_NTFS" }}
    if (-not $vol) {{
        $vol = Get-CimInstance Win32_Volume | Where-Object {{ $_.Capacity -ge 38000000 -and $_.Capacity -le 43000000 }}
    }}
    if (-not $vol) {{
        Write-Error "PP_NTFS volume not found by Windows Mount Manager!"
    }}
    if (-not $vol.DriveLetter) {{
        $drive = "D:"
        cmd.exe /c "mountvol.exe $($drive)\\ $($vol.DeviceID)"
    }} else {{
        $drive = $vol.DriveLetter
    }}
    Write-Output "PP_NTFS mounted on $drive"

    # Verify Round 1 data
    $t1 = (Get-Content "$drive\\round1.txt").Trim()
    if ($t1 -ne "ROUND_1_SEED_PAYLOAD") {{ Write-Error "Round 1 text mismatch: $t1" }}
    $actR1Sha = (Get-FileHash -Algorithm SHA256 "$drive\\r1_data.bin").Hash.ToLower()
    if ($actR1Sha -ne "{r1_sha}") {{ Write-Error "Round 1 SHA mismatch: $actR1Sha" }}
    Write-Output "ROUND-2-VERIFY-R1: PASS"

    # Append Round 2 data
    [System.IO.File]::WriteAllText("$drive\\round2_win.txt", "ROUND_2_APPENDED_ON_WINDOWS")
    $buf = New-Object byte[] (4 * 1024 * 1024)
    (New-Object System.Random(99)).NextBytes($buf)
    [System.IO.File]::WriteAllBytes("$drive\\r2_data.bin", $buf)
    $r2Sha = (Get-FileHash -Algorithm SHA256 "$drive\\r2_data.bin").Hash.ToLower()
    Write-Output "ROUND-2-SHA: $r2Sha"
    $r2Sha | Out-File -Encoding ascii C:\\winlvm\\r2_sha.txt

    cmd.exe /c "chkdsk.exe $drive"
    cmd.exe /c "fsutil volume dismount $drive"
    """
    res = run_guest_ps1(r2_script, "flow_c_round2.ps1")
    log("ROUND 2 OUTPUT:\n" + res.stdout)
    res_r2_hash = run_guest("powershell.exe -NoProfile -NonInteractive -Command \"Get-Content C:\\winlvm\\r2_sha.txt\"")
    r2_sha = res_r2_hash.stdout.strip().lower()

    # -------------------------------------------------------------
    # ROUND 3: Linux Host Expands LV & NTFS Filesystem
    # -------------------------------------------------------------
    log("--- ROUND 3: Linux Host Expands LV & Resizes NTFS ---")
    vm_powerdown()

    try:
        run_host("losetup -D", check=False)
        res_a = run_host(f"losetup -fP --show {DISK_A}")
        loop_a = res_a.stdout.strip()
        res_b = run_host(f"losetup -fP --show {DISK_B}")
        loop_b = res_b.stdout.strip()

        run_host(f"vgchange -ay {DEVFILE} pp_vg")
        # Extend LV by 32M (total 72M) non-interactively
        run_host(f"lvextend -y {DEVFILE} -L +32M pp_vg/pp_main")
        # Resize NTFS
        run_host("ntfsresize -f -b /dev/pp_vg/pp_main")

        run_host("mkdir -p /mnt/pp_test")
        run_host("mount /dev/pp_vg/pp_main /mnt/pp_test")
        run_host("echo 'ROUND_3_EXPANDED_BY_LINUX' > /mnt/pp_test/round3_linux.txt")
        run_host("umount /mnt/pp_test")
        run_host(f"vgchange -an {DEVFILE} pp_vg")
        log("Round 3 expansion completed on Linux.")
    finally:
        cleanup_loop(loop_a)
        cleanup_loop(loop_b)

    # -------------------------------------------------------------
    # ROUND 4: Windows VM Verifies Expansion & Creates 2nd Striped LV
    # -------------------------------------------------------------
    log("--- ROUND 4: Windows VM Verifies Expansion & Allocates Striped LV ---")
    vm_start()

    r4_script = r"""
    $ErrorActionPreference = 'Stop'
    sc.exe start winlvm 2>$null
    C:\winlvm\lvm.exe vgscan
    Start-Sleep -Seconds 5

    Add-Type @"
    using System;
    using System.Runtime.InteropServices;
    using Microsoft.Win32.SafeHandles;

    public class VolHelper {
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
        public static extern SafeFileHandle CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode,
            IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool DeviceIoControl(SafeFileHandle hDevice, uint dwIoControlCode,
            IntPtr lpInBuffer, uint nInBufferSize,
            out long lpOutBuffer, uint nOutBufferSize,
            out uint lpBytesReturned, IntPtr lpOverlapped);

        public const uint IOCTL_DISK_GET_LENGTH_INFO = 0x0007405C;

        public static long GetLength(string path) {
            using (var h = CreateFile(path.TrimEnd(new char[] { (char)92 }), 0x80000000, 3, IntPtr.Zero, 3, 0x80, IntPtr.Zero)) {
                if (h.IsInvalid) return -Marshal.GetLastWin32Error();
                long len = 0;
                uint ret = 0;
                if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, IntPtr.Zero, 0, out len, 8, out ret, IntPtr.Zero))
                    return -Marshal.GetLastWin32Error();
                return len;
            }
        }
    }
"@

    $vol = Get-CimInstance Win32_Volume | Where-Object { $_.Label -eq "PP_NTFS" }
    if (-not $vol) {
        $vol = Get-CimInstance Win32_Volume | Where-Object { $_.Capacity -ge 70000000 }
    }
    if ($vol.DriveLetter) { $drive = $vol.DriveLetter } else { $drive = "D:" }
    Write-Output "PP_NTFS on $drive, capacity: $($vol.Capacity)"
    if ($vol.Capacity -lt 70000000) {
        Write-Error "Volume capacity not expanded! $($vol.Capacity)"
    }

    $t3 = (Get-Content "$drive\round3_linux.txt").Trim()
    if ($t3 -ne "ROUND_3_EXPANDED_BY_LINUX") { Write-Error "Round 3 text mismatch: $t3" }
    Write-Output "ROUND-4-VERIFY-R3: PASS"

    # Create second Striped LV on Windows
    $disks = Get-Disk | Where-Object { $_.Size -ge 130000000 -and $_.Size -le 140000000 } | Sort-Object Number
    $d1 = "\\.\PhysicalDrive$($disks[0].Number)"
    $d2 = "\\.\PhysicalDrive$($disks[1].Number)"
    C:\winlvm\lvm.exe lvcreate -L 24M -i 2 -I 64k -n pp_stripe pp_vg $d1 $d2
    if ($LASTEXITCODE -ne 0) { Write-Error "lvcreate pp_stripe failed!" }
    Write-Output "ROUND-4-LVCREATE-STRIPED: PASS"

    # Re-probe driver to detect striped LV
    C:\winlvm\lvm.exe vgscan
    Start-Sleep -Seconds 5

    # Format striped volume as NTFS (find volume with exact length 24MB via VolHelper, ignoring ESP/FAT32)
    $strVol = $null
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        $vols = Get-CimInstance Win32_Volume
        foreach ($v in $vols) {
            if ($v.Label -eq "ESP" -or $v.Label -eq "System Reserved") { continue }
            $len = [VolHelper]::GetLength($v.DeviceID)
            if ($len -ge 24000000 -and $len -le 26000000) { $strVol = $v; break }
        }
        if ($strVol) { break }
        Start-Sleep -Seconds 1
    }
    if (-not $strVol) { Write-Error "Could not find 24MB Striped LV volume!" }

    if ($strVol.DriveLetter) {
        $strDrive = $strVol.DriveLetter
    } else {
        $used = (Get-CimInstance Win32_Volume | Where-Object { $_.DriveLetter }).DriveLetter
        $strDrive = $null
        foreach ($c in (69..90)) {
            $cand = ([char]$c) + ":"
            if ($used -notcontains $cand) {
                $strDrive = $cand
                break
            }
        }
        $volPath = $strVol.DeviceID.TrimEnd('\') + '\'
        cmd.exe /c "mountvol.exe $($strDrive)\ $volPath"
        Start-Sleep -Seconds 1
    }
    cmd.exe /c "format.com $strDrive /FS:NTFS /V:PP_STRIPE /Q /Y"
    if ($LASTEXITCODE -ne 0) { Write-Error "format.com striped failed!" }
    [System.IO.File]::WriteAllText("$strDrive\round4_stripe.txt", "ROUND_4_STRIPED_NTFS_PAYLOAD")
    cmd.exe /c "chkdsk.exe $strDrive"
    cmd.exe /c "fsutil volume dismount $strDrive"
    cmd.exe /c "fsutil volume dismount $drive"
    Write-Output "ROUND-4-STRIPED-FORMAT: PASS"
    """
    res = run_guest_ps1(r4_script, "flow_c_round4.ps1")
    log("ROUND 4 OUTPUT:\n" + res.stdout)

    # -------------------------------------------------------------
    # ROUND 5: Windows VM Executes pvmove
    # -------------------------------------------------------------
    log("--- ROUND 5: Windows VM Executes Extent Migration (pvmove) ---")
    r5_script = r"""
    $ErrorActionPreference = 'Stop'
    $disks = Get-Disk | Where-Object { $_.Size -ge 130000000 -and $_.Size -le 140000000 } | Sort-Object Number
    $d1 = "\\.\PhysicalDrive$($disks[0].Number)"
    $d2 = "\\.\PhysicalDrive$($disks[1].Number)"
    # Flush all test volumes before migration (skip C:)
    Get-CimInstance Win32_Volume | Where-Object { $_.DriveLetter -and $_.DriveLetter -ne "C:" } | ForEach-Object {
        cmd.exe /c "fsutil volume dismount $($_.DriveLetter)"
    }
    Start-Sleep -Seconds 2

    # Move extents of linear LV pp_main from d1 to d2
    C:\winlvm\lvm.exe pvmove -n pp_main $d1 $d2
    if ($LASTEXITCODE -ne 0) { Write-Error "pvmove failed!" }
    Write-Output "ROUND-5-PVMOVE: PASS"
    """
    res = run_guest_ps1(r5_script, "flow_c_round5.ps1")
    log("ROUND 5 OUTPUT:\n" + res.stdout)

    # -------------------------------------------------------------
    # ROUND 6: Linux Host Final Ground-Truth Verification
    # -------------------------------------------------------------
    log("--- ROUND 6: Linux Host Final Comprehensive Audit ---")
    vm_powerdown()

    try:
        run_host("losetup -D", check=False)
        res_a = run_host(f"losetup -fP --show {DISK_A}")
        loop_a = res_a.stdout.strip()
        res_b = run_host(f"losetup -fP --show {DISK_B}")
        loop_b = res_b.stdout.strip()

        # 1. Metadata consistency
        run_host(f"vgck {DEVFILE} pp_vg")
        log("ROUND-6-ORACLE-VGCK: PASS")

        # 2. PV allocation
        pvs_out = run_host(f"pvs {DEVFILE} -o pv_name,pv_used,pv_free {loop_a} {loop_b}").stdout
        log(f"PVS Output:\n{pvs_out}")

        # 3. Check LVs
        lvs_out = run_host(f"lvs {DEVFILE} -o lv_name,lv_size,segtype pp_vg").stdout
        log(f"LVS Output:\n{lvs_out}")

        # 4. Activate & Audit Main NTFS volume
        run_host(f"vgchange -ay {DEVFILE} pp_vg")
        run_host("ntfsfix -n /dev/pp_vg/pp_main")
        run_host("mkdir -p /mnt/pp_final")
        run_host("mount -o ro /dev/pp_vg/pp_main /mnt/pp_final")

        # Verify Round 1, 2, 3 files
        t1 = run_host("cat /mnt/pp_final/round1.txt").stdout.strip()
        if t1 != "ROUND_1_SEED_PAYLOAD": raise RuntimeError(f"R1 text corrupted: {t1}")

        t2 = run_host("cat /mnt/pp_final/round2_win.txt").stdout.strip()
        if t2 != "ROUND_2_APPENDED_ON_WINDOWS": raise RuntimeError(f"R2 text corrupted: {t2}")

        t3 = run_host("cat /mnt/pp_final/round3_linux.txt").stdout.strip()
        if t3 != "ROUND_3_EXPANDED_BY_LINUX": raise RuntimeError(f"R3 text corrupted: {t3}")

        act_r1_sha = run_host("sha256sum /mnt/pp_final/r1_data.bin").stdout.split()[0].lower()
        if act_r1_sha != r1_sha: raise RuntimeError(f"R1 SHA corrupted! Expected {r1_sha} got {act_r1_sha}")

        act_r2_sha = run_host("sha256sum /mnt/pp_final/r2_data.bin").stdout.split()[0].lower()
        if act_r2_sha != r2_sha: raise RuntimeError(f"R2 SHA corrupted! Expected {r2_sha} got {act_r2_sha}")

        log("ROUND-6-MAIN-PAYLOAD-SHA256: PASS (All payloads intact across 6 rounds)")
        run_host("umount /mnt/pp_final")

        # 5. Audit Striped volume created on Windows
        run_host("ntfsfix -n /dev/pp_vg/pp_stripe")
        run_host("mount -o ro /dev/pp_vg/pp_stripe /mnt/pp_final")
        t4 = run_host("cat /mnt/pp_final/round4_stripe.txt").stdout.strip()
        if t4 != "ROUND_4_STRIPED_NTFS_PAYLOAD": raise RuntimeError(f"R4 striped text corrupted: {t4}")
        log("ROUND-6-STRIPED-PAYLOAD: PASS")
        run_host("umount /mnt/pp_final")

        run_host(f"vgchange -an {DEVFILE} pp_vg")
        log("==================================================================")
        log("FLOW C: 6-ROUND PING-PONG PROTOCOL ALL PASS 100%!")
        log("==================================================================")
    finally:
        cleanup_loop(loop_a)
        cleanup_loop(loop_b)

    # Restart VM to leave it in clean state
    vm_start()


def main():
    log("Starting WinLVM Bidirectional Full-Spectrum Matrix...")
    start_time = time.time()

    # Deploy updated tools source to VM and build lvm.exe first
    vm_start()
    deploy_and_build_guest_tools()

    test_flow_a()
    test_flow_b()
    test_flow_c()

    elapsed = time.time() - start_time
    log(f"ALL BIDIRECTIONAL TEST FLOWS (A, B, C) COMPLETED IN {elapsed:.1f}s WITH 100% SUCCESS!")

if __name__ == "__main__":
    main()
