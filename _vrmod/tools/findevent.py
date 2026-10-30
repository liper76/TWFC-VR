"""Find native code that pushes an FName constant (GNames index << 14) and the virtual calls right after it
(event stubs: FindFunctionChecked(Name) then this->ProcessEvent(Function, Parms)).
Usage: findevent.py <name index> [...]   (static: reads TWFC.exe from disk)"""
import sys, struct, pefile, capstone

exe = r'C:\Games\Transformers - War for Cybertron\Binaries\TWFC.exe'
pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
code = text.get_data()
va0 = base + text.VirtualAddress
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for a in sys.argv[1:]:
    val = int(a) << 14
    pat = b'\x68' + struct.pack('<I', val)
    o = code.find(pat)
    hits = 0
    while o >= 0 and hits < 6:
        hits += 1
        start = va0 + o
        print('--- push %x at %08x' % (val, start))
        for ins in md.disasm(code[o:o + 120], start):
            print('  %08x: %s %s' % (ins.address, ins.mnemonic, ins.op_str))
            if ins.mnemonic == 'ret': break
        o = code.find(pat, o + 1)
