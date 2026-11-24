# d1–d8 踩坑与修复全记录

从首次点亮屏幕到实现 **d8（无彩条 + 常量时间快速启动 + 50G 稳定持久化）**，我们依次定位并解决了以下关键问题：

| 阶段 | 现象 / 问题 | 根因 | 最终修复 |
| --- | --- | --- | --- |
| **d1 / d2** | 亮屏前直接复位重启 | `arm_vm_init.c` 在 TTBR1 切换前对物理地址误用了 `phystokv`；帧缓冲在 TTBR1[0] 未映射 | 修复 `arm_vm_init.c` `phystokv` VA 映射 + `start.s` 建立 TTBR1[0] FB 映射 |
| **d3** | 崩溃无现场信息，只能盲猜 | 异常路径未向帧缓冲输出寄存器与回溯 | `sleh.c` / `hwtrace.c` 实现红色 Panic 屏（打印 PC/LR/FAR/ESR 与栈回溯） |
| **d4** | 内核初始化持久化时栈溢出崩溃 | `PersistOverlay` / `PersistWalV2` 在 ARM64 内核栈上分配 4 KiB 局部缓冲区，触发 `___chkstk_darwin` | 将大缓冲区移出内核栈，Mach-O 审计确认 `>=4 KiB` 栈帧为 `0`、`chkstk=0` |
| **d5** | 大量写入后 `sync` 极慢/超时 | 每次刷盘都触发整表 checkpoint 且 UFS `SYNC CACHE(10)` 范围过大 | 允许不触发全量 checkpoint 的 WAL v2 `sync()`，收窄 UFS sync LBA 范围 |
| **d6** | WAL 写满后开机卡死在苹果 Logo | 全量 WAL 下 `entryFind` 哈希查找遗漏；哈希表容量偏小；`spaceExhausted` 未及时触发 checkpoint | 修复 `entryFind` 查找逻辑，哈希容量翻倍，补充 `spaceExhausted` checkpoint |
| **d7** | 写入约 100 MB+ 后重启卡在 Logo | `walIoLock` 自旋/不可抢占导致 I/O 阻塞；checkpoint 一次性阻塞过久 | 改用可睡眠 `walIoLock`、分批增量 checkpoint 及表 CRC32/64 校验，**首次完整进入桌面并识别 50G** |
| **d8** | 随着写入增多开机变慢；顶部有 13+5 彩条 | 启动挂载全盘校验 `verifyBase=true` 呈线性退化；早期诊断彩条仍开启 | `verifyBase=false` 常量时间启动 + 运行时 `ScratchBuf<>::ckCache`；移除顶部彩条，保留 `phystokv` 修复与红色 Panic 屏 |
