"""Read-only memory analysis of a running TWFC.exe: locates GNames / GObjects and UObject layout.
Usage: python -I memscan.py [--dump out.txt]
"""
import ctypes as C, ctypes.wintypes as W, struct, sys

k32 = C.WinDLL('kernel32', use_last_error=True)
PROCESS_VM_READ, PROCESS_QUERY_INFORMATION = 0x10, 0x400
TH32CS_SNAPPROCESS, TH32CS_SNAPMODULE, TH32CS_SNAPMODULE32 = 2, 8, 0x10


class PROCESSENTRY32W(C.Structure):
    _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('th32ProcessID', W.DWORD),
                ('th32DefaultHeapID', C.c_void_p), ('th32ModuleID', W.DWORD), ('cntThreads', W.DWORD),
                ('th32ParentProcessID', W.DWORD), ('pcPriClassBase', C.c_long), ('dwFlags', W.DWORD),
                ('szExeFile', C.c_wchar * 260)]


class MODULEENTRY32W(C.Structure):
    _fields_ = [('dwSize', W.DWORD), ('th32ModuleID', W.DWORD), ('th32ProcessID', W.DWORD),
                ('GlblcntUsage', W.DWORD), ('ProccntUsage', W.DWORD), ('modBaseAddr', C.c_void_p),
                ('modBaseSize', W.DWORD), ('hModule', W.HMODULE), ('szModule', C.c_wchar * 256),
                ('szExePath', C.c_wchar * 260)]


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W(); e.dwSize = C.sizeof(e)
    ok = k32.Process32FirstW(snap, C.byref(e))
    while ok:
        if e.szExeFile.lower() == name.lower():
            k32.CloseHandle(snap); return e.th32ProcessID
        ok = k32.Process32NextW(snap, C.byref(e))
    k32.CloseHandle(snap)


def modules(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    e = MODULEENTRY32W(); e.dwSize = C.sizeof(e); out = {}
    ok = k32.Module32FirstW(snap, C.byref(e))
    while ok:
        out[e.szModule.lower()] = (e.modBaseAddr, e.modBaseSize)
        ok = k32.Module32NextW(snap, C.byref(e))
    k32.CloseHandle(snap); return out


class Proc:
    def __init__(self, pid):
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
        if not self.h: raise OSError(C.get_last_error())

    def read(self, addr, n):
        buf = C.create_string_buffer(n); got = C.c_size_t()
        if not k32.ReadProcessMemory(self.h, C.c_void_p(addr), buf, n, C.byref(got)): return None
        return buf.raw[:got.value]

    def u32(self, a):
        b = self.read(a, 4); return struct.unpack('<I', b)[0] if b else None

    def cstr(self, a, wide, n=64):
        b = self.read(a, n * (2 if wide else 1))
        if not b: return None
        s = b.decode('utf-16-le', 'replace') if wide else b.decode('latin-1')
        return s.split('\0')[0]


def main():
    pid = find_pid('TWFC.exe')
    if not pid: print('TWFC.exe not running'); return
    mods = modules(pid); base, size = mods['twfc.exe']
    print('pid', pid, 'base %x size %x' % (base, size))
    p = Proc(pid)
    data_rva, data_sz = 0x20ec000, 0x1904b0
    data = p.read(base + data_rva, data_sz)
    names = objs = None
    for off in range(0, len(data) - 12, 4):
        ptr, num, mx = struct.unpack_from('<III', data, off)
        if not (5000 < num <= mx < 4_000_000) or ptr < 0x10000: continue
        first = p.u32(ptr)
        if not first: continue
        # GNames: entry 0 is "None"
        for so in range(0, 0x20, 4):
            for wide in (False, True):
                if p.cstr(first + so, wide, 8) == 'None' and names is None:
                    names = (base + data_rva + off, ptr, num, so, wide)
        # GObjects: obj[i] stores its own index i somewhere in the header
        if objs is None:
            o1, o2 = p.u32(ptr + 4), p.u32(ptr + 8)
            if o1 and o2:
                h1, h2 = p.read(o1, 0x60), p.read(o2, 0x60)
                if h1 and h2:
                    for io in range(4, 0x60, 4):
                        if struct.unpack_from('<I', h1, io)[0] == 1 and struct.unpack_from('<I', h2, io)[0] == 2:
                            objs = (base + data_rva + off, ptr, num, io); break
    print('GNames', names and ('rva %x arr %x num %d nameoff %x wide %s' % (names[0] - base, *names[1:])))
    print('GObjects', objs and ('rva %x arr %x num %d indexoff %x' % (objs[0] - base, *objs[1:])))
    if not (names and objs): return
    _, narr, nnum, noff, nwide = names; _, oarr, onum, ioff = objs

    def name(i):
        e = p.u32(narr + 4 * i)
        return p.cstr(e + noff, nwide) if e else None

    # Find FName offset: the field whose value maps to sane names on many objects; Outer/Class are pointers.
    samples = [p.u32(oarr + 4 * i) for i in range(100, 160)]
    hdrs = [p.read(s, 0x60) for s in samples if s]
    for fo in range(4, 0x60, 4):
        vals = [struct.unpack_from('<I', h, fo)[0] for h in hdrs]
        if all(0 < v < nnum for v in vals) and len(set(vals)) > 30:
            print('candidate FName offset %x:' % fo, [name(v) for v in vals[:8]])
    if '--dump' in sys.argv:
        out = sys.argv[sys.argv.index('--dump') + 1]
        with open(out, 'w', encoding='utf-8') as f:
            for i in range(nnum):
                n = name(i)
                if n: f.write('%d %s\n' % (i, n))
        print('names dumped to', out)


if __name__ == '__main__':
    main()
