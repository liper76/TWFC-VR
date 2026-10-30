"""Find an actor's components: pointers from the actor to objects holding a LocalToWorld matrix near the actor (read-only).
Usage: findcomp.py <actor hex> [scan bytes]"""
import sys, struct, math
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
act = int(sys.argv[1], 16)
span = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x1000
loc = struct.unpack('<3f', p.read(act + 0xD8, 12))
print('actor %08x loc %.1f %.1f %.1f vt %08x' % (act, *loc, p.u32(act)))


def matrices(buf):
    for o in range(0, len(buf) - 64, 4):
        m = struct.unpack_from('<16f', buf, o)
        if not all(math.isfinite(x) for x in m): continue
        if abs(m[3]) > 1e-3 or abs(m[7]) > 1e-3 or abs(m[11]) > 1e-3 or abs(m[15] - 1) > 1e-3: continue
        rows = [math.sqrt(m[i] ** 2 + m[i + 1] ** 2 + m[i + 2] ** 2) for i in (0, 4, 8)]
        if not all(0.05 < r < 20 for r in rows): continue
        yield o, m


body = p.read(act, span)
seen = set()
for off in range(0, len(body), 4):
    v = struct.unpack_from('<I', body, off)[0]
    if v < 0x1000000 or v in seen: continue
    seen.add(v)
    buf = p.read(v, 0x800)
    if not buf or len(buf) < 0x100: continue
    for mo, m in matrices(buf):
        d = math.dist(m[12:15], loc)
        if d < 1000:
            print('  actor+%03x -> %08x vt %08x  matrix@+%03x scale %.2f pos %.1f %.1f %.1f (dist %.0f)' % (
                off, v, struct.unpack_from('<I', buf, 0)[0], mo, math.sqrt(m[0] ** 2 + m[1] ** 2 + m[2] ** 2),
                *m[12:15], d))
