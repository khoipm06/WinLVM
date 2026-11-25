#!/usr/bin/env python3
"""Deterministic out-of-tree LVM2 and LUKS2 test fixture generator.

Generates sparse raw disk images covering linear, multi-segment, multi-PV,
striped, 4Kn, dual-MDA wrapped-ring, thin-pool/thin/thin-snapshot,
classic snapshot/snapshot-origin, raid1, mirror (core and disk log),
mixed supported+unsupported VGs, and opaque LUKS2 containers.

Uses only the Python 3 standard library.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

SECTOR_SIZE = 512
MIB = 1024 * 1024
CREATION_TIME = 1700000000
CREATION_HOST = "lvmtest"

# ---------------------------------------------------------------------------
# Checksums (LVM2 CRC-32 and DM-persistent-data Castagnoli CRC-32C)
# ---------------------------------------------------------------------------

def _make_crc_table(poly: int) -> Tuple[int, ...]:
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = ((c >> 1) ^ poly) if (c & 1) else (c >> 1)
        table.append(c)
    return tuple(table)


_LVM_CRC32_TABLE = _make_crc_table(0xEDB88320)
_CRC32C_TABLE = _make_crc_table(0x82F63B78)

LVM_INITIAL_CRC = 0xF597A6CF
THIN_SUPERBLOCK_CSUM_XOR = 160774
THIN_BTREE_CSUM_XOR = 121107
THIN_BITMAP_CSUM_XOR = 240779
THIN_INDEX_CSUM_XOR = 160478


def lvm_crc32(data: bytes, init: int = LVM_INITIAL_CRC) -> int:
    c = init & 0xFFFFFFFF
    tbl = _LVM_CRC32_TABLE
    for b in data:
        c = tbl[(c ^ b) & 0xFF] ^ (c >> 8)
    return c & 0xFFFFFFFF


def dm_crc32c(data: bytes, xor_const: int) -> int:
    c = 0xFFFFFFFF
    tbl = _CRC32C_TABLE
    for b in data:
        c = tbl[(c ^ b) & 0xFF] ^ (c >> 8)
    return (c ^ xor_const) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Deterministic UUIDs and payload streams
# ---------------------------------------------------------------------------

_LVM_ALPHABET = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"


def det_uuid_raw(seed: str) -> str:
    digest = hashlib.sha256(("winlvm-uuid:" + seed).encode("ascii")).digest()
    out = []
    val = int.from_bytes(digest, "big")
    for _ in range(32):
        val, rem = divmod(val, len(_LVM_ALPHABET))
        out.append(_LVM_ALPHABET[rem])
    return "".join(out)


def fmt_uuid(raw: str) -> str:
    assert len(raw) == 32
    return (
        f"{raw[0:6]}-{raw[6:10]}-{raw[10:14]}-{raw[14:18]}"
        f"-{raw[18:22]}-{raw[22:26]}-{raw[26:32]}"
    )


def det_payload(seed: str, size: int) -> bytes:
    """Fast, deterministic, position-unique byte stream via SHAKE-128."""
    return hashlib.shake_128(("winlvm-payload:" + seed).encode("ascii")).digest(size)


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------------------
# Sparse image file writer
# ---------------------------------------------------------------------------

class DiskImage:
    def __init__(self, path: Path, size_bytes: int) -> None:
        self.path = path
        self.size_bytes = size_bytes
        self._writes: List[Tuple[int, bytes]] = []

    def write(self, offset: int, data: bytes) -> None:
        if offset < 0 or offset + len(data) > self.size_bytes:
            raise ValueError(
                f"{self.path.name}: write [{offset}, {offset + len(data)}) "
                f"out of bounds ({self.size_bytes})"
            )
        self._writes.append((offset, data))

    def flush(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with open(self.path, "wb") as f:
            f.truncate(self.size_bytes)
            for off, data in self._writes:
                f.seek(off)
                f.write(data)


# ---------------------------------------------------------------------------
# LVM2 PV label and MDA writer
# ---------------------------------------------------------------------------

def write_pv_label_and_mdas(
    img: DiskImage,
    pv_uuid_raw: str,
    meta_text: str,
    mdas: Sequence[Tuple[int, int]] = ((4096, 1044480),),
    pe_start_bytes: int = MIB,
    wrap_mda: bool = False,
) -> None:
    """Write LVM2 LABELONE + pv_header + all MDA headers & ring text."""
    raw_uuid_bytes = pv_uuid_raw.encode("ascii")
    assert len(raw_uuid_bytes) == 32

    # Build label_header + pv_header inside 512B sector 1 (byte offset 512).
    sec = bytearray(512)
    sec[0:8] = b"LABELONE"
    struct.pack_into("<Q", sec, 8, 1)  # sector_xl = 1
    struct.pack_into("<I", sec, 20, 32)  # data_offset = 32
    sec[24:32] = b"LVM2 001"

    pvh = 32
    sec[pvh : pvh + 32] = raw_uuid_bytes
    struct.pack_into("<Q", sec, pvh + 32, img.size_bytes)
    cur = pvh + 40
    # Data area list: (pe_start_bytes, 0 = to end), terminator (0, 0)
    struct.pack_into("<QQ", sec, cur, pe_start_bytes, 0)
    cur += 16
    struct.pack_into("<QQ", sec, cur, 0, 0)
    cur += 16
    # Metadata area list + terminator (0, 0)
    for m_off, m_sz in mdas:
        struct.pack_into("<QQ", sec, cur, m_off, m_sz)
        cur += 16
    struct.pack_into("<QQ", sec, cur, 0, 0)
    cur += 16
    # pv_header_extension v2 (PV_EXT_USED=1) + empty bootloader_areas terminator
    struct.pack_into("<IIQQ", sec, cur, 2, 1, 0, 0)

    struct.pack_into("<I", sec, 16, lvm_crc32(bytes(sec[20:512])))
    img.write(512, bytes(sec))

    # Write MDA header + metadata text (NUL-terminated) in every MDA.
    payload = meta_text.encode("ascii") + b"\x00"
    payload_crc = lvm_crc32(payload)
    for m_off, m_sz in mdas:
        if len(payload) + 512 > m_sz:
            raise ValueError("metadata text exceeds MDA size")
        if wrap_mda:
            # Place start 512 bytes before end of MDA so metadata genuinely wraps.
            rel_off = m_sz - 512
            first = m_sz - rel_off
            img.write(m_off + rel_off, payload[:first])
            img.write(m_off + 512, payload[first:])
        else:
            rel_off = 512
            img.write(m_off + rel_off, payload)

        hdr = bytearray(512)
        hdr[4:20] = b" LVM2 x[5A%r0N*>"
        struct.pack_into("<I", hdr, 20, 1)  # version = 1
        struct.pack_into("<QQ", hdr, 24, m_off, m_sz)
        struct.pack_into("<QQII", hdr, 40, rel_off, len(payload), payload_crc, 0)
        struct.pack_into("<I", hdr, 0, lvm_crc32(bytes(hdr[4:512])))
        img.write(m_off, bytes(hdr))


# ---------------------------------------------------------------------------
# LVM2 Text Metadata Formatter
# ---------------------------------------------------------------------------

def format_vg_metadata(
    vg_name: str,
    vg_uuid: str,
    seqno: int,
    extent_size_sectors: int,
    pvs: Sequence[Dict],
    lvs: Sequence[Dict],
    vg_tags: Sequence[str] = (),
) -> str:
    lines: List[str] = [
        f"{vg_name} {{",
        f'\tid = "{fmt_uuid(vg_uuid)}"',
        f"\tseqno = {seqno}",
        '\tformat = "lvm2"',
        '\tstatus = ["RESIZEABLE", "READ", "WRITE"]',
        "\tflags = []",
    ]
    if vg_tags:
        tags_s = ", ".join(f'"{t}"' for t in vg_tags)
        lines.append(f"\ttags = [{tags_s}]")
    lines += [
        f"\textent_size = {extent_size_sectors}",
        "\tmax_lv = 0",
        "\tmax_pv = 0",
        "\tmetadata_copies = 0",
        "",
        "\tphysical_volumes {",
    ]
    for pv in pvs:
        lines += [
            "",
            f"\t\t{pv['key']} {{",
            f'\t\t\tid = "{fmt_uuid(pv["uuid"])}"',
            f'\t\t\tdevice = "{pv["device"]}"',
            "",
            '\t\t\tstatus = ["ALLOCATABLE"]',
            "\t\t\tflags = []",
        ]
        if pv.get("tags"):
            tags_s = ", ".join(f'"{t}"' for t in pv["tags"])
            lines.append(f"\t\t\ttags = [{tags_s}]")
        lines += [
            f"\t\t\tdev_size = {pv['dev_size']}",
            f"\t\t\tpe_start = {pv['pe_start']}",
            f"\t\t\tpe_count = {pv['pe_count']}",
            "\t\t}",
        ]
    lines += [
        "\t}",
        "",
        "\tlogical_volumes {",
    ]
    for lv in lvs:
        status = lv.get("status", ["READ", "WRITE", "VISIBLE"])
        status_s = ", ".join(f'"{s}"' for s in status)
        lines += [
            "",
            f"\t\t{lv['name']} {{",
            f'\t\t\tid = "{fmt_uuid(lv["uuid"])}"',
            f"\t\t\tstatus = [{status_s}]",
            "\t\t\tflags = []",
        ]
        if lv.get("tags"):
            tags_s = ", ".join(f'"{t}"' for t in lv["tags"])
            lines.append(f"\t\t\ttags = [{tags_s}]")
        lines += [
            f"\t\t\tcreation_time = {CREATION_TIME}",
            f'\t\t\tcreation_host = "{CREATION_HOST}"',
            f"\t\t\tsegment_count = {len(lv['segments'])}",
            "",
        ]
        for idx, seg in enumerate(lv["segments"], start=1):
            lines.append(f"\t\t\tsegment{idx} {{")
            lines.append(f"\t\t\t\tstart_extent = {seg['start_extent']}")
            lines.append(f"\t\t\t\textent_count = {seg['extent_count']}")
            lines.append("")
            stype = seg["type"]
            lines.append(f'\t\t\t\ttype = "{stype}"')
            if stype == "striped":
                sc = seg["stripe_count"]
                lines.append(f"\t\t\t\tstripe_count = {sc}")
                if sc > 1:
                    lines.append(f"\t\t\t\tstripe_size = {seg['stripe_size']}")
                lines.append("")
                lines.append("\t\t\t\tstripes = [")
                pairs = seg["stripes"]
                for pi, (pv_k, pe_s) in enumerate(pairs):
                    comma = "," if pi + 1 < len(pairs) else ""
                    lines.append(f'\t\t\t\t\t"{pv_k}", {pe_s}{comma}')
                lines.append("\t\t\t\t]")
            elif stype == "thin-pool":
                lines.append(f'\t\t\t\tmetadata = "{seg["metadata"]}"')
                lines.append(f'\t\t\t\tpool = "{seg["pool"]}"')
                lines.append(f"\t\t\t\ttransaction_id = {seg['transaction_id']}")
                lines.append(f"\t\t\t\tchunk_size = {seg['chunk_size']}")
                lines.append(f'\t\t\t\tdiscards = "{seg.get("discards", "passdown")}"')
                lines.append(f"\t\t\t\tzero_new_blocks = {seg.get('zero_new_blocks', 1)}")
            elif stype == "thin":
                lines.append(f'\t\t\t\tthin_pool = "{seg["thin_pool"]}"')
                lines.append(f"\t\t\t\ttransaction_id = {seg.get('transaction_id', 0)}")
                lines.append(f"\t\t\t\tdevice_id = {seg['device_id']}")
                if seg.get("origin"):
                    lines.append(f'\t\t\t\torigin = "{seg["origin"]}"')
            elif stype == "snapshot":
                lines.append(f"\t\t\t\tchunk_size = {seg['chunk_size']}")
                lines.append(f'\t\t\t\torigin = "{seg["origin"]}"')
                lines.append(f'\t\t\t\tcow_store = "{seg["cow_store"]}"')
            elif stype in ("raid1", "raid5_ls"):
                lines.append(f"\t\t\t\tdevice_count = {seg['device_count']}")
                lines.append(f"\t\t\t\tregion_size = {seg['region_size']}")
                if "stripe_size" in seg:
                    lines.append(f"\t\t\t\tstripe_size = {seg['stripe_size']}")
                lines.append("")
                lines.append("\t\t\t\traids = [")
                legs = seg["raids"]
                for li, (mlv, ilv) in enumerate(legs):
                    comma = "," if li + 1 < len(legs) else ""
                    lines.append(f'\t\t\t\t\t"{mlv}", "{ilv}"{comma}')
                lines.append("\t\t\t\t]")
            elif stype == "mirror":
                lines.append(f"\t\t\t\tmirror_count = {seg['mirror_count']}")
                if seg.get("mirror_log"):
                    lines.append(f'\t\t\t\tmirror_log = "{seg["mirror_log"]}"')
                lines.append(f"\t\t\t\tregion_size = {seg['region_size']}")
                lines.append("")
                lines.append("\t\t\t\tmirrors = [")
                legs = seg["mirrors"]
                for li, (ilv, ext0) in enumerate(legs):
                    comma = "," if li + 1 < len(legs) else ""
                    lines.append(f'\t\t\t\t\t"{ilv}", {ext0}{comma}')
                lines.append("\t\t\t\t]")
            elif stype == "cache":
                lines.append(f'\t\t\t\tcache_pool = "{seg["cache_pool"]}"')
                lines.append(f'\t\t\t\torigin = "{seg["origin"]}"')
            elif stype == "cache-pool":
                lines.append(f'\t\t\t\tdata = "{seg["data"]}"')
                lines.append(f'\t\t\t\tmetadata = "{seg["metadata"]}"')
                lines.append(f"\t\t\t\tchunk_size = {seg['chunk_size']}")
                lines.append('\t\t\t\tcache_mode = "writethrough"')
                lines.append('\t\t\t\tpolicy = "smq"')
            else:
                raise ValueError(f"unknown segment type {stype}")
            lines.append("\t\t\t}")
        lines.append("\t\t}")
    lines += [
        "\t}",
        "",
        "}",
        "# Generated by WinLVM fixture generator",
        "",
        'contents = "Text Format Volume Group"',
        "version = 1",
        "",
        'description = ""',
        "",
        f'creation_host = "{CREATION_HOST}"',
        f"creation_time = {CREATION_TIME}",
        "",
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# On-disk complex engine builders (dm-thin v2, dm-snapshot, dm-raid, dm-log)
# ---------------------------------------------------------------------------

def _sm_bitmap_set(buf: bytearray, idx: int, rc: int) -> None:
    """Set 2-bit bit-swapped refcount in a dm-space-map bitmap block."""
    if rc == 0:
        code = 0
    elif rc == 1:
        code = 2  # 10b
    elif rc == 2:
        code = 1  # 01b
    else:
        code = 3  # 11b (overflow to ref_count_root)
    byte_off = 16 + (idx // 4)
    shift = (idx % 4) * 2
    buf[byte_off] = (buf[byte_off] & ~(3 << shift)) | (code << shift)


def _btree_max_entries(vsz: int) -> int:
    return ((4064 // (8 + vsz)) // 3) * 3


def _build_btree_leaf(
    blocknr: int,
    value_size: int,
    items: Sequence[Tuple[int, bytes]],
) -> bytes:
    max_entries = _btree_max_entries(value_size)
    assert len(items) <= max_entries
    blk = bytearray(4096)
    struct.pack_into("<IIQIIII", blk, 0, 0, 2, blocknr, len(items), max_entries, value_size, 0)
    sorted_items = sorted(items, key=lambda kv: kv[0])
    val_base = 32 + 8 * max_entries
    for i, (k, v) in enumerate(sorted_items):
        assert len(v) == value_size
        struct.pack_into("<Q", blk, 32 + 8 * i, k)
        blk[val_base + i * value_size : val_base + (i + 1) * value_size] = v
    struct.pack_into("<I", blk, 0, dm_crc32c(bytes(blk[4:]), THIN_BTREE_CSUM_XOR))
    return bytes(blk)


def build_dm_thin_metadata(
    meta_nr_blocks: int,
    data_nr_blocks: int,
    data_block_size_sectors: int,
    time_val: int,
    trans_id: int,
    devices: Sequence[Dict],
) -> bytes:
    """Build a thin_check-clean v2 dm-thin metadata image (4 KiB blocks).

    Each entry in `devices`:
      {
        'dev_id': int,
        'trans_id': int,
        'ctime': int,
        'stime': int,
        'mappings': List[Tuple[int, int, int]],  # (vblk, pblk, blk_time)
        'share_root_of': Optional[int],          # optional dev_id sharing bottom leaf
      }
    """
    blocks: Dict[int, bytes] = {}
    meta_rc: Dict[int, int] = {}

    def alloc_meta(blk_no: int, rc: int = 1) -> None:
        meta_rc[blk_no] = meta_rc.get(blk_no, 0) + rc

    # Reserve fixed metadata blocks:
    # 0=sb, 1=meta_idx, 2=meta_bmp0, 3=meta_rc_root,
    # 4=data_sm_root, 5=data_bmp0, 6=data_rc_root,
    # 7=data_mapping_root, 8=device_details_root
    for b in range(9):
        alloc_meta(b, 1)

    next_blk = 9
    dev_root_blk: Dict[int, int] = {}
    data_rc: Dict[int, int] = {}

    # Build bottom mapping leaves (one per unique leaf; shared leaves bump meta_rc).
    for dev in devices:
        did = dev["dev_id"]
        shared_from = dev.get("share_root_of")
        if shared_from is not None:
            rblk = dev_root_blk[shared_from]
            dev_root_blk[did] = rblk
            alloc_meta(rblk, 1)  # bump meta_rc from 1 -> 2
        else:
            rblk = next_blk
            next_blk += 1
            alloc_meta(rblk, 1)
            dev_root_blk[did] = rblk
            leaf_items = []
            for vblk, pblk, btime in dev["mappings"]:
                val = (pblk << 24) | (btime & 0xFFFFFF)
                leaf_items.append((vblk, struct.pack("<Q", val)))
                data_rc[pblk] = data_rc.get(pblk, 0) + 1
            blocks[rblk] = _build_btree_leaf(rblk, 8, leaf_items)

    # Top-level data_mapping_root (block 7)
    top_items = [(d["dev_id"], struct.pack("<Q", dev_root_blk[d["dev_id"]])) for d in devices]
    blocks[7] = _build_btree_leaf(7, 8, top_items)

    # device_details_root (block 8)
    detail_items = []
    for dev in devices:
        mapped_cnt = len(dev["mappings"])
        detail_items.append(
            (
                dev["dev_id"],
                struct.pack("<QQII", mapped_cnt, dev["trans_id"], dev["ctime"], dev["stime"]),
            )
        )
    blocks[8] = _build_btree_leaf(8, 24, detail_items)

    # data_sm bitmap 0 (block 5) + data_sm ref_count_root (block 6) + data_sm bitmap_root (block 4)
    data_bmp = bytearray(4096)
    struct.pack_into("<IIQ", data_bmp, 0, 0, 0, 5)
    data_rc_overflow: List[Tuple[int, bytes]] = []
    for pblk, rc in sorted(data_rc.items()):
        _sm_bitmap_set(data_bmp, pblk, rc)
        if rc >= 3:
            data_rc_overflow.append((pblk, struct.pack("<I", rc)))
    struct.pack_into("<I", data_bmp, 0, dm_crc32c(bytes(data_bmp[4:]), THIN_BITMAP_CSUM_XOR))
    blocks[5] = bytes(data_bmp)
    blocks[6] = _build_btree_leaf(6, 4, data_rc_overflow)
    data_alloc = len(data_rc)
    data_idx_entry = struct.pack("<QII", 5, 16320 - data_alloc, 0)
    blocks[4] = _build_btree_leaf(4, 16, [(0, data_idx_entry)])

    # meta_sm bitmap 0 (block 2) + meta_sm ref_count_root (block 3) + meta_sm index (block 1)
    meta_bmp = bytearray(4096)
    struct.pack_into("<IIQ", meta_bmp, 0, 0, 0, 2)
    meta_rc_overflow: List[Tuple[int, bytes]] = []
    for mblk, rc in sorted(meta_rc.items()):
        _sm_bitmap_set(meta_bmp, mblk, rc)
        if rc >= 3:
            meta_rc_overflow.append((mblk, struct.pack("<I", rc)))
    struct.pack_into("<I", meta_bmp, 0, dm_crc32c(bytes(meta_bmp[4:]), THIN_BITMAP_CSUM_XOR))
    blocks[2] = bytes(meta_bmp)
    blocks[3] = _build_btree_leaf(3, 4, meta_rc_overflow)

    meta_alloc = len(meta_rc)
    meta_idx = bytearray(4096)
    struct.pack_into("<IIQ", meta_idx, 0, 0, 0, 1)
    struct.pack_into("<QII", meta_idx, 16, 2, 16320 - meta_alloc, 0)
    struct.pack_into("<I", meta_idx, 0, dm_crc32c(bytes(meta_idx[4:]), THIN_INDEX_CSUM_XOR))
    blocks[1] = bytes(meta_idx)

    # Superblock (block 0)
    sb = bytearray(4096)
    struct.pack_into("<IIQ", sb, 0, 0, 0, 0)  # csum, flags=0, blocknr=0
    struct.pack_into("<QIIQQ", sb, 32, 27022010, 2, time_val, trans_id, 0)
    # data_space_map_root at offset 64
    struct.pack_into("<QQQQ", sb, 64, data_nr_blocks, data_alloc, 4, 6)
    # metadata_space_map_root at offset 192
    struct.pack_into("<QQQQ", sb, 192, meta_nr_blocks, meta_alloc, 1, 3)
    # roots & geometry at offset 320
    struct.pack_into(
        "<QQIIQ",
        sb,
        320,
        7,
        8,
        data_block_size_sectors,
        8,
        meta_nr_blocks,
    )
    struct.pack_into("<I", sb, 0, dm_crc32c(bytes(sb[4:]), THIN_SUPERBLOCK_CSUM_XOR))
    blocks[0] = bytes(sb)

    out = bytearray(next_blk * 4096)
    for bno, data in blocks.items():
        out[bno * 4096 : (bno + 1) * 4096] = data
    return bytes(out)


def build_dm_snapshot_cow(
    cow_size_bytes: int,
    chunk_size_sectors: int,
    exceptions: Sequence[Tuple[int, bytes]],
) -> bytes:
    """Build a Linux dm-snapshot persistent COW store image."""
    chunk_bytes = chunk_size_sectors * SECTOR_SIZE
    exc_per_area = chunk_bytes // 16
    buf = bytearray(cow_size_bytes)
    # Chunk 0: disk_header (magic="SnAp"=0x70416e53, valid=1, version=1, chunk_size)
    struct.pack_into("<IIII", buf, 0, 0x70416E53, 1, 1, chunk_size_sectors)

    for idx, (old_chunk, payload) in enumerate(exceptions):
        assert len(payload) == chunk_bytes
        area = idx // exc_per_area
        slot = idx % exc_per_area
        area_hdr_chunk = 1 + area * (exc_per_area + 1)
        new_chunk = area_hdr_chunk + 1 + slot
        struct.pack_into("<QQ", buf, area_hdr_chunk * chunk_bytes + slot * 16, old_chunk, new_chunk)
        buf[new_chunk * chunk_bytes : (new_chunk + 1) * chunk_bytes] = payload
    return bytes(buf)


def build_dm_raid1_rmeta(array_position: int, num_devices: int, region_size_sectors: int, array_sectors: int) -> bytes:
    """Build a Linux dm-raid v1 superblock (4 KiB) + clean intent bitmap."""
    buf = bytearray(8192)
    # struct dm_raid_superblock (drivers/md/dm-raid.c):
    # 0: magic "DmRd" (0x64526D44)
    # 4: features = 0
    # 8: num_devices
    # 12: array_position
    # 16: events (u64) = 1
    # 24: failed_devices (u64) = 0
    # 32: disk_recovery_offset (u64) = UINT64_MAX (in sync)
    # 40: array_resync_offset (u64) = UINT64_MAX (in sync)
    # 48: level (u32) = 1
    # 52: layout (u32) = 0
    # 56: stripe_sectors (u32) = 0xFFFFFFFF
    # 60: flags (u32) = 0
    struct.pack_into(
        "<IIIIQQQQIIII",
        buf,
        0,
        0x64526D44,
        0,
        num_devices,
        array_position,
        1,
        0,
        0xFFFFFFFFFFFFFFFF,
        0xFFFFFFFFFFFFFFFF,
        1,
        0,
        0xFFFFFFFF,
        0,
    )
    # Intent bitmap header at byte offset 4096 (MD bitmap superblock v4, BITMAP_MAGIC=0x6d746962)
    # Leaving bytes 4096..8191 cleanly initialized for Linux dm-raid bitmap_load:
    # magic=0x6d746962, version=4, uuid=0, events=1, events_cleared=1,
    # sync_size=array_sectors, state=0, chunksize=region_size_sectors*512,
    # daemon_sleep=5, write_behind=0, sectors_reserved=0
    struct.pack_into("<II", buf, 4096, 0x6D746962, 4)
    struct.pack_into("<QQQ", buf, 4096 + 24, 1, 1, array_sectors)
    struct.pack_into("<IIII", buf, 4096 + 48, 0, region_size_sectors * SECTOR_SIZE, 5, 0)
    return bytes(buf)


def build_dm_mirror_disk_log(region_size_sectors: int, array_sectors: int) -> bytes:
    """Build a clean Linux dm-log (persistent mirror log) header."""
    buf = bytearray(8192)
    # drivers/md/dm-log.c: log_header_disk:
    # magic = 0x4d695272 ("MiRr"), version = 2, nr_regions = ceil(array_sectors / region_size)
    nr_regions = (array_sectors + region_size_sectors - 1) // region_size_sectors
    struct.pack_into("<IIQ", buf, 0, 0x4D695272, 2, nr_regions)
    # Clean bitmap starts at byte offset 1024: all 1 bits = all regions in sync!
    bmp_bytes = (nr_regions + 7) // 8
    for i in range(bmp_bytes):
        buf[1024 + i] = 0xFF
    return bytes(buf)


def build_luks2_image(img: DiskImage, pbkdf: str, seed: str) -> None:
    """Build a valid opaque LUKS2 v2 binary header (primary + secondary)."""
    hdr_size = 16384
    uuid_hex = hashlib.sha256(("luks2-uuid:" + seed).encode("ascii")).hexdigest()
    uuid_str = f"{uuid_hex[0:8]}-{uuid_hex[8:12]}-{uuid_hex[12:16]}-{uuid_hex[16:20]}-{uuid_hex[20:32]}"
    salt0 = hashlib.sha256(("luks2-salt0:" + seed).encode("ascii")).digest()
    salt1 = hashlib.sha256(("luks2-salt1:" + seed).encode("ascii")).digest()

    kdf_obj = (
        {"type": "pbkdf2", "hash": "sha256", "iterations": 100000, "salt": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="}
        if pbkdf == "pbkdf2"
        else {"type": "argon2id", "time": 4, "memory": 65536, "cpus": 2, "salt": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="}
    )
    meta_json = json.dumps(
        {
            "keyslots": {
                "0": {
                    "type": "luks2",
                    "key_size": 64,
                    "af": {"type": "luks1", "stripes": 4000, "hash": "sha256"},
                    "area": {"type": "raw", "offset": "32768", "size": "258048", "encryption": "aes-xts-plain64", "key_size": 64},
                    "kdf": kdf_obj,
                }
            },
            "tokens": {},
            "segments": {
                "0": {
                    "type": "crypt",
                    "offset": "16777216",
                    "size": "dynamic",
                    "iv_tweak": "0",
                    "encryption": "aes-xts-plain64",
                    "sector_size": 512,
                }
            },
            "digests": {
                "0": {
                    "type": "pbkdf2",
                    "keyslots": ["0"],
                    "segments": ["0"],
                    "hash": "sha256",
                    "iterations": 1000,
                    "salt": "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB=",
                    "digest": "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC=",
                }
            },
            "config": {"json_size": "12288", "keyslots_size": "16744448"},
        },
        separators=(",", ":"),
    ).encode("ascii")

    for hdr_idx, (magic, off, salt) in enumerate(
        [(b"LUKS\xba\xbe", 0, salt0), (b"SKUL\xba\xbe", hdr_size, salt1)]
    ):
        hdr = bytearray(hdr_size)
        hdr[0:6] = magic
        struct.pack_into(">HQQ", hdr, 6, 2, hdr_size, 1)  # version=2, hdr_size, seqid=1
        hdr[24 : 24 + 5] = b"luks2"
        hdr[72 : 72 + 6] = b"sha256"
        hdr[104 : 104 + 64] = salt + b"\x00" * 32
        hdr[168 : 168 + len(uuid_str)] = uuid_str.encode("ascii")
        hdr[4096 : 4096 + len(meta_json)] = meta_json
        # Compute SHA-256 checksum over entire header with checksum field zeroed (448..511)
        csum = hashlib.sha256(bytes(hdr)).digest()
        hdr[448 : 448 + 32] = csum
        img.write(off, bytes(hdr))

    # Write deterministic ciphertext sample at data offset 16 MiB
    img.write(16 * MIB, det_payload("luks2-ct:" + seed, 65536))


# ---------------------------------------------------------------------------
# Expected oracle writers
# ---------------------------------------------------------------------------

def compute_lv_hash_lines(vg_name: str, lv_name: str, lv_bytes: bytes) -> List[str]:
    size = len(lv_bytes)
    win = 131072
    offsets = [0, size // 3, size // 2, size - win]
    lines = [f"# {vg_name}/{lv_name} size={size}"]
    for off in offsets:
        lines.append(
            f"{lv_name} offset={off} length={win} sha256={sha256_hex(lv_bytes[off : off + win])}"
        )
    lines.append(f"{lv_name} whole sha256={sha256_hex(lv_bytes)}")
    return lines


def write_expected_case(
    expected_root: Path,
    case_name: str,
    vg_meta: str,
    dm_lines: Sequence[str],
    lv_hash_lines: Sequence[str],
    vgs_line: str,
    pvs_lines: Sequence[str],
    lvs_lines: Sequence[str],
) -> None:
    cdir = expected_root / case_name
    cdir.mkdir(parents=True, exist_ok=True)
    (cdir / "vg-metadata.txt").write_text(vg_meta, encoding="ascii")
    (cdir / "dmsetup-table.txt").write_text("\n".join(dm_lines) + "\n", encoding="ascii")
    (cdir / "lv-hashes.txt").write_text("\n".join(lv_hash_lines) + "\n", encoding="ascii")
    (cdir / "vgs.txt").write_text(vgs_line + "\n", encoding="ascii")
    (cdir / "pvs.txt").write_text("\n".join(pvs_lines) + "\n", encoding="ascii")
    (cdir / "lvs.txt").write_text("\n".join(lvs_lines) + "\n", encoding="ascii")


# ---------------------------------------------------------------------------
# Case generators
# ---------------------------------------------------------------------------

def gen_linear_1pv(out_dir: Path, exp_dir: Optional[Path]) -> Dict:
    ext_sz = 8192  # 4 MiB
    ext_bytes = ext_sz * SECTOR_SIZE
    vg_u = det_uuid_raw("vgbase")
    pv_u = det_uuid_raw("vgbase:pv0")
    lv_u = det_uuid_raw("vgbase:lvlin")
    pvs = [{"key": "pv0", "uuid": pv_u, "device": "/dev/loop0", "dev_size": 262144, "pe_start": 2048, "pe_count": 31, "tags": ["pv_tag0"]}]
    lvs = [{
        "name": "lvlin", "uuid": lv_u, "tags": ["lv_tag0", "fast"],
        "segments": [{"start_extent": 0, "extent_count": 4, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 0)]}],
    }]
    meta = format_vg_metadata("vgbase", vg_u, 2, ext_sz, pvs, lvs, vg_tags=["base_vg"])
    data = det_payload("vgbase/lvlin", 4 * ext_bytes)
    img = DiskImage(out_dir / "linear-1pv.img", 128 * MIB)
    write_pv_label_and_mdas(img, pv_u, meta)
    img.write(MIB, data)
    img.flush()
    if exp_dir:
        write_expected_case(
            exp_dir, "linear-1pv", meta,
            ["vgbase-lvlin: 0 32768 linear 7:0 2048"],
            compute_lv_hash_lines("vgbase", "lvlin", data),
            f"vgbase|{fmt_uuid(vg_u)}|8192|2",
            [f"/dev/loop0|{fmt_uuid(pv_u)}|2048|31|1"],
            ["lvlin|vgbase|linear|16777216"],
        )
    return {"description": "Baseline single-PV linear LV with tags", "images": ["linear-1pv.img"], "vg_name": "vgbase", "extent_size_sectors": ext_sz, "pv_count": 1}


def gen_linear_1pv_multiseg(out_dir: Path, exp_dir: Optional[Path]) -> Dict:
    ext_sz = 8192
    ext_bytes = ext_sz * SECTOR_SIZE
    vg_u = det_uuid_raw("vgseg")
    pv_u = det_uuid_raw("vgseg:pv0")
    pvs = [{"key": "pv0", "uuid": pv_u, "device": "/dev/loop1", "dev_size": 262144, "pe_start": 2048, "pe_count": 31}]
    lvs = [
        {
            "name": "lvA", "uuid": det_uuid_raw("vgseg:lvA"),
            "segments": [
                {"start_extent": 0, "extent_count": 2, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 0)]},
                {"start_extent": 2, "extent_count": 4, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 4)]},
                {"start_extent": 6, "extent_count": 4, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 10)]},
            ],
        },
        {
            "name": "lvB", "uuid": det_uuid_raw("vgseg:lvB"),
            "segments": [{"start_extent": 0, "extent_count": 2, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 2)]}],
        },
        {
            "name": "lvC", "uuid": det_uuid_raw("vgseg:lvC"),
            "segments": [{"start_extent": 0, "extent_count": 2, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 8)]}],
        },
    ]
    meta = format_vg_metadata("vgseg", vg_u, 86, ext_sz, pvs, lvs)
    dA = det_payload("vgseg/lvA", 10 * ext_bytes)
    dB = det_payload("vgseg/lvB", 2 * ext_bytes)
    dC = det_payload("vgseg/lvC", 2 * ext_bytes)
    img = DiskImage(out_dir / "linear-1pv-multiseg.img", 128 * MIB)
    write_pv_label_and_mdas(img, pv_u, meta, wrap_mda=True)
    img.write(MIB + 0 * ext_bytes, dA[0 : 2 * ext_bytes])
    img.write(MIB + 2 * ext_bytes, dB)
    img.write(MIB + 4 * ext_bytes, dA[2 * ext_bytes : 6 * ext_bytes])
    img.write(MIB + 8 * ext_bytes, dC)
    img.write(MIB + 10 * ext_bytes, dA[6 * ext_bytes : 10 * ext_bytes])
    img.flush()
    if exp_dir:
        dm = [
            "vgseg-lvA: 0 16384 linear 7:1 2048",
            "vgseg-lvA: 16384 32768 linear 7:1 34816",
            "vgseg-lvA: 49152 32768 linear 7:1 83968",
            "vgseg-lvB: 0 16384 linear 7:1 18432",
            "vgseg-lvC: 0 16384 linear 7:1 67584",
        ]
        hashes = (
            compute_lv_hash_lines("vgseg", "lvA", dA)
            + compute_lv_hash_lines("vgseg", "lvB", dB)
            + compute_lv_hash_lines("vgseg", "lvC", dC)
        )
        write_expected_case(
            exp_dir, "linear-1pv-multiseg", meta, dm, hashes,
            f"vgseg|{fmt_uuid(vg_u)}|8192|86",
            [f"/dev/loop1|{fmt_uuid(pv_u)}|2048|31|1"],
            ["lvA|vgseg|linear|41943040", "lvB|vgseg|linear|8388608", "lvC|vgseg|linear|8388608"],
        )
    return {"description": "One LV with 3 discontiguous segments on a single PV", "images": ["linear-1pv-multiseg.img"], "vg_name": "vgseg", "extent_size_sectors": ext_sz, "pv_count": 1}


def gen_linear_2pv(out_dir: Path, exp_dir: Optional[Path]) -> Dict:
    ext_sz = 8192
    ext_bytes = ext_sz * SECTOR_SIZE
    vg_u = det_uuid_raw("vgspan")
    pv0_u = det_uuid_raw("vgspan:pv0")
    pv1_u = det_uuid_raw("vgspan:pv1")
    pvs = [
        {"key": "pv0", "uuid": pv0_u, "device": "/dev/loop2", "dev_size": 262144, "pe_start": 2048, "pe_count": 31},
        {"key": "pv1", "uuid": pv1_u, "device": "/dev/loop3", "dev_size": 262144, "pe_start": 2048, "pe_count": 31},
    ]
    lvs = [{
        "name": "lvspan", "uuid": det_uuid_raw("vgspan:lvspan"),
        "segments": [
            {"start_extent": 0, "extent_count": 31, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 0)]},
            {"start_extent": 31, "extent_count": 9, "type": "striped", "stripe_count": 1, "stripes": [("pv1", 0)]},
        ],
    }]
    meta = format_vg_metadata("vgspan", vg_u, 2, ext_sz, pvs, lvs)
    data = det_payload("vgspan/lvspan", 40 * ext_bytes)
    img_a = DiskImage(out_dir / "linear-2pv-a.img", 128 * MIB)
    img_b = DiskImage(out_dir / "linear-2pv-b.img", 128 * MIB)
    write_pv_label_and_mdas(img_a, pv0_u, meta)
    write_pv_label_and_mdas(img_b, pv1_u, meta)
    img_a.write(MIB, data[0 : 31 * ext_bytes])
    img_b.write(MIB, data[31 * ext_bytes : 40 * ext_bytes])
    img_a.flush()
    img_b.flush()
    if exp_dir:
        dm = [
            "vgspan-lvspan: 0 253952 linear 7:2 2048",
            "vgspan-lvspan: 253952 73728 linear 7:3 2048",
        ]
        write_expected_case(
            exp_dir, "linear-2pv", meta, dm,
            compute_lv_hash_lines("vgspan", "lvspan", data),
            f"vgspan|{fmt_uuid(vg_u)}|8192|2",
            [f"/dev/loop2|{fmt_uuid(pv0_u)}|2048|31|1", f"/dev/loop3|{fmt_uuid(pv1_u)}|2048|31|1"],
            ["lvspan|vgspan|linear|167772160"],
        )
    return {"description": "LV spanning two PVs", "images": ["linear-2pv-a.img", "linear-2pv-b.img"], "vg_name": "vgspan", "extent_size_sectors": ext_sz, "pv_count": 2}


def gen_striped_2pv(out_dir: Path, exp_dir: Optional[Path]) -> Dict:
    ext_sz = 8192
    ext_bytes = ext_sz * SECTOR_SIZE
    stripe_bytes = 128 * SECTOR_SIZE  # 64 KiB
    vg_u = det_uuid_raw("vgstripe")
    pv0_u = det_uuid_raw("vgstripe:pv0")
    pv1_u = det_uuid_raw("vgstripe:pv1")
    pvs = [
        {"key": "pv0", "uuid": pv0_u, "device": "/dev/loop4", "dev_size": 262144, "pe_start": 2048, "pe_count": 31},
        {"key": "pv1", "uuid": pv1_u, "device": "/dev/loop5", "dev_size": 262144, "pe_start": 2048, "pe_count": 31},
    ]
    lvs = [{
        "name": "lvstr", "uuid": det_uuid_raw("vgstripe:lvstr"),
        "segments": [{
            "start_extent": 0, "extent_count": 8, "type": "striped",
            "stripe_count": 2, "stripe_size": 128,
            "stripes": [("pv0", 0), ("pv1", 0)],
        }],
    }]
    meta = format_vg_metadata("vgstripe", vg_u, 2, ext_sz, pvs, lvs)
    lv_size = 8 * ext_bytes
    data = det_payload("vgstripe/lvstr", lv_size)
    pv0_buf = bytearray(4 * ext_bytes)
    pv1_buf = bytearray(4 * ext_bytes)
    n_chunks = lv_size // stripe_bytes
    for c in range(n_chunks):
        src = data[c * stripe_bytes : (c + 1) * stripe_bytes]
        row = c // 2
        if c % 2 == 0:
            pv0_buf[row * stripe_bytes : (row + 1) * stripe_bytes] = src
        else:
            pv1_buf[row * stripe_bytes : (row + 1) * stripe_bytes] = src
    img_a = DiskImage(out_dir / "striped-2pv-a.img", 128 * MIB)
    img_b = DiskImage(out_dir / "striped-2pv-b.img", 128 * MIB)
    write_pv_label_and_mdas(img_a, pv0_u, meta)
    write_pv_label_and_mdas(img_b, pv1_u, meta)
    img_a.write(MIB, bytes(pv0_buf))
    img_b.write(MIB, bytes(pv1_buf))
    img_a.flush()
    img_b.flush()
    if exp_dir:
        dm = ["vgstripe-lvstr: 0 65536 striped 2 128 7:4 2048 7:5 2048"]
        write_expected_case(
            exp_dir, "striped-2pv", meta, dm,
            compute_lv_hash_lines("vgstripe", "lvstr", data),
            f"vgstripe|{fmt_uuid(vg_u)}|8192|2",
            [f"/dev/loop4|{fmt_uuid(pv0_u)}|2048|31|1", f"/dev/loop5|{fmt_uuid(pv1_u)}|2048|31|1"],
            ["lvstr|vgstripe|striped|33554432"],
        )
    return {"description": "Round-robin 2-PV striping with 64 KiB stripe chunks", "images": ["striped-2pv-a.img", "striped-2pv-b.img"], "vg_name": "vgstripe", "extent_size_sectors": ext_sz, "pv_count": 2}


def gen_sector4k(out_dir: Path, exp_dir: Optional[Path]) -> Dict:
    ext_sz = 8192
    ext_bytes = ext_sz * SECTOR_SIZE
    vg_u = det_uuid_raw("vg4k")
    pv_u = det_uuid_raw("vg4k:pv0")
    pvs = [{"key": "pv0", "uuid": pv_u, "device": "/dev/loop6", "dev_size": 262144, "pe_start": 2048, "pe_count": 31}]
    lvs = [{
        "name": "lv4k", "uuid": det_uuid_raw("vg4k:lv4k"),
        "segments": [{"start_extent": 0, "extent_count": 4, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 0)]}],
    }]
    meta = format_vg_metadata("vg4k", vg_u, 2, ext_sz, pvs, lvs)
    data = det_payload("vg4k/lv4k", 4 * ext_bytes)
    img = DiskImage(out_dir / "sector4k.img", 128 * MIB)
    write_pv_label_and_mdas(img, pv_u, meta)
    img.write(MIB, data)
    img.flush()
    if exp_dir:
        write_expected_case(
            exp_dir, "sector4k", meta,
            ["vg4k-lv4k: 0 32768 linear 7:6 2048"],
            compute_lv_hash_lines("vg4k", "lv4k", data),
            f"vg4k|{fmt_uuid(vg_u)}|8192|2",
            [f"/dev/loop6|{fmt_uuid(pv_u)}|2048|31|1"],
            ["lv4k|vg4k|linear|16777216"],
        )
    return {"description": "4Kn-aligned PV (4096-byte logical sectors)", "images": ["sector4k.img"], "vg_name": "vg4k", "extent_size_sectors": ext_sz, "pv_count": 1}


def gen_odd_geometry(out_dir: Path, exp_dir: Optional[Path]) -> Dict:
    ext_sz = 4096  # 2 MiB extents
    ext_bytes = ext_sz * SECTOR_SIZE
    vg_u = det_uuid_raw("vgodd")
    pv_u = det_uuid_raw("vgodd:pv0")
    pvs = [{"key": "pv0", "uuid": pv_u, "device": "/dev/loop7", "dev_size": 262144, "pe_start": 2048, "pe_count": 62}]
    lvs = [{
        "name": "lvodd", "uuid": det_uuid_raw("vgodd:lvodd"),
        "segments": [{"start_extent": 0, "extent_count": 8, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 0)]}],
    }]
    meta = format_vg_metadata("vgodd", vg_u, 122, ext_sz, pvs, lvs)
    data = det_payload("vgodd/lvodd", 8 * ext_bytes)
    img = DiskImage(out_dir / "odd-geometry.img", 128 * MIB)
    # Dual MDAs at exact offsets 4096 and 133169152 (127 MiB) with wrapped ring buffer!
    write_pv_label_and_mdas(img, pv_u, meta, mdas=((4096, 1044480), (133169152, 1048576)), wrap_mda=True)
    img.write(MIB, data)
    img.flush()
    if exp_dir:
        write_expected_case(
            exp_dir, "odd-geometry", meta,
            ["vgodd-lvodd: 0 32768 linear 7:7 2048"],
            compute_lv_hash_lines("vgodd", "lvodd", data),
            f"vgodd|{fmt_uuid(vg_u)}|4096|122",
            [f"/dev/loop7|{fmt_uuid(pv_u)}|2048|62|2"],
            ["lvodd|vgodd|linear|16777216"],
        )
    return {"description": "2 MiB extents, dual metadata areas, wrapped MDA ring buffer", "images": ["odd-geometry.img"], "vg_name": "vgodd", "extent_size_sectors": ext_sz, "pv_count": 1}


def gen_thin_pool(out_dir: Path, exp_dir: Optional[Path]) -> Dict:
    ext_sz = 8192  # 4 MiB
    ext_bytes = ext_sz * SECTOR_SIZE
    chunk_sectors = 128  # 64 KiB
    chunk_bytes = chunk_sectors * SECTOR_SIZE
    vg_u = det_uuid_raw("vgthin")
    pv_u = det_uuid_raw("vgthin:pv0")
    pvs = [{"key": "pv0", "uuid": pv_u, "device": "/dev/loop10", "dev_size": 131072, "pe_start": 2048, "pe_count": 15}]
    lvs = [
        {
            "name": "tpool", "uuid": det_uuid_raw("vgthin:tpool"),
            "status": ["READ", "WRITE", "VISIBLE"],
            "segments": [{
                "start_extent": 0, "extent_count": 8, "type": "thin-pool",
                "metadata": "tpool_tmeta", "pool": "tpool_tdata",
                "transaction_id": 2, "chunk_size": chunk_sectors,
                "discards": "passdown", "zero_new_blocks": 1,
            }],
        },
        {
            "name": "lvthin", "uuid": det_uuid_raw("vgthin:lvthin"),
            "tags": ["thin_vol"],
            "segments": [{
                "start_extent": 0, "extent_count": 2, "type": "thin",
                "thin_pool": "tpool", "transaction_id": 0, "device_id": 1,
            }],
        },
        {
            "name": "lvthinsnap", "uuid": det_uuid_raw("vgthin:lvthinsnap"),
            "tags": ["thin_snap"],
            "segments": [{
                "start_extent": 0, "extent_count": 2, "type": "thin",
                "thin_pool": "tpool", "transaction_id": 1, "device_id": 2,
                "origin": "lvthin",
            }],
        },
        {
            "name": "lvol0_pmspare", "uuid": det_uuid_raw("vgthin:pmspare"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 9)]}],
        },
        {
            "name": "tpool_tmeta", "uuid": det_uuid_raw("vgthin:tmeta"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 0)]}],
        },
        {
            "name": "tpool_tdata", "uuid": det_uuid_raw("vgthin:tdata"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 8, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 1)]}],
        },
    ]
    meta = format_vg_metadata("vgthin", vg_u, 6, ext_sz, pvs, lvs)

    # 8 MiB virtual LVs (128 chunks of 64 KiB each).
    # Shared chunks (time=0, stime=1): vblk 0->pblk 3, 2->7, 17->11, 63->4, 127->1
    # Unique to lvthin (time=1): vblk 1->pblk 0, 5->2, 64->9
    # Unique to lvthinsnap (time=1): vblk 1->pblk 12, 64->13, 100->14
    lvthin_map = [(0, 3, 0), (1, 0, 1), (2, 7, 0), (5, 2, 1), (17, 11, 0), (63, 4, 0), (64, 9, 1), (127, 1, 0)]
    lvsnap_map = [(0, 3, 0), (1, 12, 1), (2, 7, 0), (17, 11, 0), (63, 4, 0), (64, 13, 1), (100, 14, 1), (127, 1, 0)]

    tmeta_bytes = build_dm_thin_metadata(
        meta_nr_blocks=1024,
        data_nr_blocks=512,
        data_block_size_sectors=chunk_sectors,
        time_val=1,
        trans_id=2,
        devices=[
            {"dev_id": 1, "trans_id": 0, "ctime": 0, "stime": 1, "mappings": lvthin_map},
            {"dev_id": 2, "trans_id": 1, "ctime": 1, "stime": 1, "mappings": lvsnap_map},
        ],
    )

    pblk_data: Dict[int, bytes] = {}
    for pblk in sorted({p for _, p, _ in lvthin_map + lvsnap_map}):
        pblk_data[pblk] = det_payload(f"vgthin/pblk/{pblk}", chunk_bytes)

    def render_thin(mappings: Sequence[Tuple[int, int, int]]) -> bytes:
        buf = bytearray(2 * ext_bytes)
        for vblk, pblk, _ in mappings:
            buf[vblk * chunk_bytes : (vblk + 1) * chunk_bytes] = pblk_data[pblk]
        return bytes(buf)

    lvthin_bytes = render_thin(lvthin_map)
    lvsnap_bytes = render_thin(lvsnap_map)

    img = DiskImage(out_dir / "thin-pool.img", 64 * MIB)
    write_pv_label_and_mdas(img, pv_u, meta)
    img.write(MIB + 0 * ext_bytes, tmeta_bytes)
    for pblk, cdata in pblk_data.items():
        img.write(MIB + 1 * ext_bytes + pblk * chunk_bytes, cdata)
    img.flush()

    if exp_dir:
        dm = [
            "vgthin-lvthin: 0 16384 thin 252:3 1",
            "vgthin-lvthinsnap: 0 16384 thin 252:3 2",
            "vgthin-tpool-tpool: 0 65536 thin-pool 252:1 252:2 128 0 0",
            "vgthin-tpool_tmeta: 0 8192 linear 7:10 2048",
            "vgthin-tpool_tdata: 0 65536 linear 7:10 10240",
        ]
        hashes = (
            compute_lv_hash_lines("vgthin", "lvthin", lvthin_bytes)
            + compute_lv_hash_lines("vgthin", "lvthinsnap", lvsnap_bytes)
        )
        write_expected_case(
            exp_dir, "thin-pool", meta, dm, hashes,
            f"vgthin|{fmt_uuid(vg_u)}|8192|6",
            [f"/dev/loop10|{fmt_uuid(pv_u)}|2048|15|1"],
            ["tpool|vgthin|thin-pool|33554432", "lvthin|vgthin|thin|8388608", "lvthinsnap|vgthin|thin|8388608"],
        )
    return {"description": "dm-thin v2 thin-pool with sparse thin LV and thin snapshot", "images": ["thin-pool.img"], "vg_name": "vgthin", "extent_size_sectors": ext_sz, "pv_count": 1}


def gen_snapshot(out_dir: Path, exp_dir: Optional[Path]) -> Dict:
    ext_sz = 8192  # 4 MiB
    ext_bytes = ext_sz * SECTOR_SIZE
    chunk_sectors = 8  # 4 KiB
    chunk_bytes = chunk_sectors * SECTOR_SIZE
    vg_u = det_uuid_raw("vgsnap")
    pv_u = det_uuid_raw("vgsnap:pv0")
    pvs = [{"key": "pv0", "uuid": pv_u, "device": "/dev/loop11", "dev_size": 131072, "pe_start": 2048, "pe_count": 15}]
    lvs = [
        {
            "name": "lvorig", "uuid": det_uuid_raw("vgsnap:lvorig"),
            "segments": [{"start_extent": 0, "extent_count": 2, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 0)]}],
        },
        {
            "name": "lvsnap", "uuid": det_uuid_raw("vgsnap:lvsnap"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 2)]}],
        },
        {
            "name": "snapshot0", "uuid": det_uuid_raw("vgsnap:snapshot0"),
            "status": ["READ", "WRITE", "VISIBLE"],
            "segments": [{
                "start_extent": 0, "extent_count": 2, "type": "snapshot",
                "chunk_size": chunk_sectors, "origin": "lvorig", "cow_store": "lvsnap",
            }],
        },
    ]
    meta = format_vg_metadata("vgsnap", vg_u, 4, ext_sz, pvs, lvs)

    orig_bytes = det_payload("vgsnap/lvorig", 2 * ext_bytes)
    snap_bytes = bytearray(orig_bytes)
    exc_chunks = [0, 1, 15, 256, 512, 1024, 2047]
    exceptions: List[Tuple[int, bytes]] = []
    for c in exc_chunks:
        old_data = det_payload(f"vgsnap/cow/{c}", chunk_bytes)
        exceptions.append((c, old_data))
        snap_bytes[c * chunk_bytes : (c + 1) * chunk_bytes] = old_data
    cow_bytes = build_dm_snapshot_cow(1 * ext_bytes, chunk_sectors, exceptions)

    img = DiskImage(out_dir / "snapshot.img", 64 * MIB)
    write_pv_label_and_mdas(img, pv_u, meta)
    img.write(MIB + 0 * ext_bytes, orig_bytes)
    img.write(MIB + 2 * ext_bytes, cow_bytes)
    img.flush()

    if exp_dir:
        dm = [
            "vgsnap-lvorig: 0 16384 snapshot-origin 252:1",
            "vgsnap-lvorig-real: 0 16384 linear 7:11 2048",
            "vgsnap-lvsnap: 0 16384 snapshot 252:1 252:2 P 8",
            "vgsnap-lvsnap-cow: 0 8192 linear 7:11 18432",
        ]
        hashes = (
            compute_lv_hash_lines("vgsnap", "lvorig", orig_bytes)
            + compute_lv_hash_lines("vgsnap", "lvsnap", bytes(snap_bytes))
        )
        write_expected_case(
            exp_dir, "snapshot", meta, dm, hashes,
            f"vgsnap|{fmt_uuid(vg_u)}|8192|4",
            [f"/dev/loop11|{fmt_uuid(pv_u)}|2048|15|1"],
            ["lvorig|vgsnap|snapshot-origin|8388608", "lvsnap|vgsnap|snapshot|8388608"],
        )
    return {"description": "Classic LVM2 snapshot-origin and persistent COW snapshot", "images": ["snapshot.img"], "vg_name": "vgsnap", "extent_size_sectors": ext_sz, "pv_count": 1}


def gen_raid1_mirror(out_dir: Path, exp_dir: Optional[Path]) -> Dict:
    ext_sz = 8192  # 4 MiB
    ext_bytes = ext_sz * SECTOR_SIZE
    vg_u = det_uuid_raw("vgraid")
    pv0_u = det_uuid_raw("vgraid:pv0")
    pv1_u = det_uuid_raw("vgraid:pv1")
    pvs = [
        {"key": "pv0", "uuid": pv0_u, "device": "/dev/loop12", "dev_size": 131072, "pe_start": 2048, "pe_count": 15},
        {"key": "pv1", "uuid": pv1_u, "device": "/dev/loop13", "dev_size": 131072, "pe_start": 2048, "pe_count": 15},
    ]
    lvs = [
        {
            "name": "lvraid1", "uuid": det_uuid_raw("vgraid:lvraid1"),
            "segments": [{
                "start_extent": 0, "extent_count": 2, "type": "raid1",
                "device_count": 2, "region_size": 4096,
                "raids": [("lvraid1_rmeta_0", "lvraid1_rimage_0"), ("lvraid1_rmeta_1", "lvraid1_rimage_1")],
            }],
        },
        {
            "name": "lvraid1_rmeta_0", "uuid": det_uuid_raw("vgraid:rmeta0"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 0)]}],
        },
        {
            "name": "lvraid1_rimage_0", "uuid": det_uuid_raw("vgraid:rimage0"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 2, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 1)]}],
        },
        {
            "name": "lvraid1_rmeta_1", "uuid": det_uuid_raw("vgraid:rmeta1"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv1", 0)]}],
        },
        {
            "name": "lvraid1_rimage_1", "uuid": det_uuid_raw("vgraid:rimage1"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 2, "type": "striped", "stripe_count": 1, "stripes": [("pv1", 1)]}],
        },
        {
            "name": "lvmirror", "uuid": det_uuid_raw("vgraid:lvmirror"),
            "segments": [{
                "start_extent": 0, "extent_count": 2, "type": "mirror",
                "mirror_count": 2, "region_size": 4096,
                "mirrors": [("lvmirror_mimage_0", 0), ("lvmirror_mimage_1", 0)],
            }],
        },
        {
            "name": "lvmirror_mimage_0", "uuid": det_uuid_raw("vgraid:mimage0"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 2, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 3)]}],
        },
        {
            "name": "lvmirror_mimage_1", "uuid": det_uuid_raw("vgraid:mimage1"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 2, "type": "striped", "stripe_count": 1, "stripes": [("pv1", 3)]}],
        },
        {
            "name": "lvmirlog", "uuid": det_uuid_raw("vgraid:lvmirlog"),
            "segments": [{
                "start_extent": 0, "extent_count": 1, "type": "mirror",
                "mirror_count": 2, "mirror_log": "lvmirlog_mlog", "region_size": 4096,
                "mirrors": [("lvmirlog_mimage_0", 0), ("lvmirlog_mimage_1", 0)],
            }],
        },
        {
            "name": "lvmirlog_mlog", "uuid": det_uuid_raw("vgraid:mlog"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 5)]}],
        },
        {
            "name": "lvmirlog_mimage_0", "uuid": det_uuid_raw("vgraid:mlimage0"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 6)]}],
        },
        {
            "name": "lvmirlog_mimage_1", "uuid": det_uuid_raw("vgraid:mlimage1"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv1", 5)]}],
        },
    ]
    meta = format_vg_metadata("vgraid", vg_u, 8, ext_sz, pvs, lvs)

    d_raid1 = det_payload("vgraid/lvraid1", 2 * ext_bytes)
    d_mir = det_payload("vgraid/lvmirror", 2 * ext_bytes)
    d_mirlog = det_payload("vgraid/lvmirlog", 1 * ext_bytes)

    img_a = DiskImage(out_dir / "raid1-mirror-a.img", 64 * MIB)
    img_b = DiskImage(out_dir / "raid1-mirror-b.img", 64 * MIB)
    write_pv_label_and_mdas(img_a, pv0_u, meta)
    write_pv_label_and_mdas(img_b, pv1_u, meta)
    img_a.write(MIB + 0 * ext_bytes, build_dm_raid1_rmeta(0, 2, 4096, 16384))
    img_b.write(MIB + 0 * ext_bytes, build_dm_raid1_rmeta(1, 2, 4096, 16384))
    img_a.write(MIB + 1 * ext_bytes, d_raid1)
    img_b.write(MIB + 1 * ext_bytes, d_raid1)
    img_a.write(MIB + 3 * ext_bytes, d_mir)
    img_b.write(MIB + 3 * ext_bytes, d_mir)
    img_a.write(MIB + 5 * ext_bytes, build_dm_mirror_disk_log(4096, 8192))
    img_a.write(MIB + 6 * ext_bytes, d_mirlog)
    img_b.write(MIB + 5 * ext_bytes, d_mirlog)
    img_a.flush()
    img_b.flush()

    if exp_dir:
        dm = [
            "vgraid-lvraid1: 0 16384 raid raid1 3 0 region_size 4096 2 252:1 252:2 252:3 252:4",
            "vgraid-lvmirror: 0 16384 mirror core 1 4096 2 252:5 0 252:6 0",
            "vgraid-lvmirlog: 0 8192 mirror disk 2 252:7 4096 2 252:8 0 252:9 0",
        ]
        hashes = (
            compute_lv_hash_lines("vgraid", "lvraid1", d_raid1)
            + compute_lv_hash_lines("vgraid", "lvmirror", d_mir)
            + compute_lv_hash_lines("vgraid", "lvmirlog", d_mirlog)
        )
        write_expected_case(
            exp_dir, "raid1-mirror", meta, dm, hashes,
            f"vgraid|{fmt_uuid(vg_u)}|8192|8",
            [f"/dev/loop12|{fmt_uuid(pv0_u)}|2048|15|1", f"/dev/loop13|{fmt_uuid(pv1_u)}|2048|15|1"],
            ["lvraid1|vgraid|raid1|8388608", "lvmirror|vgraid|mirror|8388608", "lvmirlog|vgraid|mirror|4194304"],
        )
    return {"description": "2-PV raid1 with DmRd superblocks plus core-log and disk-log mirror LVs", "images": ["raid1-mirror-a.img", "raid1-mirror-b.img"], "vg_name": "vgraid", "extent_size_sectors": ext_sz, "pv_count": 2}


def gen_mixed_vg(out_dir: Path, exp_dir: Optional[Path]) -> Dict:
    """Mixed VG: supported (snapshot-origin, snapshot, thin, raid1) + unsupported (cache, raid5_ls)."""
    ext_sz = 8192  # 4 MiB
    ext_bytes = ext_sz * SECTOR_SIZE
    chunk_sectors_thin = 128
    chunk_bytes_thin = chunk_sectors_thin * SECTOR_SIZE
    vg_u = det_uuid_raw("vgmix")
    pv0_u = det_uuid_raw("vgmix:pv0")
    pv1_u = det_uuid_raw("vgmix:pv1")
    pvs = [
        {"key": "pv0", "uuid": pv0_u, "device": "/dev/loop8", "dev_size": 262144, "pe_start": 2048, "pe_count": 31},
        {"key": "pv1", "uuid": pv1_u, "device": "/dev/loop9", "dev_size": 262144, "pe_start": 2048, "pe_count": 31},
    ]
    lvs = [
        # 1. Snapshot origin + classic snapshot
        {
            "name": "lvplain", "uuid": det_uuid_raw("vgmix:lvplain"),
            "segments": [{"start_extent": 0, "extent_count": 4, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 0)]}],
        },
        {
            "name": "lvsnap", "uuid": det_uuid_raw("vgmix:lvsnap"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 16)]}],
        },
        {
            "name": "snapshot0", "uuid": det_uuid_raw("vgmix:snapshot0"),
            "status": ["READ", "WRITE", "VISIBLE"],
            "segments": [{"start_extent": 0, "extent_count": 4, "type": "snapshot", "chunk_size": 8, "origin": "lvplain", "cow_store": "lvsnap"}],
        },
        # 2. Thin-pool + thin LV
        {
            "name": "tpool", "uuid": det_uuid_raw("vgmix:tpool"),
            "segments": [{
                "start_extent": 0, "extent_count": 8, "type": "thin-pool",
                "metadata": "tpool_tmeta", "pool": "tpool_tdata",
                "transaction_id": 1, "chunk_size": chunk_sectors_thin,
                "discards": "passdown", "zero_new_blocks": 1,
            }],
        },
        {
            "name": "lvthin", "uuid": det_uuid_raw("vgmix:lvthin"),
            "segments": [{"start_extent": 0, "extent_count": 2, "type": "thin", "thin_pool": "tpool", "transaction_id": 0, "device_id": 1}],
        },
        {
            "name": "lvol0_pmspare", "uuid": det_uuid_raw("vgmix:pmspare"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 4)]}],
        },
        {
            "name": "tpool_tmeta", "uuid": det_uuid_raw("vgmix:tmeta"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv1", 0)]}],
        },
        {
            "name": "tpool_tdata", "uuid": det_uuid_raw("vgmix:tdata"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 8, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 5)]}],
        },
        # 3. RAID1
        {
            "name": "lvraid", "uuid": det_uuid_raw("vgmix:lvraid"),
            "segments": [{
                "start_extent": 0, "extent_count": 2, "type": "raid1",
                "device_count": 2, "region_size": 4096,
                "raids": [("lvraid_rmeta_0", "lvraid_rimage_0"), ("lvraid_rmeta_1", "lvraid_rimage_1")],
            }],
        },
        {
            "name": "lvraid_rmeta_0", "uuid": det_uuid_raw("vgmix:rm0"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 13)]}],
        },
        {
            "name": "lvraid_rimage_0", "uuid": det_uuid_raw("vgmix:ri0"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 2, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 14)]}],
        },
        {
            "name": "lvraid_rmeta_1", "uuid": det_uuid_raw("vgmix:rm1"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv1", 1)]}],
        },
        {
            "name": "lvraid_rimage_1", "uuid": det_uuid_raw("vgmix:ri1"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 2, "type": "striped", "stripe_count": 1, "stripes": [("pv1", 2)]}],
        },
        # 4. Out-of-scope unsupported LV: cache (fails closed per-LV, keeps supported LVs mappable!)
        {
            "name": "lvcache", "uuid": det_uuid_raw("vgmix:lvcache"),
            "segments": [{"start_extent": 0, "extent_count": 2, "type": "cache", "cache_pool": "cpool", "origin": "lvcache_corig"}],
        },
        {
            "name": "lvcache_corig", "uuid": det_uuid_raw("vgmix:corig"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 2, "type": "striped", "stripe_count": 1, "stripes": [("pv0", 17)]}],
        },
        {
            "name": "cpool", "uuid": det_uuid_raw("vgmix:cpool"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "cache-pool", "data": "cpool_cdata", "metadata": "cpool_cmeta", "chunk_size": 128}],
        },
        {
            "name": "cpool_cdata", "uuid": det_uuid_raw("vgmix:cdata"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv1", 4)]}],
        },
        {
            "name": "cpool_cmeta", "uuid": det_uuid_raw("vgmix:cmeta"),
            "status": ["READ", "WRITE"],
            "segments": [{"start_extent": 0, "extent_count": 1, "type": "striped", "stripe_count": 1, "stripes": [("pv1", 5)]}],
        },
    ]
    meta = format_vg_metadata("vgmix", vg_u, 15, ext_sz, pvs, lvs)

    d_plain = det_payload("vgmix/lvplain", 4 * ext_bytes)
    snap_bytes = bytearray(d_plain)
    snap_exc = []
    for c in [0, 3, 64, 1024]:
        cb = det_payload(f"vgmix/cow/{c}", 4096)
        snap_exc.append((c, cb))
        snap_bytes[c * 4096 : (c + 1) * 4096] = cb
    cow_bytes = build_dm_snapshot_cow(1 * ext_bytes, 8, snap_exc)

    thin_map = [(0, 0, 0), (3, 1, 0), (16, 4, 0), (127, 2, 0)]
    tmeta_bytes = build_dm_thin_metadata(
        1024, 512, chunk_sectors_thin, 0, 1,
        [{"dev_id": 1, "trans_id": 0, "ctime": 0, "stime": 0, "mappings": thin_map}],
    )
    thin_pblks = {p: det_payload(f"vgmix/thin/{p}", chunk_bytes_thin) for _, p, _ in thin_map}
    d_thin = bytearray(2 * ext_bytes)
    for vb, pb, _ in thin_map:
        d_thin[vb * chunk_bytes_thin : (vb + 1) * chunk_bytes_thin] = thin_pblks[pb]

    d_raid = det_payload("vgmix/lvraid", 2 * ext_bytes)

    img_a = DiskImage(out_dir / "mixed-vg-a.img", 128 * MIB)
    img_b = DiskImage(out_dir / "mixed-vg-b.img", 128 * MIB)
    write_pv_label_and_mdas(img_a, pv0_u, meta)
    write_pv_label_and_mdas(img_b, pv1_u, meta)
    img_a.write(MIB + 0 * ext_bytes, d_plain)
    img_a.write(MIB + 16 * ext_bytes, cow_bytes)
    img_b.write(MIB + 0 * ext_bytes, tmeta_bytes)
    for pb, cdata in thin_pblks.items():
        img_a.write(MIB + 5 * ext_bytes + pb * chunk_bytes_thin, cdata)
    img_a.write(MIB + 13 * ext_bytes, build_dm_raid1_rmeta(0, 2, 4096, 16384))
    img_b.write(MIB + 1 * ext_bytes, build_dm_raid1_rmeta(1, 2, 4096, 16384))
    img_a.write(MIB + 14 * ext_bytes, d_raid)
    img_b.write(MIB + 2 * ext_bytes, d_raid)
    img_a.flush()
    img_b.flush()

    if exp_dir:
        dm = [
            "vgmix-lvplain: 0 32768 snapshot-origin 252:15",
            "vgmix-lvplain-real: 0 32768 linear 7:8 2048",
            "vgmix-lvsnap: 0 32768 snapshot 252:15 252:16 P 8",
            "vgmix-lvsnap-cow: 0 8192 linear 7:8 133120",
            "vgmix-lvthin: 0 16384 thin 252:7 1",
            "vgmix-lvraid: 0 16384 raid raid1 3 0 region_size 4096 2 252:10 252:11 252:12 252:13",
            "vgmix-lvcache: 0 16384 cache 252:20 252:21 252:22 128 1 writethrough smq 0",
        ]
        hashes = (
            compute_lv_hash_lines("vgmix", "lvplain", d_plain)
            + compute_lv_hash_lines("vgmix", "lvsnap", bytes(snap_bytes))
            + compute_lv_hash_lines("vgmix", "lvthin", bytes(d_thin))
            + compute_lv_hash_lines("vgmix", "lvraid", d_raid)
        )
        write_expected_case(
            exp_dir, "mixed-vg", meta, dm, hashes,
            f"vgmix|{fmt_uuid(vg_u)}|8192|15",
            [f"/dev/loop8|{fmt_uuid(pv0_u)}|2048|31|1", f"/dev/loop9|{fmt_uuid(pv1_u)}|2048|31|1"],
            [
                "lvplain|vgmix|snapshot-origin|16777216",
                "lvsnap|vgmix|snapshot|16777216",
                "lvthin|vgmix|thin|8388608",
                "lvraid|vgmix|raid1|8388608",
                "lvcache|vgmix|cache|8388608",
            ],
        )
    return {"description": "Supported snapshot/thin/raid1 LVs beside unsupported cache LV", "images": ["mixed-vg-a.img", "mixed-vg-b.img"], "vg_name": "vgmix", "extent_size_sectors": ext_sz, "pv_count": 2}


def gen_luks2(out_dir: Path, exp_dir: Optional[Path], name: str, pbkdf: str) -> Dict:
    img = DiskImage(out_dir / f"{name}.img", 32 * MIB)
    build_luks2_image(img, pbkdf, name)
    img.flush()
    if exp_dir:
        cdir = exp_dir / name
        cdir.mkdir(parents=True, exist_ok=True)
        (cdir / "luks-info.txt").write_text(f"format=LUKS2\npbkdf={pbkdf}\n", encoding="ascii")
    return {"description": f"Opaque LUKS2 container ({pbkdf})", "images": [f"{name}.img"], "has_luks": True}


ALL_CASES = [
    ("linear-1pv", lambda o, e: gen_linear_1pv(o, e)),
    ("linear-1pv-multiseg", lambda o, e: gen_linear_1pv_multiseg(o, e)),
    ("linear-2pv", lambda o, e: gen_linear_2pv(o, e)),
    ("striped-2pv", lambda o, e: gen_striped_2pv(o, e)),
    ("sector4k", lambda o, e: gen_sector4k(o, e)),
    ("odd-geometry", lambda o, e: gen_odd_geometry(o, e)),
    ("thin-pool", lambda o, e: gen_thin_pool(o, e)),
    ("snapshot", lambda o, e: gen_snapshot(o, e)),
    ("raid1-mirror", lambda o, e: gen_raid1_mirror(o, e)),
    ("mixed-vg", lambda o, e: gen_mixed_vg(o, e)),
    ("luks2-default", lambda o, e: gen_luks2(o, e, "luks2-default", "argon2id")),
    ("luks2-pbkdf2", lambda o, e: gen_luks2(o, e, "luks2-pbkdf2", "pbkdf2")),
]


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Generate deterministic LVM2/LUKS2 test disk fixtures.")
    parser.add_argument("--out-dir", type=Path, required=True, help="Output directory for generated .img files.")
    parser.add_argument("--expected-dir", type=Path, default=None, help="Optional directory to write lightweight expected text/hash oracles.")
    parser.add_argument("--manifest", type=Path, default=None, help="Optional path to write corpus-manifest.json.")
    parser.add_argument("--case", action="append", default=None, help="Optional case name filter (repeatable).")
    args = parser.parse_args(argv)

    selected = set(args.case) if args.case else None
    manifest: Dict[str, Dict] = {"cases": {}}
    args.out_dir.mkdir(parents=True, exist_ok=True)

    for name, fn in ALL_CASES:
        if selected is not None and name not in selected:
            continue
        manifest["cases"][name] = fn(args.out_dir, args.expected_dir)

    if args.manifest is not None:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
