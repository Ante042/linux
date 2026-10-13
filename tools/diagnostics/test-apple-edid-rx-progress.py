#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Prove DisplayAttributes cannot block a later AFK reply on ordered RTKit RX."""
from pathlib import Path
import argparse
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ref', help='public git revision for a negative baseline test')
args = parser.parse_args()

def source(path):
    if args.ref:
        return subprocess.check_output(['git', '-C', str(root), 'show', args.ref+':'+path], text=True)
    return (root/path).read_text()

def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    while ';' in text[start:opening]:
        start = text.index(signature, start+1)
        opening = text.index('{', start)
    end, depth = opening+1, 1
    while depth:
        depth += (text[end]=='{')-(text[end]=='}')
        end += 1
    return text[start:end]

dcp=source('drivers/gpu/drm/apple/dcp.c')
iomfb=source('drivers/gpu/drm/apple/iomfb.c')
af=source('drivers/gpu/drm/apple/afk.c')
rtkit=source('drivers/soc/apple/rtkit.c')
template=source('drivers/gpu/drm/apple/iomfb_template.c')
assert 'alloc_ordered_workqueue("rtkit-%s"' in rtkit
assert '.recv_message = dcp_recv_msg' in dcp
assert 'dcp_set_dimensions(dcp);' in template[template.index('"DisplayAttributes"'):]
assert 'queue_work(ep->wq, &work->work)' in af
prefix=r'''
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define GFP_KERNEL 0
#define kzalloc(n,f) calloc(1,n)
#define kfree(p) free(p)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define INIT_WORK(p,f) ((p)->fn=(f))
#define dev_warn(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#define dev_dbg_ratelimited(...) ((void)0)
#define trace_dcp_recv_msg(...) ((void)0)
#define DCP_FIRMWARE_H17P 2
#define IOMFB_ENDPOINT 0x37
#define AV_ENDPOINT 0x29
#define SYSTEM_ENDPOINT 0x20
#define DISP0_ENDPOINT 0x23
#define DPTX_CONTROLLER_ENDPOINT 0x24
#define APPLE_RTKIT_EP_MGMT 0
#define APPLE_RTKIT_EP_CRASHLOG 1
#define APPLE_RTKIT_EP_SYSLOG 2
#define APPLE_RTKIT_EP_IOREPORT 4
#define APPLE_RTKIT_EP_OSLOG 8
#define APPLE_RTKIT_APP_ENDPOINT_START 0x20
struct mutex { pthread_mutex_t native; };
static _Thread_local struct mutex *locks[4];
static _Thread_local unsigned int depth;
static bool held(struct mutex *m) { for(unsigned int i=0;i<depth;i++) if(locks[i]==m) return true; return false; }
static void mutex_lock(struct mutex *m) { assert(!held(m) && depth<4); assert(!pthread_mutex_lock(&m->native)); locks[depth++]=m; }
static void mutex_unlock(struct mutex *m) { assert(depth && locks[depth-1]==m); depth--; assert(!pthread_mutex_unlock(&m->native)); }
#define lockdep_assert_held(p) assert(held(p))
#define spin_lock_irqsave(p,f) do { mutex_lock(p); (f)=0; } while(0)
#define spin_unlock_irqrestore(p,f) do { (void)(f); mutex_unlock(p); } while(0)
struct work_struct { void (*fn)(struct work_struct *); };
struct workqueue_struct { struct work_struct *pending; pthread_cond_t ready; pthread_mutex_t lock; };
struct drm_device { struct { struct mutex mutex; } mode_config; };
struct drm_connector { struct drm_device *dev; struct { int width_mm,height_mm; } display_info; };
struct apple_connector { struct drm_connector base; struct work_struct hotplug_wq; };
struct apple_dcp;
struct apple_dcp_afkep { struct apple_dcp *dcp; u8 endpoint; struct mutex lock; bool stopping; struct workqueue_struct *wq; };
struct apple_dcp {
    bool quiescing; int fw_compat; void *dev;
    int width_mm,height_mm; struct { int width_mm,height_mm; } panel;
    unsigned int nr_modes; struct { struct { int width_mm,height_mm; } mode; } modes[1];
    struct apple_connector *connector;
    struct apple_dcp_afkep *dcpavservep,*dptxep,*avep,*systemep,*ibootep,*dptx_controller_ep;
    struct { struct mutex lock; bool stopped; void *owner; } iomfb;
};
struct afk_receive_message_work { struct apple_dcp_afkep *ep; u64 message; struct work_struct work; };
struct apple_rtkit_ops { void (*recv_message)(void *,u8,u64); };
struct apple_rtkit { bool shutting_down; const struct apple_rtkit_ops *ops; void *cookie,*dev; };
struct apple_rtkit_rx_work { struct apple_rtkit *rtk; u8 ep; u64 msg; struct work_struct work; };
#define current ((void *)1)
static pthread_mutex_t reply_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reply_ready=PTHREAD_COND_INITIALIZER;
static bool reply_done;
static unsigned int advances, dimension_updates;
static struct workqueue_struct dimensions_queue={.ready=PTHREAD_COND_INITIALIZER,.lock=PTHREAD_MUTEX_INITIALIZER};
static struct workqueue_struct afk_queue={.ready=PTHREAD_COND_INITIALIZER,.lock=PTHREAD_MUTEX_INITIALIZER};
static void queue_work(struct workqueue_struct *q,struct work_struct *w)
{ assert(!pthread_mutex_lock(&q->lock)); assert(!q->pending); q->pending=w; pthread_cond_signal(&q->ready); assert(!pthread_mutex_unlock(&q->lock)); }
static void schedule_work(struct work_struct *w) { queue_work(&dimensions_queue,w); }
static void afk_receive_message_worker(struct work_struct *w)
{
    struct afk_receive_message_work *a=container_of(w,struct afk_receive_message_work,work);
    assert(a->ep->endpoint==0x2a && a->message==7);
    assert(!pthread_mutex_lock(&reply_lock)); reply_done=true;
    pthread_cond_signal(&reply_ready); assert(!pthread_mutex_unlock(&reply_lock)); free(a);
}
static bool iomfb_uses_queue(struct apple_dcp *d) { (void)d; return true; }
static void iomfb_queue_advance(struct apple_dcp *d) { assert(held(&d->iomfb.lock)); advances++; }
static void dcp_set_dimensions(struct apple_dcp *d);
static void iomfb_recv_message(struct apple_dcp *d,u64 message)
{ assert(message==1 && held(&d->iomfb.lock)); dcp_set_dimensions(d); }
#define rx_unused(name) static void name(struct apple_rtkit *r,u64 m) { (void)r;(void)m; }
rx_unused(apple_rtkit_management_rx)
rx_unused(apple_rtkit_crashlog_rx)
rx_unused(apple_rtkit_syslog_rx)
rx_unused(apple_rtkit_ioreport_rx)
rx_unused(apple_rtkit_oslog_rx)
'''
parts=[function(dcp,'void dcp_set_dimensions(')]
if 'void dcp_update_connector_dimensions(' in dcp:
    parts.append(function(dcp,'void dcp_update_connector_dimensions('))
else:
    parts.append('static void dcp_update_connector_dimensions(struct apple_dcp *d, struct drm_connector *c) { (void)d; (void)c; }')
parts += [function(iomfb,'void iomfb_recv_msg('),function(af,'int afk_receive_message('),
          function(dcp,'static struct apple_dcp_afkep *dcp_afkep('),
          function(dcp,'static void dcp_recv_msg('),function(rtkit,'static void apple_rtkit_rx_work(')]
suffix=r'''
static struct drm_device drm={.mode_config.mutex.native=PTHREAD_MUTEX_INITIALIZER};
static struct apple_connector connector={.base.dev=&drm};
static struct apple_dcp d={.connector=&connector,.width_mm=600,.height_mm=340,.nr_modes=1,.fw_compat=DCP_FIRMWARE_H17P,.iomfb.lock.native=PTHREAD_MUTEX_INITIALIZER};
static struct apple_dcp_afkep av={.dcp=&d,.endpoint=0x2a,.lock.native=PTHREAD_MUTEX_INITIALIZER,.wq=&afk_queue};
static void dimension_work(struct work_struct *w)
{
    (void)w;
    mutex_lock(&drm.mode_config.mutex);
    dcp_update_connector_dimensions(&d,&connector.base); dimension_updates++;
    mutex_unlock(&drm.mode_config.mutex);
}
static void *worker(void *arg)
{
    struct workqueue_struct *q=arg;
    assert(!pthread_mutex_lock(&q->lock));
    while(!q->pending) assert(!pthread_cond_wait(&q->ready,&q->lock));
    struct work_struct *w=q->pending; q->pending=NULL;
    assert(!pthread_mutex_unlock(&q->lock)); w->fn(w); return NULL;
}
static const struct apple_rtkit_ops ops={.recv_message=dcp_recv_msg};
static struct apple_rtkit rtk={.ops=&ops,.cookie=&d};
static void *ordered_rx(void *arg)
{
    (void)arg;
    /* One ordered RX worker must complete DisplayAttributes before AFK. */
    struct apple_rtkit_rx_work *a=calloc(1,sizeof(*a)); assert(a);
    a->rtk=&rtk; a->ep=IOMFB_ENDPOINT; a->msg=1; apple_rtkit_rx_work(&a->work);
    a=calloc(1,sizeof(*a)); assert(a);
    a->rtk=&rtk; a->ep=0x2a; a->msg=7; apple_rtkit_rx_work(&a->work);
    return NULL;
}
int main(void)
{
    pthread_t rx,afk,dimensions;
    (void)schedule_work; /* The negative baseline does not defer dimensions. */
    d.dcpavservep=&av; connector.hotplug_wq.fn=dimension_work;
    mutex_lock(&drm.mode_config.mutex); /* DRM get_modes holds this while waiting. */
    assert(!pthread_create(&afk,NULL,worker,&afk_queue));
    assert(!pthread_create(&dimensions,NULL,worker,&dimensions_queue));
    assert(!pthread_mutex_lock(&reply_lock));
    assert(!pthread_create(&rx,NULL,ordered_rx,NULL));
    struct timespec deadline; assert(!clock_gettime(CLOCK_REALTIME,&deadline)); deadline.tv_sec+=2;
    int rc=0;
    while(!reply_done && !rc) rc=pthread_cond_timedwait(&reply_ready,&reply_lock,&deadline);
    if(!reply_done) { fputs("FAIL: ordered RTKit RX blocked before AFK reply while mode_config.mutex held\n",stderr); fflush(stderr); _Exit(1); }
    assert(!pthread_mutex_unlock(&reply_lock));
    assert(!dimension_updates); /* Deferred worker cannot take the probe's mutex. */
    mutex_unlock(&drm.mode_config.mutex);
    assert(!pthread_join(rx,NULL) && !pthread_join(afk,NULL) && !pthread_join(dimensions,NULL));
    assert(advances==1 && dimension_updates==1 && connector.base.display_info.width_mm==600 && connector.base.display_info.height_mm==340);
    assert(d.modes[0].mode.width_mm==600 && d.modes[0].mode.height_mm==340);
    puts("PASS: actual ordered RTKit RX -> synchronous IOMFB dimensions callback -> AFK enqueue makes reply progress while DRM mutex held; deferred dimensions update completes after release");
}
'''
with tempfile.TemporaryDirectory(prefix='apple-edid-rx-') as tmp:
    test=Path(tmp)/'test.c'; test.write_text(prefix+'\n'.join(parts)+suffix)
    cc=shlex.split(os.environ.get('HOSTCC','clang'))
    subprocess.run(cc+['-std=gnu11','-Wall','-Werror','-O1','-g','-pthread',
                       '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(test),'-o',str(Path(tmp)/'test')],check=True)
    subprocess.run([str(Path(tmp)/'test')],check=True)
