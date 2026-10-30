"""Weapon calibration (run with HandIK=0): the game's aim direction (PlayerController.Rotation) expressed in the
local axes of the weapon bone. Usage: gripcalib.py <pawn hex> [bone]"""
import sys, struct, math
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
pawn = int(sys.argv[1], 16)
bone = int(sys.argv[2]) if len(sys.argv) > 2 else 49
m = p.u32(pawn + 0x3F4)
pc = p.u32(pawn + 0x1D4)
for _ in range(5):
    l2w = struct.unpack('<16f', p.read(m + 0x50, 64))
    sb = p.u32(m + 0x1EC)
    bm = struct.unpack('<16f', p.read(sb + bone * 64, 64))
    pitch, yaw = [((v + 32768) & 0xFFFF) - 32768 for v in struct.unpack('<2i', p.read(pc + 0xE4, 8))]
    a = math.radians(pitch * 360 / 65536); b = math.radians(yaw * 360 / 65536)
    aim = (math.cos(a) * math.cos(b), math.cos(a) * math.sin(b), math.sin(a))
    axes = []
    for k in range(3):
        v = bm[k * 4:k * 4 + 3]
        w = [v[0] * l2w[c] + v[1] * l2w[4 + c] + v[2] * l2w[8 + c] for c in range(3)]
        n = math.sqrt(sum(x * x for x in w)); axes.append([x / n for x in w])
    local = [sum(aim[c] * axes[k][c] for c in range(3)) for k in range(3)]
    print('aim p%d y%d -> in bone axes X %.3f Y %.3f Z %.3f' % (pitch, yaw, *local))
