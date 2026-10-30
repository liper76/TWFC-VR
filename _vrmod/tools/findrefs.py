"""All addresses in committed read/write memory that hold a dword (read-only).
Usage: findrefs.py <value hex> [out.txt]   (prints count; with out.txt writes the addresses)"""
import sys, struct, ctypes, ctypes.wintypes as wt
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p), ('AllocationProtect', wt.DWORD),
                ('PartitionId', wt.WORD), ('RegionSize', ctypes.c_size_t), ('State', wt.DWORD), ('Protect', wt.DWORD), ('Type', wt.DWORD)]

p = Proc(find_pid('TWFC.exe'))
k32 = ctypes.windll.kernel32
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
val = struct.pack('<I', int(sys.argv[1], 16))
addr, hits = 0, []
while addr < 0xFFFF0000:
    m = MBI()
    if not k32.VirtualQueryEx(p.h, addr, ctypes.byref(m), ctypes.sizeof(m)): break
    base, size = m.BaseAddress or 0, m.RegionSize
    if m.State == 0x1000 and m.Protect in (0x04, 0x40) and size < 0x10000000:
        try: b = p.read(base, size) or b''
        except Exception: b = b''
        i = b.find(val)
        while i >= 0:
            if i % 4 == 0: hits.append(base + i)
            i = b.find(val, i + 1)
    addr = base + size
print(len(hits), 'refs')
if len(sys.argv) > 2: open(sys.argv[2], 'w').write('\n'.join('%08x' % h for h in hits))
