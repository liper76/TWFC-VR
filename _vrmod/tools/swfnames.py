"""Names inside a Scaleform GFX/SWF file: exporter file name, exported symbols, placed instance names (per sprite) and
the ActionScript 2 constant pool strings. Usage: swfnames.py <file.gfx> [--tree] [--strings]"""
import sys, struct
sys.stdout.reconfigure(encoding='utf-8', errors='replace')


def tags(d, o, end):
    while o + 2 <= end:
        cl = struct.unpack_from('<H', d, o)[0]; o += 2
        code, ln = cl >> 6, cl & 0x3F
        if ln == 0x3F: ln = struct.unpack_from('<I', d, o)[0]; o += 4
        yield code, o, ln
        o += ln
        if code == 0: break


def cstr(d, o):
    e = d.index(b'\0', o)
    return d[o:e].decode('latin1'), e + 1


def place_name(d, o, code):
    # PlaceObject2 (26) / PlaceObject3 (70): flags, [flags2], depth, [class name], [char id], [matrix], [cxform], [ratio],
    # [name] ...; matrix/cxform are bit fields - skip them by parsing
    f = d[o]; o += 1
    f2 = 0
    if code == 70: f2 = d[o]; o += 1
    depth = struct.unpack_from('<H', d, o)[0]; o += 2
    if code == 70 and (f2 & 0x08 or (f2 & 0x10 and f & 0x02)):
        _, o = cstr(d, o)
    cid = None
    if f & 0x02: cid = struct.unpack_from('<H', d, o)[0]; o += 2
    if f & 0x04: o = skip_matrix(d, o)
    if f & 0x08: o = skip_cxform(d, o)
    if f & 0x10: o += 2
    name = None
    if f & 0x20: name, o = cstr(d, o)
    return depth, cid, name


class Bits:
    def __init__(s, d, o): s.d, s.o, s.b = d, o, 0
    def u(s, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((s.d[s.o] >> (7 - s.b)) & 1); s.b += 1
            if s.b == 8: s.b = 0; s.o += 1
        return v
    def end(s): return s.o + (1 if s.b else 0)


def skip_matrix(d, o):
    b = Bits(d, o)
    if b.u(1): n = b.u(5); b.u(n); b.u(n)
    if b.u(1): n = b.u(5); b.u(n); b.u(n)
    n = b.u(5); b.u(n); b.u(n)
    return b.end()


def skip_cxform(d, o):
    b = Bits(d, o)
    ha, hm = b.u(1), b.u(1); n = b.u(4)
    if hm: [b.u(n) for _ in range(4)]
    if ha: [b.u(n) for _ in range(4)]
    return b.end()


def const_pools(d, o, end, out):
    # AS2 actions: ActionConstantPool 0x88
    while o < end:
        a = d[o]; o += 1
        if a == 0: continue
        ln = 0
        if a >= 0x80: ln = struct.unpack_from('<H', d, o)[0]; o += 2
        if a == 0x88:
            n = struct.unpack_from('<H', d, o)[0]; p = o + 2
            for _ in range(n):
                s, p = cstr(d, p); out.add(s)
        elif a == 0x9B:   # DefineFunction: name
            s, _ = cstr(d, o); out.add(s)
        o += ln


d = open(sys.argv[1], 'rb').read()
nbits = d[8] >> 3
o = 8 + (5 + 4 * nbits + 7) // 8 + 4
exported = {}; strings = set(); sprites = {}; info = None
root = []


def walk(o, end, place, depth=0):
    for code, p, ln in tags(d, o, end):
        if code == 1000:
            info = d[p:p + ln]
            i = info.find(b'.swf'); j = info.rfind(b'\0', 0, i) if i > 0 else -1
            print('exporter info:', info[max(j, 0):i + 4] if i > 0 else info[:60])
        elif code == 56:
            n = struct.unpack_from('<H', d, p)[0]; q = p + 2
            for _ in range(n):
                cid = struct.unpack_from('<H', d, q)[0]; s, q = cstr(d, q + 2); exported[cid] = s
        elif code in (26, 70):
            try:
                dep, cid, name = place_name(d, p, code)
                if name or cid is not None: place.append((dep, cid, name))
            except Exception: pass
        elif code == 39:
            sid = struct.unpack_from('<H', d, p)[0]
            sprites[sid] = []
            walk(p + 4, p + ln, sprites[sid], depth + 1)
        elif code in (12, 59):
            q = p + (2 if code == 59 else 0)
            try: const_pools(d, q, p + ln, strings)
            except Exception: pass


walk(o, len(d), root)
print('exported:', ', '.join('%d=%s' % kv for kv in sorted(exported.items())))


def show(place, ind, seen):
    for dep, cid, name in place:
        if not name and cid not in sprites: continue
        label = exported.get(cid, '')
        print('%s%s  (char %s %s, depth %d)' % ('  ' * ind, name or '-', cid, label, dep))
        if cid in sprites and cid not in seen and ind < 8:
            show(sprites[cid], ind + 1, seen | {cid})


if '--tree' in sys.argv:
    print('--- _root')
    show(root, 1, set())
    for cid, lab in sorted(exported.items()):
        if cid in sprites:
            print('--- exported %s (%d)' % (lab, cid)); show(sprites[cid], 1, {cid})
if '--strings' in sys.argv:
    print('--- AS strings:', ' '.join(sorted(strings)))
