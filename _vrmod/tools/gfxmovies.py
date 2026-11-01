"""Scaleform movies of a running TWFC.exe (read-only).
Lists UObjects whose name looks like a movie/HUD (Movie|GFx|Hud), their FGFxMovie (+0x3C) and GFxMovieView (+0x34 of
that), and with --swf <dir> dumps every GFX/CFX (Scaleform SWF) file found in memory there.
Usage: gfxmovies.py [--swf <dir>]"""
import sys, re, struct, zlib, ctypes as C, ctypes.wintypes as W
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc, k32


class MBI(C.Structure):
    _fields_ = [('BaseAddress', C.c_void_p), ('AllocationBase', C.c_void_p), ('AllocationProtect', W.DWORD),
                ('RegionSize', C.c_size_t), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]


p = Proc(find_pid('TWFC.exe'))
narr = p.u32(0x12B2F0F4); nnum = p.u32(0x12B2F0F8)
ents = struct.unpack('<%dI' % nnum, p.read(narr, 4 * nnum))
names = {}
for i, e in enumerate(ents):
    if e:
        s = p.cstr(e + 8, True, 64)
        if s and re.search(r'Movie|GFx|Hud|HUD', s): names[i] = s
swfdir = sys.argv[sys.argv.index('--swf') + 1] if '--swf' in sys.argv else None


def regions():
    addr = 0x10000
    while addr < 0x7FFF0000:
        m = MBI()
        if not k32.VirtualQueryEx(p.h, C.c_void_p(addr), C.byref(m), C.sizeof(m)): break
        if m.State == 0x1000 and m.Protect in (0x02, 0x04, 0x08, 0x20, 0x40) and m.RegionSize < 0x10000000:
            yield addr, m.RegionSize, m.Protect
        addr += m.RegionSize


objs = []; swfs = []
for addr, size, prot in regions():
    d = p.read(addr, size)
    if not d: continue
    if prot in (0x04, 0x08, 0x40):
        for o in range(4, len(d) - 4, 4):
            v = struct.unpack_from('<I', d, o)[0]
            if (v >> 14) not in names: continue
            vt = struct.unpack_from('<I', d, o - 4)[0]
            if not 0x12000000 < vt < 0x13000000: continue
            objs.append((addr + o - 4, vt, names[v >> 14], (v & 0x3FFF) - 1))
    if swfdir:
        for m in re.finditer(rb'[GC]FX[\x06-\x0C]', d):
            o = m.start(); ln = struct.unpack_from('<I', d, o + 4)[0] if o + 8 <= len(d) else 0
            if 64 < ln < 0x2000000: swfs.append((addr + o, d[o:o + 4], ln))

print('%d objects' % len(objs))
for a, vt, n, num in objs:
    fm = p.u32(a + 0x3C) or 0
    view = p.u32(fm + 0x34) if 0x10000 < fm < 0x80000000 else 0
    vview = p.u32(view) if view and 0x10000 < view < 0x80000000 else 0
    print('%08x vt %08x %s_%d  +3C %08x  view %08x (vt %08x)' % (a, vt, n, num, fm, view or 0, vview or 0))
if swfdir:
    import os
    os.makedirs(swfdir, exist_ok=True)
    print('%d swf headers' % len(swfs))
    for a, sig, ln in swfs:
        raw = p.read(a, min(ln, 0x2000000))
        if not raw: continue
        body = raw
        if sig[:1] == b'C':
            try: body = sig.replace(b'C', b'G', 1) + raw[4:8] + zlib.decompressobj().decompress(raw[8:])
            except Exception: continue
        # SWF header: RECT (nbits) then rate/count; sanity: decompressed length matches
        ok = len(body) >= ln * 0.9
        fn = os.path.join(swfdir, '%08x.gfx' % a)
        if ok: open(fn, 'wb').write(body)
        print('%08x %s len %d %s' % (a, sig, ln, 'dumped' if ok else 'short %d' % len(body)))
