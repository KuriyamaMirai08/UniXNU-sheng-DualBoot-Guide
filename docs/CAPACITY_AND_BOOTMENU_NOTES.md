# iOS 存储容量调整与开机图形选单说明

## 1. iOS 存储容量可以改吗？

**可以改，但不能在 iOS 内无损热扩容**（修改容量需要重新生成并初始化 `WALv2` 窗口，会清空当前 iOS `Data` 数据）。

- **当前 50G 的构成**：底层 `darwin` 分区总计 `18,874,363` 个 4 KiB 块（≈ **72 GiB**），前部存放 `OS` + `SystemOS` + `.vroot`，尾部存放 `WALv2`（`12,624,640` 块）。iOS 内显示的容量由 `scratch-data.img`（`48 GiB` = `51,539,607,552` B ≈ 十进制 50 GB）决定，且与 `WALv2` 的 `kPages`（`12,582,912`）和 `seedHash` 强绑定。
- **在现有 72 GiB `darwin` 分区内调整（约 ≤ 55 GiB）**：无需改 GPT 分区表；重新生成对应容量的 APFS seed、`boot_b` 和 `darwin-walv2-init.sparse.img` 即可。
- **大幅扩容（例如 100 GB / 128 GB+）**：需先备份 Android 数据，缩小 `userdata` 并扩大 `darwin` 分区，再重新生成对应大小的 APFS seed、`boot_b` 和 `WALv2` 窗口。

## 2. 免电脑开机切换菜单方向

目前在调研社区开源项目 [tc1911/kShimC4sheng](https://github.com/tc1911/kShimC4sheng)（GPL-3.0）作为免电脑开机选单的可行方向：
- **方向 A（基于 `kShimC4sheng` 改造单槽 `boot_a` 菜单）**：将 `boot_b` 的 FAT32 镜像从 176 MiB 紧缩到 ~104 MiB，使 `原厂 Android 内核 (45.4 MiB) + kShimC (4 MiB) + sheng-uefi+dtb (3.14 MiB) + FAT32 (104 MiB) ≈ 156.5 MiB < 192 MiB`，实现开机直出中文图形菜单、免重启进双系统。
- **方向 B（UEFI 原生切槽菜单）**：在 `boot_b` 的 `sheng-uefi` 内增加轻量切槽菜单与倒计时，保持 `boot_a` 完全原厂纯净。
