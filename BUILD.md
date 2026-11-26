# Building and Installing winlvm

WinLVM has two build surfaces:

1. **CMake user-mode & host-test build** — builds `lvmcore`, `lvmfmt`,
   `winlvm_service_protocol`, `winlvm_installer_archive`, `lvm`, `lvmdump`,
   `winlvm_svc`, Windows GUI (`winlvm_gui.exe`), installer/uninstaller
   (`winlvm_setup_stub.exe`, `winlvm-uninstall.exe`), and all 7 automated CTest
   suites on Linux or Windows.
2. **WDK MSBuild kernel driver build** — builds `winlvm.sys` and its driver
   packages (`winlvm.inf` / `winlvm-vol.inf`) on Windows x64.

---

## 1. User-Mode Libraries, Tools, GUI, Installer, and Tests (CMake)

### Prerequisites

- CMake 3.24 or newer
- Ninja (recommended) or Visual Studio / Make
- A C11 and C++20 compiler (GCC, Clang, MSVC, or MinGW-w64)
- Python 3 (standard library only; invokes `scripts/gen_test_fixtures.py` at
  build time to generate deterministic LVM2/LUKS2 test images out-of-tree into
  `${CMAKE_BINARY_DIR}/corpus-images/`, and `scripts/pack_installer.py` when
  packaging `winlvm-setup-x64.exe`)

### Linux (GCC / Clang)

Install build prerequisites (Debian/Ubuntu example):

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build python3
```

Configure, build, and run all 7 CTest suites:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On non-MSVC compilers, `lvmcore` and the portable kernel VG/segment C11 modules
(`winlvm_vg.c`, `winlvm_map.c`, `winlvm_thin.c`, `winlvm_snap.c`,
`winlvm_raid.c`) are compiled with `-Wframe-larger-than=512` to enforce bounded
kernel stack frames.

### Windows (MSVC)

Open an **x64 Native Tools Command Prompt for VS 2022** (with Python 3 on
`PATH`) and run:

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

### Windows Cross-Compilation (MinGW-w64)

When compiling for `WIN32` with MinGW-w64, `CMakeLists.txt` automatically links
`-static -static-libgcc -static-libstdc++` so all 6 Windows executables run
standalone without MSYS2/MinGW runtime DLLs:

```bash
cmake -B build-mingw -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
  -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
  -DCMAKE_C_FLAGS="-Werror" \
  -DCMAKE_CXX_FLAGS="-Werror" \
  -DCMAKE_EXE_LINKER_FLAGS="-static"
cmake --build build-mingw --target lvm lvmdump winlvm_svc winlvm_gui winlvm_setup_stub winlvm_uninstall
```

### Produced User-Mode Artifacts

| Target | Output | Purpose |
| --- | --- | --- |
| `lvmcore` | Static library | Freestanding C11 LVM2 parser, serializer, and dm-thin / dm-snapshot / raid1-mirror block engines |
| `lvmfmt` | Static library | Portable C++20 LVM2 format, mapping, allocator, and two-phase MDA commit engine |
| `lvm` | `lvm` / `lvm.exe` | LVM2 control-plane CLI (`pvs`/`vgs`/`lvs`, `fullreport`, JSON, tags, `pvchange`, `lvconvert`, `lvmconfig`) |
| `lvmdump` | `lvmdump` / `lvmdump.exe` | Standalone PV/VG metadata and DM-table dumper |
| `winlvm_svc` | `winlvm_svc` / `winlvm_svc.exe` | Windows SCM named-pipe broker (`\\.\pipe\winlvm-svc`) |
| `winlvm_gui` | `winlvm_gui.exe` (Windows) | Native Win32 desktop & system-tray volume manager |
| `winlvm_setup_stub` | `winlvm_setup_stub.exe` (Windows) | Native Win32 installer stub packed into `winlvm-setup-x64.exe` |
| `winlvm_uninstall` | `winlvm-uninstall.exe` (Windows) | Native Win32 uninstaller |

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

This outputs the driver package under `src\driver\x64\Release\` (or
`$(SolutionDir)x64\Release\` when built from a solution).

### Sources Compiled by `src/driver/winlvm.vcxproj`

- **Modular C11 WDM driver (`src/driver/`)** (every module kept under 500 LOC):
  - `winlvm.c` — `DriverEntry`, `WinLvmUnload`, ACL-hardened `\Device\WinLvmControl`
    (`IoCreateDeviceSecure` + `SDDL_DEVOBJ_SYS_ALL_ADM_ALL`), ring logger, WMI
    provider, and top-level IRP dispatch
  - `winlvm_pnp.c` — bus FDO PnP lifecycle, `BusRelations`, PV tag teardown, and
    live child geometry reconciliation
  - `winlvm_probe.c` — PnP disk/volume interface notification lifecycle, link
    filtering, cold partition retry, and unified PV probing
  - `winlvm_notify.c` — refcounted target-device PnP removal notifications,
    `IoWorkItem` teardown, and Mount Manager arrival notification
  - `winlvm_vg_ops.c` — recent PV path tracking, probe arrival commit, child PDO
    rebinding, and LV activation/deactivation
  - `winlvm_control.c` — `\Device\WinLvmControl` IOCTL dispatch plane
  - `winlvm_child.c` — child LV raw-PDO creation, PnP dispatch, read-only policy,
    and teardown
  - `winlvm_gate.c` — cancel-safe I/O admission and quiesce gate (`IO_CSQ`)
  - `winlvm_power.c` — WDM power IRP dispatch and system/device state tracking
  - `winlvm_dispatch_io.c` — `IRP_MJ_READ`, `IRP_MJ_WRITE`,
    `IRP_MJ_FLUSH_BUFFERS`, and `IRP_MJ_SHUTDOWN` dispatch (fast-path pass-through
    and complex-segment work-item routing)
  - `winlvm_subirp.c` — multi-piece sub-IRP fan-out, partial MDL / bounce-buffer
    handling, and completion aggregation
  - `winlvm_sector.c` — backing-disk geometry, writability, GPT/MBR partition
    layout queries, metadata reads, and LV-to-PV piece mapping
  - `winlvm_mount.c` — Mount Manager (`IOCTL_MOUNTDEV_*`) link tracking and
    volume FSCTL handlers
  - `winlvm_storage.c` — Storage, Disk, and Volume IOCTL handlers
  - `winlvm_vg.c`, `winlvm_map.c` — freestanding multi-VG registry, PV arrival /
    `seqno` state machine, streaming extent mapper, and sub-LV slot collector
  - `winlvm_thin.c`, `winlvm_snap.c`, `winlvm_raid.c` — kernel PV I/O bridge,
    complex-segment dispatchers, and `PASSIVE_LEVEL` work-item I/O runner
- **Freestanding core (`src/lvmcore/`)**: `crc32.c`, `arena.c`, `parser.c`,
  `lvmcore_thin.c`, `lvmcore_snap.c`, `lvmcore_raid.c`
- **Shared ABI headers**: `src/winlvm_ioctl.h`, `src/winlvm_types.h`
- **Driver packages**: `winlvm.inf` (bus FDO, `ROOT\winlvm`, `Class=System`) and
  `winlvm-vol.inf` (raw child LV PDOs, `WinLvm\Volume` / `WinLvmVolume`,
  `Class=Volume`, null service)
- **Kernel link libraries**: `ntoskrnl.lib`, `hal.lib`, `wmilib.lib`, `wdmsec.lib`

---

## 3. Packaging & Installing on Windows

### Standalone Installer (`winlvm-setup-x64.exe`)

Stage the user-mode binaries (`bin/lvm.exe`, `bin/lvmdump.exe`,
`bin/winlvm_svc.exe`, `bin/winlvm_gui.exe`), `winlvm-uninstall.exe`, signed
driver packages (`driver/winlvm.sys`, `driver/winlvm.inf`, `driver/winlvm.cat`,
`driver/vol/winlvm-vol.inf`, `driver/vol/winlvm-vol.cat`, and optional
`driver/winlvm.cer`), `LICENSE`, and `NOTICE`, then pack and verify:

```bash
python3 scripts/pack_installer.py \
  --stub build/winlvm_setup_stub.exe \
  --stage-dir dist/stage \
  --out dist/winlvm-setup-x64.exe
python3 scripts/pack_installer.py --verify-only dist/winlvm-setup-x64.exe
```

Running `winlvm-setup-x64.exe` on Windows x64 installs binaries into
`%ProgramFiles%\WinLVM`, registers PATH and Start Menu shortcuts, stages
`winlvm-vol.inf` and `winlvm.inf` (`ROOT\winlvm`), installs and starts
`winlvm-svc`, and writes an Uninstall entry backed by `winlvm-uninstall.exe`.
Silent automation flags (`--silent`, `--verify-only`, `--no-driver`,
`--no-service`) are also supported.

### Manual Driver Installation (Test-Signed)

Use a dedicated test VM or test machine with test signing enabled:

```powershell
bcdedit /set testsigning on
# Reboot, then trust the signing certificate:
certutil -addstore Root <path-to>\winlvm.cer
certutil -addstore TrustedPublisher <path-to>\winlvm.cer

# Stage winlvm-vol.inf first so raw LV PDOs bind immediately:
Inf2Cat.exe /driver:C:\winlvm\vol_pkg /os:10_X64
signtool.exe sign /ph /fd sha256 /sha1 <cert-thumbprint> C:\winlvm\vol_pkg\winlvm-vol.cat
pnputil.exe /add-driver C:\winlvm\vol_pkg\winlvm-vol.inf /install

# Install the root bus devnode:
devcon.exe install <driver-out>\winlvm\winlvm.inf "ROOT\winlvm"
```

Verify installation and enumerate volumes:

```powershell
devcon.exe status "ROOT\winlvm"
devcon.exe status "WinLvm\Volume*"
lvm.exe version
lvm.exe vgscan
```

To remove the bus devnode manually:

```powershell
devcon.exe remove "ROOT\winlvm"
```

---

## 4. Test Fixtures (`scripts/gen_test_fixtures.py`)

CMake invokes
`scripts/gen_test_fixtures.py` during the build to generate all 16 LVM2/LUKS2
test images into `${CMAKE_BINARY_DIR}/corpus-images/`, matching the lightweight
text/hash oracles under `corpus/expected/`. See [`corpus/README.md`](corpus/README.md)
for details.

---

## 5. Optional Pre-Commit Hook

A POSIX `sh` pre-commit hook at [`scripts/hooks/pre-commit`](scripts/hooks/pre-commit)
checks staged changes for whitespace errors (`git diff --cached --check`),
guards against committing raw disk images, VM/key artifacts, or unintended
`third_party/` submodule pointer changes, and warns on oversized multi-area
commits:

```bash
cp scripts/hooks/pre-commit .git/hooks/pre-commit && chmod +x .git/hooks/pre-commit
```
