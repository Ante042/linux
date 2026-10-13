#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise production DPAV routing, admission and in-flight EDID ownership."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
base = root / 'drivers/gpu/drm/apple'

def function(file, signature):
    source = (base / file).read_text()
    start = source.index(signature)
    opening = source.index('{', start)
    while ';' in source[start:opening]:
        start = source.index(signature, start + 1)
        opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

av = (base / 'epic/dpavservep.c').read_text()
af = (base / 'afk.c').read_text()
announce = af[af.index('static void afk_recv_handle_init('):af.index('static void afk_recv_handle_init(') + 6500]
assert announce.index('ops->prepare(service,') < announce.index('EPIC_CAT_REPORT, AFKV2_OPEN')
assert '.prepare = dcpavserv_prepare,' in av and '.prepare = dcpdpserv_prepare,' in av
prefix = r'''
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint64_t u64;
typedef int64_t s64;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define PTR_ERR(x) ((int)(intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x) > (uintptr_t)-4096)
#define spin_lock_irqsave(p,f) do { assert(!*(p)); *(p)=true; (f)=0; } while(0)
#define spin_unlock_irqrestore(p,f) do { (void)(f); assert(*(p)); *(p)=false; } while(0)
#define lockdep_assert_held(p) assert(*(p))
static bool *acquire(bool *p) { assert(!*p); *p=true; return p; }
static void release(bool **p) { if(*p) { assert(**p); **p=false; } }
static bool *once(bool *p) { assert(*p); *p=false; return NULL; }
#define scoped_guard(k,p) for(bool *_lock __attribute__((cleanup(release)))=acquire(p); _lock; _lock=once(_lock))
static void mutex_lock(bool *p) { (void)acquire(p); }
static void mutex_unlock(bool *p) { (void)once(p); }
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define trace_dcpavserv_init(...) ((void)0)
#define DRM_CONNECTOR_REGISTERED 1
#define to_platform_device(x) (x)
static void reinit_completion(int *p) { *p=0; }
static void complete(int *p) { *p=1; }
static void schedule_work(int *p) { (*p)++; }
#define dev_dbg_ratelimited(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#define trace_dcp_recv_msg(...) ((void)0)
#define DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL 2
#define DCP_FIRMWARE_H17P 2
#define AV_ENDPOINT 0x29
#define SYSTEM_ENDPOINT 0x20
#define DISP0_ENDPOINT 0x23
#define DPAVSERV_ENDPOINT 0x28
#define DPAVSERV_ENDPOINT_V2 0x2a
#define DPTX_ENDPOINT 0x2a
#define DPTX_ENDPOINT_V2 0x2c
#define DPTX_CONTROLLER_ENDPOINT 0x24
#define IOMFB_ENDPOINT 0x37
struct apple_dcp;
struct apple_connector { void *dcp; struct { int registration_state; } base; bool connected, edid_invalidation_pending; int hotplug_wq; };
struct apple_dcp_afkep { struct apple_dcp *dcp; u8 endpoint; bool wire_v2; };
struct apple_epic_service { struct apple_dcp_afkep *ep; unsigned int users; bool enabled; void *cookie; unsigned int channel; };
struct apple_dcp {
    struct apple_dcp_afkep *dcpavservep, *dptxep, *avep, *systemep, *ibootep, *dptx_controller_ep;
    struct { bool lock, enabled; u64 epoch; int enable_completion; struct apple_epic_service *service; } dcpavserv;
    struct apple_connector *connector; unsigned int nr_modes;
    struct { int iomfb_method_profile; } hw;
    bool hpd_mutex, typec_cable_connected, quiescing;
    u64 typec_generation;
    int fw_compat;
    void *dev;
};
struct drm_edid { int identity; };
static unsigned int reads, allocations, legacy_reads, receives;
static unsigned int event;
static struct apple_dcp_afkep *received_ep;
static struct apple_epic_service *afk_service_get(struct apple_epic_service *s)
{ if(!s || !s->enabled) return NULL; s->users++; return s; }
static void afk_service_put(struct apple_epic_service *s)
{ assert(s->users); s->users--; }
static void drm_edid_free(const struct drm_edid *e)
{ if(e) { assert(allocations); allocations--; free((void *)e); } }
static void dcpavserv_teardown(struct apple_epic_service *s);
static const struct drm_edid *dcpavserv_read_edid_inline(struct apple_epic_service *s)
{
    struct apple_dcp *d=s->ep->dcp;
    assert(!d->hpd_mutex && !d->dcpavserv.lock && s->users==2);
    reads++;
    if(event==1 || event==2) { d->typec_generation++; d->typec_cable_connected=(event==2); }
    if(event==3) { s->enabled=false; dcpavserv_teardown(s); }
    struct drm_edid *e=malloc(sizeof(*e)); assert(e); e->identity=42; allocations++; return e;
}
static const struct drm_edid *dcpavserv_read_edid_legacy(struct apple_epic_service *s)
{ legacy_reads++; return dcpavserv_read_edid_inline(s); }
static void iomfb_recv_msg(struct apple_dcp *d, u64 message) { (void)d; (void)message; }
static void afk_receive_message(struct apple_dcp_afkep *ep, u64 message)
{ (void)message; received_ep=ep; receives++; }
'''
parts = [function('epic/dpavservep.c', 'static void dcpavserv_notify_edid('),
         function('epic/dpavservep.c', 'static void dcpavserv_invalidate_edid('),
         function('epic/dpavservep.c', 'static void dcpavserv_teardown('),
         function('dcp.c', 'static struct apple_dcp_afkep *dcp_afkep('),
         function('dcp.c', 'static void dcp_recv_msg('),
         function('epic/dpavservep.c', 'static int dcpavserv_prepare('),
         function('epic/dpavservep.c', 'static int dcpdpserv_prepare('),
         function('epic/dpavservep.c', 'static void dcpavserv_init('),
         function('epic/dpavservep.c', 'bool dcpavserv_edid_current('),
         function('epic/dpavservep.c', 'bool dcpavserv_edid_service_current('),
         function('epic/dpavservep.c', 'const struct drm_edid *dcpavserv_copy_edid(')]
suffix = r'''
int main(void)
{
    u64 epoch=0;
    int device;
    struct apple_connector connector={.dcp=&device,.base.registration_state=DRM_CONNECTOR_REGISTERED,.connected=true};
    struct apple_dcp d={.fw_compat=DCP_FIRMWARE_H17P,.dev=&device,.connector=&connector,.nr_modes=1};
    struct apple_dcp_afkep av={.dcp=&d,.endpoint=0x28}, tx={.dcp=&d,.endpoint=0x2a};
    d.dcpavservep=&av; d.dptxep=&tx;
    assert(dcp_afkep(&d,0x28)==&av && dcp_afkep(&d,0x2a)==&tx && !dcp_afkep(&d,0x2c));
    dcp_recv_msg(&d,0x28,1); assert(received_ep==&av && receives==1);
    dcp_recv_msg(&d,0x2a,2); assert(received_ep==&tx && receives==2);
    d.hw.iomfb_method_profile=DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL;
    av.endpoint=0x2a; av.wire_v2=true; tx.endpoint=0x2c;
    assert(!dcp_afkep(&d,0x28) && dcp_afkep(&d,0x2a)==&av && dcp_afkep(&d,0x2c)==&tx);
    dcp_recv_msg(&d,0x28,3); assert(receives==2);
    dcp_recv_msg(&d,0x2a,4); assert(received_ep==&av && receives==3);
    dcp_recv_msg(&d,0x2c,5); assert(received_ep==&tx && receives==4);
    d.dcpavservep=NULL; dcp_recv_msg(&d,0x2a,6); assert(receives==4); d.dcpavservep=&av;
    struct apple_epic_service s={.ep=&av,.users=1,.enabled=true};
    const char *name="dcpav-service-epic", *class="AppleDCPDP2HDMI";
    assert(!dcpavserv_prepare(&s,name,class,1));
    assert(dcpavserv_prepare(&s,name,class,0)==-ENODEV);
    assert(dcpavserv_prepare(&s,name,class,3)==-ENODEV);
    assert(dcpavserv_prepare(&s,name,class,-1)==-ENODEV);
    assert(dcpavserv_prepare(&s,"dcpdp-service-epic",class,1)==-ENODEV);
    assert(dcpavserv_prepare(&s,name,"wrong",1)==-ENODEV);
    assert(dcpavserv_prepare(&s,NULL,class,1)==-ENODEV);
    assert(dcpavserv_prepare(&s,name,NULL,1)==-ENODEV);
    av.endpoint=0x28; assert(dcpavserv_prepare(&s,name,class,1)==-ENODEV); av.endpoint=0x2a;
    assert(dcpdpserv_prepare(&s,"dcpdp-service-epic",class,3)==-ENODEV);
    d.dcpavserv.enabled=true; assert(dcpavserv_prepare(&s,name,class,1)==-EBUSY);
    d.dcpavserv.service=&s; d.dcpavserv.epoch=1; d.typec_generation=1; d.typec_cable_connected=true;
    const struct drm_edid *e=dcpavserv_copy_edid(&d,1,&epoch);
    assert(!IS_ERR(e) && s.users==1 && reads==1); drm_edid_free(e);
    d.typec_cable_connected=false;
    assert(PTR_ERR(dcpavserv_copy_edid(&d,1,&epoch))==-ESTALE && reads==1);
    d.typec_cable_connected=true;
    assert(PTR_ERR(dcpavserv_copy_edid(&d,0,&epoch))==-ESTALE && reads==1);
    event=1; assert(PTR_ERR(dcpavserv_copy_edid(&d,1,&epoch))==-ESTALE && !allocations && s.users==1);
    event=2; d.typec_cable_connected=true;
    assert(PTR_ERR(dcpavserv_copy_edid(&d,2,&epoch))==-ESTALE && !allocations && s.users==1);
    event=0; e=dcpavserv_copy_edid(&d,3,&epoch); assert(!IS_ERR(e)); drm_edid_free(e);
    event=3; assert(PTR_ERR(dcpavserv_copy_edid(&d,3,&epoch))==-ENODEV && !allocations && !s.users);
    assert(PTR_ERR(dcpavserv_copy_edid(&d,3,&epoch))==-ENODEV);
    assert(connector.edid_invalidation_pending && connector.hotplug_wq && d.dcpavserv.epoch==2);
    /* Same slot and physical generation: CLOSE/re-OPEN gets a fresh ticket. */
    s.enabled=true; dcpavserv_init(&s,name,class,1);
    assert(d.dcpavserv.service==&s && s.users==1 && d.dcpavserv.epoch==3);
    mutex_lock(&d.dcpavserv.lock);
    assert(!dcpavserv_edid_service_current(&d,1) && dcpavserv_edid_service_current(&d,3));
    mutex_unlock(&d.dcpavserv.lock);
    event=0; e=dcpavserv_copy_edid(&d,3,&epoch); assert(!IS_ERR(e) && epoch==3);
    /* Simulate retirement in the final validation-to-publication gap. */
    s.enabled=false; dcpavserv_teardown(&s); s.enabled=true; dcpavserv_init(&s,name,class,1);
    mutex_lock(&d.dcpavserv.lock); assert(!dcpavserv_edid_service_current(&d,epoch)); mutex_unlock(&d.dcpavserv.lock);
    drm_edid_free(e); assert(d.typec_generation==3 && d.dcpavserv.epoch==5 && s.users==1);
    s.enabled=false; dcpavserv_teardown(&s);
    av.wire_v2=false; av.endpoint=0x28; d.hw.iomfb_method_profile=0;
    d.dcpavserv.enabled=false; assert(!dcpavserv_prepare(&s,name,NULL,0));
    assert(dcpavserv_prepare(&s,name,class,1)==-ENODEV);
    assert(!dcpdpserv_prepare(&s,NULL,NULL,3));
    d.dcpavserv.service=&s; d.dcpavserv.enabled=true; s.enabled=true; s.users=1; event=0;
    d.typec_cable_connected=false; e=dcpavserv_copy_edid(&d,0,&epoch);
    assert(!IS_ERR(e) && legacy_reads==1 && s.users==1); drm_edid_free(e);
    assert(!allocations && !d.hpd_mutex && !d.dcpavserv.lock);
    puts("PASS: production old/new lookup and receive, exact service admission before OPEN, delayed disconnect/replug, service detach pins, legacy transport");
}
'''
with tempfile.TemporaryDirectory(prefix='apple-h17p-edid-') as tmp:
    source=Path(tmp)/'test.c'
    source.write_text(prefix+'\n'.join(parts)+suffix)
    cc=shlex.split(os.environ.get('HOSTCC','clang'))
    subprocess.run(cc+['-std=gnu11','-Wall','-Werror',
                       '-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                       str(source),'-o',str(Path(tmp)/'test')],check=True)
    subprocess.run([str(Path(tmp)/'test')],check=True)
