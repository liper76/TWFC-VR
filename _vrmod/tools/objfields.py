"""Pointer fields of an object that point at named UObjects (read-only). Usage: objfields.py <object hex> [span hex]"""
import sys, struct
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
narr = p.u32(0x12B2F0F4); nnum = p.u32(0x12B2F0F8)


def name(o):
    vt = p.u32(o)
    if not vt or not 0x12000000 < vt < 0x13000000: return None
    v = p.u32(o + 4)
    if v is None or (v >> 14) >= nnum: return None
    e = p.u32(narr + 4 * (v >> 14))
    n = p.cstr(e + 8, True, 64) if e else None
    return n and '%s_%d' % (n, (v & 0x3FFF) - 1)


obj = int(sys.argv[1], 16)
span = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x600
print('object', name(obj))
b = p.read(obj, span)
for o in range(8, len(b) - 3, 4):
    v = struct.unpack_from('<I', b, o)[0]
    if 0x1000000 < v < 0x7FFF0000 and v & 3 == 0:
        n = name(v)
        if n: print('+%03x -> %08x %s' % (o, v, n))
