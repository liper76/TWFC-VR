"""Find UObject layout by locating objects named with a given FName index (read-only).
Usage: python -I findobj.py <nameIndex> [<nameIndex>...]
"""
import sys, struct, ctypes as C, ctypes.wintypes as W
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, modules, Proc, k32
from collections import Counter


class MBI(C.Structure):
    _fields_ = [('BaseAddress', C.c_void_p), ('AllocationBase', C.c_void_p), ('AllocationProtect', W.DWORD),
                ('RegionSize', C.c_size_t), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]


def regions(p):
    a = 0x10000; m = MBI()
    while a < 0x7fff0000 and k32.VirtualQueryEx(p.h, C.c_void_p(a), C.byref(m), C.sizeof(m)):
        if m.State == 0x1000 and m.Protect in (0x04, 0x40, 0x02, 0x20) and not (m.Protect & 0x100):
            yield m.BaseAddress, m.RegionSize
        a = (m.BaseAddress or 0) + m.RegionSize


def main():
    pid = find_pid('TWFC.exe'); base, size = modules(pid)['twfc.exe']; p = Proc(pid)
    targets = [int(x) for x in sys.argv[1:]]
    pats = {t: struct.pack('<II', t, 0) for t in targets}
    cnt = Counter(); hits = []
    for rb, rs in regions(p):
        d = p.read(rb, rs)
        if not d: continue
        for t, pat in pats.items():
            i = d.find(pat)
            while i != -1:
                if i % 4 == 0:
                    for k in range(0x10, 0x48, 4):
                        if i - k >= 0:
                            vt = struct.unpack_from('<I', d, i - k)[0]
                            if base <= vt < base + size:
                                cnt[k] += 1; hits.append((t, rb + i - k, k))
                i = d.find(pat, i + 1)
    print('name-offset candidates', cnt.most_common(6))
    for h in hits[:40]: print(h[0], hex(h[1]), hex(h[2]))


if __name__ == '__main__':
    main()
