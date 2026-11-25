# Building and Installing winlvm

WinLVM has two build surfaces:

1. **CMake user-mode & host-test build** — builds `lvmcore`, `lvmfmt`,
   `winlvm_service_protocol`, `lvm`, `lvmdump`, `winlvm_svc`, and all automated
   unit, parser, hardening, serialization, VG state-machine, CLI, IOCTL, and
   corpus oracle tests on Linux or Windows.
2. **WDK MSBuild kernel driver build** — builds `winlvm.sys` and its driver
   packages (`winlvm.inf` / `winlvm-vol.inf`) on Windows x64.

---

## 1. User-Mode Libraries, Tools, and Tests (CMake)

### Prerequisites

- CMake 3.24 or newer
- Ninja (recommended) or Visual Studio / Make
- A C11 and C++20 compiler (GCC, Clang, MSVC, or MinGW-w64)
- `zstd` CLI on `PATH` (decompresses `corpus/images/*.img.zst` at build time;
  without `zstd`, CMake warns and omits the corpus-dependent CTest targets)

### Linux (GCC / Clang)

Install build prerequisites (Debian/Ubuntu example):

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build zstd
```

Configure, build, and run the full test suite:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On non-MSVC compilers, `lvmcore` is compiled with `-Wframe-larger-than=512` to
enforce bounded kernel stack frames.

### Windows (MSVC)

1. Install `zstd` and ensure it is on `PATH` (for example,
   `choco install zstandard -y` or `winget install facebook.zstd`).
2. Open an **x64 Native Tools Command Prompt for VS 2022** and run:

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
cmake --build build
ctest --test-dir build --output-on-failure
```

Alternatively, with the Visual Studio multi-config generator:

```powershell
cmake -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### Windows (MinGW-w64)

When compiling with MinGW-w64 (`WIN32` and non-MSVC), `CMakeLists.txt`
automatically links `-static -static-libgcc -static-libstdc++` so `lvm.exe`,
`lvmdump.exe`, and `winlvm_svc.exe` run standalone without MSYS2 runtime DLLs:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Produced User-Mode Artifacts

| Target | Output | Purpose |
| --- | --- | --- |
| `lvmcore` | Static library | Freestanding C11 LVM2 parser, arena, CRC-32, serializer |
| `lvmfmt` | Static library | Portable C++20 LVM2 format, mapping, allocator, commit engine |
| `lvm` | `lvm` / `lvm.exe` | LVM2 control-plane CLI |
| `lvmdump` | `lvmdump` / `lvmdump.exe` | Standalone PV/VG metadata and DM-table dumper |
| `winlvm_svc` | `winlvm_svc` / `winlvm_svc.exe` | Windows SCM named-pipe broker (`\\.\pipe\winlvm-svc`) |

---

## 2. WDM Kernel Driver (`winlvm.sys`, Windows x64)

`winlvm.sys` is a native Windows x64 WDM driver built with MSBuild and the
Windows Driver Kit (WDK); it is not built by CMake.

### Prerequisites

- Visual Studio 2022 with **Desktop development with C++**
  (`<SpectreMitigation>false</SpectreMitigation>` is set in
  `src/driver/winlvm.vcxproj`, so Spectre-mitigated libraries are not required
  for out-of-the-box WDK builds)
- **Windows Driver Kit (WDK)** matching your Windows SDK version (providing the
  `WindowsKernelModeDriver10.0` toolset, `Inf2Cat.exe`, `signtool.exe`, and
  `devcon.exe`)

### Build Command

From an **x64 Native Tools Command Prompt for VS 2022**:

```powershell
msbuild src\driver\winlvm.vcxproj /p:Configuration=Release /p:Platform=x64
```

This outputs the driver package (including `winlvm.sys`, `winlvm.inf`, and
`winlvm.cat`) under `src\driver\x64\Release\` (or
`$(SolutionDir)x64\Release\` when built from a solution).

### Sources Compiled by `src/driver/winlvm.vcxproj`

- **Modular C11 WDM driver (`src/driver/`)**:
  - `winlvm.c` — `DriverEntry`, `WinLvmUnload`, ACL-hardened `\Device\WinLvmControl`
    (`IoCreateDeviceSecure` + `SDDL_DEVOBJ_SYS_ALL_ADM_ALL`), ring logger, WMI
    provider, and top-level IRP dispatch
  - `winlvm_pnp.c` — bus FDO PnP lifecycle, `BusRelations`, PV tag teardown, and
    live child geometry reconciliation
  - `winlvm_probe.c` — PnP disk/volume interface notification lifecycle, link
    filtering, and unified PV probing (PnP and `IOCTL_WINLVM_PROBE_DEVICE`)
  - `winlvm_control.c` — `\Device\WinLvmControl` IOCTL dispatch plane
  - `winlvm_child.c` — child LV raw-PDO creation, PnP dispatch, read-only policy,
    and teardown
  - `winlvm_gate.c` — cancel-safe I/O admission and quiesce gate (`IO_CSQ`)
  - `winlvm_power.c` — WDM power IRP dispatch for control, bus FDO, and LV PDOs
  - `winlvm_dispatch_io.c` — `IRP_MJ_READ`, `IRP_MJ_WRITE`,
    `IRP_MJ_FLUSH_BUFFERS`, and `IRP_MJ_SHUTDOWN` dispatch and aligned pass-through
  - `winlvm_subirp.c` — multi-piece sub-IRP fan-out, partial MDL / bounce-buffer
    handling, and completion aggregation
  - `winlvm_sector.c` — backing-disk geometry, writability, and GPT/MBR
    partition layout queries (`IOCTL_DISK_GET_DRIVE_LAYOUT_EX`), synchronous
    sector-aligned metadata reads, and LV-to-PV piece mapping
  - `winlvm_mount.c` — Mount Manager (`IOCTL_MOUNTDEV_*`) and volume FSCTL handlers
  - `winlvm_storage.c` — Storage, Disk, and Volume IOCTL handlers
  - `winlvm_vg.c` — freestanding multi-VG registry and PV arrival / `seqno` state machine
  - `winlvm_map.c` — freestanding streaming LV-byte to PV-extent mapper
- **Freestanding parser (`src/lvmcore/`)**: `crc32.c`, `arena.c`, `parser.c`
- **Shared ABI headers**: `src/winlvm_ioctl.h`, `src/winlvm_types.h`
- **Driver packages**: `winlvm.inf` (bus FDO, `ROOT\winlvm`, `Class=System`) and
  `winlvm-vol.inf` (raw child LV PDOs, `WinLvm\Volume` / `WinLvmVolume`,
  `Class=Volume`, null service)
- **Kernel link libraries**: `ntoskrnl.lib`, `hal.lib`, `wmilib.lib`, `wdmsec.lib`

---

## 3. Installing and Running the Driver on Windows (Test-Signed)

Use a dedicated test VM or test machine with test signing enabled.

### 1. Enable Test Signing and Trust the Test Certificate

From an elevated Command Prompt or PowerShell:

```powershell
bcdedit /set testsigning on
```

Reboot, then install the test certificate generated by the WDK build into both
machine trust stores:

```powershell
certutil -addstore Root <path-to>\winlvm.cer
certutil -addstore TrustedPublisher <path-to>\winlvm.cer
```

### 2. Package and Stage `winlvm-vol.inf` First

Child LV devices are raw PDOs (`RawDeviceOK=TRUE`) bound by `winlvm-vol.inf`
with an empty `AddService` directive so `winlvm.sys` handles all child IRPs
directly. Stage `winlvm-vol.inf` before creating the bus devnode so PnP binds
enumerated LV children immediately.

If packaging `winlvm-vol.inf` into a standalone directory (for example
`C:\winlvm\vol_pkg` containing `winlvm-vol.inf`):

```powershell
Inf2Cat.exe /driver:C:\winlvm\vol_pkg /os:10_X64
signtool.exe sign /ph /fd sha256 /sha1 <cert-thumbprint> C:\winlvm\vol_pkg\winlvm-vol.cat
pnputil.exe /add-driver C:\winlvm\vol_pkg\winlvm-vol.inf /install
```

### 3. Install the Root Bus Devnode via `devcon`

`winlvm.inf` targets the root-enumerated hardware ID `ROOT\winlvm`
(`Class=System`) and has no `[DefaultInstall]` section; use WDK `devcon.exe`
rather than right-click Install or `sc create`:

```powershell
devcon.exe install <driver-out>\winlvm\winlvm.inf "ROOT\winlvm"
```

### 4. Verify Installation and Enumerated Volumes

```powershell
devcon.exe status "ROOT\winlvm"
devcon.exe status "WinLvm\Volume*"
lvm.exe version
lvm.exe vgscan
```

- Backing PVs on PnP disks and partitions (`GUID_DEVINTERFACE_DISK` /
  `GUID_DEVINTERFACE_VOLUME`) are discovered automatically after bus startup,
  including automatic GPT/MBR partition enumeration via
  `IOCTL_DISK_GET_DRIVE_LAYOUT_EX` on whole disks (covering Linux LVM GPT/MBR
  partitions that lack `GUID_DEVINTERFACE_VOLUME`).
- For unlocked `WinLUKS` devices exposed as non-PnP NT device objects (such as
  `\Device\WinLuksVolumeN`), run `lvm.exe vgscan` or `lvm.exe pvscan` from an
  elevated shell so the CLI probes them into `winlvm.sys` via
  `IOCTL_WINLVM_PROBE_DEVICE`.

### 5. Uninstall

```powershell
devcon.exe remove "ROOT\winlvm"
```

---

## 4. Windows Control Broker Service (`winlvm_svc.exe`)

To run the optional SCM service broker (`\\.\pipe\winlvm-svc`), register and
start it from an elevated prompt:

```powershell
sc.exe create winlvm-svc binPath= "<path-to>\winlvm_svc.exe" start= demand
sc.exe start winlvm-svc
```

---

## 5. Regenerating the Ground-Truth Corpus

The committed `.img.zst` fixtures and Linux oracle files under `corpus/` can be
regenerated on a Linux VM with `lvm2`, `cryptsetup`, `python3`, `zstd`, and
`e2fsprogs` installed. See [`corpus/README.md`](corpus/README.md) for usage and
disk-space details.

---

## 6. Optional Pre-Commit Hook

A POSIX `sh` pre-commit hook at [`scripts/hooks/pre-commit`](scripts/hooks/pre-commit)
checks staged changes for whitespace errors (`git diff --cached --check`),
guards against committing raw disk images, VM/key artifacts, or unintended
`third_party/` submodule pointer changes, and warns on oversized multi-area
commits:

```bash
cp scripts/hooks/pre-commit .git/hooks/pre-commit && chmod +x .git/hooks/pre-commit
```
