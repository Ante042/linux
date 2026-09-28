#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production service-property selection using captured AFKV2 fields."""
from pathlib import Path
import os, subprocess, tempfile
root = next((p for p in Path(__file__).resolve().parents
             if (p / 'drivers/gpu/drm/apple/parser.c').is_file()), None)
if os.environ.get('KERNEL_SRC'):
    root = Path(os.environ['KERNEL_SRC'])
if root is None:
    print('SKIP: set KERNEL_SRC to the Linux source tree')
    raise SystemExit(4)
s = (root/'drivers/gpu/drm/apple/parser.c').read_text()
a = s.index('int parse_epic_service_init(')
b = s.index('\n}', a) + 2
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef int64_t s64;
#define ERR_PTR(e) ((void*)(intptr_t)(e))
#define PTR_ERR(p) ((intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define kfree(p) free((void *)(p))
struct prop {const char *key,*text;s64 value;};
struct dcp_parse_ctx {struct prop *props;unsigned n,idx;bool key_read;};
struct iterator {struct dcp_parse_ctx *handle;};
#define dcp_parse_foreach_in_dict(h,it) for((it).handle=(h),(h)->idx=0; (h)->idx<(h)->n; (h)->idx++,(h)->key_read=false)
static char *parse_string(struct dcp_parse_ctx*h){const char *s=h->key_read?h->props[h->idx].text:h->props[h->idx].key;h->key_read=true;return s?strdup(s):ERR_PTR(-EINVAL);}
static int parse_int(struct dcp_parse_ctx*h,s64*out){*out=h->props[h->idx].value;return 0;}
static void skip(struct dcp_parse_ctx*h){(void)h;}
'''
post = r'''
static void check(struct prop *p,unsigned n,s64 want){struct dcp_parse_ctx c={.props=p,.n=n};const char *name,*class;s64 unit=-1;assert(!parse_epic_service_init(&c,&name,&class,&unit));assert(unit==want);assert(!strcmp(name,"dcpdptx-port-epic"));assert(!strcmp(class,"AppleDCPDPTXRemotePort"));free((void*)name);free((void*)class);}
int main(void){
struct prop p[]={
 {"EPICName","dcpdptx-port-epic",0},
 {"EPICProviderClass","AppleDCPDPTXRemotePort",0},
 {"EPICUnit",NULL,0},{"interface-id",NULL,1}};
check(p,4,0);p[2].value=1;p[3].value=3;check(p,4,1);
struct prop t=p[2];p[2]=p[3];p[3]=t;check(p,4,1);
p[2]=(struct prop){"interface-id",NULL,3};check(p,3,3);
p[0].key="compartment";p[1].key="name";check(p,3,3);
puts("PASS: captured port0/channel1 and port1/channel3 preserve EPICUnit in either property order; H17P fallback retained");
}
'''
with tempfile.TemporaryDirectory(prefix='apple-dcp-service-') as tmp:
    p=Path(tmp)/'test.c';exe=Path(tmp)/'test';p.write_text(pre+s[a:b]+post)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
