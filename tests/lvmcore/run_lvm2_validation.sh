#!/usr/bin/env bash
# run_lvm2_validation.sh — LVM2 vgcfgrestore acceptance sweep for winlvm lvmcore.
#
# For every VG-bearing corpus case:
#   1. Dump the VG from the corpus image via build/lvmcore_dump_vgcfg
#      (vgcfgbackup file format: contents/version/description/creation_host/
#       creation_time header + serialized VG text block).
#   2. In ONE privileged docker container (winlvm-lvm2:latest, lvm2 preinstalled):
#      copy image(s) to scratch, losetup each (sector-size 4096 for sector4k),
#      vgcfgrestore --test --force (expect exit 0; --force is required by LVM2
#      when the VG contains thin volumes, no-op otherwise),
#      real vgcfgrestore to the scratch + vgck (expect exit 0),
#      detach loops (trap-guarded), copy first restored scratch out for
#      full-circle.
#   3. Full circle: re-dump the restored scratch image with our own tool and
#      semantic-compare against the original dump. Ignored (expected variance):
#      - header lines (description/creation_time/creation_host)
#      - seqno (LVM2 bumps it on every restore write)
#      - PV "device = ..." paths (vgcfgrestore rewrites them to the actual
#        loop node; LVM2 matches PVs by UUID, paths are informational)
#
# 2-PV VGs: the full VG metadata lives in every PV's metadata area, so the
# dump comes from the -a image alone, but vgcfgrestore/vgck need BOTH PVs
# visible: -a and -b images are both attached (comma-joined --devices).
#
# Prints PASS:/FAIL: per case; writes everything to lvm2-validation.log
# next to this script. Original corpus images are never written to.
#
# Usage: tests/lvmcore/run_lvm2_validation.sh [logfile]

set -u -o pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DUMP_TOOL="$REPO_ROOT/build/lvmcore_dump_vgcfg"
IMAGES_DIR="$REPO_ROOT/build/corpus-images"
WORK_DIR="$(mktemp -d /tmp/lvm2val.XXXXXX)"
LOG_FILE="${1:-$REPO_ROOT/tests/lvmcore/lvm2-validation.log}"
CONTAINER_IMAGE="winlvm-lvm2:latest"

# case|images (comma-sep)|vgname|logical-sector-size
CASES=(
  "linear-1pv|linear-1pv.img|vgbase|512"
  "linear-1pv-multiseg|linear-1pv-multiseg.img|vgseg|512"
  "linear-2pv-a|linear-2pv-a.img,linear-2pv-b.img|vgspan|512"
  "striped-2pv-a|striped-2pv-a.img,striped-2pv-b.img|vgstripe|512"
  "sector4k|sector4k.img|vg4k|4096"
  "odd-geometry|odd-geometry.img|vgodd|512"
  "mixed-vg-a|mixed-vg-a.img,mixed-vg-b.img|vgmix|512"
)

cleanup() {
  rm -rf "$WORK_DIR"
}
trap cleanup EXIT

log() { printf '%s\n' "$*" | tee -a "$LOG_FILE"; }

pass=0; fail=0

: > "$LOG_FILE"
log "# winlvm lvmcore → LVM2 vgcfgrestore validation"
log "# date: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
log "# container: $CONTAINER_IMAGE (lvm2 toolchain in docker)"
log "# dump tool: $DUMP_TOOL"
log "# cases: ${#CASES[@]}"
log ""

command -v docker >/dev/null 2>&1 || { log "FAIL: docker not available"; exit 1; }
docker image inspect "$CONTAINER_IMAGE" >/dev/null 2>&1 || { log "FAIL: container image $CONTAINER_IMAGE missing"; exit 1; }
test -x "$DUMP_TOOL" || { log "FAIL: dump tool not built ($DUMP_TOOL)"; exit 1; }

for entry in "${CASES[@]}"; do
  IFS='|' read -r case imgs vgn ssz <<< "$entry"
  first_img="${imgs%%,*}"
  img="$IMAGES_DIR/$first_img"
  dump="$WORK_DIR/$case.dump"
  missing=0
  IFS=',' read -ra img_list <<< "$imgs"
  for im in "${img_list[@]}"; do
    [ -f "$IMAGES_DIR/$im" ] || { log "FAIL: $case (image missing: $im)"; fail=$((fail+1)); missing=1; break; }
  done
  [ "$missing" = 0 ] || continue

  log "================================================================"
  log "case: $case  images: $imgs  vg: $vgn  sector-size: $ssz"
  log "----------------------------------------------------------------"

  # 1. host dump (VG metadata is fully present in each PV's metadata area)
  if ! "$DUMP_TOOL" "$img" "$dump" >>"$LOG_FILE" 2>&1; then
    log "FAIL: $case (lvmcore_dump_vgcfg failed)"
    fail=$((fail+1)); continue
  fi
  log "dump OK: $case.dump ($(wc -c <"$dump") bytes)"

  # 2. container acceptance: --test, real restore, vgck, restore-scratch copyback.
  #    Args passed as environment variables (bash -c positional args after the
  #    script string land in $0..$N, not $1..$N).
  out=$(docker run --rm --privileged \
    -v /dev:/dev \
    -v "$IMAGES_DIR:/images:ro" \
    -v "$WORK_DIR:/dumps" \
    -e CASE="$case" -e IMGS="$imgs" -e VGN="$vgn" -e SSZ="$ssz" \
    "$CONTAINER_IMAGE" bash -c '
      set -u
      dump=/dumps/$CASE.dump
      LOOPS=""
      cleanup() {
        for lo in $LOOPS; do losetup -d "$lo" 2>/dev/null; done
      }
      trap cleanup EXIT
      IFS="," read -ra imgs <<< "$IMGS"
      i=0
      for im in "${imgs[@]}"; do
        cp "/images/$im" "/tmp/scratch$i.img"
        if [ "$SSZ" = 4096 ]; then LO=$(losetup --sector-size 4096 -f --show "/tmp/scratch$i.img")
        else LO=$(losetup -f --show "/tmp/scratch$i.img"); fi
        [ -n "$LO" ] || { echo "losetup failed for $im"; exit 9; }
        LOOPS="$LOOPS $LO"
        i=$((i+1))
      done
      echo "loops:$LOOPS"
      DEVS="$(echo $LOOPS | tr " " "," | sed "s/^,//")"
      vgcfgrestore --test --force --devices "$DEVS" -f "$dump" "$VGN"
      echo "TEST_RC=$?"
      vgcfgrestore --force --devices "$DEVS" -f "$dump" "$VGN"
      echo "RESTORE_RC=$?"
      vgck --devices "$DEVS" "$VGN"
      echo "VGCK_RC=$?"
      cp /tmp/scratch0.img /dumps/$CASE.restored.img
      echo "COPYBACK_RC=$?"
    ' 2>&1) || out="$out
DOCKER_RUN_FAILED"
  printf '%s\n' "$out" | tee -a "$LOG_FILE" >/dev/null
  test_rc=$(printf '%s\n' "$out" | sed -n 's/^TEST_RC=//p' | tail -1)
  restore_rc=$(printf '%s\n' "$out" | sed -n 's/^RESTORE_RC=//p' | tail -1)
  vgck_rc=$(printf '%s\n' "$out" | sed -n 's/^VGCK_RC=//p' | tail -1)
  copyback_rc=$(printf '%s\n' "$out" | sed -n 's/^COPYBACK_RC=//p' | tail -1)

  case_ok=1
  [ "$test_rc" = "0" ] || { log "  vgcfgrestore --test RC='$test_rc' (expected 0)"; case_ok=0; }
  [ "$restore_rc" = "0" ] || { log "  real vgcfgrestore RC='$restore_rc' (expected 0)"; case_ok=0; }
  [ "$vgck_rc" = "0" ] || { log "  vgck RC='$vgck_rc' (expected 0)"; case_ok=0; }

  # 3. full circle: re-dump restored scratch, semantic compare.
  #    Ignored: header lines, seqno (bumped by restore), device paths (rewritten
  #    by vgcfgrestore to the loop node; PV identity is the UUID).
  if [ "$case_ok" = "1" ] && [ "$copyback_rc" = "0" ]; then
    redump="$WORK_DIR/$case.redump"
    if "$DUMP_TOOL" "$WORK_DIR/$case.restored.img" "$redump" >>"$LOG_FILE" 2>&1; then
      filt() {
        grep -v -E '^(description =|creation_host =|creation_time =|contents = |version = |#)' "$1" \
          | sed -e '/^[[:space:]]*seqno = /d' -e 's|device = "[^"]*"|device = "<DEV>"|'
      }
      if diff <(filt "$dump") <(filt "$redump") >"$WORK_DIR/$case.diff" 2>&1; then
        log "  full-circle: re-parse of restored image semantically identical to original dump"
      else
        log "  full-circle DIFF (unexpected):"
        sed 's/^/    /' "$WORK_DIR/$case.diff" | tee -a "$LOG_FILE" >/dev/null
        case_ok=0
      fi
    else
      log "  full-circle: re-dump of restored image FAILED"
      case_ok=0
    fi
  elif [ "$case_ok" = "1" ]; then
    log "  full-circle: skipped (scratch copyback RC='$copyback_rc')"
  fi

  if [ "$case_ok" = "1" ]; then
    log "PASS: $case (test RC=$test_rc, restore RC=$restore_rc, vgck RC=$vgck_rc)"
    pass=$((pass+1))
  else
    log "FAIL: $case (test RC='$test_rc', restore RC='$restore_rc', vgck RC='$vgck_rc')"
    fail=$((fail+1))
  fi
done

log ""
log "================================================================"
log "SUMMARY: $pass passed, $fail failed, $((pass+fail)) total"
if [ "$fail" = 0 ]; then
  log "RESULT: PASS — LVM2 tooling accepts winlvm lvmcore-serialized metadata on all cases"
  exit 0
else
  log "RESULT: FAIL"
  exit 1
fi
