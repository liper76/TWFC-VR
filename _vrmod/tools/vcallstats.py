"""Statistics of virtual call offsets preceded by "push 0; push X; push Y" (a thiscall with three arguments whose
last one is NULL, like UObject::ProcessEvent(Function, Parms, Result=NULL)). Static, reads TWFC.exe."""
import sys, collections, pefile, capstone

exe = r'C:\Games\Transformers - War for Cybertron\Binaries\TWFC.exe'
pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
code = text.get_data()
va0 = base + text.VirtualAddress
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.skipdata = True
stats = collections.Counter()
examples = {}
recent = collections.deque(maxlen=6)
for ins in md.disasm(code, va0):
    m, op = ins.mnemonic, ins.op_str
    if m == 'call' and op.startswith('dword ptr [e') and '+' in op and ']' in op:
        pushes = [r for r in recent if r[0] == 'push']
        if len(pushes) >= 3 and pushes[-3][1] == '0':
            off = op.split('+')[-1].rstrip(']').strip()
            stats[off] += 1
            examples.setdefault(off, ins.address)
    recent.append((m, op))
for off, n in stats.most_common(12):
    print('%-8s %5d  e.g. %08x' % (off, n, examples[off]))
