# winlvm

Native Windows access and volume management for Linux LVM2 storage: a WDM
software-bus kernel driver (`winlvm.sys`), a freestanding C11 metadata parser
(`lvmcore`), a portable C++20 on-disk format and transaction engine (`lvmfmt`),
an LVM2 CLI (`lvm.exe`), a diagnostic metadata dumper (`lvmdump.exe`), and a
Windows control-plane service (`winlvm_svc.exe`).

## Architecture

```text
User Mode
  lvm.exe / lvmdump.exe     LVM2 inspection, metadata mutation, and driver control
  winlvm_svc.exe            SCM service broker (\\.\pipe\winlvm-svc, SYSTEM/Admins)
        │  DeviceIoControl (\\.\WinLvmControl, SDDL_DEVOBJ_SYS_ALL_ADM_ALL)
        ▼
Kernel Mode
  winlvm.sys                WDM software-bus driver (ROOT\winlvm, Class=System)
    ├─ lvmcore (C11)        Zero-allocation, bounded-stack LVM2 label/MDA/text parser
    ├─ PnP & Probe          GUID_DEVINTERFACE_{DISK,VOLUME} + IOCTL_WINLVM_PROBE_DEVICE
    ├─ VG Registry          Multi-VG / multi-PV arrival and seqno reconciliation
    ├─ Child LV PDOs        Raw FILE_DEVICE_DISK PDOs (Class=Volume, GUID_DEVINTERFACE_VOLUME)
    │                       Mount Manager (IOCTL_MOUNTDEV_*) & Storage/Disk/Volume IOCTLs
    └─ I/O Data Path        IRP pass-through & partial-MDL sub-IRP fan-out (never ZwCreateFile)
        │  IoCallDriver (retained lower PDEVICE_OBJECT / PFILE_OBJECT)
        ▼
Backing Storage
  Physical disks, disk partitions, or unlocked WinLUKS cleartext block devices
```

### Core Components

- **`winlvm.sys` (`src/driver/`)** — Root-enumerated WDM software-bus driver
  (`ROOT\winlvm`, installed via `winlvm.inf`). Discovers candidate physical
  volumes through PnP device-interface notifications (`GUID_DEVINTERFACE_DISK`
  and `GUID_DEVINTERFACE_VOLUME`) and explicit probes
  (`IOCTL_WINLVM_PROBE_DEVICE`), reads PV labels and metadata via
  `IoGetDeviceObjectPointer` + `IoCallDriver`, and enumerates one raw volume
  PDO (`WinLvm\Volume`, bound with a null service via `winlvm-vol.inf`) per
  eligible, PV-complete logical volume. Mount Manager assigns drive letters so
  native or third-party filesystems (such as NTFS or WinBtrfs) mount directly.
- **`lvmcore` (`src/lvmcore/`)** — Freestanding C11 LVM2 label, MDA ring-buffer,
  text-metadata parser, and serializer compiled into both `winlvm.sys` and host
  test binaries. Performs no dynamic heap allocations, uses non-recursive
  parsing, and keeps stack frames at or below 512 bytes.
- **`lvmfmt` (`src/lvmfmt/`)** — Platform-agnostic C++20 LVM2 on-disk format
  library implementing LVM2's weak CRC-32 (`0xedb88320`, init `0xf597a6cf`),
  `LABELONE` / `pv_header` / `pv_header_extension` v2 handling, multi-MDA
  circular ring-buffer reads and two-phase metadata commits, text metadata
  parsing and serialization, extent allocation, overflow-checked
  extent-to-sector mapping, and sector-aligned block I/O.
- **`lvm` / `lvm.exe` (`src/tools/lvm/`)** — LVM2 control-plane CLI supporting
  offline disk-image operations (Linux and Windows) and live Windows device
  discovery, driver probing, activation, resizing, and metadata management:
  - PV commands: `pvcreate`, `pvremove`, `pvs`, `pvdisplay`, `pvscan`,
    `pvresize`, `pvmove`, `pvck`
  - VG commands: `vgcreate`, `vgremove`, `vgs`, `vgdisplay`, `vgscan`,
    `vgchange`, `vgextend`, `vgreduce`, `vgrename`, `vgcfgbackup`,
    `vgcfgrestore`, `vgck`, `vgexport`, `vgimport`
  - LV commands: `lvcreate`, `lvremove`, `lvs`, `lvdisplay`, `lvscan`,
    `lvchange`, `lvextend`, `lvreduce`, `lvresize`, `lvrename`
  - Utility commands: `lvmdiskscan`, `formats`, `segtypes`, `systemid`,
    `version`, `help`
  - Commands outside implemented scope (`pvchange`, `lvconvert`, `dumpconfig`,
    `tags`, `lvm lvmdump`) return non-zero errors explicitly rather than stub
    output.
- **`lvmdump` / `lvmdump.exe` (`src/tools/lvmdump/`)** — Standalone inspection
  utility that prints PV label metadata, MDA offsets, raw VG metadata
  (`--metadata`), and computed Linux-compatible device-mapper tables.
- **`winlvm_svc` / `winlvm_svc.exe` (`src/service/`)** — Windows Service
  Control Manager host (`winlvm-svc`) exposing an ACL-restricted named pipe
  (`\\.\pipe\winlvm-svc`, LocalSystem and Built-in Administrators only) that
  validates framed protocol messages and dispatches `Inventory` and `Refresh`
  requests to `\\.\WinLvmControl`.

## Supported Scope

- **Logical volume layouts**: `linear` (single- and multi-segment, including
  discontiguous non-monotonic segments) and `striped` (multi-stripe round-robin
  with configurable stripe counts and power-of-two stripe sizes) across
  single-PV and multi-PV volume groups.
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
  path to `winlvm.sys` via `IOCTL_WINLVM_PROBE_DEVICE`, allowing `winlvm.sys`
  to assemble the enclosed VG and expose its LVs.

## Fail-Closed Boundaries

WinLVM fails closed rather than guessing or silently corrupting on-disk state:

- **Unsupported segment types**: `thin`, `thin-pool`, `raid*`, `mirror`,
  `cache`, `cache-pool`, `snapshot`, `writecache`, `integrity`, `vdo`, and
  internal sub-volumes are parsed, identified, and refused safely on a per-LV
  basis. Plain `linear` and `striped` LVs in the same VG remain accessible for
  read/write, while metadata serialization refuses to rewrite VGs containing
  unsupported or internal LVs.
- **Write protection**: Any LV whose VG declares a non-empty `system_id`
  (foreign host ownership), whose VG or LV status omits `WRITE` (`READ`-only),
  or whose backing PV is read-only is mounted read-only; write requests fail
  with `STATUS_MEDIA_WRITE_PROTECTED`.
- **Degraded VGs & malformed metadata**: LVs missing any backing PV are not
  activated; surprise removal of a backing PV marks affected LVs unavailable.
  CRC mismatches, invalid offsets, integer overflows, out-of-bounds area
  descriptors, non-contiguous segments, and encrypted non-PV headers (such as
  raw locked LUKS2 containers) are rejected.
- **Out of scope**: Clustered LVM / `lvmlockd`, concurrent read-write mounting
  of the same VG by Linux and Windows simultaneously, boot-start driver loading
  (`SERVICE_BOOT_START`), crash-dump/hibernate filtering, and UEFI pre-boot
  drivers.

## Repository Layout

```text
src/
  winlvm_ioctl.h          Shared user/kernel control-plane IOCTL ABI
  winlvm_types.h          Shared fixed-width ABI primitives
  driver/                 WDM kernel driver (winlvm.sys, C11)
    winlvm.c              DriverEntry, ACL-secured control device, ring log, WMI, top-level dispatch
    winlvm_pnp.c          Bus FDO PnP lifecycle, BusRelations, PV cleanup, live child reconciliation
    winlvm_probe.c        PnP disk/volume notifications, link filter, PnP & IOCTL PV probing
    winlvm_control.c      \Device\WinLvmControl IOCTL dispatch plane
    winlvm_child.c        Child LV raw-PDO creation, PnP dispatch, read-only policy, teardown
    winlvm_gate.c         Cancel-safe I/O admission and quiesce gate (IO_CSQ)
    winlvm_power.c        WDM power IRP dispatch (control, bus FDO, LV PDOs)
    winlvm_dispatch_io.c  Read, Write, Flush, and Shutdown dispatch and aligned pass-through
    winlvm_subirp.c       Multi-piece sub-IRP allocation, partial MDLs, bounce buffers, completion
    winlvm_sector.c       Backing-disk geometry, writability, GPT/MBR partition layout, metadata reads, piece mapping
    winlvm_mount.c        Mount Manager (IOCTL_MOUNTDEV_*) and volume FSCTL handlers
    winlvm_storage.c      Storage, Disk, and Volume IOCTL handlers
    winlvm_vg.c           Freestanding multi-VG registry and PV arrival/seqno state machine
    winlvm_map.c          Freestanding streaming LV-byte to PV-extent mapper
    winlvm.h, winlvm_vg.h Internal driver headers
    winlvm.inf            Bus driver INF (ROOT\winlvm, Class=System)
    winlvm-vol.inf        Raw LV PDO INF (WinLvm\Volume, Class=Volume, null service)
    winlvm.vcxproj        Visual Studio / WDK MSBuild project
  lvmcore/                Freestanding C11 parser, arena allocator, CRC-32, serializer
  lvmfmt/                 Portable C++20 LVM2 format, mapping, allocator, and commit library
  tools/lvm/              lvm / lvm.exe CLI control plane
  tools/lvmdump/          lvmdump / lvmdump.exe diagnostic table & metadata dumper
  service/                winlvm_svc / winlvm_svc.exe SCM named-pipe broker

corpus/                   Real Linux-created LVM2 & LUKS2 .img.zst fixtures, oracle outputs,
                          corpus-manifest.json, and generate.sh
tests/                    Unit, parser, hardening, serialization, VG state-machine, CLI,
                          storage/mount IOCTL, control-plane, corpus oracle tests, and VM harnesses
docs/                     Architecture decisions and engineering roadmap
third_party/              Upstream reference submodules (lvm2, winbtrfs) — not built or linked
```

## Quick Start

### Build and Test User-Mode Components (Linux or Windows)

Requires CMake 3.24+, Ninja (or Visual Studio), a C11/C++20 compiler, and
`zstd` on `PATH` to unpack the committed ground-truth corpus images.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`ctest` runs all 6 suites (`test_lvmcore_parser`, `test_lvmcore_hardening`,
`test_lvmcore_serialize`, `test_winlvm_vg`, `test_driver_corpus_map` for
differential kernel-mapper SHA-256 verification, and `winlvm_tests`).

### Inspect an LVM2 Disk Image

```bash
./build/lvmdump build/corpus-images/striped-2pv-a.img
./build/lvm --image build/corpus-images/linear-1pv.img lvs
```

### Build and Install the Windows Kernel Driver

See [`BUILD.md`](BUILD.md) for complete Linux, Windows (MSVC/MinGW), and WDK
driver build instructions, test-signing setup, and `pnputil` / `devcon`
installation procedures.

## Documentation

- [`BUILD.md`](BUILD.md) — Building user-mode tools, running CTest, building
  `winlvm.sys` with the WDK, and installing the bus/volume INFs.
- [`corpus/README.md`](corpus/README.md) — Ground-truth Linux LVM2/LUKS2 disk
  image corpus, oracle files, and regeneration workflow.
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — Implemented capabilities, remaining
  validation/boot milestones, and out-of-scope boundaries.

## License

Licensed under the GNU Lesser General Public License v3.0 or later. See
[`LICENSE`](LICENSE) and [`NOTICE`](NOTICE).
