"""For each return address, show the call instruction right before it (filters stale stack values).
Usage: callsite.py <exe> <hexaddr> [...]"""
import sys, pefile, capstone

pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for a in sys.argv[2:]:
    ret = int(a, 16)
    found = None
    for n in (5, 6, 2, 3, 7):   # call rel32, call [mem], call reg, call [reg+d8], call [reg+d32]
        code = pe.get_data(ret - n - base, n)
        ins = list(md.disasm(code, ret - n))
        if len(ins) == 1 and ins[0].mnemonic == 'call' and ins[0].size == n:
            found = '%s %s' % (ins[0].mnemonic, ins[0].op_str); break
    print('%08x  %s' % (ret, found or '-'))
