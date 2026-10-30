"""Find scene-view structures: places holding the camera's POV location, and 4x4 matrices next to them whose
row 2/3 look like a projection (col 3 = 0,0,1,0). Usage: findview.py <pawn hex>"""
import sys, struct, ctypes as C, ctypes.wintypes as W
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc, k32


class MBI(C.Structure):
    _fields_ = [('BaseAddress', C.c_void_p), ('AllocationBase', C.c_void_p), ('AllocationProtect', W.DWORD),
                ('RegionSize', C.c_size_t), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]


p = Proc(find_pid('TWFC.exe'))
pawn = int(sys.argv[1], 16)
pc = p.u32(pawn + 0x1D4); cam = p.u32(pc + 0x2D4)
pov = p.read(cam + 0x324, 12)
print('pov', struct.unpack('<3f', pov))
addr = 0x10000; hits = []
while addr < 0x7FFF0000:
    m = MBI()
    if not k32.VirtualQueryEx(p.h, C.c_void_p(addr), C.byref(m), C.sizeof(m)): break
    size = m.RegionSize
    if m.State == 0x1000 and m.Protect in (0x04, 0x08, 0x40) and size < 0x10000000:
        d = p.read(addr, size)
        if d:
            o = d.find(pov)
            while o >= 0:
                hits.append(addr + o)
                lo, hi = max(0, o - 0x300), min(len(d) - 64, o + 0x300)
                for s in range(lo - lo % 4, hi, 4):
                    f = struct.unpack_from('<16f', d, s)
                    if f[3] == 0 and f[7] == 0 and f[11] == 1.0 and f[15] == 0 and f[1] == 0 and f[4] == 0 and 0.2 < abs(f[0]) < 5:
                        print('  near pov@%08x: matrix @%08x (pov%+#x) m00 %.3f m11 %.3f m22 %.5f m32 %.4f m20 %.3f m21 %.3f' % (
                            addr + o, addr + s, s - o, f[0], f[5], f[10], f[14], f[8], f[9]))
                o = d.find(pov, o + 1)
    addr += size
print('pov copies:', ' '.join('%08x' % h for h in hits[:20]))
