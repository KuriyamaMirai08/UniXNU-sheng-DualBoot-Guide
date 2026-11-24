#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "DirectDisk.h"

// Sparse 48 GiB medium so we can run exact 12,582,912-page PersistOverlay/WalV2
// operations on host without allocating 51.7 GB of RAM.
struct SparseMediumOps {
  uint64_t totalBlocks = 0;
  std::unordered_map<uint64_t, std::vector<uint8_t>> blocks;
  uint64_t readCalls = 0;
  uint64_t readBlocks = 0;
  uint64_t writeCalls = 0;
  uint64_t writeBlocks = 0;
  uint64_t syncCalls = 0;

  void resetCounters() {
    readCalls = readBlocks = writeCalls = writeBlocks = syncCalls = 0;
  }

  bool read(uint64_t block, uint32_t count, void *dst) {
    if (block > totalBlocks || count > totalBlocks - block) return false;
    ++readCalls;
    readBlocks += count;
    uint8_t *out = static_cast<uint8_t *>(dst);
    for (uint32_t i = 0; i < count; ++i) {
      auto it = blocks.find(block + i);
      if (it != blocks.end()) {
        memcpy(out + size_t(i) * persist::kBlock, it->second.data(), persist::kBlock);
      } else {
        memset(out + size_t(i) * persist::kBlock, 0, persist::kBlock);
      }
    }
    return true;
  }

  bool write(uint64_t block, uint32_t count, const void *src) {
    if (block > totalBlocks || count > totalBlocks - block) return false;
    ++writeCalls;
    writeBlocks += count;
    const uint8_t *in = static_cast<const uint8_t *>(src);
    for (uint32_t i = 0; i < count; ++i) {
      auto &slot = blocks[block + i];
      if (slot.empty()) slot.resize(persist::kBlock);
      memcpy(slot.data(), in + size_t(i) * persist::kBlock, persist::kBlock);
    }
    return true;
  }

  bool sync() {
    ++syncCalls;
    return true;
  }
};

// Model of hostSyncCache(UfsWriteRole::Data) BEFORE fix:
static uint64_t countSyncCommandsBeforeFix(uint64_t windowBlocks) {
  uint64_t cmds = 0;
  uint64_t rem = windowBlocks;
  while (rem) {
    uint32_t chunk = rem > 0xffff ? 0xffff : static_cast<uint32_t>(rem);
    ++cmds;
    rem -= chunk;
  }
  return cmds;
}

int main() {
  constexpr uint64_t kPages = (48ULL << 30) / persist::kBlock;  // 12,582,912
  constexpr uint64_t kWalBlocks = 16380;
  uint64_t windowBlocks = 0;
  assert(persist::wal::windowBlocksFor(kPages, kWalBlocks, &windowBlocks));
  assert(windowBlocks == 12624640);

  const uint64_t syncCmdsPerCall = countSyncCommandsBeforeFix(windowBlocks);
  std::printf("48 GiB Geometry: pages=%llu, walBlocks=%llu, windowBlocks=%llu\n",
              (unsigned long long)kPages, (unsigned long long)kWalBlocks,
              (unsigned long long)windowBlocks);
  std::printf("BEFORE-fix hostSyncCache(Data) SCSI SYNCHRONIZE CACHE(10) cmds per sync() = %llu\n",
              (unsigned long long)syncCmdsPerCall);
  assert(syncCmdsPerCall == 193);

  uint64_t bitmapCount = 0;
  assert(persist::bitmapBlocksFor(kPages, &bitmapCount));
  std::vector<uint8_t> bitmap(bitmapCount * persist::kBlock, 0);
  const uint32_t entryCap = uint32_t((kWalBlocks / persist::wal::kTransactionBlocks) *
                                     persist::wal::kPagesPerTransaction);
  std::vector<persist::wal::Entry> entries(entryCap);
  uint8_t seed[32] = {};
  for (int i = 0; i < 32; ++i) seed[i] = uint8_t(0xA0 + i);

  SparseMediumOps ops;
  ops.totalBlocks = windowBlocks;

  persist::Overlay ov;
  assert(ov.open(ops, kPages, seed, false, bitmap.data(), true, true,
                 entries.data(), entryCap, kWalBlocks) == persist::Open::Fresh);

  // Measure BEFORE-fix: 10 x 32 KiB writes + 10 x doSynchronizeCache (which calls ov.flush(ops))
  ops.resetCounters();
  std::vector<uint8_t> pageBuf(8 * persist::kBlock, 0x5A);
  for (int t = 0; t < 10; ++t) {
    assert(ov.writePages(ops, uint64_t(t) * 8, 8, pageBuf.data()));
    assert(ov.commitRun(ops, uint64_t(t) * 8, 8));
    // Current NativeRamBlock::doSynchronizeCache() calls flushStore -> overlay.flush():
    assert(ov.flush(ops));
  }

  const uint64_t totalSyncCmds = ops.syncCalls * syncCmdsPerCall;
  const uint64_t totalScsiCmds = ops.writeCalls + totalSyncCmds;
  std::printf("BEFORE-fix 10 x (32KiB write + synchronizeCache):\n");
  std::printf("  ops.write calls = %llu (%llu blocks = %.2f MiB)\n",
              (unsigned long long)ops.writeCalls,
              (unsigned long long)ops.writeBlocks,
              double(ops.writeBlocks) * 4096.0 / (1024.0 * 1024.0));
  std::printf("  ops.sync  calls = %llu (-> %llu SCSI SYNCHRONIZE CACHE cmds)\n",
              (unsigned long long)ops.syncCalls, (unsigned long long)totalSyncCmds);
  std::printf("  TOTAL SCSI write+sync cmds = %llu\n", (unsigned long long)totalScsiCmds);
  for (double ms : {0.2, 0.5, 1.0}) {
    std::printf("  Simulated @ %.1f ms/cmd: %.2f seconds (%.2f min)\n",
                ms, totalScsiCmds * ms / 1000.0, totalScsiCmds * ms / 60000.0);
  }
  return 0;
}
