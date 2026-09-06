# Ground-truth corpus

Real LVM2/LUKS2 disk images created by Linux, with oracle files captured from
LVM itself. This is Stage 0 in [`../docs/ROADMAP.md`](../docs/ROADMAP.md).

**Nothing in `src/` has ever been validated against real Linux-created data.**
Until this corpus exists and the tests run against it, every correctness claim
this project makes is faith-based. Build it before writing new code.

## Generating

On a Linux live VM, as root:

```bash
sudo ./generate.sh
```

### Disk space — read this on a live ISO

A live ISO's root filesystem is a **RAM disk**, and the raw images peak around
700 MB before compression. Staging them there fills it and produces failures far
from the cause: `zstd` dying, or `cryptsetup luksFormat` failing partway through
writing a 16 MiB header.

The script therefore splits its output:

|                                | Location                                                                                  | Size                               |
| ------------------------------ | ----------------------------------------------------------------------------------------- | ---------------------------------- |
| Raw `.img` while building      | `$WORK` — auto-detected as `~/disk/winlvm-corpus-work` if `~/disk` exists, else `./.work` | ~700 MB, deleted after compression |
| Final `.img.zst` + `expected/` | `$OUT` — this directory by default                                                        | ~70 MB, kept                       |

Override explicitly if needed:

```bash
sudo WORK=/mnt/bigdisk/work ./generate.sh /path/to/output
```

It preflights free space in both locations and refuses to start with a clear
message rather than failing six cases in.

Needs `lvm2`, `cryptsetup`, `python3`, `zstd`, `e2fsprogs`. SystemRescue and
the Arch ISO have everything. On an Fedora live image:

```bash
sudo dnf check-update; sudo dnf install -y lvm2 cryptsetup zstd e2fsprogs
```

Do **not** use WSL2 — its kernel generally lacks the device-mapper targets and
loop `--sector-size` support these cases need, and the failures are confusing
rather than obvious.

### Re-running

**Safe to run repeatedly.** It tears down its own leftovers first, then deletes
and rebuilds `images/` and `expected/` from scratch.

Teardown is scoped strictly to what this script creates: the VG names in
`OUR_VGS`, the dm mappings in `OUR_MAPS`, and loop devices whose backing file
lives under this directory's `images/`. It deliberately never calls
`dmsetup remove_all` or similar, so running it on a machine with real LVM
volumes will not touch them. Use a VM anyway.

### If you generated a corpus before 2026-07-27, regenerate it

An earlier version of `generate.sh` called `vgremove` from its teardown, and
teardown ran once more immediately before compression. Every volume group was
therefore **deleted from the images just before they were saved**. The oracle
files were captured while the VGs were still active, so they are correct and
look perfect — but the shipped images had their metadata areas zeroed, and
`raw_locn[0]` reads as all zeros with a valid CRC over it.

Fixed by splitting teardown in two: `purge_previous()` (destructive, startup
only) and `release_devices()` (non-destructive, everything else). A
post-teardown verification pass now re-attaches every image and confirms `pvs`
still reports a volume group, so this class of bug fails the run instead of
shipping.

The lesson generalises: **verify the artifact, not the process that produced
it.** Every check ran while the VGs were alive, which is precisely the state
that does not ship.

### Telling whether it worked

The script prints a per-case result and a summary, and **exits non-zero if any
case failed**:

```
======================================================================
passed (9): linear-1pv linear-1pv-multiseg linear-2pv striped-2pv ...
======================================================================
```

Failures are reported with the offending line, e.g.
`!! FAILED at line 214: lvextend -y -L 12M /dev/vgseg/lvA`, and that one case
is skipped rather than aborting the run. Lines beginning `~` are _expected_
soft skips — optional device-mapper targets (`dm-thin-pool`, `dm-raid`) that a
minimal live image may not ship. Only `mixed-vg` uses those, and it still
produces a useful image without them.

A complete run yields **9 directories** under `expected/` and **12 `.img.zst`
files** under `images/` (three cases use two PVs each). The script prints both
the expected and actual counts at the end. If you see fewer, read the summary —
it names exactly which case died and where.

Verification in the script:

- every LV's ext4 filesystem is written and then **read back** via
  `capture_fs_oracles` (mount read-only, checksum all files);
- `capture` asserts each oracle file exists and is non-empty;
- both LUKS cases are closed and then **re-opened from scratch**, with the ext4
  filesystem mounted and `hello.txt` checked, proving the container is
  genuinely readable rather than merely writable once in-process.

### A note on LVM warnings

LVM 2.03.17+ keeps a devices file and prints `Device /dev/loopN has no PVID
(devices file ...)` for anything not listed in it. On a live ISO that is pure
noise, and worse, it can make LVM skip the loop devices entirely. The script
points `LVM_SYSTEM_DIR` at a throwaway config with `use_devicesfile = 0`, so
those warnings should no longer appear. If you still see them, something is
overriding `LVM_SYSTEM_DIR`.

## What each image is for

| Image                 | Exercises                                           | Breaks an implementation that…                                      |
| --------------------- | --------------------------------------------------- | ------------------------------------------------------------------- |
| `linear-1pv`          | baseline single-PV linear LV                        | — (if this fails, nothing else matters)                             |
| `linear-1pv-multiseg` | one LV, several **discontiguous** segments          | treats an LV as one `start + length` range                          |
| `linear-2pv`          | LV spanning two PVs                                 | can't resolve PV UUIDs, or mishandles the cross-device seam         |
| `striped-2pv`         | round-robin striping, 64 KiB stripes                | assumes linear; the prototype's stripe code has never run           |
| `sector4k`            | 4Kn PV                                              | hardcodes 512-byte sectors                                          |
| `odd-geometry`        | 2 MiB extents, shifted `pe_start`, **two MDAs**     | assumes 4 MiB extents, 1 MiB `pe_start`, or one metadata area       |
| `mixed-vg`            | thin/raid1/snapshot beside a plain linear LV        | fails the whole VG instead of rejecting only the unsupported LVs    |
| `luks2-default`       | LUKS2 stock defaults (argon2id, aes-xts-plain64)    | the realistic data-volume case                                      |
| `luks2-pbkdf2`        | PBKDF2, the **boot-candidate** config (decision D4) | argon2id at 1 GiB is infeasible pre-boot and in a boot-start driver |

Every image also has its metadata area deliberately churned so the MDA ring
buffer **wraps** and `seqno` climbs well past 1. Wrap-around handling is a
classic silent-corruption path and the prototype has never seen a wrapped MDA.

## Filesystem data (verification anchor)

Each LV contains an **ext4 filesystem** with a known set of files, written by
`generate.sh` before the image is compressed. This data serves two purposes:

1. **Read verification**: the Windows driver reads the LV and checks whether
   these files are intact. If the read path works, the mapping is correct.
2. **Write verification anchor**: once the read path is proven correct, future
   writes are verified by "write X via the driver → read X back through the
   proven read path → compare against the oracle."

### Files written to each LV

| File                     | Size            | Content                                                                                       |
| ------------------------ | --------------- | --------------------------------------------------------------------------------------------- |
| `hello.txt`              | ~140 bytes      | Text: LV name, VG name, case name                                                             |
| `pattern-1k.bin`         | 1,024 bytes     | Repeating LV name (recognisable in hex dumps)                                                 |
| `pattern-4k.bin`         | 4,096 bytes     | Same, page-aligned boundary                                                                   |
| `random-1m.bin`          | 1,048,576 bytes | Deterministic pseudo-random (SHA-256 chain seeded by LV name; identical across regenerations) |
| `subdir/nested/deep.txt` | ~25 bytes       | Exercises directory traversal                                                                 |

The file set exercises:

- Text encoding and line endings
- Small binary at sub-block sizes
- Page-aligned binary (4 KiB, common buffer size)
- Large binary (1 MiB, multi-block reads, incompressible)
- Nested directory structure

## Oracle files

Per image, under `expected/<name>/`:

| File                                   | Why it matters                                                                                                        |
| -------------------------------------- | --------------------------------------------------------------------------------------------------------------------- |
| `dmsetup-table.txt`                    | **The authoritative answer.** Exact sector-level mapping as the kernel built it. Your extent map must reproduce this. |
| `dmsetup-deps.txt`, `dmsetup-info.txt` | device relationships                                                                                                  |
| `lvs.txt`                              | `seg_pe_ranges`, `segtype`, stripe count/size, per-segment PE layout                                                  |
| `pvs.txt`                              | `pe_start`, `pv_pe_count`, MDA count — the offset math inputs                                                         |
| `vgs.txt`                              | extent size, `seqno`, `system_id`                                                                                     |
| `vg-metadata.txt`                      | the on-disk text metadata verbatim (`vgcfgbackup`)                                                                    |
| `lv-hashes.txt`                        | SHA-256 of whole LVs and of 128 KiB ranges; reflects the ext4 filesystem content as shipped                           |
| `luksDump.txt`                         | LUKS2 header: keyslots, segments, digests, actual data offset                                                         |
| `image-checksums.txt`                  | integrity of the images themselves                                                                                    |
| `PROVENANCE.txt`                       | tool versions — LVM output formats drift between releases                                                             |

### Filesystem oracles (per LV)

Under `expected/<case>/fs-<lv>/`:

| File                           | Content                                                                |
| ------------------------------ | ---------------------------------------------------------------------- |
| `listing.txt`                  | `find` output of all files and directories on the LV's ext4 filesystem |
| `checksums.txt`                | SHA-256 of every file on the filesystem (sha256sum format)             |
| `files/hello.txt`              | Byte-exact copy of `hello.txt` for direct comparison                   |
| `files/pattern-1k.bin`         | Byte-exact copy of the 1 KiB pattern file                              |
| `files/subdir/nested/deep.txt` | Byte-exact copy of the nested text file                                |

These files are the **ground truth** for Windows-side read tests. The Windows
test reads the LV through the driver, extracts `hello.txt`, and compares it
byte-for-byte against `expected/<case>/fs-<lv>/files/hello.txt`.

## `corpus-manifest.json`

A single machine-readable file at the corpus root, generated automatically at
the end of each run. Contains every case, its images, VG metadata summary, and
the checksums of all filesystem files. This is the **entry point for agents
and automated tools** — read this file to discover what the corpus contains
and what to verify.

```json
{
  "cases": {
    "linear-1pv": {
      "description": "Baseline single-PV linear LV",
      "images": ["linear-1pv.img.zst"],
      "vg_name": "vgbase",
      "extent_size_sectors": 8192,
      "pv_count": 1,
      "lvs": {
        "lvlin": {
          "fs_type": "ext4",
          "files": [
            { "path": "./hello.txt", "sha256": "..." },
            { "path": "./pattern-1k.bin", "sha256": "..." }
          ]
        }
      }
    }
  }
}
```

## About the committed master keys

`MASTER-KEY-INSECURE-TEST-ONLY.txt` contains the LUKS2 volume key in plaintext,
and `passphrase.txt` the passphrase. This is deliberate and safe: these are
throwaway containers whose passphrase is published in `generate.sh`.

It is also genuinely useful — it lets you **unit-test the XTS sector-crypto
layer independently of the keyslot/KDF layer**. Without it, a decryption failure
is ambiguous between "my Argon2 is wrong", "my AF-merge is wrong", and "my XTS
tweak is wrong". With it you can isolate all three.

Never do this for a real container.

## Size — what to expect, and why LUKS is different

Images are stored zstd-compressed. Decompress before use:

```bash
zstd -d images/linear-1pv.img.zst
```

| Image                                                                   | Raw     | Compressed        | Why                                                                                                                                                    |
| ----------------------------------------------------------------------- | ------- | ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| plain (`linear-*`, `striped-*`, `sector4k`, `odd-geometry`, `mixed-vg`) | 128 MiB | **~1–5 MiB each** | Mostly zeros (unused PV space) + ext4 metadata + ~1 MiB pseudo-random file. The random file is incompressible, but everything else compresses heavily. |
| `luks2-*`                                                               | 128 MiB | **~30 MiB each**  | Ciphertext is by design indistinguishable from random, so it is **incompressible**.                                                                    |

The LUKS images are large for two unavoidable reasons: a stock LUKS2 format
wipes its **16 MiB keyslots area with random data**, and every written byte of
the LV becomes ciphertext. Their LVs are therefore deliberately kept to 16 MiB.
If corpus size becomes a problem, add `--luks2-keyslots-size 4m` to the
`luksFormat` call in `luks2-pbkdf2` — a smaller keyslots area is realistic for a
boot container anyway. Leave `luks2-default` stock; representing cryptsetup's
actual defaults is the entire point of that case.
