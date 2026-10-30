"""Locate camera actor(s): objects with vtable 0x1293BEB0; print their POV at +0x324 (Loc, Rot, FOV)."""
import sys, struct
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc
from findobj import regions

VT = 0x1293BEB0


def main():
    p = Proc(find_pid('TWFC.exe'))
    pat = struct.pack('<I', VT)
    for rb, rs in regions(p):
        d = p.read(rb, rs)
        if not d: continue
        i = d.find(pat)
        while i != -1:
            if i % 4 == 0:
                o = rb + i
                pov = p.read(o + 0x324, 0x1c)
                if pov:
                    x, y, z, pi, ya, ro, fov = struct.unpack('<3f3if', pov)
                    print('cam %08x pov@%08x loc(%.1f %.1f %.1f) rot(%d %d %d) fov %.1f' % (o, o + 0x324, x, y, z, pi, ya, ro, fov))
            i = d.find(pat, i + 4)


if __name__ == '__main__':
    main()
