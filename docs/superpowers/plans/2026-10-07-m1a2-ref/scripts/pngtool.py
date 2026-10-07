# Minimal PNG (RGBA8, as stbi writes) reader/writer + compare helpers for evidence scripts.
import struct, sys, zlib
def read(p):
    d=open(p,'rb').read(); i=8; idat=b''
    while i<len(d):
        n=struct.unpack('>I',d[i:i+4])[0]; t=d[i+4:i+8]; c=d[i+8:i+8+n]; i+=12+n
        if t==b'IHDR': w,h=struct.unpack('>II',c[:8])
        if t==b'IDAT': idat+=c
    raw=zlib.decompress(idat); bpp=4; stride=w*bpp; out=bytearray(); prev=bytearray(stride); p=0
    for y in range(h):
        f=raw[p]; p+=1; line=bytearray(raw[p:p+stride]); p+=stride
        for x in range(stride):
            a=line[x-bpp] if x>=bpp else 0; b=prev[x]; c=prev[x-bpp] if x>=bpp else 0
            if f==1: line[x]=(line[x]+a)&255
            elif f==2: line[x]=(line[x]+b)&255
            elif f==3: line[x]=(line[x]+(a+b)//2)&255
            elif f==4:
                pa=abs(b-c); pb=abs(a-c); pc=abs(a+b-2*c)
                pr=a if pa<=pb and pa<=pc else (b if pb<=pc else c); line[x]=(line[x]+pr)&255
        out+=line; prev=line
    return w,h,bytes(out)
def diff(a,b):
    w,h,x=read(a); _,_,y=read(b)
    return sum(1 for i in range(0,len(x),4) if x[i:i+3]!=y[i:i+3]), w*h
if __name__=='__main__':
    if sys.argv[1]=='diff':
        d,n=diff(sys.argv[2],sys.argv[3]); print(f'{d} of {n} pixels differ')
    elif sys.argv[1]=='black':
        w,h,x=read(sys.argv[2]); print(sum(1 for i in range(0,len(x),4) if x[i:i+3]==b'\0\0\0'),'of',w*h,'pixels are pure black')
