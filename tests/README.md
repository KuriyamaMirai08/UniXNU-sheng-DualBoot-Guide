# Host Verification Tests

C++ host-side regression tests covering `PersistOverlay` and `PersistWalV2` (`d5` through `d8`).

Compile and run against `PersistOverlay.h` from `KuriyamaMirai08/UniXNU-sheng` (tag `dualboot-50g-d8`):

```bash
c++ -std=c++17 -O2 -I<path-to-PersistOverlay-include> tests/test_d8_verify.cpp -o test_d8_verify && ./test_d8_verify
c++ -std=c++17 -O2 -I<path-to-PersistOverlay-include> tests/test_d7_verify.cpp -o test_d7_verify && ./test_d7_verify
c++ -std=c++17 -O2 -I<path-to-PersistOverlay-include> tests/test_d6_full_wal.cpp -o test_d6_full_wal && ./test_d6_full_wal
c++ -std=c++17 -O2 -I<path-to-PersistOverlay-include> tests/test_d5_negative.cpp -o test_d5_negative && ./test_d5_negative
```

Also run the official repo lifecycle test (`151/151` expected):

```bash
python3 scripts/repro/stage0c2e/run-stage0c2e-host.py
```
