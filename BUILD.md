# Building winlvm

There are two build surfaces, kept deliberately separate:

## 1. `lvmfmt` and the tests — portable, user-mode

This builds with CMake on any toolchain. It is the ground-truth-pinned format
library plus the corpus oracle tests.

```
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Required: CMake 3.24+, Ninja or a Visual Studio generator, zstd (for the
corpus image decompression step; the corpus tests SKIP without it).

CI runs this on `windows-latest` via `.github/workflows/ci.yml`.

## 2. `winlvm.sys` — the WDM kernel driver

This **cannot** be built by the CMake set above and is not built by CI yet.
Building a WDM driver needs the Windows Kernel-Mode Driver toolchain, which is
only available through the WDK with Visual Studio. Build it like this:

### One-time setup

1. Install Visual Studio 2022 with the **Desktop development with C++** and
   **Windows Driver Kit (WDK)** components. We use the WDM model
   (not KMDF), so make sure the WDM templates and headers are present.
2. Open `src/driver/winlvm.vcxproj` either in Visual Studio or build it from
   a Developer Command Prompt:

   ```
   msbuild src\driver\winlvm.vcxproj /p:Configuration=Release /p:Platform=x64
   ```

The build produces `winlvm.sys` and an INF/CAT package.

### What the .vcxproj compiles

- `winlvm_main.cpp` — DriverEntry, AddDevice, dispatch table, CDO
- `winlvm_pnp.cpp` — PnP device interface notification, PV probe worker
- `winlvm_ioctl.cpp` — Mount Manager IOCTL handlers (LV identity)
- `winlvm_io.cpp` — IRP_MJ_READ / IRP_MJ_WRITE, extent fan-out
- `winlvm_new.cpp` — kernel-side `operator new`/`delete` so the format
  library's `std::vector`/`std::string` allocate from the nonpaged pool
- selected `src/lvmfmt/*.cpp` files
- `winlvm.inf` and `winlvm.cat`

### Driver entry points and the prior-art copy rule

The architecture is a transposition of `winmd` (maharmstone/winmd):
root-enumerated bus PDO + control device, `IoRegisterPlugPlayNotification`
on `GUID_DEVINTERFACE_DISK`, `IoGetDeviceObjectPointer` + `IoAllocateIrp`
+ `IoCallDriver` to probe the PV label — **never `ZwCreateFile`**, which is
the structural mistake that made the previous prototype unbootable.

### Testing the driver

For the Stage 2 exit criterion, install via right-click → Install on
`winlvm.inf` on a clean VM, then verify that a Linux-created LV appears in
Disk Management with a drive letter, and that an NTFS volume on it mounts
read-only. See `docs/ROADMAP.md` §Phase 2 for the full checklist.

### Signing

Test-signing during Phase 2 development uses the WDK's `MakeCert` /
`Inf2Cat` / `SignTool` toolchain via `DriverSign` in the `.vcxproj`.

Production signing (Microsoft attestation + third-party UEFI CA for the
DXE driver) is the Phase 8 concern (`ROADMAP.md` §Phase 8).
