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

static uint32_t refCrc32(const uint8_t *data, size_t bytes) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < bytes; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

static uint64_t refCrc64(const uint8_t *data, size_t bytes) {
  uint64_t crc = UINT64_MAX;
  for (size_t i = 0; i < bytes; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xc96c5795d7870f42ULL & (0ULL - (crc & 1ULL)));
  }
  return ~crc;
}

static uint8_t pat(uint64_t page, uint64_t gen) {
  return uint8_t((page * 29 + gen * 7 + 3) & 0xFF);
}

int main() {
  // 1. Bit-exact CRC32 & CRC64 equivalence
  uint8_t sample[4096];
  for (size_t i = 0; i < sizeof(sample); ++i) sample[i] = uint8_t((i * 131 + 17) & 0xFF);
  for (size_t len : {size_t(0), size_t(1), size_t(7), size_t(64), size_t(320), size_t(4096)}) {
    assert(persist::wal::crc32(sample, len) == refCrc32(sample, len));
    assert(persist::wal::crc64(sample, len) == refCrc64(sample, len));
  }
  std::printf("PASS: Table CRC32 & CRC64 are 100%% bit-exact with reference\n");

  // 2. 48 GiB incremental checkpoint & reset-transaction test
  constexpr uint64_t kPages = (48ULL << 30) / persist::kBlock; // 12,582,912
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
  for (int i = 0; i < 32; ++i) seed[i] = uint8_t(0xC0 + i);

  SparseOps ops;
  ops.totalBlocks = windowBlocks;
  persist::Overlay ov;
  assert(ov.open(ops, kPages, seed, false, bitmap.data(), true, true,
                 entries.data(), entryCap, kWalBlocks) == persist::Open::Fresh);

  // Write 200 base pages (spread across bitmap) and checkpoint (gen=2)
  std::vector<uint8_t> pageBuf(8 * persist::kBlock);
  for (uint64_t t = 0; t < 25; ++t) {
    for (uint64_t p = 0; p < 8; ++p)
      memset(pageBuf.data() + p * persist::kBlock, pat(t * 8 + p, 1), persist::kBlock);
    assert(ov.writePages(ops, t * 8, 8, pageBuf.data()));
  }
  assert(ov.flush(ops));
  assert(ov.walStore.checkpointGeneration == 2);

  // Write 80 WAL pages: 40 overwriting base (0..39 with gen=2), 40 new (200..239 with gen=2)
  for (uint64_t t = 0; t < 5; ++t) {
    for (uint64_t p = 0; p < 8; ++p)
      memset(pageBuf.data() + p * persist::kBlock, pat(t * 8 + p, 2), persist::kBlock);
    assert(ov.writePages(ops, t * 8, 8, pageBuf.data()));
  }
  for (uint64_t t = 0; t < 5; ++t) {
    for (uint64_t p = 0; p < 8; ++p)
      memset(pageBuf.data() + p * persist::kBlock, pat(200 + t * 8 + p, 2), persist::kBlock);
    assert(ov.writePages(ops, 200 + t * 8, 8, pageBuf.data()));
  }

  ops.resetCounters();
  assert(ov.flush(ops));
  assert(ov.walStore.checkpointGeneration == 3);
  std::printf("48 GiB incremental checkpoint (80 WAL pages, 240 total present): reads=%llu (%llu blks), writes=%llu (%llu blks), syncs=%llu\n",
              (unsigned long long)ops.readCalls, (unsigned long long)ops.readBlocks,
              (unsigned long long)ops.writeCalls, (unsigned long long)ops.writeBlocks,
              (unsigned long long)ops.syncCalls);
  assert(ops.writeCalls < 2500);
  assert(ops.readCalls < 200);

  // Reload and verify all 240 pages bit-for-bit!
  std::fill(bitmap.begin(), bitmap.end(), 0);
  std::fill(entries.begin(), entries.end(), persist::wal::Entry{});
  persist::Overlay rel;
  assert(rel.resumeExistingV2(ops, kPages, seed, false, bitmap.data(),
                              entries.data(), entryCap, kWalBlocks));
  assert(rel.present == 240);
  std::vector<uint8_t> rd(persist::kBlock);
  for (uint64_t p = 0; p < 240; ++p) {
    uint8_t want = (p < 40 || p >= 200) ? pat(p, 2) : pat(p, 1);
    assert(rel.readPresent(ops, p, rd.data()));
    for (uint8_t b : rd) assert(b == want);
  }
  std::printf("PASS: 48 GiB incremental checkpoint + reload verified all 240 pages bit-for-bit\n");

  // 3. Reset transaction clears stale pre-reset checksums safely
  assert(rel.walStore.resetTransaction(ops));
  for (uint64_t p = 0; p < 8; ++p)
    memset(pageBuf.data() + p * persist::kBlock, pat(p, 9), persist::kBlock);
  assert(rel.writePages(ops, 0, 8, pageBuf.data()));
  assert(rel.flush(ops));

  std::fill(bitmap.begin(), bitmap.end(), 0);
  std::fill(entries.begin(), entries.end(), persist::wal::Entry{});
  persist::Overlay afterReset;
  assert(afterReset.resumeExistingV2(ops, kPages, seed, true, bitmap.data(),
                                     entries.data(), entryCap, kWalBlocks));
  assert(afterReset.present == 8);
  for (uint64_t p = 0; p < 8; ++p) {
    assert(afterReset.readPresent(ops, p, rd.data()));
    for (uint8_t b : rd) assert(b == pat(p, 9));
  }
  assert(!afterReset.has(40));
  std::printf("PASS: resetTransaction + non-incremental checkpoint cleared stale pages cleanly\n");
  return 0;
}
