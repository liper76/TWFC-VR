"""Derive UObject Name/Class offsets: find objects named <nameIdx> whose Class object is named 'Class' (read-only)."""
import sys, struct
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, modules, Proc
from findobj import regions

CLASS_IDX = int(sys.argv[2]) if len(sys.argv) > 2 else 677


def main():
    target = int(sys.argv[1])
    pid = find_pid('TWFC.exe'); p = Proc(pid)
    pat = struct.pack('<I', target); cpat = struct.pack('<I', CLASS_IDX)
    for rb, rs in regions(p):
        d = p.read(rb, rs)
        if not d: continue
        i = d.find(pat)
        while i != -1:
            if i % 4 == 0:
                for N in range(0x0c, 0x48, 4):
                    o = rb + i - N
                    h = p.read(o, 0x80)
                    if not h: continue
                    for K in range(0, 0x80, 4):
                        c = struct.unpack_from('<I', h, K)[0]
                        if c > 0x10000 and p.read(c + N, 4) == cpat:
                            print('obj %x nameoff %x classoff %x class %x' % (o, N, K, c))
            i = d.find(pat, i + 1)


if __name__ == '__main__':
    main()

