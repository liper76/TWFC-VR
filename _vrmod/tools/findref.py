"""Find UObjects (vtable in the exe, FName at +4) holding a pointer to a given address. Read-only.
Usage: findref.py <hexaddr> [max back bytes]"""
import sys, struct, ctypes as C, ctypes.wintypes as W
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc, k32


class MBI(C.Structure):
    _fields_ = [('BaseAddress', C.c_void_p), ('AllocationBase', C.c_void_p), ('AllocationProtect', W.DWORD),
                ('RegionSize', C.c_size_t), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]


p = Proc(find_pid('TWFC.exe'))
narr = p.u32(0x12B2F0F4); nnum = p.u32(0x12B2F0F8)
ents = struct.unpack('<%dI' % nnum, p.read(narr, 4 * nnum))
def name(v):
    i = v >> 14
    return p.cstr(ents[i] + 8, True, 48) if 0 < i < nnum and ents[i] else '?'
target = struct.pack('<I', int(sys.argv[1], 16))
back = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x400
addr = 0x10000
while addr < 0x7FFF0000:
    m = MBI()
    if not k32.VirtualQueryEx(p.h, C.c_void_p(addr), C.byref(m), C.sizeof(m)): break
    size = m.RegionSize
    if m.State == 0x1000 and m.Protect in (0x04, 0x08) and size < 0x10000000:
        d = p.read(addr, size)
        if d:
            o = d.find(target)
            while o >= 0:
                if o % 4 == 0:
                    # walk back to an object start: vtable in the exe followed by a valid FName
                    for s in range(o - o % 4, max(0, o - back), -4):
                        vt = struct.unpack_from('<I', d, s)[0]
                        if 0x12000000 < vt < 0x13000000 and s + 8 <= len(d):
                            nm = name(struct.unpack_from('<I', d, s + 4)[0])
                            if nm != '?':
                                print('%08x: in %08x %s+%#x (vt %08x, owner %08x)' % (addr + o, addr + s, nm, o - s, vt,
                                      struct.unpack_from('<I', d, s + 0x34)[0] if s + 0x38 <= len(d) else 0))
                                break
                o = d.find(target, o + 1)
    addr += size
