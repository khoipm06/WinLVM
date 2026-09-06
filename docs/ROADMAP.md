# winlvm Roadmap

**Date:** 2026-07-26
**Supersedes:** `legacy notes`, `IMPLEMENTATION_STATUS.md`, `SESSION_SUMMARY.md`
---

## 0. Context

The original project answered "can I _read_ my Linux LVM disks from Windows?"
It got partway: a real user-mode metadata scanner for single-PV linear LVs, and
a kernel driver that cannot work (`FEASIBILITY.md` §2).

The goal is now larger:

1. **Full-featured LVM2 on Windows** — read _and_ write, not a read-only viewer.
2. **Boot Windows from an LVM2 logical volume.**
3. **With the PV inside a LUKS2 container.**

The existing tree is **reference material, not a foundation**. The kernel driver
is architecturally wrong for anything bootable, and roughly a third of the
project's documented status is unverifiable narrative. We restart the
implementation, keeping only ideas and hard-won safety discipline.

**The north star, stated as one sentence:** a Windows machine that boots from
NTFS on an LVM2 logical volume inside a LUKS2 container, where a Linux machine
can read and write the same volumes without either OS corrupting the other.

---

## 1. Non-negotiable principles

These exist because the previous iteration violated them and produced a
codebase whose status nobody can trust.

1. **No claim without an artifact.** Every "it works" statement must point at a
   committed log, hash, or CI run. If it isn't committed, it didn't happen.
   This is the rule the previous iteration broke, and on a boot stack the cost
   of self-deception is an unbootable machine.
2. **Ground truth is Linux.** Correctness is defined as byte-for-byte agreement
   with `dmsetup table` / `lvs -o+seg_pe_ranges` / `cryptsetup`. Never as
   agreement with our own fixtures.
3. **Fail closed.** Unknown segment type, bad CRC, ambiguous `seqno`, foreign
   `system_id`, overflowed offset → refuse. The old code got this right; keep it.
4. **The kernel parses the minimum.** Full LVM2 text metadata parsing lives in
   user mode. The kernel needs only enough to self-assemble at boot.
5. **Never fabricate success.** No simulated handles, no `busy = false`
   placeholders. A missing driver is an error, not a mock.
6. **Read-only until proven, then write.** But _plan_ for write from day one —
   retrofitting a write path into a read-only design is how the current driver
   became unfixable.
7. **PnP + INF from the first commit of driver code.** Not a later retrofit.

---

## 2. Decisions required before Phase 1

These are blocking. Answer them and record the answers here.

| #   | Decision                                                                                                                                                                | Why it blocks                                                                                                                          | Default if undecided                                                                                                                                                                                |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| D1  | **Project license.** winmd, Quibble, and VeraCrypt-DCS are all LGPL. Deriving from winmd source means shipping LGPLv3-compatible.                                       | Determines whether Phase 2 can transcribe winmd or must clean-room it — a multi-month difference.                                      | **LGPLv3**, derive freely from winmd. Fastest path, honest lineage.                                                                                                                                 |
| D2  | **Kernel language: C or C++?** winmd is C; WinBtrfs is C++.                                                                                                             | Affects everything downstream.                                                                                                         | **C++** (as the current tree uses) with C-compatible driver entry points.                                                                                                                           |
| D3  | **Boot LV constrained to a single PV?**                                                                                                                                 | Multi-PV boot volumes are a support minefield (`FEASIBILITY.md` §4 Phase 3).                                                           | **Yes, single PV for the boot LV.** Multi-PV allowed for data LVs.                                                                                                                                  |
| D4  | **LUKS2 boot container KDF, and what crosses `ExitBootServices`.** Argon2id at 1 GiB / 4 threads is hostile to a UEFI DXE driver and infeasible in a boot-start driver. | Determines whether pre-boot unlock is viable, _and_ whether you can use VeraCrypt's re-derive-in-kernel model or must transport a key. | Require `--pbkdf pbkdf2` (or heavily reduced Argon2id) for the **boot** container only; transport the unwrapped volume key and burn it immediately. Document as a container requirement, not a bug. |

---

## 3. Target repository layout

```
winlvm/
  docs/
    FEASIBILITY.md          this assessment
    ROADMAP.md              this file
    format-lvm2.md          our own notes on the on-disk format
    format-luks2.md         our own notes on LUKS2
    boot-chain.md           the boot design, once Phase 6 starts
  corpus/
    generate.sh             Linux-side: builds the ground-truth images
    expected/               oracle outputs, committed
  src/
    lvmfmt/                 LVM2 on-disk format: parse + serialize. Platform-agnostic.
    luksfmt/                LUKS2 header, KDF, AF, digest. Platform-agnostic.
    crypto/                 XTS, AES, Argon2, PBKDF2. Shared user/kernel/UEFI.
    cli/                    lvm.exe & friends
    svc/                    activation broker (real IPC)
    driver/                 winlvm.sys — the volume manager
    dumpfilter/             dump_winlvm.sys
    uefi/                   WinLvmDxe.efi
  tests/
    unit/                   real framework (Catch2/GoogleTest), not bare assert()
    corpus/                 tests that run against corpus/ images
```

`src/lvmfmt`, `src/luksfmt`, and `src/crypto` must build for **three targets**:
Win32 user mode, kernel mode, and UEFI. Design them with no CRT dependencies
beyond what all three provide. Getting this boundary right early is what makes
Phases 5–7 tractable instead of a rewrite.

---

## 4. Phases

Each phase has a hard exit criterion. Do not start the next phase until the
current one's artifact is committed. Gate references are to `FEASIBILITY.md` §8.

---

### Phase 0 — Ground truth _(Stage 0)_ — ✅ **DONE 2026-07-27** / ⚠️ **corpus must be regenerated**

Generated on Fedora 44, LVM 2.03.38, cryptsetup 2.8.4. All 9 cases passed, 12
images, zero failures. Corpus in [`../corpus/`](../corpus/), generator in
`corpus/generate.sh`, provenance in `corpus/expected/PROVENANCE.txt`.

**2026-07-28 — `generate.sh` significantly enhanced.** The script now writes
a full **ext4 filesystem** with known files onto every LV (replacing the old raw
sector stamp approach). The corpus is not functionally usable for driver read
testing until it is regenerated with this new script on the Linux VM.

What changed in `generate.sh`:

- **Removed**: `stamp()` and `check_stamp()` (raw sector-level magic bytes).
- **Added**: `write_example_data()` — creates ext4 on each LV, writes
  `hello.txt`, `pattern-1k.bin`, `pattern-4k.bin`, `random-1m.bin` (1 MiB
  deterministic pseudo-random), and `subdir/nested/deep.txt`.
- **Added**: `capture_fs_oracles()` — mounts read-only, records a file listing,
  SHA-256 checksums, and byte-exact copies of small files into
  `expected/<case>/fs-<lv>/`.
- **Added**: `corpus-manifest.json` at the corpus root — a single
  machine-readable file describing every image, VG, and filesystem contents.
  Agents and test harnesses read this as their entry point.
- **Modified**: `verify_luks_roundtrip()` — now verifies the ext4 filesystem
  and `hello.txt` instead of raw stamps.
- **LV size adjustments**: `lvC` 4M→8M, `lvodd` 10M→16M, `lvplain` 8M→16M.
- **`lv-hashes.txt`** now reflects ext4 filesystem content (the data that
  actually ships) rather than the old stamp pattern.

**Action required:** re-run `sudo ./generate.sh` on the Linux VM and copy the
output to `corpus/`. See [`../corpus/README.md`](../corpus/README.md).

Confirmed present in the captured oracles (first run):

- `linear-1pv-multiseg`: `lvA` has **3 segments at non-monotonic PV offsets**
  (2048 → 34816 → 75776) — the discontiguous case a naive `start + length`
  mapper gets wrong.
- `striped-2pv`: `striped 2 128 7:6 2048 7:7 2048` — stripe count and 64 KiB
  chunk size.
- `linear-2pv`: a genuine cross-device seam at sector 253952.
- `odd-geometry`: `#PMda 2` — the second metadata area really exists, plus 2 MiB
  extents.
- `mixed-vg`: thin-pool, thin LV, raid1 (with `rimage` sub-LVs) **and** snapshot
  — all four rejection cases in one VG beside a plain linear LV.
- Both LUKS cases verified by closing and **re-opening the container from
  scratch**, not merely by writing to it once.

**Exit:** the corpus and oracle files are committed, and a script regenerates
them reproducibly. **Kill criterion if you won't build this** — see Stage 0.

---

### Phase 1 — `lvmfmt`: format library _(Stage 1)_ — ✅ **DONE 2026-07-27** / ⚠️ **re-verify after corpus regeneration**

Implemented in `src/lvmfmt/`: `bytes.h` (checked arithmetic), `crc32`
(LVM's non-standard variant), `config` (text metadata grammar), `label`
(LABELONE, PV header, mda_header, ring buffer), `metadata` (object model),
`mapping` (extent → device-mapper table). Plus `lvmdump` and a test suite with
two tiers (metadata text, and real images).

**Status: 22/22 tests pass against the regenerated corpus.** 8 logical volumes
across 7 corpus cases produce device-mapper tables that match Linux's
`dmsetup table` byte-for-byte, including the 3-segment discontiguous LV, the
2-PV spanning LV, the 64 KiB striped LV, 2 MiB extents, and 4Kn. Four LVs are
correctly refused (thin, raid1, snapshot, snapshot origin).

CI configured in `.github/workflows/ci.yml`: MSVC build + corpus test on
`windows-latest`.

**Bug fixed this session:** the image-tier tests failed not on a bad extraction
but on a single byte — LVM's on-disk `commit_size` includes the text's trailing
NUL terminator, which `vgcfgbackup` files (used by the metadata-text tier) omit.
`src/lvmfmt/config.cpp`'s `skip_trivia` now treats `\0` as whitespace so the
parser doesn't see the terminator as an expected-identifier failure. The CRC
check in `label.cpp` had been silently correct all along — the ring-buffer
extraction was fine; only the parser choked.

**Note (2026-07-28):** The corpus has changed (ext4 data now on LVs; `lv-hashes.txt`
will differ). The dmsetup-table and metadata-tier tests are **unaffected** — only
LV content changed. Re-run the test suite after regenerating the corpus to confirm
the image-tier tests (Tier 2) still pass.

_Original scope, retained:_

Fresh implementation of the LVM2 on-disk format, informed by (not copied from)
`src/lvmcore/metadata_parser.cpp` and `scanner.cpp`.

- Label (`LABELONE`) at sector 1, `pv_header`, data-area and metadata-area
  descriptors.
- `mda_header` at offset 4096, four `raw_locn` slots, the **weak CRC-32**
  (poly `0xedb88320`, init `0xf597a6cf`, **no final XOR**).
- Wrapped ring-buffer metadata text read, `seqno` selection, checksum validation.
- Full text-metadata parser producing a complete VG model — **including segment
  types we do not support**, parsed and labelled rather than rejected at parse
  time. Rejection is a policy decision made _above_ the parser.
- Extent→byte translation with checked arithmetic **inside the library**, not
  delegated to a caller.

Supported for mapping in this phase: `linear`, `striped` (any stripe count).
Parsed-and-rejected: `mirror`, `raid*`, `thin*`, `cache*`, `snapshot`,
`writecache`, `integrity`, `vdo`.

**Exit:** for every corpus image, our computed extent map matches the committed
`dmsetup table` oracle exactly, and LV range hashes match, verified in CI.

---

### Phase 2 — `winlvm.sys`: the volume manager, read-only _(Stage 2)_ — 🟡 **in progress**

**The single most important phase.** Transcribe winmd's architecture
(`FEASIBILITY.md` §5.1), substituting LVM2 for md RAID.

- WDM driver, control device + root-enumerated bus PDO via
  `IoReportDetectedDevice`, one PDO per LV, volume FDO per PDO.
- `winlvm.inf`: `Class = Volume`, two model entries (`ROOT\winlvm` and
  `WinLvmVolume`).
- Discovery by `IoRegisterPlugPlayNotification` on `GUID_DEVINTERFACE_DISK` and
  `GUID_DEVINTERFACE_VOLUME` with
  `PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES`; work-item deferred;
  `IoGetDeviceObjectPointer` + `IoAllocateIrp` + `IoCallDriver` to probe the PV
  label. **Never `ZwCreateFile`.**
- Mount manager: register `GUID_DEVINTERFACE_VOLUME`, answer
  `IOCTL_MOUNTDEV_QUERY_DEVICE_NAME` and `IOCTL_MOUNTDEV_QUERY_UNIQUE_ID` (LV
  UUID). Strip stale drive letters from absorbed PVs.
- Read path: pass-through via `IoCopyCurrentIrpStackLocationToNext` for
  single-member I/O; sub-IRP fan-out with partial MDLs for spanning I/O.
- Writes rejected with `STATUS_MEDIA_WRITE_PROTECTED` for now.

**Exit:** on a clean VM, installing via right-click→Install on `winlvm.inf`
causes a Linux-created LV to appear in Disk Management **with a drive letter**,
and NTFS mounts from it read-only. Screenshot and `sha256` comparison against
the Linux oracle both committed. No `sc create` anywhere.

---

### Phase 3 — Write path _(Stage 3)_ — ✅ **DONE**

Data writes only. **No LVM2 metadata mutation yet.**

- `IRP_MJ_WRITE` mirroring the read path.
- `IRP_MJ_FLUSH_BUFFERS` correctly propagated to all members — this is what
  protects NTFS's own consistency guarantees.
- `IRP_MJ_SHUTDOWN` handling.
- Honor LVM2 `system_id`: refuse read-write activation of a VG owned by another
  host. Refuse if the VG looks like it is active on Linux.

**Exit:** 72 hours of `diskspd` read/write stress with zero corruption, verified
against Linux afterwards; clean surprise-removal behavior; and a **clean Driver
Verifier run** (standard + DDI compliance). All logs committed.

---

### Phase 4 — User-mode tooling and the activation broker

Now that the driver is real, rebuild the CLI against it.

- `lvm.exe` + the multi-call `pvs`/`vgs`/`lvs`/`lvdisplay`/... family.
- `winlvm-svc`: a **real** activation broker with actual IPC — unlike the current
  `lvm2d.cpp`, which is an empty `Sleep(500)` loop the CLI never contacts.
- Reuse `src/lvmcore/device_enum.cpp` largely as-is; it is the one component
  portable nearly unchanged.
- JSON output for tests, deterministic and golden-tested.

**Exit:** CLI output matches Linux `pvs`/`vgs`/`lvs` field-for-field for the
corpus, in CI.

---

### Phase 5 — LUKS2, user mode first _(Stage 4)_ — 🟡 **in progress**

Build `luksfmt` + `crypto` as portable libraries, validated in user mode before
they ever enter the kernel.

- LUKS2 binary header (4096 bytes, magic `LUKS\xba\xbe`, version 2, big-endian),
  JSON area, secondary header at `SKUL\xba\xbe`.
- JSON objects: `keyslots`, `segments`, `digests`, `tokens`, `config`.
- KDF: Argon2id/Argon2i/PBKDF2. AF splitter (`luks1` type, stripe count, hash).
- Master-key digest verification before any data is touched.
- Sector crypto: **`aes-xts-plain64`** — XTS with a 64-bit little-endian sector
  number as tweak, sector numbering **relative to the segment's data offset**,
  not the device start. cryptsetup's default is a 512-bit key (AES-256 after the
  XTS halving) and 512-byte sectors.
- LUKS1 support is optional; decide based on whether you have LUKS1 volumes.

> **Source discipline for this phase.** The field offsets circulating in
> community documentation (libyal's `libluksde` notes, various wikis) are
> reverse-engineered and mostly right, but the **normative** reference is the
> cryptsetup project's own _LUKS2 On-Disk Format Specification_ PDF, and the
> `cryptsetup` source itself. Validate every offset against a real container
> from Phase 0's corpus before trusting any table, including the ones summarised
> in `FEASIBILITY.md`.

**Exit:** we decrypt a container created by stock `cryptsetup` with defaults, and
a Linux `cryptsetup` opens what we produce, with matching plaintext hashes both
directions. Test vectors committed. **Near-miss crypto is worse than none** —
this gate is pass/fail with no partial credit.

---

### Phase 6 — LUKS2 in the kernel

Fold `luksfmt`/`crypto` into `winlvm.sys` as an integrated layer, **not** a
separate stacked filter driver. One driver doing both LUKS and LVM means fewer
load-order dependencies, no fight with `partmgr` over an unrecognizable
partition, and a far simpler boot-start story later.

- AES-NI. Measure throughput against the raw device; a large regression here
  makes the boot goal unpleasant to live with.
- Passphrase entry via the user-mode broker for non-boot volumes; keys held in
  non-paged pool, never paged, zeroed on teardown.

**Exit:** a LUKS2-wrapped LV mounts read-write on Windows and Linux verifies the
result. Throughput measured and committed.

---

### Phase 7 — Boot, in a VM _(Stage 5)_

The ambitious part. See `FEASIBILITY.md` §4 for the full analysis.

**7a — `WinLvmDxe.efi`.** A UEFI DXE driver that unlocks LUKS2, maps the LV, and
publishes `EFI_BLOCK_IO_PROTOCOL` presenting a synthetic disk containing a GPT
with the NTFS partition. Then chainloads stock
`\EFI\Microsoft\Boot\bootmgfw.efi`. — `winload.efi` reads NTFS natively and consumes `EFI_BLOCK_IO_PROTOCOL`
without caring what produces it. VeraCrypt-DCS is the model.

Keeping the stock Microsoft boot chain means `bootmgfw`, `winload`, and
`ntoskrnl` all stay Microsoft-signed, so **Secure Boot's chain of trust survives
intact** — only `WinLvmDxe.efi` and `winlvm.sys` need enrollment/signing.

Watch: the BCD `osdevice` element identifies the OS volume by _disk signature
GUID + partition signature GUID_ on GPT. The synthetic GPT must match.

**7b — Key handoff.** Follow VeraCrypt's mechanism: allocate real RAM at a fixed
low physical address via
`AllocatePages(AllocateAddress, EfiMemoryMappedIO, ...)` — typing it MMIO keeps
Windows' memory manager from claiming it while letting the driver
`MmMapIoSpace()` it. The driver signature-scans the known addresses, copies into
kernel heap, and **burns the physical page immediately**.

Decide what crosses: the unwrapped volume key (fast boot, key briefly at a
predictable address) or the passphrase (better hygiene, but requires the KDF in
kernel at boot — infeasible for default Argon2id at 1 GiB). Default to
transporting the volume key with immediate burn; see D4.

This is the most security-sensitive component in the project and deserves its own
design document and its own review.

Confirmed by source review: `LOADER_PARAMETER_EXTENSION` has **no** channel for
passing arbitrary data to a named boot-start driver, so the out-of-band memory
region is the only viable mechanism — not a design preference.

**7c — Boot-start conversion.** `StartType = 0`, Critical Device Database
entries so kernel-mode PnP starts the driver without user-mode PnP, correct load
group ordering. Note winmd is `SERVICE_SYSTEM_START` and gives you nothing here
— this part is your own work.

**7d — `dump_winlvm.sys`.** Crash dump / hibernate filter registered in
`HKLM\SYSTEM\CurrentControlSet\Control\CrashControl\DumpFilters`, the
`dumpfve.sys` role. **Not optional** — without it, hibernation and crash dumps
fail.

Design rule, taken from `dump_veracrypt.sys`: the dump path can run at
`HIGH_LEVEL` IRQL where most memory-manager DDIs are illegal. **Everything must
be preallocated at init** — pre-build an MDL over a private nonpaged scratch
buffer and, at write time, only swap PFNs into the caller's MDL. This cannot be
retrofitted; design 7d under this constraint from its first line.

**Exit:** a VM boots Windows to desktop from NTFS-on-LV-on-LUKS, survives 20
consecutive reboots, and hibernates and resumes successfully. Video or log
evidence committed.

---

### Phase 8 — Bare metal _(Stage 6)_

- EV code-signing certificate; Microsoft **attestation** signing via Partner
  Center for `winlvm.sys` and `dump_winlvm.sys`.
- `WinLvmDxe.efi` signed under the **Microsoft third-party UEFI CA** program —
  the route VeraCrypt has used since 1.26.29 and `shim` uses. This needs **no**
  custom PK/KEK/db enrollment; the stock OEM Secure Boot keys suffice, though
  some firmware requires enabling the "Microsoft 3rd-party UEFI certificate"
  option. Do **not** pursue VeraCrypt's older custom-key path — it is marked
  _"LEGACY — DO NOT USE ON MODERN SYSTEMS"_ and is broken against the 2023 CA
  transition.
- Because `bootmgfw`/`winload`/`ntoskrnl` stay Microsoft-signed in this design,
  the goal here is **Secure Boot fully enabled**, not disabled.
- **Tested rescue media**, and a documented recovery procedure for the case
  where a Windows feature update breaks the boot chain.

**Exit:** driver loads with Secure Boot **on**; a deliberately-broken boot chain
is recovered using only the committed rescue procedure. **Do not move a personal
machine onto this stack before that recovery rehearsal passes.**

---

### Phase 9 — LVM2 metadata mutation

`lvcreate`, `lvremove`, `lvresize`, `vgcreate`, `pvcreate`, `vgextend`,
`vgreduce`.

Deliberately last, because it is the only phase that can destroy a VG that Linux
also uses. The two-slot `raw_locn` precommit/commit protocol must be implemented
exactly: on precommit the new location goes to slot 1 while the committed one
stays in slot 0; on commit the new location moves to slot 0 and slot 1 is
zeroed. Get `seqno` handling wrong and Linux may pick the wrong metadata copy.

**Exit:** every mutation is round-tripped through Linux `vgck`/`pvck` with no
warnings, across power-loss injection testing.

---

## 5. Explicitly out of scope

Not "later" — **out**, unless the project's scale changes fundamentally.

- **Thin provisioning (`dm-thin`), `dm-cache`, `dm-era`.** These are persistent
  B-tree metadata engines, not mapping tables. Each is a project comparable in
  size to everything in Phases 0–8 combined.
- **RAID levels (`dm-raid`), mirrors, `pvmove`.** winmd already does md RAID; if
  you want RAID under LVM, compose rather than reimplement.
- **Snapshots.**
- **Clustered LVM / `lvmlockd`.**
- **Concurrent read-write access from Linux and Windows simultaneously.**
- **Btrfs.** WinBtrfs's job. We expose a block device; what mounts on it is not
  our concern.

All of these must be **parsed, recognized, and refused with a clear message** —
never silently mishandled.

---

## 6. Immediate next actions

1. **Regenerate the corpus on the Linux VM.** `generate.sh` was significantly
   updated on 2026-07-28 — ext4 filesystem data, `fs-<lv>/` oracle directories,
   and `corpus-manifest.json` are now produced. The existing images are missing
   all of this. Run `sudo ./generate.sh` on the Linux VM and commit the output.
2. **Re-run the test suite** after the corpus is regenerated to confirm the
   image-tier tests (Tier 2) still pass against the new image content.
3. **Answer D1–D4 in §2 and record them here.** These block Phase 2. D1
   (license) in particular decides whether Phase 2 can transcribe winmd or must
   clean-room it — a multi-month difference. D2–D4 are recorded as defaults.
4. **Start Phase 2 (`winlvm.sys`)** — the winmd-shaped WDM volume manager,
   read-only first. Stage 2 is the project's make-or-break phase. The
   filesystem data in the corpus is the **read verification anchor**: once
   the driver can read `hello.txt` from each LV and compare it against
   `expected/<case>/fs-<lv>/files/hello.txt`, Stage 2 has meaningful evidence.

**Already done:**

- **Phase 0 complete.** 12 real Linux-created images with oracle files, in
  `corpus/`. Stage 0 passed.
- **Phase 1 complete.** `src/lvmfmt/` — LVM2 on-disk format library, 22/22
  tests pass. `dmsetup table` reproduced byte-for-byte for all corpus cases.
- **2026-07-28:** `generate.sh` enhanced — ext4 filesystem data on every LV,
  `fs-<lv>/` oracle directories, `corpus-manifest.json`.
- Added `.gitignore` negations so verification logs under `corpus/` and
  `tests/evidence/` can be committed despite the blanket `*.log` rule.
- Rewrote `README.md` to state the real status.
