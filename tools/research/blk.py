import struct,sys,os,collections
def read(p):
    f=open(p,'rb'); h=f.read(12)
    magic,ver,n=struct.unpack('<4sII',h)
    ents=[]
    for i in range(n):
        name,bs,fs,fl,off=struct.unpack('<40sIIII',f.read(56))
        ents.append((name.split(b'\0')[0].decode('latin1'),bs,fs,fl,off))
    return magic,ver,ents
def sortedci(ents):
    def k(s): return bytes([c|0x60 if 0x41<=c<=0x5a else c for c in s.encode('latin1')])
    ks=[k(e[0]) for e in ents]
    return all(ks[i]<ks[i+1] for i in range(len(ks)-1))
if __name__=='__main__':
    for p in sys.argv[1:]:
        m,v,e=read(p)
        exts=collections.Counter(os.path.splitext(x[0])[1].lower() for x in e)
        flags=collections.Counter(x[3] for x in e)
        print('==',p.split('data/')[-1],m,'ver',v,'n',len(e),'flags',dict(flags),'sortedCI',sortedci(e),'size',os.path.getsize(p))
        print('  exts',dict(exts))
        if '-v' in os.environ.get('V',''):
            for x in e: print('   ',x)
