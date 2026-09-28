#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import os, subprocess, tempfile
scratch = tempfile.TemporaryDirectory(prefix='apple-dcp-test-')
I = Path(scratch.name)
root = next((p for p in Path(__file__).resolve().parents
             if (p / 'drivers/gpu/drm/apple/afk.c').is_file()), None)
if os.environ.get('KERNEL_SRC'):
    root = Path(os.environ['KERNEL_SRC'])
if root is None:
    print('SKIP: set KERNEL_SRC to the Linux source tree')
    raise SystemExit(4)
s=(root/'drivers/gpu/drm/apple/iomfb_template.c').read_text()
def fn(name):
 a=s.index('static void '+name+'(');b=s.index('\n}',a)+2;return s[a:b]
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
#define DCP_FW_VERSION(a,b,c) ((a)*10000+(b)*100+(c))
#define DCP_FW_VER DCP_FW_VERSION(27,0,0)
#define DCP_CONTEXT_CB 1
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
struct dcp_dfb_dimensions_h17p {u32 width,height;};
struct dcp_dfb_layout_req_h17p {u32 data[3];};
struct dcp_channel{int depth;void *output[1];};
struct apple_dcp{struct dcp_channel ch_cb;};
static int step,acks;static u8 callback_result=1;
static struct dcp_dfb_dimensions_h17p dimensions;
static void boot_2(struct apple_dcp*d,void*o,void*c){(void)d;(void)o;(void)c;step=2;}
static void boot_1_11(struct apple_dcp*d,void*o,void*c){(void)d;(void)o;(void)c;}
static void boot_1_7(struct apple_dcp*d,void*o,void*c){(void)d;(void)o;(void)c;}
static void dcp_ack(struct apple_dcp*d,int c){(void)d;assert(c==1);acks++;}
static void dcp_set_dfb_dimensions(struct apple_dcp*d,bool b,struct dcp_dfb_dimensions_h17p*x,void(*cb)(struct apple_dcp*,void*,void*),void*c){(void)d;(void)b;(void)c;assert(cb==boot_1_11);dimensions=*x;step=104;}
'''
mid=r'''
static void dcp_get_dfb_state(struct apple_dcp*d,bool b,u32*x,void(*cb)(struct apple_dcp*,void*,void*),void*c){(void)d;(void)b;(void)c;assert(*x==0&&cb==boot_1_10);step=453;}
'''
mid2=r'''
static void dcp_get_dfb_layout(struct apple_dcp*d,bool b,struct dcp_dfb_layout_req_h17p*x,void(*cb)(struct apple_dcp*,void*,void*),void*c){(void)d;(void)b;(void)x;(void)c;assert(cb==boot_1_9);step=103;}
static void dcp_get_dfb_compression_info(struct apple_dcp*d,bool b,u64*x,void(*cb)(struct apple_dcp*,void*,void*),void*c){(void)d;(void)b;(void)c;assert(*x==0&&cb==boot_1_7);step=37;}
'''
post=r'''
int main(void){
struct apple_dcp d={.ch_cb={.depth=1,.output={&callback_result}}};
u32 has_fb=0;boot_1_6(&d,&has_fb,0);assert(step==103);has_fb=1;boot_1_6(&d,&has_fb,0);assert(step==37);
struct dcp_dfb_dimensions_h17p dim={3840,2160};boot_1_9(&d,&dim,0);assert(step==2);
dim.width=0;boot_1_9(&d,&dim,0);assert(step==453);
dim.height=0;boot_1_10(&d,&dim,0);assert(step==104&&!acks&&!dimensions.width&&!dimensions.height);
dim.width=3024;dim.height=1964;boot_1_10(&d,&dim,0);assert(dimensions.width==3024&&dimensions.height==1964&&!acks);
puts("PASS: firmware27 native boot follows A454 presence and A103 geometry branches; zero external geometry reaches A104, internal dimensions preserved");
}
'''
p=I/'test-dcp27-native-boot.c';p.write_text(pre+fn('boot_1_10')+mid+fn('boot_1_9')+mid2+fn('boot_1_6')+post)
subprocess.run([os.environ.get('HOSTCC', 'cc'),'-std=gnu11','-O1','-g','-fsanitize=address,undefined',str(p),'-o',str(I/'test-dcp27-native-boot')],check=True)
subprocess.run([str(I/'test-dcp27-native-boot')],check=True)
