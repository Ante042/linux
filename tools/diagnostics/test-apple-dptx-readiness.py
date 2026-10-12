#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production DPTX publication, retries and terminal service failures."""
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
base = root / 'drivers/gpu/drm/apple'
dcp = (base / 'dcp.c').read_text()
ep = (base / 'dptxep.c').read_text()
service = (base / 'dptx-service.h').read_text()


def function(source, name):
    match = re.search(r'^static (?:inline )?(?:bool|int|void) ' + name + r'\([^;{]*\)\n\{', source, re.M)
    assert match, name
    start, opening = match.start(), match.end() - 1
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


body = '\n'.join(function(service, n) for n in (
    'dptx_service_available', 'dptx_service_bind', 'dptx_service_publish', 'dptx_service_unbind'))
body += '\n' + '\n'.join(function(dcp, n) for n in (
    'dcp_is_external_h17p', 'dcp_dptx_connect_raw', 'dcp_external_link_locked',
    'dcp_dptx_connect', 'dcp_typec_reconnect_work'))
body += '\n' + function(ep, 'dptxport_init')
print('Production readiness SHA256:', hashlib.sha256(body.encode()).hexdigest())
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef int64_t s64;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define smp_load_acquire(p) (*(p))
#define smp_store_release(p, v) (*(p) = (v))
#define DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL 2
#define DPTX_CONNECT_TIMEOUT 2000
#define DPTX_TUNNEL_CONNECT_TIMEOUT 8000
#define DPTX_RECONNECT_DELAY 1000
#define DPTX_RECONNECT_RETRIES 5
#define DRM_MODE_CONNECTOR_DisplayPort 10
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
#define jiffies_to_msecs(x) (x)
#define usleep_range(a,b) ((void)(a), (void)(b))
#define system_freezable_wq NULL
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define to_delayed_work(p) container_of(p, struct delayed_work, work)
struct mutex { bool held; };
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
static struct mutex *acquire(struct mutex *m) { mutex_lock(m); return m; }
static void release(struct mutex **m) { if (*m) mutex_unlock(*m); }
#define guard(kind) struct mutex *g __attribute__((cleanup(release))) = acquire
#define lockdep_assert_held(m) assert((m)->held)
struct completion { bool done; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; unsigned int scheduled, delay; };
struct apple_dcp;
struct apple_dcp_afkep { struct apple_dcp *dcp; };
struct apple_epic_service { void *cookie; struct apple_dcp_afkep *ep; };
struct dptx_port {
    struct apple_epic_service *service;
    bool enabled, connected, closed;
    s64 unit;
    void *atcphy;
    struct completion enable_completion, linkcfg_completion;
};
struct apple_dcp {
    struct { int iomfb_method_profile; } hw;
    bool external_prepared, external_cable, quiescing, typec_cable_connected;
    int external_error;
    unsigned int typec_reconnect_tries;
    struct mutex external_lock, hpd_mutex;
    struct delayed_work typec_reconnect_wq;
    void *phy, *dev, *active_typec_route, *avep;
    unsigned int dptx_die, dptx_phy, dptx_dfp_port;
    int connector_type;
    struct dptx_port dptxport[1];
};
static unsigned int raw_attempts, firmware_calls, release_calls;
static int injected_error;
static unsigned int fail_stage;
static bool dcp_is_typec_output(struct apple_dcp *p) { (void)p; return true; }
static bool dcp_is_usb4_output(struct apple_dcp *p) { (void)p; return false; }
static bool apple_dp_tunnel_t602x(void) { return false; }
static void reinit_completion(struct completion *c) { c->done = false; }
static void complete(struct completion *c) { c->done = true; }
static unsigned long wait_for_completion_timeout(struct completion *c, unsigned long timeout)
{ (void)timeout; return c->done ? 1 : 0; }
static void mod_delayed_work(void *queue, struct delayed_work *w, unsigned int delay)
{
    struct apple_dcp *p = container_of(w, struct apple_dcp, typec_reconnect_wq);
    (void)queue;
    assert(!p->external_lock.held && !p->hpd_mutex.held);
    w->scheduled++;
    w->delay = delay;
}
static int stage(unsigned int step)
{ firmware_calls++; return fail_stage == step ? injected_error : 0; }
static int dptxport_validate_connection(struct apple_epic_service *s, u32 a, u32 b, u32 c)
{ (void)s; (void)a; (void)b; (void)c; raw_attempts++; return stage(1); }
static int dptxport_connect(struct apple_epic_service *s, u32 a, u32 b, u32 c, bool typec)
{ (void)s; (void)a; (void)b; (void)c; (void)typec; return stage(2); }
static int dptxport_request_display(struct apple_epic_service *s)
{
    struct dptx_port *p = s->cookie;
    int ret = stage(3);
    if (!ret) complete(&p->linkcfg_completion);
    return ret;
}
static int dptxport_set_hpd(struct apple_epic_service *s, bool on)
{ (void)s; (void)on; return stage(4); }
static int dptxport_set_hpd_timeout(struct apple_epic_service *s, bool on, unsigned int timeout)
{ (void)timeout; return dptxport_set_hpd(s, on); }
static int dptxport_release_display(struct apple_epic_service *s)
{ (void)s; release_calls++; return 0; }
static void av_service_connect(struct apple_dcp *p) { (void)p; }
'''
suffix = r'''
static void initial(struct apple_dcp *p, struct apple_epic_service *s, struct apple_dcp_afkep *ep)
{
    *p = (struct apple_dcp){
        .hw.iomfb_method_profile = DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL,
        .external_prepared = true, .external_cable = true, .typec_cable_connected = true,
        .phy = p,
    };
    *ep = (struct apple_dcp_afkep){ .dcp = p };
    *s = (struct apple_epic_service){ .ep = ep };
    assert(dptx_service_bind(s, &p->dptxport[0], 0) == 0);
    raw_attempts = firmware_calls = release_calls = fail_stage = 0;
    injected_error = 0;
}
static int link(struct apple_dcp *p)
{
    guard(mutex)(&p->external_lock);
    return dcp_external_link_locked(p);
}
static void publish(struct apple_epic_service *s)
{ dptxport_init(s, "dcpdptx-port-epic", "AppleDCPDPTXRemotePort", 0); }
int main(void)
{
    struct apple_dcp p;
    struct apple_epic_service service;
    struct apple_dcp_afkep ep;
    initial(&p, &service, &ep);
    /* Reproduce the observed initial call before the first AFK publication. */
    assert(link(&p) == -EAGAIN && !p.external_error);
    assert(!firmware_calls && !release_calls && !p.dptxport[0].connected);
    /* Even publication later than the bounded retry window must restart work. */
    for (unsigned int i = 0; i < DPTX_RECONNECT_RETRIES; i++)
        dcp_typec_reconnect_work(&p.typec_reconnect_wq.work);
    assert(p.typec_reconnect_tries == DPTX_RECONNECT_RETRIES && !p.external_error);
    unsigned int queued = p.typec_reconnect_wq.scheduled;
    publish(&service);
    assert(p.dptxport[0].enabled && p.dptxport[0].enable_completion.done);
    assert(p.typec_reconnect_wq.scheduled == queued + 1 && !p.typec_reconnect_wq.delay);
    assert(!firmware_calls); /* Publication only schedules; never calls firmware inline. */
    dcp_typec_reconnect_work(&p.typec_reconnect_wq.work);
    assert(raw_attempts == 1 && p.dptxport[0].connected && !p.external_error);
    assert(!p.typec_reconnect_tries);

    /* A reader between owner publication and enabled=true must also retry. */
    initial(&p, &service, &ep);
    p.dptxport[0].service = &service;
    assert(link(&p) == -EAGAIN && !p.external_error && !firmware_calls);
    publish(&service);
    assert(link(&p) == 0 && p.dptxport[0].connected);

    /* A closed published session must remain terminal, even before any power request. */
    dptx_service_unbind(&service);
    assert(link(&p) == -ENODEV && p.external_error == -ENODEV);
    assert(p.dptxport[0].service == &service);
    assert(dptx_service_bind(&service, &p.dptxport[0], 0) == -EBUSY);
    unsigned int attempts = raw_attempts;
    assert(link(&p) == -ENODEV && raw_attempts == attempts);

    /* Neither ENODEV nor EAGAIN from a real operation can masquerade as unpublished. */
    for (unsigned int phase = 1; phase <= 4; phase++) {
        for (unsigned int transient = 0; transient < 2; transient++) {
            initial(&p, &service, &ep);
            publish(&service);
            fail_stage = phase;
            injected_error = transient ? -EAGAIN : -ENODEV;
            assert(link(&p) == injected_error && p.external_error == injected_error);
            unsigned int calls = firmware_calls;
            fail_stage = 0;
            assert(link(&p) == injected_error && firmware_calls == calls);
        }
    }
    /* Missing configuration is terminal; only the first unpublished service is retryable. */
    initial(&p, &service, &ep);
    p.phy = NULL;
    assert(link(&p) == -ENODEV && p.external_error == -ENODEV && !firmware_calls);

    /* Legacy raw callers keep ENODEV and do not acquire external retry semantics. */
    initial(&p, &service, &ep);
    p.hw.iomfb_method_profile = 0;
    assert(dcp_dptx_connect(&p, 0) == -ENODEV && !firmware_calls);
    publish(&service);
    assert(!p.typec_reconnect_wq.scheduled);

    /* Publication cannot revive a removed cable, failed session, or quiescing device. */
    for (unsigned int state = 0; state < 3; state++) {
        initial(&p, &service, &ep);
        if (state == 0) p.typec_cable_connected = false;
        if (state == 1) p.external_error = -EIO;
        if (state == 2) p.quiescing = true;
        publish(&service);
        assert(!p.typec_reconnect_wq.scheduled && !firmware_calls);
    }
    puts("PASS: delayed publication, exhausted retry recovery, terminal CLOSE/operation errors, async notification");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='apple-dptx-readiness-') as directory:
    directory = Path(directory)
    unit = directory / 'test.c'
    unit.write_text(prefix + body + suffix)
    command = shlex.split(os.environ.get('CC', 'clang'))
    command += ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command + [str(unit), '-o', str(directory / 'test')], check=True)
    subprocess.run([str(directory / 'test')], check=True)
