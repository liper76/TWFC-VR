"""Find where an FName (GNames index << 14) is stored in the game's memory (read-only).
Usage: findname.py <Name> [<Name> ...]"""
import sys, struct, ctypes as C, ctypes.wintypes as W
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc, k32


class MBI(C.Structure):
    _fields_ = [('BaseAddress', C.c_void_p), ('AllocationBase', C.c_void_p), ('AllocationProtect', W.DWORD),
                ('RegionSize', C.c_size_t), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]


p = Proc(find_pid('TWFC.exe'))
narr = p.u32(0x12B2F0F4); nnum = p.u32(0x12B2F0F8)
ents = struct.unpack('<%dI' % nnum, p.read(narr, 4 * nnum))
want = {}
for i, e in enumerate(ents):
    if e and p.cstr(e + 8, True, 64) in sys.argv[1:]:
        want[struct.pack('<I', i << 14)] = p.cstr(e + 8, True, 64)
print('names:', {v: hex(struct.unpack('<I', k)[0]) for k, v in want.items()})
addr = 0x10000
while addr < 0x7FFF0000:
    m = MBI()
    if not k32.VirtualQueryEx(p.h, C.c_void_p(addr), C.byref(m), C.sizeof(m)): break
    size = m.RegionSize
    if m.State == 0x1000 and m.Protect in (0x04, 0x02, 0x08, 0x40, 0x20) and size < 0x10000000:
        data = p.read(addr, size)
        if data:
            for k, n in want.items():
                o = data.find(k)
                while o >= 0:
                    if o % 4 == 0:
                        print('%-28s at %08x' % (n, addr + o))
                    o = data.find(k, o + 1)
    addr += size
