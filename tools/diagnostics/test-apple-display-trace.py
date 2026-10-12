#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host checks for bounded trace assignments and unchanged color callback flow."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[2])
args = parser.parse_args()
root = args.source_root / 'drivers/gpu/drm/apple'
trace = (root / 'trace.h').read_text()
template = (root / 'iomfb_template.c').read_text()

def trace_assignment(name):
    start = trace.index('TRACE_EVENT(' + name + ',')
    start = trace.index('TP_fast_assign(', start) + len('TP_fast_assign(')
    end, depth = start, 1
    while depth:
        depth += (trace[end] == '(') - (trace[end] == ')')
        end += 1
    return trace[start:end - 1]

def function(name):
    match = re.search(r'static void ' + name + r'\([^;]*?\)\n\{', template)
    if not match:
        raise RuntimeError('missing callback: ' + name)
    end, depth = match.end(), 1
    while depth:
        depth += (template[end] == '{') - (template[end] == '}')
        end += 1
    return template[match.start():end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define __assign_str(x) ((void)0)
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
struct apple_dcp { int unused; };
struct apple_dcp_afkep { u8 endpoint; };
struct header_entry { u8 endpoint;u32 channel,type;size_t data_size;u8 captured,header[32]; };
struct ctm_entry { u32 location;u8 matrix_null;u64 matrix[9]; };
static unsigned int calls, swapped;static u32 methods[2],results[2];static bool valids[2];
static void *swap_cookie,*reply_data;static u8 ack[5];static bool null_ack;
static u32 get_unaligned_le32(const void *p) { const u8 *b=p;return b[0]|(u32)b[1]<<8|(u32)b[2]<<16|(u32)b[3]<<24; }
static void trace_iomfb_control_result(struct apple_dcp *d,u32 m,bool valid,u32 result)
{ (void)d;assert(calls<2);methods[calls]=m;valids[calls]=valid;results[calls++]=result; }
static void do_swap(struct apple_dcp *d,void *out,void *cookie)
{ (void)d;swapped++;swap_cookie=cookie;reply_data=out; }
static void dcp_pipe_set_428(struct apple_dcp *d,bool oob,const u32 *identity,
 void (*cb)(struct apple_dcp*,void*,void*),void *cookie)
{ assert(!oob && *identity==0x10000);cb(d,null_ack?NULL:ack+1,cookie); }
'''
code = prefix
code += 'static void capture(struct header_entry *__entry,struct apple_dcp_afkep *ep,u32 channel,u32 type,size_t data_size,const void *data) {\n' + trace_assignment('afk_recv_header') + '\n}\n'
code += 'static void capture_ctm(struct ctm_entry *__entry,u32 location,const void *matrix,u8 matrix_null) {\n' + trace_assignment('iomfb_ctm') + '\n}\n'
code += function('dcp_post_swap_a428') + '\n' + function('dcp_pre_swap_a428')
code += r'''
int main(void)
{
 struct apple_dcp_afkep ep={.endpoint=0x28};struct header_entry e;size_t n,i;
 for(n=0;n<=64;n++) {
  u8 *p=malloc(n?n:1);for(i=0;i<n;i++)p[i]=(u8)i;
  memset(&e,0xa5,sizeof(e));capture(&e,&ep,7,9,n,p);
  assert(e.endpoint==0x28 && e.channel==7 && e.type==9 && e.data_size==n);
  assert(e.captured==(n<32?n:32));
  for(i=0;i<e.captured;i++)assert(e.header[i]==i);
  for(;i<32;i++)assert(e.header[i]==0);
  free(p);
 }
 capture(&e,&ep,0,0,32,NULL);assert(e.captured==0);
 for(i=0;i<32;i++)assert(e.header[i]==0);
 {
  struct ctm_entry c;u8 matrix[73];memset(matrix,0x5a,sizeof(matrix));
  capture_ctm(&c,9,matrix+1,0);assert(c.location==9 && !c.matrix_null);
  assert(!memcmp(c.matrix,matrix+1,72));
  capture_ctm(&c,9,NULL,1);assert(c.matrix_null);
  for(i=0;i<9;i++)assert(!c.matrix[i]);
 }
 {
  struct apple_dcp d={0};u8 mat[5]={0,1,2,3,4};int cookie;
  ack[1]=5;ack[2]=6;ack[3]=7;ack[4]=8;
  dcp_pre_swap_a428(&d,mat+1,&cookie);
  assert(calls==2 && methods[0]==423 && methods[1]==428 && valids[0] && valids[1]);
  assert(results[0]==0x04030201 && results[1]==0x08070605);
  assert(swapped==1 && swap_cookie==&cookie && reply_data==ack+1);
  calls=0;null_ack=true;dcp_pre_swap_a428(&d,NULL,&cookie);
  assert(calls==2 && !valids[0] && !valids[1] && !results[0] && !results[1]);
  assert(swapped==2 && swap_cookie==&cookie && reply_data==NULL);
 }
 puts("PASS: actual trace assignments bounded/null-safe; CTM nine coefficients; A423/A428 callback flow preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='apple-display-trace-') as tmp:
    source = Path(tmp) / 'trace-test.c'
    output = Path(tmp) / 'trace-test'
    source.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(source), '-o', str(output)], check=True)
    subprocess.run([str(output)], check=True)
