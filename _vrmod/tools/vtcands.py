"""List virtual functions of a vtable (static, from TWFC.exe) whose code returns with a given stack cleanup and
optionally calls a given function (e.g. _alloca_probe). Usage: vtcands.py <vtable hex> <count> <ret bytes> [callee hex]"""
import sys, struct, pefile, capstone

exe = r'C:\Games\Transformers - War for Cybertron\Binaries\TWFC.exe'
pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
vt, count, retn = int(sys.argv[1], 16), int(sys.argv[2]), int(sys.argv[3])
callee = int(sys.argv[4], 16) if len(sys.argv) > 4 else None
for i in range(count):
    fn = struct.unpack_from('<I', img, vt - base + i * 4)[0]
    if not base <= fn < base + len(img): continue
    rets, calls, size = set(), [], 0
    for ins in md.disasm(img[fn - base:fn - base + 0x600], fn):
        size = ins.address - fn
        if ins.mnemonic == 'ret': rets.add(int(ins.op_str, 16) if ins.op_str else 0)
        if ins.mnemonic == 'call' and ins.op_str.startswith('0x'): calls.append(int(ins.op_str, 16))
        if ins.mnemonic == 'int3': break
    if retn in rets and (callee is None or callee in calls):
        print('[%3d] +%03x %08x size %x rets %s calls %d' % (i, i * 4, fn, size, sorted(rets), len(calls)))
