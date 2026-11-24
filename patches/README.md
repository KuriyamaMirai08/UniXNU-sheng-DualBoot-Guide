# d8 Patches

These three patches sit on top of the `stage0c3-deliverable` baseline in `KuriyamaMirai08/UniXNU-sheng` and are already applied at tag `dualboot-50g-d8` (`32b196d69c46ebbc87bb2308fed8bd8dc49baf33`):

1. `sheng-stage0c3-diag-ramblock-stack.patch`:
   - Moves 4 KiB buffers off ARM64 kernel stack in `PersistOverlay` / `PersistWalV2` (`d4`).
   - Adds WAL v2 full-table `entryFind` 2x hash capacity + `spaceExhausted` checkpoint (`d6`).
   - Adds sleepable `walIoLock`, incremental/batched checkpoint, and CRC32/CRC64 integrity (`d7`).
   - Enables constant-time boot (`verifyBase=false`) + runtime `ScratchBuf<>::ckCache` + coalesced `verifyEffectiveUnlocked` (`d8`).
2. `sheng-stage0c3-diag-ufs-sync-range.patch`:
   - Syncs WAL v2 without forcing a full checkpoint on every flush, narrows UFS `SYNC CACHE(10)` range (`d5`).
3. `sheng-stage0c3-diag-bars.patch`:
   - Preserves `arm_vm_init.c` `phystokv` VA fix, `start.s` TTBR1[0] framebuffer mapping, `sleh.c`, and `hwtrace.c` red panic screen with PC/LR/FAR/ESR + backtrace (`d1`-`d3`), while removing the top 13+5 diagnostic color bars (`d8`).
