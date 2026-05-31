# Architecture Notes

## Boundary

`winlvm` is split into two long-term layers:

- User mode parses LVM metadata, validates safety, builds mapping tables, and controls activation.
- Kernel mode will receive only validated mapping tables and perform deterministic block I/O remapping.

The kernel driver must remain clean-room with respect to LVM2 metadata parsing. It should not embed LVM2 source code or
parse full VG text metadata.

## First Slice

The current implementation is user-mode image scanning only:

1. Read one or more image files or explicit Windows device paths read-only.
2. Discover candidate PV base offsets from direct image offset zero, GPT partitions, and primary MBR partitions.
3. Scan the first four sectors relative to each candidate PV base for an LVM2 label.
4. Parse PV label, data area, metadata area headers, raw metadata locations, and VG text metadata.
5. Select one unambiguous newest VG metadata copy by `seqno`.
6. Build byte-accurate mappings for visible linear logical volumes.
7. Optionally export a supported LV to a raw file by copying mapped byte ranges from supplied read-only images.

All computed offsets are byte offsets. LVM sector fields are interpreted as 512-byte sectors.

## Mapping Invariants

- `pv_base_offset_bytes` is first-class and must be included in debug JSON.
- Mapping source offsets are computed as `image_offset = pv_base_offset_bytes + pv_relative_offset_bytes`.
- Windows raw device reads are internally rounded to sector-aligned reads and sliced back to the requested byte range.
- LV byte zero is preserved exactly; no synthetic partition offset is introduced.
- Unsupported or ambiguous metadata prevents topology activation/export.
- Raw export requires a supported visible LV, contiguous full-LV mapping coverage, and a non-existing output path.
- Future kernel IOCTL tables must repeat validation in kernel mode; user-mode validation is not a substitute for driver
  boundary checks.
