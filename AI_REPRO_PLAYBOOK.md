# AI Reproduction Playbook — UniXNU-sheng DualBoot (`d8`)

This document is a **deterministic execution specification** for an AI agent helping a user reproduce the verified **Xiaomi Pad 6S Pro (`sheng` / SM8550) Android (Slot A) + iPadOS 27 (Slot B, 50 GB persistent)** dual-boot setup.

> **Hard Safety Rules for the AI Agent**
> 1. **NEVER flash or repartition** until you have verified the user holds local backups of stock `boot_a.img`, `vendor_boot_a.img`, `init_boot_a.img`, `dtbo_a.img`, and the stock GPT table.
> 2. **NEVER commit or upload** private signing keys (`*.b64` / `*.key`), IPSW/DMG/AEA files, `kernel.macho`, `.img` files, or device serial numbers.
> 3. **Do NOT skip `vendor_boot_b` noramdisk or `WALv2` initialization**—skipping either causes immediate boot failure or data mount panic.

---

## Phase 0: Prerequisite Check (Ask/Verify User Inputs)

Confirm the user already has:
1. Unlocked Xiaomi Pad 6S Pro (`sheng`) with working `adb` and `fastboot`.
2. Stock Slot-A backups (`boot_a.img`, `vendor_boot_a.img`, `init_boot_a.img`, `dtbo_a.img`) + GPT backup.
3. Their own built UniXNU / iPadOS 27 deliverables (from `UniXNU-sheng` or `UniXNU-sheng-Build-Guide`):
   - `sheng-uefi` + `sheng-abl.dtb`
   - `OS` / `SystemOS` root images + `.vroot` hash tables + `SHNGROOTv2` manifest
   - `scratch-data.img` (48 GiB = `51,539,607,552` B APFS Data seed) and its 32-byte `seedHash` (hex)
   - `kernel.macho`, `.adt`, `.gpuaddon`, `.trustcache`, `.vroot`

---

## Phase 1: Clone Verified Kernel Source (`dualboot-50g-d8`)

```bash
git clone https://github.com/KuriyamaMirai08/UniXNU-sheng.git
cd UniXNU-sheng
git checkout dualboot-50g-d8
# Expected commit: 32b196d69c46ebbc87bb2308fed8bd8dc49baf33
```

The three patches in `patches/` of this guide repo are already applied at `dualboot-50g-d8`. Inspect `scripts/repro/stage0c2e/run-stage0c2e.py` and `.github/workflows/sheng-stage0c2a-r.yml` to see how the CI pipeline wires them.

---

## Phase 2: Run Host Verification Tests (Must Pass 100%)

1. Run the official lifecycle suite:
   ```bash
   python3 scripts/repro/stage0c2e/run-stage0c2e-host.py
   # Expected: wal-v2-lifecycle 151/151 PASS, unacceptable_corrupt_results=0
   ```
2. Compile and run the C++ host regression tests from `tests/` against `PersistOverlay.h`:
   ```bash
   for t in test_d5_negative test_ufs_sync_negative test_d6_full_wal test_d7_verify test_d8_verify; do
     c++ -std=c++17 -O2 -I<path-to-PersistOverlay-dir> tests/${t}.cpp -o /tmp/${t} && /tmp/${t}
   done
   ```
   All 5 binaries must exit `0`.

---

## Phase 3: Build & Audit `kernel.macho`, Then Pack `boot_b`

1. Build `kernel.macho` using the user's UniXNU workflow (with `d8` patches active).
2. Audit `kernel.macho` using `tools/audit_macho_d8.py`:
   ```bash
   python3 tools/audit_macho_d8.py /path/to/kernel.macho
   ```
   Verify output reports:
   - `BAR_SYMS: 0` (top 13+5 color bars removed)
   - `PANIC_SCREEN_FOUND: True` (red panic screen retained)
   - If `llvm-objdump` is present: `>= 4 KiB` driver stack frames `= 0` and `bl ___chkstk_darwin = 0`
3. Pack `boot_b` as **Android `boot` v3** (`page_size = 4096`):
   - `kernel` slot: `gzip(sheng-uefi)` + `sheng-abl.dtb` appended directly after the gzip stream.
   - `ramdisk` slot: FAT32 image (`176 MiB` = `184,549,376` B) containing:
     - `EFI/UniXNU/Payloads/kernel.macho`
     - `EFI/UniXNU/Payloads/kernel.macho.adt`
     - `EFI/UniXNU/Payloads/kernel.macho.gpuaddon`
     - `EFI/UniXNU/Payloads/kernel.macho.trustcache`
     - `EFI/UniXNU/Payloads/kernel.macho.vroot`
     - `EFI/UniXNU/Payloads/scratch-data.img`
     - `EFI/UniXNU/unixnu.toml` (with `data-persist=2 data-wal-v2=1 log=0` for persistent entry, and `data-persist=1 data-wal-v2=1 log=0` for read-only recovery entry).

---

## Phase 4: Build `vendor_boot_b-v3-noramdisk.img`

**Why**: Android Bootloader (ABL) concatenates `vendor_boot`'s ramdisk before `boot`'s ramdisk. Stock `vendor_boot_a.img` contains a ~30 MB Android ramdisk that prevents `sheng-uefi` from mounting the FAT32 ramdisk.

Run:
```bash
python3 tools/make_vendor_boot_noramdisk.py   --in /path/to/stock/vendor_boot_a.img   --out /path/to/out/vendor_boot_b-v3-noramdisk.img
```
Verify: output is `VNDRBOOT` v3 with `vendor_ramdisk_size = 0` and identical DTB payload.

---

## Phase 5: Partition `darwin` & Initialize `SHNGROOTv2` + `WALv2`

1. In Recovery / parted (with Android data backed up if resizing `userdata`), carve a GPT partition named `darwin` of `18,874,363` × 4 KiB blocks (≈ 72 GiB) from the tail of `userdata`.
2. Flash `SHNGROOTv2` (`OS` + `SystemOS` + `.vroot` hash tables) to the start of `darwin`.
3. Compile and run `tools/gen_walv2_init.cpp` with the user's 64-hex-char `seedHash` for `scratch-data.img`:
   ```bash
   c++ -std=c++17 -O2 -I<path-to-PersistOverlay-dir> tools/gen_walv2_init.cpp -o /tmp/gen_walv2_init
   /tmp/gen_walv2_init <seedHash_64hex> /tmp/walv2-blocks.bin
   ```
   `gen_walv2_init` generates the sparse non-zero initialization blocks for the `12,624,640`-block `WALv2` window (`kPages = 12582912`, `kWalBlocks = 16380`) and self-tests both RO and RW resume (`Resumed checkpointCapable=1`).
4. Convert `/tmp/walv2-blocks.bin` into an Android sparse image (`darwin-walv2-init.sparse.img`) targeting the exact `WALv2` tail window offset of `darwin` (`wal_first_lba` relative to `darwin` start), and flash it to `darwin`.

---

## Phase 6: Flash Slot B & Verify Both Systems

```bash
fastboot flash boot_b /path/to/boot_b-diag-d8.img
fastboot flash vendor_boot_b /path/to/vendor_boot_b-v3-noramdisk.img
fastboot set_active b
fastboot reboot
```

- **Expected iPadOS 27 boot**: Apple logo appears directly (no top color bars in `d8`), boots quickly in constant time to iPadOS 27 Desktop, Settings shows ~50 GB capacity, files/settings survive reboots.
- **Rollback / Switch back to Android**:
  ```bash
  fastboot set_active a
  fastboot reboot
  ```

---

## Stop Conditions (Do Not Guess Past Failures)

If any of these occur, stop and consult [`docs/D1_TO_D8_BUGFIXES.md`](docs/D1_TO_D8_BUGFIXES.md):
- Immediate reset before Apple logo -> check `arm_vm_init.c` `phystokv` fix & `start.s` TTBR1[0] FB mapping.
- Red panic screen showing `offline scratch Data mount failed` -> check `WALv2` init (`seedHash` mismatch or wrong `darwin` offset) or `vendor_boot_b` still having a non-zero ramdisk.
- Hang on Apple logo after >100 MB writes -> confirm `d6`/`d7`/`d8` patches (`walIoLock`, 2x hash table, `verifyBase=false`, `ckCache`) are all present.
