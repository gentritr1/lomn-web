"""M2a-3 T5b: Bionicle.ini with Win32 GetPrivateProfileString semantics, and the INI natives (the Python oracle).

Contract: tools/research/osi_natives/ini.py header (docs/osi/hud.md §6 + Microsoft's documented API behaviour). Python's
configparser is NOT an oracle here. The crafted table below is the plan's list (case, quotes, duplicates, a 300-byte value,
a missing section, comment lines) with every expectation written by hand from the contract, not from the reader.

Fixed output format, one line per case, for `diff` against the C++ side (run `python tools/tests/test_ini.py`):
    INI <id> <fn> sec=<section> key=<key> def=<default hex> size=<n> -> len=<n> hex=<value hex>
(`fn` is `str` for the string form and `int` for the Int form; the Int form prints `size=0`, `len=4` and the u32 as 8 hex digits
big-endian.)
"""
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "osi"))
sys.path.insert(0, str(REPO / "tools" / "research"))

import osi_vm  # noqa: E402
from osi_vm import Variant  # noqa: E402

LONG = b"A" * 100 + b"B" * 100 + b"C" * 100          # 300 bytes

CRAFTED = b"\r\n".join([
    b"; leading comment",
    b"[Alpha]",
    b"Key=value1",
    b"key=value2",
    b"  Spaced \t=  \t padded value \t ",
    b'Dq="double quoted"',
    b"Sq='single quoted'",
    b"Mixed=\"not matched'",
    b'OnlyOpen="abc',
    b'One="',
    b"Empty=",
    b'EmptyQuoted=""',
    b"Inline=abc ; not a comment",
    b";Commented=hidden",
    b"   ; Indented=hidden",
    b"NoEquals",
    b"Eq=a=b=c",
    b'Inner=" spaced "',
    b"Long=" + LONG,
    b"[beta]",
    b"Key=beta1",
    b"[Alpha]",
    b"Key=second section",
    b"[Gamma Sec]",
    b" x = 1 ",
    b"Neg=-5",
    b"Dec=12abc",
    b"Word=abc",
    b"Big=4294967297",
    b"Quoted=\"42\"",
]) + b"\r\n"

# id, fn, section, key, default, size -> expected bytes (hand-written from the contract)
CASES = [
    ("plain",            "str", b"Alpha",     b"Key",         b"D", 255, b"value1"),
    ("case-section",     "str", b"ALPHA",     b"Key",         b"D", 255, b"value1"),
    ("case-key",         "str", b"alpha",     b"KEY",         b"D", 255, b"value1"),
    ("dup-key-first",    "str", b"Alpha",     b"key",         b"D", 255, b"value1"),
    ("dup-section-first","str", b"Alpha",     b"Key",         b"D", 255, b"value1"),
    ("other-section",    "str", b"Beta",      b"Key",         b"D", 255, b"beta1"),
    ("trim-key-value",   "str", b"Alpha",     b"Spaced",      b"D", 255, b"padded value"),
    ("dquote",           "str", b"Alpha",     b"Dq",          b"D", 255, b"double quoted"),
    ("squote",           "str", b"Alpha",     b"Sq",          b"D", 255, b"single quoted"),
    ("mixed-quotes",     "str", b"Alpha",     b"Mixed",       b"D", 255, b"\"not matched'"),
    ("open-quote",       "str", b"Alpha",     b"OnlyOpen",    b"D", 255, b'"abc'),
    ("lone-quote",       "str", b"Alpha",     b"One",         b"D", 255, b'"'),
    ("empty-value",      "str", b"Alpha",     b"Empty",       b"D", 255, b""),
    ("empty-quoted",     "str", b"Alpha",     b"EmptyQuoted", b"D", 255, b""),
    ("inline-semicolon", "str", b"Alpha",     b"Inline",      b"D", 255, b"abc ; not a comment"),
    ("comment-line",     "str", b"Alpha",     b"Commented",   b"D", 255, b"D"),
    ("indented-comment", "str", b"Alpha",     b"Indented",    b"D", 255, b"D"),
    ("no-equals",        "str", b"Alpha",     b"NoEquals",    b"D", 255, b"D"),
    ("equals-in-value",  "str", b"Alpha",     b"Eq",          b"D", 255, b"a=b=c"),
    ("quoted-keeps-ws",  "str", b"Alpha",     b"Inner",       b"D", 255, b" spaced "),
    ("long-300-size256", "str", b"Alpha",     b"Long",        b"D", 256, LONG[:255]),
    ("long-300-size255", "str", b"Alpha",     b"Long",        b"D", 255, LONG[:254]),
    ("long-300-size4096","str", b"Alpha",     b"Long",        b"D", 4096, LONG),
    ("size-6",           "str", b"Alpha",     b"Key",         b"D", 6, b"value"),
    ("size-1",           "str", b"Alpha",     b"Key",         b"D", 1, b""),
    ("size-0",           "str", b"Alpha",     b"Key",         b"D", 0, b""),
    ("default-cut",      "str", b"Alpha",     b"Missing",     b"default", 4, b"def"),
    ("missing-key",      "str", b"Alpha",     b"Missing",     b"D", 255, b"D"),
    ("missing-section",  "str", b"Nope",      b"Key",         b"D", 255, b"D"),
    ("key-in-wrong-sec", "str", b"beta",      b"Spaced",      b"D", 255, b"D"),
    ("section-with-space","str", b"Gamma Sec", b"x",          b"D", 255, b"1"),
    ("int-plain",        "int", b"Gamma Sec", b"x",           b"7", 0, b"\0\0\0\1"),
    ("int-negative",     "int", b"Gamma Sec", b"Neg",         b"7", 0, b"\xff\xff\xff\xfb"),
    ("int-trailing-junk","int", b"Gamma Sec", b"Dec",         b"7", 0, b"\0\0\0\x0c"),
    ("int-nondigit",     "int", b"Gamma Sec", b"Word",        b"7", 0, b"\0\0\0\0"),
    ("int-wraps-u32",    "int", b"Gamma Sec", b"Big",         b"7", 0, b"\0\0\0\1"),
    ("int-quoted",       "int", b"Gamma Sec", b"Quoted",      b"7", 0, b"\0\0\0\x2a"),
    ("int-missing",      "int", b"Gamma Sec", b"Nope",        b"7", 0, b"\0\0\0\7"),
    ("int-missing-sec",  "int", b"Nope",      b"x",           b"7", 0, b"\0\0\0\7"),
]


def _ini():
    from osi_natives.ini import IniSettings
    return IniSettings.from_text(CRAFTED)


def _run(ini, fn, sec, key, dflt, size):
    if fn == "int":
        return ini.get_int(sec, key, int(dflt)).to_bytes(4, "big")
    return ini.get_private_profile_string(sec, key, dflt, size)


def render_table():
    ini, out = _ini(), []
    for cid, fn, sec, key, dflt, size, _ in CASES:
        v = _run(ini, fn, sec, key, dflt, size)
        out.append("INI %s %s sec=%s key=%s def=%s size=%d -> len=%d hex=%s"
                   % (cid, fn, sec.decode(), key.decode(), dflt.hex(), size, len(v), v.hex()))
    return out


@pytest.mark.parametrize("case", CASES, ids=[c[0] for c in CASES])
def test_crafted_case(case):
    cid, fn, sec, key, dflt, size, want = case
    assert _run(_ini(), fn, sec, key, dflt, size) == want, cid


def test_crafted_table_is_stable_and_printed(capsys):
    lines = render_table()
    assert len(lines) == len(CASES)
    with capsys.disabled():
        print("\n".join(lines))


def test_lf_only_and_missing_file():
    from osi_natives.ini import IniSettings
    a = IniSettings.from_text(b"[S]\nk=v\n")
    assert a.get_private_profile_string(b"s", b"K", b"d") == b"v"
    assert IniSettings().get_private_profile_string(b"S", b"k", b"d") == b"d"          # no file: the default
    assert IniSettings().get_int(b"S", b"k", -1) == 0xFFFFFFFF


def test_keys_before_any_section_and_unterminated_section_line():
    from osi_natives.ini import IniSettings
    i = IniSettings.from_text(b"k=top\n[Open\nj=1\n[S]\nk=in\n")
    assert i.get_private_profile_string(b"", b"k", b"d") == b"d"
    assert i.get_private_profile_string(b"S", b"k", b"d") == b"in"
    assert i.get_private_profile_string(b"Open", b"j", b"d") == b"d"


# --------------------------------------------------------------------------------------------- the owner's INI
@pytest.fixture
def owner_install(lomn_data):
    return lomn_data / "install"


def test_owner_ini_values(owner_install):
    from osi_natives.framework import Vfs
    from osi_natives.ini import IniSettings
    ini = IniSettings()
    ini.parse(Vfs(owner_install).read("Root/Bionicle.ini"))
    assert ini.cheat_menu() is False                                  # [Misc] has no CheatMenu (hud.md §6.2)
    assert ini.bad_cheat() is True                                    # [Cheat] Urinate=Pohatu: the gate is true (F12/F13)
    assert ini.cheat() is False and ini.cheat_debug() is False
    assert ini.get_fog() is True
    assert ini.resolution() == (640.0, 480.0)
    assert ini.language == 0 and ini.get_int(b"GraphicsOptions", b"Gamma", 256) == 255


# --------------------------------------------------------------------------------------------- the natives
def _vm(asm, install=None, ini_text=None):
    from osi_natives import framework, directors, services
    import osiasm
    image, _ = osiasm.assemble(asm)
    tr = osi_vm.ListTrace()
    heap = osi_vm.Heap(tr)
    types = osi_vm.TypeTable.load(REPO / "data" / "osi" / "types.json")
    reg = osi_vm.NativeRegistry.load(osi_vm.DEFAULT_NATIVES, osi_vm.DEFAULT_MODS, types)
    vm = osi_vm.Vm(image, reg, heap, tr)
    log = []
    vfs = framework.Vfs(install) if install is not None else None
    ctx = framework.HostContext(vm, heap, vfs, framework.HostFlags(deterministic=True, skip_video=True), log)
    if ini_text is not None:
        from osi_natives.ini import IniSettings
        ctx.subsystem(IniSettings).parse(ini_text)
    directors.bind(reg, ctx)
    services.bind(reg, ctx)
    return vm, ctx


def _call(native, **kw):
    vm, ctx = _vm(".function main 0\nCallGameFunction %s 0\nReturn\n" % native, **kw)
    r = vm.call("main", [])
    assert r.ok
    return r.result


def test_cheatmenu_native_reads_the_ini():
    assert _call("gcsaver.cheatmenu", ini_text=b"[Misc]\nCheatMenu=LEGOTester\n") == Variant(0x10, 1)
    assert _call("gcsaver.cheatmenu", ini_text=b"[misc]\ncheatmenu=\"LEGOTester\"\n") == Variant(0x10, 1)   # unquoted
    assert _call("gcsaver.cheatmenu", ini_text=b"[Misc]\nCheatMenu=legotester\n") == Variant(0x10, 0)      # exact compare
    assert _call("gcsaver.cheatmenu", ini_text=b"[Misc]\nCheatMenu=LEGOTester ; x\n") == Variant(0x10, 0)  # no inline comment


def test_owner_ini_natives_through_the_vfs(owner_install):
    assert _call("gcsaver.cheatmenu", install=owner_install) == Variant(0x10, 0)
    assert _call("gcgame.getresolutionwidth", install=owner_install) == Variant(0x12, osi_vm.f32_bits(640.0))
    assert _call("gcgame.getresolutionheight", install=owner_install) == Variant(0x12, osi_vm.f32_bits(480.0))


def test_resolution_natives_follow_the_ini():
    t = b"[GraphicsOptions]\nResolutionW=1280\nResolutionH=720\n"
    assert _call("gcgame.getresolutionwidth", ini_text=t) == Variant(0x12, osi_vm.f32_bits(1280.0))
    assert _call("gcgame.getresolutionheight", ini_text=t) == Variant(0x12, osi_vm.f32_bits(720.0))
    assert _call("gcgame.getresolutionwidth") == Variant(0x12, osi_vm.f32_bits(640.0))     # no INI: the cited default
    # the screen stores u16: 70000 -> 4464
    assert _call("gcgame.getresolutionwidth", ini_text=b"[GraphicsOptions]\nResolutionW=70000\n") == \
        Variant(0x12, osi_vm.f32_bits(4464.0))


def test_notes_no_longer_defer_the_ini():
    import json
    rows = json.loads((REPO / "data/osi/native_overrides.json").read_text())
    by = {r["key"]: r for r in rows}
    for k in ("gcsaver.cheatmenu", "gcgame.getresolutionwidth", "gcgame.getresolutionheight"):
        assert "ini -> M2a-3" not in by[k]["note"], k


if __name__ == "__main__":
    print("\n".join(render_table()))
