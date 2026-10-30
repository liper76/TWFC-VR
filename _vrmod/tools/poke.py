"""Debug: keep writing float values into the game for a while. Usage: poke.py <seconds> <hexaddr>=<float | int followed by i> [...]"""
import sys, struct, time, ctypes as C
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, k32

h = k32.OpenProcess(0x0008 | 0x0020 | 0x0010 | 0x0400, False, find_pid('TWFC.exe'))
pairs = [(int(a, 16), (struct.pack('<i', int(v[:-1])) if v.endswith('i') else struct.pack('<f', float(v)))) for a, v in (x.split('=') for x in sys.argv[2:])]
end = time.time() + float(sys.argv[1])
while time.time() < end:
    for a, b in pairs:
        k32.WriteProcessMemory(h, C.c_void_p(a), b, 4, None)
    time.sleep(0.001)
