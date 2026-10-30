import sys, struct, time, math
sys.path.insert(0, r'C:\Games\Transformers - War for Cybertron\_vrmod\tools')
from memscan import find_pid, Proc
p = Proc(find_pid('TWFC.exe'))
pawn = int(sys.argv[1],16); dur = float(sys.argv[2])
t0 = time.time(); mx = 0; out = []
prev = None
while time.time() - t0 < dur:
    v = struct.unpack('<3f', p.read(pawn+0xF0, 12)); l = struct.unpack('<3f', p.read(pawn+0xD8, 12))
    s = math.hypot(v[0], v[1]); mx = max(mx, s)
    out.append('%.2f speed %.0f vz %.0f z %.0f' % (time.time()-t0, s, v[2], l[2]))
    time.sleep(0.1)
print('\n'.join(out[::5])); print('max horizontal speed %.0f' % mx)