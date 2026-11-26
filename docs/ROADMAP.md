# winlvm Roadmap


## 1. Engineering Principles

1. **Ground truth is Linux.** Format, mapping, and block-engine correctness are
   validated against Linux `dmsetup`, `lvs`, `pvs`, `vgs`, `vgcfgbackup`,
   `pvck`, `vgck`, `thin_check`, `thin_dump`, and `cryptsetup`.
2. **Fail closed.** Unsupported segment types, CRC mismatches, ambiguous `seqno`
   states, foreign `system_id`, read-only metadata flags, missing PVs, degraded
   mirror writes, and overflowed offsets are refused safely rather than guessed.
3. **Minimal, bounded kernel core.** The kernel driver uses freestanding C11
   `lvmcore` (zero dynamic allocations, non-recursive, <= 512-byte stack frames)
   for both metadata parsing and complex-segment block engines (`dm-thin`,
   `dm-snapshot`, `raid1`/`mirror`). Metadata mutation and CLI/GUI orchestration
   live in user mode (`lvmfmt`, `lvm.exe`, `winlvm_svc.exe`, `winlvm_gui.exe`).
4. **Explicit error reporting.** Unavailable driver handles and unsupported
   operations return explicit errors.
5. **Native PnP and INF installation.** The driver is a root-enumerated WDM
   software bus (`ROOT\winlvm`) that exposes raw volume PDOs to Windows Mount
   Manager and forwards I/O directly to retained lower device objects via
   `IoCallDriver` (never `ZwCreateFile`).
6. **Decoupled cryptography.** `winlvm.sys` performs no cryptographic
   operations. LUKS2 decryption lives in a separate block provider (`WinLUKS`),
   composing cleanly for both `LVM-on-LUKS` and `LUKS-on-LVM`.

---

## 2. Architecture Decisions

| # | Decision | Resolution |
| --- | --- | --- |
| D1 | **Project license** | **LGPLv3-or-later** ([`LICENSE`](../LICENSE), [`NOTICE`](../NOTICE)). |
| D2 | **Kernel driver language** | **C11** (`src/driver/*.c` + freestanding `src/lvmcore/*.c`). |
| D3 | **LUKS2 separation** | **Decoupled.** `winlvm.sys` contains zero cryptography and consumes cleartext block devices exposed via PnP disk/volume interfaces or `IOCTL_WINLVM_PROBE_DEVICE` (`lvm vgscan`). |
| D4 | **Boot LV topology** | **Single PV for future boot LVs.** Multi-PV linear, striped, thin, snapshot, and raid1/mirror LVs are supported for data volumes; restricting boot LVs to a single PV avoids multi-disk early-boot assembly hazards. |
| D5 | **LUKS2 boot container KDF** | **PBKDF2 or reduced-memory Argon2id for future boot containers.** Stock Argon2id (1 GiB / 4 threads) is impractical in pre-boot UEFI DXE and early boot-start drivers; pre-boot handoff design must account for this constraint. |

---

## 3. Repository Structure

### Implemented Tree

```text
winlvm/
  corpus/               Lightweight LVM2/LUKS2 text/hash oracles and corpus-manifest.json
  scripts/
    gen_test_fixtures.py  Deterministic out-of-tree Python 3 LVM2/LUKS2 image generator
    pack_installer.py     Trailer-archive installer packager and integrity verifier
  docs/
    ROADMAP.md          This document
  src/
    winlvm_ioctl.h      Shared user/kernel control-plane ABI
    winlvm_types.h      Shared fixed-width ABI primitives
    lvmcore/            Freestanding C11 parser, arena, CRC-32/CRC-32c, serializer,
                        and dm-thin / dm-snapshot / raid1-mirror block engines
    lvmfmt/             Portable C++20 LVM2 format, mapping, allocator, two-phase MDA commit engine
    driver/             WDM kernel driver (winlvm.sys, 19 C11 modules, winlvm.inf, winlvm-vol.inf)
    tools/lvm/          lvm / lvm.exe CLI control plane
    tools/lvmdump/      lvmdump / lvmdump.exe standalone metadata & DM-table dumper
    service/            winlvm_svc / winlvm_svc.exe SCM named-pipe broker & Win32 backend
    gui/                winlvm_gui.exe native Win32 desktop & system-tray application
    installer/          winlvm_setup_stub.exe & winlvm-uninstall.exe native Win32 installer
  tests/                Unit, parser, hardening, serialization, VG state-machine, CLI,
                        service, installer archive, IOCTL, corpus oracle suites, and VM harnesses
```

### Planned Future Boot Components

```text
  src/
    dumpfilter/         dump_winlvm.sys — crash-dump and hibernation filter
    uefi/               WinLvmDxe.efi — UEFI DXE block-device producer
```

---

## 4. Implemented Foundation

### Dynamic Out-of-Tree Test Fixtures (`scripts/gen_test_fixtures.py`, `corpus/`)

- Deterministic stdlib-only Python 3 generator invoked automatically by CMake to
  build 16 LVM2/LUKS2 disk images out-of-tree in
  `${CMAKE_BINARY_DIR}/corpus-images/`.
- Covers single-PV linear, discontiguous multi-segment linear, multi-PV spanning
  linear, 2-PV 64 KiB striped, 4Kn sector geometry, odd geometry (dual MDAs,
  wrapped circular MDA ring buffers), `thin-pool` + `thin` + thin snapshot,
  `snapshot` + `snapshot-origin`, 2-PV `raid1` + `mirror` (core and disk log),
  mixed VG with fail-closed `cache` LV, and two LUKS2 opaque-container cases
  (`luks2-default`, `luks2-pbkdf2`).

### Format Libraries & Block Engines (`src/lvmcore/`, `src/lvmfmt/`)

- Full LVM2 label (`LABELONE`), `pv_header`, `pv_header_extension` v2
  (`PV_EXT_USED`), multi-MDA `mda_header`, and circular ring-buffer support.
- Weak CRC-32 (`0xedb88320`, init `0xf597a6cf`, no final XOR), CRC-32c for
  `dm-thin` metadata integrity, and checked 64-bit arithmetic throughout.
- Lossless parsing and serialization of `linear`, `striped`, `thin-pool`,
  `thin`, `snapshot`, `raid1`, `mirror`, and PV/VG/LV `tags`, with out-of-scope
  segment types (`raid4`/`5`/`6`/`10`, `cache`, `vdo`, `writecache`,
  `integrity`) parsed and refused per LV.
- Freestanding C11 on-disk block engines (`lvmcore_thin.c`, `lvmcore_snap.c`,
  `lvmcore_raid.c`) supporting `dm-thin` B-tree reads/CoW writes/leaf splits
  verified against Linux `thin_check`, `dm-snapshot` origin/snapshot CoW reads
  and writes, and `raid1`/`mirror` degraded read failover and multi-leg writes.
- Two-phase circular MDA metadata commits (`raw_locns[1]` precommit ->
  `raw_locns[0]` commit with rollback) across all active MDAs of all member PVs,
  honoring `RAW_LOCN_IGNORED`.

### WDM Kernel Driver (`src/driver/winlvm.sys`)

- Modular 19-file C11 WDM software-bus driver (`winlvm.c`, `winlvm_pnp.c`,
  `winlvm_probe.c`, `winlvm_notify.c`, `winlvm_vg_ops.c`, `winlvm_control.c`,
  `winlvm_child.c`, `winlvm_gate.c`, `winlvm_power.c`, `winlvm_dispatch_io.c`,
  `winlvm_subirp.c`, `winlvm_sector.c`, `winlvm_mount.c`, `winlvm_storage.c`,
  `winlvm_vg.c`, `winlvm_map.c`, `winlvm_thin.c`, `winlvm_snap.c`,
  `winlvm_raid.c`) linked with freestanding `src/lvmcore/` modules, keeping
  every driver file under 500 LOC.
- Split INF packaging: `winlvm.inf` (`ROOT\winlvm`, `Class=System`) and
  `winlvm-vol.inf` (`WinLvm\Volume` / `WinLvmVolume`, `Class=Volume`, null
  service for raw PDOs).
- Automatic PV discovery on `GUID_DEVINTERFACE_DISK` and
  `GUID_DEVINTERFACE_VOLUME`, cold partition symlink retry, automatic GPT/MBR
  partition scanning via `IOCTL_DISK_GET_DRIVE_LAYOUT_EX`, refcounted
  target-device removal notifications, WDM power state tracking, Mount Manager
  link tracking and arrival notification, and explicit probing via
  `IOCTL_WINLVM_PROBE_DEVICE`.
- Read, write, flush, and shutdown I/O paths with aligned single-member
  pass-through, partial-MDL multi-piece sub-IRP fan-out, `PASSIVE_LEVEL`
  work-item dispatch into the freestanding `thin`/`snapshot`/`raid1`/`mirror`
  block engines, 512e/4Kn sector alignment and bounce buffers, and cancel-safe
  quiesce gating (`IO_CSQ`).
- Enforces read-only protection (`STATUS_MEDIA_WRITE_PROTECTED`) on foreign
  `system_id`, `READ`-only VG/LV metadata, read-only backing PVs, and degraded
  `raid1`/`mirror` arrays.
- ACL-hardened `\Device\WinLvmControl` (`IoCreateDeviceSecure`,
  `SDDL_DEVOBJ_SYS_ALL_ADM_ALL`) reporting full LV inventory (including segment
  flags, active state, and MountMgr drive letter) alongside refresh, quiesce /
  resume, dynamic LV activate / deactivate / resize update, ring log drain, and
  explicit device probing.

### User-Mode Tooling, Desktop GUI, & Installer

- `lvm.exe` CLI covering full PV/VG/LV lifecycle management including
  `thin-pool`, `thin`, `snapshot`, `raid1`, `mirror`, `pvchange`, `lvconvert`
  (RAID1/mirror/linear conversion and `--merge`), `lvmconfig`/`dumpconfig`,
  `tags`, `fullreport`, and `--reportformat json` / `json_std`.
- `lvmdump.exe` diagnostic inspection utility.
- `winlvm_svc.exe` SCM service over an ACL-restricted named pipe
  (`\\.\pipe\winlvm-svc`) with framed JSON IPC and Win32 volume/MountMgr
  control backend.
- `winlvm_gui.exe` native Win32 desktop and system-tray manager.
- `winlvm-setup-x64.exe` and `winlvm-uninstall.exe` standalone installer and
  uninstaller with SHA-256 integrity-verified trailer archives.

---

## 5. Remaining Milestones

### Milestone A — Extended Driver Reliability & Verification

- Execute extended read/write I/O soak workloads (including `thin`, `snapshot`,
  and `raid1`/`mirror` volumes under Driver Verifier) followed by Linux
  `fsck`/`vgck`/`pvck`/`thin_check` verification.
- Expand power-loss fault-injection testing for user-mode two-phase MDA commits
  and kernel `dm-thin` metadata updates.
- Validate `LUKS-on-LVM` stacking permutations alongside `LVM-on-LUKS` against
  generated `luks2-default.img` and `luks2-pbkdf2.img` fixtures with `WinLUKS`.

### Milestone B — Boot-Start Driver & Crash-Dump Filter

- Convert `winlvm.sys` for `SERVICE_BOOT_START` (`StartType = 0`), Critical
  Device Database (CDD) registration, and load-order sequencing (`System Bus
  Extender` / storage miniport coordination) without early-boot probe deadlocks.
- Implement `dump_winlvm.sys` registered under
  `HKLM\SYSTEM\CurrentControlSet\Control\CrashControl\DumpFilters` with all
  scratch buffers and MDLs preallocated at init time for `HIGH_LEVEL` IRQL
  crash-dump and hibernation paths.

### Milestone C — Pre-Boot UEFI Support (`WinLvmDxe.efi`)

- Publish `EFI_BLOCK_IO_PROTOCOL` for a synthetic GPT/NTFS disk backed by an
  LVM2 logical volume (composing with a pre-boot LUKS2 block hook when
  encrypted) and chainload stock Microsoft `\EFI\Microsoft\Boot\bootmgfw.efi`
  to preserve the Secure Boot chain of trust.
- Ensure synthetic GPT disk/partition GUIDs align with BCD `osdevice` identity.
- Define and audit any pre-boot key handoff mechanism across `ExitBootServices`
  (fixed low-memory `EfiMemoryMappedIO` handoff with immediate zeroization on
  kernel pickup).

### Milestone D — Production Signing & Servicing Resilience

- Obtain Microsoft Partner Center attestation signing for `winlvm.sys` and
  `dump_winlvm.sys` (EV certificate) so the drivers load under retail Secure
  Boot without test-signing mode.
- Sign `WinLvmDxe.efi` via the Microsoft third-party UEFI CA program (no custom
  PK/KEK/db enrollment).
- Build and rehearse rescue media and documented recovery procedures for
  Windows feature-update BCD/boot-chain resets.

---

## 6. Explicitly Out of Scope

These features are intentionally excluded from WinLVM and must continue to fail
closed cleanly when encountered on disk:

- **Parity RAID (`raid4`, `raid5`, `raid6`, `raid10`) and caching/dedup/integrity
  targets (`dm-cache`, `dm-era`, `dm-vdo`, `dm-writecache`, `dm-integrity`)**.
- **Clustered LVM / `lvmlockd`.**
- **Concurrent read-write mounting from Linux and Windows simultaneously.**
- **Filesystem drivers** — WinLVM exposes standard Windows volumes; filesystems
  (NTFS, WinBtrfs, etc.) mount independently on top.
