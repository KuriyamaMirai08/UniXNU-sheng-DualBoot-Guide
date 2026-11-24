#!/usr/bin/env python3
"""Convert a stock Android vendor_boot (v3 or v4) image into a v3 noramdisk image.

ABL concatenates vendor_boot's ramdisk ahead of boot's ramdisk. Keeping the stock
Android vendor_ramdisk (~30 MiB) prevents sheng-uefi from mounting the FAT32
ramdisk packed in boot_b. This tool preserves the stock DTB and cmdline while
setting vendor_ramdisk_size = 0 in a clean v3 VNDRBOOT header.
"""

import argparse
import hashlib
import struct
from pathlib import Path

VNDRBOOT_MAGIC = b"VNDRBOOT"
V3_HEADER_SIZE = 2112


def align_up(value: int, page_size: int) -> int:
    return ((value + page_size - 1) // page_size) * page_size


def convert_to_v3_noramdisk(src_path: Path, dst_path: Path) -> None:
    raw = src_path.read_bytes()
    if len(raw) < V3_HEADER_SIZE or raw[:8] != VNDRBOOT_MAGIC:
        raise ValueError(f"{src_path} is not a valid VNDRBOOT image")

    ver, page_size, kernel_addr, ramdisk_addr, ramdisk_size = struct.unpack_from("<IIIII", raw, 8)
    if ver not in (3, 4):
        raise ValueError(f"Unsupported vendor_boot header version: {ver}")
    if page_size == 0 or (page_size & (page_size - 1)) != 0:
        raise ValueError(f"Invalid page_size: {page_size}")

    cmdline = raw[28:2076]
    tags_addr = struct.unpack_from("<I", raw, 2076)[0]
    name = raw[2080:2096]
    hdr_size, dtb_size, dtb_addr = struct.unpack_from("<IIQ", raw, 2096)

    if dtb_size == 0:
        raise ValueError("Source vendor_boot has dtb_size == 0")

    hdr_pages_bytes = align_up(hdr_size, page_size)
    ramdisk_pages_bytes = align_up(ramdisk_size, page_size)
    dtb_offset = hdr_pages_bytes + ramdisk_pages_bytes
    if dtb_offset + dtb_size > len(raw):
        raise ValueError("DTB region exceeds input file size")

    dtb_bytes = raw[dtb_offset:dtb_offset + dtb_size]
    dtb_sha = hashlib.sha256(dtb_bytes).hexdigest()

    out_hdr = bytearray(page_size)
    out_hdr[0:8] = VNDRBOOT_MAGIC
    struct.pack_into("<IIIII", out_hdr, 8, 3, page_size, kernel_addr, ramdisk_addr, 0)
    out_hdr[28:2076] = cmdline
    struct.pack_into("<I", out_hdr, 2076, tags_addr)
    out_hdr[2080:2096] = name
    struct.pack_into("<IIQ", out_hdr, 2096, V3_HEADER_SIZE, dtb_size, dtb_addr)

    dtb_padded_len = align_up(dtb_size, page_size)
    out_buf = bytes(out_hdr) + dtb_bytes + bytes(dtb_padded_len - dtb_size)

    dst_path.parent.mkdir(parents=True, exist_ok=True)
    dst_path.write_bytes(out_buf)

    # Round-trip verification
    check = dst_path.read_bytes()
    c_ver, c_psize, _, _, c_rsize = struct.unpack_from("<IIIII", check, 8)
    c_hsize, c_dtb_size, _ = struct.unpack_from("<IIQ", check, 2096)
    c_dtb_sha = hashlib.sha256(check[c_psize:c_psize + c_dtb_size]).hexdigest()
    assert c_ver == 3 and c_rsize == 0 and c_hsize == V3_HEADER_SIZE and c_dtb_sha == dtb_sha

    print(f"OK: {dst_path} ({len(out_buf)} bytes, v3 noramdisk, dtb={dtb_size} bytes, dtb_sha256={dtb_sha})")


def main() -> None:
    ap = argparse.ArgumentParser(description="Strip vendor_ramdisk from stock vendor_boot -> v3 noramdisk")
    ap.add_argument("--in", dest="src", required=True, type=Path, help="Input stock vendor_boot_a.img (v3/v4)")
    ap.add_argument("--out", dest="dst", required=True, type=Path, help="Output vendor_boot_b-v3-noramdisk.img")
    args = ap.parse_args()
    convert_to_v3_noramdisk(args.src, args.dst)


if __name__ == "__main__":
    main()
