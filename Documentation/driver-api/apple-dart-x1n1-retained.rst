Firmware-owned streams in apple-dart-x1n1
=======================================

The optional ``apple,x1n1-retained-sids`` property names streams whose DART
roots and mappings remain owned by firmware. The host must explicitly adopt
these streams before starting Linux and advertise sptm2mmio feature bit 4.
Probe fails if the retained aperture does not match the configured aperture.
Ordinary streams keep their existing initialization and batching behavior.

Host query operations 9 and 10 return the root level and occupied DVA runs.
Linux reserves those runs, skips stream disable and root replacement, and
only removes mappings which it installed itself. A domain switch removes
the old domain's software-owned mappings before replaying the new domain;
its rollback attempts to restore the old mapping set. Firmware mappings
are never inserted in the Linux-owned mappings xarray.

An inherited run whose guest IPA is ``U64_MAX`` remains reserved but cannot
be translated for CPU access. A host physical address alone is insufficient:
the host must grant its backing through stage 2 before returning a guest IPA.
With multiple streams, inherited translation succeeds only when every stream
resolves the address identically.

The native 26A428 SPTM MAP implementation validates both the starting address
and the exclusive end as 42-bit addresses. A range ending exactly at 4 TiB
therefore fails. The last 16 KiB IOVA granule is reserved when the aperture
ends at that boundary, and map_pages rejects an invalid exclusive end.
This consumes virtual DMA address space, not physical RAM. It does not reduce
map batching, device queue depth or DMA concurrency.

J714s hardware validation covered 64 KiB coherent map/translate/unmap on
DCP19/SID23 and display18/SIDs0,16, repeated across three module loads while
the inherited framebuffer, GNOME and Wi-Fi remained active. The test did
not ask the display engine to DMA from the new buffers; actual DCP modesetting
and display sleep remain separate integration work.
