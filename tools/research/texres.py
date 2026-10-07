import blk,slb,struct,re,sys,collections
def names(p):
    m,v,e=blk.read(slb.D+p); return {n.lower():n for n,*_ in e}
art=names('Art/Art.blk')
def texnames(b):
    out=[]
    for m in re.finditer(rb'TextureFilename',b):
        j=m.end()
        # binary: TOKEN_OBRACE(0x0a) then TOKEN_STRING(0x02) u32 len chars, then terminator token
        if b[j:j+2]==b'\x0a\x00' and b[j+2:j+4]==b'\x02\x00':
            n=struct.unpack_from('<I',b,j+4)[0]; out.append(b[j+8:j+8+n].decode('latin1'))
    return out
tot=collections.Counter()
for lev,area in [(a.split('/')[0],a.split('/')[1]) for a in sys.argv[1:]]:
    A=names(f'levels/{lev}/{area}.blk'); L=names(f'levels/{lev}/{lev}.blk')
    texs=set()
    for k,n in A.items():
        if k.endswith('.x'): texs|=set(texnames(slb.entry(f'levels/{lev}/{area}.blk',n)))
    res=collections.Counter(); miss=[]; artdup=[]
    for t in sorted(texs):
        base=t.split('/')[-1].split('\\')[-1]
        hasext='.' in base
        cands=[base] if hasext else [base+'.dds',base+'.tga']
        where=None
        for c in cands:
            for nm,s in (('area',A),('level',L),('art',art)):
                if c.lower() in s: where=(nm,c); break
            if where: break
        if not where: miss.append(t)
        else:
            res[where[0]+':'+where[1].rsplit('.',1)[-1]]+=1
            if any(c.lower() in art for c in cands) and where[0]!='art': artdup.append(t)
    print(lev,area,'distinct tex',len(texs),dict(res),'missing',miss[:10],len(miss),'also-in-Art',artdup[:5],len(artdup))
