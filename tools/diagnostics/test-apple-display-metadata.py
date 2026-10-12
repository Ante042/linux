#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual metadata parser/builder and independently decode base EDID."""
import argparse
import copy
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
base = root / 'drivers/gpu/drm/apple'
args = argparse.ArgumentParser(description=__doc__)
args.add_argument('--attributes-json', type=Path,
                  default=Path(__file__).with_name('display-attributes-fixture.json'))
args.add_argument('--timings-bin', type=Path, help='Optional captured TimingElements binary (not embedded in public fixtures)')
args.add_argument('--property-bin', type=Path, action='append', default=[],
                  help='Optional captured property to validate against delivered extent')
args = args.parse_args()
fixture = json.loads(args.attributes_json.read_text())


def block(text, marker):
    start = text.index(marker)
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


class ObjectPairs(list):
    pass


def wire(value, out):
    out += b'\0' * (-len(out) % 4)
    if isinstance(value, bool):
        out += struct.pack('<I', (11 << 24) | value)
    elif isinstance(value, int):
        out += struct.pack('<Iq', (4 << 24) | 8, value)
    elif isinstance(value, str):
        data = value.encode()
        out += struct.pack('<I', (9 << 24) | len(data)) + data
    elif isinstance(value, ObjectPairs):
        out += struct.pack('<I', (1 << 24) | len(value))
        for key, child in value:
            wire(key, out)
            wire(child, out)
    elif isinstance(value, list):
        out += struct.pack('<I', (2 << 24) | len(value))
        for child in value:
            wire(child, out)
    elif isinstance(value, dict):
        out += struct.pack('<I', (1 << 24) | len(value))
        for key, child in value.items():
            wire(key, out)
            wire(child, out)
    else:
        raise TypeError(value)


source = (base / 'parser.c').read_text()
parser = source[source.index('#define DCP_PARSE_HEADER'):source.index('int parse_epic_service_init(')]
metadata = (base / 'dcp-display-metadata.h').read_text()
metadata = block(metadata, 'struct dcp_display_metadata {') + ';\n'
header = (base / 'parser.h').read_text()
header = header[header.index('struct apple_dcp;'):header.index('int parse(')]
edid_header = (root / 'include/drm/drm_edid.h').read_text()
edid_header = edid_header[edid_header.index('#define EDID_LENGTH'):edid_header.index('/* EDID matching */')]
builder = (base / 'dcp-display-metadata.c').read_text()
builder = '\n'.join(line for line in builder.splitlines() if not line.startswith('#include'))
epoch = (base / 'dcp-metadata-epoch.h').read_text().replace('#include <linux/types.h>', '')
timing_fixture = [{
    "HorizontalAttributes": {"Total": 2200, "FrontPorch": 88, "SyncWidth": 44,
                             "Active": 1920, "PreciseSyncRate": 4423680, "SyncPolarity": 1, "PixelRepetition": 0},
    "VerticalAttributes": {"Total": 1125, "FrontPorch": 4, "SyncWidth": 5,
                           "Active": 1080, "PreciseSyncRate": 3932160, "SyncPolarity": 1, "PixelRepetition": 0},
    "ColorModes": [{"ID": 85, "Score": 1, "IsVirtual": False, "PixelEncoding": 0,
                    "Colorimetry": 10, "Depth": 10, "DynamicRange": 0, "EOTF": 0}],
    "ID": 62, "Score": 16224, "IsPreferred": True, "IsInterlaced": False, "IsVirtual": False,
}]
timing_raw = bytearray(struct.pack('<I', 0xd3))
wire(timing_fixture, timing_raw)
timing_source = 'static const unsigned char timing_blob[] = {' + ','.join(map(str,timing_raw)) + '};\n'
proof_checks = []
for path, value, count in [
    (("IsPreferred",), None, 1), (("IsPreferred",), False, 1),
    (("IsInterlaced",), None, 1), (("IsInterlaced",), True, 1),
    (("HorizontalAttributes", "PixelRepetition"), None, 1),
    (("VerticalAttributes", "PixelRepetition"), 1, 1),
    (("HorizontalAttributes", "SyncPolarity"), None, 1),
    (("VerticalAttributes", "SyncPolarity"), 2, 0),
    (("IsPreferred",), "true", 0),
]:
    altered = copy.deepcopy(timing_fixture)
    target = altered[0]
    for key in path[:-1]: target = target[key]
    if value is None: target.pop(path[-1])
    else: target[path[-1]] = value
    raw = bytearray(struct.pack('<I', 0xd3)); wire(altered, raw)
    index = len(proof_checks)
    timing_source += 'static const unsigned char proof_%u[] = {%s};\n' % (index, ','.join(map(str,raw)))
    proof_checks.append((index,count))
for field in ('IsPreferred', 'HorizontalAttributes'):
    altered = copy.deepcopy(timing_fixture)
    if field == 'IsPreferred':
        altered[0] = ObjectPairs(list(altered[0].items()) + [('IsPreferred', True)])
    else:
        value = altered[0]['HorizontalAttributes']
        altered[0]['HorizontalAttributes'] = ObjectPairs(list(value.items()) + [('SyncPolarity', 1)])
    raw = bytearray(struct.pack('<I', 0xd3)); wire(altered, raw)
    index = len(proof_checks)
    timing_source += 'static const unsigned char proof_%u[] = {%s};\n' % (index, ','.join(map(str,raw)))
    proof_checks.append((index,0))
proof_test = 'static void timing_proof(void) {\n'
for index,count in proof_checks:
    proof_test += f"{{ struct dcp_parse_ctx c; unsigned int n=99; assert(!parse(proof_{index},sizeof(proof_{index}),&c)); struct dcp_display_mode *m=enumerate_modes(&c,&n,600,330,0,false,true); assert(!IS_ERR(m) && n=={count}); if(n) assert(!m[0].metadata_timing_valid); free(m); }}\n"
proof_test += '}\n'
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
typedef u16 __le16;
typedef u16 __be16;
typedef u32 __le32;
#define __packed __attribute__((packed))
#undef static_assert
#define static_assert(c) _Static_assert(c, #c)
#define IS_ENABLED(x) 0
#define U32_MAX UINT32_MAX
#define U16_MAX UINT16_MAX
#define S64_MAX INT64_MAX
#define S64_MIN INT64_MIN
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define round_up(n, a) (((n) + (a) - 1) & ~((a) - 1))
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define GFP_KERNEL 0
#define kmalloc(n, f) malloc(n)
#define kcalloc(n, s, f) calloc(n,s)
#define kfree(p) free(p)
#define mul_u32_u32(a, b) ((u64)(u32)(a) * (u32)(b))
#define DIV_ROUND_CLOSEST_ULL(n, d) (((n) + (d) / 2) / (d))
#define DIV_ROUND_CLOSEST(n, d) (((n) + (d) / 2) / (d))
#define trace_iomfb_color_mode(...) ((void)0)
#define trace_iomfb_parse_mode_fail(...) ((void)0)
#define trace_iomfb_parse_mode_success(...) ((void)0)
#define trace_iomfb_timing_mode(...) ((void)0)
#define DRM_MODE_TYPE_DRIVER 1
#define DRM_MODE_TYPE_PREFERRED 2
#define DRM_MODE_FLAG_PHSYNC 1
#define DRM_MODE_FLAG_NHSYNC 2
#define DRM_MODE_FLAG_PVSYNC 4
#define DRM_MODE_FLAG_NVSYNC 8
#define cpu_to_le16(v) (v)
#define cpu_to_le32(v) (v)
#define cpu_to_be16(v) __builtin_bswap16(v)
struct drm_display_mode {
    unsigned int type, flags;
    int clock, vdisplay, vsync_start, vsync_end, vtotal;
    int hdisplay, hsync_start, hsync_end, htotal, width_mm, height_mm;
};
static void drm_mode_set_name(struct drm_display_mode *mode) { (void)mode; }
'''
stubs = r'''
struct drm_edid { struct edid raw; };
static const struct drm_edid *drm_edid_alloc(const void *data, size_t size)
{
    assert(size == 128);
    struct drm_edid *result = malloc(sizeof(*result));
    memcpy(&result->raw, data, size);
    return result;
}
static void drm_edid_free(const struct drm_edid *edid) { free((void *)edid); }
static bool drm_edid_valid(const struct drm_edid *edid)
{
    const u8 *raw = (const u8 *)&edid->raw;
    unsigned int sum = 0;
    for (unsigned int i = 0; i < 128; i++) sum += raw[i];
    return !(sum & 255) && !raw[126] && !memcmp(raw, "\0\xff\xff\xff\xff\xff\xff\0", 8);
}
'''
suffix = r'''
static void epochs(void)
{
    struct dcp_metadata_epoch e = {0};
    struct dcp_metadata_ticket before = dcp_metadata_begin(&e);
    assert(!dcp_metadata_matches(&e, before));
    dcp_metadata_epoch_connection(&e, 1, true);
    struct dcp_metadata_ticket current = dcp_metadata_begin(&e);
    assert(dcp_metadata_matches(&e, current));
    assert(!dcp_metadata_matches(&e, before)); /* Never retag pre-association metadata. */
    dcp_metadata_epoch_connection(&e, 1, true);
    assert(dcp_metadata_matches(&e, current));
    dcp_metadata_epoch_connection(&e, 2, false);
    assert(!dcp_metadata_matches(&e, current));
    dcp_metadata_epoch_connection(&e, 3, true);
    assert(!dcp_metadata_matches(&e, dcp_metadata_begin(&e))); /* Later attachment ineligible. */
    e = (struct dcp_metadata_epoch){0};
    dcp_metadata_epoch_connection(&e, 9, true);
    current = dcp_metadata_begin(&e);
    dcp_metadata_epoch_connection(&e, 10, true); /* A route change also invalidates. */
    assert(!dcp_metadata_matches(&e, current));
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--property")) {
        FILE *f=fopen(argv[2],"rb"); assert(f && !fseek(f,0,SEEK_END));
        long n=ftell(f); assert(n>20 && !fseek(f,0,SEEK_SET));
        unsigned char *b=malloc(n); assert(fread(b,n,1,f)==1); fclose(f);
        assert(dcp_property_complete(b,n-1,n));
        assert(dcp_property_complete(b,n-1,n-1));
        for(unsigned int cut=1;cut<=16;cut++)
            assert(!dcp_property_complete(b,n-1-cut,n-cut));
        assert(!dcp_property_complete(b,n-1,n+1));
        free(b); return 0;
    }
    assert(argc == 4 || argc == 5);
    /* A missing final string byte must not be supplied by zero allocation. */
    unsigned char incomplete[]={0xd3,0,0,0,1,0,0,0x82,1,0,0,0x89,0x42};
    assert(dcp_property_complete(incomplete,sizeof(incomplete),sizeof(incomplete)));
    assert(!dcp_property_complete(incomplete,12,13));
    assert(!dcp_property_complete(NULL,0,0));
    epochs();
    timing_proof();
    FILE *f = fopen(argv[1], "rb");
    assert(f && !fseek(f, 0, SEEK_END));
    long length = ftell(f);
    assert(length > 0 && !fseek(f, 0, SEEK_SET));
    void *data = malloc(length);
    assert(fread(data, length, 1, f) == 1);
    fclose(f);
    struct dcp_parse_ctx ctx;
    struct dcp_display_metadata metadata, unchanged;
    memset(&metadata, 0xa5, sizeof(metadata)); unchanged = metadata;
    assert(!parse(data, length, &ctx));
    int ret = parse_display_metadata(&ctx, &metadata);
    if (atoi(argv[2])) {
        assert(ret == -EINVAL && !memcmp(&metadata, &unchanged, sizeof(metadata)));
        free(data); return 0;
    }
    assert(!ret);
    assert(dcp_property_complete(data,length,length));
    size_t aligned=round_up((size_t)length,4);
    unsigned char *padded=malloc(aligned+4);
    memset(padded,0xa5,aligned+4); memcpy(padded,data,length);
    assert(dcp_property_complete(padded,aligned,aligned+1));
    assert(!dcp_property_complete(data,length-1,length));
    assert(!dcp_property_complete(padded,aligned,aligned+2));
    assert(!dcp_property_complete(padded,aligned+4,aligned+5));
    assert(!dcp_property_complete(padded,aligned-1,aligned+1));
    free(padded);
    struct dcp_parse_ctx timing_ctx;
    unsigned int count = 0;
    assert(!parse(timing_blob, sizeof(timing_blob), &timing_ctx));
    struct dcp_display_mode *timings = enumerate_modes(&timing_ctx, &count,
        metadata.width_cm * 10, metadata.height_cm * 10, 0, false, true);
    assert(!IS_ERR(timings) && count == 1 && timings[0].metadata_timing_valid);
    struct drm_display_mode mode = timings[0].mode;
    assert(mode.flags == (DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC));
    assert(mode.clock == 148500 && mode.hdisplay == 1920 && mode.vdisplay == 1080);
    free(timings);
    const struct drm_edid *edid = dcp_metadata_edid(&metadata, &mode);
    assert(!IS_ERR(edid));
    f = fopen(argv[3], "wb"); assert(f);
    assert(fwrite(&edid->raw, 128, 1, f) == 1); fclose(f);
    drm_edid_free(edid);
    mode.flags = 0;
    assert(IS_ERR(dcp_metadata_edid(&metadata, &mode)));
    mode.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
    mode.clock++;
    assert(IS_ERR(dcp_metadata_edid(&metadata, &mode)));
    free(data);
    if (argc == 5) {
        f=fopen(argv[4],"rb"); assert(f && !fseek(f,0,SEEK_END));
        length=ftell(f); assert(length>0 && !fseek(f,0,SEEK_SET));
        data=malloc(length); assert(fread(data,length,1,f)==1); fclose(f);
        assert(!parse(data,length,&timing_ctx)); count=0;
        timings=enumerate_modes(&timing_ctx,&count,600,330,0,false,true);
        assert(!IS_ERR(timings)); bool found60=false, found120=false;
        for(unsigned int i=0;i<count;i++) {
            assert(timings[i].timing_mode_id!=77 && timings[i].timing_mode_id!=78);
            if(timings[i].timing_mode_id==62) {
                assert(timings[i].color_mode_id==85 && timings[i].sdr_rgb.depth==10);
                assert(timings[i].metadata_timing_valid);
                found60=true;
            }
            if(timings[i].timing_mode_id==74) {
                assert(timings[i].color_mode_id==80 && timings[i].sdr_rgb.depth==8);
                found120=true;
            }
        }
        assert(found60 && found120); free(timings); free(data);
        puts("PASS: actual TimingElements retains 60Hz/color85, 120Hz/color80, excludes 144/180Hz unsafe modes");
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='dcp-display-metadata-') as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(prefix + metadata + header + edid_header + stubs + parser + builder + epoch + timing_source + proof_test + suffix)
    command = shlex.split(os.environ.get('CC', 'clang'))
    command += ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command + [str(tmp / 'test.c'), '-o', str(tmp / 'test')], check=True)
    cases = [(fixture, False)]
    for path, value in [
        (('MaxHorizontalImageSize',), 256), (('MaxVerticalImageSize',), '33'),
        (('Chromaticity','Red','X'), True), (('Chromaticity','Blue','Y'), 65536),
        (('Chromaticity','Green','Y'), 64000), (('DefaultWhitePoint','Gamma'), 65535),
        (('DefaultWhitePoint','Gamma'), 240000), (('ProductAttributes','ManufacturerID'), 'AC'),
        (('ProductAttributes','ManufacturerID'), 'A\0R'), (('ProductAttributes','LegacyManufacturerID'), 1),
        (('ProductAttributes','ProductName'), 'x'*14), (('ProductAttributes','ProductID'), 65536),
        (('ProductAttributes','YearOfManufacture'), 1989), (('ProductAttributes','WeekOfManufacture'), 54),
    ]:
        bad = copy.deepcopy(fixture)
        target = bad
        for key in path[:-1]: target = target[key]
        target[path[-1]] = value
        cases.append((bad, True))
    for key in ['Chromaticity', 'DefaultWhitePoint', 'ProductAttributes']:
        bad = copy.deepcopy(fixture); del bad[key]; cases.append((bad, True))
    for key in ['ProductAttributes', 'Chromaticity', 'DefaultWhitePoint']:
        second = copy.deepcopy(fixture[key])
        if key == 'ProductAttributes':
            second.pop('SerialNumber', None)
            second.pop('AlphanumericSerialNumber', None)
        cases.append((ObjectPairs(list(fixture.items()) + [(key, second)]), True))
    nested = copy.deepcopy(fixture)
    point = nested['Chromaticity']['Red']
    nested['Chromaticity']['Red'] = ObjectPairs(list(point.items()) + [('X', point['X'])])
    cases.append((nested, True))
    duplicate_product = copy.deepcopy(fixture)
    product = duplicate_product['ProductAttributes']
    duplicate_product['ProductAttributes'] = ObjectPairs(list(product.items()) + [('ProductName', product['ProductName'])])
    cases.append((duplicate_product, True))
    for i, (case, bad) in enumerate(cases):
        raw = bytearray(struct.pack('<I', 0xd3)); wire(case, raw)
        (tmp / 'input').write_bytes(raw)
        subprocess.run([str(tmp / 'test'), str(tmp / 'input'), str(int(bad)), str(tmp / 'edid')], check=True)
    if args.timings_bin:
        raw = bytearray(struct.pack('<I', 0xd3)); wire(fixture, raw)
        (tmp / 'input').write_bytes(raw)
        subprocess.run([str(tmp / 'test'), str(tmp / 'input'), '0', str(tmp / 'edid'), str(args.timings_bin)], check=True)
    for property_bin in args.property_bin:
        subprocess.run([str(tmp / 'test'), '--property', str(property_bin)], check=True)
    raw = (tmp / 'edid').read_bytes()
    assert len(raw) == 128 and sum(raw) % 256 == 0 and raw[126] == 0
    assert raw[:8] == bytes.fromhex('00ffffffffffff00')
    assert raw[8:10] == struct.pack('>H', 1138)
    assert struct.unpack_from('<H', raw, 10)[0] == fixture['ProductAttributes']['ProductID']
    assert struct.unpack_from('<I', raw, 12)[0] == fixture['ProductAttributes'].get('SerialNumber', 0)
    assert raw[21:24] == bytes([60, 33, 120])
    assert raw[38:54] == b'\x01'*16 and raw[35:38] == b'\0'*3
    assert struct.unpack_from('<H', raw, 54)[0] == 14850
    assert raw[56] + ((raw[58] >> 4) << 8) == 1920
    assert raw[57] + ((raw[58] & 15) << 8) == 280
    assert raw[59] + ((raw[61] >> 4) << 8) == 1080
    assert raw[60] + ((raw[61] & 15) << 8) == 45
    assert raw[62] == 88 and raw[63] == 44 and raw[64] == 0x45
    assert raw[71] == 0x1e
    chroma = [raw[27] * 4 + ((raw[25] >> 6) & 3),
              raw[28] * 4 + ((raw[25] >> 4) & 3),
              raw[29] * 4 + ((raw[25] >> 2) & 3), raw[30] * 4 + (raw[25] & 3),
              raw[31] * 4 + ((raw[26] >> 6) & 3),
              raw[32] * 4 + ((raw[26] >> 4) & 3),
              raw[33] * 4 + ((raw[26] >> 2) & 3), raw[34] * 4 + (raw[26] & 3)]
    assert chroma == [697,317,265,632,146,81,321,337]
    assert raw[72:77] == bytes.fromhex('000000fc00')
    assert b'QG270 S3' in raw and b'DCP metadata' in raw
    result = subprocess.run(['edid-decode', '--check', str(tmp / 'edid')], capture_output=True, text=True)
    if result.returncode:
        print(result.stdout)
        print(result.stderr)
    assert result.returncode == 0
    print(f'PASS: {len(cases)} actual schema cases, epoch transitions, exact base block and independent edid-decode')
