import os,struct,sys,blk
D=os.path.join(os.environ.get("LOMN_DATA",""),"install","data")+os.sep  # source tools/env.sh; check_datacheck.py overrides from --install
def entry(blkpath,name):
    m,v,e=blk.read(D+blkpath)
    for n,bs,fs,fl,off in e:
        if n.lower()==name.lower():
            f=open(D+blkpath,'rb'); f.seek(off); return f.read(fs)
def relocs(b):
    n=len(b); end=(n-4)&~3
    magic=struct.unpack_from('<I',b,end)[0]
    if magic!=0xc0ffee: return None,[]
    cnt=struct.unpack_from('<I',b,end-4)[0]
    offs=[struct.unpack_from('<I',b,end-4-4*(i+1))[0]&~3 for i in range(cnt)]
    return cnt,offs
if __name__=='__main__':
    b=entry(sys.argv[1],sys.argv[2])
    cnt,offs=relocs(b)
    print('len',len(b),'reloc count',cnt,'reloc offs',sorted(offs)[:40])
    print('head u32',[hex(x) for x in struct.unpack_from('<16I',b,0)])
    import binascii
    for i in range(0,min(len(b),int(sys.argv[3]) if len(sys.argv)>3 else 256),16):
        chunk=b[i:i+16]
        print('%04x'%i,chunk.hex(' '),''.join(chr(c) if 32<=c<127 else '.' for c in chunk))
