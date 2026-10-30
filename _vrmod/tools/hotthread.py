"""Sample EIP of the busiest thread(s) of TWFC.exe and map to module (diagnostics)."""
import sys, ctypes as C, ctypes.wintypes as W, time, struct
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, modules, Proc, k32

TH32CS_SNAPTHREAD = 4


class THREADENTRY32(C.Structure):
    _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('th32ThreadID', W.DWORD), ('th32OwnerProcessID', W.DWORD),
                ('tpBasePri', C.c_long), ('tpDeltaPri', C.c_long), ('dwFlags', W.DWORD)]


class WOW64_CONTEXT(C.Structure):
    _fields_ = [('ContextFlags', W.DWORD), ('Dr', W.DWORD * 6), ('FloatSave', C.c_byte * 112),
                ('SegGs', W.DWORD), ('SegFs', W.DWORD), ('SegEs', W.DWORD), ('SegDs', W.DWORD),
                ('Edi', W.DWORD), ('Esi', W.DWORD), ('Ebx', W.DWORD), ('Edx', W.DWORD), ('Ecx', W.DWORD), ('Eax', W.DWORD),
                ('Ebp', W.DWORD), ('Eip', W.DWORD), ('SegCs', W.DWORD), ('EFlags', W.DWORD), ('Esp', W.DWORD), ('SegSs', W.DWORD),
                ('Ext', C.c_byte * 512)]


def threads(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); e = THREADENTRY32(); e.dwSize = C.sizeof(e); out = []
    ok = k32.Thread32First(snap, C.byref(e))
    while ok:
        if e.th32OwnerProcessID == pid: out.append(e.th32ThreadID)
        ok = k32.Thread32Next(snap, C.byref(e))
    k32.CloseHandle(snap); return out


def cputime(h):
    a, b, c, d = W.FILETIME(), W.FILETIME(), W.FILETIME(), W.FILETIME()
    k32.GetThreadTimes(h, C.byref(a), C.byref(b), C.byref(c), C.byref(d))
    return (c.dwHighDateTime << 32 | c.dwLowDateTime) + (d.dwHighDateTime << 32 | d.dwLowDateTime)


def main():
    pid = find_pid('TWFC.exe'); mods = modules(pid); p = Proc(pid)
    hs = {t: k32.OpenThread(0x0002 | 0x0008 | 0x0040, False, t) for t in threads(pid)}  # suspend, getctx, queryinfo
    t0 = {t: cputime(h) for t, h in hs.items()}; time.sleep(2); t1 = {t: cputime(h) for t, h in hs.items()}
    hot = sorted(hs, key=lambda t: t1[t] - t0[t], reverse=True)[:3]

    def modof(a):
        for n, (b, s) in mods.items():
            if b <= a < b + s: return '%s+%x' % (n, a - b)
        return hex(a)
    for t in hot:
        h = hs[t]; samples = []
        for _ in range(8):
            k32.Wow64SuspendThread(h); ctx = WOW64_CONTEXT(); ctx.ContextFlags = 0x10001
            k32.Wow64GetThreadContext(h, C.byref(ctx)); k32.ResumeThread(h)
            stack = p.read(ctx.Esp, 256) or b''
            rets = [modof(v) for v in struct.unpack('<%dI' % (len(stack) // 4), stack) if any(b <= v < b + s for b, s in mods.values())][:8]
            samples.append((modof(ctx.Eip), rets)); time.sleep(0.05)
        print('thread', t, 'cpu ms', (t1[t] - t0[t]) / 1e4)
        for s in samples[:4]: print('   eip', s[0], 'stack', s[1])


if __name__ == '__main__':
    main()
