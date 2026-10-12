#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check that the actual property queue preserves panel behavior, excludes DCPEXT."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/gpu/drm/apple/iomfb.c").read_text()
start = source.index("static int iomfb_enqueue_opaque_x(")
end = source.index("\n}\n", start) + 3
prefix = r"""
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#define DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL 2
enum { IOMFB_OPAQUE_X_WAITING, IOMFB_OPAQUE_X_NEEDED,
       IOMFB_OPAQUE_X_QUEUED, IOMFB_OPAQUE_X_READY };
struct list_head { unsigned count; };
struct apple_dcp;
struct iomfb_transaction {
    void (*start)(struct apple_dcp *, struct iomfb_transaction *);
    void (*release)(struct iomfb_transaction *);
    struct list_head link;
};
struct iomfb_opaque_x_transaction { struct iomfb_transaction transaction; };
struct apple_dcp {
    struct { int iomfb_method_profile; } hw;
    struct { int lock, opaque_x_state; unsigned queued; struct list_head pending; } iomfb;
};
static unsigned allocations;
static int fail_alloc;
static void *last_allocation;
static void *allocate(size_t n) { allocations++; return last_allocation = fail_alloc ? NULL : calloc(1, n); }
#define kzalloc_obj(x) allocate(sizeof(x))
#define kfree(x) free(x)
#define lockdep_assert_held(x) ((void)(x))
#define atomic_read(x) (*(x))
static int atomic_cmpxchg(int *p, int from, int to)
{ int old = *p; if (old == from) *p = to; return old; }
static void list_add(struct list_head *n, struct list_head *h)
{ (void)n; h->count++; }
static void iomfb_opaque_x_start(struct apple_dcp *d, struct iomfb_transaction *t)
{ (void)d; (void)t; }
static void iomfb_opaque_x_release(struct iomfb_transaction *t) { (void)t; }
"""
suffix = r"""
int main(void)
{
    for (int state = 0; state < 4; state++)
    for (unsigned queued = 0; queued <= 32; queued++)
    for (int profile = 0; profile < 3; profile++) {
        struct apple_dcp d = { .hw.iomfb_method_profile = profile,
            .iomfb = { .opaque_x_state = state, .queued = queued } };
        last_allocation = NULL;
        unsigned before = allocations;
        int ret = iomfb_enqueue_opaque_x(&d);
        if (profile == 2 || state != IOMFB_OPAQUE_X_NEEDED) {
            assert(!ret && allocations == before);
            assert(d.iomfb.queued == queued && d.iomfb.opaque_x_state == state);
        } else if (queued == 32) {
            assert(ret == -EBUSY && allocations == before);
        } else {
            assert(!ret && allocations == before + 1);
            assert(d.iomfb.queued == queued + 1 && d.iomfb.pending.count == 1);
            assert(d.iomfb.opaque_x_state == IOMFB_OPAQUE_X_QUEUED);
        }
        free(last_allocation);
    }
    struct apple_dcp d = { .iomfb.opaque_x_state = IOMFB_OPAQUE_X_NEEDED };
    fail_alloc = 1;
    assert(iomfb_enqueue_opaque_x(&d) == -ENOMEM);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="j700-property-") as tmp:
    c = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    c.write_text(prefix + source[start:end] + suffix)
    subprocess.run(["clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("396 actual queue scope/state/capacity cases and allocation failure: PASS")
