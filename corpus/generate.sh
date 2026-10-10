#!/usr/bin/env bash
# Ground-truth corpus generator for winlvm (Stage 0).
#
# Run on Linux as root, in a live VM. Produces raw disk images plus oracle
# files captured from LVM itself. Everything the Windows side is later asked to
# reproduce must be provable from the files this script emits.
#
#   sudo ./generate.sh [outdir]
#
# Safe to re-run: it tears down its own leftovers first. It only ever touches
# VG names and dm mappings it created itself (see OUR_VGS / OUR_MAPS) and loop
# devices backed by files in its own images/ directory. It will not disturb
# anything else on the machine.
#
# Requires: lvm2, cryptsetup, python3, zstd, coreutils, util-linux, e2fsprogs.
# On SystemRescue / Arch everything is present. On Ubuntu live:
#   sudo apt update && sudo apt install -y lvm2 cryptsetup-bin zstd e2fsprogs

set -Eeuo pipefail

# At the start of your script, capture the original user's home
ORIGINAL_USER="${SUDO_USER:-$(whoami)}"
ORIGINAL_HOME=$(eval echo "~$ORIGINAL_USER")

OUT="${1:-$(dirname "$(readlink -f "$0")")}"
if [[ -n "${WORK:-}" ]]; then
    :
elif [[ -d "$ORIGINAL_HOME/disk" && -w "$ORIGINAL_HOME/disk" ]]; then
    WORK="$ORIGINAL_HOME/disk/winlvm-corpus-work"
else
    WORK="$OUT/disk/work"
fi
IMG_DIR="$WORK/images"     # raw .img, discarded after compression
ZST_DIR="$OUT/images"      # compressed .img.zst, kept
EXP_DIR="$OUT/expected"


# LVM 2.03.17+ keeps a devices file (/etc/lvm/devices/system.devices) and warns
# "Device ... has no PVID" for anything not listed. On a live ISO this is pure
# noise and can make commands skip our loop devices entirely. Point LVM at a
# throwaway config that disables it. Built-in defaults cover everything else.
LVM_SYSTEM_DIR="$(mktemp -d)"
export LVM_SYSTEM_DIR
cat > "$LVM_SYSTEM_DIR/lvm.conf" <<'LVMCONF'
devices {
    use_devicesfile = 0
    obtain_device_list_from_udev = 0
    filter = [ "a|.*|" ]
}
backup {
    backup = 0
    archive = 0
}
log {
    verbose = 0
}
LVMCONF

# Generous sizes: cases below allocate a lot of extents, and "insufficient free
# space" is a tedious failure to debug. These are almost entirely zeros and
# compress to a small fraction of this.
SMALL=128M
# Ciphertext is incompressible, so LUKS images dominate the corpus size: a
# stock LUKS2 header wipes a 16 MiB keyslots area with random data, and every
# written byte of the LV becomes ciphertext. Keep these lean.
LUKS_SZ=128M
LUKS_LV=16M

# Everything this script is allowed to destroy.
OUR_VGS=(vgbase vgseg vgspan vgstripe vg4k vgodd vgmix vgluks vgboot)
OUR_MAPS=(wl_luks1 wl_luks2)

PASSPHRASE='winlvm-test-passphrase'
PASSFILE="$(mktemp)"

# Recreate on demand. Teardown must never delete this: it runs before the cases
# and again after any failure, so deleting it there silently breaks every later
# LUKS case with a "cannot open key file" at luksFormat.
ensure_passfile() {
  [[ -s "$PASSFILE" ]] || printf '%s' "$PASSPHRASE" > "$PASSFILE"
}
ensure_passfile

PASSED=(); FAILED=()

# ---------------------------------------------------------------------------
# Loud failures. The previous version of this script hid every error behind
# >/dev/null and set -e, so the first failure was indistinguishable from a
# clean exit. That is how a run ends up producing one directory silently.
# ---------------------------------------------------------------------------
err_report() {
  local line=$1 cmd=$2
  echo "  !! FAILED at line $line: $cmd" >&2
}
trap 'err_report "$LINENO" "$BASH_COMMAND"' ERR

# ---------------------------------------------------------------------------
# Teardown. Scoped strictly to our own names — never `dmsetup remove_all`,
# which would wreck a real machine if this were ever run outside a VM.
# ---------------------------------------------------------------------------
# NON-DESTRUCTIVE. Deactivates volume groups, closes LUKS mappings, and detaches
# loop devices. The on-disk metadata is left intact.
#
# This is the ONLY teardown that may run after images are built. An earlier
# version called vgremove here, which deleted every volume group immediately
# before the images were compressed — the oracles were captured while the VGs
# were alive, so they looked right, but the shipped images had their metadata
# areas zeroed. Do not reintroduce vgremove outside purge_previous().
release_devices() {
  for vg in "${OUR_VGS[@]}"; do
    vgs "$vg" &>/dev/null && vgchange -an "$vg" &>/dev/null || true
  done

  for m in "${OUR_MAPS[@]}"; do
    [[ -e "/dev/mapper/$m" ]] && cryptsetup close "$m" &>/dev/null || true
  done

  # Detach only loops backed by files under our images/ directory.
  if [[ -d "$IMG_DIR" ]]; then
    local l
    while read -r l; do
      [[ -n "$l" ]] && losetup -d "$l" &>/dev/null || true
    done < <(losetup -O NAME,BACK-FILE --noheadings 2>/dev/null \
             | awk -v d="$IMG_DIR" 'index($2, d) == 1 {print $1}')
  fi

  return 0
}

# DESTRUCTIVE. Removes leftover volume groups from a previous run so their names
# are free again. Only ever called at startup, before any image is created.
purge_previous() {
  echo "clearing leftovers from any previous run"
  for vg in "${OUR_VGS[@]}"; do
    if vgs "$vg" &>/dev/null; then
      vgchange -an "$vg" &>/dev/null || true
      vgremove -f -y "$vg" &>/dev/null || true
    fi
  done
  release_devices
}

cleanup_and_exit() {
  release_devices
  rm -f "$PASSFILE"
  rm -rf "$LVM_SYSTEM_DIR"
}
trap cleanup_and_exit EXIT

[[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 1; }
for t in lvm cryptsetup python3 zstd losetup blockdev mkfs.ext4; do
  command -v "$t" >/dev/null || { echo "missing required tool: $t" >&2; exit 1; }
done

purge_previous
rm -rf "$IMG_DIR" "$EXP_DIR" "$ZST_DIR"
mkdir -p "$IMG_DIR" "$EXP_DIR" "$ZST_DIR"

# Space preflight. Raw images peak around 700 MB; failing here with a clear
# message beats an ENOSPC six cases later.
free_mb() { df -Pm "$1" 2>/dev/null | awk 'NR==2 {print $4}'; }
WORK_FREE=$(free_mb "$WORK"); OUT_FREE=$(free_mb "$OUT")
echo "work dir : $WORK  (${WORK_FREE:-?} MB free)"
echo "output   : $OUT  (${OUT_FREE:-?} MB free)"
if [[ -n "$WORK_FREE" ]] && (( WORK_FREE < 1500 )); then
  echo >&2
  echo "ERROR: only ${WORK_FREE} MB free at $WORK; need ~1500 MB for raw images." >&2
  echo "       On a live-ISO VM the root filesystem is a RAM disk. Stage the" >&2
  echo "       work directory on a real disk instead, e.g.:" >&2
  echo "         sudo WORK=~/disk/winlvm-corpus-work $0" >&2
  exit 1
fi
if [[ -n "$OUT_FREE" ]] && (( OUT_FREE < 200 )); then
  echo "ERROR: only ${OUT_FREE} MB free at $OUT; need ~200 MB for the corpus." >&2
  exit 1
fi
echo

# ---------------------------------------------------------------------------
# Example data.
#
# Each LV gets an ext4 filesystem with a known set of files. This serves as
# verification data: the Windows driver reads the LV and checks whether these
# files are intact. If the read path works, the mapping must be correct.
#
# Files written:
#   hello.txt          — text identification (LV/VG/case name)
#   pattern-1k.bin     — 1024 bytes of repeating LV name
#   pattern-4k.bin     — 4096 bytes of repeating LV name (page-aligned)
#   random-1m.bin      — 1 MiB of deterministic pseudo-random data (SHA-256
#                         chain seeded by LV name, reproducible across runs)
#   subdir/nested/deep.txt — nested directory structure
# ---------------------------------------------------------------------------
write_example_data() {
  local dev="$1" lv_name="$2" vg_name="$3" case_name="$4"
  local mnt
  mnt=$(mktemp -d)

  # -F: force (skip "is a whole device" prompt)
  # -m 0: no reserved blocks (not needed for test data)
  mkfs.ext4 -q -F -m 0 -L "wl-$lv_name" "$dev" >/dev/null 2>&1
  mount "$dev" "$mnt"

  python3 - "$mnt" "$lv_name" "$vg_name" "$case_name" <<'PY'
import os, sys, hashlib

mnt, lv, vg, case_name = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

# 1. hello.txt — human-readable identification
with open(os.path.join(mnt, 'hello.txt'), 'w') as f:
    f.write('Hello from WinLVM corpus generator\n')
    f.write(f'LV: {lv}\n')
    f.write(f'VG: {vg}\n')
    f.write(f'Case: {case_name}\n')
    f.write('This file is verification data for the WinLVM driver.\n')

# 2. pattern-1k.bin — 1024 bytes of repeating LV name
name = lv.encode()
with open(os.path.join(mnt, 'pattern-1k.bin'), 'wb') as f:
    f.write((name * ((1024 // len(name)) + 1))[:1024])

# 3. pattern-4k.bin — 4096 bytes of repeating LV name (page-aligned boundary)
with open(os.path.join(mnt, 'pattern-4k.bin'), 'wb') as f:
    f.write((name * ((4096 // len(name)) + 1))[:4096])

# 4. random-1m.bin — deterministic pseudo-random via SHA-256 chain
block = f'winlvm-{lv}-seed'.encode()
data = bytearray()
for _ in range(32768):  # 32768 * 32 bytes = 1,048,576 = 1 MiB
    block = hashlib.sha256(block).digest()
    data.extend(block)
with open(os.path.join(mnt, 'random-1m.bin'), 'wb') as f:
    f.write(bytes(data[:1048576]))

# 5. Directory structure
nested = os.path.join(mnt, 'subdir', 'nested')
os.makedirs(nested)
with open(os.path.join(nested, 'deep.txt'), 'w') as f:
    f.write(f'nested file in {lv}\n')
PY

  sync
  umount "$mnt"
  rmdir "$mnt"
  echo "    wrote example data on ext4 ($lv_name)"
}

# ---------------------------------------------------------------------------
# Filesystem oracle capture. Mounts the LV read-only and records what files
# are present and their checksums, so the Windows test harness has an exact
# specification of what to expect.
# ---------------------------------------------------------------------------
capture_fs_oracles() {  # case_name vg lv
  local case_name="$1" vg="$2" lv="$3"
  local dev="/dev/$vg/$lv"
  local d="$EXP_DIR/$case_name/fs-$lv"
  mkdir -p "$d/files"

  local mnt
  mnt=$(mktemp -d)
  mount -o ro "$dev" "$mnt"

  # File listing (relative paths, sorted)
  (cd "$mnt" && find . \( -type f -o -type d \) ! -path './lost+found' ! -path './lost+found/*' | sort) > "$d/listing.txt"

  # SHA-256 checksums of all regular files
  (cd "$mnt" && find . -type f ! -path './lost+found/*' | sort | while read -r f; do
    sha256sum "$f"
  done) > "$d/checksums.txt"

  # Copy small files for byte-exact comparison on the Windows side
  cp "$mnt/hello.txt" "$d/files/" 2>/dev/null || true
  cp "$mnt/pattern-1k.bin" "$d/files/" 2>/dev/null || true
  if [[ -f "$mnt/subdir/nested/deep.txt" ]]; then
    mkdir -p "$d/files/subdir/nested"
    cp "$mnt/subdir/nested/deep.txt" "$d/files/subdir/nested/"
  fi

  umount "$mnt"
  rmdir "$mnt"
  echo "    captured fs oracles -> expected/$case_name/fs-$lv/"
}

# Assert the oracle files a case is supposed to produce actually exist and are
# non-empty. dmsetup-table.txt is the one that matters most.
verify_oracles() {
  local name="$1" d="$EXP_DIR/$name" f missing=()
  for f in dmsetup-table.txt lvs.txt pvs.txt vgs.txt vg-metadata.txt lv-hashes.txt; do
    [[ -s "$d/$f" ]] || missing+=("$f")
  done
  if (( ${#missing[@]} )); then
    echo "    !! empty or missing oracle files: ${missing[*]}" >&2
    return 1
  fi
  return 0
}

# Close everything, then re-open the LUKS container from scratch and confirm the
# LV inside is still readable and the ext4 filesystem is intact. Without this, a
# LUKS case reports success having only proven that writing worked once,
# in-process.
verify_luks_roundtrip() { # loopdev vg lv mapname
  local dev="$1" vg="$2" lv="$3" map="$4"
  echo "    re-opening container to verify round-trip"
  cryptsetup open --key-file "$PASSFILE" "$dev" "$map"
  vgchange -ay "$vg" >/dev/null

  local mnt ok=1
  mnt=$(mktemp -d)
  mount -o ro "/dev/$vg/$lv" "$mnt"
  [[ -f "$mnt/hello.txt" ]] || ok=0
  if (( ok )); then
    grep -q "LV: $lv" "$mnt/hello.txt" || ok=0
  fi
  umount "$mnt"
  rmdir "$mnt"

  vgchange -an "$vg" >/dev/null
  cryptsetup close "$map"

  if (( ok )); then
    echo "    roundtrip verified: ext4 readable, hello.txt intact"
  else
    echo "    !! roundtrip FAILED: filesystem or hello.txt corrupted" >&2
    return 1
  fi
  return 0
}

# Churn the metadata area so the MDA ring buffer wraps and seqno climbs well
# past 1. Untested wrap handling is a classic silent-corruption source.
churn_metadata() {
  local vg="$1" rounds="${2:-40}" i done=0
  for i in $(seq 1 "$rounds"); do
    lvcreate -y -L 4M -n "churn$i" "$vg" &>/dev/null || break
    lvremove -y "$vg/churn$i" &>/dev/null || break
    done=$((done + 1))
  done
  local seq; seq=$(vgs --noheadings -o vg_seqno "$vg" | tr -d ' ')
  echo "    churned metadata: $done cycles, seqno now $seq"
  return 0
}

# ---------------------------------------------------------------------------
# Oracle capture. Must run while the VG is ACTIVE so dmsetup can be read.
# ---------------------------------------------------------------------------
capture() {
  local name="$1" vg="$2" luksmap="${3:-}"
  local d="$EXP_DIR/$name"; mkdir -p "$d"

  # Scope device-mapper captures strictly to this case's VG (and optional LUKS
  # mapping) so unrelated host DM devices never enter the oracle files.
  # Invoke dmsetup table/deps without positional args so single-device outputs
  # retain the "<name>: " prefix, then filter lines to this case.
  local dm_line_regex="^${vg}-"
  local dm_name_regex="^${vg}-"
  if [[ -n "$luksmap" ]]; then
    dm_line_regex="^(${vg}-|${luksmap}:)"
    dm_name_regex="^(${vg}-|${luksmap}$)"
  fi

  # The single most important artifact: the kernel's own sector-level mapping.
  dmsetup table | grep -E "$dm_line_regex" > "$d/dmsetup-table-all.txt" || true
  dmsetup table | grep "^${vg}-" > "$d/dmsetup-table.txt" || true
  dmsetup deps  | grep -E "$dm_line_regex" > "$d/dmsetup-deps.txt"  || true
  local dm_devs=()
  mapfile -t dm_devs < <(dmsetup info --noheadings -o name 2>/dev/null | grep -E "$dm_name_regex" | sort || true)
  if (( ${#dm_devs[@]} > 0 )); then
    dmsetup info "${dm_devs[@]}" > "$d/dmsetup-info.txt" || true
  else
    : > "$d/dmsetup-info.txt"
  fi

  lvs --units b --nosuffix -o \
    vg_name,lv_name,lv_uuid,lv_size,seg_start_pe,seg_size_pe,segtype,stripes,stripe_size,seg_pe_ranges,devices \
    "$vg" > "$d/lvs.txt"

  pvs --units b --nosuffix -S "vg_name=${vg}" -o \
    pv_name,pv_uuid,vg_name,pv_size,pe_start,pv_pe_count,pv_pe_alloc_count,pv_mda_count \
    > "$d/pvs.txt"

  vgs --units b --nosuffix -o \
    vg_name,vg_uuid,vg_extent_size,vg_extent_count,vg_seqno,vg_systemid,pv_count,lv_count \
    "$vg" > "$d/vgs.txt"

  # Authoritative text metadata, exactly as stored on disk.
  vgcfgbackup -f "$d/vg-metadata.txt" "$vg" >/dev/null

  # Per-LV range hashes. Now reflects ext4 filesystem content (the data that
  # actually ships in the image), not the old raw stamp pattern.
  : > "$d/lv-hashes.txt"
  local lv dev sz off h
  for lv in $(lvs --noheadings -o lv_name "$vg" | tr -d ' '); do
    dev="/dev/$vg/$lv"
    [[ -e "$dev" ]] || continue
    sz=$(blockdev --getsize64 "$dev")
    echo "# $vg/$lv size=$sz" >> "$d/lv-hashes.txt"
    for off in 0 $((sz / 3)) $((sz / 2)) $((sz - 131072)); do
      (( off < 0 )) && continue
      h=$(dd if="$dev" bs=1M iflag=skip_bytes,count_bytes \
            skip="$off" count=131072 status=none | sha256sum | cut -d' ' -f1)
      echo "$lv offset=$off length=131072 sha256=$h" >> "$d/lv-hashes.txt"
    done
    echo "$lv whole sha256=$(sha256sum < "$dev" | cut -d' ' -f1)" >> "$d/lv-hashes.txt"
  done

  verify_oracles "$name"
  echo "    captured -> expected/$name/"
  return 0
}

mkimg() { # name size [losetup args...]  -> prints loop device
  local name="$1" size="$2"; shift 2
  local img="$IMG_DIR/$name.img"
  rm -f "$img"
  truncate -s "$size" "$img"
  losetup --find --show "$@" "$img"
}

deactivate_vg() { # vg [luksmap]
  vgchange -an "$1" >/dev/null
  if [[ -n "${2:-}" ]]; then
    cryptsetup close "$2"
  fi
  sync
  return 0                     # explicit: never leak a conditional's status
}

dump_luks() { # imgloop expected-dir
  local dev="$1" d="$2"
  cryptsetup luksDump "$dev" > "$d/luksDump.txt"
  # The volume key lets you unit-test the XTS layer WITHOUT a working KDF.
  # Safe here: throwaway container, passphrase published in this script.
  cryptsetup luksDump --dump-master-key --batch-mode --key-file "$PASSFILE" "$dev" \
    > "$d/MASTER-KEY-INSECURE-TEST-ONLY.txt" 2>/dev/null || \
    echo "(master-key dump unsupported by this cryptsetup)" \
      > "$d/MASTER-KEY-INSECURE-TEST-ONLY.txt"
  printf '%s\n' "$PASSPHRASE" > "$d/passphrase.txt"
  return 0
}

# ---------------------------------------------------------------------------
# Case runner. Each case runs in its own subshell with its own errexit, so one
# failing case reports and is skipped rather than killing the whole run.
# ---------------------------------------------------------------------------
run_case() {
  local name="$1" fn="$2"
  echo "[$name]"
  set +e
  ( set -Eeuo pipefail
    trap 'err_report "$LINENO" "$BASH_COMMAND"' ERR
    "$fn" )
  local st=$?
  set -e
  if (( st == 0 )); then
    PASSED+=("$name"); echo "    OK"
  else
    FAILED+=("$name"); echo "    SKIPPED (exit $st)" >&2
    release_devices >/dev/null 2>&1 || true
  fi
  return 0
}

# ===========================================================================
# 1. linear-1pv — baseline. One PV, one contiguous linear LV.
# ===========================================================================
case_linear_1pv() {
  local L; L=$(mkimg linear-1pv "$SMALL")
  pvcreate -y -ff "$L" >/dev/null
  vgcreate vgbase "$L" >/dev/null
  lvcreate -y -L 16M -n lvlin vgbase >/dev/null
  write_example_data /dev/vgbase/lvlin lvlin vgbase linear-1pv
  capture linear-1pv vgbase
  capture_fs_oracles linear-1pv vgbase lvlin
  deactivate_vg vgbase
}

# ===========================================================================
# 2. linear-1pv-multiseg — one LV of DISCONTIGUOUS segments, built by
#    sandwiching other LVs between successive extensions. lvextend only grows,
#    so every target size here must be strictly larger than the last.
# ===========================================================================
case_linear_multiseg() {
  local L; L=$(mkimg linear-1pv-multiseg "$SMALL")
  pvcreate -y -ff "$L" >/dev/null
  vgcreate vgseg "$L" >/dev/null
  lvcreate -y -L 8M  -n lvA vgseg >/dev/null
  lvcreate -y -L 8M  -n lvB vgseg >/dev/null   # takes the extents after lvA
  lvextend -y -L 24M /dev/vgseg/lvA >/dev/null  # lvA must jump past lvB
  lvcreate -y -L 8M  -n lvC vgseg >/dev/null
  lvextend -y -L 40M /dev/vgseg/lvA >/dev/null  # grow again: third segment
  local segs; segs=$(lvs --noheadings -o seg_count vgseg/lvA | tr -d ' ')
  echo "    lvA segment count: $segs"
  (( segs >= 3 )) || { echo "    !! expected >=3 segments, got $segs" >&2; return 1; }
  churn_metadata vgseg
  write_example_data /dev/vgseg/lvA lvA vgseg linear-1pv-multiseg
  write_example_data /dev/vgseg/lvB lvB vgseg linear-1pv-multiseg
  write_example_data /dev/vgseg/lvC lvC vgseg linear-1pv-multiseg
  capture linear-1pv-multiseg vgseg
  capture_fs_oracles linear-1pv-multiseg vgseg lvA
  capture_fs_oracles linear-1pv-multiseg vgseg lvB
  capture_fs_oracles linear-1pv-multiseg vgseg lvC
  deactivate_vg vgseg
}

# ===========================================================================
# 3. linear-2pv — LV spanning two PVs.
# ===========================================================================
case_linear_2pv() {
  local L1 L2
  L1=$(mkimg linear-2pv-a "$SMALL"); L2=$(mkimg linear-2pv-b "$SMALL")
  pvcreate -y -ff "$L1" "$L2" >/dev/null
  vgcreate vgspan "$L1" "$L2" >/dev/null
  # Sized to exceed one PV's usable space so it must span, without filling
  # the entire VG.
  lvcreate -y -L 160M -n lvspan vgspan >/dev/null
  write_example_data /dev/vgspan/lvspan lvspan vgspan linear-2pv
  capture linear-2pv vgspan
  capture_fs_oracles linear-2pv vgspan lvspan
  deactivate_vg vgspan
}

# ===========================================================================
# 4. striped-2pv — round-robin striping, 64 KiB stripes.
# ===========================================================================
case_striped_2pv() {
  local L1 L2
  L1=$(mkimg striped-2pv-a "$SMALL"); L2=$(mkimg striped-2pv-b "$SMALL")
  pvcreate -y -ff "$L1" "$L2" >/dev/null
  vgcreate vgstripe "$L1" "$L2" >/dev/null
  lvcreate -y -L 32M -i 2 -I 64 -n lvstr vgstripe >/dev/null
  write_example_data /dev/vgstripe/lvstr lvstr vgstripe striped-2pv
  capture striped-2pv vgstripe
  capture_fs_oracles striped-2pv vgstripe lvstr
  deactivate_vg vgstripe
}

# ===========================================================================
# 5. sector4k — 4Kn PV. Everything that hardcodes 512 breaks here.
# ===========================================================================
case_sector4k() {
  local L; L=$(mkimg sector4k "$SMALL" --sector-size 4096)
  pvcreate -y -ff "$L" >/dev/null
  vgcreate vg4k "$L" >/dev/null
  lvcreate -y -L 16M -n lv4k vg4k >/dev/null
  write_example_data /dev/vg4k/lv4k lv4k vg4k sector4k
  capture sector4k vg4k
  capture_fs_oracles sector4k vg4k lv4k
  deactivate_vg vg4k
}

# ===========================================================================
# 6. odd-geometry — 2 MiB extents, shifted pe_start, and TWO metadata areas.
#    Increased to 16M to leave room for ext4 + example files.
# ===========================================================================
case_odd_geometry() {
  local L; L=$(mkimg odd-geometry "$SMALL")
  pvcreate -y -ff --dataalignment 128k --metadatacopies 2 "$L" >/dev/null
  vgcreate -s 2M vgodd "$L" >/dev/null
  lvcreate -y -L 16M -n lvodd vgodd >/dev/null
  churn_metadata vgodd 60
  write_example_data /dev/vgodd/lvodd lvodd vgodd odd-geometry
  capture odd-geometry vgodd
  capture_fs_oracles odd-geometry vgodd lvodd
  deactivate_vg vgodd
}

# ===========================================================================
# 7. mixed-vg — supported and UNSUPPORTED types side by side. The point is to
#    prove we reject thin/raid/snapshot cleanly while still mapping the plain
#    linear LV beside them, rather than failing the whole VG.
#    Optional targets are best-effort: some live images lack dm-thin/dm-raid.
#    lvplain sized at 16M for ext4 + example files.
# ===========================================================================
case_mixed_vg() {
  local L1 L2
  L1=$(mkimg mixed-vg-a "$SMALL"); L2=$(mkimg mixed-vg-b "$SMALL")
  pvcreate -y -ff "$L1" "$L2" >/dev/null
  vgcreate vgmix "$L1" "$L2" >/dev/null
  lvcreate -y -L 16M -n lvplain vgmix >/dev/null
  write_example_data /dev/vgmix/lvplain lvplain vgmix mixed-vg

  if lvcreate -y -L 32M --thinpool tpool vgmix &>/dev/null; then
    lvcreate -y -V 8M --thin -n lvthin vgmix/tpool &>/dev/null \
      && echo "    + thin pool + thin LV" || echo "    ~ thin LV unavailable"
  else
    echo "    ~ thin pool unavailable (dm-thin-pool missing)"
  fi
  lvcreate -y -L 8M --type raid1 -m1 -n lvraid vgmix &>/dev/null \
    && echo "    + raid1" || echo "    ~ raid1 unavailable (dm-raid missing)"
  lvcreate -y -L 4M -s -n lvsnap /dev/vgmix/lvplain &>/dev/null \
    && echo "    + snapshot" || echo "    ~ snapshot unavailable"

  capture mixed-vg vgmix
  capture_fs_oracles mixed-vg vgmix lvplain
  deactivate_vg vgmix
}

# ===========================================================================
# 8. luks2-default — LUKS2 with stock cryptsetup defaults (argon2id).
# ===========================================================================
case_luks2_default() {
  ensure_passfile
  local L; L=$(mkimg luks2-default "$LUKS_SZ")
  cryptsetup luksFormat --type luks2 --batch-mode --key-file "$PASSFILE" "$L" >/dev/null
  cryptsetup open --key-file "$PASSFILE" "$L" wl_luks1
  pvcreate -y -ff /dev/mapper/wl_luks1 >/dev/null
  vgcreate vgluks /dev/mapper/wl_luks1 >/dev/null
  lvcreate -y -L "$LUKS_LV" -n lvcrypt vgluks >/dev/null
  write_example_data /dev/vgluks/lvcrypt lvcrypt vgluks luks2-default
  capture luks2-default vgluks wl_luks1
  capture_fs_oracles luks2-default vgluks lvcrypt
  dump_luks "$L" "$EXP_DIR/luks2-default"
  deactivate_vg vgluks wl_luks1
  verify_luks_roundtrip "$L" vgluks lvcrypt wl_luks1
}

# ===========================================================================
# 9. luks2-pbkdf2 — the BOOT-candidate configuration. argon2id at 1 GiB is
#    infeasible in a UEFI DXE driver and in a boot-start driver, so the boot
#    container must use PBKDF2. See docs/ROADMAP.md decision D4.
# ===========================================================================
case_luks2_pbkdf2() {
  ensure_passfile
  local L; L=$(mkimg luks2-pbkdf2 "$LUKS_SZ")
  cryptsetup luksFormat --type luks2 --batch-mode --key-file "$PASSFILE" \
    --pbkdf pbkdf2 --pbkdf-force-iterations 4000 \
    --cipher aes-xts-plain64 --key-size 512 --sector-size 512 "$L" >/dev/null
  cryptsetup open --key-file "$PASSFILE" "$L" wl_luks2
  pvcreate -y -ff /dev/mapper/wl_luks2 >/dev/null
  vgcreate vgboot /dev/mapper/wl_luks2 >/dev/null
  lvcreate -y -L "$LUKS_LV" -n lvboot vgboot >/dev/null
  write_example_data /dev/vgboot/lvboot lvboot vgboot luks2-pbkdf2
  capture luks2-pbkdf2 vgboot wl_luks2
  capture_fs_oracles luks2-pbkdf2 vgboot lvboot
  dump_luks "$L" "$EXP_DIR/luks2-pbkdf2"
  deactivate_vg vgboot wl_luks2
  verify_luks_roundtrip "$L" vgboot lvboot wl_luks2
}

# ===========================================================================
run_case linear-1pv           case_linear_1pv
run_case linear-1pv-multiseg  case_linear_multiseg
run_case linear-2pv           case_linear_2pv
run_case striped-2pv          case_striped_2pv
run_case sector4k             case_sector4k
run_case odd-geometry         case_odd_geometry
run_case mixed-vg             case_mixed_vg
run_case luks2-default        case_luks2_default
run_case luks2-pbkdf2         case_luks2_pbkdf2

# ===========================================================================
# Provenance, compression, summary
# ===========================================================================
{
  echo "generated:  $(date -Is)"
  echo "host:       $(uname -s) $(uname -r) $(uname -v) $(uname -m) $(uname -o)"
  echo "lvm:        $(lvm version 2>&1 | tr '\n' ' ')"
  echo "cryptsetup: $(cryptsetup --version 2>&1)"
  echo "passed:     ${PASSED[*]:-none}"
  echo "failed:     ${FAILED[*]:-none}"
} > "$EXP_DIR/PROVENANCE.txt"

release_devices   # detach loops; metadata stays on disk

# ---------------------------------------------------------------------------
# Post-teardown verification.
#
# Everything above ran while the volume groups were active, so it proves
# nothing about what actually ends up in the shipped image. This step re-reads
# the images in exactly the state they will be compressed in.
#
# It exists because an earlier version called vgremove during teardown: every
# oracle looked perfect, and every shipped image had its metadata area zeroed.
# Verify the artifact, not the process that produced it.
# ---------------------------------------------------------------------------
echo
echo "verifying metadata survived teardown"
vfail=0
for f in "$IMG_DIR"/*.img; do
  b=$(basename "$f" .img)
  case "$b" in luks2-*) continue ;; esac   # opaque without the passphrase

  loop=$(losetup --find --show --read-only "$f") || { echo "    !! $b: losetup failed" >&2; vfail=$((vfail+1)); continue; }
  found=$(pvs --noheadings -o vg_name "$loop" 2>/dev/null | tr -d ' ')
  losetup -d "$loop" &>/dev/null || true

  if [[ -z "$found" ]]; then
    echo "    !! $b: NO VOLUME GROUP — metadata area is empty" >&2
    vfail=$((vfail + 1))
  else
    printf '    %-28s vg=%s\n' "$b" "$found"
  fi
done
if (( vfail > 0 )); then
  echo >&2
  echo "ERROR: $vfail image(s) shipped without readable VG metadata." >&2
  echo "       The oracles are captured while the VGs are active, so this can" >&2
  echo "       only be caused by something destroying metadata after capture." >&2
  FAILED+=("post-verify($vfail)")
fi

# Compress out of the (large) work directory into the (small) output directory,
# one at a time, removing each source immediately so peak usage stays low.
# Level 12 rather than 19: the plain images are nearly all zeros and compress
# ~1000:1 at any level, while LUKS payloads are ciphertext and will not
# compress at all, so the extra CPU buys nothing.
echo
echo "compressing images -> $ZST_DIR"
shopt -s nullglob
zfail=0
for f in "$IMG_DIR"/*.img; do
  b=$(basename "$f")
  if zstd -12 -T0 -q -f "$f" -o "$ZST_DIR/$b.zst"; then
    rm -f "$f"
    printf '    %-28s %s\n' "$b.zst" "$(du -h "$ZST_DIR/$b.zst" | cut -f1)"
  else
    echo "    !! zstd failed for $b (out of space at $ZST_DIR?)" >&2
    zfail=$((zfail + 1))
  fi
done
(( zfail == 0 )) || FAILED+=("compression($zfail)")

if compgen -G "$ZST_DIR/*.img.zst" > /dev/null; then
  ( cd "$ZST_DIR" && sha256sum ./*.img.zst ) > "$EXP_DIR/image-checksums.txt"
fi
rmdir "$IMG_DIR" "$WORK" 2>/dev/null || true

# ---------------------------------------------------------------------------
# Generate corpus-manifest.json — a single machine-readable file describing
# every image, its LVs, and the example data inside. Agents and the C++ test
# harness read this to know what to expect.
# ---------------------------------------------------------------------------
echo
echo "generating corpus-manifest.json"
python3 - "$EXP_DIR" "$ZST_DIR" "$OUT" <<'PY'
import json, os, sys, re

exp_dir, img_dir, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]

descriptions = {
    "linear-1pv": "Baseline single-PV linear LV",
    "linear-1pv-multiseg": "One LV with discontiguous segments on a single PV",
    "linear-2pv": "LV spanning two PVs",
    "striped-2pv": "Round-robin striping, 64 KiB stripes, two PVs",
    "sector4k": "4Kn PV (4096-byte sectors)",
    "odd-geometry": "2 MiB extents, shifted pe_start, two metadata areas",
    "mixed-vg": "Supported and unsupported LV types side by side",
    "luks2-default": "LUKS2 with stock defaults (argon2id, aes-xts-plain64)",
    "luks2-pbkdf2": "LUKS2 boot-candidate config (PBKDF2)",
}

manifest = {"cases": {}}

for case_name in sorted(os.listdir(exp_dir)):
    case_path = os.path.join(exp_dir, case_name)
    if not os.path.isdir(case_path):
        continue

    case_info = {
        "description": descriptions.get(case_name, ""),
        "images": [],
        "lvs": {},
    }

    # Collect image files for this case
    if os.path.isdir(img_dir):
        for f in sorted(os.listdir(img_dir)):
            if not f.endswith('.img.zst'):
                continue
            base = f.replace('.img.zst', '')
            if base == case_name or re.fullmatch(re.escape(case_name) + r'-[a-z]', base):
                case_info["images"].append(f)

    # Extract VG info from metadata text
    vg_meta = os.path.join(case_path, "vg-metadata.txt")
    if os.path.exists(vg_meta):
        with open(vg_meta) as f:
            text = f.read()
        m = re.search(r'^(\w+)\s*\{', text, re.MULTILINE)
        if m:
            case_info["vg_name"] = m.group(1)
        m = re.search(r'extent_size\s*=\s*(\d+)', text)
        if m:
            case_info["extent_size_sectors"] = int(m.group(1))
        case_info["pv_count"] = len(re.findall(r'pv\d+\s*\{', text))

    # Collect filesystem oracle info per LV
    for entry in sorted(os.listdir(case_path)):
        if not entry.startswith('fs-'):
            continue
        lv_name = entry[3:]
        lv_info = {"fs_type": "ext4", "files": []}

        checksums_path = os.path.join(case_path, entry, "checksums.txt")
        if os.path.exists(checksums_path):
            with open(checksums_path) as f:
                for line in f:
                    parts = line.strip().split('  ', 1)
                    if len(parts) == 2:
                        lv_info["files"].append({
                            "path": parts[1],
                            "sha256": parts[0]
                        })

        case_info["lvs"][lv_name] = lv_info

    # LUKS flag
    if os.path.exists(os.path.join(case_path, "luksDump.txt")):
        case_info["has_luks"] = True

    manifest["cases"][case_name] = case_info

out_path = os.path.join(out_dir, "corpus-manifest.json")
with open(out_path, 'w') as f:
    json.dump(manifest, f, indent=2)
    f.write('\n')
print(f"    -> {out_path}")
PY

echo
echo "======================================================================"
echo "passed (${#PASSED[@]}): ${PASSED[*]:-none}"
if (( ${#FAILED[@]} )); then
  echo "FAILED (${#FAILED[@]}): ${FAILED[*]}" >&2
fi
echo "======================================================================"
du -sh "$ZST_DIR" "$EXP_DIR" 2>/dev/null || true
echo "corpus in: $OUT"
echo "expected: 9 dirs under expected/, 12 .img.zst under images/"
printf 'actual:   %s dirs, %s images\n' \
  "$(find "$EXP_DIR" -mindepth 1 -maxdepth 1 -type d | wc -l)" \
  "$(find "$ZST_DIR" -name '*.img.zst' | wc -l)"

(( ${#FAILED[@]} == 0 )) || exit 1
