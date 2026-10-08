"""M2a-0 T3: the native registry (gen/osi/natives.json) and the variant-type table (docs/osi/types.md).

Every row is one `ScOSISystem::RegisterFunction` call (0x5fab30) in the decompiled corpus, parsed by STATEMENT (the calls
wrap lines, and Ghidra hoists most arguments into locals, so a statement's arguments are resolved through the
assignments before it):

  call:   RegisterFunction(this, class&, func&, fnptr, MAX, MIN, type0..type9)         [thiscall; 0x5fab30, probe 1]
  class/func  `_ScBaseString::_ScBaseString(&local, s_Name_<addr8> | &DAT_<addr8>)` -> the C string at <addr8>, READ
              FROM THE EXE (Ghidra's `s_` labels are truncated; the address suffix is not)
  fnptr       `FUN_<addr8>` or a named function (functions.jsonl); MAX, MIN: char literals; types: 16-bit literals,
              `theOSIType<Class>` globals, or `GetVariantType<T>::GetIt()` results (table read from the GetIt bodies).

Execution model replayed here (cited in docs/osi/types.md): `__RunInit` (0x65f530) calls the 391 `.CRT` initializers in
ascending order; a game type's id is the `RegisterType` (0x5fae00) counter at the first call that finds its global
`theOSIType<Class>` still zero (counter starts at 0x20, ScOSISystem ctor 0x5fa750); a duplicate key never replaces the
first (`insert_one` 0x5fb290 returns the existing node), so the FIRST registration in execution order wins.

INDEPENDENT CROSS-CHECK (reg_sites.json, raw exe push-walking, vs. this module's reading of the decomp): rows are matched
to reg_sites by enclosing function + statement order (`cross_check`); fnptr, enclosing function and call-site count must
agree for all 1,056.

Usage: registry.py --exe EXE --osi BASE.OSI --corpus gen/decomp --out-json gen/osi/natives.json --out-types docs/osi/types.md
"""
from __future__ import annotations

import argparse
import collections
import importlib.util
import json
import re
import struct
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(TOOLS), str(TOOLS / "decomp")]
import seed_addrs  # noqa: E402  (tools/decomp)

REGISTER_FUNCTION_DEF = "005fab30.c"   # ScOSISystem::RegisterFunction itself: its file names the function in its header
FIRST_TYPE_ID = 0x20                   # ScOSISystem::ScOSISystem (0x5fa750) stores 0x20 at this+0; RegisterType returns+increments
RUN_INIT_START = 0x76F004              # __RunInit (0x65f530): `for (p = &PTR_FUN_0076f004; *p != 0; p++) (*p)()`
NROW_TYPES = 10
# __tree<...RegisteredFunction*>::insert_one (0x5fb290) stores one of these bytes beside the node it returns: [0x74d668] after
# inserting, [0x74d66c] when the key already existed (asm 0x5fb32b and 0x5fb349).
INSERT_FLAG_TRUE, INSERT_FLAG_FALSE = 0x74D668, 0x74D66C

# Builtin variant types. Sources: docs/osi/opcodes.md "Variant types seen in the handlers" (ids), and the bodies of the
# GetVariantType<T>::GetIt specialisations (which C++ type maps to which id, see getit_table()).
BUILTIN = {
    0x0F: "nothing",
    0x10: "int",
    0x11: "string-id",
    0x12: "float",
    0x15: "colour",
    0x16: "string",
    0x200: "byref",
}
# 0 is what GetVariantType<char*>::GetIt (0x54a550) returns. 0x18/0x21/0x22 are listed in the brief as possible
# literals; unknown literals are reported as "unknown:0x.." and counted.


class RegistryError(Exception):
    pass


# --------------------------------------------------------------------------------------------------------------- image
class Image:
    """Read C strings out of the exe by virtual address (data only)."""

    def __init__(self, exe: bytes):
        self.exe = exe
        self.base, self.secs = seed_addrs.sections(exe)

    def byte(self, va: int) -> int:
        for s in self.secs:
            n = min(s["vsz"], s["rsz"])
            if s["va"] <= va < s["va"] + n:
                return self.exe[s["roff"] + (va - s["va"])]
        raise RegistryError("address %#x is not in a file-backed section" % va)

    def cstr(self, va: int) -> str:
        for s in self.secs:
            n = min(s["vsz"], s["rsz"])
            if s["va"] <= va < s["va"] + n:
                o = s["roff"] + (va - s["va"])
                end = self.exe.index(b"\0", o, s["roff"] + n)
                return self.exe[o:end].decode("latin-1")
        raise RegistryError("address %#x is not in a file-backed section" % va)


# ------------------------------------------------------------------------------------------------------ text handling
def strip_comments(t: str) -> str:
    t = re.sub(r"/\*.*?\*/", " ", t, flags=re.S)
    return re.sub(r"(?m)^\s*//.*$", "", t)


def statements(text: str) -> list[str]:
    """Split decompiled C into statements at top-level `;`, `{`, `}` (outside parentheses and quotes)."""
    out, depth, start, i, q = [], 0, 0, 0, None
    n = len(text)
    while i < n:
        c = text[i]
        if q:
            if c == "\\":
                i += 1
            elif c == q:
                q = None
        elif c in "\"'":
            q = c
        elif c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif depth == 0 and c in ";{}":
            s = " ".join(text[start:i].split())
            if s:
                out.append(s)
            start = i + 1
        i += 1
    tail = " ".join(text[start:].split())
    if tail:
        out.append(tail)
    return out


def split_args(s: str) -> list[str]:
    out, depth, cur, q, i = [], 0, [], None, 0
    while i < len(s):
        c = s[i]
        if q:
            cur.append(c)
            if c == "\\" and i + 1 < len(s):
                i += 1
                cur.append(s[i])
            elif c == q:
                q = None
        elif c in "\"'":
            q = c
            cur.append(c)
        elif c in "([<":
            depth += 1
            cur.append(c)
        elif c in ")]>":
            depth -= 1
            cur.append(c)
        elif c == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    out.append("".join(cur).strip())
    return out


_CHAR_ESC = {"0": 0, "a": 7, "b": 8, "t": 9, "n": 10, "v": 11, "f": 12, "r": 13, "\\": 92, "'": 39, '"': 34}


def parse_int(tok: str) -> int | None:
    """Integer literal as Ghidra prints it: 0x1f, 12, '\\x05', '\\0', '\\n', 'A'. None when `tok` is not a literal."""
    tok = tok.strip()
    if re.fullmatch(r"0x[0-9a-fA-F]+|[0-9]+", tok):
        return int(tok, 0)
    m = re.fullmatch(r"'(?:\\x([0-9a-fA-F]{1,2})|\\([0abtnvfr\\'\"])|(.))'", tok)
    if m:
        if m.group(1) is not None:
            return int(m.group(1), 16)
        if m.group(2) is not None:
            return _CHAR_ESC[m.group(2)]
        return ord(m.group(3))
    return None


# ------------------------------------------------------------------------------------------------------ the GetIt table
def getit_table(corpus: Path) -> dict[str, dict]:
    """`GetVariantType<T>::GetIt` specialisations, read from their decompiled bodies:
    {T: {"lit": n, "addr": a}} for `return n;` and {T: {"lazy": global, "regname": str, "addr": a}} for the lazy form
    `if (theOSITypeX == 0) { ...RegisterType(<name>)... } return theOSITypeX;`."""
    table = {}
    for p in sorted((corpus / "funcs").glob("*.c")):
        with open(p) as f:
            first = f.readline()
        m = re.match(r"// 0x([0-9a-f]+) GetVariantType<(.+)>::GetIt\s*$", first)
        if not m:
            continue
        addr, tname = int(m.group(1), 16), m.group(2)
        body = strip_comments(p.read_text())
        lit = re.search(r"return (0x[0-9a-f]+|\d+);", body)
        lazy = re.search(r"if \((theOSIType\w+) == 0\)", body)
        if lazy:
            reg = re.search(r"_ScBaseString::_ScBaseString\(&\w+,\s*(s_\w+_([0-9a-f]{8})|&DAT_([0-9a-f]{8}))\)", body)
            table[tname] = {"lazy": lazy.group(1), "addr": addr, "regaddr": int(reg.group(2) or reg.group(3), 16)}
        elif lit:
            table[tname] = {"lit": int(lit.group(1), 0), "addr": addr}
        else:
            raise RegistryError("GetIt body not understood: %s" % p.name)
    return table


# --------------------------------------------------------------------------------------------------------- the replay
class Replay:
    """The state RegisterType leaves behind, replayed in execution order."""

    def __init__(self, image: Image):
        self.image = image
        self.order: list[dict] = []            # first-use order of game types
        self.by_global: dict[str, dict] = {}
        self.guard_events = 0                  # `theOSITypeX = RegisterType(..)` statements seen in .CRT initializers
        self.getit_events = 0                  # lazy-type GetVariantType<T>::GetIt() calls seen

    def touch(self, gvar: str, regname: str, init: int, crt_index, via: str) -> None:
        if gvar in self.by_global:
            return
        cls = gvar[len("theOSIType"):]
        e = {"class": cls, "global": gvar, "regname": regname, "init": init, "crt_index": crt_index, "via": via,
             "rank": len(self.order)}
        self.order.append(e)
        self.by_global[gvar] = e


_STR_ARG = re.compile(r"(?:s_\w*_|&DAT_|DAT_)([0-9a-f]{8})(?: \+ (\d+))?$")


class FunctionParser:
    """Walks one function's statements in text order, resolving RegisterFunction calls and replaying type registration."""

    def __init__(self, image: Image, getit: dict, fn_by_name: dict[str, int], replay: Replay):
        self.image, self.getit, self.fn_by_name, self.replay = image, getit, fn_by_name, replay

    def run(self, init: int, crt_index, text: str) -> list[dict]:
        stmts = statements(strip_comments(text))
        strs: dict[str, str] = {}     # local -> string
        env: dict[str, object] = {}   # variable -> evaluated rhs
        guards: set[str] = set()
        rows = []
        for st in stmts:
            m = re.fullmatch(r"_ScBaseString::_ScBaseString\(&(\w+), ?(.+)\)", st)
            if m:
                a = _STR_ARG.search(m.group(2))
                if not a:
                    raise RegistryError("unresolved string argument %r in %#x" % (m.group(2), init))
                addr = int(a.group(1), 16) + int(a.group(2) or 0)
                strs[m.group(1)] = (self.image.cstr(addr), addr)
                continue
            m = re.fullmatch(r"if \((theOSIType\w+) == 0\)", st)
            if m:
                guards.add(m.group(1))
                continue
            m = re.fullmatch(r"(theOSIType\w+) = ScOSISystem::RegisterType\(.*,&(\w+)\)", st)
            if m:
                g, loc = m.group(1), m.group(2)
                if g not in guards:
                    raise RegistryError("unguarded RegisterType in %#x: %r" % (init, st))
                guards.discard(g)
                self.replay.guard_events += 1
                self.replay.touch(g, strs[loc][0], init, crt_index, "RegisterType guard")
                continue
            m = re.fullmatch(r"ScOSISystem::RegisterFunction ?\((.*)\)", st)
            if m:
                rows.append(self.row(init, crt_index, split_args(m.group(1)), strs, env))
                continue
            m = re.fullmatch(r"(\w+) = (.+)", st)
            if m and not m.group(1).startswith("theOSIType"):
                env[m.group(1)] = self.evaluate(m.group(2), init, crt_index)
        n = len(re.findall(r"ScOSISystem::RegisterFunction", strip_comments(text)))
        if n != len(rows):
            raise RegistryError("%#x: %d RegisterFunction mentions but %d parsed calls" % (init, n, len(rows)))
        return rows

    def evaluate(self, rhs: str, init: int, crt_index):
        rhs = rhs.strip()
        v = parse_int(rhs)
        if v is not None:
            return v
        m = re.fullmatch(r"GetVariantType<(.+)>::GetIt\(\)", rhs)
        if m:
            g = self.getit.get(m.group(1))
            if g is None:
                raise RegistryError("no GetIt body for GetVariantType<%s> (in %#x)" % (m.group(1), init))
            if "lit" in g:
                return g["lit"]
            self.replay.getit_events += 1
            self.replay.touch(g["lazy"], self.image.cstr(g["regaddr"]), init, crt_index,
                              "GetVariantType<%s>::GetIt %#x" % (m.group(1), g["addr"]))
            return ("game", g["lazy"][len("theOSIType"):])
        m = re.fullmatch(r"(theOSIType\w+)", rhs)
        if m:
            return ("game", m.group(1)[len("theOSIType"):])
        fn = self.fn_addr(rhs)
        if fn is not None:
            return ("fn", fn)
        m = re.fullmatch(r"&(\w+)", rhs)
        if m:
            return ("ref", m.group(1))
        return ("opaque", rhs)

    def fn_addr(self, tok: str):
        m = re.fullmatch(r"FUN_([0-9a-f]{8})", tok)
        if m:
            return int(m.group(1), 16)
        return self.fn_by_name.get(tok)

    def name(self, arg: str, strs: dict, env: dict) -> tuple[str, int]:
        arg = arg.strip()
        if arg in env:
            v = env[arg]
            if isinstance(v, tuple) and v[0] == "ref":
                return strs[v[1]]
            raise RegistryError("name argument %r is not a reference to a string local" % arg)
        m = re.fullmatch(r"&(\w+)", arg)
        if m and m.group(1) in strs:
            return strs[m.group(1)]
        raise RegistryError("unresolved name argument %r" % arg)

    def value(self, arg: str, env: dict, init: int):
        arg = arg.strip()
        v = parse_int(arg)
        if v is not None:
            return v
        if arg in env:
            return env[arg]
        m = re.fullmatch(r"(theOSIType\w+)", arg)
        if m:
            if m.group(1) not in self.replay.by_global:
                raise RegistryError("%s used as a type before any registration (%#x)" % (m.group(1), init))
            return ("game", m.group(1)[len("theOSIType"):])
        fn = self.fn_addr(arg)
        if fn is not None:
            return ("fn", fn)
        raise RegistryError("unresolved argument %r in %#x" % (arg, init))

    def row(self, init: int, crt_index, args: list[str], strs: dict, env: dict) -> dict:
        if len(args) != 16:
            raise RegistryError("RegisterFunction with %d arguments in %#x" % (len(args), init))
        fn = self.value(args[3], env, init)
        mx, mn = self.value(args[4], env, init), self.value(args[5], env, init)
        if not (isinstance(fn, tuple) and fn[0] == "fn" and isinstance(mx, int) and isinstance(mn, int)):
            raise RegistryError("fnptr/max/min not resolved in %#x: %r" % (init, args[3:6]))
        types = [self.value(a, env, init) for a in args[6:16]]
        for t in types:
            if not (isinstance(t, int) or (isinstance(t, tuple) and t[0] == "game")):
                raise RegistryError("type argument not resolved in %#x: %r" % (init, t))
        (cls, cls_addr), (name, name_addr) = self.name(args[1], strs, env), self.name(args[2], strs, env)
        return {"cls": cls, "name": name, "cls_addr": cls_addr, "name_addr": name_addr, "body": fn[1],
                "max": mx, "min": mn, "raw_types": types, "init": init, "crt_index": crt_index}


# ------------------------------------------------------------------------------------------------ type normalisation
UNKNOWN_PREFIX = "unknown:"


def type_name(t, max_args: int, index: int) -> str | None:
    """Row `types` entry: builtin name, "type:<Class>", None for an unused slot (0x0f at index >= max), or
    "unknown:0x.." for a literal with no builtin meaning."""
    if isinstance(t, tuple):
        return "type:" + t[1]
    if t == 0x0F:
        return None if index >= max_args else BUILTIN[0x0F]
    if t in BUILTIN:
        return BUILTIN[t]
    return "%s0x%x" % (UNKNOWN_PREFIX, t)


def hash_token(s: str) -> int:
    """ScOSISystem::GetToken (0x5faae0): h = h * 0x83 + toupper(c) over the bytes, 32-bit."""
    h = 0
    for b in s.encode("latin-1"):
        h = (h * 0x83 + (b - 0x20 if 0x60 < b < 0x7B else b)) & 0xFFFFFFFF
    return h


def dual_token(cls: str, name: str) -> tuple[int, int]:
    """The map key of RegisterFunction / BCLCallGameFunction: ScOSIDualToken(token(function), token(class))."""
    return hash_token(name), hash_token(cls)


# ----------------------------------------------------------------------------------------------------------- the build
def load_functions(corpus: Path) -> list[dict]:
    return [json.loads(line) for line in open(corpus / "functions.jsonl") if line.strip()]


def parse_corpus(corpus: Path, image: Image, crt: list[int]):
    """Parse every .CRT initializer in ascending order (type registration replay) and the registrars outside .CRT.
    Returns (rows in execution order, Replay)."""
    funcs = load_functions(corpus)
    by_name: dict[str, list[int]] = collections.defaultdict(list)
    for f in funcs:
        by_name[f["name"]].append(int(f["addr"], 16))
    fn_by_name = {n: a[0] for n, a in by_name.items() if len(a) == 1}
    replay = Replay(image)
    parser = FunctionParser(image, getit_table(corpus), fn_by_name, replay)
    wanted = {int(s["enclosing"], 16) for s in json.loads((corpus / "reg_sites.json").read_text())}
    rows = []
    for i, a in enumerate(crt):
        p = corpus / "funcs" / ("%08x.c" % a)
        if not p.exists():
            raise RegistryError(".CRT initializer %#x has no decompiled file" % a)
        got = parser.run(a, i, p.read_text())
        for r in got:
            r["phase"] = "crt"
        rows.extend(got)
    for a in sorted(wanted - set(crt)):   # registrars outside .CRT: ScInitFunc callbacks, run after every initializer
        got = parser.run(a, None, (corpus / "funcs" / ("%08x.c" % a)).read_text())
        for r in got:
            r["phase"] = "scinit"
        rows.extend(got)
    return rows, replay


def createvariant_types(corpus: Path, image: Image) -> list[dict]:
    """Classes whose id CreateVariant(<Class>*) can assign at run time (the lazy `if (theOSITypeX == 0)` in each of its
    overloads): they are registered the first time a native returns such an object, if nothing registered them before."""
    out = []
    for f in load_functions(corpus):
        if f["name"] != "CreateVariant":
            continue
        t = strip_comments((corpus / "funcs" / ("%08x.c" % int(f["addr"], 16))).read_text())
        for m in re.finditer(r"if \((theOSIType\w+) == 0\)", t):
            reg = _STR_ARG.search(re.search(r"_ScBaseString::_ScBaseString\(&\w+, ?([^)]*)\)", t[m.end():]).group(1))
            addr = int(reg.group(1), 16) + int(reg.group(2) or 0)
            out.append({"class": m.group(1)[len("theOSIType"):], "global": m.group(1), "addr": int(f["addr"], 16),
                        "regname": image.cstr(addr)})
    return out


def crt_layout(exe_bytes: bytes) -> list[int]:
    """The raw dwords of the `.CRT` section as `__RunInit` (0x65f530) walks them: it starts at 0x76f004 and stops at the
    first zero dword. Returns the pointers it would call; raises unless that is exactly the pointers seed_addrs found
    (a zero dword inside the table would silently drop every later initializer)."""
    base, secs = seed_addrs.sections(exe_bytes)
    sec = seed_addrs._section(secs, ".CRT")
    if sec["va"] + 4 != RUN_INIT_START:
        raise RegistryError(".CRT starts at %#x, __RunInit starts at %#x" % (sec["va"], RUN_INIT_START))
    raw = seed_addrs._mapped(exe_bytes, sec)
    dwords = struct.unpack("<%dI" % (len(raw) // 4), raw[:len(raw) & ~3])
    called = []
    for d in dwords[1:]:
        if d == 0:
            break
        called.append(d)
    if called != seed_addrs.compute(exe_bytes)["crt"]:
        raise RegistryError("__RunInit would call %d dwords, seed_addrs lists a different set" % len(called))
    return called


def type_site_accounting(exe_bytes: bytes, corpus: Path, crt: list[int]) -> dict:
    """Where the 445 `call RegisterType` sites of the exe live: inside .CRT initializers, inside GetIt bodies, inside
    CreateVariant overloads, or elsewhere."""
    import bisect
    funcs = sorted((int(f["addr"], 16), f["size"], f["name"]) for f in load_functions(corpus))
    starts = [f[0] for f in funcs]
    crt_set = set(crt)
    acc = collections.Counter()
    for s in seed_addrs.compute(exe_bytes)["type_sites"]:
        i = bisect.bisect_right(starts, s) - 1
        a, n, name = funcs[i]
        if not (a <= s < a + n):
            acc["unowned"] += 1
        elif a in crt_set:
            acc["crt_initializer"] += 1
        elif re.fullmatch(r"GetVariantType<.+>::GetIt", name):
            acc["getit"] += 1
        elif name == "CreateVariant":
            acc["createvariant"] += 1
        else:
            acc["other:" + name] += 1
    return dict(acc)


def cross_check(rows: list[dict], corpus: Path) -> dict:
    """Match each row to its reg_sites.json entry by (enclosing function, statement order). reg_sites comes from raw-byte
    push-walking in the exe (tools/decomp/seed_addrs.py), the rows from Ghidra's decomp, so they are two independent
    readers of the same 1,056 calls. Returns the matched entry per row and a list of disagreements."""
    sites = json.loads((corpus / "reg_sites.json").read_text())
    by_enc: dict[int, list[dict]] = collections.defaultdict(list)
    for s in sites:
        by_enc[int(s["enclosing"], 16)].append(s)
    for v in by_enc.values():
        v.sort(key=lambda s: int(s["site"], 16))
    seen: dict[int, int] = collections.defaultdict(int)
    matched, bad = [], []
    for i, r in enumerate(rows):
        lst = by_enc.get(r["init"], [])
        k = seen[r["init"]]
        seen[r["init"]] += 1
        if k >= len(lst):
            bad.append((i, "more decomp calls than reg_sites in %#x" % r["init"]))
            matched.append(None)
            continue
        s = lst[k]
        matched.append(s)
        if int(s["fnptr"], 16) != r["body"]:
            bad.append((i, "fnptr: decomp %#x vs reg_sites %s (site %s)" % (r["body"], s["fnptr"], s["site"])))
    for enc, lst in by_enc.items():
        if seen[enc] != len(lst):
            bad.append((None, "%#x: decomp has %d calls, reg_sites has %d" % (enc, seen[enc], len(lst))))
    return {"matched": matched, "bad": bad}


def type_order_from_asm(exe_bytes: bytes, corpus: Path, crt: list[int], getit: dict) -> list[str]:
    """First-use order of the game types re-derived from raw bytes only (no decompiled text): inside each .CRT initializer,
    ascending by address, every `call RegisterType` (always followed by `mov [theOSIType<Class>], ax` = 66 A3 imm32; the
    global's name comes from symbols.txt) and every call of a lazy `GetVariantType<Class*>::GetIt`; initializers in `.CRT`
    order; a class counts at its first event. The result must equal the decomp-derived replay."""
    import bisect
    base, secs = seed_addrs.sections(exe_bytes)
    text = [s for s in secs if s["name"] == ".text"][0]
    tb, tva = exe_bytes[text["roff"]:text["roff"] + text["vsz"]], text["va"]
    glob = {}
    for line in (corpus / "symbols.txt").read_text().splitlines():
        a, _, n = line.partition(" ")
        m = re.fullmatch(r"\?theOSIType(\w+?)@@3GA", n.strip())
        if m:
            glob[int(a, 16)] = m.group(1)
    lazy = {g["addr"]: g["lazy"][len("theOSIType"):] for g in getit.values() if "lazy" in g}
    funcs = sorted((int(f["addr"], 16), f["size"]) for f in load_functions(corpus))
    starts = [f[0] for f in funcs]
    crt_index = {a: i for i, a in enumerate(crt)}
    events = []                                   # (crt_index, address, class)
    for i in range(len(tb) - 5):
        if tb[i] != 0xE8:
            continue
        tgt = (tva + i + 5 + struct.unpack_from("<i", tb, i + 1)[0]) & 0xFFFFFFFF
        if tgt != seed_addrs.REGISTER_TYPE and tgt not in lazy:
            continue
        site = tva + i
        k = bisect.bisect_right(starts, site) - 1
        a, n = funcs[k]
        if not (a <= site < a + n) or a not in crt_index:
            continue                              # GetIt / CreateVariant bodies, not initializers
        if tgt == seed_addrs.REGISTER_TYPE:
            if tb[i + 5:i + 7] != b"\x66\xa3":
                raise RegistryError("RegisterType call at %#x is not followed by mov [global], ax" % site)
            cls = glob[struct.unpack_from("<I", tb, i + 7)[0]]
        else:
            cls = lazy[tgt]
        events.append((crt_index[a], site, cls))
    order = []
    for _, _, cls in sorted(events):
        if cls not in order:
            order.append(cls)
    return order


def asm_check(exe_bytes: bytes, parsed: list[dict], getit: dict, corpus: Path) -> dict:
    """A second independent reader of the same calls: the raw argument pushes in front of each RegisterFunction call.
    The compiler pushes right to left: t9..t0, MIN, MAX, fnptr, &function-name, &class-name. Anchored on the `push fnptr`
    (68 imm32), this decodes MIN and MAX (`6a ib` each), the two name-string pushes, and then each type slot backwards:
    `6a ib` (literal), `call GetIt; push eax` (target compared with the GetIt table), `<load theOSIType*>; push eax`
    (the global's address compared with symbols.txt). Items it cannot decode (register pushes, locals) are counted as
    unchecked, never as agreeing. Returns counts and the list of disagreements."""
    base, secs = seed_addrs.sections(exe_bytes)
    text = [s for s in secs if s["name"] == ".text"][0]
    tb, tva = exe_bytes[text["roff"]:text["roff"] + text["vsz"]], text["va"]
    globals_ = {}
    for line in (corpus / "symbols.txt").read_text().splitlines():
        a, _, n = line.partition(" ")
        m = re.fullmatch(r"\??theOSIType(\w+?)(?:@@3GA)?", n.strip())
        if m:
            globals_[int(a, 16)] = m.group(1)
    getit_by_addr = {v["addr"]: v for v in getit.values()}
    order = sorted(r["site"] for r in parsed)
    prev_end = {s: (order[i - 1] - tva + 5 if i else 0) for i, s in enumerate(order)}
    st, bad = collections.Counter(), []
    load_re = re.compile(rb"(?s)\x66\xa1(....)|\x0f\xb7\x05(....)|\x66\x8b\x05(....)")
    for r in parsed:
        s, lo = r["site"] - tva, prev_end[r["site"]]
        who = "%s.%s@%#x" % (r["cls"], r["name"], r["site"])
        j = tb.rfind(b"\x68" + struct.pack("<I", r["body"]), lo, s)
        if j < 0 or s - j > 64:
            bad.append((who, "no push of the fnptr %#x just before the call" % r["body"]))
            continue
        if tb[j - 4] == 0x6A and tb[j - 2] == 0x6A:
            if (tb[j - 3], tb[j - 1]) == (r["min"], r["max"]):
                st["minmax_ok"] += 1
            else:
                bad.append((who, "min/max: asm (%d,%d) decomp (%d,%d)" % (tb[j - 3], tb[j - 1], r["min"], r["max"])))
        else:
            st["minmax_unchecked"] += 1
        if all(tb.find(b"\x68" + struct.pack("<I", a), lo, s) >= 0 for a in (r["cls_addr"], r["name_addr"])):
            st["names_ok"] += 1
        else:
            bad.append((who, "class/function string address not pushed before the call"))
        pos, stop = j - 4, False
        for i in range(NROW_TYPES):
            want = r["raw_types"][i]
            if stop:
                st["types_unchecked"] += 1
                continue
            if tb[pos - 2] == 0x6A:                                    # push imm8
                got, pos = tb[pos - 1], pos - 2
            elif tb[pos - 1] == 0x50 and tb[pos - 6] == 0xE8:          # call GetIt ; push eax
                tgt = (tva + pos - 1 + struct.unpack_from("<i", tb, pos - 5)[0]) & 0xFFFFFFFF
                g = getit_by_addr.get(tgt)
                if g is None:
                    bad.append((who, "slot %d: call to %#x is not a GetIt body" % (i, tgt)))
                    stop = True
                    continue
                got = g["lit"] if "lit" in g else ("game", g["lazy"][len("theOSIType"):])
                pos -= 6
            elif tb[pos - 1] == 0x50:                                  # push eax, loaded earlier
                m = None
                for m in load_re.finditer(tb[lo:pos - 1]):
                    pass
                a = struct.unpack("<I", next(g for g in m.groups() if g))[0] if m else None
                got = ("game", globals_[a]) if a in globals_ else None
                pos -= 1
            elif 0x51 <= tb[pos - 1] <= 0x57:                          # push reg: origin not tracked
                got, pos = None, pos - 1
            else:
                st["types_unchecked"] += 1
                stop = True
                continue
            if got is None:
                st["types_unchecked"] += 1
            elif got == want:
                st["types_ok"] += 1
            else:
                bad.append((who, "slot %d: asm %r decomp %r" % (i, got, want)))
    st["sites"] = len(parsed)
    return {"stats": dict(st), "bad": bad}


def base_osi_pairs(osi_path: Path) -> set[str]:
    """The case-folded class.func pairs at 0x82 CallGameFunction sites of Base.osi (tools/research/osi.py)."""
    spec = importlib.util.spec_from_file_location("osi_research", TOOLS / "research" / "osi.py")
    osi = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(osi)
    d = Path(osi_path).read_bytes()
    o = osi.parse(d)
    pairs = set()
    for _, off in osi.subroutines(o):
        for _, op, a in osi.disasm(d, off):
            if op == 0x82:
                pairs.add((o["strings"][a[0]] + "." + o["strings"][a[1]]).lower())
    return pairs


def _check_canonical(exe_bytes: bytes) -> None:
    import hashlib
    from lomn_data import BUILDS
    if hashlib.sha256(exe_bytes).hexdigest() != BUILDS["canonical"]["exe_sha256"]:
        raise SystemExit("not the canonical exe; the addresses here are only valid for it")


def build(exe_bytes: bytes, corpus: Path, osi_path: Path | None, check: bool = True) -> dict:
    corpus = Path(corpus)
    if check:
        _check_canonical(exe_bytes)
    image = Image(exe_bytes)
    crt = crt_layout(exe_bytes)
    parsed, replay = parse_corpus(corpus, image, crt)
    cc = cross_check(parsed, corpus)
    if cc["bad"]:
        raise RegistryError("decomp vs reg_sites.json disagree: %r" % cc["bad"][:5])
    for i, r in enumerate(parsed):     # execution order: .CRT initializers ascending, statement order, then ScInitFunc
        r["site"] = int(cc["matched"][i]["site"], 16)
    getit = getit_table(corpus)
    ac = asm_check(exe_bytes, parsed, getit, corpus)
    if ac["bad"]:
        raise RegistryError("decomp vs asm pushes disagree: %r" % ac["bad"][:5])
    asm_order = type_order_from_asm(exe_bytes, corpus, crt, getit)
    if asm_order != [e["class"] for e in replay.order]:
        raise RegistryError("game-type first-use order differs between the decomp replay and the raw call sites: %r vs %r"
                            % ([e["class"] for e in replay.order], asm_order))
    pairs = base_osi_pairs(osi_path) if osi_path else set()
    first: dict[str, dict] = {}
    for r in parsed:
        first.setdefault((r["cls"] + "." + r["name"]).lower(), r)
    rows = []
    for r in parsed:
        key = (r["cls"] + "." + r["name"]).lower()
        rows.append({
            "cls": r["cls"], "name": r["name"], "key": key,
            "site": "0x%08x" % r["site"], "body": "0x%08x" % r["body"],
            "max": r["max"], "min": r["min"],
            "types": [type_name(t, r["max"], i) for i, t in enumerate(r["raw_types"])],
            "crt_index": r["crt_index"], "init": "0x%08x" % r["init"],
            "winner": first[key] is r, "called_by_base_osi": key in pairs,
            "phase": r["phase"],
        })
    keys = {r["key"] for r in rows}
    cv = createvariant_types(corpus, image)
    odd = [(e["class"], e["regname"]) for e in replay.order if e["regname"] != e["class"]] + \
          [(c["class"], c["regname"]) for c in cv if c["regname"] != c["class"]]
    if odd:
        raise RegistryError("registered type name differs from its theOSIType global: %r" % odd)
    flags = {"inserted": image.byte(INSERT_FLAG_TRUE), "refused": image.byte(INSERT_FLAG_FALSE)}
    if flags != {"inserted": 1, "refused": 0}:
        raise RegistryError("insert_one result flags are not (1, 0): %r" % flags)
    return {"rows": rows, "parsed": parsed, "replay": replay, "crt": crt, "cross_check": cc, "asm_check": ac,
            "missing": sorted(pairs - keys), "base_pairs": pairs, "asm_type_order": asm_order, "getit": getit, "flags": flags,
            "createvariant": cv,
            "type_sites": type_site_accounting(exe_bytes, corpus, crt)}


# ----------------------------------------------------------------------------------------------------------- types.md
# Evidence cited in the document (all read from the corpus / exe as data; see the module docstring).
GETIT_ID_NAMES = {0: UNKNOWN_PREFIX + "0x0", **BUILTIN}


def type_table_names(res: dict) -> set[str]:
    """Every type name defined by the emitted table: builtin names, "type:<Class>" per registered game type, and the
    unknown literals."""
    names = set(BUILTIN.values())
    names |= {"type:" + e["class"] for e in res["replay"].order}
    names |= {r_t for row in res["rows"] for r_t in row["types"] if r_t and r_t.startswith(UNKNOWN_PREFIX)}
    return names


def render_types(res: dict) -> str:
    rows, replay = res["rows"], res["replay"]
    uses = collections.Counter(t for r in rows for t in r["types"] if t)
    cv = {c["class"] for c in res["createvariant"]}
    L = []
    w = L.append
    w("# OSI variant types and the native registry (canonical exe)")
    w("")
    w("Generated by `tools/osi/registry.py` (`make osi-tables`) from the canonical `LEGOBionicle.exe` read as data, the decomp")
    w("corpus `gen/decomp` and `Base.osi`. It documents the type column of `gen/osi/natives.json` (one row per")
    w("`ScOSISystem::RegisterFunction` call, 0x5fab30). Names, addresses and counts only; no decompiled code.")
    w("")
    w("- **Rows:** %d registrations, %d distinct `class.func` keys (case-folded), %d keys registered twice."
      % (len(rows), len({r["key"] for r in rows}), sum(1 for k, n in collections.Counter(r["key"] for r in rows).items() if n > 1)))
    w("- **`types`** holds 10 entries per row: entry *i* is the declared type of argument *i* for *i* < `max`, and `null` for")
    w("  the unused slots (the binary stores 0x0f there). A declared `nothing` (0x0f below `max`) would read `\"nothing\"`; no row has one.")
    w("- **Row fields:** `site` is the `call RegisterFunction` address, `body` the native, `init` the function that makes the call,")
    w("  `crt_index` that function's index among the %d `.CRT` pointers (0 = the dword at 0x76f004; null when it is not an" % len(res["crt"]))
    w("  initializer), `winner` whether this row is the one the map keeps, `called_by_base_osi` whether `Base.osi` has a 0x82 site")
    w("  for the case-folded key, `phase` `crt` or `scinit`.")
    w("  Rows are in execution order. Menu tagging is out of scope here (moved to M2a-2).")
    w("- **Arguments** are `(this, class&, func&, fnptr, MAX, MIN, type0..type9)`; MAX is stored at +0xc of the 36-byte")
    w("  `RegisteredFunction` and MIN at +0xd (`RegisterFunction` 0x5fab30). Each type is a u16 at +0xe + 2i.")
    w("")
    w("## Builtin variant types")
    w("")
    w("| id | name | meaning and citation | declared in %d registration slots |" % (len(rows) * NROW_TYPES))
    w("|---|---|---|---|")
    meaning = {
        0x0F: "nothing: `ScOSIVariant` ctor 0x60b2a0 and opcode 0x47 (docs/osi/opcodes.md, \"Variant types seen in the handlers\"). In a registration it fills every unused slot.",
        0x10: "int32: opcodes 0x40-0x43, 0x45. `GetVariantType<T>::GetIt` returns it for bool, short, unsigned short, long, unsigned long, `ScIdentifier` and the enum types (table below).",
        0x11: "string-table index: opcode 0x46. Never a declared parameter type.",
        0x12: "float32: opcode 0x44. `GetIt` returns it for float and double.",
        0x15: "32-bit colour: opcodes 0x48, 0xa0-0xa7. `GetIt` returns it for `StRGBABytes` (0x422930).",
        0x16: "dynamic string: opcode 0x70, `ConvertVariant` 0x60b9d0. `GetIt` returns it for `_ScBaseString` and `const _ScBaseString&`.",
        0x200: "by-reference box built around a native call: opcodes 0x82-0x84. Declared by two natives only.",
    }
    for i, n in sorted(BUILTIN.items()):
        cnt = sum(1 for r in rows for k, t in enumerate(r["types"]) if t == n)
        if i == 0x0F:
            cnt = sum(1 for r in rows for t in r["types"] if t is None) + uses.get("nothing", 0)
        w("| 0x%02x | %s | %s | %d |" % (i, n, meaning[i], cnt))
    w("")
    w("## `GetVariantType<T>::GetIt` (C++ parameter type to variant type)")
    w("")
    w("Read from the %d `GetIt` bodies in the corpus. The lazy ones register a game type on the first call that finds the" % len(res["getit"]))
    w("global `theOSIType<Class>` still zero.")
    w("")
    w("| C++ type (Ghidra spelling) | `GetIt` | result |")
    w("|---|---|---|")
    for t, g in sorted(res["getit"].items(), key=lambda kv: kv[1]["addr"]):
        if "lit" in g:
            res_txt = "0x%x (%s)" % (g["lit"], GETIT_ID_NAMES.get(g["lit"], UNKNOWN_PREFIX))
        else:
            res_txt = "lazy game type `%s`" % g["lazy"][len("theOSIType"):]
        w("| `%s` | 0x%x | %s |" % (t, g["addr"], res_txt))
    w("")
    w("## Unknown literals")
    w("")
    unk = sorted({t for r in rows for t in r["types"] if t and t.startswith(UNKNOWN_PREFIX)})
    w("Type literals in the registrations that are not builtins: %s." % (", ".join("`%s`" % u for u in unk) or "none"))
    w("")
    for u in unk:
        users = ["%s.%s" % (r["cls"], r["name"]) for r in rows if u in r["types"]]
        n_slots = sum(r["types"].count(u) for r in rows)
        base = sum(1 for r in rows if u in r["types"] and r["called_by_base_osi"])
        w("- `%s`: %d slots in %d natives, %d of them called by `Base.osi`. It is what `GetVariantType<char*>::GetIt` (0x54a550)"
          % (u, n_slots, len(users), base))
        w("  returns, i.e. a `char*` parameter. `BCLCallGameFunction` (0x6015a0) converts an argument whose type differs from the")
        w("  declared one with `ConvertVariant` (0x60b9d0); docs/osi/opcodes.md records that a target other than int, float or string is")
        w("  an error (\"Requested variant conversion unimplimented\"). What a script can pass for such a slot is therefore settled by")
        w("  `ConvertVariant` and the native bodies, not here; the registry only records the literal. Natives: %s." % ", ".join(users))
    w("")
    w("`0x18`, `0x21` and `0x22` (listed in the task brief as possible literals) do **not** occur as a type argument in any of")
    w("the %d slots. In the opcode table they are opcodes (0x18 `GetMemberValueFromString`, 0x21 `GetVariableValue`, 0x22" % (len(rows) * NROW_TYPES))
    w("`SetVariableValue`); as variant types 0x21 and 0x22 would lie in the game-type range, so nothing needs settling.")
    w("")
    w("## Game types: the id order rule")
    w("")
    w("Game-type ids are assigned from 0x20 upward, on first registration, in static-init order (`.CRT` ascending). Evidence:")
    w("")
    w("- `ScOSISystem::ScOSISystem` (0x5fa750) stores 0x20 as the type counter at `this+0`; `ScOSISystem::RegisterType` (0x5fae00)")
    w("  returns the counter, inserts the name into the type map and increments the counter.")
    w("- Each class has a global `theOSIType<Class>` (u16, in `.bss`, zero until registered). Every use is `if (theOSIType<Class> == 0)")
    w("  { theOSIType<Class> = RegisterType(\"<Class>\") }`, inlined in the initializer, or the same guard inside")
    w("  `GetVariantType<Class*>::GetIt` and `CreateVariant(Class*)`. Only the first execution of a guard registers.")
    w("- `__RunInit` (0x65f530) calls the dwords of the `.CRT` section from 0x76f004 upward until a zero dword. The section is a zero")
    w("  dword at 0x76f000 (skipped), %d initializer pointers with no zero between them, and a terminating zero, so all %d run, in" % (len(res["crt"]), len(res["crt"])))
    w("  ascending order (checked by `crt_layout`). `_WinMainCRTStartup` (0x6565d0) calls `__RunInit` and only then `_WinMain@16`.")
    w("- The order was derived twice: from the decompiled statements of every initializer, and from the raw bytes alone (each")
    w("  `call RegisterType` is followed by `mov [theOSIType<Class>], ax`; each lazy `GetIt` is a known address). Both give the list below.")
    w("- The numeric id of a type is therefore `0x20 + (its position below)`, provided no `RegisterType` call runs earlier than")
    w("  the first initializer. That condition is derived from the static order, not observed: the exe was never run. The")
    w("  registry stores type NAMES, never numeric ids.")
    w("")
    w("### Registered game types in first-use order")
    w("")
    w("Replayed over all %d `.CRT` initializers in ascending order: %d `RegisterType` guards and %d calls of a lazy `GetIt` were seen" % (len(res["crt"]), replay.guard_events, replay.getit_events))
    w("(%d game types registered by them). All %d `RegisterType` call sites of the exe are accounted for: %s." % (len(replay.order), sum(res["type_sites"].values()),
      ", ".join("%d in %s" % (v, k.replace("_", " ")) for k, v in sorted(res["type_sites"].items()))))
    w("")
    w("| rank | name | first use: `.CRT` index, initializer | by | declared in registry slots | also registered by `CreateVariant` |")
    w("|---|---|---|---|---|---|")
    for e in replay.order:
        w("| %d | `type:%s` | %d, 0x%x | %s | %d | %s |" % (e["rank"], e["class"], e["crt_index"], e["init"], e["via"],
                                                          uses.get("type:" + e["class"], 0), "yes" if e["class"] in cv else "no"))
    w("")
    w("## Duplicates")
    w("")
    dups = collections.defaultdict(list)
    for r in rows:
        dups[r["key"]].append(r)
    dups = {k: v for k, v in dups.items() if len(v) > 1}
    w("%d keys are registered twice (all `GcHudSprite`), each time by a different body, from the initializers 0x%x (`.CRT` index %d)"
      % (len(dups), int(next(iter(dups.values()))[0]["init"], 16), next(iter(dups.values()))[0]["crt_index"]) if dups else "No key is registered twice.")
    if dups:
        a = next(iter(dups.values()))[1]
        w("and 0x%x (index %d)." % (int(a["init"], 16), a["crt_index"]))
        w("")
        w("**Winner: the first registration in execution order, i.e. the lower `.CRT` index.** Evidence:")
        w("")
        w("- `ScOSISystem::RegisterFunction` (0x5fab30) allocates a 36-byte `RegisteredFunction`, builds the key (the two name tokens,")
        w("  `GetToken` 0x5faae0, case-folded) and calls `__tree<...>::insert_one` (0x5fb290) on the function map. The call at")
        w("  0x5fac1f is followed directly by the epilogue: the result is ignored and a refused insert leaks the new object.")
        w("- `insert_one` descends the tree, keeps the last node not ordered after the key, and compares that node with the key")
        w("  once more (0x5fb2f4-0x5fb30e). When the node's key is not less than the new key, it stores that EXISTING node and the byte at")
        w("  0x74d66c (value %d in the file) at 0x5fb344-0x5fb355 and returns without inserting. Only otherwise does it call" % res["flags"]["refused"])
        w("  `insert_node_at` (0x5fbcb0) and store the byte at 0x74d668 (value %d). It is a unique-key insert that never overwrites." % res["flags"]["inserted"])
        w("- So the earlier registration stays and the later one is dead. `winner` is true for the first row of each key in execution")
        w("  order (`.CRT` ascending, then statement order, then the `ScInitFunc` callback). Both rows are kept in the registry.")
        w("- Key equality is equality of the token pair: the %d distinct keys have %d distinct token pairs, so no two different names"
          % (len({r["key"] for r in rows}), len({dual_token(r["cls"], r["name"]) for r in rows})))
        w("  collide.")
        w("")
        w("| key | winner body (initializer, index) | loser body (initializer, index) |")
        w("|---|---|---|")
        for k, v in dups.items():
            win = [r for r in v if r["winner"]][0]
            lose = [r for r in v if not r["winner"]][0]
            w("| `%s` | %s (%s, %d) | %s (%s, %d) |" % (k, win["body"], win["init"], win["crt_index"], lose["body"], lose["init"], lose["crt_index"]))
    w("")
    w("## Registrations outside the `.CRT` initializers")
    w("")
    late = [r for r in rows if r["phase"] != "crt"]
    if late:
        w("%d rows (%s) are registered by the callback 0x%x. `ScInitFunc::ScInitFunc` (0x4e0260) only queues it, from the initializer"
          % (len(late), ", ".join("%s.%s" % (r["cls"], r["name"]) for r in late), int(late[0]["init"], 16)))
        w("0x61d7c0; `SrInitializeAll` (0x4e02c0) calls each queued callback with argument 0 (the callback registers only when its")
        w("argument is 0), and `_WinMain@16` (0x401020) calls `SrInitializeAll` before `AppMain`, i.e. after every `.CRT` initializer. Their")
        w("`crt_index` is `null` and their `phase` is `scinit`. Their order relative to the `.CRT` rows cannot matter: both keys are unique.")
    w("")
    w("## Not statically determinable")
    w("")
    w("- **Run-time registration through `CreateVariant`.** %d `CreateVariant(Class*)` overloads carry the lazy guard (the others wrap" % len(res["createvariant"]))
    w("  builtins). If a native returns an object of a class whose global is still zero at that moment, the class is registered then,")
    w("  with the next counter value. Here every one of the %d classes is already registered by an initializer (the sets are equal)," % len(cv))
    w("  so `CreateVariant` never assigns an id in practice; this is an inference from the static order, UNVERIFIED at run time.")
    w("- **Anything reaching `RegisterType` outside the replay.** The replay reads each initializer's own statements and the `GetIt`")
    w("  table. The accounting above finds no `RegisterType` call site other than those two kinds and `CreateVariant`, so a type could")
    w("  only be registered earlier than modelled by running a `GetIt` or `CreateVariant` from code that is not an initializer.")
    w("- **Numeric ids.** Never executed; see the order rule. Consumers must key on names and replay the order themselves.")
    w("- **Type 0 and the by-reference box.** Whether the %d `unknown:0x0` slots can ever be satisfied is decided by `ConvertVariant`" % sum(r["types"].count("unknown:0x0") for r in rows))
    w("  and the natives' bodies, not by this table.")
    w("")
    return "\n".join(L) + "\n"


def write_rows(rows: list[dict], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("[\n" + ",\n".join(json.dumps(r, separators=(",", ":")) for r in rows) + "\n]\n")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--osi", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--out-json", required=True)
    ap.add_argument("--out-types", required=True)
    ap.add_argument("--check", action="store_true", help="exit 1 if --out-types is not up to date")
    a = ap.parse_args(argv)
    res = build(Path(a.exe).read_bytes(), Path(a.corpus), Path(a.osi))
    rows = res["rows"]
    doc = render_types(res)
    dups = [k for k, n in collections.Counter(r["key"] for r in rows).items() if n > 1]
    unknown = collections.Counter(t for r in rows for t in r["types"] if t and t.startswith(UNKNOWN_PREFIX))
    print("natives: %d rows, %d distinct keys, %d duplicated keys, %d winners" % (
        len(rows), len({r["key"] for r in rows}), len(dups), sum(r["winner"] for r in rows)))
    print("base.osi: %d distinct class.func pairs, %d registered, %d missing: %s" % (
        len(res["base_pairs"]), len(res["base_pairs"]) - len(res["missing"]), len(res["missing"]), ", ".join(res["missing"])))
    print("unknown type literals: %s" % (dict(unknown) or "none"))
    print("game types: %d in first-use order; fnptr cross-check vs reg_sites.json: %d/%d agree; asm pushes: %s" % (
        len(res["replay"].order), len(rows) - len(res["cross_check"]["bad"]), len(rows), res["asm_check"]["stats"]))
    if a.check:
        out = Path(a.out_types)
        ok = out.exists() and out.read_text() == doc
        print("types.md: " + ("up to date" if ok else "OUT OF DATE"))
        return 0 if ok else 1
    write_rows(rows, Path(a.out_json))
    Path(a.out_types).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out_types).write_text(doc)
    print("wrote %s and %s" % (a.out_json, a.out_types))
    return 0


if __name__ == "__main__":
    sys.exit(main())
