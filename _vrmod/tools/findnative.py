# Finds GRegisterNative(index, func) calls: push func; push index; call X  (or reversed)
# usage: python findnative.py TWFC.exe <index dec> [<index dec> ...]
import pefile,sys,struct,re,collections
pe=pefile.PE(sys.argv[1],fast_load=True); ib=pe.OPTIONAL_HEADER.ImageBase
idx=[int(a) for a in sys.argv[2:]]
for s in pe.sections:
    d=s.get_data(); va=ib+s.VirtualAddress
    for i in idx:
        pat=b'\x68'+struct.pack('<I',i)
        for m in re.finditer(re.escape(pat),d):
            o=m.start()
            # preceding push imm32
            pre=d[o-5:o]
            post=d[o+5:o+10]
            if len(pre)==5 and pre[0]==0x68 and post[0]==0xE8:
                f=struct.unpack('<I',pre[1:])[0]
                tgt=va+o+10+struct.unpack('<i',post[1:])[0]
                print('idx %d at %x: func %x  call %x'%(i,va+o,f,tgt))
