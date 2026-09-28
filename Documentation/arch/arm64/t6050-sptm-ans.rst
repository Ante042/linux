T6050 ANS under resident SPTM
============================

The apple,t6050-nvme-sptm profile uses a third sptm2mmio resource named
"sptm". Doorbell offset 0x1010 accepts the version1 batch format; feature
bit3 advertises NVMe table6 forwarding. Queue setup batches INIT,
ENABLE_CG, ADMIN_QUEUE_REGS, IOQA, IOCQ and IOSQ into one guest exit.
SPTM owns the real TCBs. Request mapping passes guest TCB IPA and byte count;
the host translates metadata and admits physical DMA pages without copying
payload data. CREATE_CQ/SQ reference their PRP1 queue page even with no
block-layer payload. Completion invalidation must release SPTM DMA references
before buffers may be reused.

The qualified full-RAM host uses identity guest IPA and native physical
addresses, with empty identity dma-ranges. The older diagnostic offset layout
is also supported by the transport. Queue and RTKit grants are16KiB aligned.
The firmware must already be running; this profile does not own its reset.
The obsolete linear-SQ control register is not written on T6050.

Hardware qualification on J714s, macOS27.0 firmware26A428:

* All18 CPUs boot, pass sleep/SIMD checks, and deliver timer interrupts.
* RTKit protocol12 initializes using mailbox IRQs1347/1350 (ADT indices1/2).
* Native table6 queue setup, controller identification and namespace discovery
  succeed. The namespace reports2 TB,4096-byte sectors and seven partitions.
* GPT header CRC and two64MiB O_DIRECT reads match, CRC32 0x9b30c356.
* Initial QD1 read-syscall throughput is1768--1793MiB/s. Direct MMIO mapping
  passes the same test. Native performance equivalence is not yet measured.

Full logs and pinned binaries are retained in the workspace directory
work/j714-mpidr-20260927. This is an initial resident profile; ordinary ANS
profiles and their wrapped-key crypto support are retained.

Write and flush qualification
-----------------------------

The resident profile disables the legacy deferred-flush policy. A successful
flush now requires completion of the corresponding hardware NVMe FLUSH;
acknowledging a request early and scheduling it later cannot establish the
requested fdatasync durability. Other profiles retain their existing policy.

On 2026-09-27, Linux with all18 CPUs and59.436GiB of guest RAM created a new
1GiB test file on the existing ALARM_ROOT ext4 filesystem, using O_DIRECT and
fdatasync. It verified the entire offset-dependent pattern, overwrote alternating
1MiB ranges, synced and verified again, and unmounted the filesystem. After a
hardware reboot, a read-only/no-journal-replay mount verified every byte of the
final pattern. Existing user files and boot files were not overwritten.

The initial1GiB pattern generation, write and fdatasync took0.198547 seconds
(5157.46MiB/s). This is a bounded sample, not sustained throughput or a native
comparison. Reboot persistence passed; sudden power-loss behavior was not tested.
Earlier timings with deferred flush enabled do not measure durable write speed.
Evidence: hardware-linux-ans-durable-guest.log and
hardware-linux-ans-durable-persist-guest.log under the workspace directory above.
Kernel Image SHA256:
019d9c370014fc726df4b39ef1f2218e36c0ca5e8bec3177e08b96c9945cc233.

Bootloader handoff
-----------------

A bootloader that quiesces both RTKit AP and IOP before releasing its shared
buffers sets the boolean ``asahi,rtkit-quiesced`` on the NVMe controller node.
The next kernel must negotiate a fresh INIT session even when CC.EN and CSTS.RDY
remain set. Those controller bits describe NVMe state, not RTKit mailbox state.
Without this indication the existing live-session adoption path attempts a
controller disable against a quiesced firmware session. A bootloader preserving
an active mailbox session must not set this property.

Queue ownership across U-Boot and Linux
--------------------------------------

Native SPTM retains the first registered ASQ/ACQ/IOSQ/IOCQ physical addresses
for the whole boot. Disabling the controller or quiescing RTKit does not
permit a later owner to replace those addresses. The firmware-27 admin
validation at 0xfffffff0270c8aa4 compares the new ASQ with its saved value;
a different address panics with VIOLATION_NVME_ILLEGAL_QUEUE_ADDRESS. The
Neo SPTM implementation also caches these addresses and rejects replacements.

A resident-SPTM U-Boot therefore allocates each queue in a separate 16 KiB
page and describes the four pages with ``memory-region`` phandles on the
NVMe device. ``memory-region-names`` is ``admin-sq``, ``admin-cq``, ``io-sq``,
``io-cq``. Each reserved-memory node has ``no-map`` and a 16 KiB aligned,
16 KiB sized ``reg``. U-Boot quiesces both RTKit sides before handing off,
and supplies ``asahi,rtkit-quiesced``. Linux validates all four reservations
and maps them cacheable on this DMA-coherent device; it never substitutes
new pages for a malformed handoff. TCB scratch storage and payload DMA
mappings retain their normal allocation paths. Queue depth and command
batching are unchanged, and there are no additional per-I/O copies.

This reservation describes firmware-owned memory, not a pool for arbitrary
DMA allocations. The pages remain reserved even if the driver is removed,
because their native SPTM registrations survive the driver lifetime.
