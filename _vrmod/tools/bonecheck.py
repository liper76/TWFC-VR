"""Print world-space position/axes of some bones of the player's mesh (read-only).
Usage: bonecheck.py <pawn hex> [bone indices...]"""
import sys, struct
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
pawn = int(sys.argv[1], 16)
bones = [int(x) for x in sys.argv[2:]] or [40, 41, 42, 49, 21, 22, 23, 5]
m = p.u32(pawn + 0x3F4)
l2w = struct.unpack('<16f', p.read(m + 0x50, 64))
sb = p.u32(m + 0x1EC)
loc = struct.unpack('<3f', p.read(pawn + 0xD8, 12))


def xf(v, w):   # row vector * matrix
    return [v[0] * l2w[0 + c] + v[1] * l2w[4 + c] + v[2] * l2w[8 + c] + (l2w[12 + c] if w else 0) for c in range(3)]


print('pawn loc %.1f %.1f %.1f, mesh yaw axis X %.2f %.2f' % (*loc, l2w[0], l2w[1]))
for b in bones:
    mm = struct.unpack('<16f', p.read(sb + b * 64, 64))
    pos = xf(mm[12:15], True)
    rel = [pos[i] - loc[i] for i in range(3)]
    ax = [xf(mm[k * 4:k * 4 + 3], False) for k in range(3)]
    print('bone %3d rel %6.1f %6.1f %6.1f  X %5.2f %5.2f %5.2f  Z %5.2f %5.2f %5.2f' % (b, *rel, *ax[0], *ax[2]))
