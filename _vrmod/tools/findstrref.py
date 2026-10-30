"""Find code referencing a string (ASCII or UTF-16) in TWFC.exe: prints the string address and each instruction that
uses it, with the start of the containing function guessed from the preceding int3/ret padding.
Usage: findstrref.py "<text>" """
import sys, struct, pefile, capstone

exe = r'C:\Games\Transformers - War for Cybertron\Binaries\TWFC.exe'
pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
t0, t1 = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
s = sys.argv[1]
for enc in ('utf-16-le', 'ascii'):
    pat = s.encode(enc)
    o = img.find(pat)
    while o >= 0:
        va = base + o
        print('string (%s) at %08x' % (enc, va))
        ref = struct.pack('<I', va)
        r = img.find(ref, t0, t1)
        while r >= 0:
            # function start: walk back to int3 padding
            f = r
            while f > t0 and img[f - 1] != 0xCC: f -= 1
            print('  ref at %08x  (function ~%08x)' % (base + r, base + f))
            r = img.find(ref, r + 1, t1)
        o = img.find(pat, o + 1)
