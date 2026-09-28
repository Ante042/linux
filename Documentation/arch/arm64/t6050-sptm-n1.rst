T6050 N1 DMA under x1n1
======================

J714s hardware qualification
---------------------------

A Linux EL1 guest with all 18 CPUs and 59.436 GiB usable RAM successfully
uploaded the personalized J714s N1 FTAB through the Control PCI function on
2026-09-27. The ROM returned response 1, advanced from stage 0 to stage 4,
and wrote MSI data 0xa5a0 into the diagnostic DMA mailbox. This verifies actual
PCIe device reads and writes through the SPTM-managed DART, beyond API-only
map/unmap tests. Native interrupt delivery and operational Wi-Fi are separate,
unfinished steps; the inherited polled firmware prototype is not a qualified
full-performance networking implementation.

The test uses a preinitialized native T6050 PCIe controller, explicit bus1
RID 0x100/0x101/0x102 to SID1/2/3 routing, and direct ECAM/BAR stage-2 windows.
The controller needs both GPIO PERST and its independent native SMC reset
release. After this reset operation, AP and IOP must both acknowledge RTKit
QUIESCED before Linux starts a fresh SMC session.

Native DMA contract
-------------------

DART id1 uses a three-level stream (initial SPTM table level1). Device DMA
addresses are low; vm-force bit40 is added only in the SPTM transport and
removed before software mapping bookkeeping. The Centauri 40-bit DMA mask
therefore remains valid. Native IODARTMapper strips this bit from returned
addresses and restores it for map/unmap operations.

ADT vm-base is 0x10000100000, vm-size is 0x3fe00000. Both SPTM MAP and UNMAP
require the exclusive mapping end to be strictly below the SID upper page
bound. Native AppleT8110DART also checks page+1 < upper_bound. Reserve the
last 16 KiB page: device window [0x100000, 0x3fefc000), size 0x3fdfc000::

    apple,dma-range = <0 0x100000 0 0x3fdfc000>;
    apple,x1n1-iova-force = <0x100 0>;

Using the full raw ADT extent caused an SPTM INVALID_DVA panic at
0x1003ff00000 on the first coherent allocation. The guard matches native
validation; it is not an arbitrary reduced DMA aperture.

The batch ABI carries Linux IOMMU_READ/WRITE/CACHE allow bits. x1n1 must
invert these three bits for SPTM NO_READ/NO_WRITE/NO_CACHE. Forwarding the
Linux bits unchanged installs inaccessible mappings: calls complete, but
N1 ROM image load returns terminal failure5. After the conversion, the
identical firmware and guest kernel passed ROM upload.

Mapping ownership
-----------------

PCI topology and shared stream IDs determine IOMMU groups. The driver records
live mappings for address lookup and replay when an additional SID attaches
to an existing domain. It prepares tables before enabling streams and rolls
back incomplete mapping cohorts on failure. Normal map/unmap retains full
255-operation transport batches; replay scans happen on stream attachment,
not on every I/O. The optional forced IOVA transform touches only bounded
transport batches and restores arguments before completion bookkeeping.

Validation and artifacts
------------------------

The DART driver compiled in the normal Asahi-targeted kernel build. Extracted
C tests covered grouping/replay, partial failures in multi-batch 256-SID
cohorts, and forced-address restoration at every completion position under
ASan/UBSan. The stage-2 bundle passed a focused QEMU boot/mapping smoke test
before hardware. QEMU does not establish N1 DMA behavior.

Hardware evidence in the local workspace:
``work/j714-mpidr-20260927/hardware-linux-n1-rom-3-guest.log``.
Kernel ``Image-n1-force`` SHA256:
``02df14333ad13e8f44d76332eadb159b0437501df6d11b3a39cbc677db3f73b8``.
Stage2 bundle ``candidate-n1-dart-permissions``; target DT
``t6050-n1-guard-enabled.dtb``. Firmware/calibration stays private.
