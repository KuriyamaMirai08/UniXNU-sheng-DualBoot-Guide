# 小米平板 6S Pro（sheng）Android + iPadOS 27 双系统复刻指南（d8 50G 版）

> 本指南面向已经构建出 UniXNU / iPadOS 27 系统产物（如已按 [UniXNU-sheng-Build-Guide](https://github.com/KuriyamaMirai08/UniXNU-sheng-Build-Guide) 完成构建）的用户，说明如何将其部署为 **A 槽 Android + B 槽 iPadOS 27（50G 持久化）** 双系统。

## 一、准备工作（必须先完成）

1. **解锁 Bootloader**，电脑装好 `adb`、`fastboot`、`python3` 与 C++17 编译器。
2. **完整备份 A 槽原厂镜像与 GPT**（至少包括 `boot_a.img`、`vendor_boot_a.img`、`init_boot_a.img`、`dtbo_a.img` 与分区表）。
3. 准备好你自己的 iPadOS 27 / UniXNU 构建输入：`sheng-uefi`、`sheng-abl.dtb`、`OS`、`SystemOS`、`.vroot`、`scratch-data.img`（48 GiB seed）及其 `seedHash`。

## 二、拉取已验证的 d8 内核源码并跑通主机测试

```bash
git clone https://github.com/KuriyamaMirai08/UniXNU-sheng.git
cd UniXNU-sheng
git checkout dualboot-50g-d8
```

运行官方主机生命周期测试（必须 `151/151 PASS`）：

```bash
python3 scripts/repro/stage0c2e/run-stage0c2e-host.py
```

编译并运行本仓库 `tests/` 下的 `d5`–`d8` 主机 C++ 回归测试，确保全部退出码为 `0`。

## 三、编译与审计 `kernel.macho`，打包 `boot_b`

1. 使用你的工作流构建 `kernel.macho`（`dualboot-50g-d8` 已自带全部 `d8` 补丁）。
2. 执行静态审计：
   ```bash
   python3 tools/audit_macho_d8.py /path/to/kernel.macho
   ```
   确认 `BAR_SYMS=0`、`PANIC_SCREEN_FOUND=True`，且驱动函数中无 `>= 4 KiB` 栈帧及 `___chkstk_darwin` 调用。
3. 打包 `boot_b`（Android `boot` v3）：
   - `kernel` = `gzip(sheng-uefi) + sheng-abl.dtb`
   - `ramdisk` = `176 MiB` FAT32 镜像，包含 `EFI/UniXNU/Payloads/*` 与 `EFI/UniXNU/unixnu.toml`（持久模式 `data-persist=2 data-wal-v2=1 log=0`）。

## 四、制作无 Ramdisk 的 `vendor_boot_b`

ABL 会把 `vendor_boot` 的 ramdisk 拼接在 `boot` 的 ramdisk 之前。如果 `vendor_boot_b` 保留了原厂 ~30 MB Android ramdisk，`sheng-uefi` 将无法挂载 FAT32。使用本仓库工具转换：

```bash
python3 tools/make_vendor_boot_noramdisk.py   --in /path/to/vendor_boot_a.img   --out /path/to/vendor_boot_b-v3-noramdisk.img
```

## 五、划分 `darwin` 分区并初始化 `SHNGROOTv2` + `WALv2`

1. 在 Recovery 中将 `userdata` 尾部缩小，划出名为 `darwin` 的 GPT 分区（`18,874,363` 个 4 KiB 块，约 72 GiB）。
2. 刷入 `SHNGROOTv2`（`OS` + `SystemOS` + `.vroot` 校验表）。
3. 编译运行 `tools/gen_walv2_init.cpp`，传入你的 `scratch-data.img` 的 `seedHash`（64 位十六进制），生成 `WALv2` 初始化稀疏镜像并刷入 `darwin` 分区尾部的 `WALv2` 窗口。

## 六、刷入 B 槽并验证双系统切换

```bash
fastboot flash boot_b boot_b-diag-d8.img
fastboot flash vendor_boot_b vendor_boot_b-v3-noramdisk.img
fastboot set_active b
fastboot reboot
```

- **进入 iPadOS 27**：直接显示苹果 Logo（`d8` 已移除顶部彩条），常量时间快速进入桌面，设置中可见 ~50 GB 容量，开关机数据持久保留。
- **切回 Android**：
  ```bash
  fastboot set_active a
  fastboot reboot
  ```
