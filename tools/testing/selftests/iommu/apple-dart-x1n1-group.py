#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
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
s=(root/'drivers/iommu/apple-dart-x1n1.c').read_text()
pre = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#define CONFIG_PCI 1
#define GFP_KERNEL 0
#define ERR_PTR(x) ((struct iommu_group *)(intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x) >= (uintptr_t)-4095)
#define DEFINE_MUTEX(n) int n
#define for_each_set_bit(i,bits,n) for(i=0;i<(n);i++) if ((bits)[i/64] & (1ul<<(i%64)))
static int locked;
static void mutex_lock(int*x) { assert(!*x);*x=1;locked++; }
static void mutex_unlock(int*x) { assert(*x);*x=0;locked--; }
static bool bitmap_empty(unsigned long*b,unsigned n) { for(unsigned i=0;i<n;i++) if(b[i/64]&(1ul<<(i%64)))return false;return true; }
static void bitmap_or(unsigned long*d,unsigned long*a,unsigned long*b,unsigned n) { for(unsigned i=0;i<(n+63)/64;i++)d[i]=a[i]|b[i]; }
struct iommu_group { unsigned refs;void*data;void(*release)(void*); };
struct apple_dart_x1n1 { unsigned num_streams; struct iommu_group*sid2group[256]; };
struct apple_dart_x1n1_master { struct apple_dart_x1n1*dart;unsigned long sids[4]; };
struct device { struct apple_dart_x1n1_master*m;bool pci;struct iommu_group*topology; };
static void*kfree_(void*p) {free(p);return NULL;}
#define kfree(p) kfree_(p)
static void*kmemdup(void*p,size_t n,int flags) { (void)flags;void*q=malloc(n);if(q)memcpy(q,p,n);return q; }
static void*dev_iommu_priv_get(struct device*d) {return d->m;}
static bool dev_is_pci(struct device*d) {return d->pci;}
static struct iommu_group*iommu_group_ref_get(struct iommu_group*g) {assert(g->refs);g->refs++;return g;}
static struct iommu_group*generic_device_group(struct device*d) { (void)d;struct iommu_group*g=calloc(1,sizeof(*g));assert(g);g->refs=1;return g; }
static struct iommu_group*pci_device_group(struct device*d) { return d->topology?iommu_group_ref_get(d->topology):generic_device_group(d); }
static void*iommu_group_get_iommudata(struct iommu_group*g) {return g->data;}
static void iommu_group_set_iommudata(struct iommu_group*g,void*p,void(*r)(void*)) {assert(!g->data);g->data=p;g->release=r;}
static void iommu_group_put(struct iommu_group*g) {assert(!locked);assert(g->refs);if(!--g->refs){if(g->release)g->release(g->data);free(g);}}
"""
pre=pre.replace('struct apple_dart_x1n1_master { struct apple_dart_x1n1*dart;unsigned long sids[4]; };',s[s.index('/* Match the native'):s.index('struct apple_dart_x1n1_domain {')])
pre=pre.replace('#define CONFIG_PCI 1','#define CONFIG_PCI 1\n#define DART_X1N1_MAX_STREAMS 256\n#define DECLARE_BITMAP(n,size) unsigned long n[((size)+63)/64]')
pre=pre.replace('static bool bitmap_empty(unsigned long*b,unsigned n) { for(unsigned i=0;i<n;i++) if(b[i/64]&(1ul<<(i%64)))return false;return true; }\n','')
pre=pre.replace('unsigned long*b,unsigned n) { for(unsigned i=0;i<(n+63)/64;i++)d[i]=a[i]|b[i]; }','const unsigned long*b,unsigned n) { for(unsigned i=0;i<(n+63)/64;i++)d[i]=a[i]|b[i]; }')
helper=s[s.index('static DEFINE_MUTEX(apple_dart_x1n1_groups_lock)'):s.index('static bool apple_dart_x1n1_capable')]
post=r'''
static struct device mkdev(struct apple_dart_x1n1*d,unsigned sid,struct iommu_group*g) {
 struct apple_dart_x1n1_master*m=calloc(1,sizeof(*m));assert(m);m->num_darts=1;m->streams[0].dart=d;m->streams[0].sids[sid/64]|=1ul<<(sid%64);
 return(struct device){m,true,g};
}
static void add(struct device *dev,struct apple_dart_x1n1*d,unsigned sid){unsigned i=dev->m->num_darts++;assert(i<X1N1_MAX_DARTS);dev->m->streams[i].dart=d;dev->m->streams[i].sids[sid/64]|=1ul<<(sid%64);}
int main(void) {
 struct apple_dart_x1n1 darts[2]={{.num_streams=256},{.num_streams=256}};
 struct device a=mkdev(&darts[0],0,NULL);a.pci=false;add(&a,&darts[1],0);
 struct device b=mkdev(&darts[1],0,NULL);b.pci=false;
 struct iommu_group*ga=apple_dart_x1n1_device_group(&a),*gb=apple_dart_x1n1_device_group(&b);
 assert(!IS_ERR(ga)&&ga==gb&&ga->refs==2&&darts[0].sid2group[0]==ga&&darts[1].sid2group[0]==ga);
 iommu_group_put(ga);iommu_group_put(gb);assert(!darts[0].sid2group[0]&&!darts[1].sid2group[0]);free(a.m);free(b.m);
 puts("PASS: multi-DART shared group and union cleanup");
 struct iommu_group*top=generic_device_group(NULL);
 a=mkdev(&darts[0],2,top);b=mkdev(&darts[0],3,top);
 ga=apple_dart_x1n1_device_group(&a);gb=apple_dart_x1n1_device_group(&b);
 assert(ga==top&&gb==top&&darts[0].sid2group[2]==top&&darts[0].sid2group[3]==top);
 iommu_group_put(ga);iommu_group_put(gb);iommu_group_put(top);
 assert(!darts[0].sid2group[2]&&!darts[0].sid2group[3]);free(a.m);free(b.m);
 puts("PASS: single-DART PCI multifunction topology and cleanup");
 a=mkdev(&darts[0],4,NULL);b=mkdev(&darts[1],5,NULL);
 ga=apple_dart_x1n1_device_group(&a);gb=apple_dart_x1n1_device_group(&b);assert(ga!=gb);
 struct device c=mkdev(&darts[0],4,NULL);add(&c,&darts[1],5);
 assert(apple_dart_x1n1_device_group(&c)==ERR_PTR(-EINVAL));
 assert(darts[0].sid2group[4]==ga&&darts[1].sid2group[5]==gb);
 iommu_group_put(ga);iommu_group_put(gb);free(a.m);free(b.m);free(c.m);
 puts("PASS: conflicting live groups on separate DARTs rejected without mutation");
 struct apple_dart_x1n1 nine[9]={0};struct apple_dart_x1n1_master full={.num_darts=8}, extra={.num_darts=1};
 for(unsigned i=0;i<9;i++){nine[i].num_streams=256;if(i<8)full.streams[i].dart=&nine[i];}extra.streams[0].dart=&nine[8];
 struct apple_dart_x1n1_master before=full;assert(x1n1_merge_master(&full,&extra)==-EINVAL&&!memcmp(&before,&full,sizeof(full)));
 puts("PASS: capacity failure leaves group ownership unchanged");
}
'''
out=I/'test-dart-multigroup.c';out.write_text(pre+helper+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(out),'-o',str(I/'test-dart-multigroup')],check=True)
subprocess.run([str(I/'test-dart-multigroup')],check=True)
