# winlvm

Native Windows access to Linux LVM2 volumes — with the longer-term goal of
booting Windows from NTFS on an LVM2 logical volume inside a LUKS2 container.

> **Status: clean slate. There is no implementation in this repository yet.**
>
> The previous prototype was deleted deliberately (see below). What remains is
> the analysis that says what to build, and the ground-truth corpus that says
> whether it is correct.

## Start here

| Document | What it is |
|---|---|
| [`docs/FEASIBILITY.md`](docs/FEASIBILITY.md) | Whether the boot goal is achievable, the boot-chain analysis, prior art worth deriving from, effort estimates, and go/no-go gates with kill criteria. |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | What to build, in what order, and the decisions that block starting. |
| [`corpus/README.md`](corpus/README.md) | The ground-truth corpus: real Linux-created LVM2/LUKS2 images with oracle files. |

## Goals

1. **Full-featured LVM2 on Windows** — read *and* write access to volumes
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
corpus/     ground truth: 12 real LVM2/LUKS2 images + oracle files from Linux
docs/       the feasibility assessment and the roadmap
shared/     transfer area for the Linux corpus-generation VM (normally empty)
third_party/  reference source only (LVM2, WinBtrfs) — not built
```

## Working agreement

The rule that matters, because the previous iteration broke it and left behind
documentation nobody could trust:

**No claim without a committed artifact.** If a test passed, commit the log. If
a driver loaded, commit the evidence. If it isn't in the repository, it didn't
happen.

The corpus in `corpus/expected/` is what that looks like in practice: every
number the Windows side will be asked to reproduce came out of Linux and is
checked in.
