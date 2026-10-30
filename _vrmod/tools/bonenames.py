"""Find the skeleton (bone names) of a skeletal mesh component: follows pointers to a TArray of FMeshBone whose
first field is an FName (read-only). Usage: bonenames.py <component hex> <bone count> [component span]"""
import sys, struct
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
narr = p.u32(0x12B2F0F4); nnum = p.u32(0x12B2F0F8)
ents = struct.unpack('<%dI' % nnum, p.read(narr, 4 * nnum))


def name(i):
    if not 0 <= i < nnum or not ents[i]: return None
    return p.cstr(ents[i] + 8, True, 48)


comp = int(sys.argv[1], 16); count = int(sys.argv[2])
span = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x300
body = p.read(comp, span)
for off in range(0, len(body), 4):
    obj = struct.unpack_from('<I', body, off)[0]
    if obj < 0x1000000: continue
    ob = p.read(obj, 0x400)
    if not ob: continue
    for o2 in range(0, len(ob) - 12, 4):
        ptr, num, mx = struct.unpack_from('<III', ob, o2)
        if num != count or mx < num or ptr < 0x1000000: continue
        data = p.read(ptr, count * 0x80)
        if not data: continue
        for stride in range(0x20, 0x80, 4):
            names = [name(struct.unpack_from('<I', data, i * stride)[0]) for i in range(min(count, len(data) // stride))]
            if sum(1 for n in names if n) >= len(names) * 0.9:
                print('comp+%03x -> mesh %08x +%03x bones %08x stride %#x' % (off, obj, o2, ptr, stride))
                for i, n in enumerate(names):
                    par = struct.unpack_from('<i', data, i * stride + stride - 8)[0]
                    print('  %3d %-28s %s' % (i, n, ' '.join('%d' % x for x in struct.unpack_from('<4i', data, i * stride + stride - 16))))
                sys.exit()
