# Third-Party Notices & Boundaries

- Original documentation, AI playbook, Python helper scripts (`tools/*.py`), and host test suites in this repository are licensed under the [MIT License](LICENSE).
- Kernel, driver, and build-script patches under `patches/` target the `KuriyamaMirai08/UniXNU-sheng` fork (`dualboot-50g-d8`) and are governed by the respective upstream UniXNU / XNU / APSL / BSD / GPL licenses.
- **No proprietary Apple or Xiaomi binaries are redistributed here.** Users must supply their own legally obtained Xiaomi stock slot-A backup (`boot_a`, `vendor_boot_a`, `init_boot_a`, `dtbo_a`) and their own built UniXNU / iPadOS 27 payload (`kernel.macho`, `sheng-uefi`, `.vroot`, `scratch-data.img`, IPSW components).
- **Never commit** device serial numbers, signing keys (`*.b64` / `*.key`), GitHub tokens, or raw partition dumps.
