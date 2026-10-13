#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual metadata epoch/cache and connector raw-EDID precedence paths."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
base = root / 'drivers/gpu/drm/apple'
dcp = (base / 'dcp.c').read_text()
iomfb = (base / 'iomfb.c').read_text()


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    while ';' in text[start:opening]:
        start = text.index(signature, start + 1)
        opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


metadata = function((base / 'dcp-display-metadata.h').read_text(), 'struct dcp_display_metadata {') + ';'
epoch = (base / 'dcp-metadata-epoch.h').read_text().replace('#include <linux/types.h>', '')
cache = dcp[dcp.index('static bool dcp_firmware_metadata_enabled('):dcp.index('void dcp_set_dimensions(')]
getter = function(iomfb, 'int dcp_get_modes(')
physical_current = function((base / 'epic/dpavservep.c').read_text(), 'bool dcpavserv_edid_current(')
service_current = function((base / 'epic/dpavservep.c').read_text(), 'bool dcpavserv_edid_service_current(')
cleanup = function(iomfb, 'static void dcp_clear_connector_edid(') + '\n' + function(iomfb, 'static void dcp_consume_edid_invalidation(')
hotplug = function(iomfb, 'void dcp_hotplug(')
disconnect = function(dcp, 'static void disconnected_hpd_event(')
chunk_callbacks = function((base / 'iomfb_template.c').read_text(), 'static u8 dcpep_cb_prop_start(') + '\n' + function((base / 'iomfb_template.c').read_text(), 'static u8 dcpep_cb_prop_chunk(')
# Check the actual ingress/egress hooks as well as executing the shared helpers.
assert dcp.count('dcp_firmware_metadata_connection(dcp, dcp->typec_generation, true)') == 2
assert dcp.count('dcp_firmware_metadata_connection(dcp, dcp->typec_generation, false)') == 3
assert dcp.count('dcp_firmware_metadata_connection(dcp, READ_ONCE(dcp->typec_generation), false)') == 2
template = (base / 'iomfb_template.c').read_text()
assert 'dcp_firmware_metadata_begin(dcp);' in template
assert 'dcp_firmware_metadata_hotplug(dcp, !!(*connected));' in template
assert 'dcp_firmware_metadata_attributes(dcp, ret ? NULL : &metadata);' in template
assert 'dcp->chunks.received += req->length;' in template
assert 'dcp_property_complete(dcp->chunks.data, dcp->chunks.received,' in template
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1u << (n))
static unsigned int trace_observations;
#define trace_iomfb_metadata(d,p,g,t,f,r,l) do { \
    (void)(d); (void)(p); (void)(g); (void)(t); (void)(f); (void)(r); (void)(l); \
    trace_observations++; \
} while (0)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define xchg(p,v) __atomic_exchange_n(p,v,__ATOMIC_SEQ_CST)
#define lockdep_assert_held(p) assert(mutex_held(p))
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL 2
#define DCP_FIRMWARE_H17P 2
#define DRM_MODE_TYPE_PREFERRED 2
#define DCP_FW_VER 26
#define DCP_FW_VERSION(a,b,c) (a)
#define SZ_1M (1024*1024)
#define GFP_KERNEL 0
#define kzalloc(n,f) calloc(1,n)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
typedef struct { bool held; } spinlock_t;
#define spin_lock_irqsave(p, flags) do { assert(!(p)->held); (p)->held=true; (flags)=0; } while (0)
#define spin_unlock_irqrestore(p, flags) do { (void)(flags); assert((p)->held); (p)->held=false; } while (0)
struct drm_display_mode { unsigned int type; int clock; };
struct dcp_display_mode { struct drm_display_mode mode; bool vrr, metadata_timing_valid; u32 min_vrr, max_vrr; };
struct work_struct { unsigned int scheduled; };
struct mutex { pthread_mutex_t native; };
static _Thread_local struct mutex *held_mutex;
static _Thread_local bool modeset_held;
static _Thread_local struct mutex *held_stack[4];
static _Thread_local unsigned int held_depth;
static bool mutex_held(struct mutex *m) { for(unsigned int i=0;i<held_depth;i++) if(held_stack[i]==m) return true; return false; }
static void mutex_lock(struct mutex *m) { assert(!modeset_held); assert(!mutex_held(m)); assert(held_depth<4); assert(!pthread_mutex_lock(&m->native)); held_stack[held_depth++]=m; held_mutex=m; }
static void mutex_unlock(struct mutex *m) { assert(held_mutex==m); assert(held_depth); held_depth--; held_mutex=held_depth ? held_stack[held_depth-1] : NULL; assert(!pthread_mutex_unlock(&m->native)); }
static struct mutex *acquire(struct mutex *m) { mutex_lock(m); return m; }
static void release(struct mutex **m) { if (*m) mutex_unlock(*m); }
static struct mutex *once(struct mutex *m) { mutex_unlock(m); return NULL; }
#define scoped_guard(kind,value) for(struct mutex *g __attribute__((cleanup(release)))=acquire(value); g; g=once(g))
struct test_blob { unsigned int identity, references, magic; };
struct drm_device { void *dev; struct { struct mutex mutex; } mode_config; };
struct drm_connector { struct drm_device *dev; void *state; struct test_blob *edid_blob_ptr; struct { struct { unsigned int min_vfreq, max_vfreq; } monitor_range; } display_info; };
struct dcp_set_dcpav_prop_chunk_req { u8 data[4096]; u32 offset, length; };
struct apple_dcp;
struct platform_device { struct apple_dcp *data; };
struct drm_edid { unsigned int identity; };
struct apple_connector {
    struct drm_connector base;
    struct platform_device *dcp;
    const struct drm_edid *drm_edid;
    bool drm_edid_firmware_derived;
    bool edid_invalidation_pending;
    u64 drm_edid_generation, drm_edid_service_epoch;
    bool connected;
    struct work_struct hotplug_wq;
};
#define to_apple_connector(p) container_of(p, struct apple_connector, base)
#define platform_get_drvdata(p) ((p)->data)
'''
state = r'''
struct apple_dcp {
    void *dev;
    struct mutex hpd_mutex;
    struct { struct mutex lock; bool enabled; void *service; u64 epoch; } dcpavserv;
    bool typec_cable_connected, quiescing;
    u64 typec_generation;
    struct { int iomfb_method_profile; } hw;
    struct apple_connector *connector;
    struct { void *data; size_t length, received; bool contiguous, payload_complete; struct dcp_metadata_ticket metadata_ticket; } chunks;
    struct {
        spinlock_t lock;
        struct dcp_metadata_epoch epoch;
        struct dcp_display_metadata attributes;
        struct drm_display_mode preferred;
        struct dcp_metadata_ticket attributes_ticket, timing_ticket;
        bool attributes_valid, timing_valid, hpd_seen;
    } metadata;
    struct dcp_display_mode *modes;
    unsigned int nr_modes;
    int fw_compat;
    struct { bool valid; } mode_state;
    struct { bool inline_v2; } *dcpavservep;
};
static struct apple_dcp *current_for_update;
static unsigned int allocations, raw_reads, mode_adds, clears, property_identity, blob_allocations, notifications, retrains;
static bool invalidate_during_update;
static bool update_error;
static bool raw_available;
static bool detach_during_read, replug_during_read;
static bool retire_after_read;
static const struct drm_edid *allocate(unsigned int identity)
{
    struct drm_edid *edid = malloc(sizeof(*edid)); assert(edid);
    edid->identity = identity; allocations++; return edid;
}
static void drm_edid_free(const struct drm_edid *edid)
{ if (edid) { assert(allocations); allocations--; free((void *)edid); } }
static const struct drm_edid *dcp_metadata_edid(const struct dcp_display_metadata *m,
                                              const struct drm_display_mode *mode)
{ assert(mode->clock); return allocate(m->serial); }
static void schedule_work(struct work_struct *w) { w->scheduled++; }
static struct drm_display_mode *drm_mode_duplicate(struct drm_device *d, const struct drm_display_mode *m)
{ (void)d; struct drm_display_mode *copy=malloc(sizeof(*copy)); *copy=*m; return copy; }
static void drm_mode_probed_add(struct drm_connector *c, struct drm_display_mode *m)
{ (void)c; mode_adds++; free(m); }
static void drm_connector_set_vrr_capable_property(struct drm_connector *c, bool v)
{ (void)c; (void)v; }
static bool dcp_has_panel(struct apple_dcp *p) { (void)p; return false; }
static const struct drm_edid *dcpavserv_copy_edid(struct apple_dcp *p, u64 generation, u64 *epoch)
{
    assert(!mutex_held(&p->hpd_mutex) && !mutex_held(&p->dcpavserv.lock));
    *epoch=p->dcpavserv.epoch;
    if (retire_after_read) { p->dcpavserv.epoch+=2; p->connector->edid_invalidation_pending=true; }
    (void)generation; raw_reads++;
    if (detach_during_read) {
        mutex_lock(&p->hpd_mutex); p->typec_generation++;
        p->typec_cable_connected=replug_during_read;
        mutex_unlock(&p->hpd_mutex);
    }
    return raw_available ? allocate(0xfeed) : ERR_PTR(-ENODEV);
}
static int drm_edid_connector_update(struct drm_connector *c, const struct drm_edid *e)
{
    assert(mutex_held(&c->dev->mode_config.mutex));
    if (e && e->identity==0xfeed) {
        assert(mutex_held(&current_for_update->dcpavserv.lock));
        if(current_for_update->hw.iomfb_method_profile==DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL)
            assert(mutex_held(&current_for_update->hpd_mutex));
    }
    if (e && update_error) return -ENOMEM;
    struct test_blob *old=c->edid_blob_ptr;
    struct test_blob *next=NULL;
    if (e) {
        next=malloc(sizeof(*next)); assert(next);
        *next=(struct test_blob){ .identity=e->identity, .references=1, .magic=0xcafe };
        blob_allocations++;
    } else clears++;
    if (old) { assert(old->magic==0xcafe && old->references==1); old->magic=0; free(old); blob_allocations--; }
    c->edid_blob_ptr=next;
    property_identity=e ? e->identity : 0;
    if (invalidate_during_update && e && e->identity!=0xfeed) current_for_update->metadata.epoch.invalidated=true;
    return 0;
}
static void dcp_update_connector_dimensions(struct apple_dcp *p, struct drm_connector *c) { (void)p; assert(mutex_held(&c->dev->mode_config.mutex)); }
static bool dcp_is_usb4_output(struct apple_dcp *p) { (void)p; return false; }
static bool apple_dp_tunnel_t602x(void) { return false; }
#define DRM_MODE_LINK_STATUS_BAD 1
static void drm_connector_set_link_status_property(struct drm_connector *c, int status) { (void)c; (void)status; }
static int dcp_retrain_active_crtc(struct apple_connector *c)
{
    assert(held_mutex!=&c->base.dev->mode_config.mutex);
    retrains++;
    return 0;
}
static void drm_kms_helper_connector_hotplug_event(struct drm_connector *c)
{
    assert(held_mutex!=&c->dev->mode_config.mutex);
    mutex_lock(&c->dev->mode_config.mutex);
    if (c->edid_blob_ptr) assert(c->edid_blob_ptr->magic==0xcafe);
    notifications++;
    mutex_unlock(&c->dev->mode_config.mutex);
}
static void dcp_retry_placeholder_edid(struct apple_dcp *p, const struct drm_edid *e)
{ (void)p; assert(e->identity==0xfeed); }
'''
suffix = r'''
static struct apple_dcp p;
static struct apple_connector c;
static struct platform_device platform;
static struct drm_device drm={ .mode_config.mutex.native=PTHREAD_MUTEX_INITIALIZER };
static struct dcp_display_mode modes[1] = {{ .mode={ .type=DRM_MODE_TYPE_PREFERRED, .clock=148500 }, .metadata_timing_valid=true }};
static struct dcp_display_metadata attrs = { .serial=1234 };
static int probe(struct drm_connector *connector)
{
    mutex_lock(&connector->dev->mode_config.mutex);
    int ret=dcp_get_modes(connector);
    mutex_unlock(&connector->dev->mode_config.mutex);
    return ret;
}
static void initial(void)
{
    mutex_lock(&drm.mode_config.mutex);
    drm_edid_free(c.drm_edid);
    c.base.dev=&drm;
    drm_edid_connector_update(&c.base,NULL);
    mutex_unlock(&drm.mode_config.mutex);
    free(p.chunks.data);
    assert(!allocations);
    memset(&p,0,sizeof(p)); memset(&c,0,sizeof(c));
    assert(!pthread_mutex_init(&p.hpd_mutex.native,NULL));
    assert(!pthread_mutex_init(&p.dcpavserv.lock.native,NULL));
    p.dcpavserv.enabled=true; p.dcpavserv.service=&p; p.dcpavserv.epoch=1;
    p.typec_cable_connected=true; p.typec_generation=1;
    detach_during_read=replug_during_read=retire_after_read=false;
    p.hw.iomfb_method_profile=DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL;
    p.fw_compat=DCP_FIRMWARE_H17P;
    static typeof(*p.dcpavservep) av={true}; p.dcpavservep=&av;
    p.connector=&c; p.modes=modes; p.nr_modes=1;
    platform.data=&p; c.dcp=&platform; c.base.dev=&drm; c.connected=true;
    p.mode_state.valid=true;
    raw_reads=mode_adds=clears=property_identity=0; raw_available=false;
    current_for_update=&p; invalidate_during_update=false; update_error=false; modes[0].metadata_timing_valid=true;
}
static void begin(bool complete)
{
    dcp_firmware_metadata_begin(&p);
    p.chunks.length=100; p.chunks.received=complete ? 100 : 99;
    p.chunks.payload_complete=complete;
}
static bool eligible(void)
{
    u64 generation;
    const struct drm_edid *edid=dcp_firmware_edid(&p,&generation);
    bool valid=!IS_ERR(edid);
    if (valid) { assert(generation==1 && edid->identity==attrs.serial); drm_edid_free(edid); }
    return valid;
}
static void publish(void)
{
    begin(true); dcp_firmware_metadata_attributes(&p,&attrs);
    begin(true); dcp_firmware_metadata_timing(&p);
}
static void *probe_thread(void *unused)
{
    (void)unused;
    for(unsigned int i=0;i<1000;i++) {
        mutex_lock(&drm.mode_config.mutex);
        c.connected=true; raw_available=!!(i&1);
        assert(dcp_get_modes(&c.base)==1);
        mutex_unlock(&drm.mode_config.mutex);
    }
    return NULL;
}
static void *hotplug_thread(void *unused)
{
    (void)unused;
    for(unsigned int i=0;i<1000;i++) {
        mutex_lock(&drm.mode_config.mutex); c.connected=false; mutex_unlock(&drm.mode_config.mutex);
        dcp_hotplug(&c.hotplug_wq);
    }
    return NULL;
}
static void *reader_thread(void *unused)
{
    (void)unused;
    for(unsigned int i=0;i<2000;i++) {
        mutex_lock(&drm.mode_config.mutex);
        struct test_blob *b=c.base.edid_blob_ptr;
        if(b) {
            assert(b->magic==0xcafe && b->references==1);
            b->references++;
            assert(b->identity==1234 || b->identity==0xfeed);
            b->references--;
        }
        mutex_unlock(&drm.mode_config.mutex);
    }
    return NULL;
}
int main(void)
{
    initial(); begin(true); /* A pre-association blob cannot become current later. */
    dcp_firmware_metadata_connection(&p,1,true);
    dcp_firmware_metadata_attributes(&p,&attrs);
    begin(true); dcp_firmware_metadata_timing(&p);
    assert(!eligible());
    begin(true); dcp_firmware_metadata_attributes(&p,&attrs);
    assert(eligible() && c.hotplug_wq.scheduled);
    /* Raw failure admits fallback, but fallback never blocks a later raw read. */
    assert(probe(&c.base)==1 && c.drm_edid_firmware_derived && raw_reads==1);
    assert(!c.drm_edid && property_identity==1234 && !allocations && mode_adds==1);
    raw_available=true;
    assert(probe(&c.base)==1 && !c.drm_edid_firmware_derived && raw_reads==2);
    assert(c.drm_edid->identity==0xfeed && mode_adds==2);
    assert(probe(&c.base)==1 && raw_reads==2); /* A raw cache remains authoritative. */

    initial(); dcp_firmware_metadata_connection(&p,1,true);
    begin(false); dcp_firmware_metadata_attributes(&p,&attrs);
    begin(true); dcp_firmware_metadata_timing(&p); assert(!eligible());
    begin(true); p.chunks.contiguous=false; dcp_firmware_metadata_attributes(&p,&attrs);
    assert(!eligible());
    begin(true); dcp_firmware_metadata_attributes(&p,&attrs); assert(eligible());
    assert(probe(&c.base)==1 && c.drm_edid_firmware_derived);
    dcp_firmware_metadata_connection(&p,2,false);
    dcp_firmware_metadata_connection(&p,3,true);
    publish(); assert(!eligible()); /* First connection only, even with later complete records. */
    assert(probe(&c.base)==1 && !c.drm_edid && !c.drm_edid_firmware_derived && clears && !allocations);

    initial(); dcp_firmware_metadata_connection(&p,1,true); begin(true);
    dcp_firmware_metadata_connection(&p,2,true); /* Change while a transfer is in flight. */
    dcp_firmware_metadata_attributes(&p,&attrs); assert(!eligible());
    initial(); dcp_firmware_metadata_connection(&p,1,true);
    dcp_firmware_metadata_hotplug(&p,false); /* Initial no-HPD state is not a second attachment. */
    publish(); assert(eligible());
    dcp_firmware_metadata_hotplug(&p,true);
    dcp_firmware_metadata_hotplug(&p,false); assert(!eligible());
    initial(); p.hw.iomfb_method_profile=0;
    dcp_firmware_metadata_connection(&p,1,true); publish(); assert(!eligible());
    assert(probe(&c.base)==1 && !c.drm_edid);
    initial(); dcp_firmware_metadata_connection(&p,1,true);
    modes[0].metadata_timing_valid=false; publish(); assert(!eligible());
    initial(); dcp_firmware_metadata_connection(&p,1,true); publish();
    invalidate_during_update=true;
    assert(probe(&c.base)==1 && !c.drm_edid_firmware_derived && !property_identity && !allocations);
    initial(); dcp_firmware_metadata_connection(&p,1,true); publish(); update_error=true;
    assert(probe(&c.base)==1 && !c.drm_edid_firmware_derived && !allocations);
    initial(); dcp_firmware_metadata_connection(&p,1,true);
    u32 length=64;
    struct dcp_set_dcpav_prop_chunk_req chunk={ .length=32 };
    assert(dcpep_cb_prop_start(&p,&length));
    assert(dcpep_cb_prop_chunk(&p,&chunk));
    assert(p.chunks.received==32 && p.chunks.contiguous);
    dcp_firmware_metadata_attributes(&p,&attrs); assert(!p.metadata.attributes_valid);
    chunk.offset=32; assert(dcpep_cb_prop_chunk(&p,&chunk));
    p.chunks.payload_complete=true; /* Structural validator tested separately. */
    dcp_firmware_metadata_attributes(&p,&attrs); assert(p.metadata.attributes_valid);
    initial(); dcp_firmware_metadata_connection(&p,1,true);
    assert(dcpep_cb_prop_start(&p,&length));
    assert(dcpep_cb_prop_chunk(&p,&chunk)); /* Out-of-order first chunk. */
    chunk.offset=0; assert(dcpep_cb_prop_chunk(&p,&chunk));
    assert(!p.chunks.contiguous);
    dcp_firmware_metadata_attributes(&p,&attrs); assert(!p.metadata.attributes_valid);
    chunk.offset=UINT32_MAX; chunk.length=8;
    assert(!dcpep_cb_prop_chunk(&p,&chunk));
    initial(); dcp_firmware_metadata_connection(&p,1,true); publish();
    assert(probe(&c.base)==1);
    c.dcp=NULL;
    dcp_hotplug(&c.hotplug_wq);
    assert(!c.drm_edid_firmware_derived && !c.base.edid_blob_ptr);
    c.dcp=&platform; c.connected=true; c.base.state=&p; p.mode_state.valid=false;
    dcp_hotplug(&c.hotplug_wq); assert(retrains);
    p.mode_state.valid=true;
    assert(probe(&c.base)==1);
    modeset_held=true; disconnected_hpd_event(&c); modeset_held=false;
    assert(!c.connected && c.edid_invalidation_pending && c.base.edid_blob_ptr);
    dcp_hotplug(&c.hotplug_wq);
    assert(!c.edid_invalidation_pending && !c.drm_edid_firmware_derived && !c.base.edid_blob_ptr);
    /* A fast reconnect cannot retain a raw EDID from the previous attachment. */
    c.connected=true; raw_available=true;
    assert(probe(&c.base)==1 && c.drm_edid);
    unsigned int previous_reads=raw_reads;
    modeset_held=true; disconnected_hpd_event(&c); modeset_held=false;
    c.connected=true;
    assert(probe(&c.base)==1 && raw_reads==previous_reads+1);
    assert(!c.edid_invalidation_pending);
    /* Worker-first ordering also consumes the invalidation after reconnect. */
    modeset_held=true; disconnected_hpd_event(&c); modeset_held=false;
    c.connected=true;
    dcp_hotplug(&c.hotplug_wq);
    assert(!c.drm_edid && !c.edid_invalidation_pending);
    assert(probe(&c.base)==1 && c.drm_edid);
    initial(); dcp_firmware_metadata_connection(&p,1,true); publish();
    pthread_t a,b,reader;
    assert(!pthread_create(&a,NULL,probe_thread,NULL));
    assert(!pthread_create(&b,NULL,hotplug_thread,NULL));
    assert(!pthread_create(&reader,NULL,reader_thread,NULL));
    assert(!pthread_join(a,NULL) && !pthread_join(b,NULL) && !pthread_join(reader,NULL));
    initial(); raw_available=true; detach_during_read=true;
    assert(probe(&c.base)==1 && !c.drm_edid && !property_identity && !allocations);
    assert(raw_reads==1 && !p.typec_cable_connected);
    assert(probe(&c.base)==1 && raw_reads==1); /* No request while disconnected. */
    initial(); raw_available=true; detach_during_read=replug_during_read=true;
    assert(probe(&c.base)==1 && !c.drm_edid && !property_identity && !allocations);
    detach_during_read=false;
    assert(probe(&c.base)==1 && c.drm_edid && c.drm_edid_generation==2);
    p.typec_generation++; /* Missed worker: generation alone rejects the old cache. */
    assert(probe(&c.base)==1 && raw_reads==3 && c.drm_edid_generation==3);
    initial(); raw_available=true;
    assert(probe(&c.base)==1 && c.drm_edid && c.drm_edid_service_epoch==1);
    p.dcpavserv.epoch+=2; /* Replacement without physical generation or worker. */
    assert(probe(&c.base)==1 && raw_reads==2 && c.drm_edid_service_epoch==3);
    p.dcpavserv.enabled=false; p.dcpavserv.epoch++;
    c.edid_invalidation_pending=true; raw_available=false;
    assert(probe(&c.base)==1 && !c.drm_edid && !property_identity && !allocations);
    initial(); raw_available=true; retire_after_read=true;
    assert(probe(&c.base)==1 && !c.drm_edid && !property_identity && !allocations);
    assert(p.typec_generation==1); /* Replacement in the check/publication gap. */
    retire_after_read=false;
    assert(probe(&c.base)==1 && c.drm_edid_service_epoch==3);
    assert(!c.edid_invalidation_pending); /* Old CLOSE cannot erase the fresh cache. */
    dcp_hotplug(&c.hotplug_wq); assert(c.drm_edid);
    initial(); assert(!blob_allocations && trace_observations);
    puts("PASS: mutex-serialized EDID/blob get_modes, hotplug, sysfs reader and same-epoch publication, first-connection scope, raw precedence, delayed disconnect/replug rejection, physical and service cache invalidation, service replacement publication gap, no added modes");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='dcp-metadata-lifetime-') as tmp:
    tmp = Path(tmp)
    (tmp / 'test.c').write_text(prefix + metadata + epoch + state + cache + chunk_callbacks + cleanup + physical_current + service_current + getter + hotplug + disconnect + suffix)
    command = shlex.split(os.environ.get('CC','clang'))
    command += ['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-sign-compare','-O1','-g',
                '-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread']
    subprocess.run(command + [str(tmp / 'test.c'),'-o',str(tmp / 'test')],check=True)
    subprocess.run([str(tmp / 'test')],check=True)
