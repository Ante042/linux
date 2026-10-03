.. SPDX-License-Identifier: GPL-2.0-only

===========================================================
Experimental mixed-granule userspace on arm64 kernels: design
===========================================================

Status and scope
================

This work starts from Aurora's M5 Pro branch, commit
4832ab22bb9080d82c29f0ca4ee686492f0552f8. The experimental branch is
``mm-mixed-userspace-abi`` in ``aurora-silicon/linux-aurora``.
It does not include the source worktree's uncommitted display changes.

The experimental kernel now executes opted-in 4K EL0 processes alongside
ordinary native-granule processes on 16K and 64K kernels. Opt-in 16K EL0
processes also run on 64K kernels. Static 4K-aligned ELF and dynamically linked PIE
executables pass the basic page-contract tests in a VM, including fork/COW,
independent permissions, shared mappings and signal return. One-shot exec
selection, large argument/environment copies and transitions back to the
native ABI are tested. Real mixed-size processes share memfd/futex state and
use process_vm_readv/writev in both directions. See the latest milestone below
for exact evidence and limits; this is not yet a complete or battle-tested ABI.

User-reported gaming result (2026-10-03)
---------------------------------------

The user reports having tested this work for gaming and that it "works
super well". The report did not specify the device, kernel build, game
titles or measured frame rates. This adds user-reported gameplay evidence
to the reproducible VM results below; it is not an automated benchmark.

Apple-enabled build and process launch (2026-10-03)
--------------------------------------------------

On a native 16K arm64 kernel, enable ``ARM64_USER4K_EXPERIMENTAL`` with
48-bit kernel VA and PA configurations. ``MM_SUBPAGE`` is selected by the
option. Keep the ordinary kernel page-size choice at 16K. Neither the
private KUnit/test-device options nor ``arm64.user4k_test=1`` are needed
for application execution.

Build the launcher with ``make -C tools/aurora-userpages``. Opt a process
tree in using::

    tools/aurora-userpages/userpages 4k COMMAND [ARGUMENTS...]

``userpages --status`` reports the current process size and exec preferences.
``userpages native COMMAND`` clears an inherited alternative preference.
Ordinary processes continue to use 16K pages. See the launcher's README for
one-shot selection and the supported 64K-native combinations.

An Apple-enabled native16 image was built with Rust, Asahi DRM, Apple DART
and KVM, using the available M2 Pro's kernel configuration as the starting
profile. Private mixed-page test devices were disabled. That same image
booted in a QEMU virt guest and passed native/4K static and dynamic execution,
large argument transitions, proc/memory interfaces, and mixed 16K/4K shared
futex/COW and process_vm copies. The actual launcher passed native/4K/native
transitions, and FEX passed the x86 memory/protection/COW/JIT/re-exec contract
with its AArch64 processes reporting 4096-byte pages. These are VM results;
these automated tests do not cover Apple hardware boot or GPU command
submission. The subsequent user-reported gaming result is recorded above.

The Apple profile exposed a link-layout failure: the mixed-granule zero page
requires 64K BSS alignment while KVM aligned ``__bss_start`` only to the
native 16K page. Kallsyms growth could then leave ``__hyp_bss_start`` at a
different address and fail the existing linker assertion. The linker script
now uses the greater of the native page size and ``ALIGNOF(.bss)`` for the
host BSS start. The assertion remains enabled. The full image links with
identical host/hypervisor BSS starts; focused LLVM and GNU linker fixtures
cover native4K, native16K, mixed16K/64K alignment and native64K.

System V shared memory across process granules (2026-10-03)
----------------------------------------------------------

SysV attach and detach now use the calling mm's page granule. The arm64
attach-alignment hook scales the native and compat rounding rules only for
SysV attachments; the global SHMLBA constant remains unchanged for kernel
allocation users. AArch32 keeps its four-process-page rounding rule, including
the existing native-page restriction when 4K translation is unavailable.

Detach compares both the native file index and subpage byte offset when
recognizing fragments left by mprotect or partial munmap. Its segment extent
is rounded to the process granule. This preserves shared-object identity and
removes all matching fragments without including adjacent mappings.

The old kernel rejected valid smaller-page fixed and rounded attaches and
could reject detach of an automatically placed smaller-page alias. The new
PID1 fixture passes native16/4K and native64/16K/4K mapping cases, partial
unmap/protection splits, alias lifetime and all directed mixed-granule SysV
sharing pairs, including attachment after IPC_RMID while another alias lives.
AArch32 automatic4K passes on both builds. Capability-masked no4K models pass
native16/native64 AArch32 attach/detach and retain rejection of 4K-only
mprotect requests. Feature-disabled arm64 and x86 SysV objects also compile.
The fixture requires CONFIG_SYSVIPC and runs only in a disposable guest.

The Apple-enabled ``7.1.12-aurora-mixed-mm2`` Image also passes the SysV
fixture, the existing native16/4K static/dynamic execution suite, and the
Windows memory contract through the existing private Wine/FEX fixture in a VM.
These are VM tests; the subsequent user-reported gaming result is recorded above.

Perf ring mappings and sampling (2026-10-03)
-------------------------------------------

Ordinary perf mappings now place the data ring after one process-sized
metadata page. The first mapper fixes that layout for the ring's lifetime;
an attempt to mmap the same ring from a different process granule returns
EINVAL. Same-granule aliases remain supported. This restriction is specific
to perf ring layout and does not prohibit mixed-granule shared memory.

The requested data capacity and wrap boundary are independent of physical
allocation. Small rings occupy native backing pages but wrap at their exact
requested capacity; larger rings retain native allocation chunks. The
vmalloc backend retains its contiguous output buffer. Both allocation and
locked-memory accounting use native pages, including a full backing page
for metadata. Only the first process page is writable by userspace.

AUX offsets retain their complete byte position, including the subpage
part, for mapping selection and lifetime management. AUX buffer sizes
remain native-page multiples because PMU setup_aux callbacks consume
native-page arrays. This does not establish hardware AUX tracing support:
the current runtime fixture tests byte-offset routing to a software event's
expected EOPNOTSUPP response, not a hardware PMU's AUX allocation or output.

The former kernel rejects conventional metadata-only and two-data-page
ring mappings in smaller-granule processes. The fixed kernel passes real
software page-fault sampling on native16/4K and native64/16K/4K, including
records crossing the ring boundary, rings smaller and larger than native
pages, metadata writes, read-only data, alias lifetime and incompatible
mixed-granule alias rejection. Both ordinary and vmalloc backends pass.
Freestanding AArch32 probes pass automatic 4K and capability-masked
native16/native64 fallback, including mmap2's fixed 4K offset units.
Feature-disabled arm64 and x86 perf objects compile. These are disposable
VM tests, not Apple hardware, game or performance qualification.

Protection-walk geometry cost (2026-10-03)
-----------------------------------------

The PTE protection walk now decodes its immutable mm granule once and
passes that value to the inlined batching selector. This removes repeated
granule decoding and restores an inline small-folio path; the batching
rules, permission checks and TLB operations remain the same.

Matched non-debug, native16 TCG kernels before and after this change used
the same configuration and benchmark executable. Three paired boots with
five retained trials each measured an 8MiB protection change about 5.8%
faster for native16 processes and 2.5% faster for 4K processes. These are
emulator results, not Apple hardware or game performance. Other timings
varied, including a roughly 1.4% increase in the 4K fork/COW measurement;
the result does not establish a universal speedup or zero native overhead.

The existing protection, COW-batching, accounting and migration KUnit
cases pass on native16 and native64 KASAN/lockdep kernels. Current-source
EL0 userfaultfd and SysV fixtures also pass, covering native16/4K and
native64/16K/4K permissions and AArch32 SysV execution. Feature-disabled
arm64 and x86 protection objects also compile.

Legacy AIO ring relocation (2026-10-03)
--------------------------------------

The AIO completion ring retains native-page backing while its virtual
address follows the process granule. The disposable ``aio-ring-init``
fixture checks small and large rings in AArch64 native16/4K and
native64/16K/4K processes. Smaller-granule tasks move the ring to an address
that is aligned to their own granule but not to the native kernel page.

Buffered memfd writes complete through the relocated ring, including a
completion queued before the move. Subsequent reads verify exact bytes
across process-page boundaries. Eventfd notification, context destruction,
removal of the ring mapping and adjacent guard preservation also pass.
The Apple-enabled mm4 Image passes the native16/4K fixture in a VM.

NUMA policy ranges and physical placement (2026-10-03)
-----------------------------------------------------

``mbind`` and ``set_mempolicy_home_node`` now align anonymous policy ranges
using the process granule. NUMA PTE walks advance and batch at that granule,
including native folios represented by several smaller leaves. Address-based
``get_mempolicy(MPOL_F_NODE)`` queries retain an exact fragment reference rather
than rounding the address to a native page and using legacy page-array GUP.

Physical NUMA placement remains a native-folio property. Migration of a 4K
leaf can move its whole 16K or 64K owner, including neighbouring leaves;
it preserves their bytes and does not change their individual VMA policies.
``MPOL_MF_MOVE`` retains the existing conservative shared-folio check.
The migration tests use the privileged ``MPOL_MF_MOVE_ALL`` flag.

Shared shmem policy is indexed by native page-cache units. Policy changes
must cover whole native file-page units within each affected VMA; partial
units return EINVAL before that VMA is split or its policy is replaced.
This is a placement restriction, not a restriction on sharing data across
process granules. Shared policy lookup includes the VMA's subpage file offset,
so an alias beginning partway through a native page selects the correct
policy after crossing into the next one. Interleave indices likewise retain
native allocation units and account for the subpage offset.

The disposable ``numa-policy-init`` and ``compat-numa-probe`` fixtures require
two memory nodes, CONFIG_NUMA, CONFIG_NUMA_MIGRATION and CONFIG_COMPAT. Put
both executables in the initramfs root, with the former named ``/init`` and
an empty ``/proc`` directory plus ``/dev/console``. Native16/4K and
native64/16K/4K KASAN/lockdep guests pass fine-range policy updates, placement,
strict-policy checking, native-owner migration with byte preservation, and
shared-file offset/rejection tests. AArch32 passes with automatic 4K pages
and with native16/native64 fallback when 4K translation is capability-masked.
The latter models use ``id_aa64mmfr0.tgran4=f`` and retain rejection of
4K-only policy addresses. All cases verify that both memory nodes exist.

The pre-change control rejects valid smaller-granule anonymous policy ranges;
``numa_fixture_baseline=1`` runs only that benign control check. The x86 NUMA
policy/shmem objects and feature-disabled arm64 shmem object also compile.
The 64K guest's boot-time ``debug_vm_pgtable`` test failed to allocate a
contiguous 512MiB block on both the unchanged control and the new kernel;
its dependent huge-page tests are therefore not qualified. The isolated
64K NUMA runs use ``initcall_blacklist=debug_vm_pgtable`` to omit that separate
boot test while retaining KASAN and lockdep. This does not fix its allocation
failure. The 16K runs retain the boot test. Automatic NUMA balancing was
disabled in these guests and remains unqualified; these results do not cover
broad pressure or memory hotplug. The published Apple mm4 configuration has
NUMA disabled and its artifact is unchanged.

Current userfaultfd support (2026-10-02)
---------------------------------------

Smaller-than-native processes support anonymous missing-page COPY/ZEROPAGE,
synchronous/asynchronous write protection (including unpopulated leaves), and
POISON. Private shmem mappings support missing-page COPY/ZEROPAGE without
populating or modifying the shared file. Shared and private shmem mappings
support minor faults, CONTINUE and write protection using each leaf's physical
byte offset. The pager may have a different granule from the target process.

Shared shmem MISSING registration/COPY/ZEROPAGE is now advertised through
MISSING_SHMEM for alternative processes. Coverage includes partial hole-punch,
shrink/regrow, retained large folios, pinned I/O, concurrent refill, allocation
failures, and COPY into swapped partial cache pages under memcg pressure.
Hugetlb userfaultfd and larger-than-native processes remain unsupported.
Shared COPY uses the requested user granule rather than a native-size copy;
these bounded tests do not establish full shared-file ABI qualification. The combined
WP_HUGETLBFS_SHMEM feature bit remains unadvertised for alternative processes
because alternative hugetlb support is absent; shmem WP is available through
successful range registration and its returned WRITEPROTECT ioctl bit.

COPY batches reserve only requested leaves within one native linear index and
transfer individual slot references to their PTEs. Source fault retries discard
unused reservations, copy into fresh unexposed backing outside VMA locks, then
revalidate VMA identity and byte position before installation. Existing slots
retain independent COW, mlock and rmap accounting. Packing was checked through
actual pagemap physical addresses; this is not a throughput benchmark. Separate
single-leaf COPY requests are not currently coalesced into a shared allocator
pool, so sparse demand paging can still consume a native folio per filled leaf.

Exclusive anonymous UFFD MOVE is now implemented and advertised for user
granules smaller than the native kernel granule. It moves one leaf's ownership
without copying its bytes or changing its physical address. Source-slot pins and
fork sharing reject the operation; pins in a different slot do not prevent it.
Present, swapped, zero-page and hole paths use the process granule. See the MOVE
milestone below for the bounded runtime matrix and remaining qualification work.

Generalization requirements (2026-09-30)
---------------------------------------

The implementation must cover both directions: smaller user granules within
native physical pages, and larger user granules backed by multiple native pages.
A 64K user leaf on a 16K kernel requires aligned contiguous backing and complete
ownership of all covered physical units. File mappings must retain byte identity
with smaller-granule aliases through writeback, EOF, truncation, invalidation,
COW and pinning. The 64K-native implementation now supports sixteen 4K
slots, including table ownership and swap offsets. Generalizing the current
4K/native geometry now includes 16K subranges on 64K kernels. Larger-than-native
user leaves remain unfinished.

Target matrix: native kernel granules 4K/16K/64K, each with native-default
AArch64 and opt-in supported user granules 4K/16K/64K. Hardware capability and
address-size constraints apply independently; unsupported modes must not be
advertised. AArch32 is an execution-state capability, not a platform-name check.
AArch32 defaults to 4K where supported and otherwise retains existing behavior.
Asymmetric CPU eligibility, affinity changes and hotplug must preserve the
selected translation granule. AArch32 mmap2 offsets remain in 4K ABI units,
including large file offsets, regardless of kernel granule.

Exec selection must use the final executable format, including interpreter and
script transitions, before committing the new mm. The current temporary stack
already has mappings before ELF class is known; its geometry cannot be changed
by flipping a flag. Late selection requires safely transferring staged arguments
into a newly allocated mm with the desired geometry. Compat setup must retain
the chosen granule alongside MMCF_AARCH32. Compat signals, vDSO/kuser mappings,
auxv, ASLR, core dumps and AArch32/AArch64 exec transitions require runtime tests.

Ryan Roberts's 2024 RFC uses boot-selected kernel geometry and runtime folding;
its PAGE_SIZE_MIN/MAX and geometry work are relevant precedents. This project
additionally needs immutable per-mm translation geometry and explicit physical
ownership units. Replacing the kernel's PAGE_SIZE with a per-task value is not
a valid design. Reference: https://lwn.net/Articles/993990/ (author's cover letter;
original Message-ID 20241014105514.3206191-1-ryan.roberts@arm.com).

The tested matrix now includes 16K and 64K kernels with native/4K AArch64
processes, AArch32 automatic 4K selection, and capability-masked AArch32
native-16K/native-64K fallback, plus 16K EL0 processes on a 64K kernel. User
granules larger than the native kernel granule remain unimplemented. Current capability
selection uses the sanitized system-wide granule intersection; exploiting a
larger capability set restricted to an asymmetric AArch32 CPU domain remains
unimplemented.

The implementation includes per-mm table geometry, packed lower tables,
TG0/TTBR0 switching with hardware/software PAN, anonymous fragment ownership,
COW, reverse mapping, reclaim, swap and migration. Native physical pages,
page-cache folios and kernel mappings retain the build-time native granule. Internal fixtures cover
substantially more cases than the current EL0 suite. Fixture success alone
does not establish broad application or concurrent-pressure correctness.

The option defaults off. Legacy page-array GUP consumers, several memory
syscalls, device/GPU mappings and broad stress coverage remain unfinished.
Core dumping and the tested proc accounting interfaces now use the target
process granule; their exact coverage is recorded below. No performance, M5 hardware, production-readiness
or complete NCE compatibility claim is made. Milestone sections below retain
their historical status statements; the latest exec milestone supersedes
earlier statements that EL0 activation was absent.

The Apple candidate keeps NUMA disabled. Separate two-node native16/native64
guests now cover explicit policy ranges, placement and migration as described
above. Automatic NUMA balancing and broader NUMA workloads remain unqualified.

The expanded objective is concurrent 4K, 16K and 64K userspace granules on
arm64, with the kernel/physical allocator granule independently fixed for the
boot. AArch64 defaults to the native granule (16K on the current Apple target);
alternatives remain opt-in. AArch32 selects 4K when the eligible CPU domain
supports both AArch32 EL0 and 4K stage-1 translation, retaining the established
native-granule behavior otherwise. The asymmetric-domain optimization and
remaining granule combinations are still requirements; homogeneous AArch32
automatic-4K and native-fallback paths have runtime coverage on both builds.

Units and invariants
====================

There are three distinct units:

* The kernel's base physical allocation, ``struct page`` and PFN unit:
  the build-time native granule (16K or 64K in current mixed-granule builds).
* A process's translation and user mapping unit: 4K, 16K or 64K, subject to
  the supported combinations above and immutable for the lifetime of its mm.
* The folio's allocation, ownership and reclamation unit: one or more
  physical base pages. This does not change merely because a user mapping
  has a smaller translation granule.

Do not redefine PAGE_SIZE dynamically or globally change PAGE_SHIFT to 12.
Do not use native ``pte_pfn()``/``pfn_pte()`` to encode/decode a 4K slot:
the low two subpage bits must survive separately from the 16K PFN. A future
mapping reference must identify the owning folio and an explicit byte offset.
Do not silently widen an unmap, permission downgrade or COW operation to
cover neighboring mappings.

Proposed allocation policy: pack compatible private 4K allocations into
16K owners rather than waste a complete 16K allocation for each 4K mapping.
Track slot occupancy and references; a folio is not freeable while any slot
has a mapping, pin or in-flight reference. Swap entries name storage and do
not keep physical backing alive after swap-cache eviction. Sharing individual
slots across unrelated owners is not required for the first allocator.
Zero newly exposed bytes and never expose an unallocated neighbor. Reclaim
and migration may operate on the whole folio, but must preserve individual
slot contents, mappings, permissions and COW relationships.

The test packs its four 4K translation tables into one native 16K page,
exercising address bits 13:12 in table descriptors as well as leaf PTEs.
Its native 16K tables each occupy a separate allocation. This is test
scaffolding, not a production allocator. Its full local TLB flushes establish
a controlled test context; they are not a proposed context-switch fast path.

The new core-MM prototype allocates a 4K-aligned root from a dedicated slab
cache. Lower-level tables now pack four logical 4K fragments into native
16K ptdesc owners. Each mm has separate PTE, PMD and PUD caches under its
page_table_lock, so parent and child tables do not share a fragment-owner
lock. Unissued slots hold references until mm destruction; issued slots
are never recycled within a live owner. A racing new allocation may keep
one private owner if another allocator populated that level's cache first.
Improving that race's packing efficiency remains possible future work.

The sparse 4K MM test uses sixteen logical tables: 64 KiB of installed
table accounting and five native owners (80 KiB of backing including cached
slots), down from the earlier prototype's 256 KiB backing. These counters
have different meanings: actual owner allocations are charged once through
GFP_PGTABLE_USER; mm page-table bytes count installed logical tables. Root
slab accounting remains separate. Native table allocations retain their
original sizes and the native test still observes fourteen native tables.

The pgtable_t token preserves a fragment's address bits 13:12, while locks
and lifetime management use the native owner. TLB batches carry tagged
fragment addresses through their existing TLBI/RCU ordering. Deferred PTE
reclaim marks the owner for RCU release, dropping each fragment's reference
without queueing multiple callbacks on the same owner. Tests cover final
release, deferred release, cache-reference teardown and native THP table
deposit/withdraw. Native PT_RECLAIM, THP and contiguous-PTE support remain
enabled in the test configuration. This is a page-table allocator, not an
allocator or ownership model for 4K anonymous data.

Subpage data ownership
=====================

``mm/user-subpage.c`` manages explicit slot references independently of
struct page's native PFN unit. The caller supplies a fresh, unexposed,
order-0 folio and transfers its reference only on success. Allocation and
memcg/NUMA policy remain the caller's responsibility; a pool must cover a
compatible policy/charge domain. The prototype does not yet bind pools to
VMAs or implement that policy selection in a fault path.

Metadata is separate from folio->mapping, folio->private and folio->swap.
Each of the four slots has its own reference count and physical byte
offset. Every live slot reference also retains the native folio. The
supplied folio reference transfers to its first reservation; there is no
permanent extra allocator reference while slots are live. Allocation reserves a
free slot under the pool lock and zeros its bytes before returning it.
An extra reference keeps a slot unavailable after another holder releases
it. This applies to aliases and to future pin/deferred-unmap references;
a raw folio reference alone is not a substitute for a slot reference.

A page-extension pointer associates each native physical page with its slot
owner without changing struct page or taking a folio-private field. Physical
lookup retains bits 13:12 and acquires a reference only to a live slot.
Reservation keeps the slot reference count at zero while initialising data;
a release publication and acquire lookup prevent exposure of partially
initialised contents. The occupancy bitmap protects an in-progress
reservation against concurrent pool closure.

An owner is unbound before its folio reference is dropped, and its metadata
is freed after an RCU grace period. Lookup requires the caller to stabilise
the physical allocation (for example, a referenced folio or a live PTE under
its lock); RCU alone protects metadata, not arbitrary physical PFN lifetime.
Lookup obtains a speculative native folio reference before acquiring the
slot reference and drops it if the slot is no longer live. Speculative PTE
users still need to recheck
their PTE after acquiring a slot: physical lookup identifies the current
live slot, not the historical identity of an unlocked mapping. Supplying
already-owned backing is rejected before any data is zeroed.

The extension is selected only by the experimental configuration. A 512 MiB
guest reports 262,144 bytes allocated for page extensions with this client
alone. This is metadata overhead, not a measurement of native-process
throughput or latency with the feature enabled.

The last slot reference allows reuse only after the caller has satisfied
its PTE/TLB lifetime obligation. The allocator cannot infer TLB completion.
The copy helper reserves a different slot and copies exactly 4K, including
when source and destination occupy different slots of one native folio.
It does not make the source immutable or implement write-fault/COW decisions;
the caller must stabilise the source and mapping permissions.

Closing a pool rejects new reservations. Existing references outlive both
closure and the originating caller. The pool retains no mm/VMA pointer.
Its lock is IRQ-safe so deferred releases can run from RCU callbacks, and
closure is constant-time: only a wholly unissued backing folio needs
immediate disposal. Other owners are released on their last live slot.
No global allocator lock or always-flush-TLB path is added.

The common reverse-map PTE walker now uses the target mm's table geometry
and stride. PVMW_SUBPAGE requests one present PTE with an exact native-PFN
plus byte-offset match. It cannot match a neighbor solely because both
addresses share a native PFN. Native-owner walks keep their original fixed
file-index bound while visiting multiple 4K PTEs; their bound must not advance
with each returned mapping. Tests cover exact matching with both locking
paths, a later alias beyond the bound, a 2 MiB table crossing, holes, and
the native 16K path. Explicit leaf helpers preserve physical bits 13:12
when constructing entries or extracting their address/protection fields.

This walker conversion does not attach anonymous reverse-map ownership to
slots. PVMW_SUBPAGE now matches migration PFNs plus slot offsets (see below), but
does not match real swap entries,
and subpage PMD mappings remain unsupported. File VMA offsets still need
their own byte component before a 4K-offset file mapping can be created.

Tests cover four slots sharing one folio, all physical offset bits,
zeroing on reuse, retained-reference reuse exclusion, copying without
neighbor modification, pool closure, deferred final release, rejection
of compound/shared-reference input folios, and concurrent allocation.
Physical lookup tests also cover duplicate-owner rejection and unbinding.
A reader races 2,048 alternating full-slot copies and owner retirement;
every acquired slot must contain one complete pattern. This test uses a
time bound and yields when the only destination is held by the reader.
An earlier fixed-count retry budget expired after one copy; that failure
is retained in kvm-rmap-races-hw-pan.log. The corrected test passes on both
single-vCPU and two-vCPU guests; it does not prove all possible races absent.
The user-copy fixture takes explicit slot references before creating PTEs
and drops them only after its private MM teardown and TLB completion.

The data folios are not on an anonymous LRU and have no normal rmap yet.
Do not publish this allocator through writable faults until reverse
mapping, reclaim/swap, per-slot exclusivity, pinning, fault races and
mapped-byte/backing-byte accounting are integrated. The current tests
do not verify memcg enforcement or application memory-pressure behavior.

Translation and context switching
=================================

Arm specifies independent TG0/T0SZ and TG1/T1SZ fields. At 48 VA bits the
four-level 4K table shifts are 39/30/21/12, whereas the four-level 16K shifts
are 47/36/25/14. Changing TG0 while retaining native-format tables is wrong.
Generic page-table accessors and every walker need an explicit mm geometry;
the kernel tables must continue to use native constants.

The production switch must keep TTBR0, TG0, T0SZ and the ASID interpretation
consistent through interrupts, preemption, uaccess, return to EL0 and
speculative walks. Review software PAN, KPTI, CnP, ASID rollover/pinning,
CPU hotplug, idle/suspend, EFI/idmap use, SVA/PASID and KVM host restore paths.
TCR.DS is shared by the two translation regimes: never clear it to obtain
a convenient 4K descriptor format while a DS-dependent TTBR1 is active.
Initial process support should reject incompatible CPU/configuration
combinations explicitly rather than partially enable them.

cpu_do_switch_mm() now synchronises the reserved TTBR0 before a TG0 change
and synchronises the new TG0 before installing the selected root. The mm's
existing ASID is retained; no new per-switch TLBI is added. Software PAN
selects TG0 at check_and_switch_context() even though user access or EL0
return restores TTBR0 later. The idmap and raw transition-table installers
restore native TG0, and idmap removal restores the active mm's TG0 even
when software PAN leaves TTBR0 reserved. A switch to init_mm selects the
native granule. T0SZ remains the existing fixed 48-bit userspace choice in
this experimental configuration; kernel TG1/T1SZ are preserved.

This is tested context-switch plumbing, not qualification of every entry,
EFI, suspend, hotplug, KPTI, ASID rollover, CnP or KVM-host path. The test
executes no 4K ELF image. The per-mm granule remains immutable and no user
activation interface exists.

All invalidation paths need the mm's granule, not just the context switch.
The non-range TLBI VA operand is in 4K units; range and TTL encodings need
their own granule-aware handling. Native 16K iteration would skip three
4K mappings out of four. Page-table freeing needs the same ordering and
RCU/TLB lifetime guarantees as the native path. Preserve native batching;
do not install full-TLB flushing as a production workaround.

The page/range invalidation helpers and deferred-unmap batches now use the
target mm's granule for strides, range operands and TTL hints. Generic PTE
gather ranges and the minimum flush stride also use mm geometry; physical
page-free batches retain native allocation units. Kernel and stage-2 callers
still explicitly select native geometry. Existing range batching, DSB
completion, ASID selection and paired KPTI invalidation remain in place.

Recording tests decode every emitted operand for 4K/16K/64K geometry,
range-capable and non-range algorithms, LPA2 alignment and different leaf
strides. They check contiguous coverage, hint/ASID encoding, a bounded
operation count and the top of the 48-bit address space. These tests execute
no TLBI instruction and do not claim hardware support for each combination.
The isolated 4K hardware test additionally removes leaves through the real
local-page, broadcast-range and deferred-batch helpers and verifies holes
and neighboring translations before restoring all mappings and registers.
It does not establish a remote CPU shootdown or production ASID rollover.

ABI direction
=============

Select the page granule when creating a new mm for exec. Do not convert
a live address space with existing VMAs, pins, threads or asynchronous I/O.
Fork inherits the granule. CLONE_VM shares the mm and therefore the granule.
A new executable selects its supported granule explicitly. The development
prctl described below is available only with the experimental config. Its
private numbers are not upstream assignments or a stable UAPI promise;
incomplete interfaces still prevent production use.

AT_PAGESZ must describe the process granule. ELF PT_LOAD alignment, file
offsets, interpreter placement, initial stack, brk, mmap/munmap/mprotect,
mremap, madvise, mincore, mlock, guard pages and signal/vDSO mappings must
all agree. Native 16K executables must retain their current behavior.
An executable's p_align alone is insufficient evidence that it requests
or requires a particular kernel ABI.

PAGE_SHIFT also describes accounting and file-cache units. Replacing every
occurrence with a process shift would corrupt those units. Keep file-cache
indices in native units and represent subpage file offsets explicitly;
audit VMA split/merge and reverse-map interval calculations accordingly.
Report mapped bytes and physically charged folio bytes separately where
their meanings differ. Charge the actual physical allocation exactly once.

Memory sharing policy
=====================

Mixed-granule sharing is not inherently prohibited. The target is a single
coherent backing object, with each mm exposing byte ranges in its own
granule. A 16K process can map a whole native page while a 4K process maps
individual slots of the same page; permissions are properties of each
mapping, not of the shared bytes globally.

Implement and test memfd/shmem first, then broader file mappings. The
first sharing test should use a 16K-aligned extent and four 4K aliases.
Write through either process, verify visibility through the other, and
exercise cross-process shared futexes. Canonical futex keys must describe
the shared object and byte offset; virtual page-size differences cannot
change the key. Private futexes need their own ordinary threading tests.

Partial EOF, truncate, hole punching, msync, dirty/writeback accounting,
MAP_PRIVATE COW and sealing must preserve byte-level object semantics.
Do not implement MAP_SHARED with private copies or round permissions up.
Unsupported mapping types must fail before a mapping is created. Temporary
restrictions are prototype limitations, not completion of the sharing goal.

Pinning and device mappings
==========================

GUP currently returns native ``struct page`` entries and iterates in native
PAGE_SIZE units. A 4K virtual interval can cross independently backed slots
within one such iteration. This requires an explicit subpage-aware range
representation, not merely returning the parent struct page. Cover both
fast and slow GUP, remote access, process_vm, ptrace, direct I/O, io_uring
registered buffers, RDMA, PFNMAP/MIXEDMAP and dma-buf/GPU mappings before
allowing their use on experimental mms. Early prototypes must reject
unsupported entry points before pinning or causing partial side effects.

Keeping kernel pages at 16K does not by itself solve device mappings.
Device/IOMMU granularity and ownership must be checked independently.
Do not expose neighboring private slots to a device or bounce-copy a shared
GPU buffer and claim equivalent coherence or performance.

x18
===

The linked XNU ``x18_toggle_4k.plist`` invokes ``vm_spawn_tool`` with the
``x18_toggle`` and ``x18_signal_handling`` tests. The corresponding
``tests/x18_toggle.c`` expects x18 to be cleared without the custom ABI and
preserved after calling ``os_set_custom_x18_abi_enabled(true)``. This does
not establish a Linux NCE runtime requirement.

This Linux base saves/restores x18 in the normal EL0 entry/return register
frame (arch/arm64/kernel/entry.S). The new page-contract selftest verifies a
sentinel across a raw syscall, a measured voluntary context switch, and a
signal whose handler overwrites x18 before sigreturn. All three checks pass
on the native 16K host and in the experimental 16K KVM guest. This is not
proof of a 4K process, ptrace behavior, or a particular NCE runtime. An
Apple-specific x18 mode, if needed by a runtime, needs a separate
demonstrated requirement and ABI.

Implementation gates
====================

1. Establish mixed-granule translations and independent permission changes
   in a disposable 16K VM with exact register restoration. Include negative
   controls so a skipped test cannot be reported as a pass.
2. Introduce explicit per-mm page-table geometry and subpage ownership with
   tests, preserving the native hot paths. Audit all generic/architecture
   walkers and invalidation callers before attaching 4K tables to an mm.
3. Boot a minimal 4K EL0 executable next to 16K processes. Implement fault,
   anonymous mmap, munmap, protection, exec, fork/COW, teardown, uaccess,
   signals and vDSO. Verify neighbor isolation and allocation failure paths.
4. Add coherent mixed-granule memfd sharing, futexes, file faults and
   truncation. Add reclaim, swap, migration and accounting tests. Refuse
   unsupported THP/hugetlb, KSM, userfaultfd and pinning paths until audited.
5. Add the required device/pinning paths and run real translation workloads.
   Measure native-process overhead, memory use, fault costs, context switches
   and target workloads against an unchanged baseline. VM success is not
   M5/Neo hardware qualification or proof of GPU/NCE performance.

Architectural test
==================

Build natively from this worktree, using an out-of-tree directory::

  python3 tools/testing/kunit/kunit.py config --arch=arm64 \
    --kunitconfig=tools/testing/kunit/configs/arm64_user4k.config \
    --build_dir=/path/to/build --make_options=LLVM=1
  make ARCH=arm64 LLVM=1 O=/path/to/build -j4 Image

The tiny VM config is for MM tests, not an Asahi deployment configuration.
Use the existing QEMU/KVM installation on a Linux arm64 host with /dev/kvm::

  timeout 90 qemu-system-aarch64 -machine virt,accel=kvm,gic-version=3 \
    -cpu host -smp 2 -m 512M -nodefaults -nographic -serial stdio \
    -monitor none -no-reboot -kernel /path/to/build/arch/arm64/boot/Image \
    -append 'console=ttyAMA0 arm64.user4k_test=1 kunit.filter_glob=arm64-user4k* kunit_shutdown=poweroff'

The hardware tests require ordinary direct PTE permissions, no CnP, DS=0, A1=1,
and 4K-granule support. These are explicit test limits, not production
workarounds. A skipped test establishes no mixed-granule behavior. Boot
again without the arm64.user4k_test parameter and verify the test skips.
The translation tests cover alternating native/4K roots, four adjacent
subpages with RO/RW/unmapped/alias descriptors, a single-PTE BBM replacement,
its changed physical address and write permission, stable kernel mappings
and restoration of TCR/TTBR0. They do not yet cover actual user execution,
execute permissions, remote TLB shootdowns or ASID rollover.

The context/uaccess case uses two kernel threads and two private mms.
Each thread performs sixteen borrowed-mm switches, migrates between the
guest's CPUs, sleeps with the mm borrowed, and checks four mappings each
round. It checks read-only and unmapped neighbors, successful writable
copies, exact physical backing and unchanged kernel TTBR1 base. Each worker
also exercises both the normal idmap and raw transition-table installers
from a 4K mm, checks native TG0, removes the idmap and repeats user access.
Copies run with page faults disabled: absent/RO mappings take exception
fixups without entering the unimplemented writable-fault/VMA paths.

The KUnit config enables ARM64_SW_TTBR0_PAN as a fallback. Hardware PAN
remains selected normally. With ARM64_USER4K_KUNIT_TEST and that fallback
enabled, the early CPU-feature override accepts ``id_aa64mmfr1.pan=0``
for explicit VM testing of software PAN. The override accepts only zero
and is absent from non-test kernels. Add it to the enabled VM command line
and verify the emulated-PAN boot message and software-PAN test result.
It is not a production setting or performance fix; its cost is unmeasured.

For a memory-sanitized ownership run, build separately with
``tools/testing/kunit/configs/arm64_user4k_data_kasan.config``. This selects
ARM64_USER4K_DATA_KUNIT_TEST and KASAN_GENERIC while disabling the raw
translation/context test suite. Boot its Image with the same initramfs
and ``kunit.filter_glob=arm64-user4k-data*``; no hardware-test boot opt-in
is required. This checks metadata lifetime, not 4K EL0 execution or
instrumentation boundaries between slots in one native allocation.

Process contract tests
======================

Build the userspace contract test natively::

  make -C tools/testing/selftests/arm64/user4k CC=clang \
    LDFLAGS=-static OUTPUT=/path/to/results all
  /path/to/results/page-contract

The default expectation is a 16K process. There are eleven checks covering
AT_PAGESZ/sysconf agreement, mapping alignment, protection boundaries,
neighbor preservation, fork/COW, unmap, shared aliases, cross-fork shared
data with local permissions, and x18. These currently test same-granule
sharing, not sharing between 4K and 16K processes.

``page-contract --expect-4k`` asserts a 4K ABI but does not enable one. It
must fail on the current kernel; the negative control currently exits 1.
Once an explicit exec opt-in exists, run it through that launcher and then
add mixed-granule peer/futex/truncate tests. Never report this negative
control as a successful 4K process test.

The static test can be /init in an otherwise empty initramfs; as PID 1 it
powers the VM off after printing results. Use gen_init_cpio with a /dev
directory, /dev/console (character device 5:1), and the executable at /init.
Pass ``-initrd /path/to/initramfs.cpio`` and ``rdinit=/init`` to QEMU, and
omit ``kunit_shutdown`` so the kernel enters userspace after KUnit. Capture
both KUnit and userspace output. The asynchronous kernel console can
interleave its output with /init; check complete test results and counts,
not just QEMU's exit status.

Recorded development results (2026-09-30)
========================================

On the native arm64 Linux host, clang/LLD 22 built the VM kernel. KVM used
two vCPUs, host CPU features and 512 MiB of RAM. The packed-table geometry
and translation tests both passed. The 16K process contract passed all
eleven checks after the architectural test restored the ordinary context.
The same eleven checks passed on the unmodified host kernel. The final
no-opt-in VM boot skipped the translation test and passed all eleven
process checks with the default 16K ABI. No 4K Linux process has run yet.

The subsequent core-MM implementation passes five KUnit cases, including
the two MM geometries, common range walking, partial and full teardown,
hardware translation through Linux-allocated 4K tables, and the original
packed-table test. The sparse cases cross PTE, PMD, PUD and root boundaries.
The range callback tests account for every byte of the 48-bit range as
either a present mapping or a hole. Teardown checks both preservation of
a shared boundary table and removal of exactly one complete PTE table.
The hardware case validates all eleven sparse translations for EL0 reads
and writes before restoring TCR, TTBR0 and PAR; TTBR1 is checked unchanged.

The core-MM geometry-stage VM run passes all five cases and all eleven native process
checks. With the boot opt-in absent, three software cases pass, two hardware
cases explicitly skip, and all eleven native process checks still pass.

The subsequent fragment/read-fault/TLBI stage adds fragment lifetime,
anonymous read-fault, native huge-page table-token, TLBI operand and gather
geometry tests. The anonymous fixture initially returned VM_FAULT_SIGBUS
because vm_area_alloc() installs dummy vm_ops; marking the test VMA
anonymous selected the intended fault path. This was a fixture correction,
not evidence of a writable-fault implementation.

The final fragment/read-fault/TLBI VM passes all ten kernel tests (eight
MM/translation cases plus two TLBI/gather cases) and all eleven native
process checks. Without arm64.user4k_test=1, eight software cases pass,
the two hardware cases skip, and all eleven native process checks pass.
The Image was built with THP, PT_RECLAIM and ARM64_CONTPTE enabled. There
is still no 4K userspace process, writable 4K data allocator or measured
application performance result.

The later context/uaccess stage passes all eleven kernel cases and eleven
native-process checks in both hardware-PAN and explicitly forced software-PAN
KVM boots. Each boot checks 32 borrowed-mm switches, 128 per-mapping user-copy
results, and four idmap/transition-table cycles. Without the hardware-test
opt-in, eight kernel cases pass, three hardware cases skip and all eleven
native-process checks pass. The five feature-disabled MM objects remain
byte-identical to the original M5 base in the audited executable sections.

The data-ownership stage adds six cases. Both hardware-PAN and software-PAN
boots pass all seventeen kernel cases and eleven native-process checks.
The context/uaccess fixture now uses the packed data references for its 4K
physical backing. The no-opt-in boot passes fourteen software cases, skips
three hardware cases and passes all eleven native-process checks.

A separate KASAN_GENERIC/INLINE build passes the six ownership cases and
all eleven native-process checks, with no sanitizer report. The allocator
object contains ASan load/store/memcpy/memset instrumentation references.
This establishes tested metadata lifetime paths, not a proof of all races
or a sanitized 4K-process implementation. The five audited feature-disabled
MM objects remain byte-identical to the M5 baseline.

The subsequent physical-lookup/reverse-walk stage passes 23 kernel cases
and eleven native-process checks with both hardware PAN and forced software
PAN. Without the hardware opt-in, twenty cases pass and three skip. The
eight data-ownership cases, including concurrent lookup/copy/retirement,
also pass on a single-vCPU guest and in the separate KASAN build, with no
sanitizer report. Seven audited feature-disabled objects (the previous five
plus mm/page_vma_mapped.o and mm/rmap.o) have byte-identical executable
sections to the M5 baseline. None of these results establishes a 4K EL0 ABI.

The VMA-offset stage adds six cases covering byte-offset arithmetic,
interval-tree candidates, real VMA duplication/splitting, complete and
partial merges, and VMA copying/shrinking. With DEBUG_VM, VM maple/RB tree
validation and PROVE_LOCKING enabled, hardware and software PAN boots pass
29 kernel cases and eleven native-process checks without reported warnings.
The default boot passes 26 cases, skips three hardware cases and passes the
same native checks. KASAN passes eight data cases and eleven native checks.
The data suite now drains batched kfree_rcu() metadata frees before the guest
can power off; earlier no-report results did not establish that every
batched metadata free completed before shutdown.

The existing host-side VMA suite passes all 27 cases under ASan/UBSan. That
suite executes VMA code with userspace stubs and a modeled PAGE_SIZE of 4K;
it is not a 4K process on this kernel. Its liburcu dependency was extracted
locally after checking the package against the pacman database SHA-256.
The changed generic MM objects also compile for x86_64. The seven audited
feature-disabled arm64 objects remain byte-identical to the base in their
executable sections; this audit does not cover every VMA object or measure
feature-enabled native throughput.

The geometry refactor parameterizes the existing compile-time formulas;
native callers still pass PAGE_SHIFT. The native context.o, mmu.o, pgd.o
and proc.o hashes were identical before and after this refactor in the
same configuration. This narrow code-generation check is not a whole-system
performance benchmark.

For the core-MM changes, separate feature-disabled builds of this tree and
the original M5 base produced byte-identical executable sections (including
init/unlikely sections where present) in mm/pgtable-generic.o, mm/pagewalk.o,
mm/memory.o, arch/arm64/mm/context.o and arch/arm64/mm/pgd.o. The JSON report
records sizes and hashes. This proves no code change in those sections with
the feature disabled; it does not measure the feature-enabled native path.

The comparison was repeated after fragment allocation and TLBI changes.
An initial TLBI refactor evaluated the range-capability alternative into a
bool outside the loop, adding runtime flag handling to native code. Keeping
the predicate at its original call site restored byte-identical executable
sections in all five audited objects. The comparison configuration has THP
disabled; native THP table deposit/withdraw is separately exercised by the
THP-enabled KUnit guest, not covered by that byte comparison.

One earlier clang build exited with SIGBUS (exit 135) while edits overlapped
that build. Its log and generated reproducer were preserved. Builds after
the source was frozen succeeded. The SIGBUS cause was not independently
reproduced; this does not establish a hardware or page-cache fault.

Development paths on this host:

* Source: /home/dj/Work/linux-user4k
* Native build: /home/dj/Work/user4k-build
* Native baseline checkout/build: /home/dj/Work/linux-user4k-baseline and
  /home/dj/Work/user4k-baseline-build; the feature-disabled candidate build
  is /home/dj/Work/user4k-native-build.
* Current fragment/read-fault/TLBI evidence:
  /home/dj/Work/user4k-results/kvm-fragments-tlb-final.log,
  kvm-fragments-tlb-default.log and native-fragments-code-comparison.json.
  The fragments-tlb-v1 bundle freezes the Image, config, initramfs, selftest,
  logs, comparison report and complete source patch with SHA-256 hashes.
* Context/uaccess evidence: kvm-context-idmap-hw-pan.log,
  kvm-context-idmap-sw-pan.log, kvm-context-default.log and
  native-context-code-comparison.json in the same results directory.
  The context-uaccess-v1 bundle freezes this stage's artifacts and patch.
* Data-ownership evidence: kvm-data-lifetime-hw-pan.log,
  kvm-data-lifetime-sw-pan.log, kvm-data-default.log, kvm-data-kasan.log,
  data-kasan-instrumentation.txt and native-data-code-comparison.json.
  The data-ownership-v1 bundle freezes both tested Images/configs and
  the source patch. The separate sanitized build stays at
  /home/dj/Work/user4k-kasan-build.
* Physical lookup and reverse-walk evidence: kvm-rmap-races-final-hw-pan.log,
  kvm-rmap-races-final-sw-pan.log, kvm-rmap-races-final-default.log,
  kvm-rmap-races-up.log and kvm-rmap-races-kasan-final.log. The
  lookup-rmap-v1 bundle freezes this stage's Images/configs, logs, comparison
  reports and complete source patch. Earlier failed fixed-retry-budget
  evidence remains in kvm-rmap-races-hw-pan.log.
* VMA offset evidence: kvm-vma-drain-hw-pan.log, kvm-vma-drain-sw-pan.log,
  kvm-vma-drain-default.log, kvm-vma-drain-kasan.log,
  vma-host-copy-shrink-regression.log, build-x86-vma.log and the two
  native-vma-offset comparison reports. The vma-offset-v1 bundle freezes
  this stage, including the earlier failed split test in
  kvm-vma-offset-hw-pan.log and its corrected successor.
* Earlier geometry-stage evidence: /home/dj/Work/user4k-results/kvm-mm-hardware.log,
  kvm-mm-default.log and native-code-comparison.json. The frozen
  mm-geometry-v1 bundle contains the Image, config, initramfs, selftest,
  hashes, logs and source patch for this tested stage.
* Earlier evidence: kvm-packed.log, kvm-contract.log,
  kvm-default-contract.log, native-contract.log and native-reject-4k.log.
  Earlier hashes naming files in the warm build directory are historical;
  that directory is reused by later builds.

Next implementation boundary
============================

The generic ``pgd_offset(mm, address)`` has an mm argument, but
``p4d_offset``, ``pud_offset`` and ``pmd_offset`` do not. Lockless variants
can be handed a local copy of an entry, so inferring the granule from the
pointer's backing page is not generally valid. Never infer the target mm
from ``current->mm``: remote walkers and kernel mappings need their actual
target. Explicit mm-aware geometry/walker interfaces with native fallbacks
now exist, and allocation/common range walking/table teardown use them.
This conversion is not complete: fault, fork, zap, GUP, folio walking,
reverse mapping and other callers still contain native-only assumptions.
Do not attach a 4K mm to a userspace task until these paths are converted
or have explicit, tested rejection paths for unsupported operations. Only
the bounded kernel-worker fixture may borrow a private mm, with faults
disabled and its mappings and lifetime controlled by the test.

``MMCF_USER_4K`` is currently set only by KUnit on an empty, isolated mm.
The production exec/fork initialization path still needs a transactional
granule selection before root allocation, and arch_setup_new_exec() must
not erase it. Scheduler/uaccess TG0 selection is implemented and tested
with borrowed mms, but actual 4K EL0 entry/return is still untested. The TLBI
helpers now have explicit geometry, but their unconverted callers still
require an audit of ranges and strides.

File-backed VMA offsets use an explicit subpage byte component while
the cache's pgoff_t remains in 16K units. VMAs now carry that component in
vm_subpage_offset, with vm_page_offset helpers for carrying, borrowing and
inverse byte-address lookup. Duplication, splitting, existing-range merging,
copying and shrinking preserve the pair. Interval-tree bounds and linear
page-index lookup include the component. The first split test exposed a
missing explicit copy in vm_area_init_from(); that copy is now implemented.
Mmap input plumbing, driver descriptors, stack growth, mapping accounting
and file fault/unmap callers still need conversion. Subpage anonymous ownership must account for
reverse mapping, COW, dirty/accessed state and allocation failure before
runtime opt-in can become reachable.

Anonymous ownership and reverse mappings
----------------------------------------

Every slot reference now retains its native folio. Tests check reference
balance across allocation, lookup, copying, reuse and closure. Exact-offset
allocation and copying reject an occupied requested offset even when other
slots are free; supplying another compatible folio permits that offset.

The anonymous rmap attachment helper preserves the native
folio->mapping/index relation: one anon_vma root and native linear index,
with a physical slot matching the VMA's linear byte offset. It rejects a
different root, index or offset. The owner pins the anonymous root until
its final slot is released, including when every VMA has already disappeared.
Per-slot mapcounts accompany the native folio mapcount. No PageAnonExclusive
claim is made for an owner whose slots may have different COW relationships.
Mlocked and droppable mappings are not accepted by this internal helper yet.

The folio aging walk now visits all constituent 4K PTEs rather than stopping
after one PTE for an order-0 native folio. A private KUnit fixture attaches
four slots to a real anonymous VMA and checks mapcounts, reference counts
and aging of all four entries. A fork-related fixture establishes native
anon_vma chains before attaching a second VMA, checks eight shared PTEs,
and destroys the parent while retaining the child. Another fixture removes
every VMA while holding a slot and checks the retained root and rejection
of attachment to an unrelated root. These fixtures passed DEBUG_VM and
lockdep in ``kvm-anon-fork-link.log`` (33 kernel tests and 11 native ABI
tests). They are not tests of normal fork's page-table copying or 4K EL0.
Earlier fixture setup failures are retained in ``kvm-anon-root.log``
(wrong lock/attachment ordering) and ``kvm-anon-fork-order.log`` (using the
fresh-VMA insertion API for an already cloned anon_vma).

Compatible neighboring mappings can share an owner, while partial COW
selects another owner when that exact slot is occupied. Normal fault-path
selection of allocation/policy/memcg domains is still unimplemented. This
approach trades arbitrary sparse-slot packing for retaining the existing
anonymous interval-tree representation; its memory and throughput costs
are unmeasured. Later stages below add exclusivity, pins, reclaim and
migration; remaining rmap callbacks and MGLRU still need an audit. Swap entries must
retain the slot offset without retaining a physical allocator reference
after the backing folio has been reclaimed. Neither the current allocator
nor this design permits exposing an unreclaimable writable ABI.

Anonymous unmap and TLB retirement
----------------------------------

The normal present-PTE zap path now removes the exact anonymous slot's rmap
and transfers its mapping reference to mmu_gather. Native data-page tokens
and slot tokens share the existing gather batches; a private, disjoint tag
is consumed before the native page-release functions. Native page/count
pairs remain intact, including count payloads whose numeric value includes
the slot tag bit. Allocation pressure retains the existing flush-and-retry
behavior. Slots are released after the gather's TLB invalidation, not when
the PTE is merely cleared. No per-slot synchronous-flush workaround is used.

Zap traversal uses the target mm's geometry and leaf stride. Tests remove
one 4K mapping while preserving three neighbors, then remove a range with
both internal and trailing holes. Another test leaves PTE/data/table cleanup
to ordinary mmput()/exit_mmap(), retaining one explicit slot reference to
verify zero mappings and the surviving ownership root afterward. The gather
test mixes native count pairs with 64 slot references across local/dynamic
batches and checks that an otherwise unreferenced slot becomes reusable and
zeroed only after the batch completes.

Two kernel readers pinned to different vCPUs borrow the isolated 4K mm,
read all four mappings, and then observe the removed slot fault while its
neighbors remain readable. This passed with hardware PAN and forced software
PAN in ``kvm-anon-remote-zap-{hw,sw}-pan.log``: 37 kernel tests and 11 native
ABI tests. The non-opt-in boot passed 33 kernel tests with four hardware
fixtures skipped. The data-only KASAN suite passed nine kernel tests and
11 native ABI tests; it does not exercise these architecture rmap fixtures.
The x86_64 compile-only check for memory/rmap/mmu_gather passed. Executable
sections of eight audited feature-disabled MM/arm64 objects are byte-identical
to the base build. This is not an enabled-feature performance measurement.

At this historical stage the fixture suite had no normal writable fault path, LRU
placement, swap/reclaim, 4K EL0 task, or exposed process ABI. File/large-folio
batching, nonpresent entries, mlock and page-table checking still require
subpage-specific audits before activation.

Leaf operations, page-table checking and fork integration
--------------------------------------------------------

The leaf physical/protection helpers now live in asm/user-pte.h, early enough
for batched setters. Batched 4K PTE installation advances physical addresses
by 4K and preserves bits 13:12. Cache/tag synchronisation still covers the
native backing pages. Contiguous-PTE operations use 16 entries/64K for a
4K mm and the native 128 entries/2 MiB for a 16K mm. Getters without an mm
infer a live table's granule from its fragment descriptor. Folding/unfolding,
access/dirty aggregation, lockless reads, partial permission changes and
partial/full clearing are exercised for both granules. An initial run found
a remaining native-size-only TLBI level selector in the permission-fault
helper; the corrected runs have no warning. Native contiguous-PTE executable
sections remain identical with the experimental feature disabled.

Page-table checking is now enabled and enforced in the main debug fixture.
Its page-extension counters track each 4K physical slot independently;
native PTE/PMD/PUD mappings cover all constituent slots. This permits four
independently writable anonymous slots without treating them as writable
aliases of the same bytes. Allocation/free checks verify every slot counter.
The 512 MiB debug guest allocates 1,310,720 bytes of page extensions for
this checker together with backing ownership; the checker is optional,
not a production default imposed by this feature.

Normal dup_mmap()/copy_page_range() now clone the slot references and rmap
counts, traverse the target geometry, and write-protect parent/child PTEs.
The fork test splits its parent into four 4K VMAs and a trailing hole,
clones them using dup_mmap(), verifies byte offsets, PTE physical identity,
read-only permissions and summed RSS, then destroys the parent and checks
that the child still owns four mappings/references. Child teardown uses
ordinary exit_mmap(). The initial RSS assertion used the approximate
per-CPU read and was corrected to get_mm_counter_sum(); that failing test
log is retained as kvm-dup-mmap.log. Pinned-slot copying is not implemented:
the initial fork stage rejected DMA-pinned backing. The later pinning stage
below copies only pinned slots. Swap and migration stages add nonpresent-entry
integration.

The final fork stage passes 41 kernel tests plus 11 native ABI tests with
hardware and software PAN; the non-opt-in run passes 37 with four hardware
fixtures skipped. Data-only KASAN covers nine backing tests plus the native
ABI checks, not the architectural fork fixture. x86_64 compile checks include
page-table checking, memory and rmap. Feature-disabled executable-section
comparisons remain identical for the eight earlier audited objects plus
arm64 fault handling; contiguous-PTE code was compared with matching
THP-enabled control configs. No 4K process ABI or ordinary writable fault
path is exposed by this stage.

Anonymous writable faults, COW and allocation tags
--------------------------------------------------

The ordinary anonymous write-fault path now allocates and charges native
backing through folio_prealloc(), reserves the matching 4K offsets, adds
slot rmap entries and places the folio on the LRU. A dense VMA can install
four empty slots from one 16K allocation; a narrow 4K VMA exposes only its
own slot. Existing neighboring PTEs/markers are preserved. Fresh backing is
zeroed once; recycled slots are cleared before their references are published.
The tests check sparse/dense boundaries, data, LRU placement, summed RSS,
native references and ordinary exit cleanup. Their guests have no active
swap: this is not reclaim validation.

The private write-protection path handles zero-page and anonymous-slot COW
at 4K granularity. It stabilises the old backing, copies only the selected
slot, revalidates the PTE under its lock, breaks the old mapping with a TLB
flush and only then publishes the replacement. Old slot rmap/reference
release follows that flush. The notifier range is exactly 4K. File-source
copy plumbing is now exercised by the later file-mapping fixtures below.
VM_LOCKED, VM_DROPPABLE and private special device mappings remain unsupported
in these internal fixtures. The later pinning stage adds per-slot exclusivity
and reuse of an already exclusive slot. A necessary COW copy still allocates
a native folio for one slot; allocation-domain reuse remains work, and no
enabled-feature performance result is claimed.

New arm64 partial copy/clear helpers preserve neighboring MTE allocation tags.
A fresh tagged destination initialises its native tag storage once; subsequent
copies/clears touch only the selected 4K. Failed copies cancel the unpublished
reservation. The MTE test checks every 16-byte tag granule during same-folio
slot reuse/copy and copying into fresh backing. It passes on QEMU TCG with
MTE enabled, including a generic-KASAN build. KVM on this host reports MTE
unavailable, so that check is explicitly skipped there. The assembly fallback
for a tag-transfer block larger than 4K is not independently exercised.

The final cached-translation test borrows the forked child's mm, reads all
four slots, waits while COW replaces one slot, then checks data and write
permissions in both child and parent. Its initial version disabled the
access-flag faults needed for fork's old PTEs and failed its reads of the
uncopied child slots. The retained trace identifies that fixture error;
allowing the initial reads through the normal fault handler makes the test
pass with hardware and software PAN. Subsequent permission probes remain
fault-disabled so they cannot themselves trigger further COW.

Final KVM runs pass 45 kernel tests with one MTE skip, plus 11 native ABI
tests. The non-opt-in boot passes 40 kernel tests with five hardware and one
MTE skip. Data-only KASAN passes nine with one MTE skip; TCG MTE/KASAN passes
all ten backing tests. Each also passes the 11 native ABI checks. These are
component fixtures, not 4K userspace ABI tests. x86_64 memory.o compiles;
feature-disabled audited MM and native page-copy executable sections match
the base. The first native audit command incorrectly requested the optional
user-subpage.o with its config disabled; its errors are retained separately
from the corrected successful native build.

The writable-ownership audit found that folio->private aliases folio->swap
and that PageAnonExclusive tracks a native page. Neither can silently serve
as four independent subpage lifetime/COW states. Explicit slot references
now attach to PTE/rmap ownership. Migration integration is described below;
kernel pinning callers still need the fragment interface described below.
Backing-memory charges must stay distinct from mapped-byte accounting;
current internal RSS assertions count logical PTEs and are not a completed
OOM/proc accounting policy.

Anonymous reclaim, swap and swapoff
-----------------------------------

A swap PTE now retains a two-bit slot offset in arm64's previously unused
bits 4:5, separately from the native 16K swap-cache index in bits 12:61.
Type/offset conversion continues to name the native swap entry. Moving a
swap offset preserves the slot bits; raw PTE copying through fork preserves
them too. The encoding fixture covers all four slots, combinations of swap
flags, several types and the high offset boundary. The migration stage below
reuses this encoding for migration entries. Device-private entries remain
unintegrated.

The anonymous reclaim rmap path processes every present 4K mapping of a
native folio, preserves each slot offset in its nonpresent PTE, and takes
one swap-count reference per PTE. It uses the existing immediate/deferred
TLB machinery with 4K address ranges. Slot references are dropped after
rmap removal; normal swap-cache/isolation references retain physical memory
through writeback and reclamation. The allocator refuses to recycle free
slots in swap-cache folios, because nonpresent PTEs can still need their
bytes. Final anonymous-owner release also tries normal free_swap_cache()
so an unused cache need not retain otherwise dead backing.

Restoration attaches slot ownership to an uptodate locked cache folio
without zeroing its data. It can reacquire a live slot, reserve an unused
slot in a surviving owner, or create metadata after the original owner was
retired. Pool metadata now retires through RCU as well as owner metadata;
a detached state prevents revival between final-reference retirement and
page-extension unbinding. Tests preserve distinct slot contents through
these cases and race 2,048 restorations against metadata retirement. Fresh
rmap attachment does not dereference a stale, unmapped anon_vma pointer in
a retained swap-cache folio; it establishes and pins the destination root.

The 4K swap-fault path restores one slot and its MTE state, revalidates its
nonpresent PTE, updates rmap/RSS/swap counts, and uses the small-slot COW
handler for a write fault. Swapoff walks use per-mm table geometry and
strides and restore each slot without promoting write permissions. A
poisoned/unreadable cache folio produces a poison entry as in the native
path. Large/KSM swap folios and arbitrary cross-root aliases are not yet
supported for the alternate ABI. Small mms currently use native swap-cluster
readahead; their VMA readahead policy still needs conversion and performance
measurement. Native readahead behavior is unchanged when the feature is off.

The guest runner is ``tools/testing/selftests/arm64/user4k/swap-fixture-init.c``.
It is an extended selftest build target, not a test to execute on the host.
As VM PID 1 it mounts the minimal test filesystems, runs the 11 native ABI
checks, enables a dedicated virtio swap disk and verifies 4 MiB of native
swap traffic before invoking the late KUnit swap suite. Its swap-control
helpers refuse to run unless this same executable is the guest's PID 1.
Host invocations of both helpers were checked to refuse the operation.
Use ``kunit.autorun=0``: a test skipped at boot remains marked skipped on
this kernel's debugfs rerun. The initial attempted rerun was rejected by the
runner rather than reported as a pass. No KUnit framework change was made.

Seven late swap tests pass with hardware and software PAN on KVM:
cached and disk round trips, mapped-LRU reclaim, real swapoff of two forked
4K mms, 32 swap/fork/COW/exit cycles with distinct data, swapoff across 2 MiB,
1 GiB and 512 GiB table boundaries, and live faults from two vCPUs. The live
readers borrow one child mm, cache its translations, wait for reclamation
and cache eviction, then refault through normal uaccess and write different
bytes in one child slot. Both writes survive and the parent's original data
remains intact. This initiates concurrent accesses; it does not establish
that every specific PTE revalidation race was hit.

The six non-hardware swap cases also pass under TCG with MTE, verifying
allocation tags after cache eviction, swapoff and COW. The live-reader case
is excluded there because that CPU configuration does not satisfy the
fixture's TTBR0 control restrictions. Generic-KASAN covers all 13 backing
and restoration tests under TCG/MTE (12 plus an MTE skip on KVM), not the
architectural swap suite. Ordinary KVM boot passes 49 kernel tests with
seven late-swap and one MTE skip; without the hardware-test opt-in it passes
44 with thirteen skips. The late swap runner separately passes its seven
cases. Each VM also passes the 11 native ABI checks. The feature-disabled
executable-section audit covers thirteen MM/arm64 objects, including swapfile
and swap_state; x86_64 checks are compile-only.

These results cover bounded, isolated anonymous mappings, not a general
memory-pressure workload or a userspace process ABI. GUP/pinning,
per-slot exclusivity, sparse/COW allocation reuse, accounting units, file and
shared mappings, userfaultfd, mlock, exec and syscall validation remain gates
before an EL0 process may opt in. The kernel and all ordinary processes still
use 16K pages by default.

Anonymous migration
-------------------

The normal migrate_pages() path now prepares metadata before unmapping a
subpage-backed folio. Source and destination owners stay bound to their
original physical folios: moving an owner pointer would race lockless slot
lookups that already acquired a native reference. Preparation allocates a
separate destination owner and pins both metadata records without taking
extra native folio references. This preserves migration's normal refcount
checks. Allocation failure leaves the original mappings intact.

Migration entries name the native PFN and retain the 4K slot in bits 4:5.
Rmap removal handles every 4K PTE, including deferred TLB invalidation, and
ordinary migration copies the native data, tags, mapping and swap-cache
state. Remapping reuses preallocated metadata without allocation or zeroing.
Failure restores the original slots. Empty metadata is retired only after
migration or rollback finishes; it does not consume the migration driver's
folio reference. A final slot put now clears its last reference and used
bit together under the pool lock, so recovery cannot observe a retiring
slot between those operations.

Fork preserves slot bits when it downgrades writable migration entries.
The exact-slot reverse-map walker also checks those bits. An entry removed
by concurrent unmap is not restored. The test that first exercised the
exact-slot walker incorrectly started each lookup at the first slot; the
API requires the candidate address. That fixture was corrected, and both
its failing and passing logs are retained.

Bounded migration tests cover dense and sparse mappings, parent/child
aliases, normal refcount-failure rollback, asynchronous migration, and the
4K mappings on each side of the 2 MiB, 1 GiB and 512 GiB table boundaries.
A separate fixture forks while migration entries are installed, removes
one parent's entry, then checks restored offsets, permissions, mapcounts
and RSS. Two live workers cache translations before normal migration and
then access the new backing through uaccess and perform child-only COW.
Another fixture resumes the workers while the source is locked with
migration entries installed: their accesses remain pending until rollback
restores the PTEs and releases the folio. Both live tests pass with hardware
and software PAN. These are kernel workers borrowing private mms, not EL0
processes.

A late swap test migrates a swap-cache folio with one present slot and seven
swap entries across two mms, then faults every slot back in. Data and MTE
tags survive; the eight-case late suite passes on KVM, and its seven
non-hardware cases pass on TCG/MTE. The backing-owner test separately
covers successful move, rollback, all entries removed, and a partially used
folio; all fourteen backing tests pass under KASAN/TCG/MTE, or thirteen
plus an MTE skip on KVM. The feature-disabled executable sections of
migrate.o, memory.o, rmap.o and page_vma_mapped.o match the base. x86_64
builds of those objects also pass; that is compile-only validation. Ordinary
KVM boot now passes 54 kernel cases with nine skips (eight late swap cases
and MTE); without the hardware opt-in it passes 47 with sixteen skips.
Each guest also passes the eleven native ABI tests.

Other migration drivers, device-private memory, hardware-poison paths,
NUMA-policy workloads, memory offlining, pins and broad pressure testing
still require integration or validation. The alternate ABI remains closed.

Per-slot exclusivity and fragment references
-------------------------------------------

Anonymous exclusivity and pin counts now belong to each 4K slot. Fresh
private mappings and COW copies become exclusive; fork clears exclusivity
when sharing an unpinned slot. A pinned slot is copied for the child while
its unpinned neighbors can remain shared. Destination data, charge and owner
metadata are allocated outside the PTE locks. An already exclusive slot is
reused on a write fault, including a pinned slot whose PTE was read-only.
The native PageAnonExclusive bit remains clear for these backing folios.

A slot pin owns both a logical slot reference and a native FOLL_PIN reference.
Sharing and pin acquisition serialize under the ownership lock, with the
caller holding the PTE lock. Pins keep the slot occupied and its anon root
alive after all mappings disappear. Migration preparation refuses an owner
with pins; reclaim restores a pinned PTE instead of making it shareable.
Migration entries preserve exclusive read state as well as write state.
This implementation has no lockless pin walker yet.

The experimental kernel interfaces get_user_fragments_remote() and
pin_user_fragments_remote() return records containing a folio, exact byte
offset, length, ownership reference and reference kind. Records end at the
target mm's granule, not the caller's. Both interfaces manage mmap_lock and
fault retries, validate VMA access, preserve partial results, and delegate
native-granule requests to ordinary GUP. release_user_fragments() handles
either reference kind and optional dirty marking. Copying a record does
not duplicate its reference.

Ordinary read references retain shared COW backing. Read pins must unshare
anonymous COW backing but leave its PTE read-only; write pins obtain writable
or valid forced-access backing. The small-granule path supports anonymous
slots and the zero page; the later file stage adds ordinary cache backing.
Device/large/KSM backing, FOLL_LONGTERM and
additional GUP flags still require integration. This is not a drop-in
replacement for struct-page-array callers: those callers must consume the
physical offsets and lengths explicitly. Until converted, native page-array
GUP rejects alternative mms with EOPNOTSUPP, and its fast walk falls back
without touching their tables. A test checks the legacy rejection. Native
mms continue through their original GUP paths.

Tests check unaligned ranges, truncated output capacity, unmapped-range
partial results, no-fault failures, overflow, read-only VMA permissions,
forced access, native 16K requests, and COW behavior for both reference kinds.
A zero-page mapping at virtual slot 1 returns physical offset 17 for a
request 17 bytes into its 4K mapping; after a write fault its private backing
has physical offset 4096+17. This checks that the API does not derive the
physical offset from the native virtual-page offset.

The pin-lifetime fixture holds two pins, forks, checks that only the pinned
slot is copied, verifies its write-fault reuse, destroys both mms, and releases
the pins individually. MTE tags survive the copy. Another fixture runs two
pinning workers against 32 fork/COW/exit cycles and asynchronous migration
attempts; parent data stays unchanged. Successful migrations and unsuccessful
attempts are observed, but the fixture does not classify every failed attempt
or prove that every possible race interleaving occurred. A late swap fixture
checks refusal to unmap a pinned slot, then successful swap and data/tag
restoration after release.

KVM hardware/software PAN and TCG/MTE runs pass the bounded reference/pin
checks. The late suite has nine passing KVM cases and eight non-hardware
TCG/MTE cases. Feature-disabled executable sections of gup.o, memory.o,
rmap.o, migrate.o and page_vma_mapped.o match the base; the x86_64 checks are
compile-only. The first native GUP fixture accidentally retained dummy VMA
operations and could not fault anonymous memory; its failed run is retained,
and the corrected anonymous-VMA fixture passes. An intermediate native guard
changed gup_fast_fallback's compiled size by four bytes; preprocessor gating
restored the baseline executable-section match, with both audit reports kept.

The additional arm64_user4k_mm_kasan.config enables the software MM suite
under generic KASAN, lockdep and PageTableCheck. At this stage, raw translation/borrowed-mm
fixtures remained disabled: the hardware boot opt-in is rejected with KASAN,
and that stage's object audit found no raw TCR/TTBR writes or borrowed-mm
operations in the KASAN test object. Later mmap/mprotect fixtures, described
below, use normal borrowed-mm operations under KASAN. A deliberate hardware-opt-in attempt reports a malformed
early option and skips the translation case; it is a gate test, not a passed
hardware translation test.

Ordinary KVM runs pass 58 kernel cases with ten skips, plus eleven native
ABI checks. The full software KASAN suite passes 51 cases with seventeen
skips on KVM, or 52 with sixteen skips under TCG/MTE. This now covers faults,
fork/COW, migration, typed references/pins and the pin race, rather than just
backing metadata. The eight non-hardware late swap cases also pass with
KASAN and MTE. These counts exclude the native ABI checks, which pass in
each guest. The data-only KASAN profile is retained for focused ownership
regressions.

There is still no EL0 opt-in or production performance claim. Legacy GUP
callers, remote-access/syscall consumers, allocation reuse, byte accounting,
exec, userfaultfd, locking and broader stress workloads
remain integration gates. Ordinary userspace continues to use 16K pages.

File-cache and shmem mappings
-----------------------------

The page cache retains native 16K indices and folio ownership. A 4K PTE maps
an explicit quarter of a native page and owns one folio reference and one
file-rmap reference. Native and alternative processes may therefore share
the same cache bytes; no cross-granule copy or blanket sharing prohibition
is needed for these mappings. Per-PTE access permissions remain independent.
This is bounded internal-fixture support, not an exposed mmap ABI.

The small-granule finish-fault and filemap fault-around paths intersect the
locked cache folio with the VMA, its paired file offset, the current PTE
table and EOF. Fault-around retains the existing byte-window policy and maps
ready folios in batches under one PTE lock. Native cache-index callbacks are
unchanged. Each installed leaf takes a separate reference; the callback's
reference is consumed only on successful finish-fault. File and shmem faults
reject a granule starting past EOF even when it lies within the last native
cache page. A partially populated final user granule remains accessible.

Private write faults without a present PTE preallocate native backing and
slot metadata before locking a cache folio. They copy the selected 4K bytes
and tags and publish an exclusive anonymous slot. Present file-PTE COW uses
the existing subpage copy path. Its neighbors remain file-backed. Necessary
private COW still has the sparse-allocation limitation described above.

File interval-tree invalidation now translates native file ranges through
the VMA's byte-offset component. Truncation additionally removes complete
small granules between the process-rounded EOF and the native-page boundary,
including private COW copies, on both sides of cache truncation. Native
partial-page behavior is retained. The native page-cache hole-punch machinery
zeros sub-native-page shared bytes while preserving private COW copies.

Fragment GET/PIN accepts ordinary file-cache leaves. A GET on a private file
mapping retains cache backing; a read PIN first unshares it into an anonymous
slot, leaving the PTE read-only. Shared-file pins use ordinary folio pin
accounting and exact physical byte offsets, and may outlive the mappings.
Device/PFN/mixed mappings and long-term pinning remain unintegrated.

File migration entries retain the subpage offset separately from the native
PFN, and restoration uses that offset. File reclaim visits every 4K leaf,
including aliases across PTE-table boundaries, instead of stopping after one
native-page-sized reference count. Native-PFN batching is not used for 4K
quarters of a large folio. Large-folio behavior, packing/batching performance,
writeback and general pressure are not established by this stage's order-0
fixtures; enabled-feature performance remains unmeasured.

Nine tests cover shared native/alternative aliases, mismatched virtual/file
alignment, fault-around, cold-PTE and present-PTE private COW, partial EOF,
truncation including COW, index-range invalidation, fragment references/pins,
fork with partial/full-native-page hole punching, and migration success,
rollback and reclaim across a 2M table boundary. One uses generic
filemap_fault/page_mkwrite against an already populated shmem cache; it is
not a cold disk-I/O or arbitrary-filesystem test.

The final KVM hardware-PAN and software-PAN runs each pass 67 kernel cases
with ten skips, plus eleven native ABI tests. The software-MM KASAN/MTE run
under TCG passes 61 kernel cases with sixteen skips, plus those eleven ABI
tests. No warning, lockdep or sanitizer report occurs in these final runs.
The nine late swap cases also pass on the preceding build (before a test-only
mmap-lock fix). All nine newly audited MM objects have feature-disabled
executable sections identical to the original base, including filemap, shmem,
truncate and mmu_gather; the x86_64 object checks are compile-only.

Intermediate failures are retained: the first private fixture incorrectly
set VM_MAYSHARE; a reclaim test checked neighboring PTEs after fault-around
had repopulated them; a fork fixture looked up a VMA without its mmap_lock.
The corrected fixtures pass. The first native audit found four-byte compiler
layout differences in memory.o and filemap.o; restoring their original native
expressions yielded byte-identical executable sections.

There is still no EL0 activation, ELF support, syscall mapping ABI, or broad
application/performance result. File-backed integration does not imply GPU
or other device-mapping support. Remote-access/futex callers, virtual/RSS
accounting, syscall alignment, allocation reuse and exec remain next gates.

Mapping syscalls, protection and virtual reservations
------------------------------------------------------

The ordinary vm_mmap/vm_munmap and arm64 mprotect paths now accept the mm's
mapping granule. The byte offset from arm64 mmap is split into native cache
index and subpage offset; the pair survives mmap preparation, callback
validation, VMA creation, prepend/append merging and partial unmap. Existing
native-index APIs retain their original units. This is internal borrowed-mm
coverage: there is still no userspace activation interface.

Virtual VMA totals, stack/data/executable sizes and core locked_vm accounting
use user granules. Global commitment remains in native pages. Each alternative
mm tracks its aggregate committed user granules and charges the rounded delta
of that aggregate, so splitting a VMA cannot inflate reservations or refunds.
Failed reservations leave both counts unchanged. The exported locked-page
accounting helper still accepts native physical pages and converts to the mm's
units. External accounting readers and direct consumers remain a subsequent
integration gate; pinned_vm remains in native physical pages.

mprotect walks mm-specific table levels and batches contiguous leaves within
a folio, including four leaves in an order-0 native folio. A batch is split
where anonymous slot exclusivity changes, so restoring write permission does
not make a fork-shared neighbor writable. Migration-entry downgrades retain
the physical subpage offset. TLB invalidation uses the actual user stride.
The arm64 erratum-2645198 helper now passes an absolute end address to the
range-flush helper, whose interface expects an end rather than a length.
That CPU-specific erratum path was not independently exercised by these VMs.

Nine added cases cover descriptor offsets, file-offset merging, aggregate
commit accounting and denied-reservation rollback, 4K unmap/accounting,
ordinary mapping entry points, protection changes across table boundaries,
mixed exclusive/shared COW batches, protection reservation refunds and changes
while migration entries are installed. The migration-entry fixture is a
serialized transition test, not concurrent pressure. Deliberate overcommit
rejection emits an expected __vm_enough_memory diagnostic.

Unlike the earlier raw-control fixtures, the new mapping/protection cases use
normal kthread_use_mm/kthread_unuse_mm and can run under KASAN. Permission
checks use ordinary fault-disabled user copies. Raw TCR/TTBR manipulation
remains disabled under KASAN; the earlier no-borrowed-mm object audit applies
only to its frozen stage.

Final KVM hardware/software-PAN runs each pass 76 kernel cases with ten skips,
plus eleven native ABI checks. TCG with KASAN and MTE passes 70 kernel cases
with sixteen skips and eleven native ABI checks. All nine late swap cases
pass on the final main Image. The feature-disabled Image boots and passes
the eleven native ABI checks. The host VMA suite passes all 27 cases, and
selected feature-disabled arm64 and x86 objects compile. No warning, lockdep
or sanitizer report occurs in these final guests. No byte-identity claim is
made for this stage's feature-disabled code.

Remaining gates include accounting consumers, mremap movement, mlock and
populate, special/device mappings, legacy GUP callers, remote access, futex,
exec/ELF/arguments, process activation, allocation reuse and broad application,
pressure and performance testing. This is not a completed 4K process ABI.

Memory accounting readers
-------------------------

Status byte/kB fields now convert virtual and RSS counters using the target
mm's granule. pinned_vm stays in native physical pages. The page-count fields
in proc stat/statm use the reader's granule; a partial reader page rounds up,
and kernel readers with no mm use native units. This lets a native monitor
read an alternative process and an alternative monitor read a native process
without assuming the target's AT_PAGESZ. File offsets in maps and PROCMAP_QUERY
include the byte component. VMA page-size queries return the user granule.

signal maxrss/cmaxrss are stored in kB, so exec history and reaped children
compare a common unit. Task accounting time integrals likewise use a fixed
unit across mm changes. OOM comparisons and diagnostic page counts convert to
native-page units, while byte traces and OOM kB messages use target-granule
conversion. The NUMA scan-window comparison and VFIO's direct locked_vm limit
check now use native units. The locked-page helper rejects overflow even when
the caller bypasses rlimits; final mm destruction checks for leaked commitment.

A new fixture calls the actual status/statm producers with explicit synthetic
4K/native counts, checks mixed-granule high-water maxima, partial-page rounding,
physical pin units and a borrowed 4K reader observing a native mm. Final KVM
passes 77 kernel cases with ten skips; TCG KASAN/MTE passes 71 with sixteen
skips. Each also passes eleven native ABI checks, without warnings or sanitizer
reports. This is reporting coverage, not induced OOM, child-reaping, or netlink
runtime coverage. Native arm64 and x86 object checks pass with task accounting
and VFIO configured; x86 also enables NUMA balancing. Initial attempts to build
those objects with their dependencies disabled failed and are retained.

smaps/PSS, pagemap/NUMA-map walks, mlock/populate, device mapping and remaining
page-array consumers still need work. No process activation or ELF execution
is provided by this stage.

Remote memory access and shared futex identity
---------------------------------------------

A single-fragment GET variant retains the caller's mmap lock and returns a
stable VMA for cache-maintenance callbacks. access_remote_vm, and therefore
its ptrace/process-memory consumers, now copy the returned physical fragment
and release its typed reference. Private forced writes preserve COW behavior;
shared read-only mappings retain their permission checks. Device callbacks
still use their existing separate path, without establishing device support.

The alternative-mm process-VM copy path pins batches of typed fragments, copies
with exact physical offsets, marks written backing dirty and releases all pins.
It preserves partial-transfer behavior and the existing task-access checks.
Native targets retain the page-array path. This is not a conversion of all
legacy GUP consumers or a long-term/fast-pin implementation.

Private futex keys retain their virtual identity. Shared file keys use the
native cache index plus the actual byte offset in that native page, so aliases
in 4K and 16K mms identify the same word even with different virtual alignment.
Typed references keep anonymous slots live until key generation completes.

Five new cases cover unaligned remote COW copies and forced-write permission,
shared file copies across a native-page boundary, process-VM batches exceeding
the stack array and returning partial results, cross-granule futex keys, and
real futex wait/wake queues in both directions between borrowed 4K/native mms.
The process-VM fixture calls the copy engine with kernel iterators; it is not
a userspace process_vm_readv/writev syscall/credential test. The futex workers
execute kernel entry points with ordinary uaccess, not EL0 programs. PI, requeue,
robust futexes and hostile concurrent VMA modification remain untested.

Final software-PAN KVM passes 82 kernel cases with ten skips; final TCG
KASAN/MTE passes 76 with sixteen skips. Each also passes eleven native ABI
checks. The nine late swap cases pass on the final main Image. Hardware-PAN
passes on the preceding build before an added shared-file process-VM test
and defensive missing-VMA cleanup. Native arm64/x86 object checks also pass
before that config-enabled-only cleanup. No warning or sanitizer report occurs
in these final runs. Actual ELF execution and process activation remain absent.

Remapping and temporary stack relocation
----------------------------------------

mremap now uses the mm's leaf stride, table boundaries and paired file offset.
Its present-leaf batching shares the protection path's fragment-aware batch
helper. Whole-PMD/PUD moves remain enabled; a new mm-aware PUD table accessor
retains packed-table address bits when transferring an entire subtree. User
alignment, length rounding, growth overflow and virtual-accounting paths use
the appropriate granule. Temporary stack relocation uses the same movement
engine while preserving the original anonymous linear offset.

Five ordinary cases cover leaf moves across 2M/1G/512G boundaries, exact
transfer of packed PMD/PUD table descriptors, source-TLB invalidation, in-place
resize, DONTUNMAP with fresh source faults, rollback from a rejected file
callback, quarter-offset file mappings, moving a fork-shared quarter then COW,
and overlapping/non-overlapping temporary-stack relocation followed by growth.
A late case moves four swap entries across a table boundary to a differently
aligned VMA and restores their individual bytes and MTE tags.

Final software-PAN KVM passes 87 kernel cases with eleven skips; TCG KASAN/MTE
passes 81 with seventeen skips. Each also passes eleven native ABI checks.
Final late swap runs pass ten KVM cases or nine non-raw-hardware KASAN/MTE
cases. The debugfs runner repeats the same result in its log; those repetitions
are not additional runs. Hardware-PAN passes the preceding build before only
the new late case was added. Feature-disabled arm64/x86 remap/protection/stack
objects compile. No warnings, lockdep or sanitizer reports occur in these runs.

These are bounded internal fixtures. Concurrent remap/pressure, moved migration
entries, userfaultfd, full ELF/argument setup, other mapping syscalls, device
mappings, allocation reuse and application performance remain incomplete.

Exec argument storage
---------------------

Temporary argument references now distinguish a native page from a typed
anonymous slot and retain its physical byte offset. User/kernel string copies
and interpreter argument removal walk the target mm's granules. String-length
limits remain 32 target granules. Temporary argument charges convert into the
calling mm's counter units, rather than adding incompatible target counts.
Stack setup aligns and limits the target's range in its own granule.

A new fixture copies multi-granule strings into both alternative and native
stacks, rejects an over-limit string before advancing the stack pointer, runs
setup_arg_pages with a borrowed mm, verifies the relocated strings and removes
three interpreter arguments. It uses kernel-source strings; actual alternative
ELF execution and user-source exec argument copying remain untested. The
first KASAN/MTE attempt incorrectly expected an exact requested stack top;
arm64's arch_align_stack randomizes within a native page before rounding. The
corrected fixture checks the allowed aligned range and verifies every string.

Final software-PAN KVM passes 88 kernel cases with eleven skips; TCG KASAN/MTE
passes 82 with seventeen skips. Each passes eleven native ABI checks without
warnings or sanitizer reports. Native arm64/x86 exec objects compile. A RISC-V
no-MMU compile check found that generic accounting/offset helpers depended on
a header included only under CONFIG_MMU; mm.h now includes that header itself.
The corrected no-MMU exec object builds, with compiler warnings in existing
asm-generic I/O helpers. The initial RISC-V configuration still selected MMU;
only the retained NONPORTABLE, CONFIG_MMU=n configuration establishes no-MMU
compile coverage. There is no RISC-V runtime claim.

Process activation, ELF segment loading/auxv, vDSO/special mappings and broad
application execution are still incomplete.

Special mappings and vDSO backing
---------------------------------

Special faults now map individual user leaves of the exact native page supplied
by the callback, including reordered page arrays and clipped VMA/table edges.
Private forced COW and private read pins create anonymous slots without changing
native aliases. A typed insertion helper maps one user granule at an explicit
physical offset; time-namespace VVAR prefaulting uses that offset. Normal
non-file pages support typed GUP, while IO/PFN/MIXED mappings remain excluded.

Three fixtures cover reordered special pages, COW and read pins, explicit
quarter insertion and lifetime, and the real arm64 vDSO/VVAR installation at
a 4K-aligned address. They read vDSO ELF bytes, fault all time/RNG quarters
through ordinary uaccess, verify backing addresses, and check remote VVAR
access rejection. They do not execute vDSO code at EL0 or switch time namespaces.
Legacy driver page/PFN insertion interfaces still need integration.

Final hardware/software-PAN KVM passes 91 kernel cases with eleven skips;
TCG KASAN/MTE passes 85 with seventeen skips. Each passes eleven native ABI
checks, with no kernel warnings or sanitizer reports. Feature-disabled arm64
and x86 memory/GUP/datastore objects compile. Checkpatch reports zero errors,
seven fixture line-length warnings and one debug VM_BUG_ON_FOLIO warning.
No late-swap rerun was made for this chunk. Process activation and actual
alternative ELF execution are the next gate, not an established capability.

Initial EL0 execution and exec selection
---------------------------------------

The experimental arm64 prctls PR_AURORA_SET_EXEC_PAGE_SIZE (0x41555001) and
PR_AURORA_GET_EXEC_PAGE_SIZE (0x41555002) select/query the calling thread's
pending exec request. All unused arguments must be zero. SET accepts 4096
only on a 4K-capable CPU; 0 or the native page size resets the request. GET
returns 4096 or 0 (native default). It does not report the current mm's size;
AT_PAGESZ does that. A request is inherited by fork, remains local to that
thread, survives a pre-commit failed exec, and is consumed on committed exec.
An ordinary exec from a 4K process therefore creates a 16K mm. AArch32 exec
with a pending 4K request is rejected before commit; that rejection is code
inspection, not tested compat execution in the current configs.

mm_alloc_exec selects immutable geometry before allocating the PGD and
argument stack. Ordinary mm_alloc remains native; dup_mm inherits geometry.
arm64's exec reset preserves the selected mm flag. The ELF loader aligns
segments and auxv with the target granule, while its static format descriptor
retains native constants. This does not complete the coredump implementation.

The PID-1 exec fixture launches native page-contract before and after a
static 4K-aligned executable and dynamic 4K-aligned PIE, using the native
libc and dynamic loader. Each invocation passes eleven checks. It validates
request errors, fork inheritance, failed-exec persistence, successful-exec
consumption and 4K-to-native reset. Separate execs verify 120000-byte argument
and environment strings in 16K-to-4K, 4K-to-4K and 4K-to-16K transitions;
a 139999-byte argument is rejected for the 4K target without losing its request.

The mixed-process executable uses a 4K-offset memfd alias and an ordinary
native alias. In each parent/child granule direction it completes 2000 futex
round trips, requires observed wakeups of blocked peers in both directions,
and retains a private COW byte plus untouched neighboring file bytes.
Real process_vm_readv/writev copies cross multiple target leaves and return a
partial read at a PROT_NONE boundary. Workers verify every copied byte.
These are parent/child permission-allowed operations, not a credentials audit.

Final hardware/software-PAN KVM passes 91 kernel cases with eleven skips;
TCG KASAN/MTE passes 85 with seventeen skips. Both also pass all exec/mixed
fixtures, including 44 page-contract checks (two native, static 4K and dynamic
4K). Final logs have no kernel warnings or sanitizer reports. The initial
dynamic attempt failed because the fixture placed libc outside the loader's
/usr/lib search path; correcting the initramfs fixed that run. A feature-off
compile found a missing generic mmu_context include; after correcting it,
native arm64/x86 fork/sys/exec/ELF and native arm64 process objects compile.
RISC-V no-MMU fork/exec also compile, with the existing I/O-helper warnings.

This establishes actual alternative execution, not a full ABI qualification.
Memory syscalls such as madvise/mincore/mlock, proc PSS/pagemap, coredumps,
general device insertion, remaining GUP users, time namespaces, multithreaded
application stress, pressure and performance still require work. The existing
late-swap tests were not rerun for this exec-only change.

Residency, synchronization and sealing
--------------------------------------

mincore now walks/counts in target user granules while retaining native cache
indices and batching cache-only quarters of one native page. Output buffering
still uses a native allocation. msync aligns its range to the process granule
and includes the VMA's byte suboffset in fsync ranges. mseal aligns/rounds to
the user granule. These paths retain their existing permission/error rules.

Nine new EL0 checks run in both native and alternative processes: untouched
and separately faulted residency, unaligned inputs and holes, cached file
quarters, a vector ending at PROT_NONE, multiple output-buffer iterations,
msync alignment/holes, a one-byte seal restricted to one user page, and fork
inheritance of that seal. A kernel callback fixture verifies three exact
fsync byte ranges across a native cache-page boundary. The memfd EL0 test does
not establish persistent filesystem writeback; swapped/hugetlb residency and
concurrent query races are not separately exercised by this chunk.

Final hardware/software-PAN KVM passes 92 kernel cases with eleven skips;
TCG KASAN/MTE passes 86 with seventeen skips. Each passes all eighteen new
EL0 checks plus the prior exec, page-contract and mixed-process fixtures.
No kernel warnings or sanitizer reports occur. Native arm64/x86 objects
compile. Checkpatch reports zero errors, one mutable test-operations warning;
the new userspace test has zero warnings. Late-swap tests were not rerun.

Prefaulting and basic advice
---------------------------

GUP's no-page-array prefault path now follows small leaves without exporting
native page references. It preserves the existing permission, partial-progress
and mmap-lock retry protocol, and updates dirty folio state for write touches.
Array-returning GUP remains unsupported for alternative mms. MAP_POPULATE and
MADV_POPULATE use target-granule counts and advancement. Locked and special
mappings still need their own integration.

Basic madvise range checks, file byte offsets, shmem swap-cache range endpoints
and guard-marker counts now use the appropriate units. Eleven new EL0 tests
run under each ABI, covering DONTNEED and fork COW, guard install/discard/remove,
read/write population and its errors, MAP_POPULATE, WIPEONFORK/DONTFORK, file
REMOVE with private COW, and a WILLNEED smoke check. Both advice and residency
programs now assert the requested AT_PAGESZ explicitly. A kernel regression
case verifies dirty-folio tracking when prefault follows an already-present,
clean writable entry. Its initial setup triggered arm64's unsafe-PTE-update
warning; the corrected fixture clears and flushes before reinstalling the PTE.

Final software-PAN KVM passes 93 kernel cases with eleven skips; TCG KASAN/MTE
passes 87 with seventeen skips. Both pass all twenty-two new EL0 advice checks
and the prior exec, residency, page-contract and mixed-process fixtures without
kernel warnings or sanitizer reports. Hardware-PAN passed the preceding build
before the dirty-tracking correction/fixture. Feature-off arm64/x86 GUP and
madvise objects compiled before that config-enabled-only correction. Final
checkpatch has zero errors/warnings for the tracked diff; the new EL0 guard
test has one intentional volatile-access warning.

This is not full madvise support. Alternative mms explicitly return EOPNOTSUPP
for COLD, PAGEOUT, FREE, KSM merge/unmerge, hugepage promotion/collapse and
memory-error injection until those engines are integrated. Native behavior is
unchanged. Those exclusions are unfinished development gates, not a production
fix or performance result. Swapped WILLNEED, lock-drop races, remote advice,
concurrent pressure and the remaining advice modes still need work. The late
swap suite was not rerun for this chunk.

Cold/pageout validation and emulator correction
----------------------------------------------

COLD and PAGEOUT now walk alternative mappings in 4 KiB units and aggregate
accessed/dirty state across the leaves backed by one native folio. A partial
range or a fork-shared folio is skipped when its mapcount exceeds the leaves
covered, preserving the existing noninterference policy. The kernel regression
covers a young final quarter, a partial range, and shared versus exclusive
ownership. Native KVM passes 94 cases with eleven skips; clean TCG KASAN passes
88 with seventeen skips and all existing EL0 fixtures. Feature-off arm64/x86
madvise, mprotect and mremap objects compiled.

The late swap suite passes ten cases on KVM and nine filtered cases on TCG.
Both ABIs pass two 4 MiB EL0 write/pageout/readback rounds, including WILLNEED
and POPULATE_READ. VmSwap and pswpout establish swap activity; mincore can
still report warm swap-cache data resident. The separate late disk test
explicitly evicts swap cache. These are distinct coverage claims.

This validation exposed an independent QEMU S1PIE dirty-tracking defect:
unmodified base and feature-disabled kernels also lost second-round writes.
The private upstream v11.1.1 build reproduced stale data in 238/256 native
pages. A local target/arm/ptw.c correction prevents a read/fetch TLB fill from
caching write permission for a clean PIE translation. The same base kernel
passes with hardware dirty updates both enabled and disabled; the corrected
emulator also passes the mixed KASAN suites. System QEMU is untouched. The
patch includes the analogous S2PIE handling, which is not runtime-qualified.
The diagnostic kernel trace has been removed. Exact artifacts are retained
under user4k-results/swap-dirty-investigation and cold-pageout-v1.

There is still measured 4K swap amplification: after read swap-in, rewriting
the range causes 1024 native swap writes for 4 MiB instead of roughly 256.
Slot exclusivity is not yet preserved through swap PTEs, forcing avoidable
COW. This remains unfinished work, not a finished performance result. The
previous COLD/PAGEOUT exclusion is superseded; other advice exclusions remain.

Preserving ownership through swap
---------------------------------

Unmapping an exclusive anonymous slot now transfers its ownership marker to
the swap PTE. Fork clears it in both mms. Swap-in and swapoff restore slot
exclusivity under the folio and PTE locks; stable-write backends still force
COW if writeback has not completed. Native swap counts aggregate quarters
and are deliberately not used as a proof of slot exclusivity.

Three new late tests verify cached swap-in, disk swap-in and swapoff. Each
checks the marker, bytes, MTE tags, contiguous quarter backing and physical
identity after subsequent write faults. Existing fork cases now explicitly
check that both swap PTE markers clear. KVM passes thirteen late cases,
TCG KASAN twelve filtered cases; both pass native/4K EL0 pageout readback.
The prior second-round 1024-write amplification disappeared (observed 228
and 246 writes at the immediate KVM/TCG counter snapshots). Those counters
can precede asynchronous completion; the physical-identity kernel checks
prove packing directly. This does not establish general workload performance
or optimize ownership recovery after a fork sharer exits. Stable-writeback
concurrency still needs a dedicated stress test.

General software-PAN KVM and patched-QEMU KASAN pass their existing suites
and all EL0 fixtures with no warnings or sanitizer reports. Feature-disabled
arm64 memory/rmap/swapfile objects compile. Checkpatch reports no errors;
its two warnings concern the native-style swap-flag data_race and debug-only
writeback assertion. Frozen artifacts are under swap-exclusivity-v1.

Pagemap, scan and clear_refs
----------------------------

Pagemap read offsets and PAGEMAP_SCAN alignment, ranges and max_pages now use
the target mm's immutable granule, including cross-ABI readers. Pagemap PFNs
and swap offsets are expressed in that same target granule; a 4K swap offset
includes its quarter within the native swap entry. Convert PFNs to physical
byte addresses before comparing different ABIs, or to native PFNs before
using global kpage interfaces. AT_PAGESZ in the target auxv identifies its
user granule. The PFN capability gate remains unchanged. Anonymous exclusivity
uses the exact slot mapcount; file exclusivity remains conservative at native
folio granularity until per-quarter file mapping counts are integrated.

clear_refs and pagemap/scan PTE and hole walks use target-sized increments.
Six EL0 checks per ABI cross both 2 MiB and 32 MiB table boundaries, verify
present/hole/guard/PROT_NONE entries, batch versus single reads, scan limits,
clear_refs mapping/data preservation, cross-ABI inspection, mixed-size memfd
physical identity and unprivileged PFN hiding. The first expanded fixture
incorrectly allowed a later alias to fill its intended hole and failed to
restore its own dumpability after setuid; corrected fixtures pass in KVM
software-PAN and patched-QEMU KASAN. These are fixture corrections, not kernel
security changes. Soft-dirty and asynchronous userfaultfd write protection are
not runtime-qualified by these configurations.

Both VMs also pass the late swap suites and all-byte pageout readback with
new pagemap checks proving swapped-entry presence and quarter offsets.
Feature-disabled arm64 and x86 proc objects compile. Frozen source and logs
are under proc-walkers-v1. smaps/PSS, NUMA statistics and their shared-memory
accounting remain a separate unfinished gate.

Resident PSS and quarter sharing
--------------------------------

File mapping metadata now records four alternative mapping counts per native
page. Native rmaps do not update these atomics; the native count is recovered
from the existing aggregate count when collecting statistics. Fork, insertion,
COW, zap (including deferred aggregate-rmap removal), reclaim and migration
maintain the quarter counts. A new test verifies mixed aliases, fork/COW and
exit; migration success/rollback and reclaim cases verify counter restoration
and zero counts after unmap. The metadata adds sixteen bytes per native page
when the experimental feature is configured. Native-path latency and broad
workload costs are unmeasured. The feature now requires per-page mapcounts;
NO_PAGE_MAPCOUNT is not yet a supported experimental configuration.

Resident smaps/PSS/private/shared totals and pagemap exclusivity use the exact
physical quarter counts for alternative leaves. Native file mappings split
their PSS contribution by physical quarter when sharing with alternative mms.
Anonymous leaves use their own slot mapcounts. NUMA PTE walks use user-granule
counts and the same sharing snapshots. Swapped shmem range accounting clips
native swap-cache entries to the requested byte range, including multi-index
xarray entries; a synthetic-xarray kernel test checks boundary clipping.

Seven EL0 checks per ABI cover dense anonymous RSS/PSS, fork, one-leaf COW,
child exit, NUMA page units and mixed-granule file aliases. Sharing one 4K
quarter of a native 16K mapping reports 14 KiB PSS to the native mapping and
2 KiB to the small mapping; after exit, the surviving mapping recovers full
private accounting. KVM with NUMA and software PAN passes all fourteen new
checks and 96 kernel cases (fourteen skips). Non-NUMA KASAN passes twelve
new checks with two explicit NUMA skips, plus 90 kernel cases (twenty skips).
Both retain passing prior EL0 suites. A separate object build covers NUMA
with PROC_PAGE_MONITOR and DEBUG_VM disabled; feature-off arm64 and x86
proc/shmem objects compile. The first attempt to disable PROC_PAGE_MONITOR
was overridden until EXPERT was enabled; only the nomonitor log proves that
configuration. Checkpatch's remaining warnings concern fixture formatting
and userspace sscanf style, not build failures.

This establishes resident accounting only. Anonymous SwapPss still divides
by native swap-entry reference counts, which combine distinct quarters; it
requires per-quarter swap reference tracking. Concurrent reporting races,
real swapped-shmem pressure and broader NUMA/migration scenarios remain to be
tested. Frozen evidence is under resident-accounting-v1.

Per-quarter anonymous swap references
-------------------------------------

Swap clusters now allocate quarter-reference metadata on first alternative
PTE use. Native and quarter counts change under the same existing cluster
lock. Fork, swap-in, zap, swapoff and unmap rollback maintain both counts;
empty-cluster table release checks and frees the quarter metadata. Allocation
failure leaves the PTE and native reference count unchanged. The metadata
cost is four unsigned counters per native swap entry in an affected cluster,
plus one pointer per cluster in feature-enabled builds. Allocation-failure
injection and broad memory-pressure cost remain untested.

SwapPss now uses the selected quarter's count. The late swap regression checks
counts of 1 before fork, 2 after fork, independent decrements on swap-in/COW,
and zero after exit. EL0 tests verify full SwapPss, equal fork sharing,
individual leaf changes after child COW and full recovery after child exit,
for both ABIs. All bytes are still checked after each 4 MiB pageout round.
KVM passes thirteen late cases, KASAN twelve filtered cases, plus both EL0
accounting sequences and the general suites. Feature-off arm64/x86 objects
compile; an explicit SWAP=n, NUMA=y, DEBUG_VM=n object build verifies stubs.

An initial KASAN fixture failed because a native PAGEOUT pass left eight pages
resident, with zero bad data bytes. PAGEOUT can skip busy folios; the revised
fixture checks pagemap and retries advice within a fixed bound to establish
its all-swapped precondition before asserting complete-range SwapPss. This
changes test setup only. Earlier failure output is retained separately from
the passing complete-fixture logs. The previous anonymous SwapPss gap is
superseded. Recovery of slot exclusivity after a fork sharer exits remains
unfinished; the current fast path still depends on the carried exclusive bit.
Artifacts are under swap-quarter-counts-v1.

Ownership recovery after a fork sharer exits
-------------------------------------------

Slot reuse now proves a sole mapping and the expected typed reference count
under the owner lock, with the folio and PTE locked. Swap-in and swapoff also
require a sole reference to that exact swap quarter. A resident write fault
requires no remaining swapped alias. Stable-writeback restrictions remain in
place. Extra GET references or pins prevent reuse, preserving their original
data while the writer receives a copy. Still-shared resident slots skip the
new folio-lock attempt.

New kernel cases check cached and evicted swap-in, swapoff, ordinary resident
COW, physical backing identity and retained-GET isolation after child exit.
The initial new swap reuse tests failed before this fix; their output and
Image are retained. Real EL0 tests verify unchanged physical backing across
post-exit resident writes for both ABIs, and a third 4 MiB rewrite/pageout
round verifies all bytes after the swap-sharing exercise. KVM passes seventeen
late cases; patched-QEMU KASAN passes sixteen applicable late cases and the
general suite (91 passing kernel cases, 24 explicit skips). Non-NUMA proc
fixtures skip only their NUMA check. The final SWAP=n object also compiles.
KVM's general run predates only the equivalent mapcount early-out that avoids
locking still-shared slots; its final late run uses the final Image.

This supersedes the post-fork ownership-recovery gap above. It is correctness
and physical-reuse evidence, not a broad performance measurement. Stable
writeback under pressure, allocation-failure injection and hardware M5 testing
remain open. Frozen artifacts are under cow-reuse-v1.

ELF core dumping
----------------

Alternative core reads hold typed GET references to individual user leaves,
including swapped leaves and file-cache quarters. The existing native path
still uses native page references. FOLL_DUMP skips absent anonymous tables
and zero PTEs, retaining sparse output without faulting in zero-filled memory.
The dump emitter and machine-check copy helper accept the selected native-page
byte range. ELF PT_LOAD alignment, NT_FILE page units and offsets, header-only
lengths and minimum dump-size admission follow the dumped process granule.
Header-only filtering also requires a zero subpage file offset. An optional
binfmt minimum-size callback preserves other formats' existing fixed limits.

The dedicated disposable-VM fixture parses actual core files for both ABIs.
Its 32 checks cover protected anonymous bytes, unfaulted file quarters,
nonzero native file indices, swapped data (pagemap verifies every leaf is
swapped before the crash), vDSO ELF contents, sparse zeros and allocated size,
DONTDUMP, executable/non-executable ELF header filtering, AUXV and NT_FILE,
and one-page/below-one-page RLIMIT_CORE admission. The earlier kernel failed
eight of the first nine 4K checks; the first implementation then exposed an
unconverted file offset. Both final KVM/software-PAN and patched-QEMU KASAN
runs pass all 32 checks. The full prior EL0 regression also passes both VMs.

The initial KASAN size-limit fixture asserted logical file size <= RLIMIT_CORE.
Core sparse seeks are not charged to the emitted-byte budget; the corrected
fixture checks admission and that no mapped-memory data was emitted with a
one-page budget. The earlier failing run lacks exact size diagnostics, so its
precise size difference is unknown. Final runs log actual size and first-data
offset. Separately, a general KVM regression caught a rare pre-existing stack
test boundary: arch_align_stack subtracts less than PAGE_SIZE but then rounds
down to 16 bytes, allowing exactly PAGE_SIZE displacement. The assertion now
includes that endpoint; stack code is unchanged.

Feature-disabled arm64/x86 objects compile. RISC-V no-MMU coredump/FDPIC objects
compile with the existing I/O-helper warnings. An initial attempt explicitly
requested the disabled binfmt_elf.o target in that no-MMU configuration and
failed on MMU-only definitions; it is not a supported configuration or passing
build. Pipe/socket core collectors, credentials, interrupted writes, hardware
machine-check injection and application debugger interoperability still need
qualification. Frozen source, Images and logs are under core-dump-v1.

Mixed-ABI context and ASID rollover stress
-----------------------------------------

A disposable-VM PID-1 fixture launches 70,000 alternating native/alternative
ELF processes while persistent workers of both ABIs migrate across four CPUs.
All use the same virtual addresses with distinct data, writable/read-only/guard
leaves, signal recovery and repeatedly rewritten executable code. Patterns mix
high serial bits so allocator rollover does not merely repeat the same expected
bytes. The native parent also checks its own mapping after each child exits.

A read-only KUnit-only parameter exposes the actual ASID generation; assertions
require at least two measured rollovers. KVM presents 256 ASID entries and
passes 70,000 launches with both hardware and software PAN, crossing 556
rollovers per run. Persistent workers each execute hundreds of thousands of
migration/JIT/data cycles. Final software-PAN rerun uses the final source;
the hardware-PAN run predates only the explicit capacity-test knob, which is
absent from its command line and defaults off.

QEMU max presents 65,536 entries. Its initial full KASAN stress was stopped
after 2,048 reported launches without rollover; it is incomplete coverage.
An explicit arm64.user4k_test_asid_bits=8 boot argument, compiled only into
the KUnit configuration, restricts allocator capacity to 256 without changing
hardware ASID width or normal boot behavior. The 2,048-launch KASAN run then
passes sixteen measured rollovers and more than 33,000 cycles in each persistent
worker, with no sanitizer report. This deliberately increases test rollover
frequency; its runtime cost is unmeasured and it is not a production setting.
Natural 16-bit-pool rollover remains unqualified. Frozen artifacts are under
context-stress-v1. These checks do not establish all concurrency or application
workloads as supported.

Memory locking in user granules
-------------------------------

mlock, mlock2, munlock and mlockall use target-page alignment and locked_vm
units; RLIMIT_MEMLOCK remains a byte limit. Accounting uses a long count so
large deferred-lock ranges cannot wrap a signed 32-bit leaf count. Physical
NR_MLOCK remains native-folio accounting: locking a quarter keeps its 16K
backing resident, while VmLck reports the requested rounded user-page bytes.

Fresh anonymous mappings use a distinct rmap operation that defers initial
locking to LRU insertion, followed by one contribution per additional locked
leaf. Existing mappings retain the ordinary mlock path. This avoids duplicate
fresh-folio accounting and preserves neighboring locks on partial unmap.
File aliases use each mapped user leaf's contribution alongside native alias
counts. PTE walks and fully-mapped-folio checks use target geometry.

Migration removal and restoration now drain pending mlock operations at the
same points as the native path. The new locked migration cases initially
failed to migrate: pending unlock references prevented ownership transfer.
The drain fix passes dense/sparse, fork-sharing and intentional rollback cases.
Native arm64/x86 mlock/rmap/memory/GUP/migration objects compile.

Eight EL0 cases per ABI cover unaligned ranges, repeated/adjacent locks,
MLOCK_ONFAULT, MAP_LOCKED, fork noninheritance and COW, future/current mappings,
unprivileged limits and large deferred counts. Kernel cases verify a fresh
folio's four locked leaves decrement independently and mixed file aliases
retain the lock until the final mapping is removed. The initial kernel test
incorrectly inspected mlock_count after the last unlock, when its union storage
holds LRU links; it now checks the cleared lock flag. A prior fixture also
incorrectly expected PAGEOUT on a locked VMA to succeed; native behavior is
EINVAL. Both corrections preserve the actual locking assertions.

KVM and KASAN pass these cases. A fourth swap round locks a fully swapped
4 MiB range and checks every byte for both ABIs. An intermittent KASAN fixture
failure left eight native pages resident after repeated PAGEOUT. Captured
kpageflags showed mapped-once swap-cache pages without KPF_LRU. PAGEOUT drains
only the current CPU's pending LRU additions. A fixture-only bounded sweep of
allowed CPUs, restoring affinity afterward, immediately established full swap
coverage in captured runs. Six repeat runs pass, three using that sweep,
including one alternative-mm run. The production reclaim path was not changed
and the all-swapped accounting assertions remain intact. The earlier failure
is retained; no data mismatch occurred in it.

A later session made /home/dj/Work read-only and removed KVM access. Ongoing
writes therefore use /tmp/aurora-user4k-source, an overlay with private copies
of modified files and read-only links to the original source. New evidence
is under work/user4k-continuation in the workspace. The original worktree and
prior frozen evidence remain intact. The overlay has built and booted under
KASAN. Further pressure, file-THP, NUMA and application workload qualification
remain necessary; memory locking does not complete the process ABI.

Lazy discard of individual user leaves
-------------------------------------

MADV_FREE now records a per-slot lazy-discard hint under folio/PTE protection.
It requires a sole mapping and no extra typed reference at advice time, so an
existing fork sharer or pin cannot have its data discarded by another mapping's
new hint. The advice clears the selected PTE's young/dirty state; whole backing
moves to the normal lazyfree LRU only when every live slot has a hint. Partial
hints retain their neighbors' dirty and swap-backed state. Reclaim drops only
clean hinted leaves without additional typed references or pins. Writes through
PTEs are detected by the dirty bit; writable/dirty fragment operations cancel
the hint. A removed quarter is invalidated before its reference is released.
Fully hinted native backing is reclaimable without swap writes. Swapped advice
drops the exact quarter's swap reference and accounting, not neighboring entries.

Migration snapshots each slot's hint when its PTE is removed, while the source
is locked. A failed migration can restore the original hint; fresh reservations
clear stale hint state. smaps LazyFree counts clean hinted user leaves, including
partial native backing, and stops counting a leaf after an EL0 store dirties it.

Ten kernel scenarios cover partial/full discard, dirty cancellation, GET held
before/after advice, writable GET, dirty reference release, PIN retention and
partial/full migration. Eight actual EL0 checks per ABI cover retained bytes
before reclaim, partial discard and neighboring data, LazyFree reporting,
redirtying, full reclaim with no extra pswpout, selected swapped-entry discard
and fork isolation. The patched-QEMU KASAN run passes these plus the existing
execution and late-swap suites (including the new mlock/readback round).
Feature-disabled arm64/x86 and alternative SWAP=n objects compile.

Initial native-control failures reflected best-effort advice on pending per-CPU
LRU additions. The fixture now visits each allowed CPU for advice and restores
its affinity. Initial kernel partial-discard tests stopped at an unhinted first
leaf without swap backing; they now place the selected hint first to exercise
that path directly. The actual EL0 fixture covers arbitrary quarters with swap
available. The initial synthetic redirty test also used an unsafe direct PTE
update and triggered the arm64 access-flag warning; it now uses the fault path.
Those failing logs are retained. These tests establish the covered transitions,
not memory-pressure races, broad workloads or performance. Artifacts are under
work/user4k-continuation/lazyfree-v1.

Threaded execution and futex recovery
------------------------------------

A static glibc pthread fixture passes both page ABIs in KASAN TCG. Four workers
perform 128 concurrent rounds of mapping/protection, lazy discard and redirty,
DONTNEED, allocator, signal/TLS and per-thread next-exec request checks while
moving between two CPUs. Mutex/condition/barrier synchronization uses real
userspace futex operations. The main thread verifies its exec request remains
independent and forks eight times while siblings are blocked; child allocator
and inherited state checks pass.

Robust mutex owner-death recovery passes, including 128 repeated thread
creation/exit/join cycles. A priority-inheritance handoff is confirmed with the
waiter's /proc task syscall reporting FUTEX_LOCK_PI or FUTEX_LOCK_PI2 while the
owner holds the mutex; recovery after a PI mutex owner dies also passes.
Non-leader-thread exec succeeds for all four 16K/4K source/target combinations,
with two live sibling threads and the former leader removed by exec. Resulting
AT_PAGESZ and consumed next-exec requests are verified. These are targeted
concurrency tests, not all scheduling policies or application workloads.
Logs and reproducible initramfs are in work/user4k-continuation.

Pipe and socket fragment lifetime
---------------------------------

Alternative vmsplice now acquires typed user fragments and coalesces contiguous
quarters into native-sized pipe buffers. A shared reference bundle follows tee,
partial consumption and pipe transfer. Gifting a quarter does not authorize
stealing its containing native page. The native vmsplice path is unchanged.

SKB splice acquisition now retains anonymous quarters through a zerocopy
completion object in addition to the ordinary page references. An independent
SKB append copies shared ownership metadata before extending it. Kernel tests
cover pskb_copy, skb_clone, skb_split, full skb_copy, blocked quarter reuse while
references remain, and reuse after the final shared reference is released.
Socket-to-pipe transfer takes new typed references before releasing its native
page reference. The SKB completion uses atomic allocations; no transport is
forced to copy data by the acquisition helper.

KASAN TCG passes 10 userspace splice checks in each ABI: unaligned and empty
vectors, native-capacity coalescing, tee and partial reads across replacement,
fork/COW with retained data, buffered-file splice, partial failure, Unix socket
retention across partial replacement, socket COW, socket-to-pipe-to-tee, and
128 unread-socket teardown/reuse cycles. The ownership KUnit test and the
existing execution fixture also pass. Feature-disabled arm64 and x86 networking, TLS and splice objects compile.
These results do not qualify transport performance.

Unix, IPv4 and IPv6 socket families now admit alternative pipe buffers. IPv4
TCP loopback passes the same userspace checks; the COW and partial-replacement
checks explicitly cork TCP and require SIOCOUTQNSD to report all bytes unsent
before changing source memory. IPv6 TCP and IPv4/IPv6 UDP also pass the
expanded userspace fixture. UDP is
explicitly corked with no receive data before source replacement or COW; its
receive splice uses the existing copy_splice_read fallback. That fallback was
initially mistaken for unsupported receive splice in the fixture; the incorrect
assertion was corrected after inspecting sock_splice_read.
Other socket families remain gated while their lifetime chains are audited.
Combining distinct zerocopy providers remains unfinished. The added SKB helper
rejects combining its ownership callback with a different zerocopy provider.
These exclusions are development gates, not a completed socket ABI. All of the
new code remains experimental, and no M5 hardware result is claimed.

The expanded UDP control exposed an existing checksum accumulation error:
skb_splice_from_iter deferred updating skb->len until the full iterator batch,
but each fragment checksum used the unchanged skb->len as its byte position.
An odd-length first fragment therefore gave subsequent fragments the wrong
parity. A direct KUnit comparison against skb_checksum failed for both ordinary
and quarter-owned backing before the fix. Passing cumulative spliced bytes into
the checksum helper corrects that position. The direct test and all 104 EL0
checks now pass, including multi-native-page UDP payloads in both ABIs. Initial
failure logs and the pre-fix Image are retained. This checksum fix also affects
the native networking path; it is not a quarter-specific workaround.

The temporary source/build overlay was cleared by an environment refresh after
the first Unix tests. The source was reconstructed from saved bundles and the
recorded edits, rebuilt, and retested. The authoritative restored run is
``tcg-splice-restored.log``; reconstructed fixture source and a complete modified
source snapshot are saved under ``work/user4k-continuation``. Subsequent edits
are snapshotted there to survive temporary-directory resets.

TLS and bounded fragment ownership
----------------------------------

Typed reference sets now retain unique anonymous quarters across sk_msg clones,
transfers and TLS plaintext records. Mutating a shared set copies ownership
metadata; pruning releases quarters no longer represented by an SG ring or SKB.
KUnit exercises clone/coalesce, partial release, reuse, borrowed transfers and
1024 ring-wrap cycles. A separate SKB trim/append test failed against the previous
helper (nine references instead of eight), then passed with pruning. Its initial
fixture accidentally allowed skb_condense to copy the payload; the corrected
negative control associates the SKB with a socket and reproduces the retention.

TLS receive uses typed writable pins and physical offsets for alternative user
iterators. Synchronous completion releases those pins directly. Asynchronous
completion queues them before signalling completion; the existing process-context
wait drains the queue and dirties/unpins memory outside the crypto callback.
The native path keeps its existing ownership. Software TLS transmit uses typed
sk_msg ownership; device transmit records also retain typed references, but no
hardware TLS offload test has been performed. BPF redirection ownership paths
compile; actual BPF redirection is not runtime-qualified here.

The final normal and forced-asynchronous KASAN TCG runs each pass 148 EL0 checks
across native and alternative Unix/TCP/UDP IPv4/IPv6 workers. TLS 1.2 and 1.3 tests
include pending plaintext across replacement and fork/COW, multiple records,
shared-file receive with boundary guards, fragmented recvmsg and repeated pending
record teardown. The default-off KUnit-only tls.user4k_test_async control selects
a cryptd wrapper around a concrete synchronous cipher driver. The forced run
records 18 deferred pin releases and 18 completed releases; the normal run records
zero. This exercises software asynchronous completion, not hardware offload.

Evidence is in tcg-skb-prune-fixed.log and tcg-skb-prune-fixed-async.log with
Image-skb-prune-fixed. All 19 ownership KUnit cases pass, including the new pruning
case; other suites retain their documented skips. These results do not establish
a complete ABI, hardware qualification or application performance.

Direct block and iomap I/O
-------------------------

Alternative user iterators now acquire typed pins into bio-owned fragment
metadata. Bio vectors carry exact physical offsets and coalesce adjacent quarters
within a folio; the separate pin list preserves their individual lifetimes.
Alignment trimming releases discarded pins and restores the corresponding
iterator bytes. The existing dirty-page completion mechanism handles deferred
file dirtying. Split and cloned bios borrow vectors from the original submitter,
which owns the pins until completion, as for native bio pins.

The raw-device control passed ten checks for 16K and failed all ten for 4K before
integration. The integrated kernel passes 40 checks across raw virtio block and
ext4 iomap direct I/O in both ABIs: fragmented/empty vectors, exact offsets,
two-megabyte multi-bio transfers, partial faults and recovery, alignment trimming,
shared-file receive guards/dirtying, asynchronous AIO, writable-pin COW and 128
reuse/failure cycles. Ext4 reports EFAULT after the partial-fault case while the
raw-device path returns its completed byte count; both ABIs match their respective
backend. The fixture originally assumed raw-device semantics for ext4 and was
corrected after inspecting iomap error propagation and the native control.

Two new kernel tests verify coalescing with distinct pin ownership, lifetime
after originating-mm teardown, borrowed clone vectors and 1024 fully trimmed
failure cycles with exact reference and pin counts. These pass under KASAN, as
do the pre-existing enabled suites. Feature-disabled arm64 and x86 block/iomap
objects compile. The 148-check forced-asynchronous TLS regression also passes.
Evidence: tcg-dio-ext4-contract.log, tcg-dio-tls-regression.log and Image-dio-final.

The ext4 fixture selects disposable guest disks by serial. Initial setup failures
from reversed enumeration and a launch before a disk copy completed are retained;
they are not kernel ABI failures. Registered buffers, long-term pins, bounce I/O
and legacy direct-io consumers remain to be integrated and qualified. No storage
performance or hardware DMA qualification is claimed by these TCG results.

Native-page arrays and io_uring mappings
--------------------------------------

The native-page array mapper now expands backing pages into the target mm's
hardware leaves while retaining batches of up to eight PTE updates per lock.
vm_insert_pages_range accepts a checked byte offset and length, allowing a 4K
mapping of part of a 16K backing page. The existing vm_insert_pages API keeps
native-page counts; vm_map_pages applies both the native file offset and its
subpage remainder. Overflow and alignment checks precede insertion. As before,
an insertion failure can leave a mapped prefix that the caller must tear down.

io_uring uses the range interface for alternative ring mappings and keeps its
native mapping path. Ring selector offsets with a sub-native remainder are
rejected. The first control failed all seven 4K checks while native passed;
the final fixture passes eight checks in each ABI, including minimum mappings,
selector validation, small/large ring wrap, 8193 submissions across native pages,
unregistered direct I/O, forced worker-thread I/O, multi-bio transfers and enlarged
SQE/CQE layouts. Registered buffers remain the next integration step.

The added KUnit test covers reversed backing-page arrays, explicit physical
subpage offsets, a PMD boundary, invalid ranges, duplicate insertion, the legacy
array API, vm_map_pages offsets, accounting and complete reference release.
It and the enabled suites pass under KASAN. Feature-disabled arm64/x86 mapping
objects compile. The swap regression completes both 16-case runs and native/4K
EL0 pageout/readback with zero bad bytes. Evidence: Image-insert-range-final,
tcg-uring-range-final.log and tcg-uring-range-swap.log.

Long-term fragment pins and registered buffers
---------------------------------------------

Typed long-term pins now use the native long-term GUP migration checks. The
migration collector can traverse typed records, release their exact pins before
migration, and retry acquisition afterwards. Fault allocations run under the
existing memalloc_pin context. Native page/folio callers keep their representation.
A movablecore=50% KASAN run confirms actual migration of existing movable backing,
all four quarters' bytes and pin counts, and rejection of FOLL_LONGTERM on GETs.

io_uring registered buffers retain typed fragments until resource release. The
registration accounts unique native folios, preserving physical memory units;
each vector retains its own physical offset. Fixed-buffer import and vectored
import index logical 4K leaves independently of physical quarter positions.

The expanded fixture passes 19 EL0 checks in each ABI, including first/late buffer
slices, fixed vectored and multi-bio I/O, forced worker completion, source and
destination use after unmap/replacement, multi-buffer registration rollback,
64 registration/reuse cycles with balanced VmPin, shared-file mappings with a
different physical offset, and resource replacement through BUFFERS_UPDATE.
Evidence: Image-uring-register-first, tcg-uring-register-first.log and
tcg-uring-register-file.log. The full first run passes enabled KUnit suites;
the expanded file run repeats the five fragment tests. Feature-disabled arm64
and x86 GUP/registration objects compile. This does not qualify every io_uring
operation, user-provided ring memory, device pins, hardware DMA or performance.

GEM shmem and PRIME mapping
--------------------------

The GEM shmem path used by Asahi and VGEM now supplies an explicit physical
quarter offset to PFN insertion. Write-notify upgrades compare complete physical
addresses. Buffer indexing retains object-relative positions after VMA splits;
DMA-BUF bounds use bytes including the subpage remainder. GEM fake offsets still
identify exact object starts. Kernel physical pages, GEM backing and device page
tables are not resized by these CPU mapping changes.

The VGEM baseline reproduced three native failures (cold faults after protection
splits, partial unmap and a partial PRIME mapping) and six 4K failures including
quarter aliasing. The fix passes 12 EL0 checks in each ABI, adding cold remap,
fork, bounds, descriptor lifetime and PRIME sharing across native/alternative
exec. A KUnit case validates reversed physical quarters across a PMD boundary,
write-notify permissions, invalid offsets and special-PTE ownership. All enabled
KUnit suites pass; feature-disabled arm64/x86 GEM, DMA-BUF and memory objects
compile. Evidence: Image-gem-pfn-kunit, tcg-gem-baseline.log and
tcg-gem-pfn-kunit.log. VGEM tests the common mapping helper, not Apple GPU
rendering, device DMA or every imported-buffer exporter. Legacy PFN walkers and
contiguous PFN remap paths still require a granule-aware audit.

PowerPC's pgtable-frag.c and ptdesc::pt_frag_refcount provide an existing
example of page-table fragment lifetime management. They are useful design
references, not drop-in implementations of arm64 mixed-granule MM.

References
==========

* Arm, Learn the Architecture: Memory Management, section 7, Translation
  granule (101811_0100_00).
* https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/tests/x18_toggle_4k.plist
* Arm Instruction Set Reference Guide:
  https://documentation-service.arm.com/static/6245c734b059dc5ff9a8bdab
* Inspected kernel paths: arch/arm64/mm/context.c,
  arch/arm64/include/asm/{mmu_context,pgtable-hwdef,tlbflush}.h,
  fs/binfmt_elf.c, mm/{mmap,memory,mprotect,mremap,madvise,gup,rmap,shmem}.c,
  kernel/futex/core.c.

AArch32 final-ELF selection and native fallback
---------------------------------------------

The final ELF class selects the granule before begin_new_exec(). When staged
arguments use another granule, bprm_set_page_shift() allocates a new mm and
copies the temporary stack bytes without changing their virtual addresses.
It rechecks the target per-string limit and keeps the old mm and temporary
RSS charge until the copy succeeds. An allocation/copy/length failure drops
only the replacement. The compat context flag preserves the selected granule.
AArch32 mmap2 offsets remain 4K units and are expanded to full-width byte
offsets before alternative-granule mapping; the native alignment path remains.

The freestanding compat probes are linked separately with 4K and 64K maximum
segment alignment. Under KASAN TCG, automatic-4K runs pass auxv, a 90KB argument,
anonymous leaves, mprotect, misalignment rejection, mmap2 offsets including
above 4GB, fork/COW, signal return, kuser cmpxchg and compat vDSO time access.
The launcher covers scripts, a pending 4K request, ELF32-to-ELF64 native-default
exec, and 64 oversized-string failures preserving the old mm and request.

A second boot with id_aa64mmfr0.tgran4=f removes 4K from the sanitized capability
model. This override exists only in USER4K_KUNIT_TEST non-4K builds and can only
remove capability. The launcher verifies a 4K request returns EOPNOTSUPP. The
64K-aligned ELF32 and script then execute with AT_PAGESZ=16384; 4K-only mmap2
offsets return EINVAL. This is a capability-model VM test, not A9 hardware
qualification. Pure policy tests also cover native shifts 12, 14 and 16.

Ryan Roberts RFC patch 44 aligns kernel/vDSO sections using PAGE_SIZE_MAX;
patch 57 sets the boot-variable kernel Image page-size flag to zero, rather
than encoding a fixed granule. That flag describes the kernel image, not a
new userspace ELF opt-in. The current fixed-16K kernel retains its Image flag.
Source: https://lists.infradead.org/pipermail/linux-arm-kernel/2024-October/970037.html
and https://lkml.rescloud.iu.edu/2410.2/05663.html .

The expanded compat fixture also passes a real PT_INTERP mapping using a
freestanding fixed-address ELF32 interpreter, ELF32-to-ELF32 argument restaging,
and an AArch32 request for a 4K AArch64 child. This is loader coverage, not an
armhf libc/dynamic-linker qualification. The current full fixture records 72
ELF32 checks plus 10 launcher checks in automatic-4K mode, and 48 ELF32 checks
plus seven launcher checks with 4K capability masked out. The preceding full
KUnit/GEM regression had 84 main-suite passes, seven main-suite skips, two TLB
passes, 19 ownership passes, and 24 GEM/PRIME EL0 passes; the diskless swap
suite skipped all 17 cases.

64K-native kernel with concurrent 4K userspace (2026-10-01)
----------------------------------------------------------

The experimental option now accepts a 64K native kernel with 48-bit VA/PA.
The compiled hierarchy accommodates the four-level 4K user tables, while
native 64K tables retain their three-level hierarchy. Runtime folding and
selected-mm table predicates keep native and alternative walks distinct.
TTBR1 and native TG0 retain 64K geometry. Lower 4K tables pack up to sixteen
fragments per native owner. Swap PTEs retain all four slot bits without
reducing the existing swap type/offset fields. Proc proportional accounting
uses a native-sized slot array, and fault-around eligibility compares its
byte limit with the process granule.

KASAN TCG results include 84 main-suite passes (seven skips), two TLB passes,
19 ownership passes, and 16 disk-backed swap cases with no failures. The swap
checks use the exact aggregate counter, including its extended table, and
independent per-slot counts. Four 8MB EL0 swap/fork/COW cycles per ABI verify
actual nonpresent swap PTEs, restored patterns and child-write isolation.
Native 64K and alternative 4K thread/futex tests and all four nonleader-exec
transitions pass. A mixed memfd test maps the last physical 4K slot: the native
mapping's PSS changes from 64KB to 62KB and back, while its 4K peer sees 2KB.
The same proc fixture passes with a 16K native page (16KB to 14KB and back).

AArch32 probes pass automatic 4K selection on the 64K build. Masking 4K
capability with the KUnit-only override still permits the 64K-aligned ELF32,
script and PT_INTERP cases at AT_PAGESZ=65536 with native mmap2 restrictions.
This explicitly models AArch32 availability independently from 4K availability;
it does not establish behavior of any specific Apple SoC.

The generic DEBUG_VM_PGTABLE boot fixture reports an order-13 (512MB)
allocation failure on this 64K configuration, including in a 2GB guest.
The mixed-granule suites run afterward, but their success does not establish
that this separate generic fixture passed. No workaround was made the default.
This remains a test limitation to investigate. Results do not qualify Apple
hardware, larger-than-native leaves, 16K-on-64K userspace, or application-wide
compatibility/performance.

ASLR granularity and automatic THP eligibility (2026-10-01)
---------------------------------------------------------

Stack and mmap masks now retain their configured byte window while exposing
the process granule's additional low bits. Brk randomization and mmap/stack
alignment use that granule. Native masks and byte ranges remain unchanged.
The ASLR fixture sets randomize_va_space=2 in a disposable guest and samples
64 independent execs per ABI. On 16K and 64K builds, native AArch64, alternative
4K AArch64, and automatic-4K AArch32 show variation in stack/mmap/brk addresses;
the alternative ABIs also vary below the native alignment. ADDR_NO_RANDOMIZE
produces stable layouts. The preceding baseline lacked the extra low-bit
variation for 4K mmap/brk. Pure checks verify that the mask's byte span is
unchanged. These checks do not measure cryptographic randomness quality.

The broader 16K ASLR run exposed a native-THP eligibility gap: the explicit
MADV_HUGEPAGE rejection did not prevent automatic THP selection in an alternative
mm. A large-mapping test encountered an anonymous PTE without slot ownership
and a nonzero RSS counter at teardown. Shared THP eligibility now rejects
alternative geometry, including advice/forced-collapse paths, while native
eligibility stays intact. This enforces the already documented unsupported
THP restriction; it does not implement alternative-granule THP. Full KUnit and
ASLR reruns with transparent_hugepage=always pass on both builds. Native THP
support remains enabled. Feature-off arm64 and x86 core objects also compile;
the x86 THP object check requires enabling THP in its test configuration.

PFN ranges and bounded iomem access (2026-10-01)
----------------------------------------------

remap_pfn_range() now advances physical byte addresses at the selected user
leaf size and uses selected-mm table boundaries. Its input PFN still denotes
a native physical page. Tests map a native page plus one user leaf across
PMD/PUD/root boundaries and verify every physical offset, neighbouring holes,
special PTEs and unchanged native-page references on 16K and 64K kernels.

follow_pfnmap_start() now walks the selected geometry and returns the requested
physical byte's offset alongside its native PFN. A caller must explicitly opt
into this byte-granular result; legacy native-page consumers receive
EOPNOTSUPP on alternative mms until their ownership/range contracts are ported.
In particular, this does not claim alternative-mm VFIO or KVM support.
generic_access_phys() uses that offset, revalidates it, and bounds each transfer
at the user leaf and VMA end. Following leaves receive separate checks.

A QEMU-virt-only fixture reads the PL011's read-only identification registers
through a 4K VMA whose virtual/native offset deliberately differs from its
physical offset. Direct iomem access and access_remote_vm() return the correct
16 bytes, stop before the following unmapped leaf, and preserve the remaining
buffer. A write request against the read-only mapping fails before I/O.
No UART state registers are written. All 88 enabled main-suite cases pass on
both kernels, alongside two TLB and 19 ownership cases. This tests the generic
byte-access path, not Apple device DMA or GPU qualification. Diagnostic table
walks also use the mm's geometry and report its page size.

The capability-masked ASLR fixture additionally passes on both builds: AArch32
retains native 16K or 64K mappings and ADDR_NO_RANDOMIZE remains deterministic.

CPU capability lifetime (2026-10-01)
----------------------------------

Alternative-4K availability is now a system CPU capability. Exec/prctl selection
uses the finalized capability rather than only reading the sanitized register.
The normal arm64 late-CPU verifier therefore refuses an incompatible CPU after
4K support has been advertised. If the system capability is absent, CPUs may
still be admitted and native-granule processes remain usable. This is a global
capability policy; a restricted eligible-CPU domain remains future work.

A KUnit-only removal model, arm64.user4k_deny_late_cpu=1, makes CPU1 fail this
capability check during late verification. It never grants a capability, does
not modify a translation register, and is absent from non-KUnit builds. In a
two-CPU guest booted with maxcpus=1, tests create live mappings before requesting
CPU1 online. On both kernel granules, advertised-4K mode rejects CPU1 with EIO,
keeps it offline, and the worker and subsequent AArch32 exec remain valid on
CPU0. With id_aa64mmfr0.tgran4=f, the same modeled CPU is admitted; native
16K/64K worker mappings and AArch32 fallback then execute on CPU1. These are
capability-model tests, not a heterogeneous hardware qualification.

Compatible-CPU admission also passes on both builds: an existing 4K worker
migrates to CPU1 and verifies its bytes there, followed by a successful
automatic-4K AArch32 exec. Final full regressions retain 88 main-suite passes
(seven skips), two TLB passes and 19 ownership passes on each kernel. The
KUnit object also builds with TRANSPARENT_HUGEPAGE disabled.

Backing copy extents (2026-10-01)
-------------------------------

The arm64 subpage copy/clear helpers now accept an aligned power-of-two extent
within a native page. Existing 4K callers retain their wrappers. MTE tag helpers
use the same extent and retain their multitag transfer path when its block fits.
Tests cover 4K/16K/64K extents where native backing permits, copies from tagged
and untagged sources, exact data/tag clearing, and untouched neighbours. This
is groundwork for larger ownership identities, not a new process ABI.

Both builds pass 88 main, two TLB and 20 ownership cases; the extra ownership
case exercises these extents. Both also pass the 16 disk-backed swap cases and
actual EL0 swap/fork/COW rounds. The copy and ownership objects compile with MTE
and transparent hugepages disabled. Process selection remains native or 4K.


16K processes on a 64K kernel (2026-10-01)
-----------------------------------------

The experimental exec selector accepts 16384 on a 64K kernel when the system
advertises the 16K user granule. Zero or the native size retains the native
AArch64 default. AArch32 still selects 4K when available and otherwise the native
size, even if an inherited pending exec request selected 16K. No new ELF flag is
introduced; binaries must meet the selected granule's segment alignment rules.

The CPU capability is finalized across the system, with late CPU verification.
Removal-only KUnit models can mask ``id_aa64mmfr0.tgran16=0`` or deny a late CPU
via ``arm64.user16k_deny_late_cpu=1``. These models cannot grant hardware support.
Tests admit a compatible CPU with a live 16K worker, reject a modeled incompatible
CPU while the worker survives on CPU0, and admit the same modeled CPU when 16K
was never advertised. A separate run masks 4K while 16K remains available and
verifies AArch32 native fallback despite a pending 16K request.

Anonymous and table allocations retain immutable ownership granules through
reference release and RCU reclamation. The 16K tables use 47/36/25/14 shifts and
two root entries for a 48-bit address space. File accounting distinguishes
actual leaf count from 4K byte coverage: one 16K mapping contributes one rmap
entry and covers four accounting slices. PSS weights each slice independently.

The 16K userspace smoke fixture exercises anonymous mappings, fork/COW,
independent protection, DONTNEED, pthread stacks/TLS, vDSO, private file COW,
shared-file bytes and PSS with a native 64K process. It also runs with KPTI forced
on. The extended fixture executes the page-contract program in another 16K mm.
The swap fixture runs four 8 MiB disk-backed rounds per ABI (64K, 4K and 16K),
checks actual swapped PTEs and swap writes, verifies every restored byte, and
forks while mappings are swapped to check COW and parent preservation.

The new fixtures are ``user16k-smoke-init``, ``cpu-granule16-init`` and the
extended ``granule-swap-init``. They are disposable-guest PID1 programs; the
smoke initramfs also needs ``/page-contract`` and the CPU fixture needs the ELF32
``compat-aslr-probe`` installed as ``/compat-aslr``. The swap fixture requires a
guest-only swap device with a header matching the native kernel page size.

These results do not qualify larger-than-native user granules, alternative
granules on a 4K kernel, legacy unported pin consumers, all huge-page interfaces,
Apple GPU DMA, or performance under Windows-game workloads. The option remains
experimental and disabled by default. The generic 64K DEBUG_VM_PGTABLE huge
allocation coverage gap described above remains separate and unresolved.

Larger tables and exact reservation units (2026-10-01)
-----------------------------------------------------

Internal 64K table geometry now works on a 16K kernel. The allocator uses a
compound native allocation for a larger hardware table, retaining its immutable
shift and owner through RCU reclamation. Interior PTE/PMD pointers resolve to
the compound head for descriptor and lock lookup. A single larger table has
one owner reference and never enters the cache of unissued smaller fragments.
The 64K geometry uses root/PMD/PTE shifts 42/29/16 and folds the PUD level.

The software fixture allocates aligned 64K data backing, walks mappings across
all table boundaries and the top of the 48-bit address space, verifies table
bytes and native backing counts, and exercises generic partial/full teardown.
Separate ownership tests cover every native-page boundary inside a table,
contiguous-entry accessed/dirty aggregation, locks and delayed freeing. These
are internal fixtures: public 64K process selection on a 16K kernel remains
disabled. Coarse data faults, coherent file backing, reverse mapping and swap
are not implemented by this table work.

Locked-VM accounting now uses ``MM_ACCOUNT_SHIFT`` units, 4K in mixed-granule
builds and the native unit otherwise. This supersedes the earlier milestone's
statement that ``locked_vm`` uses user granules. A native physical pin can be
smaller than a coarse user leaf; representing both in a common unit preserves
exact independent charge/refund pairs. VMA deltas, limits, proc ``VmLck`` and
the direct VFIO limit reader convert explicitly. Logical VMA/RSS counts and
native physical ``pinned_vm`` counts keep their existing units.

Alternative commit reservations retain aggregate logical counts. Smaller
leaves round the aggregate to native commitment units; larger leaves multiply
with an overflow check. Rejected charges leave both counters unchanged. The
per-process overcommit reserve converts through bytes before dividing by native
pages, avoiding an unsigned shift underflow for larger user granules.

Both native kernels pass the accounting KUnit cases. The 16K kernel additionally
passes internal 64K reservation/overflow/refund tests. The expanded mlock
fixture passes nine cases per enabled ABI, including locked grow/shrink/move,
partial unmap, fork, limits and counts exceeding signed 32-bit user leaves.
The freestanding ELF32 ``compat-mlock-probe`` passes six cases on each kernel
with automatic 4K selection and again with 4K capability masked, retaining
native-16K/native-64K restrictions. Its PID1 harness is ``compat-mlock-init``;
install the probe as ``/compat-mlock`` and pass ``--no4k`` only for a guest booted
with the removal-only ``id_aa64mmfr0.tgran4=f`` model. This does not test Apple A9
hardware. Feature-disabled arm64 and x86 accounting objects compile, including
the x86 VFIO consumer; this is not a VFIO device runtime test.

Internal coarse anonymous mappings and shared zero backing (2026-10-01)
---------------------------------------------------------------------

A 64K base userspace PTE on a 16K kernel now has internal fault, fork and COW
fixtures. Its mapping owns four native page references and reverse mappings,
while RSS, PTE iteration and TLB ranges count one logical page. Allocation and
COW copying cover the complete leaf; the arm64 ordered zero-allocation hook
retains MTE initialization. Sharing and restoring exclusivity cover every
constituent native page. An interior-page pin forces a complete private copy
on fork. The no-THP configuration selects MM_ID for large anonymous-folio
ownership and reuse.

Ordinary zapping now keeps physical reference/rmap counts separate from logical
PTE counts. A two-leaf fixture removes one leaf, checks its surviving neighbor,
remaps it and removes both as a batch. Reference scanning and mprotect batching
use the target mm's geometry. The anonymous fault fixture checks zero-page COW,
direct write faults, fork sharing, child COW, every native page's contents,
parent reuse and exclusivity, RO/RW protection changes, and teardown accounting.
These checks pass with and without THP.

The experimental arm64 configuration reserves a 64K-aligned shared zero region.
All covered native PFNs are recognized as zero backing, and MTE setup clears
tags across the whole region. Native and alternative read faults retain special
read-only zero PTEs with zero anonymous RSS. The test reads every byte through
copy_from_user for each hardware-supported granule, including an internal 64K
mm on the 16K kernel. Other configurations retain PAGE_SIZE zero backing.

These are internal coarse-MM foundations, not public coarse-process support.
The public 64K-on-16K selector is still disabled. Coarse file-cache backing,
swap, reclaim/migration, GUP and device integrations remain unfinished. Native
4K kernels also still lack alternative process granules.

MGLRU table geometry and boundary aging (2026-10-01)
-------------------------------------------------

MGLRU's PTE/PMD/PUD traversal, cross-VMA scanning and look-around paths now use
per-mm table dimensions, boundaries and leaf batching. Existing native index
helpers are retained except that a runtime-folded PUD always has index zero.
This distinction matters on the native 64K configuration, whose compiled table
levels also accommodate smaller alternative granules. A stronger boundary test
caught an out-of-bounds read from using the compile-time PUD index; the runtime
fold check fixed that observed failure.

The disposable PID1 selftest ``mglru-granule-init`` is linked statically with
64K ELF segment alignment. It verifies that MGLRU and MM walking are enabled,
then executes each enabled ABI. Anonymous and shared memfd mappings straddle
eight address boundaries, from 2^21 through 2^47. After an explicit first-access
warm-up, four cycles check fully referenced mappings before aging, zero
referenced bytes in every region after forced aging, scan-counter evidence,
and every data byte. The fixture pins its allocation phase to CPU0 and fills
scratch backing to publish earlier per-CPU LRU additions. Its ``--concurrent``
mode launches all enabled ABIs together: a native coordinator ages their mms at
pipe barriers, while each worker alternates CPU0/CPU1 between write/check rounds.
Both the sequential and concurrent modes pass on the 16K and 64K kernels. These
are bounded coverage tests, not performance benchmarks or sustained-pressure
qualification.

The boundary tests pass for native16K/4K and native64K/4K/16K processes. The
MGLRU-enabled main/TLB/data suites pass, as do actual disk swap and fork/COW
regressions for those enabled ABIs. Mlock, lazy-free reclaim, thread/futex and
non-leader exec regressions also pass, as do AArch32 automatic-4K and masked-4K
native fallback memlock probes. These results do not establish exhaustive MGLRU
concurrency or sustained memory-pressure qualification.

Global commit-accounting fixtures now drain prior deferred file releases before
taking baselines. A captured run refunded eight native pages during that drain;
the earlier unisolated test had observed an eight-page global drift while its
per-mm reservation stayed correct. Exact counter assertions remain in place.
The shared mlock fixture drains pending work on all CPUs before checking its
counts; the earlier one-count discrepancy has not been independently attributed
by a captured local-versus-global drain delta. No production mlock workaround
was added.


Memory-cgroup pressure and slot metadata (2026-10-01)
---------------------------------------------------

The primary 16K KASAN profile now enables cgroup v2 memory accounting.
``memcg-pressure-init`` runs native 16K and alternative 4K processes together,
each with a 32 MiB anonymous mapping, inside a 48 MiB memory limit. It checks
all bytes through permuted page access, four rounds of concurrent fork/COW,
and reclaim driven by the actual limit. A 256 MiB virtio swap device supplies
headroom for native backing, COW fragmentation and resident swap cache.
It requires swap/scan/steal/limit-hit evidence without OOM and checks that
anonymous and swap charges disappear after both workers exit. A native-only
control is available through ``--native-control``.

The native-only control and one mixed run passed this pressure test before
metadata charging was corrected. This is not yet repeatable pressure
qualification: an earlier mixed run exhausted a 64 MiB swap device, and
another run with the larger device reached OOM with nearly all resident
pages under writeback despite free swap. The latter cause is unresolved;
passing later runs must not be presented as fixing it. No reclaim throttling
or reduced-concurrency workaround has been introduced.

``memcg-accounting-init`` compares separate 32 MiB native/4K workloads under
cgroup accounting. Before the fix, the 4K workload reported only about 12 KiB
of unreclaimable slab. Slot pools and owners now request ``__GFP_ACCOUNT``;
replacement owner metadata preserves the original object's charge domain
when migration runs in another task. The test then reports about 1.6 MiB of
slab for the alternative workload and passes a conservative lower-bound
check. Allocation failures retain the existing rollback paths. Migration
metadata allocation stays nonblocking under the source/destination folio
locks. Cross-cgroup migration has not yet been separately exercised.

The memcg-enabled internal coarse-zap fixture also now charges its synthetic
folio before partial unmap can enqueue it on a memcg deferred-split queue.
The original uncharged fixture produced a warning in teardown. With the
fixture corrected, the 16K profile passes 100 main, 2 TLB and 20 data KUnit
cases (unsupported cases remain explicitly skipped).


Droppable memory and distro runtimes (2026-10-01)
-----------------------------------------------

An actual Python 3.14 run exposed a 4K startup SIGBUS: glibc allocated its
fast random state with MAP_DROPPABLE, which mmap accepted but the smaller-
granule fault path explicitly rejected. Droppable data owners now retain an
immutable policy, remain non-swap-backed, and discard dirty slots during
reclaim while respecting retained GET references and pins. Migration carries
the policy to the new owner; zero-page COW follows the same allocation rules.
Ordinary MADV_FREE redirty handling is unchanged.

The new kernel regression covers dirty discard, retained GET/PIN protection,
migration, and zero-page COW followed by zero-filled refault. Both primary16K
and native64K profiles pass 101 main and 2 TLB cases, with 20/28 data cases.
Real installed Python and Bash run concurrently under native16K/4K and under
native64K/4K/16K. All pass page-size reporting, droppable fork wipe/discard and
glibc random state, file mmap offsets/resize/flush, fork COW and protection
faults, threaded compression/hashing, SQLite WAL/mapped reads, socket polling,
cross-ABI dynamic exec/memfd sharing, and Bash allocation/pipes. The initial
native64K sharing fixture addressed byte65536 beyond its65536-byte mapping;
its corrected128KiB mapping passes. This was a fixture range error.

Memory-cgroup pressure now also passes on fresh swap disks and with an
explicit4MiB/s emulated write cap in both native-only and mixed16K/4K runs.
The cleanup assertion first waits for writeback and explicitly reclaims
unmapped swap cache before requiring zero anonymous/swap charges. Native
control demonstrated why an idle-delay-only assertion was invalid: clean
unmapped swap cache can persist after I/O completion. The earlier isolated
writeback-heavy OOM has not been reproduced or explained and remains open.


FEX and ELF/BSS mapping extent (2026-10-01)
-----------------------------------------

FEX-2604 failed to reserve its guard page at sbrk(0): the mapping returned
EEXIST. ``vm_brk_flags()`` still rounded the ELF/BSS request to native PAGE_SIZE,
so a smaller-granule process could receive extra mapped bytes beyond its
reported break. It now uses ``mm_page_align(mm, request)``. The regression
checks the exact VMA extent and accounting for a one-granule-plus-one-byte
request, then reserves the next user page with MAP_FIXED_NOREPLACE.

The installed FEX emulator now passes a freestanding x86-64 workload as an
AArch64 process with AT_PAGESZ=4096 on the 16K test kernel. A constructor checks
the actual emulator process granule. The x86 workload checks its 4K auxv,
independent 4K permission faults, fork/COW, mremap content and zero extension,
shared memfd mappings at a 4K file offset, RW/RX JIT code replacement, and
getrandom. The test needs a 2 GiB guest: its initial 768 MiB environment
refused FEX's 1 GiB allocation under ordinary heuristic overcommit. The
passing run changes no overcommit policy. This tests FEX directly in the
kernel guest's userspace; no nested VM is used by FEX. Windows games, GPU
access and Apple-hardware performance have not been tested.


FEX re-exec and inherited page-size policy (2026-10-01)
----------------------------------------------------

The native64K FEX run exposed a second geometry assumption: ``mm_find_pmd()``
used native table offsets. Its use by ``vma_adjust_trans_huge()`` during a VMA
split reached an invalid PMD lock. The lookup now uses per-mm offsets at each
level. A sparse two-native-PMD VMA regression faults one smaller user leaf,
splits its protection, and checks its neighbour. The FEX memory contract now
passes on both native16K and native64K kernels.

Extending that workload to exec another x86 process exposed a policy gap:
FEX's replacement AArch64 process reverted to native16K and jemalloc rejected
it. The private ``PR_AURORA_SET_DEFAULT_PAGE_SIZE`` and matching getter add
an explicit, per-thread inherited exec preference. Fork copies it and exec
preserves it. The existing one-shot request takes precedence and is consumed
only by successful exec. Selecting native through the one-shot setter while
an alternative preference exists installs a native override for that exec;
the inherited preference is still available afterward. The default setter
neither consumes nor replaces an existing one-shot request. Neither setter
changes the caller's current address space. Ordinary tasks still default to
native pages.

``tools/aurora-userpages`` opts a process tree in by default. Its ``--once``
option retains the original one-exec behaviour. ``native`` clears an inherited
alternative preference unless combined with ``--once``. All choices remain
subject to hardware capability checks. These private prctl numbers are not
upstream assignments.

The focused guest tests pass repeated exec, failed-exec preservation, native
override and return to the inherited size, clearing the inherited preference,
invalid arguments, per-thread independence and non-leader-thread exec. They
cover 4K on native16K and 4K/16K on native64K. The extended actual FEX workload
passes its memory/protection/COW/JIT contract in both the original emulator
and its fork/exec replacement, with the AArch64 AT_PAGESZ checked as 4096.

AArch32 selection continues to take precedence: 4K when supported, native
otherwise. Tests mask out 4K support on both native kernels and check the
remaining mapping-alignment restrictions. On native64K, an inherited16K
preference survives AArch64 -> native64K AArch32 -> 16K AArch64 execs without
forcing unsupported 4K pages or making the AArch32 process use16K pages.


Actual x86-64 Bash and Windows memory contracts (2026-10-01)
----------------------------------------------------------

A private build of FEX-2604 now runs dynamically linked x86-64 Bash and
Wine's Windows x64 memory-contract executable as 4K AArch64 host processes
on both 16K and 64K kernels. The constructor probe observes AT_PAGESZ=4096
in FEX, including its replacement processes. Native defaults remain unchanged.

The Windows test verifies 4K pages with 64K allocation granularity,
independent 4K permissions, a vectored access-violation handler, decommit and
zero-filled recommit without changing neighbours, thread access and shared
section aliases with independent protection. Both kernel configurations
passed with the default QEMU max CPU and SVE enabled. This establishes the
specified memory operations in Wine/FEX under TCG, not game rendering,
Apple GPU integration, native hardware performance or full ABI completion.

Three private FEX changes were needed during application testing: materialize
full-width constants for SVE address offsets; use the actual architectural
SVE vector length for MUL VL addressing; and batch a successful low-address
munmap while retaining the previous error fallback and bookkeeping. Their
focused tests and source patch are preserved separately. Installed host FEX
binaries are unchanged. These are userspace fixes, not kernel workarounds.

The disposable Wine fixture uses separate writable prefixes and a read-only
x86 runtime disk, selected by device serial. Its C: drive points to drive_c
and Z: to the extracted x86 root. Invoke the payload by its explicit Windows
path (Z:\wine-page-contract.exe); the earlier Unix-path invocation crashed
before reaching the payload on both native4K and mixed16K configurations.
That comparison used a feature-disabled build of the modified kernel source,
not an untouched upstream kernel. It does not exclude generic MM regressions.
Pipe-backed console output makes the payload checks observable.

The optional payload is built without a Windows SDK or CRT::

  make -C tools/testing/selftests/arm64/user4k OUTPUT=/absolute/output wine-contract

The target uses clang's x86_64-pc-windows-msvc target, llvm-dlltool and lld-link.
It does not add those tools to the normal Linux selftest build. The generated
wine-page-contract.exe must run inside a configured Wine runtime; simply
building the PE executable does not execute the test. Compile-time assertions
check the freestanding Windows x64 structure declarations.

Evidence: work/user4k-continuation/wine-memory-contract-v1 contains the
fixture source, logs, build inputs and hashes. Runtime dependencies and the
private FEX source revision are recorded there. The broader unresolved
larger-than-native granules, legacy GUP consumers, device integration and
hardware qualification requirements above remain open.


Prefault boundaries follow the process granule (2026-10-01)
--------------------------------------------------------

The three byte-range helpers fault_in_readable(), fault_in_writeable() and
fault_in_safe_writeable() previously advanced by native PAGE_SIZE. On a
larger-page kernel this could omit an interior smaller user page when checking
a range. They now advance and align by the current mm's immutable page size.
The architecture's finer permission probe remains in
fault_in_subpage_writeable(). Physical page/folio sizes are unchanged.

The new user_granule_prefault_test borrows isolated mms, starts 17 bytes into
a page and checks three-page ranges with a missing middle page, a read-only
middle page and three writable pages. All four public prefault helpers return
the expected unprocessed byte count. It covers user4K/native16K on the 16K
build and user4K/user16K/native64K on the 64K build. Both complete KUnit runs
pass: 104 main tests and two TLB tests, plus 20/28 data tests respectively
(with the documented skips). Feature-disabled arm64 and x86 mm/gup.o builds
also pass. This fixes range probing; it does not add legacy page-array GUP
support or change pin ownership. Evidence is in prefault-boundaries-v1.


Physical-page-bounded references and coarse-leaf COW (2026-10-01)
---------------------------------------------------------------

Typed user fragments now stop at the smaller of the process translation leaf
and the native physical page. For a larger user leaf, the walker selects the
interior native page and retains its ordinary folio reference or pin; it no
longer assumes an anonymous slot owner exists. Read pins on shared anonymous
backing use the ordinary unshare fault path. Smaller-than-native mappings
retain their existing slot references and pins. Core dumping advances by the
same fragment unit, so its scratch page and copy operations remain bounded by
a native page even when a user leaf is larger.

The internal 64K-on-16K fixture checks each zero-page reference, unaligned
references across every native page of anonymous backing, reference-count
balance, read-pin COW after fork, preservation of the original bytes, and
pinned-data lifetime after the child mapping is removed. It passes. This is
internal groundwork: public larger-than-native process activation remains
disabled, and file-cache promotion, swap/reclaim/migration and remaining
consumers still need integration before enabling it.

The core metadata conversion also handles both shift directions: NT_FILE
file offsets are converted from native page-cache units to the dumped mm's
page units, including larger user granules without a negative shift. A unit
test covers 4K/16K/64K units and large file offsets. The real core-dump fixture
now derives its native size from AT_PAGESZ. It passes all 32 checks for
native16K/4K and all 48 for native64K/4K/16K, including swapped data, protected
anonymous data, unfaulted file contents, sparse zero pages, AUXV/NT_FILE,
header filtering and core-size limits. It does not execute the disabled
64K-on-16K process ABI.

Final KUnit results: native16K passes 106 main, two TLB and 20 data tests;
native64K passes 105 main, two TLB and 28 data tests, with expected skips.
Feature-disabled arm64 and x86 gup/coredump objects compile. Frozen source,
Images, configs and runtime evidence are in gup-coarse-fragments-v1.


Memory-cgroup coverage on the native64K kernel (2026-10-01)
---------------------------------------------------------

The native64K validation configuration now enables CGROUPS, MEMCG and
CGROUP_PIDS. The previously recorded native64K runs had MEMCG disabled.
The unchanged kernel implementation passes its full KUnit set with the new
configuration: 105 main, two TLB and 28 data cases, with expected skips.

The pressure fixture now accepts native16K or native64K kernels and a 4K or
16K alternative worker. Each worker fills and verifies 32 MiB, forks and
changes a quarter of its logical pages while the pair shares a 48 MiB memory
limit and a 256 MiB swap device. Both native64K+4K and native64K+16K completed
four rounds with measured reclaim and swap activity, no cgroup OOM events,
correct bytes/COW isolation and zero anonymous/swap charge after exit and
explicit reclaim of any remaining clean swap cache. This is bounded pressure
coverage, not a claim about all I/O-stall or memory-exhaustion conditions.

The accounting fixture now checks both alternatives on a 64K kernel. For
32 MiB workloads it recorded 1,211,176 bytes of unreclaimable slab for user4K
and 1,143,056 for user16K, versus zero in the native64K control at the sample.
The native16K control/4K comparison also passed. These are observations from
the recorded debug builds, not a fixed ABI overhead or a performance claim.

The native64K runtime boots reported an optional debug_vm_pgtable allocation
failure for an order-13 (512 MiB) contiguous block, before the fixture started.
It occurred with both 768 MiB and 2 GiB guest RAM. The inspected debug code
explicitly skips the huge-page data tests when that allocation is unavailable;
its remaining tests and userspace continue. Record that coverage skip rather
than claiming a warning-free boot. The first runner omitted this warning from
its failure filter; the stricter rerun reported false despite the userspace
contract passing. Both raw results and the separate, explicit assessments are
preserved. No other BUG/WARNING/KASAN or pressure-test failure was observed.

This optional boot-debug allocation is distinct from the earlier native16K
memory-cgroup OOM. Its old log showed almost the full 48 MiB limit in pending
swap writeback with free swap available; why writeback did not complete in
time remains unproven. No root-cause fix for that old event is claimed.
Evidence and the exact MEMCG-enabled Image/config are in native64-memcg-v1.

Userfaultfd missing, minor, WP and pager-granule integration (2026-10-01)
======================================================================

With USERFAULTFD, PTE markers and architecture WP/minor capabilities enabled,
the focused ``uffd-wp-init`` executable now checks the full supported contract.
It runs native and 4K processes on a 16K kernel, plus native/4K/16K processes on
a 64K kernel. All five cases pass in ``tcg-{16k,64k}-uffd-contract-v2.log``.
The tests cover:

* Anonymous COPY/WP fault sequencing, ZEROPAGE and full-byte neighbour checks;
  batch backing packing verified from physical addresses; prefix accounting
  after EEXIST/EFAULT; unlocked source-page retries and fork/COW.
* Private sparse-shmem COPY/ZEROPAGE with unchanged file bytes, source retries
  and sub-native EOF boundaries.
* Private/shared shmem MINOR/CONTINUE/WP at non-native-aligned file offsets,
  alias identity/coherence, private COW, batch CONTINUE and truncation boundaries.
* MLOCK_ONFAULT during anonymous filling and shmem continuation, followed by
  munlock and discard; populated, unpopulated and asynchronous WP; per-leaf
  pagemap WP state; isolated POISON/SIGBUS behaviour.
* Native targets with 4K pagers and alternative targets with native pagers,
  passing the userfaultfd via SCM_RIGHTS and checking COPY/ZERO/batch contents.
* Exact-address/TID events, explicit fork events and child fault descriptors,
  default fork context removal, partial unregister and close cleanup.

These are bounded KASAN VM tests. They do not establish all userfaultfd race,
swap/pressure, hugetlb, shared-cache population, hardware or performance
behaviour. Native64 may skip debug_vm_pgtable's optional order-13 allocation,
as documented for the previous native64 memcg configuration.

Partial shmem population state: persistence foundation (2026-10-01)
================================================================

Shared missing-page COPY remains gated. Its backing state now has an inode
XArray indexed by native page-cache offset. Each value is a mask of missing
4K units; an absent value means fully populated cache contents. The folio lock
serializes updates. Cache removal takes i_pages before the metadata XArray
lock. Fresh cache insertion discards stale state; replacement by swap or folio
migration preserves it. Inode initialization covers allocation-failure cleanup,
and inode destruction frees the XArray. Metadata allocations use __GFP_ACCOUNT.

Native PTE/PMD installation, alternative file PTE installation and userfaultfd
CONTINUE mark exactly their exposed byte range populated. A bounded 4K alias
clears only its own bit; a native alias exposes and accounts for its complete
native granule. Missing neighbours remain physically initialized to zero.
This is the basis for coherent alias behavior, not completed shared-COPY
registration or fault delivery.

New real-kernel fixtures passed on native16 and native64: individual-index
truncation without losing neighbouring state, generic cache deletion, fresh
cache recreation, actual folio migration preserving both contents and state,
and ordinary mixed-granule alias mapping. The main KUnit suites now report
109 and 108 passes respectively, with the prior architecture/configuration
skips unchanged.

The dedicated arm64-user4k-shmem-swap suite passed all three cases on both
kernels using disposable virtio swap disks: writeout followed by complete
swap-cache eviction and disk-backed restoration; truncation while swapped;
and swapoff restoration. These fixtures seed the metadata internally; they do
not establish that userspace shared missing-page COPY works. The first two
fixture revisions failed because the direct writeout harness omitted its null
plug check and clean-for-I/O transition; those fixture errors were fixed, with
failed logs retained. The production swap path was not changed for those fixes.

Remaining integration includes atomic shared COPY/ZERO publication, distinction
between missing and minor faults for partial cache entries, buffered-write
validity updates, partial truncation/hole-punch validity and byte-granular unmap,
and cross-granule shared-file userspace/pressure tests. None may silently mark
neighbours populated merely because a native folio has been allocated.

Shared shmem missing-page copy and fault delivery (2026-10-01)
============================================================

Alternative shared shmem ranges can now register MISSING and resolve faults
with COPY/ZEROPAGE. Shared population uses a locked cache folio with independently
missing units. A reusable private source buffer per COPY request prevents user
source faults while holding an existing destination cache folio. VMA/byte-position
revalidation follows the unlocked retry. PTE collision, poison and EOF checks
precede destination writes and population-bit publication. A failed new insertion
removes its cache/accounting state; an existing shared folio remains intact.

Resident and swapped cache entries distinguish selected missing units from
present data. An inode flag is set before inserting partial backing, so minor
faults wait for initialization instead of observing the allocation as complete.
Native files which never use partial population retain the existing early minor
path. CONTINUE rejects a wholly missing leaf even when its native folio exists.
Ordinary buffered writes populate only the initialized range. A native kernel
folio consumer receives fully populated backing for the range it can expose.

A MISSING-only remote-pager test exposed a fault-around bypass: the cache lookup
could map unfilled neighbouring leaves directly. File mapping now consults the
missing mask when selecting eligible PTEs; a huge leaf cannot cover missing
units of a missing-registered mapping. Populated leaves remain eligible for
batching. The corrected case passes for native16/4K targets on a 16K kernel
and native64/4K/16K targets on a 64K kernel, with pagers using a different granule.

The expanded ``uffd-wp-init`` userspace fixture passes on both Images in
``tcg-{16k,64k}-uffd-shared-copy-v5.log``. It checks two independent missing/minor
pagers on the same file, COPY/ZERO, rejection of CONTINUE into a missing leaf,
physical backing identity and packing, write protection and shared coherence,
source-page fault retries/failure, native EOF rejection, and short-range buffered
write isolation. Earlier tests wrongly required EFAULT from native EOF COPY:
the preserved native allocator returns ENOMEM when its index lies beyond EOF;
the controls now verify that existing behavior. The fault-around failure logs
are retained separately from the passing results.

Partial hole punch, shrink/regrow validity, broader shared-file swap/pressure and
race qualification remain unfinished. The inode mask still allocates metadata
when a previously fully populated native page becomes partial; that allocation
must be made safe before a non-failing truncate operation commits. Do not treat
the basic COPY tests or currently accepted range registration as proof that all
shared MISSING semantics are complete. The MISSING_SHMEM feature bit remains
unadvertised pending those operations.

AArch32 remote UFFD COPY address spaces (2026-10-01)
--------------------------------------------------

``UFFDIO_COPY`` now validates its source byte range against the calling
pager's address space, while destination address and length alignment use
the registered target's address space and granule. The source can be
unaligned and does not require a multiple of the pager's native page size.
Previously a native AArch64 pager's valid source above 4GB was rejected with
``EINVAL`` when its userfaultfd belonged to an AArch32 process.

The freestanding ``compat-uffd-probe`` and disposable PID1 harness
``compat-uffd-init`` reproduce that rejection before the fix and pass after
it on native 16K and 64K KASAN kernels. Install the probe as
``/compat-uffd`` and the harness as ``/init``. The pager sends three leaves
from an explicitly mapped high, unaligned source into each of anonymous and
shared AArch32 mappings. Tests check missing faults and every copied byte,
invalid destination alignment, invalid lengths, address overflow, and an
inaccessible source followed by a successful retry into the same leaf.

Both kernels also pass with the removal-only boot override
``id_aa64mmfr0.tgran4=f``: AArch32 retains the native 16K/64K ABI, and 4K-only
memory operations and COPY lengths remain rejected. This models the
no-4K capability case; it is not an Apple A9 hardware test. The generic
userfaultfd object compiles with the feature disabled on arm64 and x86.

This closes the pager/target address-space boundary issue. It does not
qualify partial shared-file hole punching, shrink/regrow, or all UFFD
operations; the broader limitations above still apply.

Partial shmem holes and allocation-free resident validity (2026-10-01)
-------------------------------------------------------------------

Partial hole punching and shrinking now invalidate complete 4K units within
retained native cache pages. Only bytes requested by the operation are zeroed;
partial 4K edge units remain valid. Mapping invalidation uses each process's
leaf size: unrelated fine leaves keep their PTEs, larger covering aliases are
revoked, and private COW copies survive a hole punch. Existing EOF truncation
rules still remove private copies of complete user leaves beyond EOF.

The sharing policy preserves data across granules. A coarse alias with any
populated units can refault and expose its zero holes; its entire mapped leaf
then becomes populated. A fine pager cannot subsequently overwrite those
bytes with COPY. The tests check this with a native writable alias and a fine
pager: COPY returns EEXIST after the coarse alias has refaulted and written.

Resident missing bits now occupy two bytes in the existing per-page extension.
Compile-time assertions verify that 16K and 64K builds do not enlarge that
extension. Folio locking protects these bits. Updating them during a partial
truncate cannot allocate, addressing the previous late-allocation limitation.
New cache insertion resets the state; migration, generic cache replacement,
and shmem huge-folio collapse transfer it. Physical folio splitting retains
state with each constituent native page. The latter huge-folio lifecycle still
needs dedicated end-to-end qualification, beyond code inspection and builds.

The inode XArray now persists only swapped masks. Reclaim reserves any required
records before committing the swap transition; allocation failure leaves the
resident cache and data intact and reactivates the folio for a later attempt.
Swap-in restores the resident bits before unlocking and removes the records.
The old all-resident inode-XArray description above is superseded by this
implementation. Allocation-failure injection and sustained pressure remain
untested for this new persistence path.

``shmem-hole-init`` runs as PID1 in a disposable guest. It checks native16+4K
and native64+4K+16K processes, shifted file-offset aliases, exact PTE retention
and invalidation via pagemap, independent COW bytes, coarse refault collision,
and partial-tail shrink/regrow. The pre-fix kernel fails the 4K hole test
because its PTE remains present; both new kernels pass. Passing ``--swap``
requires a dedicated guest ``/dev/vda`` swap disk. The fixture verifies the
selected shared mapping has swap usage and actual swap writes before resolving
an unfilled neighbouring leaf with COPY, then verifies all bytes of the
original populated leaf. This passes for 4K on 16K and 4K/16K on 64K. That EL0
check does not require eviction of the swap cache; the separate KUnit disk
roundtrip below does.

The dedicated shmem swap KUnit suite passes four tests on both kernels,
including full swap-cache eviction, swapoff, whole-index truncation, and a
new partial hole punched after cold swap-out while preserving an adjacent
populated 4K unit. Main KUnit passes 111/110 cases on native16/native64 with
16/17 intentional skips. It includes unaligned byte-range holes, buffered
repopulation, physical migration and generic page-cache replacement validity.
The existing mixed-page UFFD contract also passes after this metadata change.
Affected feature-disabled arm64 and x86 objects compile. These are bounded
correctness results, not sustained pressure, race, performance or hardware
qualification. The broad shared-missing feature remains unadvertised.

Refilling holes retained in externally referenced large shmem folios (2026-10-01)
-------------------------------------------------------------------------------

A large shmem folio can remain in the cache when an external GET reference
prevents splitting during a partial hole punch or truncation. The validity
bits correctly identify missing user leaves, but the native UFFD COPY and
ZEROPAGE paths previously tried to insert a new cache folio and returned
EEXIST. This was reproduced on both native16 and native64 kernels, with a
pipe holding a vmsplice reference to an unaffected part of the large folio.
The alternative small-leaf paths already handled this retained backing.

Native shmem insertion collisions on an inode using sub-native validity now
retry against the locked retained cache folio. The operation reuses its
already prepared source, checks missing state and PTE/EOF conditions, and
fills only the requested leaf. The ordinary native insertion path keeps its
existing allocation and copy behavior. The same installation helper serves
alternative leaves; no huge-folio disabling or splitting workaround is added.

The disposable PID1 ``shmem-thp-init`` fixture requests four-native-page
backing on a tmpfs mount with ``huge=within_size``. It verifies compound head,
tail and THP flags plus physical contiguity before the test, and confirms the
large folio remains compound when a pipe reference prevents splitting. Its
40 cases cover native16+4K and native64+4K+16K, COPY/ZEROPAGE, hole punch and
shrink/regrow, with and without the external reference. Invalid COPY sources
return EFAULT without consuming the hole; duplicate COPY returns EEXIST
without changing bytes. Successful fills preserve the expected file bytes
and data read from the pipe. Both KASAN guests pass, and feature-disabled
arm64/x86 userfaultfd objects compile.

The fixture must keep its filename until the initial large-folio allocation.
Unlinking before mmap selects anonymous-shmem policy, which did not allocate
the requested small large folio with the test guest's inherited policy. The
initial fixture failures were allocation-shape failures, not a kernel lack
of large-folio support. A temporary opt-in allocation trace established this;
it was removed from the final source. The later pre-fix native COPY/ZEROPAGE
EEXIST failures used verified compound backing and are separate evidence.

This covers PTE-mapped large folios retained by GET references. It does not
qualify PMD-sized collapse and alias transitions, write-capable FOLL_PIN/DMA
users, or concurrent truncation/refill. The broad shared-missing feature
remains unadvertised until those and the pressure/failure cases are resolved.

Native PMD collapse with fine missing ranges (2026-10-01)
--------------------------------------------------------

The native PMD-installation guard now checks for a wholly missing native
page, matching the native PTE fault policy. Previously it rejected a PMD
whenever any 4K unit in the folio was missing, even when every native page
contained populated data. The new test reproduces that rejection: physical
cache collapse succeeds, but native PMD promotion returns EINVAL. After the
fix, promotion succeeds once the last wholly missing native page has some
populated data. Installing the coarse mapping marks all exposed units
populated, as for native PTE aliases; this does not overwrite their bytes.
A wholly missing native page still prevents PMD installation and retains its
missing-fault contract.

``shmem-collapse-init`` is a disposable PID1 fixture for the 16K kernel. It
creates a 32MiB file with partial native pages and one wholly missing native
page retained inside a small large folio. A native process performs
MADV_COLLAPSE while a 4K process owns UFFD mappings. Compound flags, physical
contiguity and changed PFNs prove real cache replacement; missing and minor
events prove validity survives the transfer. After filling one 4K unit in
the wholly missing native page, ``ShmemPmdMapped`` confirms a full 32MiB
native PMD mapping. Formerly missing fine units then deliver minor faults,
and COPY cannot overwrite those units.

The native process is pinned to CPU0 and the fine process to CPU1. Eight
further cycles prime native PMD and fine PTE translations, punch a 4K hole,
and alternate pager COPY, native writes and writes through a fine alias
whose file offset starts at 4K. The native PMD mapping is split for each new
hole. Duplicate COPY rejection and subsequent minor faults verify that
refaulting either ordinary alias updates validity. A complete final 32MiB
byte comparison checks all populated values and zero regions. These are
coordinated cross-CPU tests, not an uncontrolled concurrent-writer test.

The KASAN/PAGE_TABLE_CHECK_ENFORCED 16K guest passes this fixture; both 16K
and 64K kernels pass the updated missing-native-page KUnit assertions and
main suites (111/110 passing, 16/17 intentional skips). The existing UFFD
contract passes both kernels, and feature-disabled arm64/x86 objects build.
The PMD runtime fixture intentionally covers native16. Native64's PMD order
exceeds this tree's MAX_PAGECACHE_ORDER, so these results do not establish
512MiB shmem PMD support there. Smaller large-folio native64 coverage is
recorded in the preceding section.

Writable FOLL_PIN/DMA, allocation-failure/pressure tests and simultaneous
mutation/refill remain outstanding. The broad shared-missing feature is
still unadvertised; these results do not complete the full process ABI goal.

Writable pins across partial shmem holes (2026-10-01)
----------------------------------------------------

A retained writable pin remains capable of accessing cache bytes after a
hole punch revokes its user PTE. The previous partial-validity implementation
marked those bytes missing anyway. A regression fixture reproduces this with
an io_uring registered buffer: a completed READ_FIXED writes through the old
pin, then UFFDIO_COPY incorrectly succeeds over the same retained range.
Two KUnit cases also reproduce stale missing state after native/fine long-term
pins and after direct memfd_pin_folios acquisition, including after unpin.

Partial-hole invalidation now checks the folio pin count after removing
covering mappings, with a barrier paired with fast GUP's PTE recheck. If
pinned, the removed range remains populated for missing-page classification;
ordinary byte zeroing still happens. Because file pins are counted per folio,
this classification is conservative if only a neighbouring range is pinned.
A subsequent hole after unpin can become missing again. Full-folio removal
continues to detach old pins from the file's replacement cache identity.

Direct memfd cache pins have no PTE fault to publish exposure. Their acquisition
and full-folio exposure now hold the folio lock, serializing with partial hole
punch and COPY. This lock applies to shmem memfd pinning in mixed-page builds
with USERFAULTFD; ordinary GUP and feature-off builds retain their paths.
This is synchronization for the cache-visibility contract; its performance
cost has not been measured.

The new shmem-pin-init disposable guest fixture uses actual registered-buffer
pins and asynchronous READ_FIXED/WRITE_FIXED. It checks hole punch and
shrink/regrow, retained-pin COPY rejection after unpin, full-native-page detach,
subsequent successful refill, every file byte and untouched neighbours. On
retained ranges it also runs 256 hole punches on CPU1 concurrently with 256
fixed writes submitted on CPU0, followed by a deterministic write/COPY check.
This tests kernel I/O through held pins, not physical device DMA or every GUP
consumer. Shared truncate/COPY/alias mutation without pins, allocation failure,
pressure and the other broader ABI limits remain open.

The same fixture exposed a separate 16K-on-64K io_uring bug: registration
assumed each returned fragment was 4K, rejected the correct fragment count,
and returned EFAULT. Registration now records the process's leaf size; fixed
buffer import uses that stored size for segment counting. Unaligned multi-leaf
READ_FIXED/WRITE_FIXED and reordered READV_FIXED/WRITEV_FIXED requests verify
segment traversal and guard bytes for native16+4K and native64+4K+16K.

Both native16 and native64 Image builds and the final pin fixture pass.
KUnit passes the two new pin cases on both kernels. Shared UFFD regression,
retained large-folio COPY/ZERO regression, and native16 PMD collapse regression
are recorded in the native artifact directory. Feature-off arm64/x86 affected
objects compile. The broad MISSING_SHMEM capability is still unadvertised.

Discoverable shared missing-page ABI (2026-10-02)
------------------------------------------------

Alternative-page processes now advertise and accept explicit negotiation of
UFFD_FEATURE_MISSING_SHMEM. This supersedes the earlier capability-gating notes
above. The implemented base-page shared COPY/ZERO semantics are discoverable;
UFFD MOVE and alternative hugetlb features remain separate, unsupported paths.
The native ABI and default process granule are unchanged.

The new shmem-race-init fixture runs 512 COPY attempts concurrently with 512
hole punches or shrink/regrow operations, a shifted fine alias, and a separate
native process repeatedly discarding/refaulting its PTEs. Native16+4K and
native64+4K+16K pass. After the actors stop, a fresh hole accepts COPY, both
aliases see the replacement, and every file byte has the expected value.
Private COW isolation is checked during hole-punch races. The resize case
keeps alias accesses within the retained EOF prefix. Its first version wrongly
rejected native ENOMEM on a truncated target; inspection confirmed the native
allocator's existing NULL/ENOMEM EOF response, and the test now records it
separately. No kernel behavior was changed to make that test pass.

The shared-memory pressure fixture limits a worker to 8 MiB, supplies a private
128 MiB guest swap device, and populates 16 MiB of backing with missing
neighbouring leaves. Native controls use separate native pages for the holes.
Permuted refills, further partial holes, duplicate COPY rejection and complete
file-byte checks pass with measured swap, limit hits, scans and steals, zero
OOM events, and zero remaining shmem/swap charges after file destruction. Both
native16 and native64 matrices pass with the final accounting change. This is
bounded pressure coverage; it does not explain the earlier isolated anonymous
writeback-heavy OOM recorded above.

Fault injection exercises failure of the first metadata-node allocation and
failure after an internal node has been allocated. It targets only the KUnit
worker during writeout and also fails allocator retries. The cache folio stays
resident, dirty and byte-identical, with its missing mask intact and no stored
swap-metadata records. XArray can retain an empty internal node after a failed
multi-level insertion; the test checks present records rather than requiring
its root pointer to be NULL. A normal retry of the same folio then completes
swap and disk restoration, preserving data and the mask. All six shared-swap
cases pass on both kernels. Fault injection is enabled only in the test build
configuration; the fixture turns probability back off and clears the worker failure flag.

That inspection exposed a separate cgroup accounting omission. The XArray's
normal allocation path needs XA_FLAGS_ACCOUNT; __GFP_ACCOUNT on xa_store alone
covers its retry allocation. Both shmem inode initialization paths now set the
array flag. Swap preparation also pins the backing folio's memcg under RCU and
uses its charge scope, then restores the caller's scope. A regression creates
a child-cgroup folio, reclaims it from the root scope and inspects the metadata
node's actual charge: previously NULL, now the child memcg. Swap-in erases the
record. This is not claimed as a fix for the historical writeback OOM.

The expanded userfaultfd fixture explicitly requests MISSING_SHMEM and passes
its native16+4K / native64+4K+16K matrix, including independent pagers, buffered
short writes, shared/private mappings, WP, source retries and cross-granule
remote pagers. The AArch32 probe also requests the feature. Automatic 4K and
boot-modelled no-4K/native16 or native64 fallback all pass with a native pager,
including the existing target alignment restrictions. These are emulated
capability models, not Apple A9 or Apple Silicon hardware validation.

Full ABI work remains active: UFFD MOVE, larger-than-native public granules,
remaining legacy GUP interfaces, alternative THP/KSM, broader concurrency and
performance/device/game qualification are not established by these results.

Independent anonymous subpage identities (2026-10-02)
----------------------------------------------------

Reverse-map callbacks now carry the virtual address separately from the physical
subpage offset. An optional, memcg-accounted sidecar gives each slot its own
anonymous root and logical byte position. Ordinary linear owners retain the
shared root/index representation. Slot reuse clears the old identity binding;
lookup snapshots take root references before dropping the pool lock. Migration
copies the identities before removing PTEs, including entries whose target slots
have not yet acquired mapping references. Historical roots are released in work
context because their final release can sleep, including when the last slot put
or a rebind occurs under a page-table lock.

The nonlinear reverse walk is used by referenced/aging, reclaim and migration
callbacks. NUMA address lookup and hardware-poison process collection understand
independent identities as well. A metadata allocation failure during swap-in is
reported as OOM rather than SIGBUS. This is preparation for UFFD MOVE; the MOVE
feature and ioctl remain disabled for alternative processes.

Final native16/native64 Image builds (nonlinear-rmap-v5) and KASAN/lockdep KUnit
runs pass. New tests cover distinct roots, different virtual/physical offsets,
changed native logical index, fork, migration rollback, migration to new physical
backing with byte preservation, slot reuse after VMA teardown, primary-root
lifetime, and injected allocation failures followed by successful retries. Main
suites report 115/114 passes with 16/17 explicit skips; TLB suites pass 2 each and
data suites pass 20+8 skips / 28. DEBUG_ATOMIC_SLEEP is not enabled in these
profiles. Optional page-idle/DAMON/KSM callbacks compile with the feature off and
in a subpage/no-THP profile; this is not runtime qualification of those features.
Feature-off arm64 and x86 affected objects compile with MEMORY_FAILURE enabled.

The final native16+4K memcg pressure run passes swap-in byte checks and fork/COW
isolation with zero OOM and zero remaining swap charge. Final native64 userfaultfd
regression passes native64/16K/4K modes. Its initial runner invocation supplied
the wrong success string; the same captured boot was revalidated against the
fixture's UFFD CONTRACT PASS marker and all original error/exit checks. Earlier
native64+16K pressure also completed its workload, but its strict runner rejected
a pre-userspace debug_vm_pgtable order-13 allocation warning, also seen in native64
KUnit boots. That warning has not been fixed or concealed.

These tests do not implement or qualify UFFD MOVE, nonlinear swapped ownership,
physical-device DMA, or the broader outstanding ABI restrictions. Next work must
transfer one exclusive slot without copying or moving its neighbours, preserve
swap ownership across cache eviction, and test partial progress, pins and races
before enabling the ioctl.

Nonlinear swap restoration (2026-10-02)
--------------------------------------

The independent-identity work exposed a remaining swap-in assumption: a swap
PTE's physical subpage offset was required to equal the VMA's logical byte offset.
Dedicated cached and disk-backed tests reproduced VM_FAULT_SIGBUS for a 4K slot
at physical offset 12K mapped at virtual offset 4K. Swap-in now validates that the
physical offset is inside the native page and aligned to the process granule;
the per-slot reverse-map binding handles its independent logical position.

Both final Image builds (nonlinear-swap-v1) pass the new two-case
``arm64-user4k-nonlinear-swap`` suite with a private swap disk. Each case exercises
both restore orders, unrelated anonymous roots, fork while swapped, child write
COW, parent data retention, and physical-slot MTE tags. The cold case explicitly
evicts the swap-cache folio and checks a major fault. These nonlinear cases use
4K user leaves on native16/native64 kernels; nonlinear 16K leaves on native64
still require their own coverage.

The same runners pass four real userspace disk/fork/COW rounds per ABI: native16
and 4K, or native64, 4K and 16K. The existing granule-swap PID-1 fixture now accepts
``--nonlinear`` to select the focused kernel suite while retaining those EL0
checks. No skip is accepted by this focused suite. The earlier full-swap run had
16 passing existing cases and one explicit skip: its older borrowed-mm live
fault probe excludes CnP/S1PIE/S1POE and is incompatible with the current max-CPU
model. That skip was not counted as a pass or used to weaken the focused suite.
Final ordinary KUnit runs pass on both Images, with the disk-only suites explicitly
skipped when no swap disk is attached. The initial before-test fixture omitted
the folio lock for set_exclusive; the corrected before-v2 log is the SIGBUS
reproduction, while before-v1 is only a failed fixture setup.

This closes cached/cold nonlinear restoration for the tested 4K cases. UFFD MOVE
still needs an atomic ownership-transfer operation, present/swap/zero/hole paths,
per-slot pin validation and partial-progress/race coverage before advertisement.
The native default and the existing AArch32 selection/fallback policy are unchanged.

Userfaultfd MOVE implementation and bounded validation (2026-10-02)
-----------------------------------------------------------------

MOVE now uses a two-phase per-slot ownership transaction for smaller-than-native
anonymous mappings. It allocates the independent mapping metadata and any old
root retirement record before changing PTEs. Both page-table locks and the folio
lock stabilize the operation. After clearing the source PTE and invalidating its
TLB entry, commit checks the slot's map count, exclusivity and pin count again.
A rejected commit restores the source PTE. Success changes only the slot's
logical root/position and installs the same physical bytes at the destination.
Same-mm RSS and map counts do not change. Different droppable policies are
rejected. Historical roots retain the deferred release needed for sleeping final
puts. The native batched path retains its existing geometry.

The swap path checks the physical slot's swap count, preserves the encoded byte
offset and updates surviving cached-slot metadata when present. When no slot
metadata survives, the subsequent swap fault binds the independently positioned
slot using the preceding nonlinear swap fix. Zero pages, source holes and loop
alignment use the mm granule. A successful transfer also clears an earlier retry
error before accounting progress. MOVE is advertised in both the API feature
mask and the registered range's ioctl mask; larger-than-native MOVE remains
gated. The initial candidate omitted the range mask, which the first userspace
run detected before attempting a transfer; the second candidate corrected it.

``uffd-move-init`` is a disposable PID-1 fixture. On native16 it tests native16
and 4K; on native64 it tests native64, 4K and 16K. It checks physical address
identity, divergent virtual/physical offsets, neighbouring PFNs and bytes,
source faults after cross-CPU TLB invalidation, source PIN rejection, neighbour
PIN acceptance, retry after unpin, collisions, zero pages, allowed/disallowed
holes, partial progress, fork-sharing rejection, write-fault exclusivity recovery,
and transfer of the same swapped PTE followed by data validation at its new
offset. Each ABI also completes 256 back-and-forth moves on CPU0 while CPU1
repeatedly registers/unregisters io_uring buffers. The physical address and all
bytes remain unchanged. Smaller-granule modes inject failure of the initial
sidecar allocation and old-root retirement allocation; each failure preserves
the source and empty destination, and each retry succeeds.

These tests passed on both candidate Images before the temporary build directory
was lost, including KASAN/lockdep checks. The source was recovered from the
retained baseline and edit scripts, mirrored with the original Git index intact,
and rebuilt with identical configurations. The source and output directories
are now persistent under /home/dj/Work. The recovered AArch32 test executable is
byte-identical to the earlier passing one. The AArch32 probe negotiates MOVE and
checks 32-bit range validation, neighbour data and page-size restrictions. Auto4K
passed on both native kernels; on the recovered Images, capability-masked
no4K/native16 and no4K/native64 modes also pass. This is a hardware capability
model, not an Apple A9 hardware result.

Feature-off arm64/x86 userfaultfd objects and the selftest build passed before
recovery. The corrected subpage/no-THP profile compiles the ownership and
userfaultfd objects with USERFAULTFD enabled; its first attempted compile had
incorrectly forced these objects while that configuration option was disabled.

This is a functional experimental MOVE implementation with bounded tests, not a
claim of full ABI completion or hardware/game qualification. Dedicated coverage
of a swapped MOVE with a surviving GET-held slot, movement while migration runs,
larger-than-native public granules, remaining legacy GUP consumers, alternative
THP/KSM and the other previously recorded limits remains outstanding. The
historical anonymous writeback-pressure OOM remains unexplained.

Recovered MOVE Images also passed the complete ``uffd-move-v3.cpio`` contract,
including the pin race and both injected allocation failures, on all listed
native/alternative ABIs. Ordinary KUnit reports 115/114 main-suite passes with
16/17 explicit skips; both TLB suites pass 2, and data suites pass 20+8 skips/28.
The pre-existing full userfaultfd contract passes on both recovered Images.
Artifacts, source, configurations and command/result records are frozen under
``/home/dj/Work/user4k-vm-state/uffd-move-v2``. These results supersede the earlier
notes which described MOVE as disabled, while preserving the qualification
limits above.

Swapped MOVE after fork and retained references (2026-10-02)
----------------------------------------------------------

The smaller-than-native MOVE path now accepts a sole remaining swap reference
when an earlier fork cleared the swap PTE's exclusive marker. The native path
keeps its existing marker check. Smaller leaves instead verify the per-slot
swap count under both PTE locks; cached metadata must additionally have no
present mappings, no pins and no migration in progress. The source PTE is
transferred without setting its exclusive marker, so a later fault still makes
the normal reuse/COW decision. Multiple swap references continue to return
EBUSY without changing the source or destination.

A dedicated KUnit case retains a GET reference through swap-out and MOVE,
changes the virtual offset and anonymous root, and checks the original physical
slot and every neighbour. It covers both an exclusive swap PTE and a PTE whose
forked child has exited. In the latter case, a destination write copies the slot
and leaves the retained GET data unchanged. In the former, existing exclusivity
allows in-place writes; GET alone does not establish a snapshot. Tests pass for
4K-on-16K, 4K-on-64K and 16K-on-64K. The pre-fix v2 fixture reproduced EBUSY after
the child exited; v1 had an incorrect unconditional COW expectation and does not
establish a production failure.

A second case runs a MOVE worker while the test migrates the native folio
containing that slot and its unmoved neighbours. The final v7 disk-backed runs
completed 62, 41 and 55 migrations respectively, with 22, 27 and 15 MOVE calls
succeeding; data and source holes remained correct. The worker deliberately
leaves 1--2 ms gaps between successful moves to exercise completed migrations
as well as contention. This is test scheduling only. An earlier continuous
worker allowed every migration attempt to lose to temporary folio references
in one configuration; that is not evidence of data loss or a production
performance measurement. Deferred LRU additions are drained before each test
isolation attempt.

Evidence: ``user4k-vm-state/move-retained-v1/`` contains source, Images, configs,
logs and the reproduction history. These are bounded VM tests, not hardware
DMA, game performance or full ABI qualification. Remaining legacy page-array
GUP callers, larger-than-native public granules, alternative THP/KSM and the
previously recorded pressure and device limits remain outstanding.

User-event tracing with alternative page sizes (2026-10-02)
---------------------------------------------------------

The user_events enable-bitmap writer now uses a typed PIN for alternative mms.
The new pin_user_fragment_vma_remote() helper retains the caller's mmap lock
and reports the physical byte offset and exact ownership. It supports NOFAULT
so the tracing code keeps its existing asynchronous fault-repair mechanism.
NOWAIT and LONGTERM are rejected by this lock-preserving interface. Ordinary
native mms retain the existing page-array pin path.

Before this conversion, registration failed with ENOMEM for smaller-granule
processes on both kernels. The page-array GUP path rejected the alternative mm;
the enabler creation path eventually returned NULL, which registration reports
as ENOMEM. Native registration passed the same fixture.

The fixed Images pass private and shared bitmap updates, 32-bit and 64-bit
enable words at leaf ends, relocated backing with different virtual/physical
subpage offsets, adjacent-byte preservation, 32 enable/disable transitions in
forked processes, and missing-PTE fault repair after MADV_DONTNEED. Actual event
payloads are read back from tracefs. The matrix includes native16+4K and
native64+4K+16K processes. This does not establish new AArch32 tracing support.

KUnit validates the new helper's retained mmap lock, missing-page NOFAULT error,
unsupported flag rejection, partial-record boundary, returned VMA, PIN ownership
and successful NOFAULT retry with present backing. The full existing KUnit
matrix also passes. Feature-off native and no-THP GUP/tracing objects compile.

Evidence is under user4k-vm-state/user-events-v1/. Initial v2 trace-content
checks mistakenly expected decimal-only formatting; captured records already
contained the correct payload in hexadecimal with decimal in parentheses. The
v3 parser handles this representation; v4 is the formatted, Makefile-built
fixture. That test-format correction is not another kernel fix. USER_EVENTS
is enabled in the current VM build configs to execute this test.

At the user_events milestone, uprobes still used page-array GUP and native
PAGE_MASK/offset assumptions. The following milestone supersedes that limitation. Vhost logging is covered by a later milestone below;
various device pinning paths still need integration. The broader ABI limits in
the previous sections remain open.

Uprobes, return probes and granule-aware folio walks (2026-10-02)
--------------------------------------------------------------

AArch64 uprobes and uretprobes now operate in smaller-than-native processes.
Instruction access retains the exact GET/slot ownership and supplies the
physical page offset separately from the virtual instruction address to the
verification callback. The x86 verifier is updated for that explicit offset;
native, no-THP and x86 builds pass. Reference-counter writes also use typed GET
records for alternative mms. File-offset lookup includes vm_subpage_offset and
filters native interval-tree matches to the exact VMA byte interval.

Removal checks per-slot references and pins before discarding restored text.
It compares only the user leaf against the matching file bytes, removes that
slot's rmap and mapping reference, and leaves unrelated bytes intact. Native
folio-wide behavior remains. A pipe GET keeps the code PTE resident during
removal; without that retained reference, smaller leaves can return to shared
file backing on a subsequent install/remove cycle. The XOL allocation remains
one native backing page and reserves enough virtual space to align its base
natively, preserving the architecture copy hooks' offset convention.

The first converted candidate exposed a separate generic blocker:
folio_walk_start() still used native p4d/pud/pmd offsets and could not find the
PTE that typed GUP had just obtained. Uprobes then retried indefinitely. The
walker now uses per-mm table geometry and leaf sizes, including the selected
native page within a larger leaf. A KUnit case checks normal and zero-page
walks on both sides of a PMD boundary for native, 4K and 16K-on-64K modes.
KSM's mergeable-page lookup explicitly excludes alternative mms while its
native-page comparison/replacement code remains unconverted; this is not KSM
support. Device-exclusive paths still encounter their existing GUP restriction.

Before-fix tests reproduce EOPNOTSUPP when enabling probes on aligned mappings,
and a silently missed probe/counter for mappings beginning within a native file
page. Final tests cover native16+4K and native64+4K+16K, both aligned and shifted
file mappings, exact entry/return hit counts in parent and child, short reference
counters at independently shifted file offsets, original file bytes, adjacent
mapping integrity, code restoration, retained pipe data and return to file
sharing. Occupying the normal XOL hint forces another placement and its native
alignment is checked. AArch32 instruction probing retains the architecture's
existing EOPNOTSUPP behavior; this adds no AArch32 probe instruction decoder.

The initial retained-pipe fixture assumed native text must disappear immediately
after the pipe was released. The same assertion fails on the before-fix native
kernel, so it was not a valid unconditional native expectation. The final test
still verifies native initial reclamation and retained-reference behavior, and
requires smaller-page reclamation after releasing the reference. Temporary
opt-in kernel tracing located the walker retry and was removed completely.

Evidence: user4k-vm-state/uprobes-v1/ contains before/fixed Images, configs,
source, the bounded disposable-VM fixture and captured results. Primary configs
now enable PERF_EVENTS and UPROBE_EVENTS. The final KUnit matrix passes with
117/116 main cases and 16/17 explicit skips on native16/native64. This does not
qualify every uprobe concurrency scenario, optional KSM/THP behavior, device
pinning or the overall userspace ABI. Those broader limits remain open.

Vhost dirty logging with relocated user leaves (2026-10-02)
---------------------------------------------------------

Vhost's dirty-bitmap writer now uses typed PIN records when the worker's mm
has an alternative granule. It pins one aligned atomic word and maps the
returned physical folio offset. Thus an mremap relocation does not redirect
the update to a different slot of the native page. Dirty release retains the
existing PIN ownership rules. Native processes keep their fast page-array
pin path; the vhost guest dirty-page unit remains 4K, independently of the
host process's page size.

Before the fix, the real vhost-net backend transferred packets in smaller
processes but left the dirty bitmap unset. Native controls passed. The fixed
kernel passes native16+4K and native64+4K+16K with an isolated TAP backend.
The fixture checks RX payloads, TX completions, exact receive-buffer and
used-ring dirty bits at the end of a relocated leaf, an unaligned bitmap
base, and all neighboring bytes. Private and shared mappings run both with
and without a forked child: private snapshots remain untouched while shared
updates are visible. Both owner-task and borrowed-mm kernel-thread workers
are tested. After MADV_DONTNEED, pagemap confirms the bitmap PTE is absent
before another packet cycle faults it back successfully.

Each case executes 64 packets in each direction across the present and
missing-PTE phases. There are eight cases per ABI, five ABI/kernel pairings
across the two Images. The disposable PID1 fixture is vhost-log-init; the VM
configs enable TUN, PACKET and VHOST_NET. Evidence, source and before/fixed
Images are under user4k-vm-state/vhost-log-v1/. This covers software vhost-net
dirty logging, not VFIO/device DMA, network performance or all vhost backends.
The earlier fixture version admitted unsolicited IPv6 interface packets;
its native RX length failure was resolved by disabling IPv6 in the isolated
guest before creating TAP interfaces, not by a kernel data-path change.

Deferred mlock accounting and reclaim repair (2026-10-02)
-------------------------------------------------------

A subsequent native64 KUnit run exposed an invalid exact-count assumption in
user4k_mlock_shared_test. One run started at 16 instead of 17 locks; another
started at zero and cleared the folio's mlocked flag after an alias was
unmapped. Native64 reruns sometimes passed. These original failures remain
in the vhost-log-v1 evidence and are not retroactively counted as passes.

The existing mlock/LRU implementation explicitly permits undercounting:
__mlock_folio() skips a folio temporarily off LRU, and LRU insertion initializes
an already-mlocked folio's count to zero. Remaining VM_LOCKED mappings are
supposed to restore the lock during reverse-map reclaim. Reading mlock_count
after the folio becomes evictable instead reads its overlapping LRU-link
storage; such a value is not evidence of count corruption.

The exact-count fixture now disables deferred LRU caching for that test only,
with a registered cleanup restoring it even after an assertion. Production
batching is unchanged. A separate deterministic test retains normal batching,
isolates a file folio while installing two locked aliases, drains the deferred
locks while it is off LRU, and puts it back. This reproduces the zero-count
state, including loss of the folio flag after removing one mapping, in native
and smaller-granule modes. Both folio_referenced() and try_to_unmap() must
restore the lock without removing any remaining PTE, changing its backing or
changing mapcount. Final teardown must clear the lock and all mappings.

The repair test passes native16+4K and native64+4K+16K. This establishes the
required reclaim behavior for that controlled undercount; it does not identify
the exact scheduling interleaving of the original intermittent run. Temporary
LRU diagnostics are removed from both source and final Images. Evidence is
under user4k-vm-state/mlock-count-v1/.

BPF userspace pointers with fragment ownership (2026-10-02)
---------------------------------------------------------

Task-storage BPF UPTR fields now support smaller-granule processes. The update
path pins each pointee with FOLL_LONGTERM and FOLL_WRITE through the fragment
API, retaining its physical offset and ownership. A pointee must fit within
one supplying-process page and one physical fragment; crossing either boundary
is rejected. The existing native fast pin path remains.

A private ownership pointer follows the visible map value, aligned separately
from that value. It moves together with the UPTR fields when a new storage
element is published. Element allocation and charging include the private tail;
userspace copies and verifier-visible value size still cover only the public
bytes. An alternative-page update allocates a pin bundle charged to the map's
memory cgroup. Failed updates, replacement, task exit and map destruction retire
that bundle. After the existing reader grace period, workqueue release may lock
and dirty its folios without requiring the original mm, task, map or BTF record
to remain alive. Native fields still use their existing release path.

On a 64-bit kernel with MM_SUBPAGE, each UPTR-containing value gains one private
pointer plus up to seven alignment bytes. The pin bundle is allocated only for
an alternative-mm update. BPF programs continue loading the same direct kernel
pointers from the public field offsets. No throughput or memory-pressure
performance result is claimed for this bookkeeping change.

Before conversion, actual BPF programs load successfully and native UPTR map
updates pass; smaller-mode non-null updates return EOPNOTSUPP. The candidate
passes native16+4K and native64+4K+16K using a raw sys_enter BPF program. Private
and shared relocated leaves retain correct read/write access at their ends.
The fixture checks 64 successive updates, duplicate-update rollback, partial
pin failure, overflow and cross-page rejection, read-only failure, null and
half-null fields, lookup pointer redaction and untouched adjacent bytes.
Pin acquisition/release counters are folded across CPUs before checking live
pins and their return to baseline.

A storage-owning child can access the parent's pinned data, with private COW
isolation, and exit without leaking its pins. Separately, a supplying child can
exit while the parent's BPF storage retains pointers into the retired mm; a
pipe GET verifies subsequent BPF writes to that backing. Unmapping and replacing
the original virtual address likewise leaves the pinned old bytes accessible
through the retained pipe without altering the new mapping. Closing the map,
link and program releases the last pins. Both ordinary 24-byte and packed
25-byte map values pass; the latter checks hidden-tail alignment and verifies
that lookup does not overwrite a sentinel immediately after the public value.

The disposable bpf-uptr-init fixture builds statically through the arm64
selftest Makefile. Current VM configs enable DWARF5 and DEBUG_INFO_BTF so real
BPF programs can resolve task types. Evidence is under user4k-vm-state/bpf-uptr-v1/.
The first fixture passed an entire attr union and hit EINVAL even for null
native values; using each command's populated attribute extent fixes that
fixture setup. It is not another kernel fix. Wider BPF concurrency, device DMA,
larger-than-native public granules and full ABI qualification remain separate.

File-backed larger user leaves: internal groundwork (2026-10-02)
--------------------------------------------------------------

Internal 64K user mappings on a 16K kernel can now fault from a single aligned
cache folio covering the complete leaf. The alternative-granule file mapper
retains one native reference and rmap unit per covered native page, accounts
one logical user PTE, and preserves physical byte identity across 4K, native
and coarse aliases. Native mappings retain their existing mapping path.
Undersized or physically misaligned backing is rejected before installing a
coarse PTE. This does not promote existing small cache folios or enable the
public larger-than-native exec ABI.

Private first-write faults copy the entire larger leaf before marking the
anonymous destination up to date. Both cold COW and a write to an existing
read-only file PTE are covered, including a leaf beginning in the interior of
a larger cache folio. Tests check every 4K chunk, file isolation, native-unit
reference counts and logical RSS, then require complete mapping/reference
cleanup.

A new mixed-alias test exposed a real invalidation fault: a native-page file
range could reach a larger user PTE with a range shorter than its granule,
violating the zap batch invariant. The file-range walker now rounds overlapping
larger-mm ranges outward to whole user leaves within that VMA. It retains the
ordinary API's native-page rounding for fine aliases. The separate byte-range
hole API preserves neighboring fine leaves within the affected native page.
Tests exercise both interfaces, lazy file mapping after fork, partial parent
unmap, cross-alias writes and cleanup. A partial final leaf keeps its zero tail;
a leaf starting beyond EOF faults with SIGBUS.

The fixture uses an isolated RAM-backed generic-file test inode with explicit
large folios. It is not evidence for filesystem cache promotion, disk writeback,
coarse swap/migration, special mappings or public coarse exec. Those remain
requirements before enabling larger-than-native processes. Small-granule
userspace file and userfaultfd behavior is regression-tested separately.

The first test revision incorrectly read approximate RSS counters and assumed
shared file PTEs were eagerly copied by fork. The corrected fixture sums the
counters and faults the child. A subsequent test lookup lacked its mmap lock;
that fixture warning is also corrected. The unrelated anonymous mlock-count
test now uses its existing all-CPU drain helper before exact observations,
instead of draining only the CPU on which the test resumes. This changes no
production LRU batching. Original failures remain in the evidence under
user4k-vm-state/coarse-file-v1/.

Target-order shmem cache promotion groundwork (2026-10-02)
--------------------------------------------------------

The file-cache collapse implementation now accepts an explicit target order.
Existing native PMD callers still request their original order. A new internal
``collapse_file_user_page()`` entry point can promote shmem cache backing to
one larger-than-native user page. It reuses the existing isolation, unmap,
reference checks and rollback sequence; this is not yet connected to the
ordinary fault path or public larger-granule exec selection.

A sub-PMD promotion allocates through shmem's existing inode policy lookup,
zone restrictions and GFP limiting, then charges the destination to the supplied
mm. Native PMD allocation remains unchanged. Sub-PMD folios do not increment
PMD THP counters or retract PMD page tables. The inode's transparent-huge-page
policy is unchanged: its maximum order can be zero when THP is disabled, which
is distinct from the backing required by an explicitly larger user leaf.
The new entry point currently requires THP build support and shmem; filesystem
writeback and no-THP integration remain unfinished.

An internal 64K-on-16K fixture starts with four separate dirty native folios
in the middle of a larger file and active 4K aliases. Promotion preserves every
byte, then both coarse and fine mappings reference the new physical backing.
A writable pin in the middle forces rollback, including earlier isolated
folios: every original cache PFN and the pinned modification remain intact.
After releasing the pin, promotion succeeds and includes that modification.
An already suitable folio is an idempotent success; unrelated file ranges,
cache page counts and final mapping/reference cleanup are checked.

Both main KUnit matrices pass on the final Images; the coarse test is skipped
on native64. The native16 userspace PMD collapse contract and native64 mixed
userfaultfd contract pass after the refactor. Feature-off arm64, x86 and no-THP
object builds pass. The initial new maximum-order check wrongly rejected
THP-disabled shmem and is removed; that failed run is retained in the evidence.
NUMA and zone policy use is established by code inspection and compilation,
not a new multi-node hardware test. Fault retry/error handling, sparse and
swapped promotion, concurrent mutation, failure injection and full ABI
qualification remain open. Evidence: user4k-vm-state/coarse-promote-v1/.

Automatic coarse shmem faults (2026-10-02)
---------------------------------------

Shmem faults now promote undersized backing for an internal larger user leaf,
then reacquire the cache folio and complete the ordinary read/shared-write or
private-COW path. No explicit promotion call is required by the faulting
process. The original folio is unlocked and released before promotion. This
sub-PMD shmem path can run under the fault lock because it does not retract
page tables or invoke filesystem writeback, and trylocks additional source
folios with rollback on contention. Bounded tests run this path with lockdep.
That is not proof of every reclaim or concurrent mutation interleaving.

A nonblocking fault returns RETRY with the fault lock retained and does not
allocate cache backing. Transient promotion contention returns NOPAGE, leaves
the lock with the caller and permits a subsequent refault. Each invocation
performs at most one promotion if splitting or truncation races the result.
Allocation failure maps to OOM, copy poison to HWPOISON. Unexpected retained
references return EBUSY from the promotion helper and currently SIGBUS from
the larger fault, preserving the original cache and pin ownership. Releasing
the reference permits a later successful fault. This is an explicit limitation
of the experimental larger ABI, which is still unavailable through public exec;
further retained-reference/error-policy qualification remains required.

A required user leaf populates its entire granule, including cache holes,
consistent with existing coarse aliases. Another process's missing-page
registration cannot turn this required backing preparation into optional THP
collapse. The registration-based hole-preservation rule remains for native PMD
collapse. Fine aliases subsequently observe the populated coarse page and must
not overwrite its contents as an unpopulated leaf. The targeted unit test models
the registration flag, prepares real partial-validity shmem metadata, and checks
that existing 4K data survives while the other bytes become zero and populated;
it does not deliver an actual cross-process UFFD event for an EL0 coarse process.

New internal64-on-native16 cases cover fresh and already populated shared
faults, private first-write COW, logical RSS and cache references, data at the
end of the leaf through access_remote_vm, nonblocking no-allocation behavior,
a held source-folio lock, retained writable PIN rejection and recovery, and
missing-page registrations within and adjacent to the coarse file range.
The held-lock case is a controlled unit fixture, not a concurrent stress test.
All new cases and both main KUnit matrices pass on the final Images. Native16
userspace PMD-collapse and native64 mixed-userfaultfd contracts pass; native
feature-off, no-THP and x86 affected object builds pass. The initial hole test
reproduced an incorrect adjacent-registration SIGBUS before distinguishing
required user-leaf population from optional collapse.

Evidence is under user4k-vm-state/coarse-fault-v1/. No new allocation-failure
injection, swapped promotion, coarse reclaim/migration, independent concurrent
mutation or hardware validation is claimed. Ordinary filesystem promotion,
no-THP coarse backing, special mappings and public larger-granule exec remain
unfinished. Native-default and smaller-process selection remain unchanged.

Cold shmem reclaim and promotion failures (2026-10-02)
----------------------------------------------------

A dedicated disk-backed KUnit runner now reclaims both separate native file
folios and an already-promoted 64K shmem folio on a 16K kernel. It requires
file-cache swap entries and absence from swap cache before faulting again,
then checks the major-fault result, every 4K data chunk, coarse/fine alias
physical identity, cache/swap counts and final mapping/reference cleanup.
The primary16 configuration has no THP_SWAP, so reclaim of the promoted folio
also exercises the path requiring a split before swapout.

The new test exposed a file-reclaim unit mismatch. Removing a coarse PTE
subtracted one mapping/reference even though that PTE owned four native-page
units. Three stale mappings prevented reclaim and eventually triggered the
filemap_unaccount_folio assertion during inode cleanup. File unmap now scales
rmap and folio-reference removal by the native pages per user leaf; logical
RSS remains measured in user PTEs. Batch limits now use the target mm's page
table boundary and page shift. Native/hugetlb ownership remains unchanged.
This fixes file reclaim; larger anonymous swap and migration still need their
own ownership audit and runtime tests.

The same runner tests three allocation failures around the shmem fault
callback: one-shot slab failure for promotion metadata, task-filtered rejection
of the required order-2 allocation, and an actual zero-limit memory cgroup.
For the charge case, the private test inode restricts reclaim before any cache
population, and the KUnit worker temporarily joins the zero-limit cgroup. This
makes charge failure nonblocking and avoids an OOM kill. The worker returns to
root before assertions and retry. Each case preserves the original cache PFNs,
all data, cache-page count and zero user RSS, then succeeds after removing the
failure condition. Calling the fault callback directly isolates backing
allocation from page-table allocation; it is not a complete EL0 OOM test.

Current primary VM configurations enable FAIL_PAGE_ALLOC as well as FAILSLAB.
Injection remains disabled by default and is scoped to the selected test worker;
the fixture disables injection after each case. The selftest runner refuses
ordinary host execution and uses a fresh private virtio swap disk in the VM.

The original promoted-folio run fails with the captured three-map leak and
cleanup BUG. After the fix, both cold-reclaim cases and all three failure modes
pass in the dedicated suite. Main KUnit matrices and feature-off/no-THP/x86
rmap object builds also pass. Evidence is under user4k-vm-state/coarse-swap-v1/.
These bounded results do not qualify larger anonymous swap, file migration,
concurrent pressure, ordinary filesystem promotion, public coarse exec or
hardware performance. The full mixed-page ABI goal remains in progress.

Migration of larger base user leaves (2026-10-02)
-----------------------------------------------

Migration now drops and restores one reference per native page covered by a
base user PTE. The reverse-map operations already handled that full span, but
try_to_migrate_one dropped only one reference and remove_migration_pte restored
only one. Those extra references prevented the migration reference-count check
from succeeding. Hugetlb keeps its existing reference convention; smaller
anonymous mappings retain their separate subpage migration path. Native and
smaller file PTEs have a native-page multiplier of one.

Two negative controls reproduced this failure before each correction. The
first could not migrate either a clean 128K file folio mapped through two 64K
leaves or a dirty, promoted 64K shmem folio at a nonzero file offset. After the
file correction, another control still could not migrate private or fork-shared
64K anonymous backing. Both controls exited through normal test shutdown;
rollback retained the original PFNs and data without KASAN or lockdep reports.

The final native16 tests pass six file variants and five anonymous variants.
File cases retain simultaneous 64K, native16K and shifted 4K aliases crossing
a small-page table boundary. They verify migration to different backing,
permissions, every data chunk, native-unit references/mapcounts, logical RSS,
and exact cleanup. A retained GET or typed PIN forces rejection without losing
ownership or data. Anonymous cases additionally verify exclusivity on every
native constituent and full-leaf COW after restoring fork-shared mappings.
Writable pins on the final byte of a coarse anonymous leaf prevent migration.
These are bounded in-kernel cases, not a claim of arbitrary concurrent races.

Both primary Images build without compiler diagnostics. Final main KUnit
results are native16: 127 pass, 16 skip; native64: 117 pass, 26 skip, with no
failures. Larger-than-native cases intentionally skip on native64. Existing
TLB, data and migration suites pass. Feature-disabled arm64, no-THP arm64 and
x86 migration/rmap object builds pass. The file correction also passed the
cold-shmem swap and allocation-failure runner before the anonymous extension.
Evidence, negative-control Images, final Images and logs are retained under
user4k-vm-state/coarse-migrate-v1/.

This closes the demonstrated migration ownership mismatch. Larger anonymous
swap is still separate: its swap-count acquisition, PTE replacement, release,
and fault-in paths need full-span handling. Ordinary filesystem backing
promotion, no-THP coarse backing, special mappings and public larger-granule
exec remain open. The default native ABI and the existing opt-in smaller ABI
remain unchanged; these tests do not establish full ABI or hardware qualification.

Anonymous swap for larger base user pages (2026-10-02)
----------------------------------------------------

An internal larger user PTE now owns an aligned group of native swap entries.
Swap-out acquires every constituent count atomically within the cluster;
fork duplicates that group, and unmap releases it. Reverse-map and folio
references use native-page units while RSS and swap-PTE counters retain the
process's logical page units. Native and smaller-page paths retain their
existing ownership conventions.

Anonymous lineages record an immutable minimum backing order from their mm.
Mapped anonymous backing cannot split below that order. The query validates
its anon_vma through RCU and a temporary reference, including callers that
have not locked the folio; the actual split rechecks under the anon_vma lock.
Mandatory larger base-page backing can allocate grouped swap even when optional
THP_SWAP is disabled. This does not enable general native THP swap: native-page
lineages still have minimum order zero. The non-THP swap cluster size remains
256 native entries; the allocation-order arrays can now represent the mandatory
larger allocation. Group allocation can still fail under fragmentation or limits.

Swap-in reuses suitably aligned contiguous cache backing. If the entries have
been read into separate native cache folios, it allocates and charges a complete
user-page-sized folio and copies every constituent with the architecture's
page-copy and swap-restore hooks. The PTE is revalidated before installing the
complete mapping. If a later cache folio is busy, all earlier locks are released
before waiting and retrying the group. Swapoff uses the same restoration path.
Allocation failure leaves the original swap PTE and all its counts intact.

The disposable native16 disk runner passes seven larger-anonymous variants:
warm exclusive reuse, cold real reclaim and readback, fork before cold reclaim,
fork after swap-out followed by swapped-child exit, cold swapoff, injected
contiguous-allocation failure and recovery, and a write fault on warm shared
cache backing. Tests check every 4K data chunk, per-entry counts, logical RSS,
minimum-order split rejection, full-leaf COW and parent isolation, and exact
folio references after MM teardown. The three existing coarse-shmem cases also
pass, including their allocation/charge failure modes. Console and debugfs
copies of the results represent one execution, not independent repetitions.

The first candidate's test queried a fully freed entry with swp_swapcount,
whose caller must retain valid slot ownership. It crashed in that assertion;
it does not establish a production swap failure. The test now uses the safe
freed-slot query. The next candidate returned for refault when a later cache
piece was still under I/O; the bounded direct-fault test required a completed
mapping. Waiting with all earlier locks released resolved that test failure.
Final primary Image builds have no compiler diagnostics. Main KUnit runs pass
on native16 and native64; feature-off arm64, no-THP arm64 and x86 object builds
pass. Existing EL0 swap regressions also pass for native/4K on native16 and
native/4K/16K on native64, including real swapped-PTE evidence, disk readback
and fork/COW over four 8MiB rounds per ABI. Evidence is retained under
user4k-vm-state/coarse-anon-swap-v1/.

These bounded results supersede the earlier unconverted larger-anonymous-swap
limitation. They do not qualify exhausted or fragmented swap, concurrent
fault/swapoff races, coarse swap-charge failure, tagged/poisoned backing, or
no-THP runtime behavior. Public larger-page exec, ordinary filesystem promotion,
special mappings and the broader ABI qualification remain unfinished. The
native default and existing opt-in smaller process ABIs remain unchanged.

Larger-page swap limits and overlapping faults (2026-10-02)
---------------------------------------------------------

A fragmentation test found that a complete free, aligned user-page-sized group
could still be inaccessible because its cluster belonged to another allocation
order. Optional THPs can split on that failure; mandatory larger base pages
cannot. A new fallback scans for genuinely free aligned groups after the normal
allocator fails. It pins the available devices in priority order, scans outside
the per-CPU and device allocation spinlocks, and can reschedule between clusters.
A successful allocation changes the cluster's preferred allocation order and
moves its list membership while retaining all existing slot owners. Native and
optional-THP allocations retain their previous allocation paths. The fallback
skips contended clusters and does not reclaim cache entries merely to reclassify
a cluster; it can still fail when no suitable free group is available.

The negative control exhausted a private 128MiB swap device, then freed all but
one native slot per aligned 64K group. The live 64K mapping retained its backing
and bytes through failed allocation and real reclaim attempts. Freeing a complete
aligned group near the device's end still returned ENOMEM before the fallback.
The final test allocates exactly that group, verifies the remaining native slot
counts, and completes a cold disk round-trip with exact final folio references.
Synthetic slot reservations exercise the real allocator and are released on test
cleanup; they are not an application-level whole-system pressure workload.

A separate cgroup test sets memory.swap.max to zero while allowing memory
allocation. It checks the folio's actual cgroup, observes real swap-limit events,
and verifies that failed swap charging and repeated reclaim preserve the mapping,
64K data, page order and logical accounting without leaking swap charge. Lifting
the limit permits swap-out, cold readback and cleanup. This covers swap-out charge
rejection; it does not establish coarse swap-in memory-charge failure behavior.

Eight controlled overlap rounds use two CPU-bound kthreads borrowing separate
64K mms and performing actual copy_from_user/copy_to_user operations in the VM.
They overlap a third worker's swapoff. Holding a cache folio blocks completion;
the test observes swapoff stop accepting allocations and all three workers remain
incomplete before releasing that lock. Four rounds retain a warm full-size folio;
four hold the last native piece of cold, fragmented cache backing. Each round
checks complete bytes, private COW writes, accounting, cleanup and swap reactivation.
These exercise arm64 translation and uaccess in QEMU, not Apple hardware, and
cover the stated synchronization scenario rather than all possible interleavings.

The expanded disposable suite passes all seven cases on the final native16
Image. Main KUnit suites pass on native16/native64, the native16 native/4K EL0
swap regression passes, and feature-off arm64, no-THP arm64 and x86 swapfile object
builds pass without compiler diagnostics. Evidence, the failing pre-fix Image and
final Images are under user4k-vm-state/coarse-swap-limits-v1/.

A further inspected limit remains: larger swap allocations are rejected unless
the swap device has SWP_BLKDEV. Regular swap files do not receive that flag.
The fallback preserves this restriction. File extent layout and swap I/O must be
handled before lifting it. Multiple-device fallback, broader pressure/races,
coarse swap-in charge failures, tagged/poisoned memory, no-THP runtime, special
mappings and public larger-page exec remain unqualified or unfinished. These
results do not change the native default or qualify the complete process ABI.

Larger base pages on fragmented swap files (2026-10-02)
------------------------------------------------------

Mandatory larger anonymous pages can now allocate grouped swap entries in
block-backed regular files. Optional THP allocation retains its existing policy;
filesystems using swap_rw retain their existing larger-allocation restriction.
The per-CPU fast path checks the same eligibility as slow allocation, so a cached
file cluster does not silently change optional THP allocation policy.

Swap I/O now uses each extent's physical sector and length. A folio crossing
extent boundaries gets one bio per physical run, chained to the original bio.
The original folio completion runs only after every child completes; synchronous
submission uses bio_await with the same splitting function. A separate clone
bioset is prepared at swapon, avoiding a second allocation from the unsubmitted
parent's reserve. This reserve is initialized for block-backed regular files on
all configurations: existing native large-folio swap-in can also require extent
mapping. Extent metadata remains immutable while swap entries are owned.

A disposable ext4 fixture writes a 32MiB file then collapses one native page out
of each four-page interval. On native16 it presents 512 swap extents. The previous
Image rejects the larger anonymous allocations, with four failing cases and
three passing shmem cases. The fixed Image passes all eight cases, including a
new full-64K read/write check in asynchronous and synchronous modes. That check
verifies a physical discontinuity after the first native page, distinct data in
every 4K chunk, complete mapping teardown and exact final references. The prior
cold native-piece readback, cgroup limits, exhaustion/fragmentation, fork/COW and
overlapping fault/swapoff tests also pass on this file. The block-device variant
passes all eight cases as well.

Real EL0 swap-file tests pass four 8MiB pageout/readback/fork-COW rounds per ABI:
native16 and 4K on the native16 kernel, and native64, 4K and 16K on native64.
The final helpers swap off and unmount before powerdown; read-only e2fsck checks
of all three resulting ext4 images are clean. Earlier fixtures powered down
while mounted, so their non-replaying fsck output is not a filesystem-integrity
result. Both primary Image builds, main KUnit suites, and feature-off arm64,
no-THP arm64 and x86 swapfile/page_io object builds pass. Evidence is under
user4k-vm-state/coarse-swapfile-v1/.

These results supersede the blanket block-device-only limitation above. They do
not qualify network/filesystem swap_rw backends, injected partial I/O failures,
clone-reserve exhaustion, multiple swap-device priority interactions, or all
filesystem implementations. Public larger-page exec remains gated; wider ABI,
hardware, pressure and performance qualification remains unfinished. Native
process defaults and the opt-in smaller-page selection policy are unchanged.

Anonymous pressure with delayed swap I/O (2026-10-02)
----------------------------------------------------

A controlled write stall now reproduces premature memcg OOM with smaller-page
processes. Two workers each populate and verify 32MiB under a shared 48MiB
memory limit and 256MiB swap allowance. Limiting the disposable QEMU disk to
64KiB/s writes leaves almost the entire memory charge in swap writeback. The
unchanged Image fails twice for native16 plus 4K and once for native64 plus 4K.
The native16-only control waits in blk_mq_get_tag and survives; a native16 plus
4K control with classic reclaim selected explicitly also survives. Increasing
the native-only control's virtqueue size to 1024 still passes, so these results
do not establish a native-only reproduction or quantify the cause of different
block-layer queue occupancy.

MGLRU had two obstacles to the existing legacy memcg writeback-wait policy:
sort_folio moved writeback folios into a younger generation before the common
reclaim path saw them, and isolate_folio cleared their reclaim marker. The fix
allows those folios through and preserves the marker during pending writeback.
shrink_folio_list then applies its existing GFP, mapping-deadlock and repeated-
writeback checks. No batch-size reduction, queue-depth reduction, fixed delay,
or disabled-MGLRU production workaround is introduced. The affected legacy
policy also applies to cgroup v2 with CONFIG_CGROUP_WRITEBACK disabled, as in
these validation profiles; configurations using the other writeback policy
retain their existing behavior.

The first candidate changed only sorting and still failed after substantially
more scans. The final candidate also preserves the marker. Captured stacks now
show folio_wait_writeback from shrink_folio_list through MGLRU. With the write
limit held for the full 30-second interval, native16/4K, native64/4K and
native64/16K pairs complete four permuted-data and fork/COW rounds with zero OOM
or OOM-kill events. Anonymous and swap charges reach zero after exit and explicit
clean-cache reclaim. The root-cgroup observer records live charge, writeback,
scan/steal and event counters; the QMP runner records block statistics and releases
the cap at the deadline or upon a failed run's OOM to allow bounded cleanup.
These are emulated I/O conditions, not hardware measurements.

Both final primary Images and main KUnit suites pass. Feature-off arm64, no-THP
arm64 and x86 vmscan object builds pass without compiler diagnostics. Unstalled
native16-only and native16/4K pressure regressions also pass on the final Image.
The selftest's optional classic mode is diagnostic only; its performance cost
has not been measured and it is not a proposed production configuration. The
final selftest formatting build is byte-identical to the tested executable.
Evidence is under user4k-vm-state/anon-pressure-io-v1/.

This resolves the reproduced delayed-writeback OOM mechanism. The older isolated
failure's recorded counters are consistent with it, but the original log lacks
the blocked-stack evidence, so attribution of that exact historical event remains
an inference. This result does not establish all I/O-error, dead-storage,
writeback-controller, pressure or performance behavior. Broader ABI qualification,
legacy GUP/device paths and the other stated requirements remain active.

IOMMU dirty-bitmap user buffers (2026-10-02)
-------------------------------------------

The IOVA dirty-bitmap helper now pins alternative-process buffers as typed
fragments. Each descriptor retains its physical folio offset and exact byte
length, so relocated user leaves need not have matching virtual and physical
native-page offsets. One native page bounds the descriptor array; mapped IOVA
coverage derives from the sum of successfully pinned byte lengths. Writes split
at descriptor boundaries and advance to another window when necessary. Native
processes retain the native-page-array path. Both paths now dirty backing when
releasing write pins, including shared-file bitmap storage.

The new relocated-buffer test first ran against the unchanged helper: the
native16 buffer was updated, while the 4K buffer remained zero. The helper's
existing void setter does not report pin failures to its caller; the output
comparison detects that failure. The updated helper passes on native16/4K and
native64/4K/16K mms. Additional tests cover:

* A 4K bitmap exceeding the descriptor window (512 records on native16, 2048 on
  native64), complete byte/guard verification, and a backwards revisit.
* Shared file-backed bitmap pins surviving unmap and replacement at the same
  virtual address. Subsequent bitmap writes change only the retained file;
  replacement bytes stay intact, pins release, and the file remains dirty.
* Partial pinning up to an unmapped hole, no writes beyond it, successful retry
  after restoring the hole, and no modification through a read-only buffer.

These tests call the actual IOVA bitmap library while borrowing isolated test
mms in the VM. They do not exercise an IOMMU hardware dirty walk, a userspace
IOMMUFD ioctl, physical DMA, or VFIO mapping. In particular, the existing void
setter's partial-failure behavior is preserved; full ioctl error propagation
and the remaining page-array DMA consumers remain separate work.

The KUnit configuration selects IOMMUFD_DRIVER to link the library. Both final
Image builds and full suites pass, with 131 native16 and 121 native64 main-suite
passes and the existing explicit skips. Feature-off arm64, x86 and no-THP arm64
object builds pass using real IOMMUFD plus SMMUv3/Intel-IOMMU selections in their
control configurations. The first native16 link required KALLSYMS_EXTRA_PASS=1
after adding the library; subsequent builds use that linker option. This was a
build-time kallsyms convergence issue, not a runtime kernel failure. Evidence is
under user4k-vm-state/iova-bitmap-v1/. No hardware or performance claim is made.

IOMMUFD data mappings and pin ownership (2026-10-02)
--------------------------------------------------

Alternative user mappings now use exact fragment pins in IOMMUFD's data-mapping
path. Each provider and area records its indexing granule independently of
native physical-page accounting. Anonymous and file-backed user addresses retain
physical folio offsets and allocation identities in a provider-owned xarray;
device translations alone are not used as ownership records. Native user, file
and dma-buf providers retain the existing native PFN reader.

The new reader batches long-term pins, merges physically contiguous fragments
for IOMMU mapping, and reuses cached records across overlapping IOAS copies and
multiple domains. Each distinct native physical page is charged once within the
provider. Every device mapping is removed before its last fragment pin is
released. Unsupported physical/device alignment returns an error without widening
the mapping into adjacent storage. Legacy splitting still limits individual
mappings to native-page spans. Local CPU copies retain their existing fast path;
remote/domain-backed copies use the retained fragments. Native page-array access
requests for an alternative provider return EOPNOTSUPP: that interface cannot
encode arbitrary fragment offsets and ownership.

Five KUnit cases exercise the actual IOMMUFD map/copy/domain/unmap APIs with a
translation-recording mock IOMMU. Coverage includes relocated anonymous physical
addresses, contiguous mapping batches, overlapping copies, two domains, VA
replacement, remote and local CPU-copy semantics, partial pin failure, read-only
failure, partial device-map failure, stricter device alignment, legacy splitting,
the maximum IOVA boundary, shared-file offsets and dirtying, and originator exit.
The lifecycle case passes for native16/4K/internal64K mms and native64/4K/16K mms.
The internal larger-granule fixture does not remove the public larger-exec gate.

The exit test reproduced stale VFIO-compatible pin accounting: after the source
mm's users reached zero, final unpin left two native pages charged because the
accounting helper required mmget_not_zero even for a decrement. The provider's
mmgrab already retains the lock and counters. Decrements now finish using that
reference; increases still require a live mm. The before-fix test fails the pin,
locked-memory and cached-accounting counters. The final test passes for native
and 4K mms on both kernels.

The final native16 and native64 main suites report 136 and 126 passes respectively,
zero failures and the existing explicit skips. Both Image builds and IOMMUFD
object builds for feature-off arm64, no-THP arm64 and x86 pass. The KUnit selector
now links full IOMMUFD. Evidence is under user4k-vm-state/iommufd-fragments-v1/.
These are kernel API and simulated-translation tests, not physical-device DMA or
userspace ioctl qualification. Memlock-limit failure injection, wider concurrent
IOMMUFD access, typed software-access consumers, legacy VFIO type1, hardware and
performance testing remain open. No throughput result is claimed.

IOMMUFD userspace ioctl limits and dirty errors (2026-10-02)
---------------------------------------------------------

The disposable iommufd-init fixture now runs real IOMMUFD ioctls from native
and alternative EL0 processes against the kernel's generic AMDv1 mock IOMMU.
The ARM64_USER4K_IOMMUFD_TEST option selects the required test-only page-table
and mock-device configuration. It defaults off. The mock address checker uses
exact fragment offsets for alternative mms instead of native-page GUP.

For USER and MM accounting, the fixture drops CAP_IPC_LOCK and sets tight
RLIMIT_MEMLOCK limits. It verifies over-limit failure without stale charges,
overlapping IOAS copies, late domain attachment, relocated physical addresses,
replacement at the same VA, local and retained remote copies, mapped holes,
read-only backing, shared-file offsets and guard bytes. Cross-process tests
exercise both directions between native and smaller mms, retained pins after
source exit, user-wide versus per-mm limits, and successful remapping after
uncharge. Four threads perform 256 map/check/unmap cycles per accounting mode.
A fail-nth sweep injects slab, page-allocation and IOMMUFD failures until the
operation stops consuming injection sites. Every iteration checks charges and
retries successfully against both mock domains. The observed number of sites
varies with allocator state and does not represent a fixed coverage count.

A fresh alternative mapping at a shifted native-page offset can require three
native backing pages even when an earlier relocated mapping used two. The test
checks that exact charge rather than treating the extra physical backing as an
accounting leak. This is allocation geometry, not a change to the memlock policy.

Dirty-bitmap ioctl tests reproduced five false-success cases on both kernels:
an unmapped IOVA, an invalid output pointer, a non-power-of-two bitmap granule,
a read-only output buffer, and a hole after a valid bitmap prefix. The bitmap
helper now retains the first lazy pin/write error and returns it from its
iteration API. IOMMUFD returns that iteration result and the actual allocation
error, and requires a power-of-two bitmap granule. The void setter remains
available to drivers; the enclosing iteration reports failures. Prefix writes
are not undone. NO_CLEAR reads retain dirty state for a retry after repairing
the buffer; ordinary read-and-clear errors are not transactional rollback.

The final complete ioctl contract passes on native16/4K and native64/4K/16K.
It also checks exact dirty bits across a user-leaf boundary, bitmap guard bytes,
NO_CLEAR retries, successful clearing and the expected error codes. Final main
KUnit suites still report 136 native16 and 126 native64 passes, zero failures,
and the existing skips. Feature-off arm64, no-THP arm64 and x86 IOMMUFD object
builds pass. Evidence is under user4k-vm-state/iommufd-ioctl-v1/.
These are real ioctl tests using a mock IOMMU; physical DMA, hardware queues,
legacy page-array consumers and the broader ABI requirements remain open.

IOMMUFD hardware queue physical byte spans (2026-10-02)
-----------------------------------------------------

The vIOMMU hardware-queue path now pins the exact physical byte span requested
by its ioctl. An internal byte-span access has byte IOVA alignment, validates
physical contiguity inside each provider and across mapping boundaries, and
retains access interval references until queue destruction. It obtains the
physical starting offset from the retained pin records. Alternative user leaves
retain their typed allocation identities; native providers retain the existing
page supplier and pin accounting. No adjacent IOVA mapping is required merely
to round a small queue up to the native kernel page size.

The access increments each affected area's unmap lock and rolls back acquired
accesses if any later area is missing or physically discontinuous. The queue's
driver receives the exact first physical byte and retains responsibility for
its hardware-specific address and size constraints. Queue destruction stops
hardware through the existing driver destroy callback before releasing the
exact access range. The ordinary native-page-array API is still unsupported
for arbitrary alternative providers; this change converts the physical-span
consumer rather than substituting a page pointer for a fragment.

The userspace fixture exercises the actual IOMMU_HW_QUEUE_ALLOC path and the
existing mock vIOMMU queue callback, which compares its received physical address
against the parent IOMMU translation. Native queues passed the before Image,
while queues using alternative providers failed either attachment alignment or
native page-array pinning. The updated path passes native16/4K and
native64/4K/16K, for USER and MM accounting. Tests cover:

* A relocated leaf mapped by itself, with an unaligned queue strictly inside it.
* Missing predecessor and duplicate queue indexes, including callback rollback.
* Overlapping queues, source VMA replacement, dependency-ordered destruction,
  and EBUSY while an access prevents IOAS unmap.
* Retained native/alternative provider memory after the originating process exits.
* A hole after an acquired first area, discontinuity across separate providers,
  and repeated physical bytes inside one provider, with no stale unmap locks.
* A physically contiguous shared-file span crossing two distinct mapped areas.
* Allocation-failure sweeps during queue creation, successful retry, and zero
  residual pins or locked-memory charges after each unmap.

Both final Image and main KUnit suites pass with the prior counts and skips.
Feature-off arm64, no-THP arm64 and x86 IOMMUFD object builds pass. Evidence is
under user4k-vm-state/iommufd-queue-v1/. These are real queue ioctls using a mock
vIOMMU. Physical-device queue execution, performance and the remaining VFIO
consumers remain unqualified.

IOMMUFD native page arrays from alternative providers (2026-10-02)
----------------------------------------------------------------

The software page-array access path now accepts alternative user providers
when their retained physical layout can be represented by native page pointers.
Each output page must start at a native physical-page boundary, with consecutive
logical fragments within the requested part of that page. Different output
pages may be scattered or aliased. Misaligned physical bases and discontinuity
inside an output page return EINVAL; no adjacent IOVA or backing bytes are
implicitly added. A caller must access only the requested byte length, including
when the final output page is partial. The external access retains its existing
native IOVA alignment and unmap callback contract.

Alternative access intervals keep the canonical typed PIN records and their
allocation identities. Page pointers do not replace that ownership. Exact
intervals share access users; overlapping intervals retain independent users.
Final access removal releases only records no longer needed by any access or
IOMMU domain. Output advancement across areas counts native output pages rather
than provider logical indices. The private hardware-queue byte-span path uses
the same access registration and retains its full-span contiguity requirement.
Native providers keep their existing supplier, accounting and batching behavior.

The real EL0 ioctl fixture checks USER/MM accounting for native16/4K and
native64/4K/16K: lazy pinning without a domain, distinct mapped areas, duplicate
and overlapping accesses, partial final pages, source VA relocation, native-page
aliases, callback-driven unmap and rollback after a missing or incompatible
second area. Typed GET records validate every returned alternative fragment's
physical address independently of its VA. Anonymous source processes of a
different granule exit while access pins remain; retained memory is readable,
new accesses reuse it, and final release refunds the dead source account.
Allocation-failure sweeps check zero residual charges and successful retry.
Three pin threads race 256 unmap/remap iterations with the existing selftest
access callback, checking successful pins, permitted transient errors and final
zero charges. The prior dirty bitmap, hardware queue and DMA ioctl tests run too.
Before Images passed native cases and rejected alternative page-array buffers
with EOPNOTSUPP. Updated Images pass the tested representable layouts.

Evidence is under user4k-vm-state/iommufd-pages-v1/. This qualifies the IOMMUFD
API used by VFIO's modern software pin path, using the existing selftest access
consumer. It does not qualify an actual VFIO device, hardware DMA, hardware queue
execution or throughput. Arbitrary fragment layouts still require consumers
that carry explicit physical offsets and lengths. Legacy VFIO type1's direct
GUP path and the wider mixed-page ABI requirements remain open.

Mixed-granule runtime without THP (2026-10-02)
--------------------------------------------

MM_SUBPAGE now selects XARRAY_MULTI independently of transparent huge pages.
A no-THP boot exposed three failed generic-file coarse-leaf tests: their
preallocated order-3 folio had eight native cache pages, but lookup at a later
index returned ENOENT. Without multi-index XArray support, insertion did not
cover the folio's native index range. Those coarse faults returned SIGBUS despite
already suitable physical backing. The missing configuration dependency is
fixed without enabling THP or changing its allocation policy. Temporary test
instrumentation was removed after capturing the allocation/insertion/lookup.

The native16 no-THP main KUnit result changes from 123 passes, three failures,
and 23 skips to 126 passes, zero failures, and 23 skips. Native64 no-THP reports
121 passes, zero failures, and 28 skips. Both have 149 compiled cases; the
THP-enabled configuration has 152. Other enabled mixed-granule suites pass
with their explicit skips. Full real IOMMUFD ioctl contracts pass native16/4K
and native64/4K/16K in USER and MM accounting modes. The shared userfaultfd
contract passes on both no-THP kernels. AArch32 auto-4K and capability-removal
fallback to native16/native64 also pass; native16 used the initial no-THP Image
and native64 the final one. This is a boot capability model, not Apple A9
hardware testing.

Native16-only, mixed16/4K, native64-only, mixed64/4K and mixed64/16K
workers pass bounded no-THP pressure with two 32 MiB anonymous mappings sharing a 48 MiB memory limit and private swap.
They verify every byte over repeated swap-in and fork COW, report nonzero
swap, limit hits, scanning and reclaim, zero OOM kills, and zero final anonymous
and swap charges. This run uses ordinary guest swap I/O, without the separate
delayed-writeback injection. The pressure fixture now accepts EINVAL from
MADV_NOHUGEPAGE only when the THP sysfs interface is absent; otherwise its
existing failure check remains. An older packaged fixture failed before any
worker data was reported and includes a hard-coded THP sysfs path; its exact
failing instruction was not captured. These early fixture failures do not
establish anonymous memory corruption.

Evidence is under user4k-vm-state/nothp-runtime-v1/. This extends execution
coverage for smaller user leaves without THP and fixes indexing of already
provided larger file-cache backing. Automatic coarse shmem promotion still
requires the existing collapse implementation, and public larger-than-native
exec selection remains gated. This is not general no-THP filesystem promotion,
physical-device qualification, performance measurement or complete ABI readiness.

Legacy VFIO accounting lifetime (2026-10-02)
------------------------------------------

Type1 retains an mm_count reference for each DMA mapping. Final pin refunds
now use that retained mm even after its last user exits; they do not attempt
to resurrect mm_users. Refund locking cannot be interrupted after the pin
has already been released. A new asynchronous charge still requires a live
source mm. DMA owner changes credit the new account before debiting the old
one, and refund the old account even when the old process has exited. A
failed new charge leaves the existing owner and counters unchanged. Charges
remain native physical-page counts, converted by the common mm accounting
helpers independently of the user translation granule.

A private KUnit suite exercises the actual type1 accounting helpers and real
mm allocation/exit. Native16 and native64 guests check native and alternative
4K/16K/64K account geometries, source exit, partial/final refunds, failed
owner credits, transfers in both directions and same-mm idempotence. Matched
controls with the previous accounting behavior reproduce stale exit and
owner-transfer charges; the fixed implementations pass. This is metadata
lifetime coverage, including internally modeled coarse geometries, rather
than public coarse exec selection or real fragment pinning.

Evidence is under user4k-vm-state/vfio-account-v1/. Type1's mapping, replay,
external native-page array and physical-device DMA paths still need their
retained fragment-provider integration. These accounting tests do not
establish legacy VFIO DMA support for an alternative-page process.

GEM/PRIME user-granule matrix (2026-10-02)
---------------------------------------

The existing VGEM real-userspace fixture now sizes its buffer for eight
native pages and selects user granules from the native kernel size. Its
previous fixed 64 KiB buffer was too short for several native64 test ranges.
Native16 runs native16/4K; native64 runs native64/4K/16K. Each ABI passes
the existing twelve checks: full/partial GEM and PRIME mappings, distinct
backing bytes, cold faults after protection changes, partial unmap and
fixed mremap, fork, bounds and descriptor/object lifetime. PRIME sharing
now checks both directions for every pair present in the guest, verifies
the exec child's advertised user page size and confirms changed bytes in
the parent mapping. The native16 buffer is 128 KiB; native64 is 512 KiB.

The latest primary Images pass 24 checks on native16 and 36 on native64,
including six directed PRIME pairs in the latter. No new kernel mapping
fix was needed for these checks. Evidence is under
user4k-vm-state/gem-granule-v1/. This extends common GEM shmem/PRIME CPU
mapping coverage using VGEM. It does not execute Asahi GPU commands or
qualify imported buffers from other exporters, hardware DMA, Vulkan/GL,
Windows games or performance.


Legacy VFIO retained fragment provider (2026-10-02)
-------------------------------------------------

Type1 now retains typed pin records for alternative-granule mappings and
native mappings whose virtual address, IOVA or length requires finer
alignment. The native-aligned default continues through its existing GUP
and batching path. Retained records identify the original pinned allocation
and byte range; IOMMU translations are no longer used to reconstruct pin
ownership on the new path. Accounting counts unique native physical pages
within each mapping provider and refunds the retained mm after source exit.

Mapping and replay merge physically contiguous byte runs, check the actual
domain minimum alignment, and unwind successful prefixes on failure. IOTLB
synchronization precedes pin release. Cached pins preserve the original DMA
bytes after source-mm exit or address replacement; missing CPU-only ranges
use temporary GET references. A typed dirty helper records writes without
releasing cached pins. External native-page arrays accept complete, aligned,
physically contiguous native spans and preserve callers' original byte
offsets. Layouts that need an explicit physical offset or cannot be represented
by one native page pointer remain rejected. Aggregate ownership across separate
fine-grained DMA mappings is qualified in the later external-span section.

The container advertises the composed minimum supported by attached domains,
with a 4K floor while the mixed-page feature is enabled. Software dirty
tracking uses a stable 4K unit, independent of domain attachment and minimum
translation size, and marks every covered unit for native-page external pins.
A native mapping's VADDR owner change to an alternative mm still requires
ownership conversion and is rejected explicitly until that path is implemented.

The private type1 KUnit harness exercises real typed pins, source mm lifetime,
canonical mapping, replay, external pins, CPU access and DMA removal against
recording IOMMU domain operations. Seven fragment cases and two accounting
cases pass on both native16K and native64K kernels. Coverage includes failed
second-domain mapping, replay rollback with retained external pins, exact fine
ranges and dirty bits, anonymous 4K pins after source exit and transfer to a
native owner, and stable dirty granularity with a 16K-only domain. The first
callback-unpin test exposed an inconsistent device-list/IOMMU mutex order;
registration, removal and notification now use a consistent order and
notification snapshots the range before releasing the IOMMU lock.

The final source also passes the broader mixed-page KUnit suites and real EL0
IOMMUFD USER/MM regressions. These tests do not establish real EL0 legacy
VFIO ioctl behavior, physical-device DMA, Apple boot, GPU execution, Windows
games or throughput. Native-to-alternative ownership conversion, aggregate
external spans and hardware qualification remain unfinished.

Matched native-GUP controls retain the new harness and corrected lock order,
but disable fragment-provider selection and restore the native PAGE_SIZE
alignment floor. Both controls fail all seven fragment cases while their two
accounting cases pass. They power down without kernel warnings. These are
matched behavior controls, not literal original-source kernel Images.

A separate native16K MODULES=y configuration builds VFIO and type1 as modules,
with the mixed-page feature enabled and the private tests disabled. Full
MODPOST succeeds; a guest loads and unloads both modules and verifies type1
registration in six checks. This qualifies linkage of the typed PIN, GET,
mark-dirty and release APIs, rather than physical device operation.


Real EL0 legacy VFIO ioctl coverage (2026-10-02)
----------------------------------------------

A separate default-off ARM64_USER4K_VFIO_IOCTL_TEST option exposes a 0600,
CAP_SYS_ADMIN-only disposable-guest misc fixture. Standard GET_INFO, MAP_DMA,
UNMAP_DMA and DIRTY_PAGES requests call the canonical type1 dispatcher with
actual userspace arguments and the calling process's real mm. Private controls
supply recording IOMMU domains, external-page consumers and bounded CPU access.
The fixture serializes its requests; this does not qualify concurrent type1
ioctls or physical IOMMU group/device attachment. Normal kernels do not expose
this test interface.

All 29 AArch64 checks pass under native16K/4K and native64K/4K/16K. They cover
argument rejection, lazy mappings without DMA charges, temporary CPU GET,
duplicate external pins with unaligned caller offsets, every 4K dirty unit
covered by a native external page, and unmap-callback cleanup. A child maps a
fine-offset shmem range and exits. Its real mm_users reaches zero while two
native physical pages remain charged and mapped. A foreign userspace process
still receives EFAULT from direct CPU access, while a kernel worker reads and
writes the retained original bytes. Failed second-domain replay leaves the
first domain and pins intact; retry succeeds without resurrecting the source
mm. Canonical dirty-bitmap output reports exactly the two touched 4K units;
final unmap clears both domains and refunds all charges.

A freestanding AArch32 probe passes real type1 ioctls, external arrays, dirty
bitmap pointer layout, child-mm exit, kernel-worker access and final cleanup.
It checks actual AT_PAGESZ and protection alignment. With 4K available it uses
4K pages; boot models removing TGran4 support still execute it with native16K
or native64K and reject a 4K-only mprotect operation. These are capability
models, not Apple A9 or other AArch32 hardware tests.

Matched native-GUP controls retain the real ioctl fixture but disable the new
provider and restore native PAGE_SIZE alignment. They reproduce rejection of
fine mappings and alternative external pins; the fixtures fail and power down
without kernel warnings. They are matched behavior controls, not original-source
kernel Images. Existing type1 KUnit and broader mixed-page regressions also
pass on the fixture-enabled kernels.

Evidence is under user4k-vm-state/vfio-ioctl-v1/. These tests qualify the actual
EL0-to-type1 provider/ABI path with supplied domains, not the VFIO core group
attachment protocol, physical DMA, Apple boot, GPU execution or game throughput.
At this checkpoint, native-to-alternative VADDR ownership conversion and
external spans crossing multiple fine DMA mappings were unfinished; the later
sections record their implementation and bounded qualification.


Native VFIO VADDR ownership conversion (2026-10-02)
-------------------------------------------------

Native-aligned mappings continue using their existing GUP and batching path.
When VADDR updates transfer such a mapping to an alternative-page mm, or
supply a fine-aligned new address, type1 now prepares retained-provider
metadata for the original native GUP pins. Known native pin provenance and
the existing native IOMMU translations identify these references; this is
not inference of an alternative allocation's slot identity. No new pin is
acquired during preparation. Metadata allocation or failed new-owner credit
frees the candidate metadata without publishing it, releasing old pins or
changing the old account. The new account is credited with the unique physical
page count before the old full charge is refunded and ownership is committed.
Same-mm conversion adjusts only the charge difference.

Physical aliases retain each original GUP reference while charging a physical
page once within the provider. Existing native external consumers may own
independent GUP references, including different physical pages after a source
VA replacement. Their identities and references remain separate from the
mapped buffer and participate in the physical-page count. Without a mapped
domain, existing external pins become the original cached buffer records.
Native reserved/PFNMAP conversion remains unsupported by this ordinary-pin
provider; the native path's existing behavior is unchanged.

A new type1 KUnit case passes mapped/lazy, physical-alias and distinct
external-page variants on both native16K and native64K kernels. It checks
metadata abort, failed credit, exited-source refunds, transfer to a 4K mm,
original buffer bytes, duplicate external identities, replay and complete
reference/account cleanup. A same-mm native fine-address variant checks
alias charge reduction without changing the process or releasing native pins.
The total type1 result is ten passes, zero failures
on both kernels, including the existing accounting and fragment cases.

Real EL0 tests fork/exec a native-page source, verify that its full aligned MAP
uses legacy GUP, and retain the mapping after that source exits. Alternative
4K/16K recipients use canonical VADDR invalidation and update ioctls. A zero
memlock limit with only effective CAP_IPC_LOCK removed forces ENOMEM; the old
owner, native pins, metadata and charge remain intact. Restoring the original
limit/capability permits retry, CPU access under the new mm's authorization,
external native-page pin reuse and final recipient refund. Native16 runs 29
native checks and 41 alternative4K checks; native64 runs 29 native checks and
41 checks for each alternative4K/16K. AArch32 auto4K remains covered by the
same fixture. Group restrictions stay in production; only a default-off,
root-only recording-domain control changes the private test group's membership.

Matched controls restore the previous native-to-alternative owner gate while
retaining the new harness. Their real VADDR tests return EOPNOTSUPP instead of
permitting credit/retry; dependent CPU operations stop while VADDR is invalid.
These are matched behavior controls, not original-source kernel Images.
Broader mixed-page KUnit regressions, feature-off arm64/x86 compilation and
module build/load/unload checks also pass. Evidence is under
user4k-vm-state/vfio-owner-v1/. This qualifies the tested provider ownership
paths with supplied domains, not concurrent physical-device attachment/DMA,
Apple GPU execution, game performance or the remaining full ABI requirements.


VFIO native-page arrays across fine DMA mappings (2026-10-02)
------------------------------------------------------------

An external native-page array may now span several adjacent fine-grained DMA
mappings. Each constituent retains a reference in its own DMA owner, with its
exact byte extent and existing per-provider physical-page accounting. Every
byte must resolve to the same native physical page at the corresponding native
offset. Missing mappings, denied permissions, discontinuous physical pages and
incompatible offsets reject the request and unwind all references acquired by
that request. Previously held references remain intact. Ordinary native-aligned
mappings keep their existing native GUP path.

Unpin walks the same constituents under the container lock. Unmapping any
constituent invokes the existing device notification contract: a consumer must
release the native-page reference, including all of its constituent references,
before that mapping can disappear. Exact extents also bound dirty bitmap writes
and determine whether a cached pin is still protected by an external consumer.
A physical page shared by independent DMA providers remains charged separately
to each provider; this does not add cross-provider charge deduplication.

The new KUnit case passes lazy/mapped spans with separate 4K/native mm owners,
source-mm exit, duplicate references, failed larger requests, reverse-order page
release, fine dirty bitmaps and constituent notification/cleanup. Four negative
variants cover a hole, permission denial, discontinuous backing and wrong native
offset. The type1 suites total eleven passes and zero failures on each native16K
and native64K kernel.

Real EL0 creates a native-page span with independent 4K MAP ioctls, pins before
source exit and reuses all cached constituents after exit. Lazy and mapped
variants check each provider's charge, fine dirty bits, duplicate references and
UNMAP of one constituent followed by full cleanup. These add sixteen assertions
to each tested A64 ABI: native16K/4K and native64K/4K/16K. The supplied domains
record translations; these results do not qualify physical device DMA, concurrent
type1 consumers, Apple boot/GPU execution or game throughput.


VFIO compat dirty-bitmap pointer decoding (2026-10-02)
----------------------------------------------------

The AArch32 probe now tests nonzero padding after the 32-bit bitmap pointer.
The prior kernel accepted the zero-padded request but rejected the same valid
pointer with a changed padding byte, for both GET_BITMAP and bitmap-returning
UNMAP. The native pointer copy interpreted that padding as pointer bits.
Separate old-kernel probes isolate each rejection on native16K/native64K.

Type1 now decodes a compat pointer at its actual width, retaining the 64-bit
bitmap scalars and ignoring pointer padding. Payload-size checks use the
architecture's compat alignment fixup, preserving aligned AArch32/x32 sizes
and accounting for the smaller IA32 layout. Native calls keep their existing
structure copy. No public UAPI structure changes.

Real AArch32 tests check a zero-padded query, a nonzero-padded query and a
nonzero-padded bitmap-returning unmap, including returned bits, callback pin
cleanup and locked-memory refund. They run with automatic4K and the modeled
no4K native16K/native64K fallback. The existing A64 type1 tests remain applicable
across all five native/alternative granule contexts. Supplied recording domains
qualify this ioctl path, not physical-device DMA or Apple GPU/game execution.
IA32/x32 compilation is covered separately; their runtime behavior is unqualified.


VFIO concurrent pin, unmap and registration qualification (2026-10-02)
---------------------------------------------------------------------

A new KUnit case uses a separately locked test consumer and three kernel
workers to force pinning, device registration and overlapping DMA unmaps.
The consumer publishes references under its own lock. An unmap callback
signals a pending registration while holding the production device-list lock,
then permits a consumer to acquire a duplicate pin, read the original native
page and release its references before callback cleanup. This exercises actual
type1 APIs and their lock ordering rather than removing serialization from the
real-ioctl fixture.

Half the cycles force a second unmap to remove the first fine DMA mapping
while its callback still runs. A second permanent consumer verifies the
original range snapshot after removal; the two unmaps together report exactly
one removed mapping. Each cycle checks page identity and bytes, per-mm locked
memory refund, the DMA tree and recording-domain translations. The final cycle
retains a seed pin and drops the last source-mm user before worker pinning,
including the overlapping unmap and callback refund after source exit.

Native16K runs 4K/native16K metadata mms and native64K runs
4K/16K/native64K metadata mms, each with lazy and mapped providers. The areas
alternate between two fine DMA mappings and a separate mapping per 4K unit.
Each of the ten models passes 1,024 cycles: 10,240 forced interleavings,
including 5,120 overlapping unmaps. The type1 suites total twelve passes and
zero failures on both KASAN/lockdep kernels, with no reported kernel warning.
These are retained test mms with real page tables and pins; concurrent EL0
ioctls, physical-device DMA, all possible concurrency schedules and hardware
performance remain unqualified. The root-only real-ioctl fixture still
serializes its own requests. No production MM or type1 behavior changes here.
