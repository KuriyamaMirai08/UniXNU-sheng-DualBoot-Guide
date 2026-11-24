# UniXNU-sheng DualBoot Guide (Android + iPadOS 27, 50 GB Persistent)

> Verified dual-boot reproduction guide, AI agent playbook, patches, helper tools, and host test suite for **Xiaomi Pad 6S Pro 12.4 (`sheng` / SM8550)** running **Android (Slot A)** + **iPadOS 27 / UniXNU (Slot B)** with **50 GB persistent APFS storage**.

**Target audience**: Users (and their AI coding assistants) who have already built their own UniXNU / iPadOS 27 kernel & root images (for example via [KuriyamaMirai08/UniXNU-sheng-Build-Guide](https://github.com/KuriyamaMirai08/UniXNU-sheng-Build-Guide)) and want a **reproducible, verified A/B dual-boot setup** without hitting the kernel stack, WALv2, `vendor_boot`, or UFS persistence pitfalls we solved across `d1`–`d8`.

## Verified Baseline (`d8`)

| Item | Verified Value |
| --- | --- |
| Source Fork & Tag | [KuriyamaMirai08/UniXNU-sheng](https://github.com/KuriyamaMirai08/UniXNU-sheng) @ tag `dualboot-50g-d8` (`32b196d69c46ebbc87bb2308fed8bd8dc49baf33`) |
| CI Build | GitHub Actions Run `45345094577` (16/16 steps PASS) |
| Slot A / Slot B | Slot A = Stock Android / HyperOS; Slot B = UniXNU / iPadOS 27 |
| `boot_b` Format | Android `boot` v3 (`185,733,120` B): `1.12 MiB` gzipped `sheng-uefi` (`3.00 MiB`) + `140 KB` appended DTB + `176 MiB` FAT32 ramdisk |
| `vendor_boot_b` Format | Android `vendor_boot` v3 **noramdisk** (`3,891,200` B, `ramdisk_size = 0`, stock DTB preserved) |
| `darwin` GPT Partition | `18,874,363` × 4 KiB blocks (≈ 72 GiB), carved from `userdata` tail |
| iOS Storage Capacity | `48 GiB` APFS Data seed (`51,539,607,552` B ≈ 50 GB decimal, `12,582,912` pages) |
| `d8` Runtime Highlights | Constant-time boot (`verifyBase=false` + `ckCache`), no top 13+5 color bars, red Panic screen retained |

## Quick Start

- **For AI Agents**: Point your AI assistant directly to [`AI_REPRO_PLAYBOOK.md`](AI_REPRO_PLAYBOOK.md). It is written as a deterministic, step-by-step execution specification with hard safety gates and verification commands.
- **For Humans**: Read [`docs/STEP_BY_STEP_CN.md`](docs/STEP_BY_STEP_CN.md) (中文完整操作指南) and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Repository Layout

```text
├── AI_REPRO_PLAYBOOK.md              # Deterministic step-by-step spec for AI agents
├── README.md
├── LICENSE                           # MIT (docs, tools, tests); see THIRD_PARTY_NOTICES.md
├── THIRD_PARTY_NOTICES.md
├── docs/
│   ├── ARCHITECTURE.md               # A/B slots, 72 GiB darwin layout, vendor_boot v3 noramdisk, WALv2
│   ├── STEP_BY_STEP_CN.md            # 中文完整复刻指南
│   ├── D1_TO_D8_BUGFIXES.md          # d1–d8 踩坑与修复全记录
│   ├── VERIFICATION_AND_ROLLBACK.md  # 静态审计、主机测试与 fastboot 回退
│   └── CAPACITY_AND_BOOTMENU_NOTES.md# 容量调整说明与开机选单演进路线
├── evidence/
│   ├── baseline-d8.json              # Verified d8 geometry and checksums
│   └── deliverable-receipt-d8.json   # Verified d8 kernel build receipt
├── patches/                          # The 3 d8 patches (already applied at tag dualboot-50g-d8)
├── tests/                            # C++ host regression tests (d5–d8)
└── tools/
    ├── make_vendor_boot_noramdisk.py # Strip Android ramdisk from stock vendor_boot -> v3 noramdisk
    ├── audit_macho_d8.py             # Audit kernel.macho (no bars, panic screen ok, 0 >=4KiB frames)
    └── gen_walv2_init.cpp            # Initialize 48 GiB WALv2 persistence window from seedHash
```

## Disclaimer

This repository contains **zero proprietary Apple or Xiaomi binaries**, **zero private keys**, and **zero device-specific identifiers**. Modifying GPT partitions and flashing `boot`/`vendor_boot` carries brick risk—always back up Slot A and GPT before touching your device.
