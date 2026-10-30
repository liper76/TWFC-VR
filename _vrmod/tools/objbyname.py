"""Find live UObjects by name (FName at +4 = index<<14 | number, vtable at +0 in the exe). Optionally only components
near a world point (LocalToWorld at +0x50). Usage: objbyname.py <Name> [x y z radius]"""
import sys, struct, math, ctypes as C, ctypes.wintypes as W
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc, k32


class MBI(C.Structure):
    _fields_ = [('BaseAddress', C.c_void_p), ('AllocationBase', C.c_void_p), ('AllocationProtect', W.DWORD),
                ('RegionSize', C.c_size_t), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]


p = Proc(find_pid('TWFC.exe'))
narr = p.u32(0x12B2F0F4); nnum = p.u32(0x12B2F0F8)
ents = struct.unpack('<%dI' % nnum, p.read(narr, 4 * nnum))
idx = next(i for i, e in enumerate(ents) if e and p.cstr(e + 8, True, 64) == sys.argv[1])
pt = [float(x) for x in sys.argv[2:5]] if len(sys.argv) > 4 and sys.argv[2] != 'owner' else None
rad = float(sys.argv[5]) if len(sys.argv) > 5 else 300
addr = 0x10000; out = []
while addr < 0x7FFF0000:
    m = MBI()
    if not k32.VirtualQueryEx(p.h, C.c_void_p(addr), C.byref(m), C.sizeof(m)): break
    size = m.RegionSize
    if m.State == 0x1000 and m.Protect in (0x04, 0x08) and size < 0x10000000:
        d = p.read(addr, size)
        if d:
            for o in range(4, len(d) - 4, 4):
                v = struct.unpack_from('<I', d, o)[0]
                if v >> 14 != idx: continue
                vt = struct.unpack_from('<I', d, o - 4)[0]
                if not 0x12000000 < vt < 0x13000000: continue
                obj = addr + o - 4
                info = ''
                if pt:
                    f = p.read(obj + 0x50, 64)
                    if not f: continue
                    f = struct.unpack('<16f', f)
                    if not all(math.isfinite(x) for x in f[12:15]) or math.dist(f[12:15], pt) > rad: continue
                    info = 'dist %.0f scale %.2f' % (math.dist(f[12:15], pt), math.sqrt(f[0] ** 2 + f[1] ** 2 + f[2] ** 2))
                if len(sys.argv) > 2 and sys.argv[2] == 'owner':
                    ow = p.u32(obj + 0x34) or 0
                    if ow != int(sys.argv[3], 16) and (ow < 0x10000 or p.u32(ow + 0x2C) != int(sys.argv[3], 16)): continue
                    f = struct.unpack('<16f', p.read(obj + 0x50, 64)); info = 'pos %.0f %.0f %.0f scale@178 %.2f' % (f[12], f[13], f[14], struct.unpack('<f', p.read(obj + 0x178, 4))[0])
                out.append('%08x vt %08x #%d owner %08x %s' % (obj, vt, v & 0x3FFF, p.u32(obj + 0x34) or 0, info))
    addr += size
print(len(out)); print('\n'.join(out[:40]))
