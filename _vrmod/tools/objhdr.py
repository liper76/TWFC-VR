"""Dump an object's header and interpret dwords as FName indices / pointers to objects (read-only).
Usage: objhdr.py <hexaddr> [<hexaddr> ...]"""
import sys, struct
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, modules, Proc

pid = find_pid('TWFC.exe'); p = Proc(pid); base, size = modules(pid)['twfc.exe']
narr = p.u32(0x12B2F0F4); nnum = p.u32(0x12B2F0F8)
ents = struct.unpack('<%dI' % nnum, p.read(narr, 4 * nnum))


def name(i):
    if not 0 <= i < nnum or not ents[i]: return None
    return p.cstr(ents[i] + 8, True, 48)


for a in sys.argv[1:]:
    o = int(a, 16)
    d = struct.unpack('<32I', p.read(o, 128))
    print('object %08x' % o)
    for k, v in enumerate(d):
        s = ''
        if 0 < v < nnum and name(v): s += ' name? %r' % name(v)
        if v > 0x10000:
            inner = p.read(v, 0x60)
            if inner:
                hits = [(j * 4, name(x)) for j, x in enumerate(struct.unpack('<24I', inner)) if 0 < x < nnum and name(x)]
                if hits: s += ' -> obj? names@ %s' % hits[:4]
            if base <= v < base + size: s += ' [module]'
        print('  +%02x %08x%s' % (k * 4, v, s))
