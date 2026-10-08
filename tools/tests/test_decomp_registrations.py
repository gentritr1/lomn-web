"""M2a-0: the seeded decomp encloses every script-native registration (skipped until `make decomp` has run).

reg_sites.json and seed_addrs.txt are written by run_decomp from tools/decomp/seed_addrs.py (raw exe bytes); the
corpus is Ghidra's. These checks tie the two together: every RegisterFunction call site sits inside an exported
function, every registered fn pointer and every `.CRT` initializer is a function start, and the decompiled text
carries all 1,056 registration calls.
"""
import json
import re

# 0x5fab30 is ScOSISystem::RegisterFunction itself; its own file names it in the header line and body.
REGISTER_FUNCTION_DEF = "005fab30.c"


def _functions(gen_decomp):
    funcs = [json.loads(l) for l in (gen_decomp / "functions.jsonl").read_text().splitlines() if l]
    return sorted((int(f["addr"], 16), f["size"]) for f in funcs)


def _reg_sites(gen_decomp):
    return [{k: int(v, 16) if isinstance(v, str) else v for k, v in r.items()}
            for r in json.loads((gen_decomp / "reg_sites.json").read_text())]


def test_reg_sites_present(gen_decomp):
    sites = _reg_sites(gen_decomp)
    assert len(sites) == 1056
    seeds = [int(l, 16) for l in (gen_decomp / "seed_addrs.txt").read_text().split()]
    assert len(seeds) == 1448 and seeds == sorted(set(seeds))  # 1447 before the enclosing seeds


def test_every_site_inside_exactly_one_function(gen_decomp):
    funcs = _functions(gen_decomp)
    bad = []
    for r in _reg_sites(gen_decomp):
        s = r["site"]
        owners = [a for a, n in funcs if a <= s < a + n]
        if owners != [r["enclosing"]]:  # exactly one owner, and it is seed_addrs' enclosing prologue
            bad.append((hex(s), hex(r["enclosing"]), [hex(a) for a in owners]))
    assert not bad, bad[:20]


def test_every_fnptr_is_a_function_start(gen_decomp):
    starts = {a for a, _ in _functions(gen_decomp)}
    missing = sorted({r["fnptr"] for r in _reg_sites(gen_decomp)} - starts)
    assert not missing, [hex(a) for a in missing[:20]]


def test_every_crt_initializer_is_a_function(gen_decomp, canonical_exe):
    import seed_addrs
    crt = seed_addrs.compute(canonical_exe.read_bytes())["crt"]
    assert len(crt) == 391
    starts = {a for a, _ in _functions(gen_decomp)}
    missing = sorted(set(crt) - starts)
    assert not missing, [hex(a) for a in missing[:20]]


def test_register_function_calls_in_corpus(gen_decomp):
    # Count the name, not "name(": a call with many arguments wraps lines in the decompiled text.
    n = sum(len(re.findall(r"ScOSISystem::RegisterFunction", p.read_text()))
            for p in sorted((gen_decomp / "funcs").glob("*.c")) if p.name != REGISTER_FUNCTION_DEF)
    assert n == 1056
