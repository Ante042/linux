#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Native D133/A375 wire layout, identity predicate and deferred ACK contexts."""
from pathlib import Path
import os,subprocess,tempfile
root=next(p for p in Path(__file__).resolve().parents if (p/'drivers/gpu/drm/apple/iomfb_v27.c').exists())
source=(root/'drivers/gpu/drm/apple/iomfb-display-identity-v27.inc').read_text()
parent=(root/'drivers/gpu/drm/apple/iomfb_v27.c').read_text()
assert '[133] = trampoline_display_identity_v27' in parent and '[133] = { 0x0, 0x4 }' in parent
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint32_t __le32;
#define __packed __attribute__((packed))
#undef static_assert
#define static_assert(x) _Static_assert(x, #x)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define GFP_KERNEL 0
#define WARN_ON(x) (x)
enum dcp_context_id{DCP_CONTEXT_CB=0,DCP_CONTEXT_ASYNC=3,DCP_CONTEXT_OOBCB=4,DCP_CONTEXT_OOBASYNC=7};
struct dcp_channel{unsigned depth;void*output[4];};
struct apple_dcp{struct dcp_channel ch_cb,ch_async,ch_oobcb,ch_oobasync;};
struct dcp_method_entry{const char*name;char tag[4],tag_h17g[4];};
static unsigned pushes,acks,frees;static int ack_context;static bool fail_alloc;
static void(*pending)(struct apple_dcp*,void*,void*);static void*pending_cookie;
static int expected_origin;
static void* kzalloc(size_t n,int f){return fail_alloc?NULL:calloc(1,n);}
static void kfree(void*p){frees++;free(p);}
static void put_unaligned_le32(u32 v,void*p){memcpy(p,&v,4);}
#define dev_info(...) ((void)0)
#define trace_iomfb_callback(...) ((void)0)
static void dcp_ack(struct apple_dcp*d,enum dcp_context_id ctx){acks++;ack_context=ctx;}
static int dcp_push_nested(struct apple_dcp*d,enum dcp_context_id origin,const struct dcp_method_entry*m,u32 il,u32 ol,void*data,void(*cb)(struct apple_dcp*,void*,void*),void*cookie){
 const u8*bytes=data;u8 expected[24]={0};expected[14]=10;
 assert(il==24&&ol==20&&!memcmp(m->tag,"A375",4)&&origin==expected_origin);
 assert(!memcmp(bytes,expected,24));pushes++;pending=cb;pending_cookie=cookie;return 0;
}
'''
post=r'''
int main(void){
struct apple_dcp d={0};struct dcp_channel*channels[]={&d.ch_cb,&d.ch_async,&d.ch_oobcb,&d.ch_oobasync};int contexts[]={0,3,4,7};
for(int ch=0;ch<4;ch++)for(int v=0;v<3;v++)for(int id=0xae40;id<=0xae45;id++)for(int success=0;success<3;success++){
 u8 output[8]={0};u8 response[20]={0};unsigned n=acks,p=pushes;
 channels[ch]->depth=1;channels[ch]->output[0]=output;
 expected_origin=contexts[ch];
 assert(!trampoline_display_identity_v27(&d,133,output,NULL)&&acks==n&&pushes==p+1);
 if(v==1)memcpy(response,"APP",4);if(v==2)memcpy(response,"SAM",4);
 response[14]=id;response[15]=id>>8;response[18]=success;response[19]=0xa5;
 pending(&d,response,pending_cookie);
 assert(output[0]==(success==1&&v!=2&&(id==0xae42||id==0xae43)));
 for(int i=1;i<8;i++)assert(!output[i]);
 assert(acks==n+1&&ack_context==contexts[ch]&&frees==pushes);
 channels[ch]->depth=0;
}
u8 output[4]={0};d.ch_cb.depth=1;d.ch_cb.output[0]=output;fail_alloc=true;
unsigned p=pushes;assert(trampoline_display_identity_v27(&d,133,output,NULL)&&pushes==p&&!output[0]);
puts("DCP_DISPLAY_IDENTITY_V27_PASS: native A375 payload, products/vendors, deferred ACK on four callback contexts");return 0;
}
'''
code=pre+source+post
if os.environ.get('TEST_SOURCE'):Path(os.environ['TEST_SOURCE']).write_text(code)
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['clang','-std=gnu11','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
