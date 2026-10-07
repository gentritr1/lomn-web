import os,struct,collections,sys
sys.path.insert(0,os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from xbin import Reader, walk, ints, floats
D=sys.argv[1]
st=collections.Counter(); ex=collections.defaultdict(list)
for dp,dn,fn in os.walk(D):
  for f in fn:
    if not f.lower().endswith('.blk'): continue
    p=os.path.join(dp,f); d=open(p,'rb').read(); cnt=struct.unpack_from('<I',d,8)[0]
    for i in range(cnt):
      o=12+i*56; name=d[o:o+40].split(b'\0')[0].decode('latin1'); cs,us,fl,off=struct.unpack_from('<IIII',d,o+40)
      if not name.lower().endswith('.x'): continue
      top=Reader(d[off:off+us]).parse()
      key=(os.path.relpath(p,D),name)
      for o2 in walk(top):
        if o2.refs: st['has-refs:'+o2.type]+=1; ex['refs'].append(key)
        if o2.type=='Mesh':
          iv=ints(o2.values); nv=iv[0]; nf=iv[1]
          kids=collections.Counter(c.type for c in o2.children)
          for k,v in kids.items():
            if v>1: st['dup-child:'+k]+=1
          for c in o2.children:
            st['child:'+c.type]+=1
            ci=ints(c.values)
            if c.type=='MeshNormals':
              nn=ci[0]; nfn=ci[1]
              st['normals==verts' if nn==nv else 'normals!=verts']+=1
              # face normal indices
              k=2; same=True
              fk=2
              for j in range(nfn):
                cc=ci[k]; nidx=ci[k+1:k+1+cc]; k+=1+cc
                cf=iv[fk]; pidx=iv[fk+1:fk+1+cf]; fk+=1+cf
                if nidx!=pidx: same=False
              st['fn==faces' if nfn==nf else 'fn!=faces']+=1
              st['normidx==posidx' if same else 'normidx!=posidx']+=1
              if not same and len(ex['nsplit'])<5: ex['nsplit'].append(key)
            elif c.type=='MeshTextureCoords':
              st['uv==verts' if ci[0]==nv else 'uv!=verts']+=1
            elif c.type=='MeshVertexColors':
              nc=ci[0]; idx=ci[1::1][:0]
              # values: nc, then per entry int index + 4 floats
              vals=c.values; ii=[v for k2,v in vals if k2=='i'][1:]
              st['vc==verts' if nc==nv else 'vc!=verts']+=1
              st['vcidx-identity' if ii==list(range(nc)) else 'vcidx-other']+=1
              if ii!=list(range(nc)) and len(ex['vc'])<5: ex['vc'].append((key,ii[:10]))
            elif c.type=='MeshMaterialList':
              nm,nfi=ci[0],ci[1]
              st['nfi==nf' if nfi==nf else ('nfi<nf' if nfi<nf else 'nfi>nf')]+=1
              if nfi!=nf and len(ex['nfi'])<5: ex['nfi'].append((key,nm,nfi,nf))
              inl=[x for x in c.children if x.type=='Material']
              if inl and c.refs: st['mixed-inline-refs']+=1
              if c.refs: st['mat-refs-only']+=1
              for x in c.children:
                if x.type!='Material': st['mml-child:'+x.type]+=1
                else:
                  tx=[y for y in x.children if y.type=='TextureFilename']
                  st['mat-tex%d'%len(tx)]+=1
                  for y in x.children:
                    if y.type!='TextureFilename': st['mat-child:'+y.type]+=1
                  fl=floats(x.values)
                  if len(fl)!=11: st['matfloats%d'%len(fl)]+=1
          # face sizes
          k=2
          for j in range(nf):
            st['fsz%d'%iv[k]]+=1; k+=1+iv[k]
print(dict(sorted(st.items())))
for k,v in ex.items(): print(k,v[:5])
