#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import os, subprocess, tempfile
scratch = tempfile.TemporaryDirectory(prefix='apple-dcp-test-')
I = Path(scratch.name)
root = next((p for p in Path(__file__).resolve().parents
             if (p / 'drivers/gpu/drm/apple/afk.c').is_file()), None)
if os.environ.get('KERNEL_SRC'):
    root = Path(os.environ['KERNEL_SRC'])
if root is None:
    print('SKIP: set KERNEL_SRC to the Linux source tree')
    raise SystemExit(4)
S=(root/'drivers/gpu/drm/apple')
s=(S/'afk.c').read_text();h=(S/'afk.h').read_text()
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64,phys_addr_t;
typedef u16 __le16;typedef u32 __le32;typedef u64 __le64;
#define __packed __attribute__((packed))
#define static_assert _Static_assert
#define U64_MAX UINT64_MAX
#define PAGE_SIZE 4096ul
#define PAGE_MASK (~(PAGE_SIZE-1))
#define GFP_KERNEL 0
#define MEMREMAP_WB 1
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define le64_to_cpu(x) (x)
#define cpu_to_le16(x) (x)
#define cpu_to_le32(x) (x)
#define cpu_to_le64(x) (x)
#define dma_rmb() ((void)0)
#define dma_wmb() ((void)0)
#define dev_err(d,...) ((void)(d))
#define dev_warn(d,...) ((void)(d))
#define dev_err_ratelimited(d,...) ((void)(d))
static int fail_alloc, allocations;
static void *kvmalloc(size_t n,int g){(void)g;if(++allocations==fail_alloc)return NULL;return malloc(n);}
static void *kvzalloc(size_t n,int g){void *p=kvmalloc(n,g);if(p)memset(p,0,n);return p;}
#define kvfree free
struct iommu_domain {int unused;};struct device{int unused;};
struct apple_dcp{struct device *dev;};struct apple_dcp_afkep{struct apple_dcp *dcp;u32 endpoint;};
struct apple_epic_service;
struct apple_epic_service_ops {int (*call)(struct apple_epic_service *,u32,const void*,size_t,void*,size_t);};
struct apple_epic_service {bool torndown;const struct apple_epic_service_ops *ops;};
static struct apple_epic_service service;
static bool service_missing;static int handler_calls,handler_error;
static struct apple_epic_service *afk_epic_find_service(struct apple_dcp_afkep *e,u32 c){(void)e;assert(c==5);return service_missing?NULL:&service;}
static u8 pages[4][PAGE_SIZE];static bool mapped[4];
static struct iommu_domain domain;
static struct iommu_domain *iommu_get_domain_for_dev(struct device*d){(void)d;return &domain;}
static phys_addr_t iommu_iova_to_phys(struct iommu_domain*d,u64 va){(void)d;if(va<0x100000||va>=0x104000||!mapped[(va-0x100000)/PAGE_SIZE])return 0;return 0x10000+((va-0x100000)/PAGE_SIZE)*0x2000+(va&(PAGE_SIZE-1));}
static void *memremap(phys_addr_t pa,size_t n,int flags){assert(flags==MEMREMAP_WB);unsigned p=(pa-0x10000)/0x2000,off=(pa-0x10000)%0x2000;assert(p<4&&off+n<=PAGE_SIZE);return &pages[p][off];}
static void memunmap(void*p){(void)p;}
static u8 sent[32768];static size_t sent_size;static u8 sent_type;static unsigned sent_count;
static int afk_send_epic(struct apple_dcp_afkep *e,u32 c,u16 tag,int et,int cat,u8 type,const void*p,size_t n){(void)e;assert(c==5&&tag==0xa7&&et==4&&cat==0x20&&n<=sizeof(sent));memcpy(sent,p,n);sent_size=n;sent_type=type;sent_count++;return 0;}
'''
# Include production wire structs/constants, then the production copy/dispatch functions.
wire=h[h.index('struct epic_cmd_v2 {'):h.index('struct afk_ringbuffer {')]
code=s[s.index('static int afk_copy_remote'):s.index('static void afk_recv_handle(struct apple_dcp_afkep')]
post=r'''
static struct device dev;static struct apple_dcp dcp={&dev};static struct apple_dcp_afkep ep={&dcp,0x2c};
static u8 packet[8192], message[8192];
static int handler(struct apple_epic_service*s,u32 cmd,const void*in,size_t n,void*out,size_t out_n){(void)s;assert(cmd==10&&n==32&&out_n==32&&((const u8*)in)[8]==0x5a);handler_calls++;memcpy(out,in,n);*(u32*)out=0;*(u64*)((u8*)out+16)=4;return handler_error;}
static const struct apple_epic_service_ops ops={handler};
static void reset(unsigned flags){memset(packet,0,sizeof(packet));memset(message,0,sizeof(message));memset(pages,0,sizeof(pages));for(int i=0;i<4;i++)mapped[i]=true;service=(struct apple_epic_service){.ops=&ops};service_missing=false;handler_calls=handler_error=allocations=fail_alloc=sent_count=0;sent_size=0;struct epic_cmd_v2*c=(void*)packet;c->flags=flags;c->tag=0xa7;c->length=96;struct epic_service_call*m=(void*)message;m->command=10;m->data_len=32;m->magic=EPIC_SERVICE_CALL_MAGIC;message[64+8]=0x5a;}
static void call(size_t n){afk_recv_command_v2(&ep,5,EPIC_SUBTYPE_STD_SERVICE,packet,n);assert(sent_count==1);}
static u32 status(void){return ((struct epic_cmd_v2*)sent)->length;}
static void check_reply(const u8*p){const struct epic_service_call*m=(const void*)p;assert(m->magic==EPIC_SERVICE_CALL_MAGIC&&m->data_len==32&&m->command==10);assert(*(u64*)(p+64+16)==4&&p[64+8]==0x5a);}
int main(void){
 reset(0);memcpy(packet+8,message,96);call(104);assert(!status()&&handler_calls==1&&sent_size==104);check_reply(sent+8);puts("PASS: native 8-byte context + 64-byte DCPAV inline command/reply");
 reset(1);struct epic_cmd_v2*c=(void*)packet;c->txbuf=0x100ff8;c->txlen=96;c->rxbuf=0x102ff0;c->rxlen=128;
 assert(!afk_copy_remote(&ep,c->txbuf,message,96,true));call(32);assert(!status()&&handler_calls==1&&sent_size==32&&((struct epic_cmd_v2*)sent)->rxlen==96);
 assert(!afk_copy_remote(&ep,c->rxbuf,message,96,false));check_reply(message);puts("PASS: scatter-backed OOB request/reply crossing CPU page boundaries");
 reset(1);c=(void*)packet;c->rxbuf=0x102ff0;c->rxlen=96;memcpy(packet+32,message,96);call(128);assert(!status()&&sent_size==32);assert(!afk_copy_remote(&ep,c->rxbuf,message,96,false));check_reply(message);puts("PASS: inline request with OOB response");
 reset(1);c=(void*)packet;c->txbuf=0x100ff8;c->txlen=96;assert(!afk_copy_remote(&ep,c->txbuf,message,96,true));call(32);assert(!status()&&sent_size==128);check_reply(sent+32);puts("PASS: OOB request with inline response");
 reset(1);memcpy(packet+32,message,96);call(128);assert(!status()&&sent_size==128);check_reply(sent+32);puts("PASS: OOB descriptor with zero DMA lengths retains inline data");
 reset(1);c=(void*)packet;c->rxbuf=0x102000;c->rxlen=95;memcpy(packet+32,message,96);call(128);assert(status()==0xe00002e1&&!handler_calls&&sent_size==32);puts("PASS: insufficient response capacity rejected before handler");
 reset(0);message[12]=0;memcpy(packet+8,message,96);call(104);assert(status()==0xe00002c2&&!handler_calls&&sent_size==8);
 reset(0);((struct epic_service_call*)message)->data_len=31;memcpy(packet+8,message,96);call(104);assert(status()==0xe00002c2&&!handler_calls);puts("PASS: message magic and exact body size validated");
 reset(1);c=(void*)packet;c->txbuf=0x100ff8;c->txlen=96;mapped[1]=false;call(32);assert(status()&&!handler_calls);puts("PASS: ungranted remote page produces error response");
 reset(0);service_missing=true;call(8);assert(status()==0xe00002d7);puts("PASS: unavailable service uses native no-handler response");
 reset(0);fail_alloc=1;memcpy(packet+8,message,96);call(104);assert(status()==0xe00002bd&&!handler_calls);
 reset(0);fail_alloc=2;memcpy(packet+8,message,96);call(104);assert(status()==0xe00002bd&&!handler_calls);puts("PASS: request/reply allocation failure returns tagged error");
 reset(0);handler_error=-EINVAL;memcpy(packet+8,message,96);call(104);assert(status()==0xe00002c2&&handler_calls==1);puts("PASS: handler failure does not acknowledge success");
 reset(0);afk_recv_command_v2(&ep,5,0xc0,packet,7);assert(!sent_count);puts("PASS: truncated context cannot generate a reply with an invalid tag");
}
'''
p=I/'test-afk27-incoming.c';p.write_text(pre+wire+code+post)
subprocess.run([os.environ.get('HOSTCC', 'cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-O1','-fsanitize=undefined,address','-g',str(p),'-o',str(I/'test-afk27-incoming')],check=True)
subprocess.run([str(I/'test-afk27-incoming')],check=True)
