"""Find all copies of the current camera location (from the mod log's latest 'probe out') in game memory."""
import sys, struct, re
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, modules, Proc
from findobj import regions

LOG = r"C:\Games\Transformers - War for Cybertron\BinariesVR\twfcvr.log"


def main():
    lines = [l for l in open(LOG, encoding='latin-1') if 'probe out' in l]
    m = re.search(r'00:\S+/(-?\d+) 04:\S+/(-?\d+) 08:\S+/(-?\d+)', lines[-1])
    xi, yi, zi = (int(v) & 0xffffffff for v in m.groups())
    x, y, z = (struct.unpack('<f', struct.pack('<I', v))[0] for v in (xi, yi, zi))
    print('camera location', x, y, z)
    pid = find_pid('TWFC.exe'); p = Proc(pid); mods = modules(pid)
    pat = struct.pack('<I', xi) + struct.pack('<I', yi)
    for rb, rs in regions(p):
        d = p.read(rb, rs)
        if not d: continue
        i = d.find(pat)
        while i != -1:
            a = rb + i
            zz = struct.unpack_from('<f', d, i + 8)[0] if i + 12 <= len(d) else None
            after = d[i + 12:i + 40]
            ints = struct.unpack('<7i', after) if len(after) == 28 else ()
            print('%08x z=%s next ints %s' % (a, zz, ints))
            i = d.find(pat, i + 4)


if __name__ == '__main__':
    main()
