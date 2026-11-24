#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "DirectDisk.h"

struct SparseMediumOps {
  uint64_t totalBlocks = 0;
  std::unordered_map<uint64_t, std::vector<uint8_t>> blocks;
  uint64_t writeCalls = 0, writeBlocks = 0, syncCalls = 0;

  void resetCounters() { writeCalls = writeBlocks = syncCalls = 0; }

  bool read(uint64_t block, uint32_t count, void *dst) {
    if (block > totalBlocks || count > totalBlocks - block) return false;
    uint8_t *out = static_cast<uint8_t *>(dst);
    for (uint32_t i = 0; i < count; ++i) {
      auto it = blocks.find(block + i);
      if (it != blocks.end()) memcpy(out + size_t(i) * persist::kBlock, it->second.data(), persist::kBlock);
      else memset(out + size_t(i) * persist::kBlock, 0, persist::kBlock);
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

  bool sync() { ++syncCalls; return true; }
};

int main() {
  // 1. AFTER-fix 48 GiB benchmark: 10 x (32 KiB write + doSynchronizeCache -> store.sync() only, no full checkpoint)
  constexpr uint64_t kPages = (48ULL << 30) / persist::kBlock;  // 12,582,912
  constexpr uint64_t kWalBlocks = 16380;
  uint64_t windowBlocks = 0;
  assert(persist::wal::windowBlocksFor(kPages, kWalBlocks, &windowBlocks));
  assert(windowBlocks == 12624640);

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

  ops.resetCounters();
  std::vector<uint8_t> pageBuf(8 * persist::kBlock);
  for (int t = 0; t < 10; ++t) {
    for (size_t i = 0; i < pageBuf.size(); ++i) pageBuf[i] = uint8_t((t * 17 + i / 4096) & 0xff);
    assert(ov.writePages(ops, uint64_t(t) * 8, 8, pageBuf.data()));
    assert(ov.commitRun(ops, uint64_t(t) * 8, 8));
    // AFTER-fix NativeRamBlock::doSynchronizeCache() calls store.sync() (1 dirty-range sync cmd):
    assert(ops.sync());
  }

  // With Patch A, each ops.sync() after 1 WAL tx (<= 10 blocks) issues 1 SCSI SYNC cmd (10 cmds total for 10 ops.sync).
  // Inside appendTransactionUnlocked: 3 writes (begin, data, commit) + 2 syncs = 5 cmds/tx.
  // Plus 1 doSynchronizeCache sync = 1 cmd -> total 6 cmds/tx, 60 cmds for 10 txs.
  const uint64_t afterSyncCmds = ops.syncCalls * 1;
  const uint64_t afterTotalCmds = ops.writeCalls + afterSyncCmds;
  std::printf("AFTER-fix 10 x (32KiB write + synchronizeCache) on 48 GiB:\n");
  std::printf("  ops.write calls = %llu (%llu blocks = %.2f MiB)\n",
              (unsigned long long)ops.writeCalls,
              (unsigned long long)ops.writeBlocks,
              double(ops.writeBlocks) * 4096.0 / (1024.0 * 1024.0));
  std::printf("  ops.sync  calls = %llu (-> %llu SCSI SYNCHRONIZE CACHE cmds)\n",
              (unsigned long long)ops.syncCalls, (unsigned long long)afterSyncCmds);
  std::printf("  TOTAL SCSI write+sync cmds = %llu\n", (unsigned long long)afterTotalCmds);
  for (double ms : {0.2, 0.5, 1.0}) {
    std::printf("  Simulated @ %.1f ms/cmd: %.4f seconds\n", ms, afterTotalCmds * ms / 1000.0);
  }
  assert(afterTotalCmds == 60);

  // 2. Durability & Crash Recovery test WITHOUT checkpoint:
  // Simulate abrupt power loss right after 100 WAL transactions (without calling flush/checkpoint),
  // then resumeExistingV2() and verify 100% of 800 written pages bit-for-bit!
  for (int t = 10; t < 100; ++t) {
    for (size_t i = 0; i < pageBuf.size(); ++i) pageBuf[i] = uint8_t((t * 17 + i / 4096) & 0xff);
    assert(ov.writePages(ops, uint64_t(t) * 8, 8, pageBuf.data()));
    assert(ov.commitRun(ops, uint64_t(t) * 8, 8));
    assert(ops.sync());
  }

  std::fill(bitmap.begin(), bitmap.end(), 0);
  std::fill(entries.begin(), entries.end(), persist::wal::Entry{});
  persist::Overlay recovered;
  assert(recovered.resumeExistingV2(ops, kPages, seed, false, bitmap.data(),
                                    entries.data(), entryCap, kWalBlocks));
  assert(recovered.present == 800);
  std::vector<uint8_t> rd(persist::kBlock);
  for (int t = 0; t < 100; ++t) {
    for (int p = 0; p < 8; ++p) {
      uint64_t idx = uint64_t(t) * 8 + p;
      assert(recovered.has(idx));
      assert(recovered.readPresent(ops, idx, rd.data()));
      uint8_t want = uint8_t((t * 17 + p) & 0xff);
      for (uint8_t b : rd) assert(b == want);
    }
  }
  std::printf("PASS: 100 WAL writes (800 pages) WITHOUT checkpoint recovered 100%% bit-for-bit after abrupt power loss\n");

  // 3. Negative test: Realistic torn WAL tail (valid begin+payload written, commit missing/torn) rolls back cleanly to tx 99
  const uint64_t slot100Start = recovered.walStore.walStart +
                                recovered.walStore.nextSlot * persist::wal::kTransactionBlocks;
  std::vector<uint8_t> tornBegin(persist::kBlock, 0);
  memcpy(tornBegin.data(), "SHNGV2JB", 8);
  persist::put32(tornBegin.data() + 8, persist::wal::kVersion);
  persist::put32(tornBegin.data() + 12, persist::wal::kBeginBytes);
  persist::put64(tornBegin.data() + 16, recovered.walStore.nextSequence);
  persist::put64(tornBegin.data() + 24, recovered.walStore.lastSequence);
  memcpy(tornBegin.data() + 32, seed, 32);
  persist::put64(tornBegin.data() + 64, kPages);
  persist::put32(tornBegin.data() + 72, 1);
  persist::put32(tornBegin.data() + 76, persist::wal::kTransactionKindNormal);
  persist::put32(tornBegin.data() + 80, uint32_t(recovered.walStore.nextSlot));
  persist::put64(tornBegin.data() + 320, persist::wal::crc64(tornBegin.data(), 320));
  assert(ops.write(slot100Start, 1, tornBegin.data()));
  std::fill(bitmap.begin(), bitmap.end(), 0);
  std::fill(entries.begin(), entries.end(), persist::wal::Entry{});
  persist::Overlay recTorn;
  assert(recTorn.resumeExistingV2(ops, kPages, seed, false, bitmap.data(),
                                   entries.data(), entryCap, kWalBlocks));
  assert(recTorn.present == 800);
  assert(recTorn.walStore.stoppedAtUncommittedTail);
  std::printf("PASS: Torn WAL tail (valid begin without commit) rolled back cleanly to 800 pages\n");

  // 4. Negative test: Random garbage at scanned>0 fails closed
  std::vector<uint8_t> garbage(persist::kBlock, 0xCC);
  assert(ops.write(slot100Start, 1, garbage.data()));
  std::fill(bitmap.begin(), bitmap.end(), 0);
  std::fill(entries.begin(), entries.end(), persist::wal::Entry{});
  persist::Overlay recCorrupt;
  assert(!recCorrupt.resumeExistingV2(ops, kPages, seed, false, bitmap.data(),
                                      entries.data(), entryCap, kWalBlocks));
  assert(!recCorrupt.active);
  std::printf("PASS: Unrecognized corruption at scanned>0 fails closed\n");

  // 4. Negative test: Wrong seed / corrupted root fails closed (resumeExistingV2 returns false)
  uint8_t badSeed[32] = {};
  memcpy(badSeed, seed, 32);
  badSeed[0] ^= 0xFF;
  persist::Overlay recBad;
  assert(!recBad.resumeExistingV2(ops, kPages, badSeed, false, bitmap.data(),
                                   entries.data(), entryCap, kWalBlocks));
  assert(!recBad.active);
  std::printf("PASS: Wrong seed fails closed\n");
  return 0;
}
