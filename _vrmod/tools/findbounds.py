"""Find a component's world Bounds (FBoxSphereBounds: Origin, BoxExtent, SphereRadius) near its LocalToWorld origin.
Usage: findbounds.py <comp hex>"""
import sys, struct, math
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
c = int(sys.argv[1], 16)
b = p.read(c, 0x400)
l2w = struct.unpack_from('<16f', b, 0x50)
pos = l2w[12:15]
print('pos %.1f %.1f %.1f  X %.2f %.2f %.2f' % (*pos, *l2w[0:3]))
for o in range(0, 0x3E0, 4):
    v = struct.unpack_from('<7f', b, o)
    if not all(math.isfinite(x) for x in v): continue
    if math.dist(v[0:3], pos) < 150 and all(0 < e < 300 for e in v[3:6]) and 0 < v[6] < 400 and o != 0x50 + 48:
        print('+%03x origin %.1f %.1f %.1f extent %.1f %.1f %.1f radius %.1f  (origin - pos %.1f %.1f %.1f)' % (
            o, *v, v[0] - pos[0], v[1] - pos[1], v[2] - pos[2]))
