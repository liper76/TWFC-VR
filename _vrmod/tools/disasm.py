import pefile,capstone,sys
pe=pefile.PE(sys.argv[1],fast_load=True); ib=pe.OPTIONAL_HEADER.ImageBase
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
start=int(sys.argv[2],16); n=int(sys.argv[3],16)
code=pe.get_data(start-ib,n)
for ins in md.disasm(code,start): print('%x: %s %s'%(ins.address,ins.mnemonic,ins.op_str))
