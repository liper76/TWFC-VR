"""Look for the game's aim hit point: float triples in the player controller / pawn / their referenced objects
that lie on the aim ray (PlayerController location + rotation) beyond 100 units. Usage: findhit.py <pawn hex>"""
import sys, struct, math
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
narr = p.u32(0x12B2F0F4); nnum = p.u32(0x12B2F0F8)
ents = struct.unpack('<%dI' % nnum, p.read(narr, 4 * nnum))
def name(v):
    i = v >> 14
    return p.cstr(ents[i] + 8, True, 48) if 0 < i < nnum and ents[i] else '?'
pawn = int(sys.argv[1], 16)
pc = p.u32(pawn + 0x1D4)
o = struct.unpack('<3f', p.read(pc + 0xD8, 12))
r = struct.unpack('<3i', p.read(pc + 0xE4, 12))
pi, ya = r[0] * 2 * math.pi / 65536, r[1] * 2 * math.pi / 65536
d = (math.cos(pi) * math.cos(ya), math.cos(pi) * math.sin(ya), math.sin(pi))
print('ray from %.0f %.0f %.0f dir %.2f %.2f %.2f' % (*o, *d))
objs = {pc: 'PC', pawn: 'pawn'}
for base in (pc, pawn):
    b = p.read(base, 0x1400) or b''
    for off in range(0, len(b), 4):
        v = struct.unpack_from('<I', b, off)[0]
        if v > 0x1000000 and v not in objs and p.read(v, 8):
            objs[v] = '%s+%x(%s)' % ('PC' if base == pc else 'pawn', off, name(p.u32(v + 4) or 0))
for a, label in objs.items():
    b = p.read(a, 0x1400)
    if not b: continue
    for off in range(0, len(b) - 12, 4):
        x, y, z = struct.unpack_from('<3f', b, off)
        if not all(math.isfinite(t) and abs(t) < 1e6 for t in (x, y, z)): continue
        v = (x - o[0], y - o[1], z - o[2]); L = math.sqrt(sum(t * t for t in v))
        if L < 100: continue
        c = sum(v[i] * d[i] for i in range(3)) / L
        if c > 0.9995: print('  %s +%#x: %.0f %.0f %.0f  dist %.0f cos %.5f' % (label, off, x, y, z, L, c))
