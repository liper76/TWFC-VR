"""List the game's key bindings (PlayerInput FKeyBind: FName Name, FString Command) for given key-name parts (read-only).
Usage: keybinds.py [XboxTypeS_ ...]"""
import sys, struct, ctypes as C, ctypes.wintypes as W
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc, k32


class MBI(C.Structure):
    _fields_ = [('BaseAddress', C.c_void_p), ('AllocationBase', C.c_void_p), ('AllocationProtect', W.DWORD),
                ('RegionSize', C.c_size_t), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]


parts = sys.argv[1:] or ['XboxTypeS_']
p = Proc(find_pid('TWFC.exe'))
narr = p.u32(0x12B2F0F4); nnum = p.u32(0x12B2F0F8)
ents = struct.unpack('<%dI' % nnum, p.read(narr, 4 * nnum))
want = {}
for i, e in enumerate(ents):
    if not e: continue
    n = p.cstr(e + 8, True, 64)
    if n and any(n.startswith(x) for x in parts):
        want[i << 14] = n
print(len(want), 'key names')
seen = set()
addr = 0x10000
while addr < 0x7FFF0000:
    m = MBI()
    if not k32.VirtualQueryEx(p.h, C.c_void_p(addr), C.byref(m), C.sizeof(m)): break
    size = m.RegionSize
    if m.State == 0x1000 and m.Protect in (0x04, 0x08, 0x40) and size < 0x10000000:
        data = p.read(addr, size)
        if data:
            for o in range(0, len(data) - 20, 4):
                v = struct.unpack_from('<I', data, o)[0]
                if v not in want: continue
                cmd = None
                for fo in (4, 8):
                    ptr, num, mx = struct.unpack_from('<Iii', data, o + fo)
                    if not (1 < num <= mx < 400) or ptr < 0x10000: continue
                    cmd = p.cstr(ptr, True, min(num, 300))
                    if cmd and len(cmd) == num - 1: break
                    cmd = None
                if not cmd: continue
                key = (want[v], cmd)
                if key in seen: continue
                seen.add(key)
                print('%08x  %-26s %s' % (addr + o, want[v], cmd))
    addr += size
