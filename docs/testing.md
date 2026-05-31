# Testing Strategy

## Local Tests

`winlvm_tests` builds synthetic LVM-like images in the test temp directory and verifies:

- parser support for LVM text sections and single-stripe linear mappings;
- unsupported segment rejection;
- direct PV image scanning;
- MBR partition PV base preservation;
- duplicate PV UUID fail-closed behavior;
- raw LV export byte-for-byte output;
- bounded raw LV export byte-for-byte output;
- bounded SHA-256 LV range hashing;
- bounded export out-of-bounds rejection;
- raw export overwrite refusal;
- compact PV label UUID matching hyphenated VG metadata UUIDs;
- deterministic JSON schema presence.

## Golden Tests

Golden fixtures should be generated on Linux, not WSL at test runtime:

```bash
pvs -o+pv_uuid,pe_start,pv_size,dev_size
vgs -o+vg_uuid,vg_attr,vg_seqno
lvs -a -o+lv_uuid,segtype,devices,seg_start,seg_size,stripes,stripesize
dmsetup table
```

Commit small generated images and expected JSON/oracle outputs once available. Windows CI should compare scanner JSON
against these files without requiring Linux, WSL, or privileged disk access.

## Range Hash Validation

Before full raw export of a large LV, validate bounded ranges against Linux:

```bash
dd if=/dev/vg0/home bs=1 skip=0 count=131072 status=none | sha256sum
dd if=/dev/vg0/home bs=1 skip=65536 count=131072 status=none | sha256sum
```

Windows should normally use in-process bounded hashing so validation does not write LV bytes to a file:

```powershell
build\Debug\lvm.exe hash vg0/home --device \\.\Harddisk1Partition1 --offset 0 --length 131072
build\Debug\lvm.exe verify vg0/home --device \\.\Harddisk1Partition1 --offset 0 --length 131072 --expected-hash <linux-sha256>
```

`lvm hash` prints the SHA-256 digest for the requested LV byte range. `lvm verify` computes the same bounded digest and
returns success only when it matches `--expected-hash`. Both commands require `--length` to avoid accidental full-LV reads
on real devices.

Use bounded export only when a raw range file is needed for inspection:

```powershell
build\Debug\lvm.exe export vg0/home --device \\.\Harddisk1Partition1 --output .\home-range.raw --readonly --offset 0 --length 131072
Get-FileHash .\home-range.raw -Algorithm SHA256
```

For the attached Disk 1 Partition 1 smoke test on this machine, `myVG/myLV` hashed the first 131072 bytes with SHA-256:

```text
967ed9c89b5571545ff9f64248e87156f406a7702542d6aa162d3dfe89d204f2
```
