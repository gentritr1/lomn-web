"""Rewrite Ghidra's x87 FPU-status-word compare idioms into plain C comparisons.

fcom sets C0 (AH bit 0x01) when st0 < src, C3 (0x40) when equal, C2 (0x04) when unordered (NaN).
Ghidra renders `fnstsw ax; test ah,M; j..` as
    (byte)( T | T ... ) == V      with T = (byte)((ushort)((ushort)(COND) << K) >> 8)  [bit K-8]
                                  or T = COND                                          [bit 0]
We evaluate the expression for the three ordered outcomes (LT, EQ, GT) and emit the matching
operator. The NaN outcome is deliberately ignored (marked with a trailing comment).
"""
import re,sys
def match_paren(s,i):            # s[i]=='(' -> index of matching ')'
    depth=0
    for j in range(i,len(s)):
        if s[j]=='(': depth+=1
        elif s[j]==')':
            depth-=1
            if depth==0: return j
    return -1
def split_top(s,sep):
    parts=[];depth=0;last=0;i=0
    while i<len(s):
        c=s[i]
        if c=='(': depth+=1
        elif c==')': depth-=1
        elif depth==0 and s.startswith(sep,i): parts.append(s[last:i]); last=i+len(sep); i+=len(sep); continue
        i+=1
    parts.append(s[last:]); return [p.strip() for p in parts]
def strip_outer(s):
    s=s.strip()
    while s.startswith('(') and match_paren(s,0)==len(s)-1: s=s[1:-1].strip()
    return s
TERM=re.compile(r'^\(byte\)\(\(ushort\)\(\(ushort\)(\(.*\)) << (0x[0-9a-f]+|\d+)\) >> 8\)$',re.S)
def term(t):
    t=re.sub(r'\s+\)',')',re.sub(r'\(\s+','(',' '.join(t.split()))); m=TERM.match(t)
    if m: return strip_outer(m.group(1)), int(m.group(2),0)-8
    return strip_outer(t), 0
def classify(cond):
    if cond.startswith('NAN('): return 'nan',None
    for op,kind in ((' < ','lt'),(' == ','eq')):
        p=split_top(cond,op)
        if len(p)==2: return kind,(p[0],p[1])
    return None,None
OUT={frozenset(['LT']):'<',frozenset(['EQ']):'==',frozenset(['GT']):'>',frozenset(['LT','EQ']):'<=',
     frozenset(['GT','EQ']):'>=',frozenset(['LT','GT']):'!='}
CMP=re.compile(r'^\s*(==|!=)\s*(0x[0-9a-f]+|\d+)')
def rewrite_at(s,i):
    """s[i:] starts with '(byte)(' ; return (new_text,end) or None."""
    o=i+len('(byte)'); c=match_paren(s,o)
    if c<0: return None
    inner=s[o+1:c]
    if 'NAN(' not in inner and '<< 0xe' not in inner and '<< 10' not in inner: return None
    m=CMP.match(s[c+1:])
    if not m: return None
    terms=[term(t) for t in split_top(' '.join(inner.split()),'|')]
    bits={};ops=None
    for cond,bit in terms:
        kind,opnd=classify(cond)
        if kind is None: return None
        bits[bit]=kind
        if opnd: ops=opnd if ops is None else ops
    if ops is None: return None
    val=int(m.group(2),0); true=set()
    for case,holds in (('LT','lt'),('EQ','eq'),('GT',None)):
        x=sum(1<<b for b,k in bits.items() if k==holds)
        if (x==val)==(m.group(1)=='=='): true.add(case)
    if not true or len(true)==3: return None
    op=OUT[frozenset(true)]
    return '(%s %s %s /*x87*/)'%(ops[0],op,ops[1]), c+1+m.end()
def fix(src):
    out=[];i=0;n=0
    while True:
        j=src.find('(byte)(',i)
        if j<0: out.append(src[i:]); break
        r=rewrite_at(src,j)
        if r: out.append(src[i:j]); out.append(r[0]); i=r[1]; n+=1
        else: out.append(src[i:j+7]); i=j+7
    return ''.join(out),n
if __name__=='__main__':
    tot=0
    for f in sys.argv[1:]:
        s=open(f).read(); t,n=fix(s); tot+=n
        if n: open(f,'w').write(t)
    print('rewrote',tot)
