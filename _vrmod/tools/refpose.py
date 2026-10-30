"""Compose the reference (bind) pose of the player's skeleton from RefSkeleton (quat at +8, pos at +0x18, parent
at +0x34) and compare with the animated component-space pose. Usage: refpose.py <pawn hex> [bones...]"""
import sys, struct, math
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
pawn = int(sys.argv[1], 16)
show = [int(x) for x in sys.argv[2:]] or [1, 2, 3, 4, 20, 21, 39, 40, 41]
mesh = p.u32(pawn + 0x3F4); sk = p.u32(mesh + 0x1BC)
arr = p.u32(sk + 0x68); n = p.u32(sk + 0x6C)
sb = p.u32(mesh + 0x1EC)


def qmat(x, y, z, w, conj=False):
    if conj: x, y, z = -x, -y, -z
    # UE3 FQuatRotationTranslationMatrix rows
    return [[1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)],
            [2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)],
            [2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)]]


def compose(a, b):   # row-vector: a then b ; each = (rows, pos)
    ra, pa = a; rb, pb = b
    r = [[sum(ra[i][k] * rb[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    pos = [sum(pa[k] * rb[k][j] for k in range(3)) + pb[j] for j in range(3)]
    return r, pos


for conj in (False, True):
    comp = []
    for i in range(n):
        d = p.read(arr + i * 64, 64)
        q = struct.unpack_from('<4f', d, 8); t = struct.unpack_from('<3f', d, 0x18); par = struct.unpack_from('<i', d, 0x34)[0]
        local = (qmat(*q, conj=conj and i > 0) if True else None, list(t))
        comp.append(local if i == 0 else compose(local, comp[par]))
    print('conj non-root' if conj else 'as stored')
    for b in show:
        a = struct.unpack('<16f', p.read(sb + b * 64, 64))
        print('  %3d ref %7.1f %7.1f %7.1f | anim %7.1f %7.1f %7.1f' % (b, *comp[b][1], *a[12:15]))
