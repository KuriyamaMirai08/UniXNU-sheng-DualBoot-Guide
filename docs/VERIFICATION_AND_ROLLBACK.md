# 验证与回退规范

## 1. 上机前必检项

1. **主机测试全绿**：
   - `run-stage0c2e-host.py`：`wal-v2-lifecycle` `151/151 PASS`，`unacceptable_corrupt_results=0`。
   - `tests/test_d5_negative.cpp` ~ `tests/test_d8_verify.cpp` 全部通过。
2. **`kernel.macho` 静态审计**：
   ```bash
   python3 tools/audit_macho_d8.py /path/to/kernel.macho
   ```
   确认 `BAR_SYMS=0`、`PANIC_SCREEN_FOUND=True`、驱动函数 `>=4 KiB` 栈帧为 `0`。
3. **`vendor_boot_b` 校验**：
   确认 `header_version = 3` 且 `vendor_ramdisk_size = 0`。

## 2. 双系统切换与安全回退

- **切换到 iPadOS 27（B 槽）**：
  ```bash
  fastboot set_active b
  fastboot reboot
  ```
- **切回 Android（A 槽）**：
  ```bash
  fastboot set_active a
  fastboot reboot
  ```
- **如果误刷坏 B 槽**：只需 `fastboot set_active a && fastboot reboot` 即可立即回到完好无损的原厂 Android。
