"""Find code in TWFC.exe that uses an absolute address (e.g. a global variable): prints each instruction with the
guessed start of its function (after int3/ret padding). Read-only.
Usage: findabsref.py <hexaddr> [...]"""
import sys, struct, pefile, capstone

exe = r'C:\Games\Transformers - War for Cybertron\BinariesVR\TWFC.exe'
pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
t0, t1 = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def func_start(rva):
    i = rva
    while i > t0 and rva - i < 0x4000:
        if img[i - 1] == 0xCC and img[i - 2] == 0xCC:
            return i
        i -= 1
    return None


for a in sys.argv[1:]:
    target = int(a, 16)
    pat = struct.pack('<I', target)
    print('==== %08X' % target)
    pos = img.find(pat, t0, t1)
    while pos != -1:
        hit = None
        for back in range(1, 8):   # find an instruction that contains the operand
            for ins in md.disasm(img[pos - back:pos - back + 16], base + pos - back, 1):
                if ins.size > back and ins.size >= back + 4 and '0x%x' % target in ins.op_str:
                    hit = ins
            if hit:
                break
        if hit:
            fs = func_start(pos)
            print('%08x: %-6s %-40s func %s' % (hit.address, hit.mnemonic, hit.op_str, '%08x' % (base + fs) if fs else '?'))
        pos = img.find(pat, pos + 1, t1)
