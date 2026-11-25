# Test Corpus

Test disk images are generated deterministically out-of-tree at build time by
[`scripts/gen_test_fixtures.py`](../scripts/gen_test_fixtures.py) into
`${CMAKE_BINARY_DIR}/corpus-images/`. Expected LVM2 metadata, device-mapper tables, and SHA-256 oracle specifications live under `expected/` alongside `corpus-manifest.json`.

## Generating Locally

```bash
python3 scripts/gen_test_fixtures.py --out-dir build/corpus-images
```

To regenerate the text/hash oracle files alongside the images:

```bash
python3 scripts/gen_test_fixtures.py \
  --out-dir build/corpus-images \
  --expected-dir corpus/expected \
  --manifest corpus/corpus-manifest.json
```

## Cases

| Case | Images | Coverage |
| --- | --- | --- |
| `linear-1pv` | `linear-1pv.img` | Single-PV linear LV with PV, VG, and LV tags |
| `linear-1pv-multiseg` | `linear-1pv-multiseg.img` | 3-segment discontiguous linear LV + wrapped MDA ring |
| `linear-2pv` | `linear-2pv-a.img`, `linear-2pv-b.img` | Linear LV spanning two PVs |
| `striped-2pv` | `striped-2pv-a.img`, `striped-2pv-b.img` | 2-PV round-robin striped LV (64 KiB stripe chunks) |
| `sector4k` | `sector4k.img` | 4Kn-aligned PV geometry |
| `odd-geometry` | `odd-geometry.img` | 2 MiB extents, dual MDAs (`4096` and `133169152`), wrapped MDA ring |
| `thin-pool` | `thin-pool.img` | `dm-thin` v2 pool with sparse thin LV and thin snapshot |
| `snapshot` | `snapshot.img` | Classic LVM2 `snapshot-origin` + persistent `SnAp` COW snapshot |
| `raid1-mirror` | `raid1-mirror-a.img`, `raid1-mirror-b.img` | 2-PV `raid1` (`DmRd` superblocks), core-log `mirror`, disk-log `mirror` |
| `mixed-vg` | `mixed-vg-a.img`, `mixed-vg-b.img` | Supported `snapshot-origin`, `snapshot`, `thin`, `raid1` alongside unsupported `cache` |
| `luks2-default` | `luks2-default.img` | Opaque LUKS2 v2 header (Argon2id) rejected as non-PV |
| `luks2-pbkdf2` | `luks2-pbkdf2.img` | Opaque LUKS2 v2 header (PBKDF2) rejected as non-PV |

## Oracle Files

Each non-LUKS case under `expected/<case>/` contains:

- `vg-metadata.txt` — canonical LVM2 text metadata for the volume group
- `dmsetup-table.txt` — expected device-mapper targets
- `lv-hashes.txt` — SHA-256 checksums for four 128 KiB windows and the full LV stream
- `vgs.txt`, `pvs.txt`, `lvs.txt` — concise identity summaries
