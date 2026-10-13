#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Execute actual endpoint startup and early/late/missing DPAV admission paths."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'drivers/gpu/drm/apple/epic/dpavservep.c').read_text()
def function(signature):
    start=source.index(signature);opening=source.index('{',start)
    end,depth=opening+1,1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

prefix=r'''
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef int64_t s64;typedef uint32_t u32;typedef uint64_t u64;
#define U16_MAX UINT16_MAX
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL 2
#define DPAVSERV_ENDPOINT 0x28
#define DPAVSERV_ENDPOINT_V2 0x2a
#define DRM_CONNECTOR_REGISTERED 1
#define to_platform_device(p) (p)
#define trace_dcpavserv_init(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_info_ratelimited(...) ((void)0)
#define dev_err(...) ((void)0)
static unsigned int warnings;
#define dev_warn(...) (warnings++)
static void mutex_lock(bool *p) { assert(!*p);*p=true; }
static void mutex_unlock(bool *p) { assert(*p);*p=false; }
#define lockdep_assert_held(p) assert(*(p))
static unsigned int waits,snapshots,shutdowns,opens;
static void init_completion(bool *p) { *p=false; }
static void reinit_completion(bool *p) { *p=false; }
static void complete(bool *p) { *p=true; }
static unsigned int msecs_to_jiffies(unsigned int n) { return n; }
static int wait_for_completion_timeout(bool *p,unsigned int timeout)
{ assert(timeout==1000);waits++;return *p; }
static void schedule_work(unsigned int *p) { (*p)++; }
struct apple_dcp;
struct apple_dcp_afkep { struct apple_dcp *dcp; u32 endpoint; bool wire_v2,inline_v2,match_epic_name; };
struct apple_epic_service { struct apple_dcp_afkep *ep; bool enabled,torndown; struct { u64 generation,epoch; } admission; unsigned int users,channel; void *cookie; };
struct apple_connector { void *dcp; bool connected,edid_invalidation_pending; struct { int registration_state; } base; unsigned int hotplug_wq; };
struct apple_dcp {
    void *dev; struct { int iomfb_method_profile; } hw;
    bool quiescing,hpd_mutex,typec_cable_connected; u64 typec_generation; unsigned int nr_modes;
    struct apple_connector *connector;
    struct apple_dcp_afkep *dcpavservep;
    struct { bool lock,enabled,enable_completion; u64 epoch; struct apple_epic_service *service; } dcpavserv;
};
static struct apple_epic_service *afk_service_get(struct apple_epic_service *s)
{ if(!s || !s->enabled || s->torndown) return NULL; s->users++;return s; }
static void afk_service_put(struct apple_epic_service *s) { assert(s->users);s->users--; }
static void afk_service_retire(struct apple_epic_service *s) { s->enabled=false;s->torndown=true; }
static const int dpavservep_ops[1]={0};
static struct apple_dcp_afkep ep;
static struct apple_epic_service service;
static int start_error,announce_early;
static bool announce(struct apple_dcp *d,const char *name,const char *class,s64 unit);
static struct apple_dcp_afkep *afk_init(struct apple_dcp *d,u32 endpoint,const void *ops)
{ assert(ops==dpavservep_ops);ep=(struct apple_dcp_afkep){.dcp=d,.endpoint=endpoint};return &ep; }
static int afk_start(struct apple_dcp_afkep *e)
{ assert(e==&ep && e->dcp->dcpavservep==e);if(announce_early) assert(announce(e->dcp,"dcpav-service-epic","AppleDCPDP2HDMI",e->wire_v2?1:0));return start_error; }
static void afk_shutdown(struct apple_dcp_afkep *e) { assert(e==&ep);shutdowns++; }
static void afk_trace_ring_snapshot(struct apple_dcp_afkep *e) { assert(e==&ep);snapshots++; }
'''
parts=[function('static void dcpavserv_notify_edid('),function('static bool dcpavserv_identity('),
       function('static bool dcpavserv_admission_current('),function('static int dcpavserv_prepare('),
       function('static void dcpavserv_init('),function('static void dcpavserv_invalidate_edid('),
       function('static void dcpavserv_teardown('),function('void dpavservep_detach('),
       function('int dpavservep_init(')]
suffix=r'''
static bool announce(struct apple_dcp *d,const char *name,const char *class,s64 unit)
{
    assert(!d->dcpavserv.lock);service=(struct apple_epic_service){.ep=&ep,.channel=1};
    if(dcpavserv_prepare(&service,name,class,unit)) return false;
    if(ep.wire_v2) opens++; /* Standard OPEN after exact prepare, before publish. */
    service.enabled=true;dcpavserv_init(&service,name,class,unit);
    return d->dcpavserv.enabled;
}
static struct apple_dcp d;
static struct apple_connector c;
static int device;
static void initial(bool v2)
{
    if(service.users) dcpavserv_teardown(&service);
    assert(!service.users);
    memset(&d,0,sizeof(d));memset(&c,0,sizeof(c));memset(&service,0,sizeof(service));
    d.dev=&device;d.connector=&c;d.nr_modes=1;
    d.hw.iomfb_method_profile=v2?2:0;c.dcp=&device;c.connected=true;c.base.registration_state=1;
    waits=snapshots=warnings=shutdowns=opens=0;start_error=announce_early=0;
}
int main(void)
{
    initial(true);announce_early=1;
    assert(dpavservep_init(&d)==0 && !waits && !warnings && !snapshots);
    assert(d.dcpavserv.enabled && service.users==1 && d.dcpavserv.epoch==1 && opens==1 && c.hotplug_wq==1);
    assert(ep.endpoint==0x2a && ep.wire_v2 && !ep.inline_v2);
    initial(true); /* Late announcement after successful nonblocking startup. */
    assert(dpavservep_init(&d)==0 && !waits && !d.dcpavserv.enabled && !opens);
    assert(announce(&d,"dcpav-service-epic","AppleDCPDP2HDMI",1));
    assert(service.users==1 && d.dcpavserv.enable_completion && c.hotplug_wq==1 && opens==1);
    initial(true); /* Permanently missing service: no fake admission or timeout. */
    assert(dpavservep_init(&d)==0 && !waits && !warnings && !d.dcpavserv.enabled && !d.dcpavserv.service && !c.hotplug_wq);
    assert(!announce(&d,"dcpav-service-epic","wrong",1));
    assert(!announce(&d,"dcpav-service-epic","AppleDCPDP2HDMI",0));
    assert(!announce(&d,"dcpdp-service-epic","AppleDCPDP2HDMI",3));
    assert(!d.dcpavserv.enabled && !service.users && !opens);
    assert(announce(&d,"dcpav-service-epic","AppleDCPDP2HDMI",1));
    assert(opens==1 && d.dcpavserv.enabled);
    initial(false);announce_early=1;
    assert(dpavservep_init(&d)==0 && waits==1 && !warnings && !opens && ep.endpoint==0x28);
    assert(service.users==1 && d.dcpavserv.enabled && !c.hotplug_wq);
    initial(false);
    assert(dpavservep_init(&d)==-ETIMEDOUT && waits==1 && warnings==1 && snapshots==1 && !d.dcpavserv.enabled);
    initial(true);start_error=-EIO;
    assert(dpavservep_init(&d)==-EIO && shutdowns==1 && !d.dcpavservep && !waits && !service.users);
    initial(true); puts("PASS: actual DPAV init/admission/notify early, late, missing, wrong identity, legacy waits and AFK startup failure");
}
'''
with tempfile.TemporaryDirectory(prefix='dpav-announcement-') as tmp:
    p=Path(tmp)/'test.c';p.write_text(prefix+'\n'.join(parts)+suffix)
    cc=shlex.split(os.environ.get('HOSTCC','clang'))
    subprocess.run(cc+['-std=gnu11','-Wall','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p),'-o',str(Path(tmp)/'test')],check=True)
    subprocess.run([str(Path(tmp)/'test')],check=True)
