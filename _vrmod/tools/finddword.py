"""Find every place in TWFC.exe (any section) where a dword value is stored, e.g. a function address in a vtable
or a call target. Prints the address, its section and, in code, the decoded instruction. Read-only.
Usage: finddword.py <hexvalue> [...]"""
import sys, struct, pefile, capstone

exe = r'C:\Games\Transformers - War for Cybertron\BinariesVR\TWFC.exe'
pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def section(rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + s.Misc_VirtualSize:
            return s.Name.rstrip(b'\0').decode()
    return '?'


for a in sys.argv[1:]:
    v = int(a, 16)
    print('==== %08X' % v)
    pat = struct.pack('<I', v)
    pos = img.find(pat)
    while pos != -1:
        print('%08x %s' % (base + pos, section(pos)))
        pos = img.find(pat, pos + 1)
    # direct calls/jumps (rel32) to the value
    for s in pe.sections:
        if not s.Name.startswith(b'.text'):
            continue
        t0, t1 = s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize
        for op in (b'\xe8', b'\xe9'):
            i = img.find(op, t0, t1)
            while i != -1 and i < t1 - 5:
                if base + i + 5 + struct.unpack_from('<i', img, i + 1)[0] == v:
                    print('%08x %s %s' % (base + i, 'call' if op == b'\xe8' else 'jmp', section(i)))
                i = img.find(op, i + 1, t1)
