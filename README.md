# winlvm

Native Windows access and volume management for Linux LVM2 storage. WinLVM provides a Windows WDM storage-bus kernel driver (`winlvm.sys`), CLI tools (`lvm.exe`, `lvmdump.exe`), a control-plane service (`winlvm_svc.exe`), a Win32 desktop/tray manager (`winlvm_gui.exe`), and a standalone x64 installer (`winlvm-setup-x64.exe`).

## Capabilities

- **Supported LV layouts**: `linear` (single- and multi-segment), `striped` (multi-PV round-robin), `thin-pool` / `thin` (including thin snapshots), `snapshot` / `snapshot-origin` (plus offline `--merge`), `raid1`, and `mirror`.
- **Disk & sector formats**: Whole-disk and GPT/MBR-partitioned Physical Volumes, 512e and 4Kn sector geometries, multi-MDA metadata areas, and PV/VG/LV tags.
- **Volume management**: PV/VG/LV inspection (`pvs`, `vgs`, `lvs`, `fullreport` with JSON output, `lvmdump`), creation, activation/deactivation, resizing, conversion, and metadata backup/restore.
- **Safety boundaries**: Fails closed (read-only or unmounted) on foreign `system_id`, `READ`-only flags, missing backing PVs, degraded `raid1`/`mirror` writes, CRC/bounds errors, encrypted headers, and unsupported targets (`raid4`/`5`/`6`/`10`, `cache`, `writecache`, `integrity`, `vdo`).
- **LUKS2 composition**: Operates on cleartext block devices, stacking with independent providers such as WinLUKS for `LVM-on-LUKS` volumes.

Clustered LVM (`lvmlockd`), concurrent Linux/Windows read-write mounting, boot-start loading, crash-dump filtering, and UEFI pre-boot drivers are out of scope.

## Quick Start

### Windows Usage

Run `winlvm-setup-x64.exe` as Administrator to install `winlvm.sys`, register the `winlvm-svc` service, add `lvm.exe` / `lvmdump.exe` to `PATH`, and install `winlvm_gui.exe` (use `winlvm-uninstall.exe` to remove).

```powershell
lvm.exe version
lvm.exe vgscan
lvm.exe lvs
```

### Build and Test (Linux or Windows)

Requires CMake 3.24+, Ninja (or Visual Studio), a C11/C++20 compiler, and Python 3 (used at build time to generate test disk images and oracle files into the build directory):

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Inspect a generated test disk image:

```bash
./build/lvmdump build/corpus-images/striped-2pv-a.img
./build/lvm --image build/corpus-images/thin-pool.img lvs --reportformat json
```

See [`BUILD.md`](BUILD.md) for MSVC/MinGW-w64 cross-compilation, WDK kernel driver builds (`winlvm.sys`), installer packaging, and test-signing installation.

## Documentation

- [`BUILD.md`](BUILD.md) — User-mode, MinGW-w64, WDK driver, and installer build instructions.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — Driver/service/library architecture, control-plane IOCTL ABI, on-disk format details, and repository layout.
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — Engineering principles, architectural decisions, remaining milestones, and out-of-scope boundaries.

## Acknowledgements

- **[Linux LVM2](https://gitlab.com/lvmteam/lvm2)** (`lvmteam/lvm2`) — Upstream LVM2 on-disk metadata format, label/MDA ring specification, CRC-32 convention, and CLI/JSON reporting reference.
- **[WinBtrfs](https://github.com/maharmstone/btrfs)** (Mark Harmstone) — Architectural reference for Windows kernel storage/volume PnP integration and Windows-Linux filesystem interoperability.
- **[Linux Kernel Device Mapper](https://www.kernel.org/)** (`dm-thin`, `dm-snapshot`, `dm-raid`, `dm-log`) & **[thin-provisioning-tools](https://github.com/jthornber/thin-provisioning-tools)** — On-disk superblock, B-tree, space-map, persistent COW, and bitmap format specifications.
- **[cryptsetup](https://gitlab.com/cryptsetup/cryptsetup)** & **WinLUKS** — LUKS2 opaque header recognition and decoupled block-device stacking integration.
- **Microsoft Windows Driver Kit (WDK)** — Windows WDM, PnP/Power, Storage/Volume IOCTL, and Mount Manager interfaces.

## License

Licensed under the GNU Lesser General Public License v3.0 or later. See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE).
