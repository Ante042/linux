#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the production dispatcher and check the sole D134 reply exception."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/gpu/drm/apple/iomfb.c").read_text()
start = source.index("static bool dcp_external_zero_reply(")
end = source.index("static void dcpep_handle_ack(", start)
prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define DCP_FIRMWARE_H17P 1
#define DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL 2
#define DCP_IOMFB_METHODS_H17G 3
#define DCP_MAX_CALL_DEPTH 8
#define IOMFB_MAX_CB 600
#define DCP_PACKET_ALIGNMENT 64
#define ALIGN(n, a) (((n) + (a) - 1) & ~((a) - 1))
#define dev_err(...) ((void)dev)
#define dev_info(...) ((void)dev)
#define dev_warn(...) ((void)dev)
enum dcp_context_id { DCP_CONTEXT_CB = 0, DCP_CONTEXT_ASYNC = 3 };
struct device_node { bool external; };
struct device { struct device_node *of_node; };
struct dcp_packet_header { char tag[4]; u32 in_len, out_len; };
struct dcp_channel { unsigned depth; void *output[8]; unsigned end[8]; };
struct apple_dcp {
    struct { int iomfb_method_profile; } hw;
    struct device *dev;
    int fw_compat;
    unsigned experimental_callback_logs;
    bool crashed;
    bool (**cb_handlers)(struct apple_dcp *, int, void *, void *);
    unsigned long unknown_callbacks[16];
    int start_done;
    struct dcp_channel channel;
};
static bool machine;
static unsigned acks, case_tag;
static struct dcp_channel *dcp_get_channel(struct apple_dcp *d, enum dcp_context_id c)
{ (void)c; return &d->channel; }
static int dcp_parse_tag(char *p) { (void)p; return case_tag; }
static bool of_machine_is_compatible(const char *n) { (void)n; return machine; }
static bool of_device_is_compatible(struct device_node *n, const char *s)
{ (void)s; return n->external; }
static bool iomfb_check_callback_h17p(struct apple_dcp *d, int t, u32 i, u32 o)
{ (void)d; (void)t; (void)i; (void)o; return true; }
static bool iomfb_uses_queue(struct apple_dcp *d) { (void)d; return false; }
static bool test_and_set_bit(int t, unsigned long *b) { (void)t; (void)b; return false; }
static void complete(int *p) { *p = 1; }
static u8 dcp_push_depth(unsigned *d) { return (*d)++; }
static void dcp_ack(struct apple_dcp *d, enum dcp_context_id c)
{ (void)c; acks++; d->channel.depth--; }
"""
suffix = r"""
int main(void)
{
    struct device_node node;
    struct device dev = { &node };
    bool (*handlers[IOMFB_MAX_CB])(struct apple_dcp *, int, void *, void *) = {};
    unsigned cases = 0;
    for (unsigned scope = 0; scope < 8; scope++)
    for (unsigned tag = 133; tag <= 135; tag++)
    for (unsigned context = 0; context < 8; context++)
    for (unsigned in = 0; in <= 8; in++)
    for (unsigned out = 0; out <= 8; out++) {
        struct apple_dcp d = {
            .dev = &dev, .fw_compat = 1, .experimental_callback_logs = 64,
            .cb_handlers = handlers, .hw.iomfb_method_profile = 2,
        };
        union { uint64_t align; unsigned char data[64]; } packet;
        struct dcp_packet_header *h = (void *)packet.data;
        memset(&packet, 0xa5, sizeof(packet));
        h->in_len = in; h->out_len = out;
        machine = scope & 1; node.external = scope & 2;
        if (!(scope & 4))
            d.hw.iomfb_method_profile = 0;
        case_tag = tag; acks = 0;
        dcpep_handle_cb(&d, context, h, sizeof(*h) + in + out, 0);
        bool accepted = scope == 7 && tag == 134 && context == 3 && !in && out == 4;
        if (scope & 4) {
            assert(acks == accepted && d.crashed != accepted);
            for (unsigned i = 0; i < out; i++)
                assert(packet.data[sizeof(*h) + in + i] == (accepted ? 0 : 0xa5));
        } else {
            assert(acks == 1 && !d.crashed); /* Existing legacy behavior. */
        }
        assert(packet.data[sizeof(*h) + in + out] == 0xa5);
        cases++;
    }
    return cases != 15552;
}
"""
with tempfile.TemporaryDirectory(prefix="j700-d134-") as tmp:
    c = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    c.write_text(prefix + source[start:end] + suffix)
    subprocess.run(["clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("15552 dispatcher cases: D134 bounds/scope, rejection and legacy replies PASS")
