#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host-only interleavings through production external lifecycle and queue code."""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
base = root / 'drivers/gpu/drm/apple'
dcp = (base / 'dcp.c').read_text()
iomfb = (base / 'iomfb.c').read_text()
h17p = (base / 'iomfb_h17p.c').read_text()
drm = (base / 'apple_drv.c').read_text()


def block(source, marker):
    start = source.index(marker)
    begin = source.index('{', start)
    end, depth = begin + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def function(source, name):
    import re
    marker = re.search(r'^(?:static )?(?:bool|void|int) ' + name + r'\([^;{]*\)\n\{', source, re.M)
    assert marker, name
    return block(source, marker.group()[:-1])


def external_branch(name, result):
    body = function(dcp, name)
    branch = block(body, 'if (dcp_is_external_h17p(dcp)) {')
    return body[:body.index('if (dcp_is_external_h17p(dcp)) {')] + branch + result + '\n}'


parts = [function(iomfb, 'dcp_handle_hotplug_actions')]
for name in ('iomfb_channels_idle', 'iomfb_pause_presentation', 'iomfb_queue_settle', 'iomfb_queue'):
    parts.append(function(iomfb, name))
parts.append(block(h17p, 'struct dcp_external_power_cookie {') + ';')
for name in ('iomfb_external_power_complete', 'iomfb_external_reply_size',
             'iomfb_external_parameter_complete', 'iomfb_external_device_complete'):
    parts.append(function(h17p, name))
parts.append(function(h17p, 'iomfb_external_power_h17p').replace(
    'int iomfb_external_power_h17p(', 'static int test_wire_power('))
# Model a future qualified OFF policy only in this harness. The production
# decoder is also tested directly and currently MUST reject every OFF reply.
parts.append(r'''
static int iomfb_external_power_h17p(struct apple_dcp *p, bool on)
{
    int ret = test_wire_power(p, on);
    return test_qualified_off && !on && ret == -EOPNOTSUPP && !p->external_power_reply ? 0 : ret;
}
''')
for name in ('dcp_is_external_h17p', 'dcp_external_report_disconnected',
             'dcp_external_publish_generation', 'dcp_external_link_locked',
             'dcp_external_start_locked', 'dcp_external_stop_locked',
             'dcp_external_disconnect', 'dcp_external_stop_work', 'dcp_external_hotplug'):
    parts.append(function(dcp, name))
parts += [external_branch('dcp_dptx_connect_oob', '\nreturn -ENOSYS;'),
          external_branch('dcp_poweron', ''), external_branch('dcp_poweroff', ''),
          function(dcp, 'dcp_external_recover')]
# Include real admission call sites; only their allocation/start/release dependencies are mocked.
parts += [function(iomfb, 'iomfb_queue_atomic'), function(iomfb, 'iomfb_backlight_kick'),
          function(iomfb, 'iomfb_queue_crc_h17p')]
parts += [function(iomfb, 'dcp_crtc_atomic_modeset')]
parts += [function(drm, 'apple_crtc_atomic_enable'), function(drm, 'apple_crtc_atomic_disable')]
route_admission = function(dcp, 'dcp_typec_route_deactivate')
route_admission = route_admission[:route_admission.index('\n\t/*')] + '\n\treturn 0;\n}'
route_admission = route_admission.replace('dcp_typec_route_deactivate(', 'test_route_deactivate_admission(')
parts.append(route_admission)
production = '\n'.join(parts)
print('Production lifecycle SHA256:', hashlib.sha256(production.encode()).hexdigest())
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define msecs_to_jiffies(x) (x)
#define time_after_eq(a, b) ((a) >= (b))
#define DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL 2
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
#define WARN_ONCE(...) ((void)0)
enum { DCP_FIRMWARE_V_12_3, DCP_FIRMWARE_V_13_5, DCP_FIRMWARE_H17P };
#define IOMFBPARAM_ADAPTIVE_SYNC 1
struct dcp_set_parameter_dcp { u32 param, value[8], count; };
struct mutex { bool held; };
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
static struct mutex *acquire(struct mutex *m) { mutex_lock(m); return m; }
static void release(struct mutex **m) { if (*m) mutex_unlock(*m); }
static struct mutex *once(struct mutex *m) { mutex_unlock(m); return NULL; }
#define JOIN2(a,b) a##b
#define JOIN(a,b) JOIN2(a,b)
#define guard(kind) struct mutex *JOIN(guard_, __COUNTER__) __attribute__((cleanup(release))) = acquire
#define scoped_guard(kind, value) for (struct mutex *g __attribute__((cleanup(release))) = acquire(value); g; g = once(g))
#define lockdep_assert_held(m) assert((m)->held)
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
struct list_head { unsigned int count; };
static bool list_empty(struct list_head *h) { return !h->count; }
static void list_add_tail(struct list_head *n, struct list_head *h) { (void)n; h->count++; }
#define list_add list_add_tail
struct work_struct { unsigned int scheduled; };
struct delayed_work { unsigned int cancelled; };
struct completion { bool done; };
struct kref { unsigned int count; };
struct dcp_wait_cookie { struct kref refcount; struct completion done; u32 status; };
struct dcp_packet_header { char tag[4]; u32 in_len, out_len; };
struct dcp_set_power_state_req_h17p { u64 unklong; u32 unkint; unsigned char null, padding[3]; } __attribute__((packed));
struct dcp_set_power_state_resp { u32 unknown, ret; } __attribute__((packed));
struct device { int unused; };
struct apple_connector { bool connected; struct work_struct hotplug_wq; };
struct dptx_port { bool connected; void *service; };
struct iomfb_transaction { struct list_head link; void (*start)(void *,void *), (*release)(void *); bool brightness_only, presentation; };
struct platform_device;
struct drm_crtc_state { bool active, active_changed, mode_changed, color_mgmt_changed; void *event; struct { int hdisplay, vdisplay; } mode; };
struct drm_device { int event_lock; };
struct drm_crtc { struct drm_crtc_state *state; struct drm_device *dev; };
struct drm_atomic_state { struct drm_crtc_state *next; };
struct apple_crtc { struct drm_crtc base; struct platform_device *dcp; };
#define to_apple_crtc(p) container_of(p, struct apple_crtc, base)
static struct drm_crtc_state *drm_atomic_get_new_crtc_state(struct drm_atomic_state *s, struct drm_crtc *c) { (void)c; return s->next; }
static bool drm_atomic_crtc_needs_modeset(struct drm_crtc_state *s) { return s->mode_changed; }
static unsigned int modesets, vblanks;
static int dcp_crtc_atomic_modeset(struct drm_crtc *, struct drm_atomic_state *);
#define spin_lock_irq(lock) ((void)(lock))
#define spin_unlock_irq(lock) ((void)(lock))
static void drm_crtc_send_vblank_event(struct drm_crtc *c, void *e) { (void)c; assert(e); vblanks++; }
struct apple_dcp;
struct iomfb_atomic_transaction { struct iomfb_transaction transaction; struct drm_crtc *crtc; struct drm_atomic_state *state; struct apple_dcp *dcp; };
struct iomfb_crc_transaction { struct iomfb_transaction transaction; u32 swap_id; };
struct apple_dcp {
    struct device *dev;
    struct { int iomfb_method_profile; } hw;
    struct mutex external_lock, hpd_mutex;
    u64 external_generation, external_crtc_generation, typec_generation, external_rx_generation, external_stop_generation, external_power_reply;
    bool external_power_reply_valid, external_power_attempted, quiescing;
    int fw_compat;
    bool external_prepared, external_cable, external_running, external_stopped;
    bool external_hotplug_pending, external_hotplug_connected;
    int external_error;
    bool crashed, pipe_enabled_h17p, typec_crtc_off, typec_cable_connected, placeholder_retried, crc_enabled;
    struct delayed_work typec_reconnect_wq, placeholder_edid_wq;
    struct work_struct external_stop_work, vblank_wq;
    struct apple_connector *connector, *typec_connector;
    struct dptx_port dptxport[1];
    void *avep;
    struct { bool pending; } present_state_h17p;
    struct { unsigned int depth; } ch_cmd, ch_cb, ch_oobcmd, ch_oobcb, ch_async, ch_oobasync;
    struct {
        struct mutex lock;
        struct list_head pending;
        struct iomfb_transaction *active;
        unsigned int queued;
        bool stopped, presentation_paused, backlight_queued;
        struct work_struct work;
    } iomfb;
    struct { bool valid; } mode_state;
    unsigned int retained_dma, retained_fb;
};
struct platform_device { struct apple_dcp *data; };
struct mux_control { int unused; };
struct apple_dcp_typec_route { struct apple_dcp *dcp; struct mux_control *active_xbar; bool tunnel; };
#define platform_get_drvdata(p) ((p)->data)
static struct apple_dcp *current_dcp;
static unsigned long jiffies;
static unsigned int sleeps, power0, power1, drops, releases, connects, allocations;
static int connect_error, hpd_error, release_error, wire_error;
static bool drain_callback, timeout_reply, absent_reply, test_qualified_off;
static int prep_error;
static bool on_zero_reply;
static u32 wire_in, wire_out;
static char events[256];
static unsigned int event_count;
static void event(char c) { assert(event_count < sizeof(events) - 1); events[event_count++] = c; }
static bool dcp_external_hotplug(struct apple_dcp *, bool);
static void usleep_range(unsigned int a, unsigned int b)
{
    (void)a; (void)b;
    assert(!current_dcp->iomfb.lock.held);
    jiffies += 2; sleeps++;
    if (drain_callback) {
        mutex_lock(&current_dcp->iomfb.lock);
        assert(dcp_external_hotplug(current_dcp, false));
        current_dcp->iomfb.active = NULL;
        current_dcp->iomfb.pending.count = current_dcp->iomfb.queued = 0;
        current_dcp->present_state_h17p.pending = false;
        current_dcp->ch_cb.depth = 0;
        mutex_unlock(&current_dcp->iomfb.lock);
        drain_callback = false;
    }
}
static unsigned int dcp_mode_hotplug(void *state, bool connected, bool *result)
{ (void)state; if (result) *result = connected; return 1; }
#define DCP_HOTPLUG_NOTIFY 1
#define DCP_HOTPLUG_VBLANK 2
static void dcp_handle_hotplug_actions(struct apple_dcp *, unsigned int);
static void schedule_work(struct work_struct *);
static void schedule_work(struct work_struct *w)
{
    w->scheduled++;
    if (current_dcp && w == &current_dcp->connector->hotplug_wq) event('N');
}
static void cancel_delayed_work_sync(struct delayed_work *w)
{ assert(!current_dcp->external_lock.held); w->cancelled++; }
static void cancel_work_sync(struct work_struct *w)
{ assert(!current_dcp->external_lock.held); w->scheduled = 0; }
static bool dptx_service_available(struct dptx_port *p) { return p->service != NULL; }
static int dptxport_set_hpd(void *s, bool hpd)
{ (void)s; assert(!hpd); event('H'); drops++; return hpd_error; }
static int dptxport_release_display(void *s)
{ (void)s; event('R'); releases++; return release_error; }

static void av_service_disconnect(struct apple_dcp *p) { (void)p; }
static int dcp_dptx_connect_raw(struct apple_dcp *p, u32 port)
{
    assert(!port && p->external_lock.held);
    if (p->dptxport[0].connected) return 0;
    event('C'); connects++;
    if (connect_error) return connect_error;
    p->dptxport[0].connected = true;
    mutex_lock(&p->iomfb.lock);
    assert(dcp_external_hotplug(p, true));
    mutex_unlock(&p->iomfb.lock);
    return 0;
}
static void kref_init(struct kref *r) { r->count = 1; }
static void kref_get(struct kref *r) { r->count++; }
static void kref_put(struct kref *r, void (*fn)(struct kref *)) { assert(r->count); if (!--r->count) fn(r); }
static void release_wait_cookie(struct kref *r) { allocations--; free(container_of(r, struct dcp_wait_cookie, refcount)); }
static void *allocate(size_t size) { allocations++; return calloc(1, size); }
#define kzalloc_obj(obj) allocate(sizeof(obj))
static void kfree(void *p) { if (p) { allocations--; free(p); } }
static void init_completion(struct completion *c) { c->done = false; }
static void complete(struct completion *c) { c->done = true; }
static unsigned long wait_for_completion_timeout(struct completion *c, unsigned long timeout)
{ (void)timeout; assert(!current_dcp->iomfb.lock.held); return c->done; }
static u64 get_unaligned_le64(const void *p) { u64 n; memcpy(&n, p, sizeof(n)); return n; }
static struct dcp_wait_cookie *late_cookie;
static void iomfb_external_power_complete(struct apple_dcp *, void *, void *);
static void dcp_set_power_state(struct apple_dcp *p, bool oob,
                               struct dcp_set_power_state_req_h17p *req,
                               void (*cb)(struct apple_dcp *, void *, void *), void *cookie)
{
    struct { struct dcp_packet_header header; struct dcp_set_power_state_req_h17p req; struct dcp_set_power_state_resp resp; } packet = {0};
    assert(oob && p->iomfb.presentation_paused && !p->iomfb.lock.held);
    assert(req->unklong <= 1 && !req->unkint && !req->null);
    event(req->unklong ? '1' : '0');
    if (req->unklong) power1++; else power0++;
    packet.header.in_len = wire_in;
    packet.header.out_len = wire_out;
    packet.resp.unknown = req->unklong && !on_zero_reply ? 1 : 0;
    packet.resp.ret = wire_error;
    if (timeout_reply) { late_cookie = cookie; return; }
    mutex_lock(&p->iomfb.lock);
    assert(dcp_external_hotplug(p, req->unklong));
    cb(p, absent_reply ? NULL : &packet.resp, cookie);
    mutex_unlock(&p->iomfb.lock);
}
static void iomfb_opaque_x_reset_h17p(struct apple_dcp *p) { (void)p; }
static void dcp_set_display_device(struct apple_dcp *p, bool oob, u32 *handle,
                                    void (*cb)(struct apple_dcp *, void *, void *), void *cookie)
{
    struct { struct dcp_packet_header h; u32 input, output; } packet = {0};
    assert(!oob && *handle == 2); event('D');
    packet.h.in_len = 4; packet.h.out_len = prep_error == 1 ? 0 : 4;
    cb(p, &packet.output, cookie);
}
static void dcp_set_parameter_dcp(struct apple_dcp *p, bool oob, struct dcp_set_parameter_dcp *param,
                                 void (*cb)(struct apple_dcp *, void *, void *), void *cookie)
{
    struct { struct dcp_packet_header h; struct dcp_set_parameter_dcp input; u32 output; } packet = {0};
    assert(!oob && param->param == IOMFBPARAM_ADAPTIVE_SYNC && param->count == 3); event('A');
    for (unsigned int i = 0; i < 8; i++) assert(!param->value[i]);
    packet.h.in_len = 40; packet.h.out_len = prep_error == 2 ? 0 : 4;
    cb(p, &packet.output, cookie);
}
static void dcp_mode_begin(void *state) { (void)state; }
static unsigned int dcp_mode_finish(void *state, bool success, bool *connected)
{ (void)state; (void)success; (void)connected; return 0; }
static int iomfb_modeset_h17p(struct apple_dcp *p, struct drm_crtc_state *s)
{ (void)s; assert(p->external_lock.held && p->external_running); modesets++; return 0; }
#define iomfb_modeset_v12_3 iomfb_modeset_h17p
#define iomfb_modeset_v13_3 iomfb_modeset_h17p
static void iomfb_atomic_start(void *a, void *b) { (void)a; (void)b; }
static void iomfb_atomic_release(void *p) { kfree(p); }
static struct drm_atomic_state *drm_atomic_state_get(struct drm_atomic_state *s) { return s; }
static void iomfb_backlight_start(void *a, void *b) { (void)a; (void)b; }
static void iomfb_backlight_release(void *p) { kfree(p); }
static void iomfb_crc_start(void *a, void *b) { (void)a; (void)b; }
static void iomfb_crc_release(void *p) { kfree(p); }
'''
suffix = r'''
static struct apple_connector connector;
static void initial(struct apple_dcp *p)
{
    assert(!allocations);
    memset(p, 0, sizeof(*p));
    memset(&connector, 0, sizeof(connector)); connector.connected = true;
    p->connector = &connector;
    p->external_prepared = p->external_cable = p->external_running = p->external_power_attempted = true;
    p->fw_compat = DCP_FIRMWARE_H17P;
    p->external_generation = p->external_crtc_generation = p->external_rx_generation = 1;
    p->hw.iomfb_method_profile = DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL;
    p->dptxport[0] = (struct dptx_port){ .connected = true, .service = p };
    p->retained_dma = 7; p->retained_fb = 11;
    current_dcp = p;
    jiffies = sleeps = power0 = power1 = drops = releases = connects = event_count = 0;
    connect_error = hpd_error = release_error = wire_error = 0;
    drain_callback = timeout_reply = absent_reply = false;
    wire_in = 16; wire_out = 8;
    late_cookie = NULL; test_qualified_off = true; prep_error = 0; on_zero_reply = false;
    memset(events, 0, sizeof(events));
}
static int stop(struct apple_dcp *p, u64 generation)
{
    guard(mutex)(&p->external_lock);
    return dcp_external_stop_locked(p, generation);
}
static void owned(struct apple_dcp *p)
{ assert(p->retained_dma == 7 && p->retained_fb == 11 && !allocations); }
int main(void)
{
    struct apple_dcp p;
    struct platform_device dev = { &p };
    initial(&p);
    assert(stop(&p, 1) == 0);
    assert(!strcmp(events, "0HR") && power0 == 1 && releases == 1);
    assert(p.iomfb.presentation_paused && p.external_stopped && !p.crashed);
    assert(stop(&p, 1) == 0 && power0 == 1 && releases == 1);
    owned(&p);

    /* Atomic, backlight and CRC are rejected; control transactions remain admissible. */
    iomfb_queue_atomic(&p, NULL, NULL);
    iomfb_backlight_kick(&p);
    p.crc_enabled = true;
    mutex_lock(&p.iomfb.lock); iomfb_queue_crc_h17p(&p, 1); mutex_unlock(&p.iomfb.lock);
    assert(!p.crashed && !p.iomfb.queued && !allocations);
    struct iomfb_transaction presentation = { .presentation = true };
    struct iomfb_transaction control = {0};
    assert(iomfb_queue(&p, &presentation) == -EAGAIN);
    assert(iomfb_queue(&p, &control) == 0);
    p.iomfb.queued = p.iomfb.pending.count = 0;

    /* A callback must be able to acquire iomfb.lock and complete a drain. */
    initial(&p);
    p.iomfb.active = &presentation; p.iomfb.pending.count = p.iomfb.queued = 1;
    p.present_state_h17p.pending = true; p.ch_cb.depth = 1; drain_callback = true;
    assert(stop(&p, 1) == 0 && sleeps && power0 == 1 && !p.crashed);
    owned(&p);

    /* Every channel, queued-count, active, pending and present state blocks strict drain. */
    for (unsigned int field = 0; field < 10; field++) {
        initial(&p);
        if (field == 0) p.ch_cmd.depth = 1;
        if (field == 1) p.ch_cb.depth = 1;
        if (field == 2) p.ch_oobcmd.depth = 1;
        if (field == 3) p.ch_oobcb.depth = 1;
        if (field == 4) p.ch_async.depth = 1;
        if (field == 5) p.ch_oobasync.depth = 1;
        if (field == 6) p.iomfb.active = &presentation;
        if (field == 7) p.iomfb.pending.count = 1;
        if (field == 8) p.present_state_h17p.pending = true;
        if (field == 9) p.iomfb.queued = 1;
        assert(stop(&p, 1) == -ETIMEDOUT && !power0 && !drops && !releases);
        assert(p.iomfb.presentation_paused && p.external_error == -ETIMEDOUT);
        assert(stop(&p, 1) == -ETIMEDOUT && !power0);
        dcp_external_recover(&dev);
        assert(!power1 && !p.crashed);
        owned(&p);
    }
    for (unsigned int fault = 0; fault < 7; fault++) {
        initial(&p);
        if (fault == 0) p.crashed = true;
        if (fault == 1) p.iomfb.stopped = true;
        if (fault == 2) wire_out = 12;
        if (fault == 3) wire_in = 12;
        if (fault == 4) { wire_error = 5; test_qualified_off = false; }
        if (fault == 5) hpd_error = -EIO;
        if (fault == 6) release_error = -EIO;
        assert(stop(&p, 1) < 0 && p.external_error && p.iomfb.presentation_paused);
        assert(p.dptxport[0].connected && !p.external_stopped);
        assert(power0 == (fault > 1));
        assert(releases == (fault == 6));
        owned(&p);
    }
    initial(&p); timeout_reply = true;
    assert(stop(&p, 1) == -ETIMEDOUT && late_cookie && allocations == 1);
    assert(!drops && !releases && p.dptxport[0].connected);
    iomfb_external_power_complete(&p, NULL, late_cookie); /* late callback owns its reference */
    owned(&p);

    /* Physical removal + new cable before old CRTC disable: stale token cannot stop new link. */
    initial(&p);
    assert(dcp_external_disconnect(&p, true) == 0);
    assert(!p.external_cable && !connector.connected && p.external_generation == 2);
    assert(dcp_dptx_connect_oob(&dev, 0) == 0);
    assert(p.external_generation == 3 && !p.external_running && !connector.connected);
    assert(p.iomfb.presentation_paused && power1 == 0);
    dcp_poweroff(&dev);
    assert(power0 == 1 && !p.external_running); /* token still belongs to generation 1 */
    /* A stale disable must still deliver host DRM event bookkeeping. */
    struct drm_crtc_state inactive = { .active_changed = true, .event = &p };
    struct drm_device drm_dev = {0};
    struct apple_crtc crtc = { .dcp = &dev, .base = { .state = &inactive, .dev = &drm_dev } };
    struct drm_atomic_state atomic = { .next = &inactive };
    apple_crtc_atomic_disable(&crtc.base, &atomic);
    assert(power0 == 1 && !inactive.event && vblanks == 1);
    /* Active -> active recovery invokes the real CRTC hook and refreshes the token. */
    struct drm_crtc_state active = { .active = true, .mode_changed = true, .mode = { 1920, 1080 } };
    atomic.next = &active;
    apple_crtc_atomic_enable(&crtc.base, &atomic);
    assert(modesets == 1);
    assert(p.external_crtc_generation == 3 && power1 == 1);
    dcp_poweroff(&dev);
    assert(power0 == 2 && p.external_stopped);
    dcp_poweroff(&dev); assert(power0 == 2);
    owned(&p);

    /* Uncertain physical stop reports cable loss but keeps the resource and blocks restart. */
    initial(&p); test_qualified_off = false;
    assert(dcp_external_disconnect(&p, true) == -EOPNOTSUPP);
    assert(!connector.connected && p.dptxport[0].connected && !releases);
    assert(dcp_dptx_connect_oob(&dev, 0) == -EOPNOTSUPP && !power1);
    assert(p.external_crtc_generation == 1 && p.iomfb.presentation_paused);
    owned(&p);
    /* A deferred old RX disconnect cannot stop a later connection epoch. */
    initial(&p);
    mutex_lock(&p.iomfb.lock);
    assert(dcp_external_hotplug(&p, false));
    mutex_unlock(&p.iomfb.lock);
    assert(p.external_stop_generation == 1);
    p.external_generation = p.external_rx_generation = p.external_crtc_generation = 3;
    dcp_external_stop_work(&p.external_stop_work);
    assert(!power0 && !drops && p.external_running);
    owned(&p);

    /* Before initialization there is no firmware/shared-memory transport to touch. */
    initial(&p);
    p.external_prepared = p.external_power_attempted = p.external_running = false;
    p.dptxport[0].connected = false;
    assert(stop(&p, 1) == 0 && !power0 && !power1 && !releases);
    assert(dcp_dptx_connect_oob(&dev, 0) == -EAGAIN && !connects);
    owned(&p);

    /* Link hookup alone never powers the pipe. A first activation preserves DA1 order. */
    initial(&p);
    p.external_power_attempted = p.external_running = false;
    p.iomfb.presentation_paused = true;
    p.dptxport[0].connected = false;
    assert(dcp_dptx_connect_oob(&dev, 0) == 0 && connects == 1 && !power1);
    dcp_external_recover(&dev);
    assert(strstr(events, "DA1") && p.external_running && power1 == 1);
    owned(&p);

    /* Exact ON requires the 64-bit reply 1; all-zero must not activate modesets. */
    initial(&p);
    p.external_running = false; p.external_stopped = true;
    p.iomfb.presentation_paused = true; on_zero_reply = true;
    unsigned int previous_modesets = modesets;
    dcp_external_recover(&dev);
    assert(p.external_error == -EREMOTEIO && !p.external_running);
    assert(p.external_power_reply_valid && p.external_power_reply == 0);
    assert(dcp_crtc_atomic_modeset(&crtc.base, &atomic) == -EPIPE);
    apple_crtc_atomic_enable(&crtc.base, &atomic);
    assert(modesets == previous_modesets && power1 == 1);
    owned(&p);

    /* Production OFF policy always records but refuses to qualify its 8-byte reply. */
    initial(&p); p.iomfb.presentation_paused = true; wire_error = 0x1234;
    assert(test_wire_power(&p, false) == -EOPNOTSUPP);
    assert(p.external_power_reply_valid && p.external_power_reply == (0x1234ULL << 32));
    assert(!drops && !releases);
    owned(&p);
    /* Notification delivery is deferred: a client can reenter the lifecycle lock. */
    initial(&p);
    assert(dcp_external_disconnect(&p, true) == 0);
    assert(p.connector->hotplug_wq.scheduled && !p.external_lock.held);
    mutex_lock(&p.external_lock); mutex_unlock(&p.external_lock);

    /* Fabric cleanup cannot release a failed or still-running external route. */
    initial(&p);
    struct apple_dcp_typec_route route = { .dcp = &p };
    assert(test_route_deactivate_admission(&route) == -EBUSY);
    p.external_error = -EOPNOTSUPP;
    assert(test_route_deactivate_admission(&route) == -EOPNOTSUPP);
    p.external_error = 0; p.external_stopped = true;
    assert(test_route_deactivate_admission(&route) == 0);
    owned(&p);

    /* Missing replies and malformed first-start preparation fail closed. */
    initial(&p); p.iomfb.presentation_paused = true; absent_reply = true;
    assert(test_wire_power(&p, false) == -EPROTO && !p.external_power_reply_valid);
    owned(&p);
    for (int stage = 1; stage <= 2; stage++) {
        initial(&p); p.external_running = false; p.iomfb.presentation_paused = true;
        prep_error = stage;
        dcp_external_recover(&dev);
        assert(p.external_error == -EPROTO && !power1 && p.iomfb.presentation_paused);
        owned(&p);
    }
    puts("PASS: strict drain, callbacks, admission, exact RPC, stop/restart, stale epochs, modeset gate, retained ownership");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='apple-external-lifecycle-') as tmp:
    tmp = Path(tmp)
    unit = tmp / 'test.c'
    unit.write_text(prefix + production + suffix)
    command = shlex.split(os.environ.get('CC', 'clang'))
    command += ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-variable', '-Wno-unused-parameter', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command + [str(unit), '-o', str(tmp / 'test')], check=True)
    subprocess.run([str(tmp / 'test')], check=True)
