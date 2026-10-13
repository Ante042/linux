#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Decode bounded AFK trace prefixes offline using production codec/EDID helpers."""
from pathlib import Path
import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('trace', nargs='?', type=Path, help='trace text (default stdin)')
parser.add_argument('--endpoint', type=lambda text:int(text,0), default=0x2a)
parser.add_argument('--owned-rx', nargs=2, metavar=('BASE','CAPACITY'), help='optional original request descriptor; never followed')
parser.add_argument('--owned-tx', nargs=2, metavar=('BASE','CAPACITY'))
args=parser.parse_args()
if bool(args.owned_rx)!=bool(args.owned_tx):
    parser.error('supply both original request descriptors or neither')
root=Path(__file__).resolve().parents[2]
pattern=re.compile(r'afk_recv_header:.*?endpoint (0x[0-9a-fA-F]+) channel (0x[0-9a-fA-F]+) type (0x[0-9a-fA-F]+) size ([0-9]+) header\[([0-9]+)\]=(.*)$')
cc=shlex.split(os.environ.get('HOSTCC','clang'))
with tempfile.TemporaryDirectory(prefix='afkv2-decode-') as tmp:
    executable=Path(tmp)/'decoder'
    subprocess.run(cc+['-std=c11','-Wall','-Wextra','-Werror','-O2',
                       '-I'+str(root/'tools/testing/selftests/drivers/gpu/apple-afkv2/host-include'),
                       '-I'+str(root/'drivers/gpu/drm/apple'),
                       str(root/'tools/diagnostics/apple-afkv2-edid-decode.c'),'-o',str(executable)],check=True)
    stream=args.trace.open() if args.trace else sys.stdin
    try:
        for number,line in enumerate(stream,1):
            match=pattern.search(line)
            if not match or int(match[1],0)!=args.endpoint:
                continue
            count=int(match[5]); data=bytes.fromhex(match[6].strip())
            if count!=len(data) or count>256:
                raise ValueError(f'line {number}: captured count mismatch or over limit')
            command=[str(executable),match[4]]
            if args.owned_rx:
                command+=args.owned_rx+args.owned_tx
            decoded=json.loads(subprocess.check_output(command,input=data))
            decoded.update(trace_line=number,endpoint=int(match[1],0),queue_channel=int(match[2],0),queue_type=int(match[3],0))
            if decoded['queue_channel'] or decoded['queue_type']:
                decoded['linux_v2_queue_envelope']='rejected_nonzero_channel_or_type'
            else:
                decoded['linux_v2_queue_envelope']='zero_channel_and_type'
            print(json.dumps(decoded,sort_keys=True))
    finally:
        if args.trace:
            stream.close()
