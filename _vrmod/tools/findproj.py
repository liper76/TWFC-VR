"""Find UE3 perspective projection matrices in memory (reverse Z: row2 = 0,0,0|z,1  row3 = 0,0,near,0) and
print the near-plane values; then where that float lives in the exe's data. Read-only."""
import sys, struct, ctypes as C, ctypes.wintypes as W, math
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc, k32, modules


class MBI(C.Structure):
    _fields_ = [('BaseAddress', C.c_void_p), ('AllocationBase', C.c_void_p), ('AllocationProtect', W.DWORD),
                ('RegionSize', C.c_size_t), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]


pid = find_pid('TWFC.exe'); p = Proc(pid)
one = struct.pack('<f', 1.0)
nears = {}
addr = 0x10000
while addr < 0x7FFF0000:
    m = MBI()
    if not k32.VirtualQueryEx(p.h, C.c_void_p(addr), C.byref(m), C.sizeof(m)): break
    size = m.RegionSize
    if m.State == 0x1000 and m.Protect in (0x04, 0x08, 0x40) and size < 0x10000000:
        d = p.read(addr, size)
        if d:
            o = d.find(one)
            while o >= 0:
                s = o - 0x2C   # matrix start if this 1.0 is M[2][3]
                if s >= 0 and s % 4 == 0 and s + 64 <= len(d):
                    f = struct.unpack_from('<16f', d, s)
                    if (f[1] == f[2] == f[3] == f[4] == f[6] == f[7] == f[8] == f[9] == f[12] == f[13] == f[15] == 0
                            and 0.2 < f[0] < 5 and 0.2 < f[5] < 5 and f[11] == 1.0 and f[14] != 0):
                        nears.setdefault((round(f[10], 5), round(f[14], 4)), []).append(addr + s)
                o = d.find(one, o + 1)
    addr += size
for n, a in nears.items():
    print('m22,m32 %s  x%d  e.g. %s' % (n, len(a), ' '.join('%08x' % x for x in a[:4])))
base, msize = modules(pid)['twfc.exe']
data = p.read(base, msize)
for n in nears:
    n = n[1]; pat = struct.pack('<f', n)
    o = data.find(pat); hits = []
    while o >= 0 and len(hits) < 10:
        if o % 4 == 0: hits.append('%08x' % (base + o))
        o = data.find(pat, o + 1)
    print('float %.4f in exe image at: %s' % (n, ' '.join(hits)))
