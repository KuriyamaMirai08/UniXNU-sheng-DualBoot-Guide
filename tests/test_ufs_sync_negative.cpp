#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

// Include the host RootCommandsBridge + patched RootCommands.h directly to test
// hostSyncCache dirty-range tracking, chunking (>65535 blocks), boundary isolation,
// and authority denial/revocation!
#include "tests/RootCommandsBridge.cpp"

static std::vector<std::pair<uint64_t, uint32_t>> gSyncCdbs;
static uint32_t gInterceptedSyncCount = 0;

static uint64_t spyIo(unsigned op, uint64_t address, uint64_t value, void *bytes) {
  (void)bytes;
  if (op == 0) {
    if (address == 0x34) return 1;      // UTRLRSR = 1
    if (address == 0x30) return 15;     // UTMRLRSR = 15
    if (address == 0x60) return 1;      // UFS ready
    return 0;
  }
  if (op == 1 && address == 0x58) {
    // Doorbell! Inspect each bit set in value (2*slot)
    for (unsigned slot = 0; slot < kRegionSlots; ++slot) {
      if (value & (1ULL << (2 * slot))) {
        const uint8_t *ucd = memory + ufsSlotUcdOffset(slot);
        const uint8_t *cdb = ucd + 16;
        const uint8_t opcode = cdb[0];
        if (opcode == 0x35) { // SYNCHRONIZE CACHE(10)
          uint32_t lba = (uint32_t(cdb[2]) << 24) | (uint32_t(cdb[3]) << 16) |
                         (uint32_t(cdb[4]) << 8) | uint32_t(cdb[5]);
          uint16_t cnt = (uint16_t(cdb[7]) << 8) | uint16_t(cdb[8]);
          gSyncCdbs.push_back({lba, cnt});
          ++gInterceptedSyncCount;
        }
        // Complete slot immediately with SCSI GOOD (0)
        uint8_t *rsp = memory + ufsSlotUcdOffset(slot) + kRspOffset;
        memcpy(rsp, ucd, 32);
        rsp[0] = uint8_t(ucd[0] + 0x20);
        rsp[1] = 0;
        memset(rsp + 12, 0, 4);
        uint8_t *utrd = memory + ufsSlotUtrdOffset(slot);
        memset(utrd + 8, 0, 4); // OCS = 0 (success)
      }
    }
    return 0;
  }
  return 0;
}

int main() {
  ufs_test_reset(spyIo, 0x8cb5e9000ULL);
  // Register Data window [1000, 1000 + 12624640) -> 48 GiB window!
  assert(ufs_test_establish(100000000ULL, 100, 1000 + 12624640ULL + 10000ULL, 1000, 99999999ULL, 99999998ULL) == 1);
  assert(ufs_test_store_open(1000, 12624640ULL) == 1);

  // Test 1: Clean sync (no prior writes) -> 1 bounded command (lba=1000, count=1, NEVER count=0!)
  gSyncCdbs.clear();
  IOReturn rc = hostSyncCache(UfsWriteRole::Data); std::printf("rc=0x%x cdbs=%zu\n", rc, gSyncCdbs.size()); assert(rc == kIOReturnSuccess);
  assert(gSyncCdbs.size() == 1);
  assert(gSyncCdbs[0].first == 1000 && gSyncCdbs[0].second == 1);
  std::printf("PASS: Clean hostSyncCache(Data) issued 1 bounded CDB (lba=1000, count=1)\n");

  // Test 2: Write 8 blocks at lba=1050, then sync -> 1 CDB covering exact dirty range [1050, 1058)
  std::vector<uint8_t> buf(8 * 4096, 0x33);
  assert(hostWrite4k(UfsWriteRole::Data, 0, 1050, 8, buf.data(), false) == kIOReturnSuccess);
  gSyncCdbs.clear();
  assert(hostSyncCache(UfsWriteRole::Data) == kIOReturnSuccess);
  assert(gSyncCdbs.size() == 1);
  assert(gSyncCdbs[0].first == 1050 && gSyncCdbs[0].second == 8);
  std::printf("PASS: Dirty hostSyncCache(Data) issued 1 CDB for exact dirty range (lba=1050, count=8)\n");

  // Test 3: Second sync with no new writes resets to 1 block (dirty range was cleared)
  gSyncCdbs.clear();
  assert(hostSyncCache(UfsWriteRole::Data) == kIOReturnSuccess);
  assert(gSyncCdbs.size() == 1 && gSyncCdbs[0].first == 1000 && gSyncCdbs[0].second == 1);

  // Test 4: Disjoint writes (lba=1010 and lba=1010+70000) -> 2 chunked CDBs (65535 + 4473), NEVER count=0, strictly inside window
  assert(hostWrite4k(UfsWriteRole::Data, 0, 1010, 1, buf.data(), false) == kIOReturnSuccess);
  assert(hostWrite4k(UfsWriteRole::Data, 0, 1010 + 70000, 8, buf.data(), false) == kIOReturnSuccess);
  gSyncCdbs.clear();
  assert(hostSyncCache(UfsWriteRole::Data) == kIOReturnSuccess);
  assert(gSyncCdbs.size() == 2);
  assert(gSyncCdbs[0].first == 1010 && gSyncCdbs[0].second == 65535);
  assert(gSyncCdbs[1].first == 1010 + 65535 && gSyncCdbs[1].second == 4473);
  for (auto [lba, cnt] : gSyncCdbs) {
    assert(cnt > 0);
    assert(lba >= 1000 && lba + cnt <= 1000 + 12624640ULL);
  }
  std::printf("PASS: Large dirty range (>65535) chunks accurately inside authorized window (2 CDBs vs 193)\n");

  // Test 5: Authority revoked -> hostSyncCache denied with 0 doorbells
  ufsWriteAuthorityRevoke(false);
  gSyncCdbs.clear();
  assert(hostSyncCache(UfsWriteRole::Data) == kIOReturnNotPrivileged);
  assert(gSyncCdbs.empty());
  std::printf("PASS: Revoked authority denies hostSyncCache with 0 doorbells\n");
  return 0;
}
