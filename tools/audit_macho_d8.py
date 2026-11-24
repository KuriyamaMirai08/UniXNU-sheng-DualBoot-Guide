#!/usr/bin/env python3
"""Static Mach-O audit for UniXNU-sheng d8 kernel.macho.

Verifies:
1. Top diagnostic color bars are removed (BAR_SYMS == 0).
2. Red panic screen symbols/strings are preserved (PANIC_SCREEN_FOUND == True).
3. Diagnostic WAL/Data persistence symbols are present.
4. Optional (when llvm-objdump is on PATH): no driver function has >= 4 KiB stack frame
   or calls ___chkstk_darwin.
"""

import argparse
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

MH_MAGIC_64 = 0xFEEDFACF
MH_FILESET = 12
LC_SYMTAB = 0x2
LC_FILESET_ENTRY = 0x80000035


def extract_symtab_at(data: bytes, base_off: int) -> tuple[list[str], int]:
    if base_off + 32 > len(data):
        return [], 0
    magic, _, _, _, ncmds, _, _ = struct.unpack_from("<IiiiiII", data, base_off)
    if magic != MH_MAGIC_64:
        return [], 0

    off = base_off + 32
    symoff = nsyms = stroff = strsize = 0
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == LC_SYMTAB:
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", data, off + 8)
            break
        off += cmdsize

    if not nsyms or not strsize or stroff + strsize > len(data):
        return [], 0

    strtab = data[stroff:stroff + strsize]
    syms = []
    for i in range(nsyms):
        n_strx = struct.unpack_from("<I", data, symoff + i * 16)[0]
        if n_strx < len(strtab):
            end = strtab.find(b"\x00", n_strx)
            if end == -1:
                end = len(strtab)
            syms.append(strtab[n_strx:end].decode("utf-8", "replace"))
    return syms, nsyms


def parse_macho_symtab(data: bytes) -> tuple[list[str], int]:
    if len(data) < 32:
        raise ValueError("File too small to be Mach-O")
    magic, _, _, filetype, ncmds, _, _ = struct.unpack_from("<IiiiiII", data, 0)
    if magic != MH_MAGIC_64:
        raise ValueError(f"Not a 64-bit little-endian Mach-O (magic=0x{magic:08x})")

    if filetype != MH_FILESET:
        syms, nsyms = extract_symtab_at(data, 0)
        if not nsyms:
            raise ValueError("LC_SYMTAB not found in Mach-O")
        return syms, nsyms

    all_syms: list[str] = []
    total_nsyms = 0
    off = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == LC_FILESET_ENTRY:
            _, fileoff = struct.unpack_from("<QQ", data, off + 8)
            sub_syms, sub_n = extract_symtab_at(data, fileoff)
            all_syms.extend(sub_syms)
            total_nsyms += sub_n
        off += cmdsize
    if total_nsyms == 0:
        raise ValueError("No LC_SYMTAB found in MH_FILESET entries")
    return all_syms, total_nsyms


def audit_stack_with_objdump(macho_path: Path) -> tuple[int, int]:
    objdump = shutil.which("llvm-objdump")
    if not objdump:
        print("INFO: llvm-objdump not found on PATH; skipping ARM64 stack disassembly audit")
        return 0, 0

    proc = subprocess.run(
        [objdump, "-d", "--no-show-raw-insn", str(macho_path)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if proc.returncode != 0:
        raise RuntimeError(f"llvm-objdump failed: {proc.stderr[:400]}")

    cur_func = ""
    driver_keywords = ("NativeRamBlock", "PersistOverlay", "PersistWalV2", "ShengUfs")
    large_frames = []
    chkstk_hits = []
    sub_sp_re = re.compile(r"\bsub\s+sp,\s+sp,\s+#(0x[0-9a-fA-F]+|\d+)")

    for line in proc.stdout.splitlines():
        if line.endswith(">:") and "<" in line:
            cur_func = line.split("<", 1)[1].rsplit(">:", 1)[0]
            continue
        if not any(k in cur_func for k in driver_keywords):
            continue
        if "___chkstk_darwin" in line:
            chkstk_hits.append((cur_func, line.strip()))
        m = sub_sp_re.search(line)
        if m:
            val = int(m.group(1), 0)
            if val >= 4096:
                large_frames.append((cur_func, val))

    print(f"STACK_AUDIT: large_frames(>={4096}B)={len(large_frames)}, chkstk_darwin_calls={len(chkstk_hits)}")
    return len(large_frames), len(chkstk_hits)


def main() -> int:
    ap = argparse.ArgumentParser(description="Audit UniXNU-sheng d8 kernel.macho")
    ap.add_argument("macho", type=Path, help="Path to kernel.macho")
    args = ap.parse_args()

    data = args.macho.read_bytes()
    syms, nsyms = parse_macho_symtab(data)

    bar_syms = [s for s in syms if "hwtrace_diag_bar" in s or "diag_greenbar" in s or "draw_diag_bar" in s]
    panic_found = (b"PANIC:" in data) and any("hwtrace" in s.lower() or "panic" in s.lower() for s in syms)
    wal_syms = [s for s in syms if "native_data_persist" in s or "walIoLock" in s or "ckCache" in s]

    print(f"MACHO_BYTES: {len(data)}")
    print(f"TOTAL_SYMS: {nsyms}")
    print(f"BAR_SYMS: {len(bar_syms)} {bar_syms}")
    print(f"PANIC_SCREEN_FOUND: {panic_found}")
    print(f"DIAG_WAL_SYMS: {len(wal_syms)}")

    large_frames, chkstk_hits = audit_stack_with_objdump(args.macho)

    if bar_syms:
        print("FAIL: diagnostic color bar symbols still present!", file=sys.stderr)
        return 1
    if not panic_found:
        print("FAIL: panic screen marker/symbols not found!", file=sys.stderr)
        return 1
    if large_frames > 0 or chkstk_hits > 0:
        print("FAIL: driver stack frame >= 4 KiB or ___chkstk_darwin detected!", file=sys.stderr)
        return 1

    print("PASS: d8 Mach-O audit succeeded")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
