"""Bounded OSI (SAGE script bytecode) reader + literal finder for M1a-3 (camera / fog literals).

Format (header + tables) follows the public SAGESharp reader (github.com/TheLegendOfMataNui/SAGESharp,
SAGESharp/OSI/OSIFile.cs + BCLInstruction.cs, MPL-2.0; read, not copied). Everything here is our own code and treats
the file as untrusted: every read is bounds-checked, table counts are capped by the remaining bytes, and bytecode is
walked linearly per subroutine with an instruction cap.

Two call finders, which answer DIFFERENT questions (do not cross-check one against the other):
  eval_calls(d, o, cls, method)   the straight-line constant evaluator - the same algorithm as the C++
                                  lomn::sage::OsiFile::eval_calls (osi.h) and the one a9_oracle.py uses: folds 0 - 40
                                  to -40, so it reports the values the game passes.  CLI: --eval CLASS METHOD
  find_plain_push_calls(d, o, wanted)  a whole-file SURVEY of where a function is called: decodes only the argc
                                  instructions right before each call, and only when they are plain pushes; a computed
                                  argument (vllg's SetFog start "0 - 40") reads as None and the slots shift (so its fog
                                  colour prints as 0).  CLI: --plain-calls class.func ...

Usage: osi.py Base.osi --eval lego_area_vllg initfull                                  (JSON to stdout)
       osi.py Base.osi --plain-calls gcareadirector.setfog gcgraphicsoptions.setdrawdistance ...
       osi.py Base.osi --summary
"""
import json, struct, sys

MAX_INSNS_PER_SUB = 1 << 15  # = the C++ kMaxOsiInstructions (measured maximum 1,707)
# opcode -> operand struct format (little-endian); 'T' = 3-byte signed int.
OPS = {0x04: 'HH', 0x05: 'HH', 0x06: 'HH', 0x10: 'H', 0x11: 'H', 0x12: 'H', 0x13: 'H', 0x14: 'b', 0x15: 'H', 0x16: 'H',
       0x17: 'H', 0x21: 'H', 0x22: 'H', 0x23: 'b', 0x24: 'H', 0x25: 'H', 0x31: 'b', 0x33: 'b', 0x34: 'b', 0x40: 'i',
       0x41: 'T', 0x42: 'h', 0x43: 'b', 0x44: 'f', 0x46: 'H', 0x48: 'I', 0x49: 'H', 0x50: 'b', 0x51: 'b', 0x53: 'h',
       0x54: 'h', 0x80: 'HH', 0x81: 'HH', 0x82: 'HHb', 0x83: 'Hb', 0x84: 'ib'}
NOARG = {0x00, 0x02, 0x03, 0x18, 0x19, 0x1A, 0x30, 0x32, 0x35, 0x45, 0x47, 0x52, 0x60, 0x61, 0x62, 0x63, 0x64, 0x6A,
         0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x7A, 0x7B, 0x90, 0x91,
         0x92, 0x93, 0x94, 0x95, 0x96, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xB0, 0xB1, 0xB2, 0xB8, 0xB9,
         0xBA, 0xBB, 0xBC, 0xBD, 0xBF, 0xFF}


class OsiError(Exception):
    pass


class R:
    def __init__(self, d, p=0):
        self.d, self.p = d, p

    def take(self, n):
        if n < 0 or self.p + n > len(self.d):
            raise OsiError('read past end at %d (+%d)' % (self.p, n))
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def u8(self): return self.take(1)[0]
    def u16(self): return struct.unpack('<H', self.take(2))[0]
    def u32(self): return struct.unpack('<I', self.take(4))[0]

    def count(self, min_entry):
        n = self.u16()
        if n * min_entry > len(self.d) - self.p:
            raise OsiError('count %d exceeds remaining bytes' % n)
        return n

    def strings(self):
        out = []
        for _ in range(self.count(2)):
            n = self.u8()
            out.append(self.take(n).decode('latin1'))
            self.take(1)  # terminator
        return out


def parse(d):
    r = R(d)
    if r.take(4) != b'OSI\0':
        raise OsiError('bad magic')
    major, minor = r.u16(), r.u16()
    if major != 4:
        raise OsiError('unsupported version %d.%d (LOMN uses 4.1)' % (major, minor))
    o = {'version': [major, minor]}
    o['strings'] = r.strings()
    o['globals'] = r.strings()
    funcs = []
    for _ in range(r.count(7)):
        n = r.u8()
        name = r.take(n).decode('latin1')
        off, npar = r.u32(), r.u16()
        funcs.append((name, off, npar))
    classes = []
    for _ in range(r.count(2)):
        props = [r.u16() for _ in range(r.u8())]
        meths = []
        for _ in range(r.u8()):
            meths.append((r.u16(), r.u32()))
        classes.append([None, props, meths])
    for c in classes:
        n = r.u8()
        c[0] = r.take(n).decode('latin1')
        r.take(1)
    o['symbols'] = r.strings()
    o['sources'] = r.strings()
    o['functions'] = funcs
    o['classes'] = classes
    o['tables_end'] = r.p
    return o


def disasm(d, start):
    """Linear walk of one subroutine (SAGESharp's rule: stop at a Return past the last forward branch target)."""
    r = R(d, start)
    out, last_target = [], 0
    for _ in range(MAX_INSNS_PER_SUB):
        at = r.p
        op = r.u8()
        if op in OPS:
            args = []
            for f in OPS[op]:
                if f == 'T':
                    b = r.take(3)
                    v = b[0] | b[1] << 8 | b[2] << 16
                    args.append(v - (1 << 24) if v & 0x800000 else v)
                else:
                    args.append(struct.unpack('<' + f, r.take(struct.calcsize(f)))[0])
        elif op in NOARG:
            args = []
        else:
            raise OsiError('unknown opcode 0x%02x at %d' % (op, at))
        out.append((at, op, args))
        if op in (0x53, 0x54):
            last_target = max(last_target, r.p + args[0])
        elif op == 0x52 and r.p > last_target:
            return out
    raise OsiError('subroutine at %d exceeds the instruction cap' % start)


def literal(o, ins):
    at, op, a = ins
    if op in (0x40, 0x41, 0x42, 0x43): return a[0]
    if op == 0x44: return a[0]
    if op == 0x45: return 0
    if op == 0x46: return o['strings'][a[0]] if a[0] < len(o['strings']) else None
    if op == 0x48: return '0x%08x' % a[0]
    return None


def subroutines(o):
    for name, off, _ in o['functions']:
        yield name, off
    seen = set()
    for cname, _, meths in o['classes']:
        for sym, off in meths:
            if off in seen:
                continue
            seen.add(off)
            mname = o['symbols'][sym] if sym < len(o['symbols']) else '?'
            yield cname + '.' + mname, off


def find_plain_push_calls(d, o, wanted):
    """Every CallGameFunction(class, func, argc) whose 'class.func' (lower case) is in `wanted`, with the argc preceding
    instructions decoded as literals where they are plain pushes (None otherwise)."""
    hits = []
    for sub, off in subroutines(o):
        ins = disasm(d, off)
        for i, (at, op, a) in enumerate(ins):
            if op != 0x82:
                continue
            cls = o['strings'][a[0]] if a[0] < len(o['strings']) else '?'
            fn = o['strings'][a[1]] if a[1] < len(o['strings']) else '?'
            key = (cls + '.' + fn).lower()
            if key not in wanted:
                continue
            argc = a[2]
            prev = ins[max(0, i - argc):i]
            args = [literal(o, p) for p in prev] if len(prev) == argc else None
            hits.append({'call': key, 'in': sub, 'offset': at, 'argc': argc, 'args': args})
    return hits


def eval_calls(d, o, cls, method):
    """CallGameFunction calls in cls.method with their literal arguments, evaluating straight-line constant code:
    pushes, + - * / on numbers, Pop/PopN; every other opcode, branch or branch target forgets the stack."""
    off = None
    for name, meths in ((c[0], c[2]) for c in o['classes']):
        if name.lower() == cls.lower():
            for sym, mo in meths:
                if o['symbols'][sym].lower() == method.lower():
                    off = mo
    if off is None:
        return None
    ins = disasm(d, off)
    targets = set()
    for i, (at, op, a) in enumerate(ins):
        if op in (0x53, 0x54):
            nxt = ins[i + 1][0] if i + 1 < len(ins) else at + 3
            targets.add(nxt + a[0])
    st, calls = [], []
    for at, op, a in ins:
        if at in targets:
            st = []
        if op in (0x40, 0x41, 0x42, 0x43, 0x48):
            st.append(a[0])
        elif op == 0x45:
            st.append(0)
        elif op == 0x44:
            st.append(a[0])
        elif op == 0x46:
            st.append(o['strings'][a[0]])
        elif op == 0x47:
            st.append(None)
        elif op in (0x00, 0x04, 0x05, 0x06):
            pass
        elif op == 0x30:
            st and st.pop()
        elif op == 0x31:
            for _ in range(a[0]):
                st and st.pop()
        elif op in (0x70, 0x71, 0x72, 0x73):
            y = st.pop() if st else None
            x = st.pop() if st else None
            if isinstance(x, (int, float)) and isinstance(y, (int, float)) and not isinstance(x, bool):
                r = {0x70: lambda: x + y, 0x71: lambda: x - y, 0x72: lambda: x * y, 0x73: lambda: x / y if y else 0}[op]()
                st.append(float(r) if isinstance(x, float) or isinstance(y, float) or op == 0x73 else int(r))
            else:
                st.append(None)
        elif op == 0x82:
            argc = a[2]
            args = [st.pop() if st else None for _ in range(argc)][::-1]
            calls.append(((o['strings'][a[0]] + '.' + o['strings'][a[1]]).lower(), args))
            st.append(None)
        else:
            st = []
    return calls


def main():
    d = open(sys.argv[1], 'rb').read()
    o = parse(d)
    if '--summary' in sys.argv:
        n = sum(len(disasm(d, off)) for _, off in subroutines(o))
        print(json.dumps({'version': o['version'], 'strings': len(o['strings']), 'globals': len(o['globals']),
                          'functions': len(o['functions']), 'classes': len(o['classes']), 'symbols': len(o['symbols']),
                          'sources': len(o['sources']), 'tables_end': o['tables_end'], 'instructions': n}))
        return
    if '--eval' in sys.argv:
        i = sys.argv.index('--eval')
        calls = eval_calls(d, o, sys.argv[i + 1], sys.argv[i + 2])
        print(json.dumps(None if calls is None else [{'call': k, 'args': a} for k, a in calls], indent=1))
        return
    i = sys.argv.index('--plain-calls')
    wanted = set(w.lower() for w in sys.argv[i + 1:])
    print(json.dumps(find_plain_push_calls(d, o, wanted), indent=1))


if __name__ == '__main__':
    main()
