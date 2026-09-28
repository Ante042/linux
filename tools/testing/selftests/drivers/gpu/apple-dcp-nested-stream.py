#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real RPC push/ACK stacks with native firmware-owned stream offsets."""
from pathlib import Path
import os,re,subprocess,tempfile
root=next(p for p in Path(__file__).resolve().parents if (p/'drivers/gpu/drm/apple/iomfb.c').exists())
s=(root/'drivers/gpu/drm/apple/iomfb.c').read_text();h=(root/'drivers/gpu/drm/apple/iomfb.h').read_text()
a=h.index('enum dcp_context_id {');enum=h[a:h.index('\n};',a)+3]
def f(name):
 pat=r'^(?:static )?(?:inline )?(?:void|int|u8|u16|u64|enum dcp_context_id|struct dcp_channel \*)\s*'+name+r'\('
 m=re.search(pat,s,re.M);assert m,name;a=m.start();return s[a:s.index('\n}',a)+2]
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;typedef uint32_t __le32;
#define __packed __attribute__((packed))
#undef static_assert
#define static_assert(x) _Static_assert(x, #x)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define FIELD_PREP(m,v) (((u64)(v)<<__builtin_ctzll(m))&(m))
#define FIELD_GET(m,v) (((u64)(v)&(m))>>__builtin_ctzll(m))
#define IOMFB_MSG_CONTEXT 0xf00ULL
#define IOMFB_MSG_OFFSET 0xffff0000ULL
#define IOMFB_MSG_LENGTH 0xffff00000000ULL
#define IOMFB_MSG_ACK 0x40ULL
#define IOMFB_MESSAGE_TYPE 0xfULL
#define IOMFB_MESSAGE_TYPE_MSG 2
#define IOMFB_ENDPOINT 0x20
#define DCP_MAX_CALL_DEPTH 8
#define DCP_PACKET_ALIGNMENT 64
#define DCP_IOMFB_METHODS_H17G 1
#define ALIGN(n,a) (((n)+(a)-1)&~((a)-1))
#define WARN_ON(x) ({bool z=!!(x);assert(!z);z;})
#define GFP_KERNEL 0
#define kzalloc(n,f) calloc(1,n)
#define kfree(p) free(p)
#define trace_iomfb_push(...) ((void)0)
#define trace_iomfb_callback(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define msecs_to_jiffies(n) (n)
#define mod_delayed_work(...) ((void)0)
static unsigned long jiffies;
static int dcp_trace_budget,dcp_trace_calls;
struct apple_dcp;
typedef void(*dcp_callback_t)(struct apple_dcp*,void*,void*);
struct dcp_channel{dcp_callback_t callbacks[8];void*cookies[8],*output[8];u16 end[8];char tag[8][4];unsigned long pushed_at[8];u8 depth;};
struct apple_dcp{struct dcp_channel ch_cb,ch_cmd,ch_async,ch_oobcb,ch_oobcmd,ch_oobasync;struct{int iomfb_method_profile;}hw;void*shmem;bool warned_stall;};
struct dcp_method_entry{const char*name;char tag[4],tag_h17g[4];};
struct dcp_packet_header{char tag[4];u32 in_len,out_len;};
static u64 sent[8];static unsigned nsent;
static void dcp_send_message(struct apple_dcp*d,unsigned ep,u64 msg){assert(ep==0x20&&nsent<8);sent[nsent++]=msg;}
static void put_unaligned_le32(u32 v,void*p){memcpy(p,&v,4);}
'''
functions='\n'.join(f(n)for n in ['dcp_tx_offset','dcp_channel_offset','dcpep_msg','dcpep_ack','dcp_call_context','dcp_get_channel','dcp_packet_start','dcp_push_depth','dcp_pop_depth','dcp_push_context','dcp_push','dcp_push_nested','dcp_ack','dcpep_handle_ack'])
identity=(root/'drivers/gpu/drm/apple/iomfb-display-identity-v27.inc').read_text()
post=r'''
int main(void){
const int origins[]={0,3,4,7},wire[]={0,1,4,5};
/* Captured native stream initialization, not computed by the tested helpers. */
const int tx[]={0,0x20000,0x8000,0x28000},rx[]={0x60000,0x40000,0x68000,0x48000};
for(int i=0;i<4;i++){
 struct apple_dcp d={.shmem=calloc(1,0x80000)};struct dcp_channel*c=dcp_get_channel(&d,origins[i]);
 assert(dcp_tx_offset(wire[i])==tx[i]&&dcp_channel_offset(wire[i])==rx[i]);
 assert(c==dcp_get_channel(&d,wire[i]));
 /* An unrelated AP command must not steal a firmware-owned nested query. */
 d.ch_cmd.depth=1;d.ch_oobcmd.depth=1;
 c->depth=1;c->end[0]=64;c->output[0]=d.shmem+rx[i]+12;memcpy(c->tag[0],"D133",4);
 nsent=0;assert(!trampoline_display_identity_v27(&d,133,c->output[0],NULL));
 assert(nsent==1&&FIELD_GET(IOMFB_MSG_CONTEXT,sent[0])==(u64)wire[i]);
 assert(FIELD_GET(IOMFB_MSG_OFFSET,sent[0])==64&&FIELD_GET(IOMFB_MSG_LENGTH,sent[0])==56);
 assert(c->depth==2&&c->end[1]==128&&c->output[1]==d.shmem+tx[i]+100);
 struct dcp_packet_header*header=d.shmem+tx[i]+64;
 assert(!memcmp(header->tag,"573A",4)&&header->in_len==24&&header->out_len==20);
 u8*request=(void*)(header+1);for(int j=0;j<24;j++)assert(request[j]==(j==14?10:0));
 u8*reply=c->output[1];memcpy(reply,"APP",4);reply[14]=0x42;reply[15]=0xae;reply[18]=1;
 dcpep_handle_ack(&d,wire[i]);
 assert(nsent==2&&FIELD_GET(IOMFB_MSG_CONTEXT,sent[1])==(u64)origins[i]&&(sent[1]&IOMFB_MSG_ACK));
 assert(c->depth==0&&*(u8*)(d.shmem+rx[i]+12)==1);
 assert(d.ch_cmd.depth==1&&d.ch_oobcmd.depth==1);
 free(d.shmem);
}

for(int oob=0;oob<2;oob++){
 struct apple_dcp d={.shmem=calloc(1,0x80000)};struct dcp_method_entry m={.tag={'A','0','0','0'}};
 struct dcp_channel *cmd=oob?&d.ch_oobcmd:&d.ch_cmd,*cb=oob?&d.ch_oobcb:&d.ch_cb;
 nsent=0;dcp_push(&d,oob,&m,0,0,NULL,NULL,NULL);
 assert(FIELD_GET(IOMFB_MSG_CONTEXT,sent[0])==(u64)(oob?6:2)&&cmd->depth==1);
 dcpep_handle_ack(&d,oob?6:2);assert(!cmd->depth);
 cmd->depth=1;cb->depth=1;cb->end[0]=64;
 dcp_push(&d,oob,&m,0,0,NULL,NULL,NULL);
 assert(FIELD_GET(IOMFB_MSG_CONTEXT,sent[1])==(u64)(oob?4:0)&&cb->depth==2);
 assert(FIELD_GET(IOMFB_MSG_OFFSET,sent[1])==64);
 unsigned n=nsent;assert(dcp_push_nested(&d,DCP_CONTEXT_CMD,&m,0,0,NULL,NULL,NULL)==-EINVAL&&nsent==n);
 free(d.shmem);
}
puts("DCP_NESTED_STREAM_PASS: actual RPC/ACK stacks, native TX/RX regions, firmware-owned contexts1/5");return 0;
}
'''
code=pre+enum+functions+identity+post
if os.environ.get('TEST_SOURCE'):Path(os.environ['TEST_SOURCE']).write_text(code)
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['clang','-std=gnu11','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
