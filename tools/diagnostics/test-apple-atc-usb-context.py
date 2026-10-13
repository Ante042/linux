#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual-function masked context controls; no electrical recovery is modeled.

Optional parent-source points to an independently preserved public source snapshot.
No Git lookup, fetch, target access or hardware is performed by this test.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--out', type=Path)
parser.add_argument('--parent-source', type=Path)
parser.add_argument('--gate-parent-source', type=Path)
args = parser.parse_args()
fixture = root / 'tools/diagnostics/test-apple-atc-parking.py'
code = fixture.read_text().split('with tempfile.TemporaryDirectory')[0]
variants = {}
if args.parent_source:
    variants['parent'] = {
        'atc': (args.parent_source / 'drivers/phy/apple/atc.c').read_text(),
        'dp': (args.parent_source / 'drivers/phy/apple/atc-j700-dp.h').read_text(),
    }
if args.gate_parent_source:
    variants['gate-parent'] = {
        'atc': (args.gate_parent_source / 'drivers/phy/apple/atc.c').read_text(),
        'dp': (args.gate_parent_source / 'drivers/phy/apple/atc-j700-dp.h').read_text(),
    }
variants['candidate'] = {
    'atc': (root / 'drivers/phy/apple/atc.c').read_text(),
    'dp': (root / 'drivers/phy/apple/atc-j700-dp.h').read_text(),
}
controls=r'''
static void seed_usb(struct apple_atcphy *p, unsigned int first)
{
    orientation_initial(p, true);
    registers[0x200 / 4] = 0xee;
    registers[0xc / 4] = 0;
    /* Synthetic USB context, never a private factory table or captured register image. */
    for (unsigned int offset = 0x9000; offset < 0x15000; offset += 4)
        registers[offset / 4] = 0x5a5a1234 ^ offset;
    registers[0x8 / 4] = 0xa5000000;
    registers[0x10c / 4] = 0xa5000000;
    orient(p, first);
    assert(select_mode(p, APPLE_ATCPHY_MODE_USB3_DP) == 0);
}

static void mask_coverage(void)
{
    struct apple_atcphy p;
    memset(dp_write_masks, 0, sizeof(dp_write_masks));
    seed_usb(&p, 1);
    for (unsigned int rate = 0; rate < 4; rate++) {
        track_dp_writes = true;
        assert(j700_dp_start(&p, 1, rate, false) == 0);
        j700_dp_equalize(&p, 0, true, 0x123);
        j700_dp_equalize(&p, 0, false, 0x123);
        assert(j700_dp_stop(&p) == 0);
        track_dp_writes = false;
    }
    unsigned int entries = 0;
    for (unsigned int offset = 0; offset < sizeof(dp_write_masks); offset += 4) {
        if (!((offset >= 0x9000 && offset <= 0xdfff) || offset == 8 ||
              offset == 0xc || offset == 0x10c || offset == 0x200)) continue;
        u32 expected = 0;
        for (unsigned int i = 0; i < ARRAY_SIZE(j700_usb_fields); i++)
            if (j700_usb_fields[i].offset == offset) expected = j700_usb_fields[i].mask;
        assert(dp_write_masks[offset / 4] == expected);
        entries += !!expected;
    }
    assert(entries == 27);
    puts("PASS: 27 masks equal the actual public DP start/voltage/stop write union at every rate");
}
static void masked_roundtrip(void)
{
    const struct phy_configure_opts_dp rate = {
        .set_rate=true, .set_lanes=true, .link_rate=8100, .lanes=2,
    };
    bool failure=false;
    for (unsigned int first=0; first<2; first++) {
        struct apple_atcphy p;
        seed_usb(&p, first);
        unsigned int old_dp=!first;
#ifdef PROPOSAL
        assert(p.dp_usb_context_valid & BIT(old_dp));
        assert(p.dp_usb_context[old_dp][1] == 0);
        assert(p.dp_usb_context[old_dp][3] == (0xeU << (4 * old_dp)));
        assert(((registers[0x200 / 4] >> (4 * old_dp)) & 0xf) == 1);
#endif
        u32 saved[27], outside[27];
        for (unsigned int i=0;i<27;i++) {
            u32 offset,mask;
            j700_usb_field(i,old_dp,&offset,&mask);
            saved[i]=core_read32(&p,offset)&mask;
        }
        /* Power/receiver were saved before route idle, not at this post-route sample. */
        saved[1]=0; saved[3]=0xeU<<(4*old_dp);
        assert(j700_dp_configure(&p,&rate)==0);
        unsigned int edges_before=writes;
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
        assert(writes==edges_before); /* same mode never resets or recaptures */
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0);
        for(unsigned int i=0;i<27;i++) {
            u32 offset,mask;j700_usb_field(i,old_dp,&offset,&mask);
            u32 other_offset,other_mask; j700_usb_field(i,!old_dp,&other_offset,&other_mask);
            outside[i]=registers[offset/4]&~(mask | (i<4 ? other_mask : 0));
        }
        orient(&p,!first);
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
        unsigned int mismatch=0;
        for(unsigned int i=0;i<27;i++) {
            u32 offset,mask;j700_usb_field(i,old_dp,&offset,&mask);
            if((registers[offset/4]&mask)!=saved[i]) mismatch++;
            u32 other_offset,other_mask; j700_usb_field(i,!old_dp,&other_offset,&other_mask);
            assert((registers[offset/4]&~(mask | (i<4 ? other_mask : 0)))==outside[i]);
        }
        printf("first=%u former_dp_pma=%u masked_mismatches=%u receiver=%#x power=%#x\n",
               first,old_dp,mismatch,registers[0xc/4],registers[0x200/4]);
        failure |= mismatch!=0;
        assert(!usb2_writes&&!reset_writes);
        assert(j700_dp_configure(&p,&rate)==0);
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0);
        orient(&p,first);
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
        assert(!p.mode_dirty&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3);
    }
    fflush(stdout);assert(!failure);
}
static void restore_controls(void)
{
#ifdef PROPOSAL
    struct apple_atcphy p;
    const struct phy_configure_opts_dp rate={.set_rate=true,.set_lanes=true,.link_rate=8100,.lanes=2};
    seed_usb(&p,1);
    assert(j700_dp_configure(&p,&rate)==0);
    unsigned int valid=p.dp_usb_context_valid,dirty=p.dp_usb_context_dirty;
    u32 snapshot[2][27];memcpy(snapshot,p.dp_usb_context,sizeof(snapshot));
    stuck_ack=true;
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==-ETIMEDOUT);
    assert(p.dp_owned_pmas==1&&p.dp_usb_context_valid==valid&&p.dp_usb_context_dirty==dirty);
    assert(!memcmp(snapshot,p.dp_usb_context,sizeof(snapshot)));
    recover();
    fail_caller="j700_field_update";
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==-ETIMEDOUT);
    assert(p.dp_owned_pmas==1&&p.dp_usb_context_valid==valid);
    assert(!memcmp(snapshot,p.dp_usb_context,sizeof(snapshot)));
    recover();assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0);
    orient(&p,0);
    assert(j700_usb_capture(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
    p.dp_owned_pmas=1;
    unsigned int before=writes;
    assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_USB3_DP)==-EBUSY&&writes==before);
    p.dp_owned_pmas=0;p.dp_idle_pmas=1;
    assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_USB3_DP)==-EBUSY&&writes==before);
    p.dp_idle_pmas=0;p.pipe_state=ATCPHY_PIPEHANDLER_STATE_USB3;
    assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_USB3_DP)==-EBUSY&&writes==before);
    p.pipe_state=ATCPHY_PIPEHANDLER_STATE_DUMMY;
    fail_caller="j700_usb_field_update";
    assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_USB3_DP)==-ETIMEDOUT);
    assert(p.dp_usb_context_valid&1);
    assert(p.dp_usb_context_dirty&1);
    recover();assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
    assert(!(p.dp_usb_context_valid&1)&&!(p.dp_usb_context_dirty&1));
    seed_usb(&p,1);
    assert(j700_dp_configure(&p,&rate)==0);
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0);
    orient(&p,0);
    u32 old_context[27];memcpy(old_context,p.dp_usb_context[0],sizeof(old_context));
    fail_caller="j700_usb_field_update";
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==-ETIMEDOUT);
    assert(p.mode_dirty&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_DUMMY);
    assert((p.dp_usb_context_valid&1)&&(p.dp_usb_context_dirty&1));
    assert(!memcmp(old_context,p.dp_usb_context[0],sizeof(old_context)));
    before=writes;
    assert(j700_dp_configure(&p,&rate)==-EBUSY&&writes==before);
    recover();assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
    assert(!p.mode_dirty&&!(p.dp_usb_context_valid&1));
    seed_usb(&p,1);
    assert(j700_dp_configure(&p,&rate)==0);
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0);
    orient(&p,0);
    blocked_offset=0x9244;
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==-EIO);
    assert(p.mode_dirty&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_DUMMY);
    assert((p.dp_usb_context_valid&1)&&(p.dp_usb_context_dirty&1));
    before=writes;
    assert(j700_dp_configure(&p,&rate)==-EBUSY&&writes==before);
    blocked_offset=0;
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0&&!p.mode_dirty);
    puts("PASS: actual mux static/power restore failures keep dirty route/context, block training and retry");
    seed_usb(&p,1);
    assert(j700_dp_configure(&p,&rate)==0);
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0);
    orient(&p,0);
    registers[ACIOPHY_BIST_MAC_USBPHY_STATUS2/4]=BIT(7);
    bist_status_stuck=true; bist_faults=0;
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
    assert(bist_faults==2 && (registers[ACIOPHY_BIST_MAC_USBPHY_STATUS2/4]&BIT(7)));
    assert((registers[0x200/4]&0xf)==0xe && !(registers[0xc/4]&0x44));
    stuck_ack=true;
    before=writes;
    assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==-ETIMEDOUT && writes==before);
    puts("PASS: restoration never clears simulated BIST/ACK faults or forces readiness");
    for(unsigned int fixed=0;fixed<2;fixed++) {
        seed_usb(&p,0);p.fixed_usb2=fixed;hw.has_t8140_dp=false;
        before=writes;
        assert(j700_usb_capture(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);
        assert(j700_usb_restore(&p,APPLE_ATCPHY_MODE_USB3_DP)==0&&writes==before);
        hw.has_t8140_dp=true;
    }
    puts("PASS: park/DP-idle failure retention, immutable snapshot, live/pending-owner restore guards, power readback failure/retry, legacy exclusions");
#endif
}

static void inactive_host_gates(void)
{
#ifdef PROPOSAL
    const struct phy_configure_opts_dp rate = {
        .set_rate=true, .set_lanes=true, .link_rate=8100, .lanes=2,
    };
    unsigned int violations = 0;
    for (unsigned int backend = ATCPHY_PIPEHANDLER_STATE_USB3;
         backend <= ATCPHY_PIPEHANDLER_STATE_USB4; backend++) {
        for (unsigned int entry = 0; entry < 3; entry++) {
            struct apple_atcphy p;
            seed_usb(&p, 1);
            assert(j700_dp_configure(&p, &rate) == 0);
            assert(select_mode(&p, APPLE_ATCPHY_MODE_USB2) == 0);
            orient(&p, 0);
            p.host_active = false;
            p.pipe_state = backend;
            unsigned int before_writes = writes, before_reads = mmio_reads;
            unsigned int valid = p.dp_usb_context_valid, dirty = p.dp_usb_context_dirty;
            u32 snapshot[2][27]; memcpy(snapshot, p.dp_usb_context, sizeof(snapshot));
            int ret = entry == 0 ? j700_usb_capture(&p, APPLE_ATCPHY_MODE_USB3_DP) :
                      entry == 1 ? j700_usb_restore(&p, APPLE_ATCPHY_MODE_USB3_DP) :
                                   atcphy_configure(&p, APPLE_ATCPHY_MODE_USB3_DP);
            bool blocked = ret == -EBUSY && writes == before_writes &&
                           mmio_reads == before_reads && valid == p.dp_usb_context_valid &&
                           dirty == p.dp_usb_context_dirty &&
                           !memcmp(snapshot, p.dp_usb_context, sizeof(snapshot));
            printf("inactive-host backend=%u entry=%u ret=%d new_writes=%u new_reads=%u blocked=%u\n",
                   backend, entry, ret, writes-before_writes, mmio_reads-before_reads, blocked);
            violations += !blocked;
        }
    }
    fflush(stdout); assert(!violations);
    puts("PASS: six inactive-host/live-PIPE entry gates retain snapshots and perform zero MMIO");
#endif
}
static void failure_diagnostics(void)
{
#ifdef PROPOSAL
    struct apple_atcphy p;
    const struct phy_configure_opts_dp rate={.set_rate=true,.set_lanes=true,.link_rate=8100,.lanes=2};
    const char *stages[] = { "stage=static", "stage=power", "stage=receiver" };
    for (unsigned int stage=0; stage<3; stage++) {
        seed_usb(&p,1); assert(j700_dp_configure(&p,&rate)==0);
        assert(select_mode(&p,APPLE_ATCPHY_MODE_USB2)==0); orient(&p,0);
        diagnostic_records=0; last_diagnostic[0]=0;
        blocked_offset=stage==0 ? 0x9244 : stage==1 ? 0x200 : 0xc;
        int ret=select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP);
        assert(ret==(stage==0 ? -EIO : -ETIMEDOUT));
        assert(diagnostic_records==1 && strstr(last_diagnostic,stages[stage]));
        assert(strstr(last_diagnostic,"pma=0") && strstr(last_diagnostic,"reg="));
        assert(strstr(last_diagnostic,"mask=") && strstr(last_diagnostic,"expected="));
        assert(strstr(last_diagnostic,"observed="));
        assert(p.mode_dirty && (p.dp_usb_context_valid&1) && (p.dp_usb_context_dirty&1));
        blocked_offset=0;
    }
    puts("PASS: static/power/receiver errors preserve state and report stage/PMA/register/mask/expected/final observed");
#endif
}
int main(int argc,char **argv)
{
    if(argc>1 && !strcmp(argv[1],"inactive-host")) { inactive_host_gates(); return 0; }
    if(argc>1)return original_fixture_main(argc,argv);
    mask_coverage();masked_roundtrip();restore_controls();inactive_host_gates();failure_diagnostics();
    puts("PASS: masked restoration roundtrips; no BIST readiness or USB speed is modeled");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="apple-atc-usb-context-") as temporary:
    out = args.out or Path(temporary)
    out.mkdir(parents=True, exist_ok=True)
    for name, texts in variants.items():
        ns={'__file__':str(fixture)}
        local=code.replace("atc = (root / 'drivers/phy/apple/atc.c').read_text()", 'atc = '+repr(texts['atc']))
        local=local.replace("dp = (root / 'drivers/phy/apple/atc-j700-dp.h').read_text()", 'dp = '+repr(texts['dp']))


        exec(local,ns)
        production=ns['production'].replace('static u32 registers[', 'static u32 blocked_offset;\nstatic unsigned int mmio_reads;\nstatic bool track_dp_writes;\nstatic u32 dp_write_masks[0x30000 / 4];\nstatic u32 registers[', 1)
        production=production.replace('static u32 readl(void *address) { return *(u32 *)address; }', 'static u32 readl(void *address) { mmio_reads++; return *(u32 *)address; }', 1)
        production=production.replace('    registers[offset / 4] = (registers[offset / 4] & ~mask) | value;', '    if (track_dp_writes) dp_write_masks[offset / 4] |= mask | value;\n    if (blocked_offset && offset == blocked_offset) { writes++; return; }\n    registers[offset / 4] = (registers[offset / 4] & ~mask) | value;', 1)
        if name=='parent':
            proposal=texts.copy();proposal['dp']=variants['candidate']['dp']
            # Only share the public field inventory and address mapper with the parent control.
            start=proposal['dp'].index('/* Masks are the union')
            end=proposal['dp'].index('/* Capture before')
            production=production.replace(texts['dp'],proposal['dp'][start:end]+texts['dp'])
        suffix=ns['suffix'].replace('int main(', 'int original_fixture_main(',1)+controls
        unit=out/(name+'-restoration-test.c')
        logger = r'''
#include <stdarg.h>
static unsigned int diagnostic_records;
static char last_diagnostic[512];
static void record_debug(const char *format, ...)
{
    if (!strstr(format, "USB restore stage=")) return;
    va_list args; va_start(args,format);
    vsnprintf(last_diagnostic,sizeof(last_diagnostic),format,args);
    va_end(args); diagnostic_records++;
    fputs(last_diagnostic,stdout);
}
#undef dev_dbg
#define dev_dbg(dev, format, ...) record_debug(format, ##__VA_ARGS__)
'''
        unit.write_text(ns['prefix']+logger+('#define PROPOSAL\n' if name!='parent' else '')+production+suffix)
        subprocess.run(['clang','-std=gnu11','-Wall','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(unit),'-o',str(out/(name+'-restoration-test'))],check=True)
        r=subprocess.run([str(out/(name+'-restoration-test'))]+(['inactive-host'] if name=='gate-parent' else []),capture_output=True,text=True)
        (out/(name+'-restoration-test.log')).write_text(r.stdout+r.stderr)
        print(name,'exit',r.returncode,r.stdout,r.stderr[-500:])
        assert r.returncode!=0 if name in ('parent','gate-parent') else r.returncode==0
