"""Find bone arrays in a (skeletal mesh) component: TArrays of matrices (SpaceBases) or quat+pos atoms (read-only).
Usage: findbones.py <component hex> [scan bytes]"""
import sys, struct, math
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
comp = int(sys.argv[1], 16)
span = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x800
body = p.read(comp, span)


def is_mat(m):
    if not all(math.isfinite(x) for x in m): return False
    if abs(m[3]) > 1e-3 or abs(m[7]) > 1e-3 or abs(m[11]) > 1e-3 or abs(m[15] - 1) > 1e-3: return False
    return all(0.05 < math.sqrt(m[i] ** 2 + m[i + 1] ** 2 + m[i + 2] ** 2) < 20 for i in (0, 4, 8))


def is_quat(q):
    return all(math.isfinite(x) for x in q) and abs(math.sqrt(sum(x * x for x in q)) - 1) < 1e-2


for off in range(0, len(body) - 12, 4):
    ptr, num, mx = struct.unpack_from('<III', body, off)
    if not (4 <= num <= mx <= 1000) or ptr < 0x1000000: continue
    data = p.read(ptr, min(num, 8) * 64)
    if not data: continue
    kinds = []
    for stride in (64, 48, 32, 28):
        n = min(num, len(data) // stride)
        if stride == 64 and n and all(is_mat(struct.unpack_from('<16f', data, i * 64)) for i in range(n)):
            kinds.append('matrix')
        if stride in (48, 32, 28) and n and all(is_quat(struct.unpack_from('<4f', data, i * stride)) for i in range(n)):
            kinds.append('quat/%d' % stride)
    print('  +%03x TArray ptr %08x num %d max %d %s' % (off, ptr, num, mx, kinds))
