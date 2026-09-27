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

DMA ranges translate the guest RAM IPA window to its actual physical backing.
SART receives DMA addresses and converts them back to guest physical before
submitting its own transport. Queue and RTKit grants are16KiB aligned.
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
profiles and their wrapped-key crypto support are retained. No SSD writes or
filesystem changes were used for qualification.
