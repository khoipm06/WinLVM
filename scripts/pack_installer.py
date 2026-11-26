#!/usr/bin/env python3
"""Portable self-extracting installer packager for WinLVM.

Appends a deterministic, integrity-checked trailer archive (`WLVMINST` v1)
onto `winlvm_setup_stub.exe` to produce the standalone `winlvm-setup-x64.exe`.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
import zlib
from pathlib import Path

ARCHIVE_MAGIC = b"WLVMINST"
ARCHIVE_VERSION = 1
ENTRY_MAGIC = b"WLE1"
ENTRY_HEADER_STRUCT = struct.Struct("<4sHHQQII32s")
FOOTER_STRUCT = struct.Struct("<8sIIQQ32s")

COMP_STORE = 0
COMP_DEFLATE = 1

MAX_ENTRIES = 4096
MAX_ENTRY_BYTES = 256 * 1024 * 1024
MAX_TOTAL_UNPACKED = 512 * 1024 * 1024

REQUIRED_RELEASE_FILES = (
    "driver/winlvm.sys",
    "driver/winlvm.inf",
    "driver/winlvm.cat",
    "driver/vol/winlvm-vol.inf",
    "driver/vol/winlvm-vol.cat",
    "bin/lvm.exe",
    "bin/lvmdump.exe",
    "bin/winlvm_svc.exe",
    "bin/winlvm_gui.exe",
    "winlvm-uninstall.exe",
    "LICENSE",
    "NOTICE",
)

WINDOWS_RESERVED = {
    "con",
    "prn",
    "aux",
    "nul",
    *(f"com{i}" for i in range(1, 10)),
    *(f"lpt{i}" for i in range(1, 10)),
}


def validate_rel_path(rel_path: str) -> None:
    if not rel_path or len(rel_path.encode("utf-8")) > 1024:
        raise ValueError(f"invalid entry path length: {rel_path!r}")
    if rel_path.startswith("/") or rel_path.endswith("/"):
        raise ValueError(f"entry path must be relative: {rel_path!r}")
    for ch in rel_path:
        code = ord(ch)
        if code < 0x20 or code == 0x7F or ch in '\\:<>"|?*':
            raise ValueError(f"forbidden character {ch!r} in path: {rel_path!r}")
    for seg in rel_path.split("/"):
        if not seg or seg in (".", ".."):
            raise ValueError(f"invalid path segment in: {rel_path!r}")
        if seg.endswith(".") or seg.endswith(" "):
            raise ValueError(f"segment ends with dot or space: {rel_path!r}")
        stem = seg.split(".", 1)[0].lower()
        if stem in WINDOWS_RESERVED:
            raise ValueError(f"reserved Windows device name in path: {rel_path!r}")


def deflate_raw(raw: bytes, level: int = 9) -> bytes:
    comp = zlib.compressobj(level=level, method=zlib.DEFLATED, wbits=-15)
    return comp.compress(raw) + comp.flush()


def collect_stage_files(stage_dir: Path) -> list[tuple[str, Path]]:
    if not stage_dir.is_dir():
        raise FileNotFoundError(f"stage directory not found: {stage_dir}")

    entries: list[tuple[str, Path]] = []
    seen_lower: set[str] = set()
    for root, dirs, files in os.walk(stage_dir, followlinks=False):
        dirs.sort()
        files.sort()
        root_path = Path(root)
        for dname in list(dirs):
            dpath = root_path / dname
            if dpath.is_symlink():
                raise ValueError(f"refusing to pack symlink directory: {dpath}")
        for fname in files:
            fpath = root_path / fname
            if fpath.is_symlink():
                raise ValueError(f"refusing to pack symlink file: {fpath}")
            if not fpath.is_file():
                continue
            rel = fpath.relative_to(stage_dir).as_posix()
            validate_rel_path(rel)
            low = rel.lower()
            if low in seen_lower:
                raise ValueError(f"case-insensitive duplicate path: {rel}")
            seen_lower.add(low)
            entries.append((rel, fpath))

    entries.sort(key=lambda item: item[0])
    if not entries:
        raise ValueError(f"stage directory contains no files: {stage_dir}")
    if len(entries) > MAX_ENTRIES:
        raise ValueError(f"too many staged files ({len(entries)} > {MAX_ENTRIES})")
    return entries


def pack_installer(
    stub_path: Path,
    stage_dir: Path,
    out_path: Path,
    compress: bool = True,
    strict: bool = False,
) -> tuple[int, int, str]:
    if not stub_path.is_file():
        raise FileNotFoundError(f"stub executable not found: {stub_path}")

    stub_bytes = stub_path.read_bytes()
    entries = collect_stage_files(stage_dir)
    if strict:
        present = {rel.lower() for rel, _ in entries}
        missing = [req for req in REQUIRED_RELEASE_FILES if req.lower() not in present]
        if missing:
            raise ValueError(
                "strict release staging missing required files: " + ", ".join(missing)
            )

    payload_offset = len(stub_bytes)
    whole_sha = hashlib.sha256()
    total_raw = 0
    payload_size = 0

    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = out_path.with_suffix(out_path.suffix + ".tmp")

    try:
        with tmp_path.open("wb") as fh:
            fh.write(stub_bytes)

            for rel, fpath in entries:
                raw = fpath.read_bytes()
                raw_size = len(raw)
                if raw_size > MAX_ENTRY_BYTES:
                    raise ValueError(f"entry exceeds max size: {rel} ({raw_size} bytes)")
                total_raw += raw_size
                if total_raw > MAX_TOTAL_UNPACKED:
                    raise ValueError("total unpacked archive size exceeds limit")

                raw_crc32 = zlib.crc32(raw) & 0xFFFFFFFF
                raw_sha256 = hashlib.sha256(raw).digest()
                path_bytes = rel.encode("utf-8")

                method = COMP_STORE
                stored = raw
                if compress and raw_size > 0:
                    deflated = deflate_raw(raw, level=9)
                    if len(deflated) < raw_size:
                        method = COMP_DEFLATE
                        stored = deflated

                hdr = ENTRY_HEADER_STRUCT.pack(
                    ENTRY_MAGIC,
                    len(path_bytes),
                    method,
                    raw_size,
                    len(stored),
                    raw_crc32,
                    0,
                    raw_sha256,
                )
                fh.write(hdr)
                fh.write(path_bytes)
                fh.write(stored)

                whole_sha.update(hdr)
                whole_sha.update(path_bytes)
                whole_sha.update(stored)
                payload_size += len(hdr) + len(path_bytes) + len(stored)

            footer_prefix = struct.pack(
                "<8sIIQQ",
                ARCHIVE_MAGIC,
                ARCHIVE_VERSION,
                len(entries),
                payload_offset,
                payload_size,
            )
            whole_sha.update(footer_prefix)
            archive_digest = whole_sha.digest()
            fh.write(footer_prefix)
            fh.write(archive_digest)

        verify_installer(tmp_path)
        os.replace(tmp_path, out_path)
    except Exception:
        if tmp_path.exists():
            tmp_path.unlink()
        raise

    return len(entries), out_path.stat().st_size, archive_digest.hex()


def verify_installer(exe_path: Path) -> tuple[int, str]:
    data = exe_path.read_bytes()
    if len(data) < FOOTER_STRUCT.size:
        raise ValueError("file smaller than archive footer")

    footer_off = len(data) - FOOTER_STRUCT.size
    magic, version, count, payload_offset, payload_size, expected_sha = (
        FOOTER_STRUCT.unpack_from(data, footer_off)
    )
    if magic != ARCHIVE_MAGIC:
        raise ValueError(f"invalid archive magic: {magic!r}")
    if version != ARCHIVE_VERSION:
        raise ValueError(f"unsupported archive version: {version}")
    if count == 0 or count > MAX_ENTRIES:
        raise ValueError(f"invalid entry count: {count}")
    if payload_offset > footer_off or payload_size != footer_off - payload_offset:
        raise ValueError("payload offset/size inconsistent with file size")

    whole_sha = hashlib.sha256()
    whole_sha.update(data[payload_offset : payload_offset + payload_size])
    whole_sha.update(data[footer_off : footer_off + 32])
    if whole_sha.digest() != expected_sha:
        raise ValueError("whole-archive SHA-256 mismatch")

    cursor = payload_offset
    end = payload_offset + payload_size
    seen: set[str] = set()
    for _ in range(count):
        if end - cursor < ENTRY_HEADER_STRUCT.size:
            raise ValueError("truncated entry header")
        (
            emagic,
            path_len,
            method,
            raw_size,
            stored_size,
            raw_crc32,
            flags,
            raw_sha256,
        ) = ENTRY_HEADER_STRUCT.unpack_from(data, cursor)
        if emagic != ENTRY_MAGIC or flags != 0:
            raise ValueError("corrupt entry header")
        cursor += ENTRY_HEADER_STRUCT.size
        if end - cursor < path_len + stored_size:
            raise ValueError("truncated entry path or payload")
        rel = data[cursor : cursor + path_len].decode("utf-8")
        validate_rel_path(rel)
        low = rel.lower()
        if low in seen:
            raise ValueError(f"duplicate entry path: {rel}")
        seen.add(low)
        cursor += path_len
        stored = data[cursor : cursor + stored_size]
        cursor += stored_size

        if method == COMP_STORE:
            raw = stored
        elif method == COMP_DEFLATE:
            raw = zlib.decompress(stored, wbits=-15)
        else:
            raise ValueError(f"unsupported compression method {method} for {rel}")
        if len(raw) != raw_size:
            raise ValueError(f"raw size mismatch for {rel}")
        if (zlib.crc32(raw) & 0xFFFFFFFF) != raw_crc32:
            raise ValueError(f"CRC-32 mismatch for {rel}")
        if hashlib.sha256(raw).digest() != raw_sha256:
            raise ValueError(f"SHA-256 mismatch for {rel}")

    if cursor != end:
        raise ValueError("unexpected trailing bytes in payload region")
    return count, expected_sha.hex()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Pack staged WinLVM artifacts into a standalone self-extracting installer."
    )
    parser.add_argument("--stub", type=Path, help="Path to winlvm_setup_stub.exe")
    parser.add_argument("--stage-dir", type=Path, help="Directory with staged payload files")
    parser.add_argument("--out", type=Path, help="Output path for winlvm-setup-x64.exe")
    parser.add_argument(
        "--no-compress",
        action="store_true",
        help="Store payload entries uncompressed",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Require all standard WinLVM release files to be present in --stage-dir",
    )
    parser.add_argument(
        "--verify-only",
        type=Path,
        help="Verify an existing packed installer executable and exit",
    )
    args = parser.parse_args(argv)

    if args.verify_only is not None:
        count, digest = verify_installer(args.verify_only)
        print(f"OK: {args.verify_only} ({count} entries, sha256={digest})")
        return 0

    if args.stub is None or args.stage_dir is None or args.out is None:
        parser.error("--stub, --stage-dir, and --out are required unless --verify-only is used")

    count, total_size, digest = pack_installer(
        stub_path=args.stub,
        stage_dir=args.stage_dir,
        out_path=args.out,
        compress=not args.no_compress,
        strict=args.strict,
    )
    print(
        f"Packed {count} entries into {args.out} ({total_size} bytes, sha256={digest})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
