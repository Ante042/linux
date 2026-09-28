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
This identifies the saved boot's firmware, not the current live boot. Before
enabling the candidate, capture the live DCP UUID and initial IOMFB version
word and compare them to the reference. A manually supplied firmware-compat
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
* Display reg[4] resolves to 0x286700000, size 0x114000. PMC bandwidth
  resources must be derived from the T6050 PMGR descriptions.
* Display clock IDs are 361 and 458 in this ADT. The 27.0 driver requires
  ``apple,clock-ids`` in the same order as ``clocks`` and resolves callbacks
  against those handles. It does not reuse Neo's IDs 154 and 194.

The guest needs explicit ASC/mailbox/interrupt access, SPTM DART mediation
for these identities, and native panel properties. Do not clear or reset
firmware-owned display tables merely to make a probe succeed.

The default-surface descriptor values and swap flag policy inherited from
the 26.x driver are not yet qualified on J714. Native 27.0 surface
serialization and panel/PMC property behavior need validation before this
profile is made the default. No J714 DCP pageflip, power transition,
backlight, or external-output result is claimed by this port.
