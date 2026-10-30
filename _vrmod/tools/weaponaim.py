"""Weapon model axes vs the game's aim (PlayerController.Rotation) and vs the pawn mesh. Usage: weaponaim.py <pawn hex>"""
import sys, struct, math
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
pawn = int(sys.argv[1], 16)
mesh = p.u32(pawn + 0x3F4)
ca, cn = p.u32(pawn + 0x50), p.u32(pawn + 0x54)
comps = struct.unpack('<%dI' % cn, p.read(ca, 4 * cn))
vt = p.u32(mesh)
pc = p.u32(pawn + 0x1D4)


def yawpitch(v):
    return math.degrees(math.atan2(v[1], v[0])), math.degrees(math.atan2(v[2], math.hypot(v[0], v[1])))


ml = struct.unpack('<16f', p.read(mesh + 0x50, 64))
r = struct.unpack('<3i', p.read(pc + 0xE4, 12))
aimy = ((r[1] + 32768) % 65536 - 32768) * 360 / 65536
aimp = ((r[0] + 32768) % 65536 - 32768) * 360 / 65536
pr = struct.unpack('<3i', p.read(pawn + 0xE4, 12))
print('aim yaw %.1f pitch %.1f | pawn yaw %.1f | mesh X yaw %.1f' % (aimy, aimp, ((pr[1] + 32768) % 65536 - 32768) * 360 / 65536, yawpitch(ml[0:3])[0]))
for c in comps:
    if c == mesh or p.u32(c) != vt: continue
    f = struct.unpack('<16f', p.read(c + 0x50, 64))
    for k, nm in ((0, 'X'), (1, 'Y'), (2, 'Z')):
        y, pt = yawpitch(f[k * 4:k * 4 + 3])
        print('  weapon %08x %s axis yaw %.1f pitch %.1f' % (c, nm, y, pt))
