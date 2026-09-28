#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual optional direct-PHY crossbar bridge and error paths."""
from pathlib import Path
import os,subprocess,tempfile
root=next(p for p in Path(__file__).resolve().parents if (p/'drivers/gpu/drm/apple/dcp.c').exists())
s=(root/'drivers/gpu/drm/apple/dcp.c').read_text();a=s.index('int dcp_direct_crossbar_link(');b=s.index('\n}',a)+2
source=r'''
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
struct phy{};struct mux_control{};
struct apple_dcp{bool dptx_tunnel;struct phy*phy;struct mux_control*xbar;};
static bool have_apple_t6050_dp_link_config=true,have_apple_dpxbar_link_configure=true;
static int get_error,configure_error,link_error,gets,configs,links;static bool last_up;
static int apple_t6050_dp_link_config(struct phy*p,unsigned*c,bool*u){gets++;*c=1;*u=false;return get_error;}
static int apple_dpxbar_link_configure(struct mux_control*m,unsigned c,bool u){assert(c==1&&!u);configs++;return configure_error;}
static int dcp_dpxbar_link(struct mux_control*m,bool up){links++;last_up=up;return link_error;}
#define symbol_get(n) (have_##n ? n : NULL)
#define symbol_put(n) ((void)0)
'''+s[a:b]+r'''
int main(void){
struct phy p;struct mux_control m;struct apple_dcp d={.phy=&p,.xbar=&m};
assert(!dcp_direct_crossbar_link(&d,true)&&gets==1&&configs==1&&links==1&&last_up);
assert(!dcp_direct_crossbar_link(&d,false)&&configs==1&&links==2&&!last_up);
get_error=-ENOLINK;assert(!dcp_direct_crossbar_link(&d,false)&&links==3);
assert(dcp_direct_crossbar_link(&d,true)==-ENOLINK&&configs==1&&links==3);
get_error=-EIO;assert(dcp_direct_crossbar_link(&d,true)==-EIO&&links==3);
get_error=-EOPNOTSUPP;assert(!dcp_direct_crossbar_link(&d,true)&&links==3);
get_error=0;configure_error=-EBUSY;assert(dcp_direct_crossbar_link(&d,true)==-EBUSY&&links==3);
configure_error=0;link_error=-EIO;assert(dcp_direct_crossbar_link(&d,true)==-EIO&&links==4);
have_apple_dpxbar_link_configure=false;assert(dcp_direct_crossbar_link(&d,true)==-ENODEV&&links==4);
have_apple_t6050_dp_link_config=false;assert(!dcp_direct_crossbar_link(&d,true)&&links==4);
have_apple_t6050_dp_link_config=true;int n=gets;d.dptx_tunnel=true;assert(!dcp_direct_crossbar_link(&d,true)&&gets==n);
d.dptx_tunnel=false;d.phy=NULL;assert(!dcp_direct_crossbar_link(&d,true)&&gets==n);
d.phy=&p;d.xbar=NULL;assert(!dcp_direct_crossbar_link(&d,true)&&gets==n);
puts("DCP_DIRECT_LINK_TEST_PASS");return 0;
}
'''
if os.environ.get('TEST_SOURCE'):Path(os.environ['TEST_SOURCE']).write_text(source)
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(source)
 subprocess.run(['clang','-std=gnu11','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
