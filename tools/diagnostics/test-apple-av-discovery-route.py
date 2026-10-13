#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the real AFK lookup/receive functions against a routing oracle."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'drivers/gpu/drm/apple/dcp.c').read_text()
h=(root/'drivers/gpu/drm/apple/dcp-internal.h').read_text()
def function(name):
 m=re.search(r'^static [^\n]*\b'+name+r'\(',s,re.M);assert m
 a=s.index('{',m.start());n=1;i=a+1
 while n:
  n+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[m.start():i]
a=h.index('enum {\n\tSYSTEM_ENDPOINT');b=h.index('\n};',a)+3
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define trace_dcp_recv_msg(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#define dev_dbg_ratelimited(...) ((void)0)
enum { DCP_FIRMWARE_H17P=17, DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL=99 };
struct apple_dcp_afkep { unsigned int endpoint; };
struct apple_dcp {
 bool quiescing;
 unsigned int fw_compat;
 struct { int iomfb_method_profile; } hw;
 struct apple_dcp_afkep *av_discovery,*avep,*systemep,*ibootep,*dcpavservep,*dptxep,*dptx_controller_ep;
};
static struct apple_dcp_afkep *received;
static unsigned int iomfb_calls;
static void afk_receive_message(struct apple_dcp_afkep *p,u64 msg) { (void)msg;received=p; }
static void iomfb_recv_msg(struct apple_dcp *p,u64 msg) { (void)p;(void)msg;iomfb_calls++; }
'''
suffix=r'''
int main(void) {
 struct apple_dcp_afkep port={DPTX_ENDPOINT_V2}, probe={DPTX_ENDPOINT}, oldav={AV_ENDPOINT};
 struct apple_dcp d={.fw_compat=DCP_FIRMWARE_H17P,.hw.iomfb_method_profile=DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL,.dptxep=&port,.avep=&oldav};
 assert(dcp_afkep(&d,DPTX_ENDPOINT_V2)==&port);
 assert(dcp_afkep(&d,AV_ENDPOINT)==&oldav);
 d.av_discovery=&probe;
 assert(dcp_afkep(&d,DPTX_ENDPOINT)==&probe);
 assert(dcp_afkep(&d,DPTX_ENDPOINT_V2)==&port);
 received=0;dcp_recv_msg(&d,DPTX_ENDPOINT,1);assert(received==&probe);
 received=0;dcp_recv_msg(&d,DPTX_ENDPOINT_V2,1);assert(received==&port);
 received=0;dcp_recv_msg(&d,AV_ENDPOINT,1);assert(received==&oldav);
 received=0;d.quiescing=true;dcp_recv_msg(&d,DPTX_ENDPOINT,1);assert(!received);d.quiescing=false;
 dcp_recv_msg(&d,IOMFB_ENDPOINT,1);assert(iomfb_calls==1);
 d.av_discovery=0;d.hw.iomfb_method_profile=0;d.fw_compat=0;port.endpoint=DPTX_ENDPOINT;
 received=0;dcp_recv_msg(&d,DPTX_ENDPOINT,1);assert(received==&port);
 received=0;dcp_recv_msg(&d,DPTX_ENDPOINT_V2,1);assert(received==&port);
 assert(dcp_afkep(&d,DPTX_ENDPOINT_V2)==0);
 d.fw_compat=DCP_FIRMWARE_H17P;received=0;dcp_recv_msg(&d,0x2f,1);assert(!received);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='av-routing-') as tmp:
 p=Path(tmp);c=p/'route.c';c.write_text(prefix+h[a:b]+function('dcp_afkep')+function('dcp_recv_msg')+suffix)
 subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-fsanitize=address,undefined',str(c),'-o',str(p/'route')],check=True)
 subprocess.run([str(p/'route')],check=True)
print('PASS: actual AFK endpoint lookup and receive routing, legacy and H17P, quiescing and unknown endpoints')
