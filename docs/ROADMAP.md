# winlvm Roadmap


## 1. Engineering Principles

1. **Ground truth is Linux.** Format and mapping correctness are validated
   against real Linux-created disk images and captured outputs from `dmsetup`,
   `lvs`, `pvs`, `vgs`, `vgcfgbackup`, `pvck`, `vgck`, and `cryptsetup`.
2. **Fail closed.** Unknown segment types, CRC mismatches, ambiguous `seqno`
   states, foreign `system_id`, read-only metadata flags, missing PVs, and
   overflowed offsets are refused safely rather than guessed.
3. **Minimal, bounded kernel parser.** The kernel driver uses the freestanding
   C11 `lvmcore` library (zero dynamic allocations, non-recursive, <= 512-byte
   stack frames). Metadata mutation and CLI orchestration live in user mode
   (`lvmfmt`, `lvm.exe`, `winlvm_svc.exe`).
4. **Explicit error reporting.** Unavailable driver handles and unsupported
   subcommands return explicit errors.
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
| D4 | **Boot LV topology** | **Single PV for future boot LVs.** Multi-PV linear and striped LVs are supported for data volumes; restricting boot LVs to a single PV avoids multi-disk early-boot assembly hazards. |
| D5 | **LUKS2 boot container KDF** | **PBKDF2 or reduced-memory Argon2id for future boot containers.** Stock Argon2id (1 GiB / 4 threads) is impractical in pre-boot UEFI DXE and early boot-start drivers; pre-boot handoff design must account for this constraint. |

---

## 3. Repository Structure

### Implemented Tree

```text
winlvm/
  corpus/               Linux-generated LVM2/LUKS2 .img.zst fixtures, oracles, manifest, generate.sh
  docs/
    ROADMAP.md          This document
  src/
    winlvm_ioctl.h      Shared user/kernel control-plane ABI
    winlvm_types.h      Shared fixed-width ABI primitives
    lvmcore/            Freestanding C11 LVM2 parser, arena, CRC-32, serializer
    lvmfmt/             Portable C++20 LVM2 format, mapping, allocator, two-phase MDA commit engine
    driver/             WDM kernel driver (winlvm.sys, 14 C11 modules, winlvm.inf, winlvm-vol.inf)
    tools/lvm/          lvm / lvm.exe CLI control plane
    tools/lvmdump/      lvmdump / lvmdump.exe standalone metadata & DM-table dumper
    service/            winlvm_svc / winlvm_svc.exe SCM named-pipe broker
  tests/                Unit, parser, hardening, serialization, VG state-machine, CLI,
                        IOCTL, control-plane, corpus oracle tests, and Windows VM harnesses
```

### Planned Future Boot Components

```text
  src/
    dumpfilter/         dump_winlvm.sys — crash-dump and hibernation filter
    uefi/               WinLvmDxe.efi — UEFI DXE block-device producer
```

---

## 4. Implemented Foundation

### Ground-Truth Corpus (`corpus/`)

- 9 test cases (12 `.img.zst` images) generated on Linux (`lvm2`, `cryptsetup`,
  `e2fsprogs`), with committed `dmsetup table`, `lvs`, `pvs`, `vgs`,
  `vgcfgbackup`, `lv-hashes.txt`, per-LV ext4 filesystem oracles, `luksDump.txt`,
  and `corpus-manifest.json`.
- Covers single-PV linear, discontiguous multi-segment linear, multi-PV spanning
  linear, 2-PV 64 KiB striped, 4Kn sector geometry, odd geometry (2 MiB extents,
  shifted `pe_start`, dual MDAs, wrapped circular MDA ring buffers), mixed VG
  with unsupported segment types (`thin`, `raid1`, `snapshot`), and two LUKS2
  opaque-container cases (`luks2-default`, `luks2-pbkdf2`).

### Format Libraries (`src/lvmcore/`, `src/lvmfmt/`)

- Full LVM2 label (`LABELONE`), `pv_header`, `pv_header_extension` v2
  (`PV_EXT_USED`), multi-MDA `mda_header`, and circular ring-buffer support.
- Weak CRC-32 (`0xedb88320`, init `0xf597a6cf`, no final XOR) and checked
  64-bit arithmetic throughout.
- Exact `dmsetup table` parity across all supported corpus LVs (`linear` and
  `striped`), with unsupported segment types parsed and refused per LV.
- Two-phase circular MDA metadata commits (`raw_locns[1]` precommit ->
  `raw_locns[0]` commit with rollback) across all active MDAs of all member PVs,
  honoring `RAW_LOCN_IGNORED`.

### WDM Kernel Driver (`src/driver/winlvm.sys`)

- Modular 14-file C11 WDM software-bus driver (`winlvm.c`, `winlvm_pnp.c`,
  `winlvm_probe.c`, `winlvm_control.c`, `winlvm_child.c`, `winlvm_gate.c`,
  `winlvm_power.c`, `winlvm_dispatch_io.c`, `winlvm_subirp.c`, `winlvm_sector.c`,
  `winlvm_mount.c`, `winlvm_storage.c`, `winlvm_vg.c`, `winlvm_map.c`) linked
  with `src/lvmcore/{crc32,arena,parser}.c`.
- Split INF packaging: `winlvm.inf` (`ROOT\winlvm`, `Class=System`) and
  `winlvm-vol.inf` (`WinLvm\Volume` / `WinLvmVolume`, `Class=Volume`, null
  service for raw PDOs).
- Automatic PV discovery on `GUID_DEVINTERFACE_DISK` and
  `GUID_DEVINTERFACE_VOLUME`, automatic GPT/MBR partition scanning on whole
  disks via `IOCTL_DISK_GET_DRIVE_LAYOUT_EX` (covering Linux LVM partitions
  without Windows volume interfaces), and explicit non-PnP probing via
  `IOCTL_WINLVM_PROBE_DEVICE` (enabling `LVM-on-LUKS` stacking with `WinLUKS`).
- Read, write, flush, and shutdown I/O paths with aligned single-member
  pass-through, partial-MDL multi-piece sub-IRP fan-out, 512e/4Kn sector
  alignment and bounce buffers, and cancel-safe quiesce gating (`IO_CSQ`).
- Enforces read-only protection (`STATUS_MEDIA_WRITE_PROTECTED`) on foreign
  `system_id`, `READ`-only VG/LV metadata, and read-only backing PVs.
- ACL-hardened `\Device\WinLvmControl` (`IoCreateDeviceSecure`,
  `SDDL_DEVOBJ_SYS_ALL_ADM_ALL`) supporting inventory, refresh, VG quiesce /
  resume, dynamic LV activate / deactivate / resize update, ring log drain, and
  explicit device probing.

### User-Mode Tooling (`src/tools/lvm/`, `src/tools/lvmdump/`, `src/service/`)

- `lvm.exe` CLI covering PV, VG, and LV inspection, creation, resizing,
  migration (`pvmove`), backup/restore (`vgcfgbackup`/`vgcfgrestore`), integrity
  checking (`pvck`/`vgck`), export/import, and online driver activation/probing.
- `lvmdump.exe` diagnostic inspection utility.
- `winlvm_svc.exe` SCM service over an ACL-restricted named pipe
  (`\\.\pipe\winlvm-svc`) forwarding `Inventory` and `Refresh` requests to the
  driver control plane.

---

## 5. Remaining Milestones

### Milestone A — Extended Driver Reliability & Verification

Single- and multi-PV `linear` and `striped` I/O under Driver Verifier
(`0x0000093B`) and `LVM-on-LUKS` stacking with `WinLUKS` are implemented and
verified. Remaining work for this milestone covers extended endurance and
fault-injection coverage:

- Execute multi-day read/write I/O soak workloads (such as extended `diskspd`
  runs) followed by Linux `fsck`/`vgck`/`pvck` verification.
- Expand power-loss fault-injection testing for user-mode two-phase MDA
  metadata commits.
- Validate `LUKS-on-LVM` stacking permutations alongside `LVM-on-LUKS` against
  `corpus/images/luks2-default.img.zst` and `corpus/images/luks2-pbkdf2.img.zst`
  with `WinLUKS`.

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

- **Thin provisioning (`dm-thin`), `dm-cache`, `dm-era`, `dm-vdo`,
  `dm-writecache`, `dm-integrity`** — persistent B-tree/stateful block engines
  outside linear/striped mapping scope.
- **LVM RAID (`dm-raid`) and mirrors** — compose with dedicated RAID volume
  managers instead.
- **LVM snapshots.**
- **Clustered LVM / `lvmlockd`.**
- **Concurrent read-write mounting from Linux and Windows simultaneously.**
- **Filesystem drivers** — WinLVM exposes standard Windows volumes; filesystems
  (NTFS, WinBtrfs, etc.) mount independently on top.
