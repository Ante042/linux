T6050 display firmware 27.0 ABI
==============================

This profile has driven the J714s internal panel on hardware at fixed
3024x1964/120 Hz, including GNOME and its framebuffer-console handoff.
The T6050 node must name ``apple,t6050-dcp`` and carry
``apple,firmware-compat = <27 0 0>``. The stable J714 guest DT is unchanged,
so the qualified display path currently uses a separate RAM-boot DCP DT.
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
This establishes the live image identity used for the hardware tests. The initial IOMFB version word still needs runtime checking. A manually supplied firmware-compat
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

Hardware integration
--------------------

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
layout checks pass, and the resulting descriptor has been used during
successful hardware initialization and scanout.

The matching 26A428 Recovery image's IOMobileFramebuffer library resolves
the previously opaque swap fields. Its _kern_SwapBegin clears the 0x6e0-byte
record at context+0x18. _kern_SwapDebugInfo at 0x18e0c365c stores its two u64
arguments at context+0x58: record offsets 0x40/0x48 are debug data, not mode
flags. _kern_SwapSetEventWait writes per-layer u64 values at record+0x50;
_kern_SwapSetEventSignalOnGlass writes at 0x70; _kern_SwapSetEventSignal
writes per-layer values at 0x78. Corresponding shared-event port handles
are supplied separately by these setters.

The 27.0 structure names these fields and asserts their offsets. This
Linux driver supplies no IOSurface shared-event handles, so it preserves
the cleared native defaults instead of replaying Neo's captured debug
words 0x34865202/4 and event values 2/3. Legacy 26.x behavior is unchanged.
The reference library text SHA-256 is
86598dec087d1a8919da72a545fafe4e084b50944385a909c7e02723413a4343.
Executing its standalone debug setter against a private user buffer and
comparing compiled structure offsets confirms this layout; it is not a
hardware pageflip test.

The SPTM DART backend adopts the existing firmware roots through the native
INIT/POWERUP protocol. DCP firmware CPU allocations use its native
[1 TiB, 2 TiB) aperture, while scanout and PIODMA keep independent 42-bit
apertures. Guest aliases cover only the ADT shared firmware segments and
retained VRAM; private executable firmware is not mapped into Linux.

The display-subsystem ``memory-region`` must describe the CPU aperture used
by simpledrm. DCP's own ``memory-region`` separately describes the complete
retained VRAM allocation. These can be different aliases of the same host
memory; conflating them prevents native DRM from removing simpledrm.

Timestamps, brightness and display power
---------------------------------------

The native 26A428 Mach time functions use AGTCNTVCTSS (S3_4_C15_C10_6).
On this hardware it advances at 24 MHz while the Linux architectural timer
advances at 1 GHz. The 27.0 swap timestamps therefore read the Apple counter
directly; using architectural ticks stalls timestamped swaps. Hardware
measurements gave 120.00 completed flips/second after this correction.

``_kern_SwapSetBrightness`` stores a binary64 nits value at swap+0x35e and
sets the update byte at +0x354. The old packed-DAC fields occupy different
offsets and are not used on this profile. Integer encoding avoids kernel
floating point. Runtime property20 is IOMFBBrightnessLevel in16.16 nits;
property15 is overrideBacklight, unlike the older ABI. The panel's ordinary
600-nit user limit is forwarded from the ADT's backlight node. The user has
confirmed that both GNOME's slider and keyboard brightness controls change
the panel visibly. Brightness feedback arrives as powerlog-service mNits
reports on system endpoint channel3, using the compact EPIC header also
found on H17P. The retained ring captured requests40/240/140 nits and matching
40000/240000/140000 mNits reports. After enabling the compact decoder,
Linux actual_brightness read back40/240/140 for the same sequence.

D592 calls native ``resize_default_fb_surface_gated``. This applies a resize
queued by the AP; Linux handles explicit D586 geometry synchronously and
has no pending resize to apply. Its acknowledgement lets the normal power-
off call return. Three five-second DPMS cycles completed the firmware power
off/on sequence and restored successful scanout. This is display power
control, not system suspend or deep sleep. Firmware feedback returned to
the requested140nits after the latest cycle; optical off/on confirmation
was not obtained.

VRR range advertisement alone is not hardware qualification. DCP latches
adaptive-sync parameters when reselecting the mode, so changing VRR_ENABLED
must run the modeset path. The authored VRR implementation from aurora-silicon/linux@tb, commit
38c14f610, is already in this branch. On the current27.0 candidate, paced
flip intervals remain similar with VRR_ENABLED off/on; this does not yet
qualify the toggle or a fixed-versus-variable panel refresh distinction.
System sleep and external HDMI also remain under investigation.

Firmware physical addresses in a guest
-------------------------------------

``reg`` describes Linux CPU resources and therefore contains guest IPAs.
The DCP coprocessor still sees host physical MMIO. An optional
``apple,firmware-physical-regs`` array supplies one u64 host base per
``disp-N`` resource, followed by the appended bandwidth scratch/doorbell
resources in driver index order. Its length must equal the resource count.
The T6050 test fixture supplies six entries; its last two both describe
PMC at 0x286700000. Without the property, bases default to resource start,
preserving native behavior.

Register/BW replies use these firmware addresses. A firmware map_physical
request is translated in reverse to the corresponding CPU resource before
calling dma_map_resource. The HV/IOMMU resource-DMA path must preserve that distinction when forwarding
to SPTM. Successful internal scanout does not qualify arbitrary additional
MMIO ranges or external display pipelines.

AFKV2 commands from firmware
---------------------------

26A428 AFKEPCommandRemote uses an 8-byte command context followed, when
flag bit0 is set, by a 24-byte DMA descriptor. Descriptor presence does not
require both directions to use DMA: a zero txlen leaves the request inline,
and a zero rxlen leaves the response inline. DCPAV messages inside this
context have a 64-byte header, unlike the older 68-byte AP-call envelope.
The native implementations at 0xfffffe00091f6f34, 0xfffffe00091f7080,
0xfffffe00091f71e0 and 0xfffffe000a13bac0 establish these layouts.

The receive path translates each DMA page through the firmware device's
IOMMU domain, allowing physically scattered buffers and inherited shared
aliases. Replies retain the command tag; descriptor replies report the
actual response length. Invalid requests and unavailable services receive
native transport error statuses. The existing service handlers still own
DCPAV command semantics. This does not reduce the outgoing command slots
or serialize unrelated commands.

Disconnected external pipeline boot
-----------------------------------

26A428 UnifiedPipeline2::start_hardware_boot at 0xfffffe00097b1600 only
queries default-framebuffer information if A454 returns true. It next
queries configured geometry with A103. Nonzero geometry needs no A104/A105;
otherwise A453 supplies geometry and its result is forwarded to A104, then
committed by A105, including the zero geometry of an external pipeline
without a timing. Rejecting that zero result fails the D120 boot callback.
The 27.0 path follows those branches; older firmware paths are unchanged.

Native T6050 dispext clock-ids is explicitly empty. An external profile
therefore accepts an empty clock table, while missing or inconsistent clock
metadata remains an error. Its bandwidth request uses its own PMC agent:
ApplePMC::getBWRMemory matches the BIRQ argument against 24-byte bwr-agent
entries, and AppleT6050PMC::getBWRMemoryOffset returns 0x20000+(agent<<14).
On J714s, dispext0 request54 selects agent3 at PMC+0x2c000; disp0 request150
selects agent0 at PMC+0x20000. A guest must grant and describe the correct
page rather than reuse the internal display's request slot.

Standalone source-based regression checks run on the build host::

  python3 tools/testing/selftests/drivers/gpu/apple-dcp-afkv2.py
  python3 tools/testing/selftests/drivers/gpu/apple-dcp-native-boot.py

They exercise native wire forms, scatter-backed DMA, allocation and mapping
failures, response capacities, and the disconnected/native boot branches
with sanitizers. They do not emulate DCP firmware or qualify optical output.

The external 26A428 DPTX endpoint is 0x2c. Its captured announcements expose
``EPICUnit`` 0 and 1 alongside ``interface-id`` 1 and 3. These fields are
not interchangeable: the explicit unit must take precedence regardless of
dictionary order. The lower-case H17P form still uses interface-id when an
explicit EPICUnit is absent. A focused regression test covers both native
port/channel pairs, reversed property order, and that fallback::

  python3 tools/testing/selftests/drivers/gpu/apple-dcp-service-init.py

Endpoint 0x24 advertises ``dcpav-controller-epic`` and
``dcpdp-controller-epic``. Those controller interfaces must not be treated
as the older ``dcpav-service-epic`` EDID service. Their routing and service
creation protocol still needs separate integration. External IOMFB boot
and DPTX announcements alone do not establish HDMI or USB-C scanout.

On J714s hardware, the corrected 27.0 startup completes for both internal
and external0 DCP. A read-only driver-state snapshot confirms both external
DPTX ports enabled with units0/1 and initialized service cookies; the earlier
port0 initialization timeout is gone. HDMI and USB-C routing, incoming DPTX
link-training callbacks, and external scanout remain unqualified.

T8142 physical crossbar
----------------------

The T6050 ATC crossbar advertises the T8142 register interface. Its five
outputs are DPPHY and DPIN0..3. The initial driver covers the eight external
DCP source states (two ports per DCP), with even states only on DPPHY,
matching native connection validation. AUX source roles have a different
selector-to-clock-bit mapping and are not exposed by this initial path.

With ``apple,t8142-display-crossbar``, mux selection programs only the
route. While holding that selection, the consumer must configure its PHY
or tunnel clock and call ``apple_dpxbar_link_configure(mux, pclk, uhbr)``
with the actual PCLK1..4 and coding before ``apple_dpxbar_link_up()``.
There is no implicit clock or link-rate choice. A link configuration change
requires link-down first. Releasing or changing the selection invalidates
its clock configuration; reselecting the same source does not. Other
outputs retain their route and clock gates. The coding bit is shared per
PCLK, so conflicting simultaneous users are rejected before register writes.
Older crossbar generations retain their existing selection semantics.

The native T8142 path requires neither the older cycle-slip delay nor the
old register0xc00 tunable. Link-down preserves the selector and output
permission bit while disabling source and pixel-clock gates and restoring
reset gates. Native readback checks use the T8142 status register layout.
The source-based regression test exercises this sequence and output isolation::

  python3 tools/testing/selftests/drivers/gpu/apple-dpxbar-t8142.py

Control-page access also requires the ATC provider's common power gates.
On T6050, native TypeCPhy power-on enables provider clock indices0 and1;
ADT identifies these for HDMI's ATC3 as589 (ATC3_USB_AON_V) and62
(ATC3_COMMON). Reading the crossbar before these were enabled produced an
asynchronous fabric SError on hardware. Enabling them allowed all32 tested
control/status registers to be read without exceptions. The host setup
entry ``sptm_hv_guest_atc_xbar_setup`` resolves the ADT resources, enables
those two clocks and maps one control page, without touching PHY lanes or
pixel-clock clients. Physical PHY configuration and DCP consumer integration
are still separate work; a successful mux test is not external scanout proof.

On J714s, an isolated Linux test programmed and read back all36 supported
source/output selector combinations, confirmed the other register fields
and gates were unchanged, and restored the original selector registers.
The test required the crossbar source gates to be off and never enabled
PHY lanes or pixel clocks. Link-up/down clock behavior and optical output
remain unverified on hardware.
