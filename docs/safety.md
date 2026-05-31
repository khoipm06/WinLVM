# Safety Policy

## Current Prototype

- Image inputs are read-only.
- Explicit `--device` inputs are opened with `GENERIC_READ` only and shared read/write access.
- No activation, writes, metadata mutation, or automount behavior is implemented.
- Linux metadata device paths are diagnostic only and are not used for I/O.
- Raw export writes only the requested output file and refuses to overwrite an existing file.
- Raw export supports `--offset` and `--length` for bounded validation of large LVs.

## Fail-Closed Cases

The scanner reports errors and refuses to produce usable topology for:

- duplicate PV UUIDs across supplied inputs;
- missing PVs referenced by selected VG metadata;
- newest metadata copies with matching VG UUID/seqno but different text;
- unsupported LV status or segment types;
- hidden/internal LVs;
- non-contiguous LV segments;
- arithmetic overflow;
- mappings outside the supplied backing image.
- raw export requests for ambiguous, missing, unsupported, or partially mapped LVs.

## Future Real-Disk Guard Ladder

Before physical disk support, the service/driver design must define how to identify and reject boot/system disks,
BitLocker-managed devices, Windows dynamic disks, Storage Spaces, mounted Windows volumes, overlapping writable
activations, and any case where Windows cannot lock or offline the relevant backing objects safely.

The current `--device` path is scan/export input only. It is not activation, does not lock/dismount/offline anything,
and must remain read-only until the service/driver safety model exists.
