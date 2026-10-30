"""Print GNames entries containing any of the given substrings (case-insensitive, read-only).
Usage: names.py <part> [<part> ...]"""
import sys, struct
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
narr = p.u32(0x12B2F0F4); nnum = p.u32(0x12B2F0F8)
ents = struct.unpack('<%dI' % nnum, p.read(narr, 4 * nnum))
parts = [a.lower() for a in sys.argv[1:]]
for i, e in enumerate(ents):
    if not e: continue
    n = p.cstr(e + 8, True, 80)
    if n and any(x in n.lower() for x in parts):
        print(i, n)
