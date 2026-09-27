T6050 SPTM storage import
========================

The ANS driver, RTKit and mailbox changes are path-scoped ports from
``aurora-silicon/linux-aurora@j873`` at
``a9e4a9181033e0e09bcb1c6b112b6426eccd7a70``. The imported commits retain
the source author name, email and date. Each has an ``Original-commit``
trailer and lists its included paths; unrelated GPU, desktop, thermal and
Wi-Fi changes mentioned in original commit messages were not imported.

Twenty-five commits were applied. A merge whose selected file changes were
already present was skipped. The pre-existing admin-buffer alignment and
admin-queue destruction fixes were retained rather than duplicated. The
existing wrapped-key NVMe crypto profile, key/IV programming and key cleanup
were preserved during conflict resolution. The previous removal of the
legacy NULL-PRP chicken-bit write was also retained.

The resulting RTKit and mailbox sources and headers match the pinned j873
versions. The NVMe source differs intentionally by the retained crypto and
NULL-PRP handling. The SPTM-backed SART frontend is retained; j873's raw SART
implementation does not replace it. NVMe is built in for the RAM-disk test
kernel. This import by itself does not enable ANS in the T6050 guest DT.

The merged kernel built successfully and booted in QEMU and on the physical
J714s with all 18 CPUs. All per-CPU sleep/SIMD checks passed, followed by
three timer wakes. The hardware boot also repeated the SART and DART
map/unmap checks with the Linux framebuffer console. These tests qualify
the merged kernel baseline, not ANS transfers: its guest DT still omits ANS.

Hardware evidence preceding the import establishes SART and DART map/unmap
from Linux, and all 18 CPUs passing sleep/SIMD checks. Read-only ANS inspection
found CPU_CONTROL=0x10, BOOT_STATUS=0xde71ce55, CC=0x474000 and CSTS=0 in
the primary and secure controller windows. The device is boot-ready but its
controller is disabled. Actual ANS DMA, SSD reads, throughput, DVFS and a
desktop remain to be qualified. No SSD write or format is part of this test.

Further reference commits are ``m1n1-aurora@j873``
``29e59aaaeb5de3f26456dcc3ecdcd7643a873b58`` and ``u-boot@j873``
``52474099334fdd459e49236a5392a10ccd6d158d``. Their macOS 27 handoff and
queue handling guide the T6050 adaptation; their addresses must not be
substituted for the captured T6050 ADT.
