# Census of every _char.slb record in the install (M2a-3 T11a ruling, natives.md §5.4). Run from tools/research with LOMN_DATA set:
#   cd tools/research && PYTHONPATH=. ../../.venv/bin/python char_census.py
import os,re,struct,collections,blk,slb
D=os.environ['LOMN_DATA']+'/install/data'
def ids_of(b):
    cnt,offs=slb.relocs(b)
    end=len(b) if cnt is None else ((len(b)-4)&~3)-4-4*cnt   # data end (Slb::parse_impl)
    n,base=struct.unpack_from('<II',b,4)
    if cnt is None or 8 not in offs or base+n*0x34>end: return None
    return [struct.unpack_from('<I',b,base+i*0x34)[0] for i in range(n)]
def s(i): return i.to_bytes(4,'big').rstrip(b'\0').decode('latin1')
rows=[]
for root,ds,fs in sorted(os.walk(D)):
    for f in sorted(fs):
        if not f.lower().endswith('.blk'): continue
        p=os.path.join(root,f); rel=os.path.relpath(p,D)
        for n,bs,fsz,fl,off in blk.read(p)[2]:
            if n.lower().endswith("_char.slb") and not rel.startswith("levels"): print("not an area file (skipped):",rel,n); continue
            if n.lower().endswith("_char.slb"):
                fh=open(p,'rb'); fh.seek(off); b=fh.read(fsz)
                r=ids_of(b)
                if r is None: print("NOT the area layout (no pointer at +8 or records past the data):",rel,n,"len",len(b),"u32[0:3]",struct.unpack_from("<3I",b,0)); continue
                rows.append((rel,n,r))
tot=sum(len(r[2]) for r in rows)
print('files',len(rows),'records',tot)
for rel,n,ids in rows: print(f'  {rel:28s} {n:18s} n={len(ids):3d}', ' '.join(s(i) for i in ids))
print('cnt1 records:',[(r,n) for r,n,ids in rows for i in ids if s(i).lower()=='cnt1'])
print('intra-file duplicates:',[(r,n,s(i),c) for r,n,ids in rows for i,c in collections.Counter(ids).items() if c>1])
num=re.compile(r'^..\d\d$')
print('ids of the numbered form %c%c%02d:',sorted({s(i) for r,n,ids in rows for i in ids if num.match(s(i))}))
lv=collections.defaultdict(lambda:collections.defaultdict(set))
for rel,n,ids in rows:
    for i in ids: lv[rel.split('/')[1] if rel.startswith('levels') else rel][s(i)].add(n)
for L in sorted(lv):
    sh={k:sorted(v) for k,v in lv[L].items() if len(v)>1}
    print('shared across files in',L,':',len(sh), sh)
allids=collections.Counter(s(i) for r,n,ids in rows for i in ids)
print('distinct ids',len(allids))
print('max total records of one id over ALL area files (nothing ever removed):',allids.most_common(3))
pre=collections.Counter()
for k,v in allids.items(): pre[k[:2]]+=v
print('max total records sharing a 2-char numbered prefix (nothing ever removed):',pre.most_common(3))
