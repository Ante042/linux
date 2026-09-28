T6050 N1 DMA under x1n1
======================

J714s hardware qualification
---------------------------

A Linux EL1 guest with all 18 CPUs and 59.436 GiB usable RAM successfully
uploaded the personalized J714s N1 FTAB through the Control PCI function on
2026-09-27. The ROM returned response 1, advanced from stage 0 to stage 4,
and wrote MSI data 0xa5a0 into the diagnostic DMA mailbox. This verifies actual
PCIe device reads and writes through the SPTM-managed DART, beyond API-only
map/unmap tests. This initial result used a diagnostic MSI DMA mailbox. The later native-MSI
and runtime qualification is described below; radio operation and throughput
remain unqualified.

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

Native MSI, reset and runtime qualification
------------------------------------------

The preinitialized ``apple,x1n1-pcie`` host now exposes hierarchical native
MSI using the AIC parent. T6050 port0 captures address0xfffff000 and maps
32 local vectors onto AIC1824..1855. Linux allocates aligned vector ranges;
per-function positions are not hardcoded. Its DART advertises the capture
page as IOMMU_RESV_MSI, so the MSI address remains untranslated.

The optional J714s port resources support the native firmware transition:
endpoint bus mastering stops and pending transactions drain; the endpoint
enters D3hot; bridge decode/bus numbers are cleared before PME broadcast;
PME acknowledgement/L2 precede GPIO PERST and refclk-gating disable. The
native DART power transition retains tables/mappings, and port reset restores
RID/MSI routes before link training and PCI state restoration. Errors leave
bridge decode disabled. The cycle is restricted to the preboot personality
with no enumerated sibling functions. It does not reset an active Wi-Fi bus.

Guest resources passed alongside ECAM are port0 at0x62800000+0x8000,
PHY at0x62808000+0x4000, interrupt routing at0x6280c000+0x4000, and the
PERST pad at0x62810140+4. The host provides target-derived four-cell tunable
entries (offset, width, mask, value), and the kernel executes the reviewed
native port sequence. The corresponding physical resources remain owned by
this preinitialized controller; initial PHY/SMC setup still occurs in x1n1's
hardware setup workflow.

The IRQ conversion removes recurring Wi-Fi/Bluetooth traffic polling.
Control reserves4 MSI vectors, Alpha16, Beta2. Alpha's vectors must exist
before its IPC handshake. They remain allocated through runtime, with a
synchronized callback handoff to the Wi-Fi worker. Beta owns its own vectors.
Finite bootstrap/command response polls remain.

Each function has its own IOMMU domain on this machine. Buffers therefore
retain the consuming function's DMA device and matching lifetime: Control
owns secondary FTAB and Control IPC; each memory-swap working copy belongs
to its recipient; Alpha owns its window, context, messages and firmware log.
Allocating all buffers through Control prevented Alpha from starting.

Hardware ``hardware-linux-n1-runtime-3-guest.log`` establishes:

* Native Control MSI reception, ROM success1/stage4.
* Port cycle completed with endpoint identity106b:1901 and boot stage1.
* Control reached OS stage2, completed IPC and CCHI hello.
* Alpha completed its IPC handshake and registered ``wlan0``.
* Beta completed IPC and registered ``hci0``.

Kernel ``Image-n1-early-alpha-irq`` SHA256:
``8a43716a91eb395ced1226c78c94d460647ace6712ee49002d97da3d22500720``.
Stage2 ``candidate-n1-port-cycle`` and target DT
``t6050-n1-cycle-enabled.dtb``. Full kernel builds and IRQ cleanup failure
checks passed. Focused QEMU tests validate the additional stage-2 resource
windows only; all endpoint/interrupt results above are from hardware.

Interface registration is not association, packet transfer, Bluetooth radio
qualification or native performance equivalence. Production data-ring startup,
firmware27 connection details and sustained traffic are still in progress.

Native macOS 27 scan qualification
---------------------------------

J714s uses the native 14-byte scan header followed by the channel-list TLV260
and five-byte options TLV357. Channels use the ACI band encoding and respect
cfg80211's requested/allowed list. Native default dwell fields are 40/40/110
milliseconds; the inherited 110/110/0 values caused Alpha to enter execution
stage3 on the first scan even after the required TLVs were added.

With the corrected native request, hardware completed 38 channels and reported
36 beacons, including the requested home network. Alpha remained at execution
stage2 after completion. This establishes scanning, not association or packet
traffic. J813 retains its existing firmware-specific scan path.

Evidence: ``hardware-linux-n1-scan-dwell-1-guest.log``; kernel
``Image-n1-scan-dwell`` SHA256:
``bf1fcb4998ed820629fd937b7dea1aeeb1570b27c5d120216ca0b6cc9eb67ce7``.
The native ABI references are ACIScanAdapter::buildScanRequestNewApi at
0x1809bf400 and IO80211ScanRequest defaults at0x1807b46d4 in the captured
DriverKit cache. Firmware payload/calibration and network details remain
private local artifacts.
