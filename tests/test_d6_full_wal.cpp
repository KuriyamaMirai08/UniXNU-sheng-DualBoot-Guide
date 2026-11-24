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
  uint64_t writes = 0, syncs = 0;

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
    ++writes;
    const uint8_t *in = static_cast<const uint8_t *>(src);
    for (uint32_t i = 0; i < count; ++i) {
      auto &slot = blocks[block + i];
      if (slot.empty()) slot.resize(persist::kBlock);
      memcpy(slot.data(), in + size_t(i) * persist::kBlock, persist::kBlock);
    }
    return true;
  }

  bool sync() { ++syncs; return true; }
};

static uint8_t patternFor(uint64_t page) {
  return uint8_t((page * 37 + 11) & 0xFF);
}

int main() {
  constexpr uint64_t kPages = 256;
  constexpr uint64_t kWalBlocks = 40; // 4 slots * 8 pages/tx = 32 exactCap
  uint64_t windowBlocks = 0;
  assert(persist::wal::windowBlocksFor(kPages, kWalBlocks, &windowBlocks));
  uint64_t bitmapCount = 0;
  assert(persist::bitmapBlocksFor(kPages, &bitmapCount));
  const uint32_t exactCap = uint32_t((kWalBlocks / persist::wal::kTransactionBlocks) *
                                     persist::wal::kPagesPerTransaction);
  assert(exactCap == 32);

  uint8_t seed[32] = {};
  for (int i = 0; i < 32; ++i) seed[i] = uint8_t(0x50 + i);

  std::vector<uint8_t> bitmap(bitmapCount * persist::kBlock, 0);
  std::vector<persist::wal::Entry> entries2x(exactCap * 2);
  SparseOps ops;
  ops.totalBlocks = windowBlocks;

  persist::Overlay ov;
  assert(ov.open(ops, kPages, seed, false, bitmap.data(), true, true,
                 entries2x.data(), exactCap * 2, kWalBlocks) == persist::Open::Fresh);

  // 1. Write 8 base pages (0..7) and checkpoint (gen=2)
  std::vector<uint8_t> pageBuf(8 * persist::kBlock);
  for (uint64_t p = 0; p < 8; ++p)
    memset(pageBuf.data() + p * persist::kBlock, patternFor(p), persist::kBlock);
  assert(ov.writePages(ops, 0, 8, pageBuf.data()));
  assert(ov.commitRun(ops, 0, 8));
  assert(ov.flush(ops));
  assert(ov.walStore.checkpointGeneration == 2);

  // 2. Write 4 WAL transactions of 8 distinct pages each (8..39 -> 32 distinct pages, filling 4/4 slots)
  for (uint64_t s = 0; s < 4; ++s) {
    for (uint64_t p = 0; p < 8; ++p)
      memset(pageBuf.data() + p * persist::kBlock, patternFor(8 + s * 8 + p), persist::kBlock);
    assert(ov.writePages(ops, 8 + s * 8, 8, pageBuf.data()));
    assert(ov.commitRun(ops, 8 + s * 8, 8));
  }
  assert(ov.walStore.nextSlot == 4);
  assert(ov.walStore.lastSequence - ov.walStore.baseSequence == 4);

  // Now simulate exactCap (100% full hash table) vs 2*exactCap
  std::vector<persist::wal::Entry> entriesExact(exactCap);
  std::vector<uint8_t> rd(persist::kBlock);

#ifdef REPRO_BEFORE
  // Repro Bug A2: resumeExistingV2(..., exactCap, ...) reproduces Boot #2 screen:
  // spaceExhausted=true, lastSeq-baseSeq=4, nextSlot=0, returns false with lastFault=0xFF!
  std::fill(bitmap.begin(), bitmap.end(), 0);
  persist::Overlay roExact;
  bool okRoExact = roExact.resumeExistingV2(ops, kPages, seed, true, bitmap.data(),
                                            entriesExact.data(), exactCap, kWalBlocks);
  assert(!okRoExact);
  assert(roExact.walStore.spaceExhausted);
  assert(roExact.walStore.lastSequence - roExact.walStore.baseSequence == 4);
  assert(roExact.walStore.nextSlot == 0);
  assert(roExact.walStore.lastFault == 0xFF);
  std::printf("REPRO_BEFORE Bug A2: resumeExistingV2(exactCap) failed with spaceExhausted=1 fault=0x%X\n",
              roExact.walStore.lastFault);

  // Repro Bug A1: even on a live overlay with exactCap, flush() fails when reading base pages!
  std::fill(bitmap.begin(), bitmap.end(), 0);
  SparseOps opsA1;
  opsA1.totalBlocks = windowBlocks;
  persist::Overlay ovA1;
  assert(ovA1.open(opsA1, kPages, seed, false, bitmap.data(), true, true,
                   entriesExact.data(), exactCap, kWalBlocks) == persist::Open::Fresh);
  for (uint64_t p = 0; p < 8; ++p)
    memset(pageBuf.data() + p * persist::kBlock, patternFor(p), persist::kBlock);
  assert(ovA1.writePages(opsA1, 0, 8, pageBuf.data()));
  assert(ovA1.flush(opsA1));
  for (uint64_t s = 0; s < 4; ++s) {
    for (uint64_t p = 0; p < 8; ++p)
      memset(pageBuf.data() + p * persist::kBlock, patternFor(8 + s * 8 + p), persist::kBlock);
    assert(ovA1.writePages(opsA1, 8 + s * 8, 8, pageBuf.data()));
  }
  bool okFlushA1 = ovA1.flush(opsA1);
  assert(!okFlushA1);
  assert(ovA1.walStore.lastFault == 0xFF);
  std::printf("REPRO_BEFORE Bug A1: flush() on full exactCap table failed with fault=0x%X\n",
              ovA1.walStore.lastFault);

  // Repro Bug B: reopen with 2*exactCap (no Bug A) and write page 40 -> overwrites slot 0 without checkpoint!
  std::fill(bitmap.begin(), bitmap.end(), 0);
  persist::Overlay rw2x;
  assert(rw2x.resumeExistingV2(ops, kPages, seed, false, bitmap.data(),
                               entries2x.data(), exactCap * 2, kWalBlocks));
  assert(rw2x.walStore.spaceExhausted);
  assert(rw2x.walStore.nextSlot == 0);
  uint8_t p40[persist::kBlock];
  memset(p40, patternFor(40), sizeof p40);
  assert(rw2x.writePages(ops, 40, 1, p40));
  assert(rw2x.walStore.checkpointGeneration == 2); // BUG B: did NOT checkpoint!
  std::printf("REPRO_BEFORE Bug B: write after full-WAL resume did NOT checkpoint (gen stayed %llu)\n",
              (unsigned long long)rw2x.walStore.checkpointGeneration);
#else
  // AFTER FIX:
  // 1. exactCap (100% full table) read-only resume & read all 40 pages succeeds with 0 writes!
  std::fill(bitmap.begin(), bitmap.end(), 0);
  persist::Overlay roExact;
  uint64_t wBefore = ops.writes, sBefore = ops.syncs;
  assert(roExact.resumeExistingV2(ops, kPages, seed, true, bitmap.data(),
                                  entriesExact.data(), exactCap, kWalBlocks));
  assert(ops.writes == wBefore && ops.syncs == sBefore);
  assert(roExact.present == 40);
  for (uint64_t p = 0; p < 40; ++p) {
    assert(roExact.readPresent(ops, p, rd.data()));
    for (uint8_t b : rd) assert(b == patternFor(p));
  }
  // Negative check: on 100%-full exactCap table, inserting a NEW key (insertAt != nullptr) must still return false!
  persist::wal::Entry *foundPtr = reinterpret_cast<persist::wal::Entry *>(1);
  uint32_t insAt = 0;
  assert(!roExact.walStore.entryFind(40, &foundPtr, &insAt));
  std::printf("PASS: exactCap full-table read-only resume + 40-page verify (0 writes); insert-probe still fails closed\n");

  // 2. 2*exactCap writable resume + write page 40 -> triggers checkpoint (gen 2->3) and preserves all 41 pages!
  std::fill(bitmap.begin(), bitmap.end(), 0);
  persist::Overlay rw2x;
  assert(rw2x.resumeExistingV2(ops, kPages, seed, false, bitmap.data(),
                               entries2x.data(), exactCap * 2, kWalBlocks));
  assert(rw2x.walStore.spaceExhausted);
  uint8_t p40[persist::kBlock];
  memset(p40, patternFor(40), sizeof p40);
  assert(rw2x.writePages(ops, 40, 1, p40));
  assert(rw2x.commitRun(ops, 40, 1));
  assert(rw2x.walStore.checkpointGeneration == 3);
  assert(!rw2x.walStore.spaceExhausted);
  assert(rw2x.walStore.nextSlot == 1);

  std::fill(bitmap.begin(), bitmap.end(), 0);
  persist::Overlay verifyAll;
  assert(verifyAll.resumeExistingV2(ops, kPages, seed, true, bitmap.data(),
                                    entries2x.data(), exactCap * 2, kWalBlocks));
  assert(verifyAll.present == 41);
  for (uint64_t p = 0; p < 41; ++p) {
    assert(verifyAll.readPresent(ops, p, rd.data()));
    for (uint8_t b : rd) assert(b == patternFor(p));
  }
  std::printf("PASS: 2xCap writable resume + write triggered gen 2->3 checkpoint & preserved all 41 pages\n");
#endif
  return 0;
}
