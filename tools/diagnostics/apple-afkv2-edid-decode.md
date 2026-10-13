# Offline AFKV2 EDID prefix audit

This tool decodes captured bytes only. It has no device access, never follows a
peer address, never changes command opcodes or driver validation, and cannot
establish live ownership or request correlation from a response alone.

Run from this public checkout:

```sh
python3 tools/diagnostics/decode-apple-afkv2-trace.py /path/to/private-trace.txt
python3 tools/diagnostics/test-apple-afkv2-edid-decode.py
```

The wrapper compiles `apple-afkv2-edid-decode.c` with the production
`afkv2-codec.h` and `epic/dpavserv-edid.h`, using the existing host shims. HOSTCC
selects the compiler. Input defaults to stdin; endpoint filtering defaults to
0x2a. It accepts the existing `afk_recv_header` text format with either a
32-byte or 256-byte prefix. It validates the count against the actual hex
bytes. Output is one JSON object per selected record. Keep trace input and
decoded output private; fixtures in the public tests are synthetic.

Optional `--owned-rx BASE CAPACITY --owned-tx BASE CAPACITY` supplies independently
known original request descriptors. The tool compares equality and bounds, but
still reports live-slot ownership unavailable: matching numerical ranges is
not proof of an active owned DMA allocation. These values are never mapped,
read, written or freed.

For direct binary-prefix input, compile the C file with the same include paths
as the wrapper and invoke `decoder RECORD_SIZE < prefix.bin`. At most 256 bytes
are accepted. Do not append fabricated zeros to incomplete captures.

## Public implementation comparison

The audited Linux implementation is the diagnostic candidate based on
57f9f3623299185d7b834ecb892e96ed0a7ea819, including
`drivers/gpu/drm/apple/afk.c`, `afkv2-codec.h`, `afk.h` and
`epic/dpavservep.c`. The comparison source is the public m1n1 object
65e063ceee96492e5668fb1131c3d52343c10098, `src/afk.c`, with its GPL-2.0-only OR MIT
license, and the MIT-licensed `proxyclient/m1n1/fw/afk/epic.py` standard-service
implementation. Its AFKEPV2 C code describes the T8152 transport; this is generic
public transport evidence, not qualification of the H17P HDMI EDID service.

| Layer | Inline Linux DPAV path | Owned AFKV2 OOB path |
| --- | --- | --- |
| AFK frame | 24-byte header, interface/subtype/category/token | Same header |
| Command | 8 bytes; flag bit 0 clear; value is request response capacity or response status | 8 bytes; flag bit 0 set; same value interpretation |
| Payload location | Bytes immediately after the command | 24-byte descriptor: RX/TX addresses and lengths; payload is in independently owned buffers |
| Response handling | Copy captured inline bytes after strict interface/type/tag/token/capacity checks | Require successful OOB response, no inline tail, exact original addresses and bounded lengths before using allocated buffers |
| Ownership | Local response allocation and bounded inline copy | Original outstanding command's allocated RX/TX buffers, slot/tag/refcount and timeout retention |

The public m1n1 AFKV2 sender uses preallocated owned TX/RX buffers and OOB
requests. Its response routine can also accept inline response bytes; for OOB
RX bytes it checks the returned address against its own RX buffer and capacity,
then copies from that local owned buffer. It does not establish that this H17P
EDID command accepts OOB, or that sending the same command inline is equivalent.
The Linux response paths deliberately retain separate inline and OOB matchers.
A response accepted by one shape matcher is not automatically accepted by the
other.

m1n1's response loop correlates interface/subtype/command ID and notes that
firmware may supply its own token. The Linux inline matcher additionally requires
zero token; Linux's owned OOB matcher does not use that inline token guard. The
audit tool reports the actual token and both applicable production shape checks.
It never removes a guard or claims correlation using response values as a
substitute for an independently recorded request.

The standard service header is 64 bytes: group at offset 2, command at 4,
data_len at 8 and magic 0x69706378 at 12. Only a matched group 1 command 7
standard-service payload is interpreted as an EDID reply. REPORT records are
kept distinct from command responses; the driver's report branch does not treat
an arbitrary C0 REPORT as an EDID completion.

## Padding and response interpretation

Current Linux inline EDID requests contain a 48-byte command body, with
`max_size=32776`, `data_pad=32776`, and response body capacity 32824. The service
helper allocates 32888 bytes including its 64-byte header, but sets inline
service data_len to 48 and sends only 112 service/body bytes. The AFKV2 command
adds 8 bytes and independently encodes response capacity 32888. Legacy service
calls declare data_len 32824 and transmit the reserved request area. Generic
public m1n1 standard-service calls declare the supplied data length and a
separate response length; they do not prove the EDID method's required padding.

This distinction could matter if the method uses reserved in-place output
space or checks request body length. Neither a service name nor an empty reply
establishes that ABI. Do not change padding, opcode or transport on that basis.

For a complete 144-byte successful inline C0 response, the command consumes 8
bytes after the 24-byte AFK header, leaving 112 inline bytes. If the matched
service data_len is 48, that is exactly a 64-byte service header and a 48-byte
EDID header, with no EDID bytes. The EDID max_size and used_size would be at
packet offsets 96 and 128. A 32-byte capture of that record does not contain
those fields: their values remain unknown, not zero.

A minimal valid inline EDID response needs 280 total bytes: 24 AFK + 8 command +
64 service + 48 EDID header + 8 leading bytes + 128-byte base block. A 256-byte
prefix cannot validate the complete base-block checksum. The tool still reports
captured max/used fields and declared lengths, but does not accept missing bytes
or claim a complete EDID. The production validators reject header-only, malformed
and inconsistent replies normally.

Switching only `inline_v2` would also select the legacy EDID reader and change
short-response handling. Any future OOB EDID candidate would need to preserve
strict actual-length EDID validation, original buffer ownership, slot/tag
correlation and timeout retention. No such driver candidate is included here.

Tests execute the production codec/EDID helpers with independent synthetic LE
bytes. They cover empty/header-only replies, missing capture, absent inline
payload, OOB descriptor-only input, unproven versus supplied ranges, unexpected
inline OOB tails, wrong service, nonzero status/token, reports, zero interface,
truncated 256-byte capture, bounds and trace count mismatches.
