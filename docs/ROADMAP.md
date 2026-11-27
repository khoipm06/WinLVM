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

## 3. Future & Boot Architecture

See [`ARCHITECTURE.md`](./ARCHITECTURE.md) for the current driver, user-mode, and repository layout. Planned pre-boot and crash-dump extensions:

```text
  src/
    dumpfilter/         dump_winlvm.sys — crash-dump and hibernation filter
    uefi/               WinLvmDxe.efi — UEFI DXE block-device producer
```

---

## 4. Test Fixtures

`scripts/gen_test_fixtures.py` (invoked automatically by CMake into `${CMAKE_BINARY_DIR}/corpus-images/` and `${CMAKE_BINARY_DIR}/corpus-expected/`) generates deterministic LVM2/LUKS2 fixtures covering single-PV linear, discontiguous multi-segment linear, multi-PV spanning linear, 2-PV 64 KiB striped, 4Kn sector geometry, odd geometry (dual MDAs, wrapped circular MDA ring buffers), `thin-pool` + `thin` + thin snapshot, `snapshot` + `snapshot-origin`, 2-PV `raid1` + `mirror` (core and disk log), mixed VG with fail-closed `cache` LV, and two LUKS2 opaque-container cases (`luks2-default`, `luks2-pbkdf2`).

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
