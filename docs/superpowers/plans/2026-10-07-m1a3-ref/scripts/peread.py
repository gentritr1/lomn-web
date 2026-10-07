"""Read typed values at virtual addresses of a PE (data only; the exe is never run).
usage: peread.py EXE spec...   spec = <hexaddr>[:f|d|i|u|s|q|b<n>][x<count>]
  f=float32 d=float64 i=int32 u=uint32(hex) s=C string q=uint64 b<n>=n raw bytes"""
import struct, sys
def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]; opt = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    secs = []
    o = pe + 24 + opt
    for i in range(nsec):
        name, vs, va, rs, ra = struct.unpack_from('<8sIIII', d, o + 40 * i)[:5]
        secs.append((name.rstrip(b'\0').decode(), va, vs, ra, rs))
    def off(v):
        r = v - base
        for n, va, vs, ra, rs in secs:
            if va <= r < va + max(vs, rs):
                if r - va >= rs: return None  # bss
                return ra + r - va
        raise ValueError(hex(v))
    return d, off
def main():
    d, off = load(sys.argv[1])
    for spec in sys.argv[2:]:
        cnt = 1
        if 'x' in spec.split(':')[-1] and ':' in spec:
            spec, c = spec.rsplit('x', 1); cnt = int(c)
        a, _, t = spec.partition(':'); t = t or 'u'
        v = int(a, 16)
        for k in range(cnt):
            o = off(v)
            if o is None: print(hex(v), 'bss (zero-initialised)'); v += 4; continue
            if t == 'f': print(hex(v), 'f32', repr(struct.unpack_from('<f', d, o)[0]), 'bits', hex(struct.unpack_from('<I', d, o)[0])); v += 4
            elif t == 'd': print(hex(v), 'f64', repr(struct.unpack_from('<d', d, o)[0])); v += 8
            elif t == 'i': print(hex(v), 'i32', struct.unpack_from('<i', d, o)[0]); v += 4
            elif t == 'u': print(hex(v), 'u32', hex(struct.unpack_from('<I', d, o)[0])); v += 4
            elif t == 'q': print(hex(v), 'u64', hex(struct.unpack_from('<Q', d, o)[0])); v += 8
            elif t == 's': e = d.index(b'\0', o); print(hex(v), repr(d[o:e].decode('latin1'))); v += e - o + 1
            elif t.startswith('b'): n = int(t[1:]); print(hex(v), d[o:o + n].hex()); v += n
if __name__ == '__main__': main()
