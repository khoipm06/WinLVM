# WinLVM Architecture

## Layered Design

```text
User Mode
  winlvm_gui.exe            Native Win32 desktop manager & system tray
  lvm.exe / lvmdump.exe     LVM2 inspection, metadata mutation, and driver control
  winlvm_svc.exe            SCM service broker (\\.\pipe\winlvm-svc, SYSTEM/Admins)
        |  DeviceIoControl (\\.\WinLvmControl, SDDL_DEVOBJ_SYS_ALL_ADM_ALL)
        v
Kernel Mode
  winlvm.sys                WDM software-bus driver (ROOT\winlvm, Class=System)
    |- lvmcore (C11)        Non-allocating, bounded-stack LVM2 parser and on-disk
    |                       dm-thin, dm-snapshot, and raid1/mirror block engines
    |- PnP & Power          GUID_DEVINTERFACE_{DISK,VOLUME}, target-device removal
    |                       notifications, WDM S/D power tracking, MountMgr arrival
    |- VG Registry          Multi-VG / multi-PV arrival and seqno reconciliation
    |- Child LV PDOs        Raw FILE_DEVICE_DISK PDOs (Class=Volume, GUID_DEVINTERFACE_VOLUME)
    |                       Mount Manager (IOCTL_MOUNTDEV_*) & Storage/Disk/Volume IOCTLs
    '- I/O Data Path        Aligned pass-through, partial-MDL sub-IRP fan-out, and
                            PASSIVE_LEVEL complex-segment worker dispatch
        |  IoCallDriver (retained lower PDEVICE_OBJECT / PFILE_OBJECT)
        v
Backing Storage
  Physical disks, disk partitions, or unlocked WinLUKS cleartext block devices
```

## Core Components

- **`winlvm.sys` (`src/driver/`)**: Root-enumerated WDM software-bus driver (`ROOT\winlvm`, `winlvm.inf`). Discovers physical volumes through PnP device-interface notifications (`GUID_DEVINTERFACE_DISK`, `GUID_DEVINTERFACE_VOLUME`), GPT/MBR partition scanning (`IOCTL_DISK_GET_DRIVE_LAYOUT_EX`), and explicit probes (`IOCTL_WINLVM_PROBE_DEVICE`). Reads PV labels and metadata via `IoGetDeviceObjectPointer` and `IoCallDriver`, tracks target-device removal notifications and power transitions, and enumerates one raw volume PDO (`WinLvm\Volume`, bound via `winlvm-vol.inf`) per user-visible, PV-complete logical volume.
- **`lvmcore` (`src/lvmcore/`)**: Freestanding C11 LVM2 label, MDA ring-buffer, text-metadata parser/serializer, and portable block engines (`dm-thin`, `dm-snapshot`, `raid1`/`mirror`) compiled into both `winlvm.sys` and host tests. Performs no dynamic heap allocations, uses non-recursive parsing, and keeps stack frames at or below 512 bytes.
- **`lvmfmt` (`src/lvmfmt/`)**: Portable C++20 LVM2 format library implementing LVM2 weak CRC-32 (`0xedb88320`, init `0xf597a6cf`, no final XOR), `LABELONE` / `pv_header` / `pv_header_extension` v2 handling, multi-MDA circular ring-buffer reads and two-phase commits, text metadata parsing/serialization, extent allocation, overflow-checked extent-to-sector mapping, and sector-aligned block I/O.
- **`lvm` / `lvm.exe` (`src/tools/lvm/`)**: LVM2 CLI for offline disk images and live Windows volume groups:
  - PV: `pvcreate`, `pvremove`, `pvs`, `pvdisplay`, `pvscan`, `pvchange`, `pvresize`, `pvmove`, `pvck`
  - VG: `vgcreate`, `vgremove`, `vgs`, `vgdisplay`, `vgscan`, `vgchange`, `vgextend`, `vgreduce`, `vgrename`, `vgcfgbackup`, `vgcfgrestore`, `vgck`, `vgexport`, `vgimport`
  - LV: `lvcreate` (`linear`, `striped`, `thin-pool`, `thin`, `snapshot`), `lvremove`, `lvs`, `lvdisplay`, `lvscan`, `lvchange`, `lvconvert` (`raid1`, `mirror`, `linear`, `--merge`), `lvextend`, `lvreduce`, `lvresize`, `lvrename`
  - Reporting & utility: `fullreport`, `--reportformat json` / `json_std`, `tags`, `lvmconfig` / `dumpconfig` / `config`, `lvmdiskscan`, `formats`, `segtypes`, `systemid`, `version`, `help`
- **`lvmdump` / `lvmdump.exe` (`src/tools/lvmdump/`)**: Standalone dumper for PV label metadata, MDA offsets, raw VG metadata (`--metadata`), and computed Linux-compatible device-mapper tables.
- **`winlvm_svc` / `winlvm_svc.exe` (`src/service/`)**: Windows SCM service (`winlvm-svc`) exposing an ACL-restricted named pipe (`\\.\pipe\winlvm-svc`, LocalSystem and Built-in Administrators only) using framed JSON IPC for live topology discovery, LV activation/deactivation, drive-letter assignment, rescanning, and metadata updates.
- **`winlvm_gui.exe` (`src/gui/`)**: Native Win32 desktop and system-tray manager communicating with `winlvm_svc` or directly with the Win32 backend.
- **`winlvm-setup-x64.exe` & `winlvm-uninstall.exe` (`src/installer/`, `scripts/pack_installer.py`)**: Self-contained x64 installer with an integrity-checked trailer archive and matching uninstaller.

## On-Disk Format & Block Engines

- **PV & Metadata Areas**: Supports `LABELONE`, `pv_header`, `pv_header_extension` v2 (`PV_EXT_USED`), 512e/4Kn sector geometries (propagating strictest alignment and using bounce buffers when unaligned), whole-disk and GPT/MBR-partitioned PVs, multiple metadata areas (`#PMda >= 1`), and wrapped circular MDA ring buffers.
- **Two-Phase Metadata Commits**: New metadata text is written into free ring space without overwriting the active payload, published via `raw_locns[1]` (precommit), and promoted to `raw_locns[0]` with `raw_locns[1]` cleared across every non-ignored MDA (`RAW_LOCN_IGNORED`) on every member PV, rolling back prior MDA headers on I/O error.
- **Internal Sub-Volumes**: `*_tmeta`, `*_tdata`, `*_pmspare`, `*_rimage_*`, `*_rmeta_*`, `*_mimage_*`, `*_mlog`, `*_cow`, `snapshot<N>` wrappers, and `thin-pool` containers are resolved internally and hidden from Mount Manager enumeration.
- **Block Engines**:
  - `dm-thin`: Superblock, 2-level B-tree mappings, space-map allocation, CoW writes with leaf splits, and CRC-32C (`0x82f63b78`) verification.
  - `dm-snapshot`: Persistent exception tables (`SnAp`) for reads/CoW writes on snapshot and origin volumes, plus offline `lvconvert --merge`.
  - `raid1` / `mirror`: Degraded read-only failover across surviving legs; multi-leg writes fail closed if any leg is missing.

## Control Plane ABI (`src/winlvm_ioctl.h`)

`\Device\WinLvmControl` (`\\.\WinLvmControl`) enforces `SDDL_DEVOBJ_SYS_ALL_ADM_ALL` via `IoCreateDeviceSecure` and coordinates online geometry changes with a cancel-safe quiesce gate (`IO_CSQ`):

| IOCTL | Access | Purpose |
| --- | --- | --- |
| `IOCTL_WINLVM_INVENTORY` | Read/Write | Enumerate discovered VGs and LV states (`WINLVM_INVENTORY_LV_ENTRY`) |
| `IOCTL_WINLVM_REFRESH` | Read/Write | Trigger rescan of known candidate volumes |
| `IOCTL_WINLVM_QUIESCE_VG` | Read/Write | Pause I/O on a VG before metadata/geometry update |
| `IOCTL_WINLVM_RESUME_VG` | Read/Write | Resume I/O on a quiesced VG |
| `IOCTL_WINLVM_ACTIVATE_LV` | Read/Write | Expose child PDO for an LV by UUID |
| `IOCTL_WINLVM_DEACTIVATE_LV` | Read/Write | Remove child PDO for an LV by UUID |
| `IOCTL_WINLVM_UPDATE_LV` | Read/Write | Reconcile byte size and sector geometry online |
| `IOCTL_WINLVM_GET_LOG` | Read | Drain kernel diagnostic ring buffer |
| `IOCTL_WINLVM_PROBE_DEVICE` | Read/Write | Probe an explicit NT device path (e.g., `\Device\WinLuksVolumeN`) |

## Decoupled `LVM-on-LUKS` Stacking

`winlvm.sys` performs no cryptographic operations and consumes cleartext block devices. Unlocked devices exposed via PnP `GUID_DEVINTERFACE_DISK` / `GUID_DEVINTERFACE_VOLUME` are discovered automatically; non-PnP NT device objects (such as `\Device\WinLuksVolumeN`) are discovered by `lvm vgscan` / `lvm pvscan` and submitted via `IOCTL_WINLVM_PROBE_DEVICE`.

## Fail-Closed Boundaries

- **Unsupported segment types**: `raid4`, `raid5`, `raid6`, `raid10`, `cache`, `cache-pool`, `writecache`, `integrity`, `vdo`, and unknown types are refused per LV while keeping supported LVs in the same VG accessible; metadata serialization refuses to rewrite VGs with unsupported layouts.
- **Write protection (`STATUS_MEDIA_WRITE_PROTECTED`)**: Enforced for non-empty foreign `system_id`, `READ`-only VG/LV flags, read-only backing PVs, and degraded `raid1`/`mirror` arrays.
- **Degraded & malformed inputs**: Incomplete multi-PV linear/striped/thin/snapshot VGs, surprise-removed PVs, CRC mismatches, invalid offsets, integer overflows, non-contiguous segments, and encrypted headers (e.g. locked LUKS2) fail closed.

## Repository Layout

```text
src/
  winlvm_ioctl.h          Shared user/kernel control-plane IOCTL ABI
  winlvm_types.h          Shared fixed-width ABI primitives
  driver/                 WDM kernel driver (winlvm.sys, winlvm.inf, winlvm-vol.inf)
  lvmcore/                Freestanding C11 parser, arena, CRC-32/CRC-32c, serializer, block engines
  lvmfmt/                 C++20 LVM2 format, mapping, allocator, and two-phase commit library
  tools/lvm/              lvm / lvm.exe CLI control plane
  tools/lvmdump/          lvmdump / lvmdump.exe diagnostic table & metadata dumper
  service/                winlvm_svc / winlvm_svc.exe SCM named-pipe broker & Win32 backend
  gui/                    winlvm_gui.exe Win32 desktop & system-tray application
  installer/              winlvm_setup_stub.exe & winlvm-uninstall.exe Win32 installer

scripts/
  gen_test_fixtures.py    Deterministic out-of-tree LVM2/LUKS2 fixture & oracle generator
  pack_installer.py       Trailer-archive installer packager & verifier
tests/                    Unit, parser, hardening, serialization, VG state-machine, CLI,
                          service, installer archive, IOCTL, corpus oracle, and VM suites
docs/                     Architecture and engineering roadmap
third_party/              Upstream reference submodules (lvm2, winbtrfs) — reference only
```
