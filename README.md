# winlvm

Native Windows LVM2-compatible tooling for existing Linux-created LVM disks.

This repository is intentionally starting with a metadata/image-mode prototype. It does not expose Windows block
devices yet, does not write LVM metadata, and does not activate real disks. The first production-quality slice is a
read-only scanner that can consume Linux-created disk/PV images, parse LVM2 metadata, reconstruct supported linear
logical-volume mappings, and emit deterministic JSON for oracle tests.

## Current Scope

- C++20, CMake, and CTest.
- LVM-style command names: `lvm`, `pvs`, `vgs`, `lvs`, `pvscan`, `vgscan`, `lvscan`, `pvdisplay`, `vgdisplay`,
  `lvdisplay`, `lvchange`, and `vgchange`.
- Image-mode only through repeated `--image <path>`.
- Explicit read-only Windows source devices through repeated `--device <path>`.
- JSON debug output with `--reportformat json` or `lvm dump --image <path> --json`.
- Raw read-only LV export with `lvm export <vg/lv> --image <path> --output <path> --readonly`.
- Bounded SHA-256 LV range validation with `lvm hash <vg/lv> --offset <bytes> --length <bytes>`.
- Expected-hash comparison with `lvm verify <vg/lv> --offset <bytes> --length <bytes> --expected-hash <hex>`.
- Direct-PV and GPT/primary-MBR whole-disk image candidate scanning.
- LVM2 PV label, metadata area, raw metadata location, metadata CRC, and simple text metadata parsing.
- Linear mappings represented by LVM metadata as `type = "linear"` or single-stripe `type = "striped"`.
- Fail-closed diagnostics for duplicate PV UUIDs, missing PVs, ambiguous newest metadata, unsupported segment types,
  hidden/internal LVs, and out-of-bounds mappings.

## Explicit Deferrals

Physical disk scanning, activation services, the Windows kernel mapping driver, WinBtrfs integration, read/write support,
metadata mutation, VG `system_id` claiming, striped multi-PV mappings, snapshots, thin provisioning, RAID, mirrors,
cache/writecache, LUKS, discard/TRIM, and concurrent access policy enforcement are deferred.

## Build

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Examples

```powershell
build\pvs.exe --image .\single-pv-linear.img
build\vgs.exe --image .\single-pv-linear.img
build\lvs.exe --image .\single-pv-linear.img
build\lvm.exe dump --image .\single-pv-linear.img --json
build\lvm.exe export vg0/home --image .\single-pv-linear.img --output .\home.raw --readonly
build\lvm.exe export vg0/home --image .\single-pv-linear.img --output .\home-head.raw --readonly --offset 0 --length 131072
build\lvm.exe hash vg0/home --image .\single-pv-linear.img --offset 0 --length 131072
build\lvm.exe verify vg0/home --image .\single-pv-linear.img --offset 0 --length 131072 --expected-hash <linux-sha256>
build\lvm.exe dump --device \\.\Harddisk1Partition1 --json
build\Debug\lvm.exe hash myVG/myLV --device \\.\Harddisk1Partition1 --offset 0 --length 131072
```

All image and device inputs are opened read-only. Linux metadata `device = "/dev/..."` paths are never trusted as
backing devices. Export refuses to overwrite an existing output file.

For large real LVs, validate bounded ranges before attempting full export. Example Linux oracle:

```bash
dd if=/dev/vg0/home bs=1 skip=0 count=131072 status=none | sha256sum
```

Compare that hash with a bounded Windows hash of the same offset and length:

```powershell
build\Debug\lvm.exe hash vg0/home --device \\.\Harddisk1Partition1 --offset 0 --length 131072
build\Debug\lvm.exe verify vg0/home --device \\.\Harddisk1Partition1 --offset 0 --length 131072 --expected-hash <linux-sha256>
```

`lvm hash` and `lvm verify` require `--length` so real-device validation remains intentionally bounded. Bounded
`lvm export` plus `Get-FileHash` remains useful when a raw range file is needed for inspection, but hashing in-process is
preferred for oracle checks because it avoids writing LV bytes to disk.
