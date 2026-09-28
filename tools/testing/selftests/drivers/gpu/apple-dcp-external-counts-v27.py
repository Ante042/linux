#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Firmware27 packed count ABI, shared DCP state, balanced events and concurrency."""
from pathlib import Path
import os, subprocess, tempfile
root = next(p for p in Path(__file__).resolve().parents if (p/'drivers/gpu/drm/apple/iomfb_v27.c').exists())
source = (root/'drivers/gpu/drm/apple/iomfb-external-counts-v27.inc').read_text()
parent = (root/'drivers/gpu/drm/apple/iomfb_v27.c').read_text()
for tag, size in [(130, '0x8, 0x0'), (131, '0x10, 0xc'), (132, '0x4, 0x0')]:
    assert f'[{tag}] = {{ {size} }}' in parent
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
typedef uint8_t u8; typedef uint32_t u32;
#define DEFINE_SPINLOCK(n) pthread_mutex_t n=PTHREAD_MUTEX_INITIALIZER
#define spin_lock_irqsave(n, f) do { (f)=0; assert(!pthread_mutex_lock(n)); } while(0)
#define spin_unlock_irqrestore(n, f) do { (void)(f); assert(!pthread_mutex_unlock(n)); } while(0)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define DRM_MODE_CONNECTOR_eDP 14
#define trace_iomfb_callback(...) ((void)0)
struct apple_dcp { int connector_type; };
static u32 get_unaligned_le32(const void*p) { const u8*b=p;return b[0]|b[1]<<8|b[2]<<16|(u32)b[3]<<24; }
static void put_unaligned_le32(u32 v,void*p) {u8*b=p;for(int i=0;i<4;i++)b[i]=v>>(8*i);}
'''
post = r'''
static struct apple_dcp internal={14}, hdmi={11}, usbc={10};
static void read_counts(struct apple_dcp*d,u32 a,u32 b,u32 c) {
 u8 in[16]={0},out[12];memset(out,0xa5,sizeof(out));
 assert(trampoline_get_external_counts_v27(d,131,out,in));
 assert(get_unaligned_le32(out)==a&&get_unaligned_le32(out+4)==b&&get_unaligned_le32(out+8)==c);
}
static void* cycle(void*arg) {
 struct apple_dcp*d=arg;u8 plug[8]={1,1,3},unplug[8]={0,1,3},on[4]={1},off[4]={0};
 for(int i=0;i<1000;i++) {
  trampoline_update_external_counts_v27(d,130,NULL,plug);
  trampoline_apply_suppression_v27(d,132,NULL,on);
  trampoline_apply_suppression_v27(d,132,NULL,off);
  trampoline_update_external_counts_v27(d,130,NULL,unplug);
 }return NULL;
}
int main(void) {
 /* Captured Samsung D130 payload: count is packed at offset2, not4. */
 u8 plug[8]={1,0,2,0,0,0,0,0},unplug[8]={0,0,2,0,0,0,0,0};
 trampoline_update_external_counts_v27(&internal,130,NULL,plug);read_counts(&hdmi,0,0,0);
 trampoline_update_external_counts_v27(&hdmi,130,NULL,plug);read_counts(&internal,2,0,0);
 u8 apple[8]={3,3,4},off[8]={2,3,4}; /* bool uses bit0, not nonzero. */
 trampoline_update_external_counts_v27(&usbc,130,NULL,apple);read_counts(&hdmi,6,1,0);
 u8 on[4]={3},clear[4]={2};
 trampoline_apply_suppression_v27(&internal,132,NULL,on);read_counts(&usbc,6,1,1);
 trampoline_apply_suppression_v27(&hdmi,132,NULL,on);read_counts(&usbc,6,1,2);
 for(unsigned mask=0;mask<8;mask++) {
  u8 in[16]={0},out[14];memset(out,0xa5,sizeof(out));
  for(int i=0;i<3;i++)in[12+i]=2+((mask>>i)&1);
  trampoline_get_external_counts_v27(&hdmi,131,out+1,in);
  u32 values[]={6,1,2};
  for(int i=0;i<3;i++)assert(get_unaligned_le32(out+1+4*i)==((mask>>i)&1?0xa5a5a5a5:values[i]));
  assert(out[0]==0xa5&&out[13]==0xa5);
 }
 trampoline_update_external_counts_v27(&hdmi,130,NULL,unplug);
 trampoline_update_external_counts_v27(&usbc,130,NULL,off);
 trampoline_apply_suppression_v27(&internal,132,NULL,clear);
 trampoline_apply_suppression_v27(&hdmi,132,NULL,clear);read_counts(&internal,0,0,0);
 pthread_t a,b;assert(!pthread_create(&a,NULL,cycle,&hdmi));assert(!pthread_create(&b,NULL,cycle,&usbc));
 assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));read_counts(&internal,0,0,0);
 puts("DCP_EXTERNAL_COUNTS_V27_PASS: packed wire ABI, shared counts, internal gating, suppression, nullable outputs, concurrent balanced events");
}
'''
code = pre + source + post
if os.environ.get('TEST_SOURCE'): Path(os.environ['TEST_SOURCE']).write_text(code)
with tempfile.TemporaryDirectory() as d:
    p=Path(d); (p/'test.c').write_text(code)
    subprocess.run(['clang','-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
