"""List the attachments of a pawn's mesh (SkeletalMeshComponent.Attachments at +0x260, 0x30 per entry) (read-only).
Usage: attachments.py <pawn hex>"""
import sys, struct
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, Proc

p = Proc(find_pid('TWFC.exe'))
narr = p.u32(0x12B2F0F4)


def fname(v):
    e = p.u32(narr + 4 * (v >> 14))
    n = p.cstr(e + 8, True, 64) if e else '?'
    return '%s_%d' % (n, (v & 0x3FFF) - 1) if v & 0x3FFF else n


pawn = int(sys.argv[1], 16)
m = p.u32(pawn + 0x3F4)
arr, num = p.u32(m + 0x260), p.u32(m + 0x264)
print('mesh %08x: %d attachments' % (m, num))
for i in range(num):
    e = p.read(arr + i * 0x30, 0x30)
    c, bone = struct.unpack_from('<II', e)
    rel = struct.unpack_from('<9f', e, 8)
    mesh = p.u32(c + 0x1BC) or 0
    mn = fname(p.u32(mesh + 4)) if 0x1000000 < mesh < 0x7FFF0000 else '-'
    print('%2d comp %08x %-34s bone %-22s mesh %-26s rel %s' % (i, c, fname(p.u32(c + 4)), fname(bone), mn,
                                                                ' '.join('%.2f' % x for x in rel)))
