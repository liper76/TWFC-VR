"""Find components whose LocalToWorld sits at a bone of the player's mesh (e.g. the weapon at the hand) and compare
axes with that bone. Searches pointers from the pawn two levels deep. Usage: findweapon.py <pawn hex> [bone]"""
import sys, struct, math
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
pawn = int(sys.argv[1], 16); bone = int(sys.argv[2]) if len(sys.argv) > 2 else 49
m = p.u32(pawn + 0x3F4)
l2w = struct.unpack('<16f', p.read(m + 0x50, 64))
bm = struct.unpack('<16f', p.read(p.u32(m + 0x1EC) + bone * 64, 64))


def xf(v, w):
    return [v[0] * l2w[c] + v[1] * l2w[4 + c] + v[2] * l2w[8 + c] + (l2w[12 + c] if w else 0) for c in range(3)]


def nrm(v):
    l = math.sqrt(sum(x * x for x in v)) or 1
    return [x / l for x in v]


bpos = xf(bm[12:15], True)
bax = [nrm(xf(bm[k * 4:k * 4 + 3], False)) for k in range(3)]
print('bone %d world %.0f %.0f %.0f X %s' % (bone, *bpos, ['%.2f' % x for x in bax[0]]))
seen = set(); found = set()


def scan(obj, depth, path):
    body = p.read(obj, 0x1000)
    if not body: return
    for off in range(0, len(body) - 4, 4):
        v = struct.unpack_from('<I', body, off)[0]
        if v < 0x1000000 or v in seen: continue
        seen.add(v)
        b = p.read(v, 0x200)
        if not b or len(b) < 0x200: continue
        for mo in range(0, 0x1C0, 4):
            f = struct.unpack_from('<16f', b, mo)
            if not all(math.isfinite(x) for x in f) or abs(f[15] - 1) > 1e-3 or abs(f[3]) > 1e-3 or abs(f[7]) > 1e-3: continue
            if math.dist(f[12:15], bpos) < 60 and (v, mo) not in found:
                found.add((v, mo))
                ax = [nrm(f[k * 4:k * 4 + 3]) for k in range(3)]
                rel = [[sum(ax[k][c] * bax[j][c] for c in range(3)) for j in range(3)] for k in range(3)]
                print('%s+%03x -> %08x vt %08x L2W@+%03x scale %.2f dist %.0f; axes in bone frame X %s Y %s Z %s' % (
                    path, off, v, struct.unpack_from('<I', b, 0)[0], mo, math.sqrt(sum(x * x for x in f[0:3])),
                    math.dist(f[12:15], bpos), ['%.2f' % x for x in rel[0]], ['%.2f' % x for x in rel[1]], ['%.2f' % x for x in rel[2]]))
        if depth > 1: scan(v, depth - 1, path + '+%03x' % off)


scan(pawn, 2, 'pawn')
