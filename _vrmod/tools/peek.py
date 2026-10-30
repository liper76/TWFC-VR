"""Debug: print int32 values at addresses, twice with a delay (read-only). Usage: peek.py <seconds> <hexaddr> [...]"""
import sys, struct, time, ctypes as C
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, k32

h = k32.OpenProcess(0x0010 | 0x0400, False, find_pid('TWFC.exe'))
addrs = [int(a, 16) for a in sys.argv[2:]]

def read():
    out = []
    for a in addrs:
        b = C.create_string_buffer(4)
        k32.ReadProcessMemory(h, C.c_void_p(a), b, 4, None)
        out.append(struct.unpack('<i', b.raw)[0])
    return out

a = read()
time.sleep(float(sys.argv[1]))
b = read()
for x, v0, v1 in zip(addrs, a, b):
    print('%08X: %d -> %d (+%d)' % (x, v0, v1, v1 - v0))
