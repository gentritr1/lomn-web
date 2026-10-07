"""Writes crafted hostile Base.osi files for END-TO-END time/memory probes of lomn_view (M1a-3 REF3 revision 2; the
in-process probes are engine/tests/probe_osi.cpp). Put the output at <install>/data/Base.osi of a written synthetic install
(lomn_view --synthetic 2 --write-install DIR) and run lomn_view on levt/stat under /usr/bin/time.
Usage: osi_craft.py OUT KIND   KIND in: calls (Fable's call flood), stack (string-push flood), funcs (65535 functions on
one long subroutine: check_all), nops (one maximal subroutine)."""
import struct, sys


def build(strings, cls, method_code, functions=()):
    b = bytearray(b'OSI\0' + struct.pack('<HH', 4, 1))
    def table(v):
        b.extend(struct.pack('<H', len(v)))
        for s in v:
            b.append(len(s)); b.extend(s.encode()); b.append(0)
    table(strings); table([])
    b.extend(struct.pack('<H', len(functions)))
    fpatch = []
    for name in functions:
        b.append(len(name)); b.extend(name.encode()); fpatch.append(len(b)); b.extend(b'\0\0\0\0\0\0')
    b.extend(struct.pack('<H', 1)); b.append(0); b.append(1); b.extend(struct.pack('<H', 0))
    mpatch = len(b); b.extend(b'\0\0\0\0')
    b.append(len(cls)); b.extend(cls.encode()); b.append(0)
    table(['initfull']); table([])
    code = len(b)
    struct.pack_into('<I', b, mpatch, code)
    for p in fpatch: struct.pack_into('<I', b, p, code)
    b.extend(method_code)
    return bytes(b)


def main():
    out, kind = sys.argv[1], sys.argv[2]
    s = ['gcareadirector', 'setfog', 'x' * 255]
    if kind == 'calls':      # CallGameFunction argc 127, 100,000 times (600 KB): one OsiCall of 127 args each
        code = bytes([0x82, 0, 0, 1, 0, 127]) * 100000 + b'\x52'
        d = build(s, 'lego_area_statt', code)
    elif kind == 'stack':    # PushString (255 chars) 200,000 times: the literal stack copies every string
        code = bytes([0x46, 2, 0]) * 200000 + b'\x52'
        d = build(s, 'lego_area_statt', code)
    elif kind == 'funcs':    # 65,535 functions sharing one 300,000-instruction subroutine: check_all walks it per function
        code = b'\x00' * 300000 + b'\x52'
        d = build(s, 'lego_area_statt', code, functions=['f'] * 65535)
    elif kind == 'nops':     # one subroutine at the per-subroutine cap
        code = b'\x00' * ((1 << 20) - 1) + b'\x52'
        d = build(s, 'lego_area_statt', code)
    else:
        sys.exit('unknown kind')
    open(out, 'wb').write(d)


if __name__ == '__main__':
    main()
