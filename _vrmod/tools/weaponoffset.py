"""Where the held weapon sits relative to the right hand bone (the IK target), in the weapon's own frame.
Usage: weaponoffset.py <pawn hex> <weapon comp hex> [hand bone]"""
import sys, struct, math
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
pawn, wc = int(sys.argv[1], 16), int(sys.argv[2], 16)
bone = int(sys.argv[3]) if len(sys.argv) > 3 else 42
m = p.u32(pawn + 0x3F4)
l2w = struct.unpack('<16f', p.read(m + 0x50, 64))
bm = struct.unpack('<16f', p.read(p.u32(m + 0x1EC) + bone * 64, 64))
hand = [bm[12] * l2w[c] + bm[13] * l2w[4 + c] + bm[14] * l2w[8 + c] + l2w[12 + c] for c in range(3)]
w = struct.unpack('<16f', p.read(wc + 0x50, 64))
ax = []
for k in range(3):
    v = w[k * 4:k * 4 + 3]; l = math.sqrt(sum(x * x for x in v)); ax.append([x / l for x in v])
bo = struct.unpack('<3f', p.read(wc + 0xBC, 12))
for label, pt in (('weapon origin', w[12:15]), ('bounds centre', bo)):
    d = [pt[c] - hand[c] for c in range(3)]
    print('%-14s - hand: fwd %.1f right %.1f up %.1f (weapon X/Y/Z axes, Unreal units)' % (
        label, *[sum(d[c] * ax[k][c] for c in range(3)) for k in range(3)]))
