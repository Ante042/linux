#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production mapping/attachment functions with a fault-injecting DART transport."""
from pathlib import Path
import os, subprocess, tempfile
scratch = tempfile.TemporaryDirectory(prefix='apple-dart-x1n1-')
I = Path(scratch.name)
root = next((p for p in Path(__file__).resolve().parents if (p / 'drivers/iommu/apple-dart-x1n1.c').is_file()), None)
if os.environ.get('KERNEL_SRC'):
 root = Path(os.environ['KERNEL_SRC'])
if root is None:
 print('SKIP: set KERNEL_SRC to the Linux source tree')
 raise SystemExit(4)
src=(root/'drivers/iommu/apple-dart-x1n1.c').read_text()
pre = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
typedef uint64_t u64;typedef uint32_t u32;typedef uint64_t phys_addr_t;typedef int gfp_t;
#define U64_MAX UINT64_MAX
#define DART_X1N1_PAGE_SHIFT 14
#define DART_X1N1_PAGE_SIZE (1ul<<14)
#define DART_X1N1_BLOCK_SHIFT 25
#define DART_X1N1_BLOCK_SIZE (1ul<<25)
#define DART_X1N1_APERTURE_END 0xffffffffUL
#define DART_X1N1_MAX_STREAMS 256
#define X1N1_BATCH_MAX_OPS 255
#define X1N1_MMIO_DART_DB 0x800
#define X1N1_DART_INIT 1
#define X1N1_DART_ATTACH 2
#define X1N1_DART_MAP 3
#define X1N1_DART_UNMAP 4
#define X1N1_DART_ENABLE 5
#define X1N1_DART_DISABLE 6
#define GFP_KERNEL 0
#define GFP_ATOMIC 0
#define GFP_NOWAIT 0
#define XA_PRESENT 0
#define min(a,b) ((a)<(b)?(a):(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define WARN_ON(x) assert(!(x))
#define dev_err(d,...) do {(void)(d);fprintf(stderr,__VA_ARGS__);assert(0);}while(0)
#define DECLARE_BITMAP(n,size) unsigned long n[((size)+63)/64]
#define for_each_set_bit(i,b,n) for(i=0;i<(n);i++)if((b)[i/64]&(1ul<<(i%64)))
static void set_bit(unsigned i,unsigned long*b){b[i/64]|=1ul<<(i%64);}
static void clear_bit(unsigned i,unsigned long*b){b[i/64]&=~(1ul<<(i%64));}
static bool test_bit(unsigned i,unsigned long*b){return b[i/64]&(1ul<<(i%64));}
static void bitmap_zero(unsigned long*b,unsigned n){memset(b,0,((n+63)/64)*8);}
static unsigned bitmap_weight(unsigned long*b,unsigned n){unsigned v=0;for(unsigned i=0;i<n;i++)v+=test_bit(i,b);return v;}
static bool bitmap_empty(unsigned long*b,unsigned n){return !bitmap_weight(b,n);}
#define check_mul_overflow(a,b,p) __builtin_mul_overflow(a,b,p)
#define spin_lock_irqsave(lock,flags) do{(void)(lock);flags=0;}while(0)
#define spin_unlock_irqrestore(lock,flags) do{(void)(lock);(void)(flags);}while(0)
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
struct xe {unsigned long idx;void*value;};
struct xarray {struct xe e[4096];unsigned count;};
static int alloc_fail_after=-1;
static void*xa_mk_value(unsigned long v){return(void*)((v<<1)|1);}
static unsigned long xa_to_value(void*p){assert((uintptr_t)p&1);return(uintptr_t)p>>1;}
static int xa_err(void*p){return (uintptr_t)p>=(uintptr_t)-4095?(int)(intptr_t)p:0;}
static void*xa_load(struct xarray*x,unsigned long i){for(unsigned n=0;n<x->count;n++)if(x->e[n].idx==i)return x->e[n].value;return NULL;}
static int xa_insert(struct xarray*x,unsigned long i,void*v,int g){(void)g;if(xa_load(x,i))return-EBUSY;if(alloc_fail_after==0)return-ENOMEM;if(alloc_fail_after>0)alloc_fail_after--;assert(x->count<4096);x->e[x->count++]=(struct xe){i,v};return 0;}
static void*xa_store(struct xarray*x,unsigned long i,void*v,int g){for(unsigned n=0;n<x->count;n++)if(x->e[n].idx==i){void*old=x->e[n].value;x->e[n].value=v;return old;}int r=xa_insert(x,i,v,g);return r?(void*)(intptr_t)r:NULL;}
static void*xa_erase(struct xarray*x,unsigned long i){for(unsigned n=0;n<x->count;n++)if(x->e[n].idx==i){void*v=x->e[n].value;x->e[n]=x->e[--x->count];return v;}return NULL;}
static void*xa_find(struct xarray*x,unsigned long*i,unsigned long max,int m){(void)m;unsigned long best=ULONG_MAX;void*v=NULL;for(unsigned n=0;n<x->count;n++)if(x->e[n].idx>=*i&&x->e[n].idx<=max&&x->e[n].idx<best){best=x->e[n].idx;v=x->e[n].value;}if(v)*i=best;return v;}
struct x1n1_op {u32 op,flags;u64 arg[6],ret;};
struct x1n1_batch {u32 count,flags;int status;u32 done;struct x1n1_op op[255];};
struct iommu_domain{int ignored;};struct iommu_iotlb_gather{int ignored;};
struct apple_dart_x1n1 {unsigned num_streams,dart_id;struct x1n1_batch*batch;void*dev;int batch_lock;};
struct apple_dart_x1n1_master {struct apple_dart_x1n1*dart;DECLARE_BITMAP(sids,256);};
struct device {struct apple_dart_x1n1_master*master;};
struct apple_dart_x1n1_domain {struct iommu_domain domain;struct apple_dart_x1n1*dart;DECLARE_BITMAP(sids,256);int lock;struct xarray l2_attached,mappings;};
static struct apple_dart_x1n1_domain*to_x1n1_domain(struct iommu_domain*d){return(void*)d;}
static void*dev_iommu_priv_get(struct device*d){return d->master;}
static int apple_dart_x1n1_ensure_init(struct apple_dart_x1n1*d){(void)d;return 0;}
static struct xarray hw[256];static bool enabled[256],root[256];
static int fail_op,fail_sid=-1,fail_nth,seen;static unsigned maps_sent,rings;
static int x1n1_ring(struct apple_dart_x1n1*d,u32 off){
 assert(off==0x800+8*d->dart_id);struct x1n1_batch*b=d->batch;b->done=0;b->status=0;rings++;
 for(unsigned n=0;n<b->count;n++){
  struct x1n1_op*o=&b->op[n];unsigned sid=o->arg[0];assert(sid<256);
  if((int)o->op==fail_op&&(fail_sid<0||(int)sid==fail_sid)&&++seen==fail_nth){b->status=-EIO;return-EIO;}
  if(o->op==X1N1_DART_ATTACH){if(o->arg[2]==1)root[sid]=true;}
  else if(o->op==X1N1_DART_ENABLE){assert(root[sid]);enabled[sid]=true;}
  else if(o->op==X1N1_DART_DISABLE)enabled[sid]=false;
  else if(o->op==X1N1_DART_MAP){assert(root[sid]);maps_sent++;for(u64 j=0;j<o->arg[3];j+=16384){unsigned long idx=(o->arg[1]+j)>>14;assert(!xa_load(&hw[sid],idx));assert(!xa_insert(&hw[sid],idx,xa_mk_value(((o->arg[2]+j)>>14)*256+o->arg[4]),0));}}
  else if(o->op==X1N1_DART_UNMAP){for(u64 j=0;j<o->arg[2];j+=16384)xa_erase(&hw[sid],(o->arg[1]+j)>>14);}
  else assert(0);
  b->done++;
 }
 return 0;
}
"""
pre=pre[:pre.index('struct iommu_domain{')]
pre='\n'.join(l for l in pre.splitlines() if not l.startswith('static void bitmap_zero'))+'\n'
pre=pre.replace('#define X1N1_BATCH_MAX_OPS 255','#define X1N1_BATCH_MAX_OPS 255')
pre+='''
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
typedef u64 dma_addr_t;
#define DART_X1N1_NATIVE_APERTURE_END ((1ul<<42)-1)
#define X1N1_DART_RETAINED_INFO 9
#define BITS_TO_LONGS(n) (((n)+63)/64)
#define max(a,b) ((a)>(b)?(a):(b))
#define SINGLE_DEPTH_NESTING 1
#define mutex_lock_nested(x,n) ((void)(x))
#define spin_lock(x) ((void)(x))
#define spin_unlock(x) ((void)(x))
#define local_irq_save(f) do{f=0;}while(0)
#define local_irq_restore(f) ((void)(f))
#define write_lock(x) ((void)(x))
#define write_lock_nested(x,n) ((void)(x))
#define write_unlock(x) ((void)(x))
#define read_lock_irqsave(l,f) spin_lock_irqsave(l,f)
#define read_unlock_irqrestore(l,f) spin_unlock_irqrestore(l,f)
typedef int rwlock_t;
struct mutex {int unused;};
struct iommu_domain {void *owner,*ops;struct {u64 aperture_start,aperture_end;bool force_aperture;} geometry;};
struct iommu_iotlb_gather {int unused;};
static const struct {void *default_domain_ops;} apple_dart_x1n1_ops={0};
'''
pre+=src[src.index('struct x1n1_retained_run {'):src.index('struct apple_dart_x1n1 {')]
pre+='''struct apple_dart_x1n1 {unsigned num_streams,dart_id;struct x1n1_batch *batch;void *dev;int batch_lock;u64 dma_min,dma_max;struct x1n1_retained_sid *retained[256];};\n'''
pre+=src[src.index('/* Match the native'):src.index('/* Native IODARTMapper')]
pre+='''
struct device {struct apple_dart_x1n1_master *master;};
static void *dev_iommu_priv_get(struct device *d){return d->master;}
static int apple_dart_x1n1_ensure_init(struct apple_dart_x1n1*d){(void)d;return 0;}
static struct xarray hw[8][256];
static bool enabled[8][256],root[8][256];
static struct xarray leaves[8][256];
static int fail_op,fail_dart=-1,fail_sid=-1,fail_nth,seen;
static unsigned maps_sent,rings,map_rings,largest_map_batch;
static int x1n1_ring(struct apple_dart_x1n1*d,u32 off){
 assert(off==0x800+8*d->dart_id);struct x1n1_batch*b=d->batch;b->done=0;b->status=0;rings++;
 assert(b->count<=255);
 if(b->op[0].op==X1N1_DART_MAP){map_rings++;largest_map_batch=max(largest_map_batch,b->count);}
 for(unsigned n=0;n<b->count;n++){
  struct x1n1_op*o=&b->op[n];unsigned sid=o->arg[0],id=d->dart_id;assert(id<8&&sid<256);
  if((int)o->op==fail_op&&(fail_dart<0||(int)id==fail_dart)&&(fail_sid<0||(int)sid==fail_sid)&&++seen==fail_nth){b->status=-EIO;return-EIO;}
  if(o->op==X1N1_DART_ATTACH){
   if(o->arg[2]==1)root[id][sid]=true;
   else{unsigned long key=o->arg[1]>>25;if(!d->retained[sid])assert(!xa_load(&leaves[id][sid],key));if(!xa_load(&leaves[id][sid],key))assert(!xa_insert(&leaves[id][sid],key,xa_mk_value(1),0));}
  }
  else if(o->op==X1N1_DART_ENABLE){assert(root[id][sid]);enabled[id][sid]=true;}
  else if(o->op==X1N1_DART_DISABLE)enabled[id][sid]=false;
  else if(o->op==X1N1_DART_RETAINED_INFO){assert(d->retained[sid]);root[id][sid]=enabled[id][sid]=true;}
  else if(o->op==X1N1_DART_MAP){
   assert(root[id][sid]);maps_sent++;
   for(u64 j=0;j<o->arg[3];j+=16384){unsigned long idx=(o->arg[1]+j)>>14;assert(!xa_load(&hw[id][sid],idx));assert(!xa_insert(&hw[id][sid],idx,xa_mk_value(((o->arg[2]+j)>>14)*256+o->arg[4]),0));}
  }
  else if(o->op==X1N1_DART_UNMAP){for(u64 j=0;j<o->arg[2];j+=16384)xa_erase(&hw[id][sid],(o->arg[1]+j)>>14);}
  else assert(0);
  b->done++;
 }
 return 0;
}
'''
pre+=src[src.index('static const struct x1n1_retained_run *'):src.index('/* Bring the DART up')]
helper=src[src.index('/* Live mappings are indexed'):src.index('static void apple_dart_x1n1_iotlb_sync')]
helper+=src[src.index('static phys_addr_t apple_dart_x1n1_iova_to_phys'):src.index('static void apple_dart_x1n1_domain_free')]
post=Path(__file__).with_name('apple-dart-x1n1-map.c').read_text()
out=I/'test-dart-multidart.c';out.write_text(pre+helper+post)
subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Wno-unused-parameter','-Werror',str(out),'-o',str(I/'test-dart-multidart')],check=True)
subprocess.run([str(I/'test-dart-multidart')],check=True)
