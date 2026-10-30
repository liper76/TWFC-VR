"""Offsets inside an object that hold a given pointer (read-only). Usage: findptr.py <object hex> <value hex> [span hex]"""
import sys, struct
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
obj, val = int(sys.argv[1], 16), int(sys.argv[2], 16)
span = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x1000
b = p.read(obj, span)
for o in range(0, len(b) - 3, 4):
    if struct.unpack_from('<I', b, o)[0] == val: print('+%03x' % o)
