#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Test the diagnostic parameter and actual copy gate, plus bounded trace capture."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
av=(root/'drivers/gpu/drm/apple/epic/dpavservep.c').read_text()
trace=(root/'drivers/gpu/drm/apple/trace.h').read_text()

def function(source,signature):
    start=source.index(signature); opening=source.index('{',start)
    while ';' in source[start:opening]:
        start=source.index(signature,start+1); opening=source.index('{',start)
    end,depth=opening+1,1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

assert 'static atomic_t diagnostic_h17p_edid_probe = ATOMIC_INIT(0);' in av
assert re.search(r'module_param_cb\(diagnostic_h17p_edid_probe,\s*&diagnostic_h17p_edid_probe_ops,\s*NULL, 0600\)',av)
# Verify the parameter only changes permission; existing service OPEN is untouched.
setter=function(av,'static int diagnostic_h17p_edid_probe_set(')
assert 'afk_' not in setter and 'dcpavserv_copy_edid' not in setter
prefix=r'''
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint64_t u64;
typedef _Atomic int atomic_t;
#define ATOMIC_INIT(x) (x)
#define atomic_set(p,x) atomic_store(p,x)
#define atomic_read(p) atomic_load(p)
#define atomic_xchg(p,x) atomic_exchange(p,x)
#define READ_ONCE(x) (x)
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define PTR_ERR(x) ((int)(intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x)>(uintptr_t)-4096)
#define DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL 2
#define lockdep_assert_held(p) assert(*(p))
static bool *acquire(bool *p) { assert(!*p); *p=true; return p; }
static void release(bool **p) { if(*p) { assert(**p); **p=false; } }
static bool *once(bool *p) { assert(*p); *p=false; return NULL; }
static void mutex_lock(bool *p) { (void)acquire(p); }
static void mutex_unlock(bool *p) { (void)once(p); }
#define scoped_guard(k,p) for(bool *_lock __attribute__((cleanup(release)))=acquire(p); _lock; _lock=once(_lock))
struct kernel_param { void *arg; };
static int kstrtobool(const char *p,bool *v)
{ if(!strcmp(p,"1") || !strcmp(p,"Y")) { *v=true; return 0; } if(!strcmp(p,"0") || !strcmp(p,"N")) { *v=false; return 0; } return -EINVAL; }
static int param_get_bool(char *p,const struct kernel_param *k)
{ return sprintf(p,"%c\n",*(bool *)k->arg ? 'Y':'N'); }
struct apple_dcp;
struct ep { struct apple_dcp *dcp; bool inline_v2; };
struct apple_epic_service { struct ep *ep; bool enabled; unsigned int users; };
struct apple_dcp {
    struct { int iomfb_method_profile; } hw;
    bool hpd_mutex,typec_cable_connected,quiescing;
    u64 typec_generation;
    struct { bool lock,enabled; u64 epoch; struct apple_epic_service *service; } dcpavserv;
};
struct drm_edid { int identity; };
static unsigned int reads,legacy_reads,allocations;
static int read_error;
static struct apple_epic_service *afk_service_get(struct apple_epic_service *s)
{ if(!s || !s->enabled) return NULL; s->users++; return s; }
static void afk_service_put(struct apple_epic_service *s) { assert(s->users); s->users--; }
static void drm_edid_free(const struct drm_edid *e) { if(e) { assert(allocations); allocations--; free((void *)e); } }
static const struct drm_edid *dcpavserv_read_edid_inline(struct apple_epic_service *s)
{
    assert(s->users==2 && !s->ep->dcp->hpd_mutex && !s->ep->dcp->dcpavserv.lock);
    reads++; if(read_error) return ERR_PTR(read_error);
    struct drm_edid *e=malloc(sizeof(*e)); assert(e); e->identity=42; allocations++; return e;
}
static const struct drm_edid *dcpavserv_read_edid_legacy(struct apple_epic_service *s)
{ legacy_reads++; return dcpavserv_read_edid_inline(s); }
static atomic_t diagnostic_h17p_edid_probe=ATOMIC_INIT(0);
'''
parts=[setter,function(av,'static int diagnostic_h17p_edid_probe_get('),
       function(av,'static bool dcpavserv_edid_probe_allowed('),
       function(av,'bool dcpavserv_edid_current('),function(av,'bool dcpavserv_edid_service_current('),
       function(av,'const struct drm_edid *dcpavserv_copy_edid(')]
suffix=r'''
static struct apple_dcp d={.hw.iomfb_method_profile=2,.typec_cable_connected=true,.typec_generation=1,.dcpavserv.enabled=true,.dcpavserv.epoch=1};
static atomic_t winners=ATOMIC_INIT(0);
static void *contender(void *unused) { (void)unused; if(dcpavserv_edid_probe_allowed(&d)) atomic_fetch_add(&winners,1); return NULL; }
int main(void)
{
    struct ep ep={.dcp=&d,.inline_v2=true}; struct apple_epic_service s={.ep=&ep,.enabled=true,.users=1};
    struct kernel_param parameter={0}; char state[4]; u64 epoch=0;
    d.dcpavserv.service=&s;
    assert(diagnostic_h17p_edid_probe_get(state,&parameter)==2 && !strcmp(state,"N\n"));
    assert(PTR_ERR(dcpavserv_copy_edid(&d,1,&epoch))==-EAGAIN && !reads);
    assert(!diagnostic_h17p_edid_probe_set("1",&parameter));
    const struct drm_edid *e=dcpavserv_copy_edid(&d,1,&epoch);
    assert(!IS_ERR(e) && reads==1 && epoch==1 && s.users==1); drm_edid_free(e);
    assert(PTR_ERR(dcpavserv_copy_edid(&d,1,&epoch))==-EAGAIN && reads==1);
    assert(diagnostic_h17p_edid_probe_get(state,&parameter)==2 && !strcmp(state,"N\n"));
    for(unsigned int i=0;i<2;i++) {
        read_error=i ? -ETIMEDOUT : -EIO;
        assert(!diagnostic_h17p_edid_probe_set("Y",&parameter));
        assert(PTR_ERR(dcpavserv_copy_edid(&d,1,&epoch))==read_error);
        assert(PTR_ERR(dcpavserv_copy_edid(&d,1,&epoch))==-EAGAIN);
        assert(!atomic_read(&diagnostic_h17p_edid_probe) && s.users==1);
    }
    assert(reads==3 && !allocations); read_error=0;
    assert(!diagnostic_h17p_edid_probe_set("1",&parameter));
    d.dcpavserv.enabled=false;
    assert(PTR_ERR(dcpavserv_copy_edid(&d,1,&epoch))==-ENODEV);
    assert(PTR_ERR(dcpavserv_copy_edid(&d,1,&epoch))==-EAGAIN && reads==3);
    d.dcpavserv.enabled=true; d.typec_cable_connected=false;
    assert(!diagnostic_h17p_edid_probe_set("1",&parameter));
    assert(PTR_ERR(dcpavserv_copy_edid(&d,1,&epoch))==-ESTALE);
    assert(!atomic_read(&diagnostic_h17p_edid_probe) && reads==3); d.typec_cable_connected=true;
    assert(!diagnostic_h17p_edid_probe_set("1",&parameter));
    assert(diagnostic_h17p_edid_probe_set("invalid",&parameter)==-EINVAL);
    assert(atomic_read(&diagnostic_h17p_edid_probe));
    assert(!diagnostic_h17p_edid_probe_set("0",&parameter));
    assert(PTR_ERR(dcpavserv_copy_edid(&d,1,&epoch))==-EAGAIN && reads==3);
    /* Other profiles neither require nor consume this H17P permission. */
    d.hw.iomfb_method_profile=0; ep.inline_v2=false;
    e=dcpavserv_copy_edid(&d,0,&epoch); assert(!IS_ERR(e) && legacy_reads==1); drm_edid_free(e);
    assert(!diagnostic_h17p_edid_probe_set("1",&parameter));
    d.hw.iomfb_method_profile=3;
    e=dcpavserv_copy_edid(&d,0,&epoch); assert(!IS_ERR(e) && legacy_reads==2); drm_edid_free(e);
    assert(atomic_read(&diagnostic_h17p_edid_probe));
    d.hw.iomfb_method_profile=2;
    pthread_t threads[8];
    for(unsigned int i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,contender,NULL));
    for(unsigned int i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
    assert(atomic_read(&winners)==1 && !atomic_read(&diagnostic_h17p_edid_probe));
    assert(!allocations && s.users==1);
    puts("PASS: actual diagnostic bool setter/getter and copy gate: default no read, one read per arm, EIO/timeout/missing-service/stale failures consumed, invalid writes, foreign profiles unchanged, one atomic winner");
}
'''
# Compile the actual trace assignment with its declared array/count types.
event=trace[trace.index('TRACE_EVENT(afk_recv_header,'):trace.index('TRACE_EVENT(afk_ring_snapshot,')]
count=re.search(r'__field\((\w+), captured\)',event).group(1)
capacity=int(re.search(r'__array\(u8, header, (\d+)\)',event).group(1))
assert capacity==256 and count=='u16'
start=event.index('TP_fast_assign(')+len('TP_fast_assign(');end,depth=start,1
while depth:
    depth+=(event[end]=='(')-(event[end]==')');end+=1
assignment=event[start:end-1]
trace_test=r'''
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define __assign_str(x) ((void)0)
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
struct ep { u8 endpoint; };
'''+f'struct entry {{ u8 endpoint; u32 channel,type; size_t data_size; {count} captured; u8 header[{capacity}]; }};\n'+r'''
static void capture(struct entry *__entry,struct ep *ep,u32 channel,u32 type,size_t data_size,const void *data)
{
'''+assignment+r'''
}
int main(void)
{
    struct entry e; struct ep ep={0x2a};
    u8 *data=malloc(144); assert(data); for(unsigned int i=0;i<144;i++) data[i]=(u8)i;
    capture(&e,&ep,1,0xc0,144,data);
    assert(e.captured==144 && e.data_size==144 && e.endpoint==0x2a && !memcmp(e.header,data,144));
    for(unsigned int i=144;i<256;i++) assert(e.header[i]==0); free(data);
    data=malloc(256); assert(data); memset(data,0xa5,256);
    capture(&e,&ep,1,0xc0,SIZE_MAX,data); assert(e.captured==256 && e.header[255]==0xa5); free(data);
    capture(&e,&ep,1,0,1000,NULL); assert(!e.captured);
    for(unsigned int i=0;i<256;i++) assert(e.header[i]==0);
    capture(&e,&ep,1,0,0,NULL); assert(!e.captured);
    puts("PASS: actual opt-in trace assignment captures complete 144 bytes and clamps at 256 without u8 wrap, null reads or overrun");
}
'''
with tempfile.TemporaryDirectory(prefix='apple-edid-once-') as tmp:
    cc=shlex.split(os.environ.get('HOSTCC','clang'))
    for name,text in [('once',prefix+'\n'.join(parts)+suffix),('trace',trace_test)]:
        path=Path(tmp)/name; path.with_suffix('.c').write_text(text)
        subprocess.run(cc+['-std=gnu11','-Wall','-Werror','-O1','-g','-pthread',
                           '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(path.with_suffix('.c')),'-o',str(path)],check=True)
        subprocess.run([str(path)],check=True)
