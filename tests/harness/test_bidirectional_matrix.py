#!/usr/bin/env python3
"""
WinLVM End-to-End Bidirectional Test Runner.

Coordinates interoperability testing between a Linux host (upstream LVM2 /
ntfs-3g via root losetup or unprivileged libguestfs guestfish) and a Windows
guest VM running winlvm.sys and lvm.exe over SSH.

Flows:
  - Flow A: Linux originator -> Windows guest consumer (NTFS + SHA-256 + chkdsk)
  - Flow B: Windows originator -> Linux consumer (pvcreate/vgcreate/lvcreate/
            lvextend/lvrename/pvmove -> Linux pvck/vgck/ntfsfix/SHA-256)
  - Flow C: 6-round interleaved ping-pong mutations across Linux and Windows
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


DEVFILE = '--devicesfile ""'


@dataclass
class Config:
    ssh_host: str
    ssh_port: int
    ssh_user: str
    ssh_identity: str | None
    vm_name: str | None
    work_dir: Path
    repo_dir: Path
    guest_dir: str
    lvm_exe: str
    disk_a: Path
    disk_b: Path
    log_file: Path
    linux_backend: str  # "guestfish" or "host"
    skip_build: bool
    flows: str


def env_or(name: str, default: str | None = None) -> str | None:
    val = os.environ.get(name)
    return val if val else default


def parse_args() -> Config:
    repo_default = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(
        description="WinLVM bidirectional Linux <-> Windows interoperability test runner."
    )
    parser.add_argument(
        "--ssh-host",
        default=env_or("WINLVM_SSH_HOST", "127.0.0.1"),
        help="Windows guest SSH hostname or IP (env: WINLVM_SSH_HOST, default: 127.0.0.1).",
    )
    parser.add_argument(
        "--ssh-port",
        type=int,
        default=int(env_or("WINLVM_SSH_PORT", "2222") or "2222"),
        help="Windows guest SSH port (env: WINLVM_SSH_PORT, default: 2222).",
    )
    parser.add_argument(
        "--ssh-user",
        default=env_or("WINLVM_SSH_USER", "Administrator"),
        help="Windows guest SSH username (env: WINLVM_SSH_USER, default: Administrator).",
    )
    parser.add_argument(
        "--ssh-identity",
        default=env_or("WINLVM_SSH_IDENTITY"),
        help="Optional SSH private key path (env: WINLVM_SSH_IDENTITY).",
    )
    parser.add_argument(
        "--vm-name",
        default=env_or("WINLVM_VM_NAME"),
        help="Optional libvirt VM name for virsh powerdown/start (env: WINLVM_VM_NAME).",
    )
    parser.add_argument(
        "--work-dir",
        default=env_or("WINLVM_WORK_DIR"),
        help="Working directory for raw disk images, temp payloads, and logs (env: WINLVM_WORK_DIR).",
    )
    parser.add_argument(
        "--repo-dir",
        default=env_or("WINLVM_REPO_DIR", str(repo_default)),
        help="Path to winlvm repository root (env: WINLVM_REPO_DIR).",
    )
    parser.add_argument(
        "--guest-dir",
        default=env_or("WINLVM_GUEST_DIR", r"C:\winlvm"),
        help=r"Guest staging directory on Windows (env: WINLVM_GUEST_DIR, default: C:\winlvm).",
    )
    parser.add_argument(
        "--lvm-exe",
        default=env_or("WINLVM_LVM_EXE"),
        help=r"Path to lvm.exe on Windows guest (env: WINLVM_LVM_EXE, default: <guest-dir>\lvm.exe).",
    )
    parser.add_argument(
        "--disk-a",
        default=env_or("WINLVM_DISK_A"),
        help="Path to raw disk A image (default: <work-dir>/disk-a.img).",
    )
    parser.add_argument(
        "--disk-b",
        default=env_or("WINLVM_DISK_B"),
        help="Path to raw disk B image (default: <work-dir>/disk-b.img).",
    )
    parser.add_argument(
        "--log-file",
        default=env_or("WINLVM_LOG_FILE"),
        help="Output log path (default: <work-dir>/bidirectional_matrix.log).",
    )
    parser.add_argument(
        "--linux-backend",
        choices=["auto", "guestfish", "host"],
        default=env_or("WINLVM_LINUX_BACKEND", "auto"),
        help="Linux disk-image backend: 'guestfish' (unprivileged), 'host' (root losetup), or 'auto'.",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Skip deploying source and rebuilding lvm.exe / winlvm.sys on the Windows guest.",
    )
    parser.add_argument(
        "--flows",
        choices=["all", "a", "b", "c"],
        default="all",
        help="Which test flows to execute (default: all).",
    )

    ns = parser.parse_args()

    if ns.work_dir:
        work_dir = Path(ns.work_dir).expanduser().resolve()
        work_dir.mkdir(parents=True, exist_ok=True)
    else:
        work_dir = Path(tempfile.mkdtemp(prefix="winlvm-matrix-"))

    backend = ns.linux_backend
    if backend == "auto":
        if os.geteuid() == 0:
            backend = "host"
        elif shutil.which("guestfish"):
            backend = "guestfish"
        else:
            parser.error(
                "Not running as root and 'guestfish' was not found on PATH. "
                "Install libguestfs-tools for unprivileged execution or run as root with --linux-backend host."
            )
    elif backend == "guestfish" and not shutil.which("guestfish"):
        parser.error("--linux-backend guestfish requested, but 'guestfish' is not on PATH.")
    elif backend == "host" and os.geteuid() != 0:
        parser.error("--linux-backend host requires root privileges (or use --linux-backend guestfish).")

    guest_dir = ns.guest_dir.rstrip("\\/")
    lvm_exe = ns.lvm_exe if ns.lvm_exe else f"{guest_dir}\\lvm.exe"
    disk_a = Path(ns.disk_a).expanduser().resolve() if ns.disk_a else (work_dir / "disk-a.img")
    disk_b = Path(ns.disk_b).expanduser().resolve() if ns.disk_b else (work_dir / "disk-b.img")
    log_file = Path(ns.log_file).expanduser().resolve() if ns.log_file else (work_dir / "bidirectional_matrix.log")
    log_file.parent.mkdir(parents=True, exist_ok=True)

    return Config(
        ssh_host=ns.ssh_host,
        ssh_port=ns.ssh_port,
        ssh_user=ns.ssh_user,
        ssh_identity=ns.ssh_identity,
        vm_name=ns.vm_name,
        work_dir=work_dir,
        repo_dir=Path(ns.repo_dir).expanduser().resolve(),
        guest_dir=guest_dir,
        lvm_exe=lvm_exe,
        disk_a=disk_a,
        disk_b=disk_b,
        log_file=log_file,
        linux_backend=backend,
        skip_build=ns.skip_build,
        flows=ns.flows,
    )


class Runner:
    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self.mount_dir = cfg.work_dir / "mnt"
        self.mount_dir.mkdir(parents=True, exist_ok=True)

    def log(self, msg: str) -> None:
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
        formatted = f"[{timestamp}] {msg}"
        print(formatted, flush=True)
        with self.cfg.log_file.open("a", encoding="utf-8") as f:
            f.write(formatted + "\n")

    def _ssh_base(self) -> list[str]:
        cmd = [
            "ssh",
            "-p",
            str(self.cfg.ssh_port),
            "-o",
            "ConnectTimeout=8",
            "-o",
            "BatchMode=yes",
            "-o",
            "StrictHostKeyChecking=no",
            "-o",
            "UserKnownHostsFile=/dev/null",
        ]
        if self.cfg.ssh_identity:
            cmd.extend(["-i", self.cfg.ssh_identity])
        cmd.append(f"{self.cfg.ssh_user}@{self.cfg.ssh_host}")
        return cmd

    def _scp_to_guest(self, local_path: Path, remote_name: str) -> None:
        guest_posix = self.cfg.guest_dir.replace("\\", "/")
        cmd = [
            "scp",
            "-P",
            str(self.cfg.ssh_port),
            "-o",
            "ConnectTimeout=8",
            "-o",
            "BatchMode=yes",
            "-o",
            "StrictHostKeyChecking=no",
            "-o",
            "UserKnownHostsFile=/dev/null",
        ]
        if self.cfg.ssh_identity:
            cmd.extend(["-i", self.cfg.ssh_identity])
        cmd.extend([str(local_path), f"{self.cfg.ssh_user}@{self.cfg.ssh_host}:{guest_posix}/{remote_name}"])
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if res.returncode != 0:
            self.log(f"SCP FAILED: {local_path}\nSTDOUT: {res.stdout}\nSTDERR: {res.stderr}")
            raise RuntimeError(f"SCP failed for {local_path}")

    def run_host(self, cmd: str, check: bool = True) -> subprocess.CompletedProcess[str]:
        """Execute a Linux storage command directly (requires root backend)."""
        res = subprocess.run(
            ["sh", "-c", f"{cmd} < /dev/null"],
            text=True,
            capture_output=True,
        )
        if check and res.returncode != 0:
            self.log(f"HOST CMD FAILED: {cmd}\nSTDOUT: {res.stdout}\nSTDERR: {res.stderr}")
            raise RuntimeError(f"Host command failed: {cmd}")
        return res

    def run_guestfish(self, commands: list[str], disks: list[Path]) -> subprocess.CompletedProcess[str]:
        """Execute unprivileged LVM/NTFS operations inside libguestfs appliance."""
        cmd = ["guestfish"]
        for d in disks:
            cmd.extend(["-a", str(d)])
        script = "\n".join(commands) + "\n"
        res = subprocess.run(cmd, input=script, text=True, capture_output=True, timeout=300)
        if res.returncode != 0:
            self.log(f"GUESTFISH FAILED\nCOMMANDS:\n{script}\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}")
            raise RuntimeError("guestfish command sequence failed")
        return res

    def run_guest(self, cmd: str, check: bool = True) -> subprocess.CompletedProcess[str]:
        args = self._ssh_base() + [cmd]
        res = subprocess.run(args, capture_output=True, text=True, timeout=180)
        if check and res.returncode != 0:
            self.log(f"GUEST CMD FAILED: {cmd}\nSTDOUT: {res.stdout}\nSTDERR: {res.stderr}")
            raise RuntimeError(f"Guest command failed: {cmd}")
        return res

    def run_guest_ps1(self, script_content: str, script_name: str = "step.ps1") -> subprocess.CompletedProcess[str]:
        local_path = self.cfg.work_dir / script_name
        local_path.write_text(script_content, encoding="utf-8")
        self._scp_to_guest(local_path, script_name)
        cmd = (
            "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
            f"-File \"{self.cfg.guest_dir}\\{script_name}\""
        )
        return self.run_guest(cmd, check=True)

    def wait_guest_ssh(self, max_wait: int = 120) -> None:
        self.log("Waiting for Windows guest SSH...")
        start = time.time()
        while time.time() - start < max_wait:
            try:
                res = self.run_guest("echo SSH_OK", check=False)
                if res.returncode == 0 and "SSH_OK" in res.stdout:
                    self.log("Windows guest SSH is responsive.")
                    return
            except Exception:
                pass
            time.sleep(3)
        raise TimeoutError("Timed out waiting for Windows guest SSH")

    def vm_powerdown(self) -> None:
        if not self.cfg.vm_name:
            self.log("No --vm-name configured; skipping virsh powerdown.")
            return
        vm = self.cfg.vm_name
        self.log(f"Powering down VM {vm}...")
        res = subprocess.run(["virsh", "domstate", vm], capture_output=True, text=True)
        if "shut off" in res.stdout:
            self.log("VM is already shut off.")
            return
        try:
            subprocess.run(
                [
                    "virsh",
                    "qemu-agent-command",
                    vm,
                    '{"execute":"guest-shutdown","arguments":{"mode":"powerdown"}}',
                ],
                capture_output=True,
                text=True,
                timeout=10,
            )
        except Exception:
            pass
        for _ in range(20):
            res = subprocess.run(["virsh", "domstate", vm], capture_output=True, text=True)
            if "shut off" in res.stdout:
                self.log("VM is cleanly shut off.")
                return
            time.sleep(2)
        self.log("Forcing virsh destroy...")
        subprocess.run(["virsh", "destroy", vm], check=False)
        time.sleep(2)

    def vm_start(self) -> None:
        if self.cfg.vm_name:
            vm = self.cfg.vm_name
            res = subprocess.run(["virsh", "domstate", vm], capture_output=True, text=True)
            if "running" not in res.stdout:
                self.log(f"Starting VM {vm}...")
                subprocess.run(["virsh", "start", vm], check=True)
                time.sleep(20)
        self.wait_guest_ssh(120)

    def reset_raw_disks(self, size_mb: int = 128) -> None:
        size_bytes = size_mb * 1024 * 1024
        for disk in (self.cfg.disk_a, self.cfg.disk_b):
            disk.parent.mkdir(parents=True, exist_ok=True)
            with disk.open("wb") as f:
                f.truncate(size_bytes)

    def deploy_and_build_guest_tools(self) -> None:
        if self.cfg.skip_build:
            self.log("Skipping guest source deployment and build (--skip-build).")
            return
        self.log("Packaging and deploying source to Windows guest...")
        tar_path = self.cfg.work_dir / "winlvm_src.tgz"
        subprocess.run(
            ["tar", "czf", str(tar_path), "--exclude=x64", "--exclude=build", "src"],
            cwd=str(self.cfg.repo_dir),
            check=True,
        )
        self.run_guest(f'powershell.exe -NoProfile -Command "New-Item -ItemType Directory -Force -Path \'{self.cfg.guest_dir}\' | Out-Null"')
        self._scp_to_guest(tar_path, "winlvm_src.tgz")
        self.run_guest(f'cd /d "{self.cfg.guest_dir}" && tar -xzmf winlvm_src.tgz')

        gdir = self.cfg.guest_dir
        build_script = f"""
$ErrorActionPreference = 'Stop'
$vswhere = "${{env:ProgramFiles(x86)}}\\Microsoft Visual Studio\\Installer\\vswhere.exe"
$vsPath = $null
if (Test-Path $vswhere) {{
    $vsPath = & $vswhere -latest -products * -property installationPath
}}
if (-not $vsPath) {{
    $vsPath = "${{env:ProgramFiles(x86)}}\\Microsoft Visual Studio\\2022\\BuildTools"
}}
$vcvars = Join-Path $vsPath "VC\\Auxiliary\\Build\\vcvars64.bat"
$msbuild = Join-Path $vsPath "MSBuild\\Current\\Bin\\MSBuild.exe"
cmd.exe /c "call `"$vcvars`" && cl.exe /nologo /EHsc /std:c++17 /O2 /I `"{gdir}\\src`" /I `"{gdir}\\src\\lvmfmt\\include`" /I `"{gdir}\\src\\tools\\lvm`" `"{gdir}\\src\\tools\\lvm\\*.cpp`" `"{gdir}\\src\\lvmfmt\\*.cpp`" /Fe:`"{self.cfg.lvm_exe}`""
if ($LASTEXITCODE -ne 0) {{ Write-Error "cl.exe failed" }}
& $msbuild "{gdir}\\src\\driver\\winlvm.vcxproj" /p:Configuration=Release /p:Platform=x64 /p:EnableInfVerif=false /m /v:m /nologo
if ($LASTEXITCODE -ne 0) {{ Write-Error "MSBuild failed" }}
Write-Output "GUEST-BUILD: PASS"
"""
        res = self.run_guest_ps1(build_script, "build_guest.ps1")
        self.log("GUEST BUILD OUTPUT:\n" + res.stdout)

    def cleanup_host_storage(self, loops: list[str] | None = None) -> None:
        if self.cfg.linux_backend != "host":
            return
        for vg in ("flowa_vg", "win_flowb_vg", "pp_vg"):
            self.run_host(f"vgchange -an {DEVFILE} {vg} 2>/dev/null || true", check=False)
        for mnt in ("flowa_test", "flowb_audit", "pp_test", "pp_final"):
            self.run_host(f"umount '{self.mount_dir / mnt}' 2>/dev/null || true", check=False)
        if loops:
            for lo in loops:
                if lo:
                    self.run_host(f"losetup -d '{lo}' 2>/dev/null || true", check=False)

    def _create_deterministic_file(self, path: Path, size_bytes: int, seed_byte: int) -> str:
        import hashlib

        h = hashlib.sha256()
        block = bytearray(65536)
        remaining = size_bytes
        idx = 0
        with path.open("wb") as f:
            while remaining > 0:
                n = min(len(block), remaining)
                for i in range(n):
                    block[i] = (seed_byte + idx + i * 31) & 0xFF
                chunk = bytes(block[:n])
                f.write(chunk)
                h.update(chunk)
                remaining -= n
                idx += 1
        return h.hexdigest()

    # ------------------------------------------------------------------
    # FLOW A: Linux originator -> Windows guest consumer
    # ------------------------------------------------------------------
    def test_flow_a(self) -> None:
        self.log("==================================================================")
        self.log("STARTING FLOW A: Linux Originator -> Windows Guest Consumer")
        self.log("==================================================================")

        self.vm_powerdown()
        self.reset_raw_disks(128)

        payload_local = self.cfg.work_dir / "flowa_payload_4m.bin"
        expected_sha = self._create_deterministic_file(payload_local, 4 * 1024 * 1024, 0x3A)
        self.log(f"Flow A 4 MiB payload SHA256: {expected_sha}")

        if self.cfg.linux_backend == "guestfish":
            self.run_guestfish(
                [
                    "run",
                    "pvcreate /dev/sda",
                    "pvcreate /dev/sdb",
                    "vgcreate flowa_vg '/dev/sda /dev/sdb'",
                    "lvcreate flowa_lin flowa_vg 64",
                    "debug sh 'lvcreate -y -W y -L 32M -i 2 -I 64k -n flowa_str flowa_vg /dev/sda /dev/sdb'",
                    "mkfs-opts ntfs /dev/flowa_vg/flowa_lin quick:true force:true label:FlowA_NTFS",
                    "mount /dev/flowa_vg/flowa_lin /",
                    "write /sentinel.txt 'WINLVM_FLOW_A_TEXT_SENTINEL\n'",
                    "mkdir-p /dir1/dir2/dir3",
                    "write /dir1/dir2/dir3/nested.txt 'DEEP_NESTED_CONTENT_FLOW_A\n'",
                    f"upload '{payload_local}' /payload_4m.bin",
                    "sync",
                    "umount /",
                    "vg-activate false flowa_vg",
                ],
                [self.cfg.disk_a, self.cfg.disk_b],
            )
        else:
            loops: list[str] = []
            mnt = self.mount_dir / "flowa_test"
            mnt.mkdir(parents=True, exist_ok=True)
            try:
                loop_a = self.run_host(f"losetup -fP --show '{self.cfg.disk_a}'").stdout.strip()
                loop_b = self.run_host(f"losetup -fP --show '{self.cfg.disk_b}'").stdout.strip()
                loops = [loop_a, loop_b]
                self.run_host(f"wipefs -a '{loop_a}' '{loop_b}'")
                self.run_host(f"pvcreate -y -ff {DEVFILE} '{loop_a}' '{loop_b}'")
                self.run_host(f"vgcreate -y -ff -s 4M {DEVFILE} flowa_vg '{loop_a}' '{loop_b}'")
                self.run_host(f"lvcreate -y -W y {DEVFILE} -L 64M -n flowa_lin flowa_vg '{loop_a}'")
                self.run_host(f"lvcreate -y -W y {DEVFILE} -L 32M -i 2 -I 64k -n flowa_str flowa_vg '{loop_a}' '{loop_b}'")
                self.run_host("mkfs.ntfs -Q -F -L FlowA_NTFS /dev/flowa_vg/flowa_lin")
                self.run_host(f"mount /dev/flowa_vg/flowa_lin '{mnt}'")
                self.run_host(f"printf 'WINLVM_FLOW_A_TEXT_SENTINEL\\n' > '{mnt}/sentinel.txt'")
                self.run_host(f"mkdir -p '{mnt}/dir1/dir2/dir3'")
                self.run_host(f"printf 'DEEP_NESTED_CONTENT_FLOW_A\\n' > '{mnt}/dir1/dir2/dir3/nested.txt'")
                self.run_host(f"cp '{payload_local}' '{mnt}/payload_4m.bin'")
                self.run_host(f"umount '{mnt}'")
                self.run_host(f"vgchange -an {DEVFILE} flowa_vg")
            finally:
                self.cleanup_host_storage(loops)

        self.vm_start()

        lvm = self.cfg.lvm_exe
        guest_script = f"""
$ErrorActionPreference = 'Stop'
sc.exe start winlvm 2>$null
& "{lvm}" vgscan
Start-Sleep -Seconds 5
& "{lvm}" pvs
& "{lvm}" vgs
& "{lvm}" lvs

$vol = Get-CimInstance Win32_Volume | Where-Object {{ $_.Label -eq "FlowA_NTFS" }}
if (-not $vol) {{
    $vol = Get-CimInstance Win32_Volume | Where-Object {{ $_.Capacity -ge 60000000 -and $_.Capacity -le 70000000 }}
}}
if (-not $vol) {{ Write-Error "FlowA_NTFS volume not found by Windows Mount Manager." }}
if (-not $vol.DriveLetter) {{
    $drive = "D:"
    cmd.exe /c "mountvol.exe $drive\\ $($vol.DeviceID)"
}} else {{
    $drive = $vol.DriveLetter
}}
if (((Get-Content "$drive\\sentinel.txt").Trim()) -ne "WINLVM_FLOW_A_TEXT_SENTINEL") {{
    Write-Error "Sentinel text mismatch"
}}
if (((Get-Content "$drive\\dir1\\dir2\\dir3\\nested.txt").Trim()) -ne "DEEP_NESTED_CONTENT_FLOW_A") {{
    Write-Error "Nested text mismatch"
}}
$actualSha = (Get-FileHash -Algorithm SHA256 "$drive\\payload_4m.bin").Hash.ToLower()
if ($actualSha -ne "{expected_sha}") {{
    Write-Error "Binary hash mismatch! Expected {expected_sha} got $actualSha"
}}
cmd.exe /c "chkdsk.exe $drive"
if ($LASTEXITCODE -ne 0) {{ Write-Error "chkdsk failed with exit code $LASTEXITCODE" }}
Write-Output "FLOW-A: PASS ($actualSha)"
"""
        res = self.run_guest_ps1(guest_script, "flow_a_verify.ps1")
        self.log("FLOW A GUEST OUTPUT:\n" + res.stdout)

    # ------------------------------------------------------------------
    # FLOW B: Windows originator -> Linux consumer
    # ------------------------------------------------------------------
    def test_flow_b(self) -> None:
        self.log("==================================================================")
        self.log("STARTING FLOW B: Windows Originator -> Linux Consumer")
        self.log("==================================================================")

        self.vm_powerdown()
        self.reset_raw_disks(128)
        self.vm_start()

        lvm = self.cfg.lvm_exe
        gdir = self.cfg.guest_dir
        guest_script = f"""
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public class VolHelper {{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    public static extern SafeFileHandle CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeviceIoControl(SafeFileHandle hDevice, uint dwIoControlCode,
        IntPtr lpInBuffer, uint nInBufferSize, out long lpOutBuffer, uint nOutBufferSize,
        out uint lpBytesReturned, IntPtr lpOverlapped);
    public const uint IOCTL_DISK_GET_LENGTH_INFO = 0x0007405C;
    public static long GetLength(string path) {{
        using (var h = CreateFile(path.TrimEnd(new char[] {{ (char)92 }}), 0x80000000, 3, IntPtr.Zero, 3, 0x80, IntPtr.Zero)) {{
            if (h.IsInvalid) return -Marshal.GetLastWin32Error();
            long len = 0; uint ret = 0;
            if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, IntPtr.Zero, 0, out len, 8, out ret, IntPtr.Zero))
                return -Marshal.GetLastWin32Error();
            return len;
        }}
    }}
}}
"@

$disks = Get-Disk | Where-Object {{ $_.Size -ge 130000000 -and $_.Size -le 140000000 }} | Sort-Object Number
if ($disks.Count -lt 2) {{ Write-Error "Could not find two 128 MiB test disks on Windows." }}
$d1 = "\\\\.\\PhysicalDrive$($disks[0].Number)"
$d2 = "\\\\.\\PhysicalDrive$($disks[1].Number)"

Set-Disk -Number $disks[0].Number -IsOffline $false -ErrorAction SilentlyContinue
Set-Disk -Number $disks[0].Number -IsReadOnly $false -ErrorAction SilentlyContinue
Set-Disk -Number $disks[1].Number -IsOffline $false -ErrorAction SilentlyContinue
Set-Disk -Number $disks[1].Number -IsReadOnly $false -ErrorAction SilentlyContinue

& "{lvm}" pvcreate -y -f $d1 $d2
if ($LASTEXITCODE -ne 0) {{ Write-Error "pvcreate failed" }}
& "{lvm}" vgcreate -s 4M win_flowb_vg $d1 $d2
if ($LASTEXITCODE -ne 0) {{ Write-Error "vgcreate failed" }}
& "{lvm}" lvcreate -L 40M -n win_lin win_flowb_vg $d1 $d2
if ($LASTEXITCODE -ne 0) {{ Write-Error "lvcreate win_lin failed" }}
& "{lvm}" lvcreate -L 32M -i 2 -I 64k -n win_str win_flowb_vg $d1 $d2
if ($LASTEXITCODE -ne 0) {{ Write-Error "lvcreate win_str failed" }}
& "{lvm}" vgscan
Start-Sleep -Seconds 5

$linVol = $null; $strVol = $null
for ($attempt = 0; $attempt -lt 15; $attempt++) {{
    foreach ($v in (Get-CimInstance Win32_Volume)) {{
        $len = [VolHelper]::GetLength($v.DeviceID)
        if ($len -ge 40000000 -and $len -le 43000000) {{ $linVol = $v }}
        if ($len -ge 32000000 -and $len -le 35000000) {{ $strVol = $v }}
    }}
    if ($linVol -and $strVol) {{ break }}
    Start-Sleep -Seconds 1
}}
if (-not $linVol -or -not $strVol) {{ Write-Error "Failed to locate Windows-created LV volumes." }}
$driveLin = if ($linVol.DriveLetter) {{ $linVol.DriveLetter }} else {{ cmd.exe /c "mountvol.exe D:\\ $($linVol.DeviceID)"; "D:" }}
$driveStr = if ($strVol.DriveLetter) {{ $strVol.DriveLetter }} else {{ cmd.exe /c "mountvol.exe E:\\ $($strVol.DeviceID)"; "E:" }}

cmd.exe /c "format.com $driveLin /FS:NTFS /V:WIN_ORIGIN /Q /Y"
if ($LASTEXITCODE -ne 0) {{ Write-Error "format linear failed" }}
cmd.exe /c "format.com $driveStr /FS:NTFS /V:WIN_STRIPE /Q /Y"
if ($LASTEXITCODE -ne 0) {{ Write-Error "format striped failed" }}

[System.IO.File]::WriteAllText("$driveLin\\win_origin.txt", "CREATED_ON_WINDOWS_VM_BY_WINLVM")
$buf = New-Object byte[] (4 * 1024 * 1024)
(New-Object System.Random(42)).NextBytes($buf)
[System.IO.File]::WriteAllBytes("$driveLin\\win_payload_4m.bin", $buf)
$winSha = (Get-FileHash -Algorithm SHA256 "$driveLin\\win_payload_4m.bin").Hash.ToLower()
$winSha | Out-File -Encoding ascii "{gdir}\\flowb_win_sha.txt"
[System.IO.File]::WriteAllText("$driveStr\\win_stripe.txt", "STRIPED_LV_CREATED_ON_WINDOWS_VM")

cmd.exe /c "chkdsk.exe $driveLin"
cmd.exe /c "chkdsk.exe $driveStr"
cmd.exe /c "fsutil volume dismount $driveLin"
cmd.exe /c "fsutil volume dismount $driveStr"
Start-Sleep -Seconds 2

& "{lvm}" lvextend -L +16M win_flowb_vg/win_lin $d1 $d2
if ($LASTEXITCODE -ne 0) {{ Write-Error "lvextend failed" }}
& "{lvm}" lvrename win_flowb_vg/win_lin win_renamed $d1 $d2
if ($LASTEXITCODE -ne 0) {{ Write-Error "lvrename failed" }}
& "{lvm}" pvmove -n win_renamed $d1 $d2
if ($LASTEXITCODE -ne 0) {{ Write-Error "pvmove failed" }}
Write-Output "FLOW-B-GUEST: PASS ($winSha)"
"""
        res = self.run_guest_ps1(guest_script, "flow_b_create.ps1")
        self.log("FLOW B GUEST OUTPUT:\n" + res.stdout)

        res_hash = self.run_guest(
            f'powershell.exe -NoProfile -NonInteractive -Command "Get-Content \'{gdir}\\flowb_win_sha.txt\'"'
        )
        expected_win_sha = res_hash.stdout.strip().lower()
        self.log(f"Expected Windows-created SHA256: {expected_win_sha}")

        self.vm_powerdown()

        if self.cfg.linux_backend == "guestfish":
            gf_res = self.run_guestfish(
                [
                    "run",
                    "debug sh 'pvck /dev/sda && pvck /dev/sdb && vgck win_flowb_vg'",
                    "lvs",
                    "vg-activate true win_flowb_vg",
                    "ntfsresize-opts /dev/win_flowb_vg/win_renamed force:true",
                    "ntfsfix /dev/win_flowb_vg/win_renamed",
                    "mount-ro /dev/win_flowb_vg/win_renamed /",
                    "cat /win_origin.txt",
                    "checksum sha256 /win_payload_4m.bin",
                    "umount /",
                    "ntfsfix /dev/win_flowb_vg/win_str",
                    "mount-ro /dev/win_flowb_vg/win_str /",
                    "cat /win_stripe.txt",
                    "umount /",
                    "vg-activate false win_flowb_vg",
                ],
                [self.cfg.disk_a, self.cfg.disk_b],
            )
            out = gf_res.stdout
            if "CREATED_ON_WINDOWS_VM_BY_WINLVM" not in out:
                raise RuntimeError("Linear origin text mismatch in guestfish audit")
            if expected_win_sha not in out.lower():
                raise RuntimeError(f"Expected SHA256 {expected_win_sha} not found in guestfish output:\n{out}")
            if "STRIPED_LV_CREATED_ON_WINDOWS_VM" not in out:
                raise RuntimeError("Striped origin text mismatch in guestfish audit")
        else:
            loops = []
            mnt = self.mount_dir / "flowb_audit"
            mnt.mkdir(parents=True, exist_ok=True)
            try:
                loop_a = self.run_host(f"losetup -fP --show '{self.cfg.disk_a}'").stdout.strip()
                loop_b = self.run_host(f"losetup -fP --show '{self.cfg.disk_b}'").stdout.strip()
                loops = [loop_a, loop_b]
                self.run_host(f"pvck {DEVFILE} '{loop_a}' '{loop_b}'")
                self.run_host(f"vgck {DEVFILE} win_flowb_vg")
                res_lvs = self.run_host(f"lvs {DEVFILE} win_flowb_vg")
                if "win_renamed" not in res_lvs.stdout or "win_str" not in res_lvs.stdout:
                    raise RuntimeError("Linux lvs did not report win_renamed and win_str")
                self.run_host(f"vgchange -ay {DEVFILE} win_flowb_vg")
                self.run_host("ntfsresize -f -b /dev/win_flowb_vg/win_renamed")
                self.run_host("ntfsfix -n /dev/win_flowb_vg/win_renamed")
                self.run_host(f"mount -o ro /dev/win_flowb_vg/win_renamed '{mnt}'")
                if "CREATED_ON_WINDOWS_VM_BY_WINLVM" not in self.run_host(f"cat '{mnt}/win_origin.txt'").stdout:
                    raise RuntimeError("Origin text mismatch on Linux host audit")
                audit_sha = self.run_host(f"sha256sum '{mnt}/win_payload_4m.bin'").stdout.split()[0].lower()
                if audit_sha != expected_win_sha:
                    raise RuntimeError(f"SHA256 mismatch: expected {expected_win_sha}, got {audit_sha}")
                self.run_host(f"umount '{mnt}'")
                self.run_host("ntfsfix -n /dev/win_flowb_vg/win_str")
                self.run_host(f"mount -o ro /dev/win_flowb_vg/win_str '{mnt}'")
                if "STRIPED_LV_CREATED_ON_WINDOWS_VM" not in self.run_host(f"cat '{mnt}/win_stripe.txt'").stdout:
                    raise RuntimeError("Striped text mismatch on Linux host audit")
                self.run_host(f"umount '{mnt}'")
                self.run_host(f"vgchange -an {DEVFILE} win_flowb_vg")
            finally:
                self.cleanup_host_storage(loops)

        self.log("FLOW B: PASS")
        self.vm_start()

    # ------------------------------------------------------------------
    # FLOW C: 6-round interleaved ping-pong mutations
    # ------------------------------------------------------------------
    def test_flow_c(self) -> None:
        self.log("==================================================================")
        self.log("STARTING FLOW C: 6-Round Interleaved Ping-Pong Protocol")
        self.log("==================================================================")

        self.vm_powerdown()
        self.reset_raw_disks(128)

        r1_local = self.cfg.work_dir / "r1_data.bin"
        r1_sha = self._create_deterministic_file(r1_local, 2 * 1024 * 1024, 0x55)

        if self.cfg.linux_backend == "guestfish":
            self.run_guestfish(
                [
                    "run",
                    "pvcreate /dev/sda",
                    "pvcreate /dev/sdb",
                    "vgcreate pp_vg '/dev/sda /dev/sdb'",
                    "lvcreate pp_main pp_vg 40",
                    "mkfs-opts ntfs /dev/pp_vg/pp_main quick:true force:true label:PP_NTFS",
                    "mount /dev/pp_vg/pp_main /",
                    "write /round1.txt 'ROUND_1_SEED_PAYLOAD\n'",
                    f"upload '{r1_local}' /r1_data.bin",
                    "sync",
                    "umount /",
                    "vg-activate false pp_vg",
                ],
                [self.cfg.disk_a, self.cfg.disk_b],
            )
        else:
            loops: list[str] = []
            mnt = self.mount_dir / "pp_test"
            mnt.mkdir(parents=True, exist_ok=True)
            try:
                loop_a = self.run_host(f"losetup -fP --show '{self.cfg.disk_a}'").stdout.strip()
                loop_b = self.run_host(f"losetup -fP --show '{self.cfg.disk_b}'").stdout.strip()
                loops = [loop_a, loop_b]
                self.run_host(f"wipefs -a '{loop_a}' '{loop_b}'")
                self.run_host(f"pvcreate -y -ff {DEVFILE} '{loop_a}' '{loop_b}'")
                self.run_host(f"vgcreate -y -ff -s 4M {DEVFILE} pp_vg '{loop_a}' '{loop_b}'")
                self.run_host(f"lvcreate -y -W y {DEVFILE} -L 40M -n pp_main pp_vg '{loop_a}'")
                self.run_host("mkfs.ntfs -Q -F -L PP_NTFS /dev/pp_vg/pp_main")
                self.run_host(f"mount /dev/pp_vg/pp_main '{mnt}'")
                self.run_host(f"printf 'ROUND_1_SEED_PAYLOAD\\n' > '{mnt}/round1.txt'")
                self.run_host(f"cp '{r1_local}' '{mnt}/r1_data.bin'")
                self.run_host(f"umount '{mnt}'")
                self.run_host(f"vgchange -an {DEVFILE} pp_vg")
            finally:
                self.cleanup_host_storage(loops)

        self.vm_start()
        lvm = self.cfg.lvm_exe
        gdir = self.cfg.guest_dir

        r2_script = f"""
$ErrorActionPreference = 'Stop'
sc.exe start winlvm 2>$null
& "{lvm}" vgscan
Start-Sleep -Seconds 5
$vol = Get-CimInstance Win32_Volume | Where-Object {{ $_.Label -eq "PP_NTFS" }}
if (-not $vol) {{ $vol = Get-CimInstance Win32_Volume | Where-Object {{ $_.Capacity -ge 38000000 -and $_.Capacity -le 43000000 }} }}
if (-not $vol) {{ Write-Error "PP_NTFS volume not found." }}
$drive = if ($vol.DriveLetter) {{ $vol.DriveLetter }} else {{ cmd.exe /c "mountvol.exe D:\\ $($vol.DeviceID)"; "D:" }}
if (((Get-Content "$drive\\round1.txt").Trim()) -ne "ROUND_1_SEED_PAYLOAD") {{ Write-Error "Round 1 text mismatch" }}
$actR1 = (Get-FileHash -Algorithm SHA256 "$drive\\r1_data.bin").Hash.ToLower()
if ($actR1 -ne "{r1_sha}") {{ Write-Error "Round 1 SHA mismatch: $actR1" }}
[System.IO.File]::WriteAllText("$drive\\round2_win.txt", "ROUND_2_APPENDED_ON_WINDOWS")
$buf = New-Object byte[] (4 * 1024 * 1024)
(New-Object System.Random(99)).NextBytes($buf)
[System.IO.File]::WriteAllBytes("$drive\\r2_data.bin", $buf)
$r2Sha = (Get-FileHash -Algorithm SHA256 "$drive\\r2_data.bin").Hash.ToLower()
$r2Sha | Out-File -Encoding ascii "{gdir}\\r2_sha.txt"
cmd.exe /c "chkdsk.exe $drive"
cmd.exe /c "fsutil volume dismount $drive"
"""
        self.run_guest_ps1(r2_script, "flow_c_round2.ps1")
        r2_sha = self.run_guest(
            f'powershell.exe -NoProfile -NonInteractive -Command "Get-Content \'{gdir}\\r2_sha.txt\'"'
        ).stdout.strip().lower()

        self.vm_powerdown()
        if self.cfg.linux_backend == "guestfish":
            self.run_guestfish(
                [
                    "run",
                    "vg-activate true pp_vg",
                    "debug sh 'lvextend -y -L +32M pp_vg/pp_main'",
                    "ntfsresize-opts /dev/pp_vg/pp_main force:true",
                    "mount /dev/pp_vg/pp_main /",
                    "write /round3_linux.txt 'ROUND_3_EXPANDED_BY_LINUX\n'",
                    "sync",
                    "umount /",
                    "vg-activate false pp_vg",
                ],
                [self.cfg.disk_a, self.cfg.disk_b],
            )
        else:
            loops = []
            mnt = self.mount_dir / "pp_test"
            mnt.mkdir(parents=True, exist_ok=True)
            try:
                loop_a = self.run_host(f"losetup -fP --show '{self.cfg.disk_a}'").stdout.strip()
                loop_b = self.run_host(f"losetup -fP --show '{self.cfg.disk_b}'").stdout.strip()
                loops = [loop_a, loop_b]
                self.run_host(f"vgchange -ay {DEVFILE} pp_vg")
                self.run_host(f"lvextend -y {DEVFILE} -L +32M pp_vg/pp_main")
                self.run_host("ntfsresize -f -b /dev/pp_vg/pp_main")
                self.run_host(f"mount /dev/pp_vg/pp_main '{mnt}'")
                self.run_host(f"printf 'ROUND_3_EXPANDED_BY_LINUX\\n' > '{mnt}/round3_linux.txt'")
                self.run_host(f"umount '{mnt}'")
                self.run_host(f"vgchange -an {DEVFILE} pp_vg")
            finally:
                self.cleanup_host_storage(loops)

        self.vm_start()
        r4_r5_script = f"""
$ErrorActionPreference = 'Stop'
sc.exe start winlvm 2>$null
& "{lvm}" vgscan
Start-Sleep -Seconds 5
$vol = Get-CimInstance Win32_Volume | Where-Object {{ $_.Label -eq "PP_NTFS" }}
if (-not $vol) {{ $vol = Get-CimInstance Win32_Volume | Where-Object {{ $_.Capacity -ge 70000000 }} }}
if ($vol.Capacity -lt 70000000) {{ Write-Error "Expanded volume capacity not observed: $($vol.Capacity)" }}
$drive = if ($vol.DriveLetter) {{ $vol.DriveLetter }} else {{ "D:" }}
if (((Get-Content "$drive\\round3_linux.txt").Trim()) -ne "ROUND_3_EXPANDED_BY_LINUX") {{ Write-Error "Round 3 text mismatch" }}
$disks = Get-Disk | Where-Object {{ $_.Size -ge 130000000 -and $_.Size -le 140000000 }} | Sort-Object Number
$d1 = "\\\\.\\PhysicalDrive$($disks[0].Number)"
$d2 = "\\\\.\\PhysicalDrive$($disks[1].Number)"
& "{lvm}" lvcreate -L 24M -i 2 -I 64k -n pp_stripe pp_vg $d1 $d2
if ($LASTEXITCODE -ne 0) {{ Write-Error "lvcreate pp_stripe failed" }}
& "{lvm}" vgscan
Start-Sleep -Seconds 5
Get-CimInstance Win32_Volume | Where-Object {{ $_.DriveLetter -and $_.DriveLetter -ne "C:" }} | ForEach-Object {{
    cmd.exe /c "fsutil volume dismount $($_.DriveLetter)"
}}
Start-Sleep -Seconds 2
& "{lvm}" pvmove -n pp_main $d1 $d2
if ($LASTEXITCODE -ne 0) {{ Write-Error "pvmove pp_main failed" }}
"""
        self.run_guest_ps1(r4_r5_script, "flow_c_round4_5.ps1")

        self.vm_powerdown()
        if self.cfg.linux_backend == "guestfish":
            gf_res = self.run_guestfish(
                [
                    "run",
                    "debug sh 'pvck /dev/sda && pvck /dev/sdb && vgck pp_vg'",
                    "vg-activate true pp_vg",
                    "ntfsfix /dev/pp_vg/pp_main",
                    "mount-ro /dev/pp_vg/pp_main /",
                    "cat /round1.txt",
                    "cat /round2_win.txt",
                    "cat /round3_linux.txt",
                    "checksum sha256 /r1_data.bin",
                    "checksum sha256 /r2_data.bin",
                    "umount /",
                    "vg-activate false pp_vg",
                ],
                [self.cfg.disk_a, self.cfg.disk_b],
            )
            out = gf_res.stdout
            for token in (
                "ROUND_1_SEED_PAYLOAD",
                "ROUND_2_APPENDED_ON_WINDOWS",
                "ROUND_3_EXPANDED_BY_LINUX",
                r1_sha,
                r2_sha,
            ):
                if token not in out.lower() and token not in out:
                    raise RuntimeError(f"Expected token {token!r} missing in Round 6 guestfish audit")
        else:
            loops = []
            mnt = self.mount_dir / "pp_final"
            mnt.mkdir(parents=True, exist_ok=True)
            try:
                loop_a = self.run_host(f"losetup -fP --show '{self.cfg.disk_a}'").stdout.strip()
                loop_b = self.run_host(f"losetup -fP --show '{self.cfg.disk_b}'").stdout.strip()
                loops = [loop_a, loop_b]
                self.run_host(f"pvck {DEVFILE} '{loop_a}' '{loop_b}'")
                self.run_host(f"vgck {DEVFILE} pp_vg")
                self.run_host(f"vgchange -ay {DEVFILE} pp_vg")
                self.run_host("ntfsfix -n /dev/pp_vg/pp_main")
                self.run_host(f"mount -o ro /dev/pp_vg/pp_main '{mnt}'")
                if self.run_host(f"cat '{mnt}/round1.txt'").stdout.strip() != "ROUND_1_SEED_PAYLOAD":
                    raise RuntimeError("Round 1 text corrupted")
                if self.run_host(f"cat '{mnt}/round2_win.txt'").stdout.strip() != "ROUND_2_APPENDED_ON_WINDOWS":
                    raise RuntimeError("Round 2 text corrupted")
                if self.run_host(f"cat '{mnt}/round3_linux.txt'").stdout.strip() != "ROUND_3_EXPANDED_BY_LINUX":
                    raise RuntimeError("Round 3 text corrupted")
                if self.run_host(f"sha256sum '{mnt}/r1_data.bin'").stdout.split()[0].lower() != r1_sha:
                    raise RuntimeError("Round 1 SHA256 mismatch")
                if self.run_host(f"sha256sum '{mnt}/r2_data.bin'").stdout.split()[0].lower() != r2_sha:
                    raise RuntimeError("Round 2 SHA256 mismatch")
                self.run_host(f"umount '{mnt}'")
                self.run_host(f"vgchange -an {DEVFILE} pp_vg")
            finally:
                self.cleanup_host_storage(loops)

        self.log("FLOW C: PASS")
        self.vm_start()


def main() -> int:
    cfg = parse_args()
    runner = Runner(cfg)
    runner.log(
        f"Starting bidirectional test matrix (backend={cfg.linux_backend}, "
        f"ssh={cfg.ssh_user}@{cfg.ssh_host}:{cfg.ssh_port}, work_dir={cfg.work_dir})"
    )
    start_time = time.time()

    runner.vm_start()
    runner.deploy_and_build_guest_tools()

    if cfg.flows in ("all", "a"):
        runner.test_flow_a()
    if cfg.flows in ("all", "b"):
        runner.test_flow_b()
    if cfg.flows in ("all", "c"):
        runner.test_flow_c()

    elapsed = time.time() - start_time
    runner.log(f"ALL REQUESTED BIDIRECTIONAL FLOWS ({cfg.flows}) PASSED IN {elapsed:.1f}s.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
