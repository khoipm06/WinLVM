# winlvm

Native Windows access and volume management for Linux LVM2 storage: a WDM
software-bus kernel driver (`winlvm.sys`), freestanding C11 metadata and
complex-segment block engines (`lvmcore`), a portable C++20 on-disk format and
transaction engine (`lvmfmt`), a comprehensive LVM2 CLI (`lvm.exe`), a
diagnostic metadata dumper (`lvmdump.exe`), a Windows control-plane service
(`winlvm_svc.exe`), a native Win32 desktop and system-tray manager
(`winlvm_gui.exe`), and a standalone Windows installer (`winlvm-setup-x64.exe`)
and uninstaller (`winlvm-uninstall.exe`).

## Architecture

```text
User Mode
  winlvm_gui.exe            Native Win32 desktop manager & system tray
  lvm.exe / lvmdump.exe     LVM2 inspection, metadata mutation, and driver control
  winlvm_svc.exe            SCM service broker (\\.\pipe\winlvm-svc, SYSTEM/Admins)
        │  DeviceIoControl (\\.\WinLvmControl, SDDL_DEVOBJ_SYS_ALL_ADM_ALL)
        ▼
Kernel Mode
  winlvm.sys                WDM software-bus driver (ROOT\winlvm, Class=System)
    ├─ lvmcore (C11)        Zero-allocation, bounded-stack LVM2 parser and on-disk
    │                       dm-thin, dm-snapshot, and raid1/mirror block engines
    ├─ PnP & Power          GUID_DEVINTERFACE_{DISK,VOLUME}, target-device removal
    │                       notifications, WDM S/D power tracking, MountMgr arrival
    ├─ VG Registry          Multi-VG / multi-PV arrival and seqno reconciliation
    ├─ Child LV PDOs        Raw FILE_DEVICE_DISK PDOs (Class=Volume, GUID_DEVINTERFACE_VOLUME)
    │                       Mount Manager (IOCTL_MOUNTDEV_*) & Storage/Disk/Volume IOCTLs
    └─ I/O Data Path        Aligned pass-through, partial-MDL sub-IRP fan-out, and
                            PASSIVE_LEVEL complex-segment worker dispatch (never ZwCreateFile)
        │  IoCallDriver (retained lower PDEVICE_OBJECT / PFILE_OBJECT)
        ▼
Backing Storage
  Physical disks, disk partitions, or unlocked WinLUKS cleartext block devices
```

### Core Components

- **`winlvm.sys` (`src/driver/`)** — Root-enumerated WDM software-bus driver
  (`ROOT\winlvm`, installed via `winlvm.inf`). Discovers candidate physical
  volumes through PnP device-interface notifications (`GUID_DEVINTERFACE_DISK`
  and `GUID_DEVINTERFACE_VOLUME`), automatic GPT/MBR partition scanning, and
  explicit probes (`IOCTL_WINLVM_PROBE_DEVICE`), reads PV labels and metadata
  via `IoGetDeviceObjectPointer` + `IoCallDriver`, tracks target-device removal
  notifications and power transitions, and enumerates one raw volume PDO
  (`WinLvm\Volume`, bound with a null service via `winlvm-vol.inf`) per
  user-visible, PV-complete logical volume. Mount Manager assigns drive letters
  so native or third-party filesystems (such as NTFS or WinBtrfs) mount
  directly.
- **`lvmcore` (`src/lvmcore/`)** — Freestanding C11 LVM2 label, MDA ring-buffer,
  text-metadata parser/serializer, and portable block engines (`dm-thin`,
  `dm-snapshot`, `raid1`/`mirror`) compiled into both `winlvm.sys` and host test
  binaries. Performs no dynamic heap allocations, uses non-recursive parsing,
  and keeps stack frames at or below 512 bytes.
- **`lvmfmt` (`src/lvmfmt/`)** — Platform-agnostic C++20 LVM2 on-disk format
  library implementing LVM2's weak CRC-32 (`0xedb88320`, init `0xf597a6cf`),
  `LABELONE` / `pv_header` / `pv_header_extension` v2 handling, multi-MDA
  circular ring-buffer reads and two-phase metadata commits, text metadata
  parsing and serialization (including tags and composite segment layouts),
  extent allocation, overflow-checked extent-to-sector mapping, and
  sector-aligned block I/O.
- **`lvm` / `lvm.exe` (`src/tools/lvm/`)** — LVM2 control-plane CLI supporting
  offline disk-image operations (Linux and Windows) and live Windows device
  discovery, driver probing, activation, resizing, and metadata management:
  - PV commands: `pvcreate`, `pvremove`, `pvs`, `pvdisplay`, `pvscan`,
    `pvchange`, `pvresize`, `pvmove`, `pvck`
  - VG commands: `vgcreate`, `vgremove`, `vgs`, `vgdisplay`, `vgscan`,
    `vgchange`, `vgextend`, `vgreduce`, `vgrename`, `vgcfgbackup`,
    `vgcfgrestore`, `vgck`, `vgexport`, `vgimport`
  - LV commands: `lvcreate` (`linear`, `striped`, `thin-pool`, `thin`,
    `snapshot`), `lvremove`, `lvs`, `lvdisplay`, `lvscan`, `lvchange`,
    `lvconvert` (`raid1`, `mirror`, `linear`, `--merge`), `lvextend`,
    `lvreduce`, `lvresize`, `lvrename`
  - Reporting & utility commands: `fullreport`, `--reportformat json` /
    `json_std`, `tags`, `lvmconfig` / `dumpconfig` / `config`, `lvmdiskscan`,
    `formats`, `segtypes`, `systemid`, `version`, `help`
- **`lvmdump` / `lvmdump.exe` (`src/tools/lvmdump/`)** — Standalone inspection
  utility that prints PV label metadata, MDA offsets, raw VG metadata
  (`--metadata`), and computed Linux-compatible device-mapper tables.
- **`winlvm_svc` / `winlvm_svc.exe` (`src/service/`)** — Windows Service
  Control Manager host (`winlvm-svc`) exposing an ACL-restricted named pipe
  (`\\.\pipe\winlvm-svc`, LocalSystem and Built-in Administrators only) with a
  versioned framed JSON protocol for live topology discovery, volume
  activation/deactivation, drive-letter assignment, rescanning, and metadata
  management.
- **`winlvm_gui.exe` (`src/gui/`)** — Native Win32 desktop and system-tray
  volume manager (DPI-aware, zero third-party GUI frameworks) communicating with
  `winlvm_svc` or directly with the Win32 backend for topology inspection, LV
  activation, drive-letter management, creation/resizing/conversion dialogs, and
  quick tray navigation.
- **`winlvm-setup-x64.exe` & `winlvm-uninstall.exe` (`src/installer/`,
  `scripts/pack_installer.py`)** — Self-contained Windows x64 installer with an
  integrity-checked trailer archive format (installing binaries, PATH entry,
  driver packages, and `winlvm-svc`) and matching clean uninstaller.

## Supported Scope

- **Logical volume layouts**:
  - `linear` (single- and multi-segment, including discontiguous non-monotonic
    segments) and `striped` (multi-stripe round-robin with configurable stripe
    counts and power-of-two stripe sizes) across single-PV and multi-PV volume
    groups.
  - `thin-pool` and `thin` (including thin snapshots): full `dm-thin` metadata
    superblock, 2-level B-tree data mappings, space-map block allocation, CoW
    writes with leaf splits, and CRC32c verification validated against Linux
    `thin_check`.
  - `snapshot` and `snapshot-origin`: classic `dm-snapshot` exception tables
    supporting consistent reads and CoW writes on both snapshot and origin
    volumes, plus offline `lvconvert --merge`.
  - `raid1` and `mirror`: read failover across surviving legs (including
    degraded read-only access when a leg PV is absent) and multi-leg write
    fan-out that fails closed if any leg is missing so mirror legs never diverge
    silently.
- **Metadata & tags**: Lossless parsing and serialization of PV, VG, and LV
  `tags`, `status`, and `flags` across both `lvmcore` and `lvmfmt`. Internal
  sub-volumes (`*_tmeta`, `*_tdata`, `*_pmspare`, `*_rimage_*`, `*_rmeta_*`,
  `*_mimage_*`, `*_mlog`, `*_cow`, `snapshot<N>` wrappers, and `thin-pool`
  containers) remain hidden from Windows Mount Manager enumeration while
  user-visible volumes mount cleanly.
- **Sector geometries & PV layouts**: Both 512e and 4Kn logical/physical sector
  geometries (propagating the strictest member sector alignment to exposed LV
  geometry and using aligned bounce buffers when required), whole-disk and
  GPT/MBR-partitioned PVs (including Linux LVM partition types lacking Windows
  volume interfaces), multiple metadata areas (`#PMda >= 1`), wrapped circular
  MDA ring buffers, and `pv_header_extension` v2 (`PV_EXT_USED`).
- **Two-phase circular MDA metadata commits**: Metadata updates write the new
  text payload into free ring space without overwriting the active copy,
  publish `raw_locns[1]` (precommit), then promote to `raw_locns[0]` and zero
  `raw_locns[1]` (commit) across every active (non-ignored) MDA on every member
  PV, rolling back prior MDA headers on I/O failure.
- **Live driver control plane (`\Device\WinLvmControl`)**: Administrator-only
  IOCTLs (`IOCTL_WINLVM_INVENTORY`, `IOCTL_WINLVM_REFRESH`,
  `IOCTL_WINLVM_QUIESCE_VG`, `IOCTL_WINLVM_RESUME_VG`,
  `IOCTL_WINLVM_ACTIVATE_LV`, `IOCTL_WINLVM_DEACTIVATE_LV`,
  `IOCTL_WINLVM_UPDATE_LV`, `IOCTL_WINLVM_GET_LOG`, and
  `IOCTL_WINLVM_PROBE_DEVICE`) coordinated with a cancel-safe I/O quiesce gate
  for online geometry reconciliation.
- **Decoupled `LVM-on-LUKS` stacking with `WinLUKS`**: `winlvm.sys` contains no
  cryptography and operates strictly on cleartext block devices. When an
  independent LUKS2 provider such as `WinLUKS` exposes an unlocked cleartext
  device via PnP `GUID_DEVINTERFACE_DISK` / `GUID_DEVINTERFACE_VOLUME`,
  `winlvm.sys` probes it automatically on arrival. For unlocked devices exposed
  as non-PnP NT device objects (such as `\Device\WinLuksVolumeN`), running
  `lvm vgscan` or `lvm pvscan` discovers the unlocked device and submits its NT
  path to `winlvm.sys` via `IOCTL_WINLVM_PROBE_DEVICE`.

## Fail-Closed Boundaries

WinLVM fails closed rather than guessing or silently corrupting on-disk state:

- **Unsupported segment types**: `raid4`, `raid5`, `raid6`, `raid10`, `cache`,
  `cache-pool`, `writecache`, `integrity`, `vdo`, and unknown segment types are
  parsed, identified, and refused safely on a per-LV basis. Supported LVs in the
  same VG remain accessible, while metadata serialization refuses to rewrite VGs
  containing unsupported layouts or unrecognized internal sub-volumes.
- **Write protection**: Any LV whose VG declares a non-empty `system_id`
  (foreign host ownership), whose VG or LV status omits `WRITE` (`READ`-only),
  whose backing PV is read-only, or whose `raid1`/`mirror` layout is missing a
  leg is mounted read-only; write requests fail with
  `STATUS_MEDIA_WRITE_PROTECTED`.
- **Degraded VGs & malformed metadata**: Linear, striped, thin, and snapshot LVs
  missing any required backing PV are not activated; surprise removal of a
  backing PV marks affected LVs unavailable. CRC mismatches, invalid offsets,
  integer overflows, out-of-bounds area descriptors, non-contiguous segments,
  and encrypted non-PV headers (such as raw locked LUKS2 containers) are
  rejected.
- **Out of scope**: Clustered LVM / `lvmlockd`, concurrent read-write mounting
  of the same VG by Linux and Windows simultaneously, boot-start driver loading
  (`SERVICE_BOOT_START`), crash-dump/hibernate filtering, and UEFI pre-boot
  drivers.

## Repository Layout

```text
src/
  winlvm_ioctl.h          Shared user/kernel control-plane IOCTL ABI
  winlvm_types.h          Shared fixed-width ABI primitives
  driver/                 WDM kernel driver (winlvm.sys, modular C11, winlvm.inf, winlvm-vol.inf)
  lvmcore/                Freestanding C11 parser, arena, CRC-32/CRC-32c, serializer,
                          and dm-thin / dm-snapshot / raid1-mirror block engines
  lvmfmt/                 Portable C++20 LVM2 format, mapping, allocator, and commit library
  tools/lvm/              lvm / lvm.exe CLI control plane
  tools/lvmdump/          lvmdump / lvmdump.exe diagnostic table & metadata dumper
  service/                winlvm_svc / winlvm_svc.exe SCM named-pipe broker & Win32 backend
  gui/                    winlvm_gui.exe native Win32 desktop & system-tray application
  installer/              winlvm_setup_stub.exe & winlvm-uninstall.exe native Win32 installer

scripts/
  gen_test_fixtures.py    Deterministic out-of-tree Python 3 LVM2/LUKS2 fixture generator
  pack_installer.py       Trailer-archive installer packager & integrity verifier
corpus/                   Expected LVM2/LUKS2 metadata, DM-table, and SHA-256 oracles
tests/                    Unit, parser, hardening, serialization, VG state-machine, CLI,
                          service, installer archive, storage/mount IOCTL, and corpus oracle suites
docs/                     Architecture decisions and engineering roadmap
third_party/              Upstream reference submodules (lvm2, winbtrfs) — not built or linked
```

## Quick Start

### Build and Test User-Mode Components (Linux or Windows)

Requires CMake 3.24+, Ninja (or Visual Studio), a C11/C++20 compiler, and
Python 3 (used automatically at build time to generate deterministic LVM2/LUKS2
test fixtures out-of-tree in `${CMAKE_BINARY_DIR}/corpus-images/`).

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`ctest` runs all 7 suites (`test_lvmcore_hardening`, `test_winlvm_vg`,
`test_lvmcore_parser`, `test_lvmcore_serialize`, `test_driver_corpus_map`,
`winlvm_tests`, and `test_installer_archive`).

### Inspect an LVM2 Disk Image

```bash
./build/lvmdump build/corpus-images/striped-2pv-a.img
./build/lvm --image build/corpus-images/thin-pool.img lvs --reportformat json
```

### Build and Install on Windows

See [`BUILD.md`](BUILD.md) for Linux, Windows (MSVC/MinGW-w64), WDK kernel
driver build instructions, standalone installer packaging
(`winlvm-setup-x64.exe`), and test-signing installation procedures.

## Documentation

- [`BUILD.md`](BUILD.md) — Building user-mode tools, GUI, installer, and
  `winlvm.sys`, running CTest, and installing on Windows.
- [`corpus/README.md`](corpus/README.md) — Dynamic out-of-tree LVM2/LUKS2 test
  fixture generator (`scripts/gen_test_fixtures.py`) and text oracle layout.
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — Implemented capabilities, remaining
  validation/boot milestones, and out-of-scope boundaries.

## License

Licensed under the GNU Lesser General Public License v3.0 or later. See
[`LICENSE`](LICENSE) and [`NOTICE`](NOTICE).
