#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise native27 drive messages against the actual callback implementation."""
from pathlib import Path
import os,subprocess,tempfile
root=next(p for p in Path(__file__).resolve().parents if (p/'drivers/gpu/drm/apple/dptxep.c').exists())
source=(root/'drivers/gpu/drm/apple/dptx-drive-v27.inc').read_text()
parent=(root/'drivers/gpu/drm/apple/dptxep.c').read_text()
a=parent.index('struct dptxport_apcall_max_drive_settings {');b=parent.index('\n};',a)+3;source+=parent[a:b]
a=parent.index('static int\ndptxport_call_get_max_drive_settings(');b=parent.index('\n}',a)+2;source+=parent[a:b]
pre=r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint32_t __le32;
#define __packed __attribute__((packed))
#define cpu_to_le32(v) (v)
#define le32_to_cpu(v) (v)
union phy_configure_opts {struct {unsigned voltage[4],pre[4],lanes,link_rate;bool set_voltages,set_rate,set_lanes;}dp;};
struct dptx_port{unsigned lane_count,drive_voltage[4],drive_pre[4];void *atcphy;union phy_configure_opts phy_ops;};

#define DCP_FIRMWARE_V_27_0 27
struct dcp{bool dptx_tunnel;int fw_compat;};struct ep{struct dcp*dcp;};struct apple_epic_service{struct ep*ep;struct dptx_port*cookie;};
static unsigned calls;static int failure;static union phy_configure_opts last;
static int phy_configure(void*p,union phy_configure_opts*o){assert(p);calls++;last=*o;return failure;}
'''
post=r'''
int main(void){
unsigned char input[80]={0},output[88];
struct dptxport_drive_v27*r=(void*)input,*o=(void*)output;
struct dcp d={0};struct ep e={&d};struct dptx_port p={.lane_count=4,.atcphy=&d};struct apple_epic_service s={&e,&p};
assert(sizeof(*r)==32 && offsetof(struct dptxport_drive_v27,count_or_status)==16 && offsetof(struct dptxport_drive_v27,lane)==32);
p.phy_ops.dp.link_rate=8100;
struct dptxport_apcall_max_drive_settings max;
assert(dptxport_call_get_max_drive_settings(&s,&max,31)==-EINVAL);
memset(&max,0,sizeof(max));assert(!dptxport_call_get_max_drive_settings(&s,&max,sizeof(max)));assert(max.max_drive_settings[0]==3&&max.max_drive_settings[1]==3);
d.fw_compat=27;assert(!dptxport_call_get_max_drive_settings(&s,&max,sizeof(max)));u32 third;memcpy(&third,max._unk1,4);assert(max.max_drive_settings[0]==0&&max.max_drive_settings[1]==3&&third==3);
r->count_or_status=4;
for(unsigned i=0;i<4;i++){r->lane[i].voltage=i;r->lane[i].pre=3-i;}
memset(output,0xa5,sizeof(output));
assert(!dptxport_call_drive_v27(&s,true,input,80,output,80));
assert(calls==1&&last.dp.set_voltages&&!last.dp.set_rate&&!last.dp.set_lanes&&last.dp.link_rate==8100&&last.dp.lanes==4);
assert(!o->status_or_count&&o->count_or_status==4&&output[80]==0xa5);
for(unsigned i=0;i<4;i++)assert(last.dp.voltage[i]==i&&last.dp.pre[i]==3-i);
r->status_or_count=4;
assert(!dptxport_call_drive_v27(&s,false,input,32,output,80));
assert(o->status_or_count==4&&!o->count_or_status&&calls==1);
for(unsigned i=0;i<4;i++)assert(!o->lane[i].type&&o->lane[i].voltage==i&&o->lane[i].pre==3-i);
assert(dptxport_call_drive_v27(&s,true,input,79,output,80)==-EINVAL);
assert(dptxport_call_drive_v27(&s,false,input,32,output,79)==-EINVAL);
r->count_or_status=5;assert(dptxport_call_drive_v27(&s,true,input,80,output,80)==-EINVAL);
r->count_or_status=4;r->lane[3].type=1;assert(dptxport_call_drive_v27(&s,true,input,80,output,80)==-EINVAL);r->lane[3].type=0;
r->lane[3].voltage=4;assert(dptxport_call_drive_v27(&s,true,input,80,output,80)==-EINVAL);r->lane[3].voltage=0;
failure=-EIO;assert(dptxport_call_drive_v27(&s,true,input,80,output,80)==-EIO);assert(p.drive_voltage[3]==3);failure=0;
p.atcphy=NULL;assert(dptxport_call_drive_v27(&s,true,input,80,output,80)==-ENODEV);
d.dptx_tunnel=true;assert(!dptxport_call_drive_v27(&s,true,input,80,output,32));assert(calls==2);
d.dptx_tunnel=false;p.atcphy=&d;r->count_or_status=2;
assert(!dptxport_call_drive_v27(&s,true,input,56,input,56));assert(r->count_or_status==2&&r->lane[1].voltage==1&&r->lane[1].pre==2);
p.lane_count=1;r->count_or_status=2;assert(dptxport_call_drive_v27(&s,true,input,56,output,80)==-EINVAL);
r->count_or_status=0;assert(!dptxport_call_drive_v27(&s,true,input,32,output,32));assert(calls==3);
puts("PASS: firmware27 drive ABI, four distinct lanes, bounded payloads, aliased buffers, PHY error/no-PHY propagation, tunnel semantics");
}
'''
if os.environ.get('TEST_SOURCE'):
    Path(os.environ['TEST_SOURCE']).write_text(pre+source+post)
with tempfile.TemporaryDirectory(prefix='dcp-drive27-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(pre+source+post)
 subprocess.run([os.environ.get('HOSTCC','cc'),'-O1','-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
