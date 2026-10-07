import os,struct,collections,sys
D=sys.argv[1]
dds=collections.Counter(); tga=collections.Counter(); ex=collections.defaultdict(list); dims=collections.Counter()
for dp,dn,fn in os.walk(D):
  for f in fn:
    if not f.lower().endswith('.blk'): continue
    p=os.path.join(dp,f); d=open(p,'rb').read(); cnt=struct.unpack_from('<I',d,8)[0]
    for i in range(cnt):
      o=12+i*56; name=d[o:o+40].split(b'\0')[0].decode('latin1'); cs,us,fl,off=struct.unpack_from('<IIII',d,o+40)
      b=d[off:off+us]; ln=name.lower(); key=(os.path.relpath(p,D),name)
      if ln.endswith('.dds'):
        if b[:4]!=b'DDS ': dds['badmagic']+=1; ex['ddsbad'].append(key); continue
        (sz,flags,h,w,pitch,depth,mips)=struct.unpack_from('<7I',b,4)
        pf=struct.unpack_from('<8I',b,76); caps=struct.unpack_from('<4I',b,108)
        fcc=struct.pack('<I',pf[2]) if pf[1]&4 else b''
        k=(fcc.decode('latin1'),pf[1],pf[3],tuple(hex(x) for x in pf[4:8]) if not fcc else (),mips,hex(caps[1]))
        dds[k]+=1; dims[(w,h)]+=1
        if not fcc and len(ex['unc'])<10: ex['unc'].append((key,w,h,len(b)))
        if w>4096 or h>4096 or w==0 or h==0: ex['bigdim'].append(key)
        if w&3 or h&3: ex['non4'].append((key,w,h))
      elif ln.endswith('.tga'):
        idl,cmt,it,cmo,cml,cmd,xo,yo,w,h,bpp,desc=struct.unpack_from('<BBBHHBHHHHBB',b,0)
        tga[(it,cmt,bpp,desc&0x0f,(desc>>4)&3,cmd)]+=1
        if len(ex['tga%d'%it])<3: ex['tga%d'%it].append((key,w,h,len(b),idl))
print('DDS',sum(dds.values())); [print(' ',k,v) for k,v in dds.most_common()]
print('TGA',sum(tga.values()),'(type,cmaptype,bpp,alphabits,origin,cmapdepth)'); [print(' ',k,v) for k,v in tga.most_common()]
for k,v in ex.items(): print(k,len(v),v[:6])
print('maxdim',max(dims, key=lambda x:x[0]*x[1]))
