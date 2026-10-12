#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production link ceiling callback and its isolation boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/gpu/drm/apple/dptxep.c").read_text()
start = source.index("static int dptxport_call_get_max_link_rate(")
end = source.index("\n}\n", start) + 3
stub = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
typedef uint32_t u32;
#define LINK_RATE_RBR 6
#define LINK_RATE_HBR 10
#define LINK_RATE_HBR2 20
#define LINK_RATE_HBR3 30
#define DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL 7
#define READ_ONCE(x) (x)
#define cpu_to_le32(x) (x)
struct apple_dcp { struct { int iomfb_method_profile; } hw; };
struct apple_epic_endpoint { struct apple_dcp *dcp; };
struct apple_epic_service { struct apple_epic_endpoint *ep; void *cookie; };
struct dptx_port { void *atcphy; };
struct dptxport_apcall_link_rate { u32 retcode; char pad[12]; u32 link_rate; char tail[12]; };
static u32 j700_dp_max_link_rate;
static bool j700;
static bool of_machine_is_compatible(const char *name) { (void)name; return j700; }
"""
main = r"""
int main(void) {
    struct apple_dcp dcp = {};
    struct apple_epic_endpoint ep = { &dcp };
    struct dptx_port port = {};
    struct apple_epic_service service = { &ep, &port };
    struct dptxport_apcall_link_rate reply;
    unsigned cases = 0;
    for (unsigned rate = 0; rate <= 40; rate++)
    for (unsigned scope = 0; scope < 8; scope++) {
        j700 = scope & 1;
        port.atcphy = (scope & 2) ? &port : NULL;
        dcp.hw.iomfb_method_profile = (scope & 4) ? 7 : 0;
        j700_dp_max_link_rate = rate;
        assert(dptxport_call_get_max_link_rate(&service, &reply, sizeof(reply)) == 0);
        bool valid = rate == 6 || rate == 10 || rate == 20 || rate == 30;
        assert(reply.link_rate == ((scope == 7 && valid) ? rate : 30));
        assert(reply.retcode == 0);
        cases++;
    }
    service.cookie = NULL;
    assert(!dptxport_call_get_max_link_rate(&service, &reply, sizeof(reply)));
    assert(reply.link_rate == 30);
    for (unsigned size = 0; size < sizeof(reply); size++)
        assert(dptxport_call_get_max_link_rate(&service, &reply, size) == -EINVAL);
    return cases != 328;
}
"""
with tempfile.TemporaryDirectory(prefix="j700-rate-") as tmp:
    c = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    c.write_text(stub + source[start:end] + main)
    subprocess.run(["clang", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("328 link ceilings and scope cases, null port and all short replies: PASS")
