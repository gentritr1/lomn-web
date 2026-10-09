"""M2a-2 T2a-pc: tools/osi/error_pc.py, the per-handler error-PC pass (docs/osi/opcodes.md "Error PC (the PC at the
throw), per handler"; conformance rule 15; D10/D16).

Two kinds of test. The synthetic ones feed a small hand-written listing (invented addresses, not the exe's code) to the
abstract interpreter and run anywhere. The pinned ones read the canonical exe as data and skip with
`SKIPPED (no LOMN_DATA)` without the game files. Every pinned value was hand-read from
`objdump -d --x86-asm-syntax=intel --start-address=H --stop-address=H+size LEGOBionicle.exe` (2026-10-09), with the
format string read from the exe at the pushed address:
- 0x02 0x6038a0 / 0x03 0x6038d0: `inc [this+0x1f58]` at 0x6038a6 / 0x6038d6, then Error at 0x6038b4 / 0x6038e4 -> +1
  (the M2a-1 rule 6.13 value).
- 0x41 0x6065e0: Error ("unwritten instruction BCL_PushConstanti24 called", 0x74fe70) at 0x6065ee, no PC write -> +0.
- BCLUnused 0x60be60: Error ("Call of undefined instruction", 0x75132f) at 0x60be6e, no PC write -> +0.
- 0x82 0x6015a0: five `inc` (0x6015c6, 0x6015d8, 0x6015f3, 0x601611, 0x60162c) before the map miss SrOSIError at
  0x601725 ("Call to unregistered Game Function", 0x74ec84) -> +5; the sixth `inc` (0x601780) precedes "Too many
  parameters passed to game function" (0x74eca7) at 0x6017ac, which is caught (.exc catch pad 0x601c94) -> +6.
- 0x73 0x6045f0: `inc` at 0x6045fa, then "Divide by 0" (0x74f557) at 0x604668, 0x6046d7, 0x604733, 0x6047b8 and the
  lhs / rhs type errors (0x74f563 / 0x74f597) at 0x604787 / 0x6047a4 -> +1 each.
- 0x75 0x604a00: `inc` at 0x604a0a, then lhs / rhs type errors at 0x604a5d / 0x604a7a and "Divide by 0" at 0x604a8e
  -> +1 each.
"""
import importlib.util
import shutil
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod                  # dataclasses resolve annotations through sys.modules
    spec.loader.exec_module(mod)
    return mod


epc = _load("osi_error_pc", REPO / "tools" / "osi" / "error_pc.py")


# ----------------------------------------------------------------------------------------------- synthetic listings

def _listing(rows):
    """objdump-style lines from (addr, size, text) rows; the bytes column is filler of the right length."""
    out = []
    for addr, size, text in rows:
        mnem, _, ops = text.partition(" ")
        out.append("  %x: %s \t%s\t%s" % (addr, " ".join(["90"] * size), mnem, ops))
    return "\n".join(out)


VTABLE = 0x9000
ERROR_SLOT = 0xC


def _analyze_synthetic(rows, entry):
    dis = epc.Disasm.from_listing(_listing(rows))
    an = epc.Analyzer(dis, vtable={ERROR_SLOT: "Error"}, strings={0x7000: "boom", 0x7010: "bang %d"}, exc_sites={})
    return an.handler(entry)


def test_synthetic_inc_then_error_is_plus_one():
    res = _analyze_synthetic([
        (0x1000, 2, "mov edx, ecx"),
        (0x1002, 6, "inc dword ptr [edx + 0x1f58]"),
        (0x1008, 5, "push 0x7000"),
        (0x100d, 1, "push edx"),
        (0x100e, 2, "mov esi, dword ptr [edx]"),
        (0x1010, 3, "call dword ptr [esi + 0xc]"),
        (0x1013, 1, "ret"),
    ], 0x1000)
    assert [(s.addr, sorted(s.pcs), s.message) for s in res.sites] == [(0x1010, ["+1"], "boom")]
    assert res.exits == set()          # Error never returns: the path ends at the call


def test_synthetic_paths_and_spilled_this():
    """`this` spilled to a frame slot and reloaded; an error on the branch before the second advance is +1, the
    one after it +3 (add imm); the normal exit is +3."""
    res = _analyze_synthetic([
        (0x2000, 6, "mov dword ptr [ebp - 0x10], ecx"),
        (0x2006, 3, "mov eax, dword ptr [ebp - 0x10]"),
        (0x2009, 6, "inc dword ptr [eax + 0x1f58]"),
        (0x200f, 2, "test ecx, ecx"),
        (0x2011, 2, "je 0x2020"),
        (0x2013, 5, "push 0x7010"),
        (0x2018, 3, "push dword ptr [ebp - 0x10]"),
        (0x201b, 2, "mov esi, dword ptr [eax]"),
        (0x201d, 3, "call dword ptr [esi + 0xc]"),
        (0x2020, 3, "mov edi, dword ptr [ebp - 0x10]"),
        (0x2023, 7, "add dword ptr [edi + 0x1f58], 0x2"),
        (0x202a, 2, "mov esi, dword ptr [edi]"),
        (0x202c, 2, "push edi"),
        (0x202e, 3, "call dword ptr [esi + 0xc]"),
        (0x2031, 1, "ret"),
    ], 0x2000)
    got = {s.addr: sorted(s.pcs) for s in res.sites}
    assert got == {0x201D: ["+1"], 0x202E: ["+3"]}


def test_synthetic_unknown_pc_write_form_is_refused():
    """Only the four write forms are modelled; anything else touching [this+0x1f58] must stop the pass."""
    with pytest.raises(epc.UnmodelledPcWrite):
        _analyze_synthetic([
            (0x3000, 7, "or dword ptr [ecx + 0x1f58], 0x1"),
            (0x3007, 1, "ret"),
        ], 0x3000)


# ------------------------------------------------------------------------------------------------- the canonical exe

@pytest.fixture(scope="module")
def result(canonical_exe):
    if not canonical_exe.exists():
        pytest.skip("SKIPPED (no canonical exe)")
    if shutil.which("objdump") is None:
        pytest.skip("SKIPPED (no objdump)")
    return epc.analyze(canonical_exe)


def _sites(result, op):
    h = result.by_op[op]
    return {s.addr: s for s in h.sites}


def test_handler_count(result):
    assert len(result.handlers) == 97                       # 96 named + BCLUnused (opcodes.md)
    assert result.by_op[0x01].addr == 0x60BE60 and result.by_op[0x85].addr == 0x60BE60


@pytest.mark.parametrize("op,site,fmt", [(0x02, 0x6038B4, "DebugOn"), (0x03, 0x6038E4, "DebugOff")])
def test_debug_on_off_plus_one(result, op, site, fmt):
    s = _sites(result, op)
    assert list(s) == [site]
    assert s[site].pcs == {"+1"} and fmt in s[site].message and not s[site].caught


def test_unused_and_pushi24_plus_zero(result):
    assert {a: s.pcs for a, s in _sites(result, 0x41).items()} == {0x6065EE: {"+0"}}
    assert {a: s.pcs for a, s in _sites(result, 0x01).items()} == {0x60BE6E: {"+0"}}
    assert _sites(result, 0x01)[0x60BE6E].message == "Call of undefined instruction"


def test_call_game_function_map_miss_and_caught_argc(result):
    s = _sites(result, 0x82)
    assert s[0x601725].pcs == {"+5"} and s[0x601725].message == "Call to unregistered Game Function"
    assert not s[0x601725].caught
    assert s[0x6017AC].pcs == {"+6"} and s[0x6017AC].caught
    # the caught errors are re-raised outside the try as "Script is calling %s.%s"
    assert s[0x601DD3].message.startswith("Script is calling") and "+6" in s[0x601DD3].pcs


def test_divide_errors_plus_one(result):
    s = _sites(result, 0x73)
    for a in (0x604668, 0x6046D7, 0x604733, 0x6047B8):
        assert s[a].pcs == {"+1"} and s[a].message == "Divide by 0"
    assert s[0x604787].pcs == {"+1"} and "(lhs)" in s[0x604787].message
    assert s[0x6047A4].pcs == {"+1"} and "(rhs)" in s[0x6047A4].message


def test_modulus_errors_plus_one(result):
    s = _sites(result, 0x75)
    assert {a: x.pcs for a, x in s.items() if x.kind != "callee"} == {
        0x604A5D: {"+1"}, 0x604A7A: {"+1"}, 0x604A8E: {"+1"}}
    assert s[0x604A8E].message == "Divide by 0"


def test_normal_exits_match_operand_sizes(result):
    """Self-check of the pass against osi.py: every handler that falls through (no jump, call or return) leaves PC at
    1 + its operand bytes on every normal exit."""
    osi = _load("osi_research", REPO / "tools" / "research" / "osi.py")
    size = {"b": 1, "B": 1, "h": 2, "H": 2, "i": 4, "I": 4, "f": 4, "T": 3}
    control = {0x50, 0x51, 0x52, 0x53, 0x54, 0xFF}          # set PC from an operand, a frame or stop
    checked = 0
    for op, h in result.by_op.items():
        if op in control or h.addr == 0x60BE60 or not h.exits:
            continue
        fmt = "" if op in osi.NOARG else osi.OPS[op]
        want = "+%d" % (1 + sum(size[c] for c in fmt))
        assert h.exits == {want}, (hex(op), h.exits, want)
        checked += 1
    assert checked >= 80


def test_pc_write_forms(result):
    """Every PC write the walk reached is one of the four modelled forms (anything else raises UnmodelledPcWrite);
    counted per instruction address. Reconciled by hand against a grep of every `[reg + 0x1f58]` store in the
    linear-sweep listing: that grep finds 166, missing 6 handler incs the sweep decoded out of step (BCLHalt 0x603848,
    the three line-number ops, 0x7b 0x605c02, 0x47 0x606848) and adding 3 that no handler reaches (the ctor 0x60b283,
    CallWithReturn 0x60b5a5, SetPCAndArgs 0x60b623): 166 - 3 + 6 = 169."""
    forms = {}
    for f in result.pc_writes.values():
        forms[f] = forms.get(f, 0) + 1
    assert forms == {"inc": 161, "add imm": 4, "add reg": 1, "mov reg": 3}


def test_call_from_string_rethrow(result):
    """0x83: the class operand is consumed (+3) before the string check; the n byte (+4, inc 0x60237c) is read inside
    the try before the lookup, so every caught OSI error is re-raised at 0x602b24 with +4."""
    s = _sites(result, 0x83)
    assert s[0x6022C3].pcs == {"+3"} and not s[0x6022C3].caught
    assert s[0x602431].pcs == {"+4"} and s[0x602431].caught == 0x6029F6
    rethrow = s[0x602B24]
    non_alloc = {pc for pc, origins in rethrow.by_origin.items()
                 for o in origins if o is not None and s[o].kind != "alloc"}
    assert non_alloc == {"+4"}


def test_document_block_is_current(result):
    doc = (REPO / "docs" / "osi" / "opcodes.md").read_text()
    assert epc.replace_block(doc, epc.markdown(result)) == doc, "run: tools/osi/error_pc.py --write docs/osi/opcodes.md"
