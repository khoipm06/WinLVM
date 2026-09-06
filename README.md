# winlvm

Native Windows access to Linux LVM2 volumes — with the longer-term goal of
booting Windows from NTFS on an LVM2 logical volume inside a LUKS2 container.

> **Status: Phase 1 complete. Phase 2 (`winlvm.sys`) is next.**
>
> `src/lvmfmt/` — the LVM2 on-disk format library — is implemented and
> 22/22 tests pass against real Linux-created images. The corpus has been
> enhanced with ext4 filesystem data and a machine-readable manifest.
> The kernel driver does not exist yet.

## Start here

| Document | What it is |
|---|---|
| [`docs/FEASIBILITY.md`](docs/FEASIBILITY.md) | Whether the boot goal is achievable, the boot-chain analysis, prior art worth deriving from, effort estimates, and go/no-go gates with kill criteria. |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | What to build, in what order, and the decisions that block starting. |
| [`corpus/README.md`](corpus/README.md) | The ground-truth corpus: real Linux-created LVM2/LUKS2 images with oracle files. |

## Goals

1. **Full-featured LVM2 on Windows** — read _and_ write access to volumes
   created on Linux, not a read-only viewer.
2. **Boot Windows from an LVM2 logical volume.**
3. **With the physical volume inside a LUKS2 container.**

## Why the prototype was deleted

The earlier `src/` tree contained a user-mode LVM2 metadata scanner that worked
for single-PV linear volumes, and a kernel driver that could never work. Rather
than build on it, it was removed at commit `6bbbe7a` — recoverable from git
history if ever needed.

The reasons, in short:

- The driver was a legacy non-PnP design with no INF. It never registered a
  volume interface, so Windows could not treat its output as a real volume, and
  it reopened the backing device with `ZwCreateFile` **on every read IRP**. That
  is unusable for a data volume and categorically impossible on a boot path.
- Whole subsystems were unreachable: `stripe_support.cpp` was fully implemented
  but no code path could call it, `image_reader.cpp` was a 9-line stub, and the
  "activation service" was a `Sleep` loop nothing talked to.
- Activation fabricated a fake handle when the driver was absent and printed
  success indistinguishable from the real thing.
- Nothing had ever been validated against a real Linux-created LVM image.

The correct architecture — a winmd-shaped WDM volume manager — differs from that
design at essentially every layer. `docs/FEASIBILITY.md` §5.1 covers it.

## What exists now

```
corpus/
  generate.sh          Linux-side: builds real LVM2/LUKS2 images (run as root in Linux VM)
  expected/            oracle files: dmsetup table, lvs/pvs/vgs, metadata text,
                         lv-hashes, per-LV ext4 filesystem oracles (fs-<lv>/)
  images/              12 compressed .img.zst files
  corpus-manifest.json machine-readable summary: all cases, images, file checksums

docs/
  FEASIBILITY.md       boot-chain feasibility assessment, prior art, go/no-go gates
  ROADMAP.md           phased plan with 8 phases and 4 blocking decisions

src/lvmfmt/            LVM2 on-disk format library (C++20, platform-agnostic)
  bytes.h              overflow-checked arithmetic for untrusted on-disk data
  crc32.{h,cpp}        LVM2's non-standard CRC-32 (poly 0xedb88320, init 0xf597a6cf)
  source.{h,cpp}       ByteSource abstraction + FileSource
  config.{h,cpp}       LVM2 text metadata grammar parser
  label.{h,cpp}        LABELONE, PV header, mda_header, ring-buffer read
  metadata.{h,cpp}     VolumeGroup / PhysicalVolume / LogicalVolume object model
  mapping.{h,cpp}      extent → device-mapper table, overflow-checked sector arithmetic

src/tools/lvmdump/     CLI: read an image, print computed DM table

tests/
  test_units.cpp       CRC-32 vectors, config parser, overflow checks, label parsing
  test_corpus.cpp      Tier 1 (metadata text) + Tier 2 (real images) oracle tests

third_party/           reference source only (LVM2, WinBtrfs) — not built
shared/                transfer area for the Linux corpus-generation VM (normally empty)
```

### Corpus filesystem data

Every LV in the corpus contains an **ext4 filesystem** with a known set of
files (`hello.txt`, `pattern-1k.bin`, `pattern-4k.bin`, `random-1m.bin`,
`subdir/nested/deep.txt`). This is the **read verification anchor** for Phase 2:
once the Windows driver can read `hello.txt` through the LV and compare it
byte-for-byte against `corpus/expected/<case>/fs-<lv>/files/hello.txt`, Stage 2
has real evidence.

`corpus-manifest.json` at the corpus root is the **entry point for agents and
automation** — it lists every image, LVs, VG metadata summary, and the SHA-256
of every file on each LV's filesystem. Read it before touching the corpus.

> **Corpus must be regenerated.** `generate.sh` was updated on 2026-07-28.
> The existing `images/` and `expected/` predate the ext4 data and
> `corpus-manifest.json`. Re-run `sudo ./generate.sh` on the Linux VM.

## Working agreement

The rule that matters, because the previous iteration broke it and left behind
documentation nobody could trust:

**No claim without a committed artifact.** If a test passed, commit the log. If
a driver loaded, commit the evidence. If it isn't in the repository, it didn't
happen.

The corpus in `corpus/expected/` is what that looks like in practice: every
number the Windows side will be asked to reproduce came out of Linux and is
checked in.
