T6050 display firmware 27.0 ABI
==============================

This is an explicit firmware ABI port, not a hardware qualification. The
T6050 node must name ``apple,t6050-dcp`` and carry
``apple,firmware-compat = <27 0 0>``. The stable J714 guest DT is unchanged,
so simpledrm remains the display driver until a separate DCP test DT is used.
Do not advertise another 27.x release as compatible without comparing it.

Reference
---------

The reference is the mac17j kernelcache from UniversalMac 27.0 build 26A428,
SHA-256 a691760372651464138779c3201c1886a385ca656397362d8e7701ba19ebf436
of the decompressed Mach-O. The native serializers and callback dispatchers
retain symbols. The latest separate macOS capture is 27.2 build 26B5086k;
it is not the source of this 27.0 ABI table. Firmware/driver bytes and device-specific captures remain
private; they are not part of the source changes.

The saved live-27.0 ADT names DCP UUID
64863924-B56B-3E32-87FE-038677F52709. Those UUID bytes occur at offset
205332 in the decompressed 26A428 t605x DCP bundle (SHA-256
 d9384573025721074ec1a7190626a8bab36cf61f764055e8e05c7d5d98cb318f).
The 2026-09-28 live ADT independently carries the same UUID (ADT SHA-256
d3dbf5a4d89fe19dbf98e0c8db6749f8c51d1082ba7a4214688376512eceed20).
This closes the live image identity check for that boot, not presentation
qualification. The initial IOMFB version word still needs runtime checking. A manually supplied firmware-compat
property selects the ABI; it does not prove what iBoot loaded.

The transport is imported from the Neo H17P series by DjDeveloperr and the
T8142 H17G integration by Acelogic. Existing J414s Type-C/Thunderbolt routing,
VRR, and legacy boolean ``apple,iomfb-surfaces`` semantics are retained.
The existing RTKit IOP-OFF plus idle-notification implementation is retained.

Verified differences
--------------------

===================================== =========== ===========
Operation                             H17P 25G83  T605X 26A428
===================================== =========== ===========
pmu_service_matched                   A131        A136
backlight_service_matched             A132        A137
set_create_DFB                        A385        A389
vi_set_temperature_hint               A386        A390
swap_start                            A406        A406
swap_submit_main                      A408        A408
set_parameter                         A442        A450
create_default_frame_buffer           A446        A454
first_client_open                     A457        A466
setDisplayRefreshProperties           A465        A475
flush_supportsPower                   A468        A478
update_dfb(IOSurface*)                 A472        A483
setPowerState                         A473        A484
start_hardware_boot callback          D121        D120
create_default_fb_surface callback    D582        D586
swap_complete_ap_gated callback       D590        D594
===================================== =========== ===========

The 26A428 UPPipeAP_H17GSCD table uses A027 for setup_video_limits, A033 for
update_notify_clients, A035 for apt_supported, A036 for get_dfb_info, and A037
for get_dfb_compression_info. These also differ from the 25G76 H17G table.

At 0xfffffe000ab7fcec the A408 serializer copies 0x6e0 bytes from
IOMFBSwapRec. It emits four 0x22c-byte surfaces starting at 0x6e0, IOVAs at
0xf90, and the trailing null flags at 0xfea. The request is 0xff4 bytes and
the reply is 0x0c. The old 26.x request was 0xe9c bytes and required a
two-byte tail relocation; applying that relocation to 27.0 corrupts its
already aligned IOVAs. The 27.0 profile does not perform it.

A408 and A483 both call ``SurfaceContents::from`` at 0xfffffe000abb6a64.
It writes tearing at byte 1, premultiplied at byte 2, plane count at 3 and
pixel format at 0xb. Thus the Neo per-surface two-byte relocation must also
be skipped for 27.0. These offsets, allocation size at 0x29, and the three
pointer-presence fields at 0x51/0x149/0x1ed have compile-time assertions.
``hidePointers`` writes 1 to a non-NULL metadata pointer; these are presence
markers, not copied host pointers.

The native swap_submit_with_tags routine confirms swap ID at 0x98, source
rectangles at 0xac, destination rectangles at 0x10c, and enabled surfaces at
0x14c. The D594 deserializer requires 0x730 input bytes, retaining the 26.x
completion layout. All these offsets and sizes have compile-time assertions.

A479 abort_swaps serializes an eight-byte client handle and returns four
bytes. It must not inherit the older IOUserClient request/response structs.
The 27.0 callback dispatcher checks native minimum wire lengths before
calling typed handlers. Provider property replies use the matching device's
raw property bytes, rather than Neo-specific panel power tables.

Hardware integration still required
-----------------------------------

Saved J714s ADT resources, after parent-range translation:

* DCP ASC: 0x48ae00000, size 0x88000; IOP VBAR: 0x48a850000, size 0x4000.
* DCP DART: 0x48a340000, id 19, firmware SID 23, exclave SID 24.
* Display DART: 0x48a300000, id 18, scanout SID 0, PIODMA SID 16.
* Both DARTs include a secondary aperture at 0x2803f0000 and vm-base
  0x10000000000. Existing firmware mappings must survive guest attachment.
* Display reg[4] resolves to 0x286700000, size 0x114000. Native T6050
  ``getBWRMemoryOffset`` returns 0x20000 + agent * 0x4000. The live ADT's
  DISP_INT request 0x96 has agent index 0. PMGR group 0x22 selects map
  0x81 plus 0x700000; map 0x81 is ADT PMGR reg[8], 0x286000000.
  The resulting DISP_INT bandwidth scratch is 0x286720000, size 8.
* Display clock IDs are 361 and 458 in this ADT. The 27.0 driver requires
  ``apple,clock-ids`` in the same order as ``clocks`` and resolves callbacks
  against those handles. It does not reuse Neo's IDs 154 and 194.

The guest needs explicit ASC/mailbox/interrupt access, SPTM DART mediation
for these identities, and native panel properties. Do not clear or reset
firmware-owned display tables merely to make a probe succeed.

The native default-surface constructor sets the format returned by A036
(after pixel-definition normalization); A037 supplies compression mode. It
computes allocation size from callback width/height and per-plane layout,
then clamps it to the inherited PurpleGfxMem region. In particular, the
word at 0x29 is allocation size, not an opaque panel constant. For b3a8,
native compression mode 2 normalizes to w40a with 8-byte elements; other
modes use 4-byte and 1-byte planes. Compression modes 2 and 3 have distinct
tile-allocation formulae. The 27.0 driver caches A036/A037 replies from the existing boot chain and
uses D586's width/height. It requires a ``memory-region`` named
``framebuffer`` for the inherited allocation limit. The empty IOSurface
constructor initializes 1-byte elements of size 1x1 and no plane geometry;
setPixelFormat and allocate_carveout only change its format/allocation.
The driver matches that initial descriptor and returns the A483 boolean to
D586, rather than reporting unconditional success. b3a8, w40a, BGRA, w30r
and l10r are supported with compression 0, 2 or 3; other values are rejected
explicitly. Its integer-only calculation reproduces native binary32 size
rounding without using floating point in the kernel. Native arithmetic and
layout checks pass; runtime query values/presentation are not tested.

The native AP swap entry copies both flag words from its caller; the A408
serializer only copies the record. This establishes layout, not that Neo's
0x34865202/4 policy or gap bytes 2/3 are correct for this display. They remain
unqualified pending caller-side or firmware-behavior evidence.

The current SPTM DART backend uses a low-4-GiB DMA aperture with optional
bit-40 force, disables a SID during attachment, and attaches a fresh root.
DCP's inherited mappings must instead survive attachment. Separate driver
instances also request exclusive ownership of their MMIO resource, so they
cannot all claim the same synthetic doorbell page without a shared-provider
change or distinct emulated aliases. These integration requirements and
panel/provider behavior must be resolved before enabling the profile. No J714 DCP pageflip, power transition,
backlight, or external-output result is claimed by this port.

Firmware physical addresses in a guest
-------------------------------------

``reg`` describes Linux CPU resources and therefore contains guest IPAs.
The DCP coprocessor still sees host physical MMIO. An optional
``apple,firmware-physical-regs`` array supplies one u64 host base per
``disp-N`` resource, followed by the appended bandwidth scratch/doorbell
resources in driver index order. Its length must equal the resource count.
The disabled T6050 fixture supplies six entries; its last two both describe
PMC at 0x286700000. Without the property, bases default to resource start,
preserving native behavior.

Register/BW replies use these firmware addresses. A firmware map_physical
request is translated in reverse to the corresponding CPU resource before
calling dma_map_resource. The HV/IOMMU resource-DMA path must also translate
that IPA to host MMIO before forwarding to SPTM; its current RAM-only map
translation is not sufficient. This source change does not implement that
host-side operation or establish firmware DMA success.
