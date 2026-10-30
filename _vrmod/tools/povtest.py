"""Write a yaw offset into a candidate POV copy for a few seconds and screenshot (research only)."""
import sys, struct, time, threading, ctypes as C
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from memscan import find_pid, k32
import subprocess

addr = int(sys.argv[1], 16)
off = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x10   # yaw offset inside the candidate (Loc 12 bytes + pitch)
out = sys.argv[3] if len(sys.argv) > 3 else 'pov'
pid = find_pid('TWFC.exe')
h = k32.OpenProcess(0x0008 | 0x0010 | 0x0020 | 0x0400, False, pid)
buf = C.create_string_buffer(4); n = C.c_size_t()
k32.ReadProcessMemory(h, C.c_void_p(addr + off), buf, 4, C.byref(n))
orig = struct.unpack('<i', buf.raw)[0]
print('orig yaw', orig)
stop = time.time() + 3.0
val = struct.pack('<i', orig + 16384)
while time.time() < stop:
    k32.WriteProcessMemory(h, C.c_void_p(addr + off), val, 4, C.byref(n))
    if time.time() > stop - 1.0 and not getattr(sys, 'shot', False):
        sys.shot = True
        subprocess.Popen(['powershell', '-NoProfile', '-Command',
            "Add-Type -AssemblyName System.Windows.Forms,System.Drawing; $s=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds; $b=New-Object System.Drawing.Bitmap $s.Width,$s.Height; $g=[System.Drawing.Graphics]::FromImage($b); $g.CopyFromScreen($s.Location,[System.Drawing.Point]::Empty,$s.Size); (New-Object System.Drawing.Bitmap $b,512,288).Save('C:\\Games\\Transformers - War for Cybertron\\_vrmod\\tools\\%s.png')" % out])
print('done')
