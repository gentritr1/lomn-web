"""Measures, on a real Base.osi, every quantity the C++ OSI reader caps (M1a-3 REF3 revision 2).
Every cap in engine/include/lomn/sage/osi.h cites the value this prints. Uses osi.py (the independent Python reader).
Usage: osi_maxima.py Base.osi   (JSON to stdout)"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import osi

d = open(sys.argv[1], 'rb').read()
o = osi.parse(d)
subs = list(osi.subroutines(o))
m = dict(file_bytes=len(d), strings=len(o['strings']), longest_string=max(map(len, o['strings'])),
         string_table_bytes=sum(map(len, o['strings'])), functions=len(o['functions']), classes=len(o['classes']),
         methods_total=sum(len(c[2]) for c in o['classes']), unique_subroutines=len(subs),
         function_offsets_unique=len(set(off for _, off, _ in o['functions'])))
ins_max = ins_sum = calls_max = calls_sum = argc_max = depth_max = tgt_max = 0
argbytes_call = argbytes_sub = args_sub = 0
where = {}
for name, off in subs:
    ins = osi.disasm(d, off)
    ins_sum += len(ins)
    if len(ins) > ins_max: ins_max, where['ins'] = len(ins), name
    tg = set()
    for i, (at, op, a) in enumerate(ins):
        if op in (0x53, 0x54):
            tg.add((ins[i + 1][0] if i + 1 < len(ins) else at + 3) + a[0])
    tgt_max = max(tgt_max, len(tg))
    # the evaluator of osi.eval_calls, instrumented: stack depth, calls, args, string bytes copied into calls
    st, nc, nargs, sb = [], 0, 0, 0
    for at, op, a in ins:
        if at in tg: st = []
        if op in (0x40, 0x41, 0x42, 0x43, 0x48, 0x45, 0x44, 0x47): st.append(0)
        elif op == 0x46: st.append(o['strings'][a[0]] if a[0] < len(o['strings']) else None)
        elif op in (0x00, 0x04, 0x05, 0x06): pass
        elif op == 0x30: st and st.pop()
        elif op == 0x31:
            for _ in range(a[0]): st and st.pop()
        elif op in (0x70, 0x71, 0x72, 0x73):
            st and st.pop(); st and st.pop(); st.append(0)
        elif op == 0x82:
            argc = max(0, a[2]); argc_max = max(argc_max, argc)
            args = [st.pop() if st else None for _ in range(argc)]
            b = sum(len(x) for x in args if isinstance(x, str))
            argbytes_call = max(argbytes_call, b); sb += b
            nc += 1; nargs += argc; st.append(None)
        else: st = []
        depth_max = max(depth_max, len(st))
    calls_sum += nc
    if nc > calls_max: calls_max, where['calls'] = nc, name
    args_sub = max(args_sub, nargs); argbytes_sub = max(argbytes_sub, sb)
m.update(insns_per_sub_max=ins_max, insns_total=ins_sum, branch_targets_per_sub_max=tgt_max, calls_per_sub_max=calls_max,
         calls_total=calls_sum, argc_max=argc_max, args_per_sub_max=args_sub, string_arg_bytes_per_call_max=argbytes_call,
         string_arg_bytes_per_sub_max=argbytes_sub, eval_stack_depth_max=depth_max, where=where)
print(json.dumps(m, indent=1))
