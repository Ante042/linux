#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""D569 native fixed-width names, boolean values, per-DCP metadata and failures."""
from pathlib import Path
import os, subprocess, tempfile
root=next(p for p in Path(__file__).resolve().parents if (p/'drivers/gpu/drm/apple/iomfb_v27.c').exists())
source=(root/'drivers/gpu/drm/apple/iomfb-boolean-properties-v27.inc').read_text()
parent=(root/'drivers/gpu/drm/apple/iomfb_v27.c').read_text()
assert '[569] = trampoline_set_boolean_property_v27' in parent and '[569] = { 0x44, 0x4 }' in parent
assert 'INIT_LIST_HEAD(&dcp->iomfb_boolean_properties)' in (root/'drivers/gpu/drm/apple/dcp.c').read_text()
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint32_t u32;
#define GFP_KERNEL 0
#define trace_iomfb_callback(...) ((void)0)
#define dev_dbg(...) ((void)0)
struct list_head {struct list_head *next,*prev;};
#define INIT_LIST_HEAD(p) ((p)->next=(p),(p)->prev=(p))
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define list_for_each_entry(p,h,m) for(struct list_head*iter=(h)->next;iter!=(h)&&((p)=container_of(iter,typeof(*(p)),m),1);iter=iter->next)
static void list_add_tail(struct list_head*n,struct list_head*h){n->next=h;n->prev=h->prev;h->prev->next=n;h->prev=n;}
#define list_for_each_entry_safe(p,n,h,m) for(struct list_head*iter=(h)->next,*after=iter->next;iter!=(h)&&((p)=container_of(iter,typeof(*(p)),m),(n)=after==(h)?NULL:container_of(after,typeof(*(p)),m),1);iter=after,after=iter->next)
static void list_del(struct list_head*n){n->next->prev=n->prev;n->prev->next=n->next;}
#define kfree(p) free(p)
struct apple_dcp{void*dev;struct list_head iomfb_boolean_properties;};
static unsigned allocations;static bool fail;
static void*kzalloc(size_t n,int f){if(fail)return NULL;allocations++;return calloc(1,n);}
static void put_unaligned_le32(u32 v,void*p){u8*b=p;for(int i=0;i<4;i++)b[i]=v>>(8*i);}
'''
post=r'''
static void set(struct apple_dcp*d,const void*in,unsigned success){u8 out[6]={0xa5,0xa5,0xa5,0xa5,0xa5,0xa5};assert(trampoline_set_boolean_property_v27(d,569,out+1,(void*)in));assert(out[0]==0xa5&&out[1]==success&&!out[2]&&!out[3]&&!out[4]&&out[5]==0xa5);}
static struct dcp_boolean_property_v27*first(struct apple_dcp*d){return container_of(d->iomfb_boolean_properties.next,struct dcp_boolean_property_v27,node);}

int main(void){
 struct apple_dcp a={0},b={0};INIT_LIST_HEAD(&a.iomfb_boolean_properties);INIT_LIST_HEAD(&b.iomfb_boolean_properties);
 u8 request[68]={0};memcpy(request,"IOMFBHotplugKeysChangedKey",26);request[64]=3;
 set(&a,request,1);assert(first(&a)->value&&allocations==1);assert(!strcmp(first(&a)->name,"IOMFBHotplugKeysChangedKey"));
 request[64]=2;set(&a,request,1);assert(!first(&a)->value&&allocations==1);
 request[64]=1;set(&b,request,1);assert(first(&b)->value&&!first(&a)->value&&allocations==2);
 memset(request,'x',64);request[64]=1;set(&a,request,1);assert(allocations==3);struct dcp_boolean_property_v27*p=container_of(a.iomfb_boolean_properties.prev,struct dcp_boolean_property_v27,node);assert(strlen(p->name)==63&&p->value);
 request[63]='y';request[64]=0;set(&a,request,1);assert(allocations==3&&!p->value);
 request[0]='z';fail=true;set(&a,request,0);assert(allocations==3);request[0]='x';set(&a,request,1);assert(allocations==3);
 iomfb_boolean_properties_cleanup_v27(&a);iomfb_boolean_properties_cleanup_v27(&b);assert(a.iomfb_boolean_properties.next==&a.iomfb_boolean_properties&&b.iomfb_boolean_properties.next==&b.iomfb_boolean_properties);puts("DCP_BOOLEAN_PROPERTIES_V27_PASS: native name truncation, bit0, per-DCP replacement, bounded replies, allocation failure");
}
'''
code=pre+source+post
if os.environ.get('TEST_SOURCE'):Path(os.environ['TEST_SOURCE']).write_text(code)
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['clang','-std=gnu11','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
