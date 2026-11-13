"""M2a-3 T5b: Bionicle.ini with Win32 `GetPrivateProfileStringA` / `GetPrivateProfileIntA` semantics (the Python oracle).
Written from docs/osi/hud.md §6 and Microsoft's documented API behaviour; never from engine/, never from configparser
(which differs on duplicates, quoting and inline comments).

Contract (docs/superpowers/plans/2026-11-13-lomn-web-m2a3.md, Task 5):
- section and key names match case-insensitively (ASCII folding only);
- the first match in file order wins (a repeated key, or a repeated section holding the same key);
- a line is trimmed of spaces and tabs; a `;` line is a comment; a `;` inside a value is part of the value (no inline
  comments); a line without `=` or with an empty key is ignored; a key before any `[section]` is ignored;
- `[name]` is the text up to the first `]` (not trimmed); a `[` line with no `]` is ignored;
- the value is trimmed, then unquoted when it starts and ends with the same quote (`"` or `'`, length >= 2);
- the result is cut to size - 1 bytes (size 0 gives b""); a missing file, section or key gives the default (also cut);
- the Int form returns the default when the key is missing, else the leading decimal integer of the value (optional `-`,
  0 when there are no digits) as a 32-bit unsigned value.
NOT specified (and so not tested): whether a key absent from the FIRST of two same-named sections is searched in the second
(this reader does search it, "first match in file order").
Citations: GetPrivateProfileStringA [0x771424], GetPrivateProfileIntA [0x77141c]; call sites listed in hud.md §6.2.
"""
from __future__ import annotations

from typing import Optional

M32 = 0xFFFFFFFF


def _fold(s: bytes) -> bytes:
    return bytes(c + 32 if 0x41 <= c <= 0x5A else c for c in s)


def _cut(v: bytes) -> bytes:
    i = v.find(b"\0")
    return v if i < 0 else v[:i]


class IniSettings:
    """The parsed INI: entries (section, key, value) in file order, names folded. `language` is `[0x70b384]` (the .data
    byte, 0 = English); load() takes it from [Misc] Language (GetLanguage 0x5d4c10, hud.md §6.2)."""

    def __init__(self, ctx=None):
        self.entries: list[tuple[bytes, bytes, bytes]] = []
        self.language = 0
        vfs = getattr(ctx, "vfs", None) if ctx is not None else None
        if vfs is not None:
            data = vfs.read("Root/Bionicle.ini")                       # read-only: LoadIni's write branch is never run
            if data is not None:
                self.parse(data)

    def parse(self, text: bytes) -> "IniSettings":
        self.entries = []
        sec: Optional[bytes] = None
        for ln in text.splitlines():
            s = ln.strip(b" \t")
            if not s or s[0:1] == b";":
                continue
            if s[0:1] == b"[":
                j = s.find(b"]")
                if j > 0:
                    sec = _fold(s[1:j])
                continue
            eq = s.find(b"=")
            if sec is None or eq <= 0:
                continue
            key, val = s[:eq].strip(b" \t"), s[eq + 1:].strip(b" \t")
            if not key:
                continue
            if len(val) >= 2 and val[0] == val[-1] and val[0:1] in (b'"', b"'"):
                val = val[1:-1]
            self.entries.append((sec, _fold(key), val))
        self.language = self.get_int(b"Misc", b"Language", 0) & 0xFF
        return self

    @classmethod
    def from_text(cls, text: bytes) -> "IniSettings":
        return cls().parse(text)

    def set(self, section: bytes, key: bytes, value: bytes) -> None:
        """Programmatic override (tests): replaces the first entry of that name, else appends."""
        e = (_fold(section), _fold(key), value)
        for i, (s, k, _) in enumerate(self.entries):
            if (s, k) == e[:2]:
                self.entries[i] = e
                return
        self.entries.append(e)

    def _find(self, section: bytes, key: bytes) -> Optional[bytes]:
        sk = (_fold(section), _fold(key))
        for s, k, v in self.entries:
            if (s, k) == sk:
                return v
        return None

    def get_private_profile_string(self, section: bytes, key: bytes, default: bytes, size: int = 0xFF) -> bytes:
        v = self._find(section, key)
        v = _cut(default if v is None else v)
        return v[:max(size - 1, 0)]

    def get_int(self, section: bytes, key: bytes, default: int) -> int:
        v = self._find(section, key)
        if v is None:
            return default & M32
        v = _cut(v)
        i, neg = 0, False
        if v[:1] == b"-":
            neg, i = True, 1
        j = i
        while j < len(v) and 0x30 <= v[j] <= 0x39:
            j += 1
        n = int(v[i:j]) if j > i else 0
        return (-n if neg else n) & M32

    # --- the readers of hud.md §6.2 (exact string compares: _ScBaseString::compare 0x4f5990 is byte-exact)
    def _is(self, section: bytes, key: bytes, needle: bytes) -> bool:
        return self.get_private_profile_string(section, key, b"", 0xFF) == needle

    def cheat_menu(self) -> bool:                                       # CheatMenu 0x5d4fc0
        return self._is(b"Misc", b"CheatMenu", b"LEGOTester")

    def bad_cheat(self) -> bool:                                        # BadCheat 0x5d4e40: the Urinate gate (§6.3)
        return self._is(b"Cheat", b"Urinate", b"Pohatu")

    def cheat_debug(self) -> bool:                                      # CheatDebug 0x5d4f00
        return self._is(b"Cheat", b"Cheat", b"NoGreenCapes")

    def cheat(self) -> bool:                                            # Cheat 0x5d4d00
        return self._is(b"Cheat", b"Cheat", b"NoGreenCapes") or self._is(b"Misc", b"Cheat", b"LEGOTester")

    def get_fog(self) -> bool:                                          # GetFog 0x5d51c0: low byte == 1
        return self.get_int(b"GraphicsOptions", b"Fog", 1) & 0xFF == 1

    def resolution(self) -> tuple[float, float]:                        # GetScreenData 0x5d5080: u16 W/H (INFERRED link
        return (float(self.get_int(b"GraphicsOptions", b"ResolutionW", 0x280) & 0xFFFF),   # to [[0x70fa64]+0x10], hud.md §6.2)
                float(self.get_int(b"GraphicsOptions", b"ResolutionH", 0x1E0) & 0xFFFF))
