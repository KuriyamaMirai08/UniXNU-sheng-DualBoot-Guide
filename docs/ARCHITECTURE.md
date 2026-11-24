# Architecture (`d8`)

## 1. A/B Slot Layout

Xiaomi Pad 6S Pro (`sheng`) is an A/B device:
- **Slot A (`_a`)**: Kept completely untouched as stock Android / HyperOS (`boot_a`, `vendor_boot_a`, `init_boot_a`, `dtbo_a`).
- **Slot B (`_b`)**: Dedicated to UniXNU / iPadOS 27:
  - `boot_b` (`boot` v3, `185,733,120` B): gzipped `sheng-uefi` (`3.00 MiB`) + appended `sheng-abl.dtb` (`140 KB`) + `176 MiB` FAT32 ramdisk containing `kernel.macho`, `.adt`, `.gpuaddon`, `.trustcache`, `.vroot`, `scratch-data.img`, and `unixnu.toml`.
  - `vendor_boot_b` (`vendor_boot` v3 noramdisk, `3,891,200` B): derived from stock `vendor_boot_a.img` with `vendor_ramdisk_size = 0` so ABL passes our FAT32 ramdisk cleanly to `sheng-uefi`.

## 2. `darwin` GPT Partition Geometry (~72 GiB)

`darwin` holds `18,874,363` × 4 KiB blocks (≈ 72 GiB / 77.3 GB) carved from `userdata`:

```text
[0 MiB]           SHNGROOTv2 manifest + OS (~9.61 GB) + SystemOS (~6.02 GB) + .vroot tables
[guard + ledger]  1,024-block guard + 16-block Ledger (atomic metadata)
[tail window]     12,624,640 blocks (~48.16 GiB) WALv2 persistent Data store:
                    - Header / backup / checkpoint metadata
                    - 16,380-block WALv2 ring buffer (walEntryCapacity slots)
                    - 48 GiB (12,582,912 pages) persistent Data overlay blocks
```

## 3. Constant-Time Boot & Persistence (`d8`)

- `NativeRamBlock` attaches to `ShengUfs` (`mode=2` RW, `mode=1` RO recovery) using `resumeExistingV2(..., verifyBase=false)`.
- Boot no longer scans the entire 48 GiB overlay on mount (`O(1)` mount time regardless of how much data has been written).
- Runtime reads/writes use `ScratchBuf<>::ckCache` and coalesced `verifyEffectiveUnlocked` with sleepable `walIoLock` (`d7`/`d8`).
