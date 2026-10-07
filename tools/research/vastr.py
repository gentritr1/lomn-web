import os,struct,sys
EXE=os.path.join(os.environ.get("LOMN_DATA",""),"install","LEGOBionicle.exe")  # source tools/env.sh
d=open(EXE,'rb').read()
pe=struct.unpack_from('<I',d,0x3c)[0]
nsec=struct.unpack_from('<H',d,pe+6)[0]; opt=struct.unpack_from('<H',d,pe+20)[0]
base=struct.unpack_from('<I',d,pe+24+28)[0]
secs=[]
o=pe+24+opt
for i in range(nsec):
    name,vs,va,rs,ra=struct.unpack_from('<8sIIII',d,o+40*i)[:5]
    secs.append((name,va,vs,ra,rs))
def off(v):
    r=v-base
    for n,va,vs,ra,rs in secs:
        if va<=r<va+max(vs,rs): return ra+r-va
for a in sys.argv[1:]:
    v=int(a,16); o=off(v); e=d.index(b'\0',o)
    print(hex(v), repr(d[o:e].decode('latin1')))
