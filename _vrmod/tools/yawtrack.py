"""Print a pawn's yaw (degrees), speed and position every 0.5 s for a while (read-only): vehicle steering tests.
Usage: yawtrack.py <hexpawn> <seconds>"""
import sys, struct, time, math
sys.path.insert(0, r'C:\Games\Transformers - War for Cybertron\_vrmod\tools')
from memscan import find_pid, Proc
p = Proc(find_pid('TWFC.exe'))
pawn = int(sys.argv[1], 16); dur = float(sys.argv[2])
t0 = time.time()
while time.time() - t0 < dur:
    yaw = struct.unpack('<i', p.read(pawn + 0xE8, 4))[0] * 360.0 / 65536.0
    v = struct.unpack('<3f', p.read(pawn + 0xF0, 12)); l = struct.unpack('<3f', p.read(pawn + 0xD8, 12))
    pc = struct.unpack('<I', p.read(pawn + 0x1D4, 4))[0]
    cy = struct.unpack('<i', p.read(pc + 0xE8, 4))[0] * 360.0 / 65536.0 if pc else 0
    vdir = math.degrees(math.atan2(v[1], v[0]))
    print('%5.1f yaw %7.1f  ctrl %7.1f  move dir %7.1f  speed %5.0f  at %.0f %.0f' % (time.time() - t0, yaw % 360, cy % 360, vdir % 360, math.hypot(v[0], v[1]), l[0], l[1]))
    time.sleep(0.5)
