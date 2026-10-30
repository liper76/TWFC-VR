"""Disassemble virtual functions of a vtable (static, from TWFC.exe) and report those touching given displacements.
Usage: vtscan.py <vtable hex> <count> <disp hex>[,<disp hex>...]
"""
import sys, pefile, capstone

EXE = r"C:\Games\Transformers - War for Cybertron\Binaries\TWFC.exe"
pe = pefile.PE(EXE, fast_load=True); ib = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True


def u32(a): return int.from_bytes(pe.get_data(a - ib, 4), 'little')


def func_insns(a, maxlen=0x600):
    code = pe.get_data(a - ib, maxlen)
    out = []
    for ins in md.disasm(code, a):
        out.append(ins)
        if ins.mnemonic in ('ret', 'int3') and len(out) > 2: break
    return out


def main():
    vt = int(sys.argv[1], 16); n = int(sys.argv[2]); disps = [int(x, 16) for x in sys.argv[3].split(',')]
    for i in range(n):
        f = u32(vt + 4 * i)
        if not (0x10901000 <= f < 0x11948000): print('end at', i); break
        hits = []
        for ins in func_insns(f):
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.disp in disps:
                    hits.append('%x: %s %s' % (ins.address, ins.mnemonic, ins.op_str))
        if hits: print('vt[%d] = %x' % (i, f)); [print('    ' + h) for h in hits[:8]]


if __name__ == '__main__':
    main()
