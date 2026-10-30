"""Find UObject::ProcessEvent's vtable slot and FindFunctionChecked: script event wrappers look like
    call FindFunctionChecked ; push eax (or the result is passed) ; ... ; call [reg+SLOT]
Ranks (direct call target, virtual slot) pairs seen within a few instructions of each other."""
import pefile, capstone, collections
exe = r'C:\Games\Transformers - War for Cybertron\Binaries\TWFC.exe'
pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
code = text.get_data()
start = base + text.VirtualAddress
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.skipdata = True
pairs = collections.Counter()
examples = {}
recent = collections.deque(maxlen=8)
loaded = {}   # register -> vtable slot loaded into it ("mov eax, dword ptr [edx + 0x104]")
REGS = ('eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp')
for ins in md.disasm(code, start):
    op = ins.op_str
    if ins.mnemonic == 'mov' and op.count('[') == 1 and op.split(',')[0] in REGS:
        inner = op[op.index('[') + 1:op.index(']')]
        r2, _, off = inner.partition(' + ')
        if r2 in REGS and off.startswith('0x') and int(off, 16) < 0x400:
            loaded[op.split(',')[0]] = int(off, 16)
        else:
            loaded.pop(op.split(',')[0], None)
    if ins.mnemonic == 'call':
        slot = None
        if op.startswith('0x'):
            recent.append(('d', int(op, 16), ins.address))
        elif op in loaded:
            slot = loaded[op]
        elif op.startswith('dword ptr [') and '+' in op and op.count('[') == 1:
            inner = op[op.index('[') + 1:-1]
            reg, _, off = inner.partition(' + ')
            if off.startswith('0x') and reg in REGS:
                slot = int(off, 16)
        if slot is not None:
            if True:
                for kind, tgt, at in recent:
                    if kind == 'd' and ins.address - at < 24:
                        pairs[(tgt, slot)] += 1
                        examples.setdefault((tgt, slot), ins.address)
            recent.append(('v', 0, ins.address))
    elif ins.mnemonic in ('ret', 'int3'):
        recent.clear()
for (tgt, slot), n in pairs.most_common(15):
    print('target %08x slot 0x%x (index %d): %d  e.g. %08x' % (tgt, slot, slot // 4, n, examples[(tgt, slot)]))
