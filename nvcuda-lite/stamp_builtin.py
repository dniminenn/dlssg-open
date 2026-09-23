#!/usr/bin/env python3
"""Stamp the 'Wine builtin DLL' signature into a PE's DOS stub so Wine treats it as a builtin."""
import sys, struct
p = sys.argv[1]; d = bytearray(open(p, 'rb').read())
assert d[:2] == b'MZ'
e_lfanew = struct.unpack_from('<I', d, 0x3c)[0]
sig = b'Wine builtin DLL\0'
assert e_lfanew >= 0x40 + len(sig), 'no room in DOS stub'
d[0x40:0x40 + len(sig)] = sig
open(p, 'wb').write(d); print('stamped', p)
