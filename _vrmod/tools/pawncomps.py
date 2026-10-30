"""List an actor's Components (TArray at +0x50) with names, vtables, positions and mesh names (read-only).
Usage: pawncomps.py <actor hex>"""
import sys, struct
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
narr = p.u32(0x12B2F0F4)


def name(obj):
    v = p.u32(obj + 4)
    if v is None: return '?'
    e = p.u32(narr + 4 * (v >> 14))
    n = p.cstr(e + 8, True, 64) if e else '?'
    return '%s_%d' % (n, (v & 0x3FFF) - 1) if v & 0x3FFF else n


act = int(sys.argv[1], 16)
arr, num = p.u32(act + 0x50), p.u32(act + 0x54)
print('actor %08x %s: %d components' % (act, name(act), num))
for i in range(num):
    c = p.u32(arr + 4 * i)
    if not c: continue
    f = struct.unpack('<16f', p.read(c + 0x50, 64))
    mesh = p.u32(c + 0x1BC) or 0
    extra = ''
    if 0x1000000 < mesh < 0x7FFF0000 and p.u32(mesh) and 0x12000000 < p.u32(mesh) < 0x13000000:
        extra = ' mesh %08x %s' % (mesh, name(mesh))
    print('%2d %08x vt %08x %-40s pos %7.0f %7.0f %7.0f%s' % (i, c, p.u32(c), name(c), f[12], f[13], f[14], extra))
