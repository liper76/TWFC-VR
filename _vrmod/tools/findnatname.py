# Finds UE3 native name tables {L"execXxx", &Class::execXxx}: prints the function pointer next to the name.
# usage: python findnatname.py TWFC.exe execSetLocation [...]
import pefile,sys,struct,re
pe=pefile.PE(sys.argv[1],fast_load=True); ib=pe.OPTIONAL_HEADER.ImageBase
data=pe.__data__
# names in this build look like "intAActorexecSetLocation"
for name in sys.argv[2:]:
    pat=(name+'\0').encode('utf-16-le')
    for m in re.finditer(re.escape(pat),data):
        if m.start()%2: continue
        rva=pe.get_rva_from_offset(m.start()); va=ib+rva
        print('%s string %x'%(name,va))
        ref=struct.pack('<I',va)
        for r in re.finditer(re.escape(ref),data):
            o=r.start(); rv=pe.get_rva_from_offset(o)
            nxt=struct.unpack('<I',data[o+4:o+8])[0]; prv=struct.unpack('<I',data[o-4:o])[0]
            print('  ref at %x: prev %x next %x'%(ib+rv,prv,nxt))
