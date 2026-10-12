#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Aurora Silicon
"""Read back only the J700 ATC words selected by its DP setup lists.

No register is written. Output excludes calibration values. Run --selftest
without hardware access; --read-hardware requires permission to read /dev/mem.
"""
import argparse
import json
import mmap
import os
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path('/sys/firmware/devicetree/base')
PHY = ROOT / 'soc/phy@40b000000'
GROUPS = (
    ('apple,tunable-dp-common-pre', 'core'),
    ('apple,tunable-dp-lpdptx', 'lpdptx'),
    ('apple,tunable-dp-common-post', 'core'),
)


def decode(data, size):
    if len(data) > 65536 or len(data) % 12 or size < 4:
        raise ValueError('invalid setup list size')
    result = list(struct.iter_unpack('>III', data))
    for offset, mask, value in result:
        if offset % 4 or offset > size - 4:
            raise ValueError('setup word outside resource')
    return result


def compose(groups):
    expected = {}
    for base, words in groups:
        for offset, mask, value in words:
            address = base + offset
            known, prior = expected.get(address, (0, 0))
            expected[address] = (known | mask | value, (prior & ~mask) | value)
    if len(expected) > 256:
        raise ValueError('readback word limit exceeded')
    return expected


def read_word(fd, address):
    if address % 4:
        raise ValueError('unaligned word address')
    page = address - address % mmap.PAGESIZE
    with mmap.mmap(fd, mmap.PAGESIZE, flags=mmap.MAP_SHARED,
                   prot=mmap.PROT_READ, offset=page) as view:
        return struct.unpack_from('<I', view, address - page)[0]


def aux_state(analog, supply):
    return {
        'analog_power_down': bool(analog & 1),
        'small_supply': bool(supply & 1),
        'small_override': bool(supply & 2),
        'large_supply': bool(supply & 4),
        'large_override': bool(supply & 8),
        'clamp_override': bool(supply & 256),
        'clamped': bool(supply & 512),
    }


def lane_state(protocol, power, receiver):
    return [{'lane': lane,
             'rx_protocol': (protocol >> (6 * lane)) & 7,
             'tx_protocol': (protocol >> (6 * lane + 3)) & 7,
             'power_control': (power >> (4 * lane)) & 15,
             'receiver_small_override': bool(receiver & (1 << (2 + lane))),
             'receiver_large_override': bool(receiver & (1 << (6 + lane)))}
            for lane in range(2)]


def acquire():
    if b'apple,j700' not in (ROOT / 'compatible').read_bytes().split(b'\0'):
        raise ValueError('requires J700')
    if b'apple,t8140-atcphy' not in (PHY / 'compatible').read_bytes().split(b'\0'):
        raise ValueError('requires T8140 ATC')
    names = (PHY / 'reg-names').read_bytes().rstrip(b'\0').decode().split('\0')
    raw = (PHY / 'reg').read_bytes()
    if len(raw) != 16 * len(names):
        raise ValueError('unexpected resource layout')
    regs = dict(zip(names, struct.iter_unpack('>QQ', raw)))
    if regs.get('core') != (0x40b000000, 0x4c000):
        raise ValueError('unexpected core resource')
    if regs.get('lpdptx') != (0x40b050000, 0x4000):
        raise ValueError('unexpected LPDPTX resource')
    groups = []
    for prop, region in GROUPS:
        base, size = regs[region]
        groups.append((base, decode((PHY / prop).read_bytes(), size)))
    expected = compose(groups)
    failures = []
    power = None
    lanes = None
    fd = os.open('/dev/mem', os.O_RDONLY | os.O_CLOEXEC)
    try:
        for address, (mask, value) in sorted(expected.items()):
            mismatch = (read_word(fd, address) ^ value) & mask
            if mismatch:
                region = 'core' if address < regs['lpdptx'][0] else 'lpdptx'
                failures.append({'region': region,
                                 'offset': hex(address - regs[region][0]),
                                 'mismatch_mask': hex(mismatch)})
        power = aux_state(read_word(fd, regs['core'][0] + 0x16000),
                          read_word(fd, regs['core'][0] + 0x16400))
        lanes = lane_state(read_word(fd, regs['core'][0] + 0x60),
                           read_word(fd, regs['core'][0] + 0x200),
                           read_word(fd, regs['core'][0] + 0xc))
    finally:
        os.close(fd)
    print(json.dumps({'checked_words': len(expected), 'mismatches': failures, 'aux_state': power, 'lanes': lanes}))


class CodecTests(unittest.TestCase):
    def test_lane_state(self):
        lanes = lane_state(0x489, 0x10, 0x88)
        self.assertEqual((lanes[0]['rx_protocol'], lanes[0]['tx_protocol']), (1, 1))
        self.assertEqual((lanes[1]['rx_protocol'], lanes[1]['tx_protocol']), (2, 2))
        self.assertEqual(lanes[1]['power_control'], 1)
        self.assertTrue(lanes[1]['receiver_small_override'])
        self.assertTrue(lanes[1]['receiver_large_override'])

    def test_aux_state(self):
        self.assertTrue(all(aux_state(1, 0x30f).values()))
        self.assertFalse(any(aux_state(0, 0).values()))

    def test_read_mapping(self):
        with tempfile.TemporaryFile() as stream:
            stream.truncate(2 * mmap.PAGESIZE)
            stream.seek(mmap.PAGESIZE + 4)
            stream.write(struct.pack('<I', 0x1234abcd))
            stream.flush()
            self.assertEqual(read_word(stream.fileno(), mmap.PAGESIZE + 4), 0x1234abcd)
            with self.assertRaises(ValueError):
                read_word(stream.fileno(), 1)

    def test_overlap(self):
        self.assertEqual(compose([(16, [(0, 15, 5), (0, 3, 2)])]), {16: (15, 6)})

    def test_set_outside_mask(self):
        self.assertEqual(compose([(0, [(0, 1, 8)])]), {0: (9, 8)})

    def test_regions(self):
        self.assertEqual(len(compose([(0, [(0, 1, 1)]), (16, [(0, 1, 0)])])), 2)

    def test_bounds(self):
        for data, size in [(b'x', 4), (struct.pack('>III', 4, 1, 1), 4),
                           (struct.pack('>III', 1, 1, 1), 8)]:
            with self.assertRaises(ValueError):
                decode(data, size)
        self.assertEqual(decode(struct.pack('>III', 4, 1, 1), 8), [(4, 1, 1)])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--selftest', action='store_true')
    group.add_argument('--read-hardware', action='store_true')
    args = parser.parse_args()
    if args.selftest:
        unittest.main(argv=['apple-atc-readback'], exit=True)
    else:
        acquire()
