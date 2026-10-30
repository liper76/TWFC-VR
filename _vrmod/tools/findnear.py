"""Find components (LocalToWorld at +0x50, vtable in the exe, Owner at +0x34) located near a world point.
Usage: findnear.py <x> <y> <z> [radius]"""
import sys, struct, math, ctypes as C, ctypes.wintypes as W
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc, k32


class MBI(C.Structure):
    _fields_ = [('BaseAddress', C.c_void_p), ('AllocationBase', C.c_void_p), ('AllocationProtect', W.DWORD),
                ('RegionSize', C.c_size_t), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]


p = Proc(find_pid('TWFC.exe'))
pt = [float(x) for x in sys.argv[1:4]]
rad = float(sys.argv[4]) if len(sys.argv) > 4 else 200
one = struct.pack('<f', 1.0)
addr = 0x10000
seen = {}
while addr < 0x7FFF0000:
    m = MBI()
    if not k32.VirtualQueryEx(p.h, C.c_void_p(addr), C.byref(m), C.sizeof(m)): break
    size = m.RegionSize
    if m.State == 0x1000 and m.Protect in (0x04, 0x08) and size < 0x10000000:
        d = p.read(addr, size)
        if d:
            o = d.find(one)
            while o >= 0:
                s = o - 0x3C   # 1.0 = M[3][3]
                if s >= 0x50 and s % 4 == 0:
                    f = struct.unpack_from('<16f', d, s)
                    if f[3] == 0 and f[7] == 0 and f[11] == 0 and all(math.isfinite(x) for x in f[12:15]) and math.dist(f[12:15], pt) < rad:
                        obj = s - 0x50
                        vt, owner = struct.unpack_from('<I', d, obj)[0], struct.unpack_from('<I', d, obj + 0x34)[0]
                        if 0x12000000 < vt < 0x13000000:
                            seen[addr + obj] = (vt, owner, math.dist(f[12:15], pt), math.sqrt(f[0] ** 2 + f[1] ** 2 + f[2] ** 2))
                o = d.find(one, o + 1)
    addr += size
for a, (vt, ow, dist, sc) in sorted(seen.items(), key=lambda x: x[1][2]):
    print('%08x vt %08x owner %08x dist %.0f scale %.2f' % (a, vt, ow, dist, sc))
