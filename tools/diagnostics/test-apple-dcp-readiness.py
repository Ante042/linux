#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production DCP matching/readiness and publication against host stubs."""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[2])
args = parser.parse_args()
dcp = (args.source_root / 'drivers/gpu/drm/apple/dcp.c').read_text()
drm = (args.source_root / 'drivers/gpu/drm/apple/apple_drv.c').read_text()


def function(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


# This slice is the production probe's publication boundary, including its
# failure exits and prepared-before-component_add ordering, not a copied model.
probe = function(dcp, 'static int dcp_platform_probe(')
publish = probe[probe.rindex('\tif (dcp->hw.iomfb_method_profile ==\n\t    DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL) {'):]
production = '\n'.join((
    function(dcp, 'bool dcp_h17p_external_ready('),
    function(drm, 'static bool apple_dcp_usable('),
    function(drm, 'static int add_dcp_components('),
    function(drm, 'static int apple_drm_init_dcp('),
    'static int publish_provider(struct platform_device *pdev, struct apple_dcp *dcp)\n'
    '{\n\tstruct device *dev = &pdev->dev;\n\tint ret;\n' + publish,
))
print('Production readiness/publication SHA256:', hashlib.sha256(production.encode()).hexdigest())
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define IS_ENABLED(x) 0
#define MAX_COPROCESSORS 4
#define DL_FLAG_AUTOREMOVE_SUPPLIER 0
#define DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL 2
#define dev_warn(...) ((void)0)
#define msleep(x) ((void)(x))
#define component_compare_of 0
#define apple_dcp_id_tbl 0
static int dcp_comp_ops;
struct device_node { bool available, external, phys; unsigned int index; };
struct device { struct device_node *of_node; void *data; };
struct platform_device { struct device dev; bool bound, registered; };
struct apple_dcp {
    bool external_prepared, quiescing, crashed;
    struct { int iomfb_method_profile; } hw;
};
struct drm_device { int unused; };
struct drm_encoder { int unused; };
struct apple_drm_private { struct drm_device drm; };
struct component_match { bool includes[2]; };
static struct device_node nodes[2];
static struct platform_device devices[2];
static struct apple_dcp dcps[2];
static struct apple_drm_private priv;
static struct device master = { .data = &priv };
static struct component_match matches;
static bool enabled, absent_external;
static unsigned int references, released, node_puts, probes[2], bind_attempts, disables;
static int preflight_error, external_probe_error, bind_result;
static void try_bind(void);
static bool dcp_h17p_external_enabled(void) { return enabled; }
static bool of_device_is_available(struct device_node *n) { return n->available; }
static bool of_device_is_compatible(struct device_node *n, const char *s)
{
    return n->external && (!strcmp(s, "apple,t8140-dcpext") || !strcmp(s, "apple,dcpext"));
}
static bool of_property_present(struct device_node *n, const char *s)
{ assert(!strcmp(s, "phys")); return n->phys; }
#define for_each_matching_node(np, table) \
    for (unsigned int node_i = 0; node_i < 2 && ((np) = &nodes[node_i]); node_i++)
#define for_each_endpoint_of_node(np, endpoint) \
    for ((endpoint) = NULL; (endpoint) != NULL; (endpoint) = NULL)
static struct device_node *of_graph_get_remote_port_parent(struct device_node *n)
{ return n; }
static void of_node_put(struct device_node *n) { (void)n; node_puts++; }
static struct platform_device *of_find_device_by_node(struct device_node *n)
{
    if (n->external && absent_external)
        return NULL;
    references++;
    return &devices[n->index];
}
static void *platform_get_drvdata(struct platform_device *p) { return p->dev.data; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void put_device(struct device *d) { (void)d; released++; }
/* Deliberately provided: using this during synchronous bind is a regression. */
#define device_is_bound(dev) (devices[(dev)->of_node->index].bound)
static void drm_of_component_match_add(struct device *d, struct component_match **m,
                                       int compare, struct device_node *n)
{ (void)d; (void)compare; *m = &matches; matches.includes[n->index] = true; }
static void device_link_add(struct device *a, struct device *b, int flags)
{ (void)a; (void)b; (void)flags; }
static int apple_probe_per_dcp(struct device *d, struct drm_device *drm,
                              struct platform_device *p, int index, bool external,
                              struct drm_encoder **encoder, u32 *mask)
{
    (void)d; (void)drm; (void)index; (void)encoder; (void)mask;
    assert(external == p->dev.of_node->external);
    probes[p->dev.of_node->index]++;
    return external ? external_probe_error : 0;
}
static int apple_probe_typec_ports(struct drm_device *d, struct platform_device **p,
                                  u32 *masks, int count)
{ (void)d; (void)p; (void)masks; (void)count; return 0; }
static int dcp_start(struct platform_device *p) { (void)p; return 0; }
static int dcp_wait_ready(struct platform_device *p, u64 timeout)
{ (void)p; (void)timeout; return 0; }
static u64 get_jiffies_64(void) { return 0; }
static u64 msecs_to_jiffies(unsigned int ms) { return ms; }
static bool time_after_eq64(u64 a, u64 b) { return a >= b; }
static int dcp_comp_bind(struct device *d, void *master_data, void *data)
{ (void)d; (void)master_data; (void)data; return preflight_error; }
static void dcp_disable_typec_work(struct apple_dcp *d, bool quiesce)
{ disables++; d->quiescing = quiesce; }
static int component_add(struct device *d, const void *ops)
{
    (void)ops;
    struct platform_device *p = &devices[d->of_node->index];
    assert(!p->bound);
    p->registered = true;
    try_bind(); /* Linux component_add may bind the master before probe returns. */
    return 0;
}
'''
suffix = r'''
static void try_bind(void)
{
    for (unsigned int i = 0; i < 2; i++)
        if (matches.includes[i] && !devices[i].registered)
            return;
    bind_attempts++;
    bind_result = apple_drm_init_dcp(&master);
}
static void initial(bool opt_in)
{
    memset(nodes, 0, sizeof(nodes));
    memset(devices, 0, sizeof(devices));
    memset(dcps, 0, sizeof(dcps));
    memset(&matches, 0, sizeof(matches));
    memset(probes, 0, sizeof(probes));
    for (unsigned int i = 0; i < 2; i++) {
        nodes[i] = (struct device_node){ .available = true, .external = i == 1, .index = i };
        devices[i].dev = (struct device){ .of_node = &nodes[i], .data = &dcps[i] };
    }
    dcps[1].hw.iomfb_method_profile = DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL;
    devices[0].registered = devices[0].bound = true;
    enabled = opt_in;
    absent_external = false;
    references = released = node_puts = bind_attempts = disables = 0;
    preflight_error = external_probe_error = bind_result = 0;
}
static int match_providers(void)
{
    struct component_match *match = NULL;
    return add_dcp_components(&master, &match);
}
int main(void)
{
    /* Default internal display does not wait for an unsupported external DCP. */
    initial(false);
    assert(match_providers() == 1 && matches.includes[0] && !matches.includes[1]);
    assert(!dcp_h17p_external_ready(&nodes[1]) && references == 0);
    try_bind();
    assert(bind_attempts == 1 && bind_result == 0 && probes[0] == 1 && !probes[1]);

    /* Delayed external provider remains in the match; the master must wait. */
    initial(true);
    assert(match_providers() == 2 && matches.includes[1]);
    try_bind();
    assert(bind_attempts == 0 && !probes[0] && !probes[1]);
    /* Last provider is prepared, but driver core bound is still false here. */
    assert(publish_provider(&devices[1], &dcps[1]) == 0);
    assert(dcps[1].external_prepared && !devices[1].bound);
    assert(bind_attempts == 1 && bind_result == 0 && probes[0] == 1 && probes[1] == 1);
    devices[1].bound = true;

    /* Permanent preflight failure cannot silently drop a requested provider. */
    initial(true);
    assert(match_providers() == 2);
    preflight_error = -EIO;
    assert(publish_provider(&devices[1], &dcps[1]) == -EIO);
    assert(!devices[1].registered && !dcps[1].external_prepared && disables == 1);
    try_bind();
    assert(matches.includes[1] && bind_attempts == 0 && !probes[0]);

    /* An unavailable DT node is excluded; a matched but absent device is fatal. */
    initial(true);
    nodes[1].available = false;
    assert(!apple_dcp_usable(&nodes[1]) && match_providers() == 1);
    initial(true);
    absent_external = true;
    assert(match_providers() == 2);
    assert(apple_drm_init_dcp(&master) == -ENODEV && !probes[1]);

    /* Readiness rejects every unavailable provider state and releases lookup refs. */
    for (unsigned int state = 0; state < 4; state++) {
        initial(true);
        dcps[1].external_prepared = true;
        if (state == 0) devices[1].dev.data = NULL;
        if (state == 1) dcps[1].external_prepared = false;
        if (state == 2) dcps[1].quiescing = true;
        if (state == 3) dcps[1].crashed = true;
        assert(!dcp_h17p_external_ready(&nodes[1]));
        assert(references == 1 && released == 1);
        assert(apple_drm_init_dcp(&master) == -ENODEV && !probes[1]);
    }
    initial(true);
    dcps[1].external_prepared = true;
    external_probe_error = -EIO;
    assert(apple_drm_init_dcp(&master) == -EIO);
    assert(probes[1] == 1 && node_puts == 1);
    puts("PASS: opt-in matching, delayed/preflight-failed providers, synchronous publication, readiness rejection");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='apple-dcp-readiness-') as directory:
    path = Path(directory)
    unit = path / 'test.c'
    unit.write_text(prefix + production + suffix)
    command = shlex.split(os.environ.get('CC', 'clang'))
    command += ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command + [str(unit), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
