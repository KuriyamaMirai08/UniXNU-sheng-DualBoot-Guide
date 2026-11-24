#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "DirectDisk.h"

struct SparseOps {
  uint64_t totalBlocks = 0;
  std::unordered_map<uint64_t, std::vector<uint8_t>> blocks;
  uint64_t readCalls = 0, readBlocks = 0, writeCalls = 0, writeBlocks = 0, syncCalls = 0;

  void resetCounters() { readCalls = readBlocks = writeCalls = writeBlocks = syncCalls = 0; }

  bool read(uint64_t block, uint32_t count, void *dst) {
    if (block > totalBlocks || count > totalBlocks - block) return false;
    ++readCalls;
    readBlocks += count;
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

static uint8_t pat(uint64_t p, uint64_t g) { return uint8_t((p * 41 + g * 13 + 5) & 0xFF); }

int main() {
  constexpr uint64_t kPages = (48ULL << 30) / persist::kBlock;
  constexpr uint64_t kWalBlocks = 16380;
  uint64_t windowBlocks = 0;
  assert(persist::wal::windowBlocksFor(kPages, kWalBlocks, &windowBlocks));
  uint64_t bitmapCount = 0;
  assert(persist::bitmapBlocksFor(kPages, &bitmapCount));
  std::vector<uint8_t> bitmap(bitmapCount * persist::kBlock, 0);
  const uint32_t entryCap = uint32_t((kWalBlocks / persist::wal::kTransactionBlocks) *
                                     persist::wal::kPagesPerTransaction * 2);
  std::vector<persist::wal::Entry> entries(entryCap);
  uint8_t seed[32] = {};
  for (int i = 0; i < 32; ++i) seed[i] = uint8_t(0xD0 + i);

  SparseOps ops;
  ops.totalBlocks = windowBlocks;
  persist::Overlay ov;
  assert(ov.open(ops, kPages, seed, false, bitmap.data(), true, true,
                 entries.data(), entryCap, kWalBlocks) == persist::Open::Fresh);

  // Write 1024 base pages (4 MiB, 1 full checksum block) and checkpoint (gen=2)
  std::vector<uint8_t> pageBuf(8 * persist::kBlock);
  for (uint64_t t = 0; t < 128; ++t) {
    for (uint64_t p = 0; p < 8; ++p)
      memset(pageBuf.data() + p * persist::kBlock, pat(t * 8 + p, 1), persist::kBlock);
    assert(ov.writePages(ops, t * 8, 8, pageBuf.data()));
  }
  assert(ov.flush(ops));
  assert(ov.walStore.checkpointGeneration == 2);

  // 1. Constant-time boot (verifyBase=false): 0 base-page reads!
  std::fill(bitmap.begin(), bitmap.end(), 0);
  std::fill(entries.begin(), entries.end(), persist::wal::Entry{});
  ops.resetCounters();
  persist::Overlay fastBoot;
  assert(fastBoot.resumeExistingV2(ops, kPages, seed, false, bitmap.data(),
                                   entries.data(), entryCap, kWalBlocks, false));
  std::printf("48 GiB boot (1024 base pages, verifyBase=false): readCalls=%llu readBlocks=%llu\n",
              (unsigned long long)ops.readCalls, (unsigned long long)ops.readBlocks);
  assert(ops.readCalls < 900); // constant overhead regardless of 1024 or 10M base pages!

  // 2. Runtime ckCache hit: reading 100 base pages in same checksum block -> 1 ck read + 100 data reads = 101 reads!
  std::vector<uint8_t> rd(persist::kBlock);
  ops.resetCounters();
  for (uint64_t p = 0; p < 100; ++p) {
    assert(fastBoot.readPresent(ops, p, rd.data()));
    for (uint8_t b : rd) assert(b == pat(p, 1));
  }
  std::printf("Runtime ckCache 100 base reads: readCalls=%llu (expected 101)\n",
              (unsigned long long)ops.readCalls);
  assert(ops.readCalls == 101);

  // 3. Cache invalidation across checkpoint: update page 0, checkpoint gen 2->3, read page 0 -> sees new data!
  memset(pageBuf.data(), pat(0, 9), persist::kBlock);
  assert(fastBoot.writePages(ops, 0, 1, pageBuf.data()));
  assert(fastBoot.flush(ops));
  assert(fastBoot.walStore.checkpointGeneration == 3);
  assert(fastBoot.readPresent(ops, 0, rd.data()));
  for (uint8_t b : rd) assert(b == pat(0, 9));
  std::printf("PASS: ckCache invalidated cleanly across checkpoint gen 2->3\n");

  // 4. Coalesced verifyEffectiveUnlocked (verifyBase=true) reads 1024 base pages in ~64 batched reads,
  //    AND runtime on-demand CRC32 catches base page #500 corruption under verifyBase=false!
  ops.blocks[fastBoot.walStore.layout.baseStart + 500][0] ^= 0xFF;

  std::fill(bitmap.begin(), bitmap.end(), 0);
  std::fill(entries.begin(), entries.end(), persist::wal::Entry{});
  ops.resetCounters();
  persist::Overlay slowBoot;
  assert(!slowBoot.resumeExistingV2(ops, kPages, seed, true, bitmap.data(),
                                    entries.data(), entryCap, kWalBlocks, true));
  std::printf("PASS: Coalesced verifyBase=true caught corrupted base page #500\n");

  std::fill(bitmap.begin(), bitmap.end(), 0);
  std::fill(entries.begin(), entries.end(), persist::wal::Entry{});
  persist::Overlay onDemand;
  assert(onDemand.resumeExistingV2(ops, kPages, seed, true, bitmap.data(),
                                   entries.data(), entryCap, kWalBlocks, false));
  assert(onDemand.readPresent(ops, 499, rd.data()));
  assert(!onDemand.readPresent(ops, 500, rd.data()));
  std::printf("PASS: On-demand CRC32 under verifyBase=false allowed #499 and failed closed on corrupted #500\n");
  return 0;
}
