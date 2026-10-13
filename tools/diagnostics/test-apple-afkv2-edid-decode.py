#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Independent synthetic bytes through the production-code trace decoder."""
from pathlib import Path
import json
import os
import shlex
import struct
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
cc=shlex.split(os.environ.get('HOSTCC','clang'))
def packet(body, *, category=2, oob=False, status=0, token=0, interface=1):
    command=struct.pack('<BBHI',int(oob),0x10,0,status)+body
    return struct.pack('<BBHIQBBHI',0x6f,0x24,interface,len(command)+16,token,0xc0,category,0x0201,0x12345678)+command

def service(body, *, length=None, group=1, command=7, magic=0x69706378):
    return struct.pack('<HHIII48x',0,group,command,len(body) if length is None else length,magic)+body

def edid_header(maximum=32776, used=0):
    return struct.pack('<Q24xQ8x',maximum,used)

with tempfile.TemporaryDirectory(prefix='afkv2-edid-decode-test-') as tmp:
    executable=Path(tmp)/'decoder'
    subprocess.run(cc+['-std=c11','-Wall','-Wextra','-Werror','-O1','-g',
                       '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                       '-I'+str(root/'tools/testing/selftests/drivers/gpu/apple-afkv2/host-include'),
                       '-I'+str(root/'drivers/gpu/drm/apple'),
                       str(root/'tools/diagnostics/apple-afkv2-edid-decode.c'),'-o',str(executable)],check=True)
    def decode(data, declared=None, extra=()):
        result=subprocess.run([str(executable),str(len(data) if declared is None else declared),*extra],input=data,capture_output=True,check=True)
        return json.loads(result.stdout)
    empty=packet(service(edid_header()))
    assert len(empty)==144
    result=decode(empty)
    assert result['frame']['interface']==1 and result['frame']['token']=='0x0000000000000000'
    assert result['command']==dict(oob=False,id=16,value=0,value_role='status',inline_declared=112,inline_captured=112)
    assert result['service']['data_len']==48 and result['edid']['max_size']==32776 and result['edid']['used_size']==0
    assert result['classification']=='empty_edid_reply_rejected' and result['production_edid_rc']==-5
    assert result['production_inline_shape_rc']==0 and result['production_oob_shape_rc']!=0
    # Prefix-only capture does not invent the other 112 bytes or zero-valued fields.
    result=decode(empty[:32],144)
    assert result['missing_bytes']==112 and 'service' not in result and 'edid' not in result
    assert result['production_inline_shape_rc'] is None and result['production_oob_shape_rc'] is None
    assert result['command']['inline_declared']==112 and result['command']['inline_captured']==0
    assert result['classification']=='service_header_not_captured' and result['production_frame_rc']!=0
    # Header-only inline response is distinct from an OOB descriptor.
    result=decode(packet(b''))
    assert result['classification']=='no_inline_service_payload' and not result['command']['oob']
    descriptor=struct.pack('<QQII',0xffff000000001000,0xffff000000002000,32888,112)
    result=decode(packet(descriptor,oob=True))
    assert result['classification']=='oob_descriptor_payload_not_captured'
    assert result['oob']['rx_size']==32888 and result['oob']['memory_followed'] is False
    assert result['oob']['live_slot_ownership']=='not_available_offline'
    assert result['production_oob_shape_rc']==0 and result['production_inline_shape_rc']!=0 and 'service' not in result and 'edid' not in result
    result=decode(packet(descriptor,oob=True),extra=('0xffff000000001000','32888','0xffff000000002000','112'))
    assert result['oob']['supplied_request_descriptors_match'] is True
    result=decode(packet(descriptor,oob=True),extra=('0x1000','32888','0x2000','112'))
    assert result['oob']['supplied_request_descriptors_match'] is False
    result=decode(packet(descriptor+b'\0',oob=True))
    assert result['production_oob_shape_rc']!=0 # Success cannot smuggle an inline tail.
    result=decode(packet(service(edid_header(used=136))))
    assert result['classification']=='edid_data_absent_rejected' and result['production_edid_rc']==-5
    result=decode(packet(service(edid_header(),group=2)))
    assert result['classification']=='not_matched_edid_service_reply' and 'edid' not in result
    result=decode(packet(service(edid_header(),length=0)))
    assert result['classification']=='edid_header_absent_or_truncated' and 'edid' not in result
    result=decode(packet(service(edid_header()),status=0xe00002c7))
    assert result['classification']=='nonzero_transport_status' and 'edid' not in result
    result=decode(packet(service(edid_header()),token=1))
    assert result['production_inline_shape_rc']!=0 and 'production_edid_rc' not in result
    result=decode(packet(service(edid_header()),category=0))
    assert result['classification']=='report_not_command_response' and 'command' not in result
    result=decode(packet(service(edid_header()),interface=0))
    assert result['classification']=='driver_rejected_zero_interface'
    # A real base block requires 280 total bytes: 256 trace bytes cannot prove its checksum.
    body=edid_header(used=136)+bytes(8)+bytes(128)
    large=packet(service(body));assert len(large)==280
    result=decode(large[:256],280)
    assert result['missing_bytes']==24 and result['edid']['used_size']==136
    assert result['classification']=='incomplete_or_malformed_service_reply' and 'production_edid_rc' not in result
    for length in range(32):
        result=decode(empty[:length],144)
        assert result['record_complete'] is False and 'edid' not in result
    bad=bytearray(empty);struct.pack_into('<I',bad,4,1000)
    assert decode(bad)['classification']=='malformed_frame'
    for data,declared in [(bytes(257),257),(bytes(32),31)]:
        result=subprocess.run([str(executable),str(declared)],input=data,capture_output=True)
        assert result.returncode==2
    # Full trace-line parser preserves endpoint/channel/type and validates count.
    trace=Path(tmp)/'trace.txt'
    trace.write_text('synthetic: afk_recv_header: dcp: endpoint 0x2a channel 0x0 type 0x0 size 144 header[144]='+empty.hex(' ')+'\n')
    decoded=json.loads(subprocess.check_output(['python3',str(root/'tools/diagnostics/decode-apple-afkv2-trace.py'),str(trace)]))
    assert decoded['endpoint']==0x2a and decoded['classification']=='empty_edid_reply_rejected'
    trace.write_text('synthetic: afk_recv_header: dcp: endpoint 0x2a channel 0x0 type 0x0 size 144 header[143]='+empty.hex(' ')+'\n')
    assert subprocess.run(['python3',str(root/'tools/diagnostics/decode-apple-afkv2-trace.py'),str(trace)],capture_output=True).returncode!=0
print('PASS: actual production-code decoder, LE fields, complete/partial/absent inline replies, OOB descriptor-only/no memory access, supplied ranges, strict service/EDID checks, status/token/report distinctions, 256-byte bounds')
