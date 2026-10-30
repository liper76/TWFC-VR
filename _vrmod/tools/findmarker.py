"""Find memory holding a float bit pattern (hex) - used with split test 7 to locate the renderer's view copy."""
import sys, struct
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, modules, Proc
from findobj import regions

bits = int(sys.argv[1], 16)
pid = find_pid('TWFC.exe'); p = Proc(pid); base, size = modules(pid)['twfc.exe']
pat = struct.pack('<I', bits)
for rb, rs in regions(p):
    d = p.read(rb, rs)
    if not d: continue
    i = d.find(pat)
    while i != -1:
        if i % 4 == 0:
            ctx = d[i:i + 32]
            f = struct.unpack('<8f', ctx.ljust(32, b'\0'))
            print('%08x %s floats %s' % (rb + i, 'MODULE' if base <= rb + i < base + size else '', ' '.join('%.1f' % x for x in f)))
        i = d.find(pat, i + 1)
