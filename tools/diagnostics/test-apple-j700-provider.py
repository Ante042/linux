#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual provider callback against missing and malformed properties."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/gpu/drm/apple/iomfb_h17p.c").read_text()
body = source[source.index("struct dcp_h17p_provider_request {"):
              source.index("static bool trampoline_luma_span_h17p")]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint32_t __le32;
#define __packed __attribute__((packed))
#undef static_assert
#define static_assert(cond) _Static_assert(cond, "")
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define sizeof_field(t, f) sizeof(((t *)0)->f)
#define le32_to_cpu(x) (x)
#define cpu_to_le32(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define DCP_IOMFB_METHODS_H17G 1
#define DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL 2
#define trace_iomfb_callback(d, t, n) ((void)(d), (void)(t))
#define dev_err(...) ((void)0)
#define dev_warn_once(...) ((void)0)
struct device_node { bool internal, external, present; int length; };
struct device { struct device_node *of_node; };
struct apple_dcp { struct { int iomfb_method_profile; } hw; struct device *dev; bool crashed; };
static bool j700 = true;
static bool of_machine_is_compatible(const char *name)
{
	assert(!strcmp(name, "apple,j700"));
	return j700;
}
static bool of_device_is_compatible(struct device_node *n, const char *name)
{
	return (!strcmp(name, "apple,t8140-dcp") && n->internal) ||
	       (!strcmp(name, "apple,t8140-dcpext") && n->external);
}
static bool of_property_present(struct device_node *n, const char *name)
{
	(void)name;
	return n->present;
}
static int of_property_count_u8_elems(struct device_node *n, const char *name)
{
	(void)name;
	return n->present ? n->length : -EINVAL;
}
static int of_property_read_u8_array(struct device_node *n, const char *name, void *p, unsigned int len)
{
	(void)n; (void)name;
	memset(p, 0x5a, len);
	return 0;
}
static const void *memchr_inv(const void *p, int value, size_t size)
{
	const u8 *b = p;
	for (size_t i = 0; i < size; i++)
		if (b[i] != value)
			return b + i;
	return NULL;
}
'''
suffix = r'''
int main(void)
{
	struct device_node node = { .external = true };
	struct device dev = { .of_node = &node };
	struct apple_dcp dcp = { .dev = &dev, .hw.iomfb_method_profile = 2 };
	struct dcp_h17p_provider_request req = { .scope = { 'V', 'O', 'R', 'P' }, .capacity = 0xc00 };
	struct dcp_h17p_provider_reply reply;
	unsigned int cases = 0;
	for (unsigned int i = 0; i < ARRAY_SIZE(provider_properties); i++) {
		memset(&reply, 0xff, sizeof(reply));
		strcpy(req.name, provider_properties[i].name);
		assert(trampoline_provider_property_h17p(&dcp, 400, &reply, &req));
		assert(!dcp.crashed && reply.length == provider_properties[i].length);
		if (reply.length)
			assert(!memcmp(reply.data, provider_properties[i].legacy_data, reply.length));
		cases++;
	}
	strcpy(req.name, "power-lut-data-x");
	j700 = false;
	assert(!trampoline_provider_property_h17p(&dcp, 400, &reply, &req) && dcp.crashed);
	j700 = true; dcp.crashed = false; dcp.hw.iomfb_method_profile = 3;
	assert(!trampoline_provider_property_h17p(&dcp, 400, &reply, &req) && dcp.crashed);
	node.external = false; node.internal = true; dcp.crashed = false;
	assert(trampoline_provider_property_h17p(&dcp, 400, &reply, &req) && !dcp.crashed);
	node.internal = false; node.external = true; node.present = true; node.length = 3;
	dcp.hw.iomfb_method_profile = 2; dcp.crashed = false;
	assert(!trampoline_provider_property_h17p(&dcp, 400, &reply, &req) && dcp.crashed);
	node.length = 4; dcp.crashed = false;
	assert(trampoline_provider_property_h17p(&dcp, 400, &reply, &req) && !dcp.crashed);
	assert(reply.length == 4 && reply.data[0] == 0x5a);
	strcpy(req.name, "unqualified-property");
	assert(!trampoline_provider_property_h17p(&dcp, 400, &reply, &req));
	printf("PASS %u provider payloads and scope/length rejection cases\n", cases);
	return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="j700-provider-") as work:
    work = Path(work)
    test = work / "test.c"
    test.write_text(prefix + body + suffix)
    command = shlex.split(os.environ.get("CC", "clang"))
    command += ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
    command += ["-fsanitize=address,undefined", str(test), "-o", str(work / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(work / "test")], check=True,
                   env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
