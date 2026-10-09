"""M2a-2 T1: the A8 coverage report. Which natives did a run hit, by tier, and in what order.

  coverage.py --trace T | --log L [--overrides F] [--natives P] [--mods P]

Input is one of
  --trace  a trace (docs/osi/trace.md section 2): `N` lines are native calls, `K` lines start a frame, an `E` line with
           the message "Call to unregistered Game Function" is an unregistered call;
  --log    a natives.log (trace.md section 6): v1 lines `%08d <cls>.<fn> %d -> ...`; v2 adds `K %6d` frame lines,
           which this tool reads (and skips its `C` records). A v1 log has no frame information, so every call is
           attributed to frame -1.
Frame -1 is Setup (everything before the first `K`). The registry is data/osi/natives.json (winners only; every row is
tier `stub` unless an override says otherwise) plus data/osi/natives_beta150.json (tier `mod`), merged the way the
oracle does it (the first registration of a key wins). `--overrides` (default data/osi/native_overrides.json when it
exists) maps a key to a tier: a list of rows with `key`, or a dict keyed by native key.

Output, byte for byte (A8):
  coverage: natives distinct %d (tier1 %d, tier2 %d, stub %d, mod %d, tier3 %d), calls %d, unregistered %d
  native %-48s tier %-5s first %6d calls %6d body %s
one `native` row per distinct registered native, sorted by (first-hit frame, first-hit sequence). `calls` counts every
native call seen; `unregistered` counts calls to a key that is not in the registry (an `N` line or the unregistered-
function `E` line). The five tier counts sum to `distinct`.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DATA = REPO / "data" / "osi"
TIERS = ("tier1", "tier2", "stub", "mod", "tier3")
TIER_ALIASES = {"1": "tier1", "tier1": "tier1", "implemented": "tier1", "2": "tier2", "tier2": "tier2",
                "3": "tier3", "tier3": "tier3", "stub": "stub", "mod": "mod"}
UNREGISTERED_MSG = "Call to unregistered Game Function"


def norm_tier(value, where: str = "") -> str:
    t = TIER_ALIASES.get(str(value).lower())
    if t is None:
        raise ValueError("unknown tier %r%s" % (value, (" for " + where) if where else ""))
    return t


def load_registry(natives, mods=None, overrides=None) -> dict[str, dict]:
    """{key: {'tier': .., 'body': ..}}. natives.json winners first, then the mod rows; the first registration of a key wins
    (the oracle's `NativeRegistry.add`). An override replaces the tier."""
    reg: dict[str, dict] = {}
    for path, default_tier in ((natives, "stub"), (mods, "mod")):
        if path is None:
            continue
        for row in json.loads(Path(path).read_text()):
            if not row.get("winner", True):
                continue
            key = row.get("key") or (row["cls"] + "." + row["name"]).lower()
            reg.setdefault(key, {"tier": norm_tier(row.get("tier", default_tier), key), "body": row.get("body", "-")})
    if overrides is not None:
        data = json.loads(Path(overrides).read_text())
        rows = [dict(v, key=k) for k, v in data.items()] if isinstance(data, dict) else data
        for row in rows:
            key = row["key"]
            if key not in reg:
                print("coverage.py: override for unknown native %r ignored" % key, file=sys.stderr)
                continue
            reg[key]["tier"] = norm_tier(row["tier"], key)
    return reg


_ESC = re.compile(r"\\x([0-9a-f]{2})")


def _key(token: str) -> str:
    """A trace/log name token (trace.md section 1) -> the registry key (lower case)."""
    return _ESC.sub(lambda m: chr(int(m.group(1), 16)), token).replace("\\\\", "\\").lower()


def parse_trace(lines) -> tuple[list[tuple[int, int, str]], int]:
    """[(frame, seq, key)] in call order, and the count of unregistered-function errors. seq is the order of the N line."""
    events, unreg, frame = [], 0, -1
    for ln in lines:
        c = ln[:2]
        if c == "N ":
            events.append((frame, len(events), _key(ln.split(" ", 3)[2])))
        elif c == "K ":
            frame = int(ln.split()[1])
        elif c == "E " and UNREGISTERED_MSG in ln:
            unreg += 1
    return events, unreg


def parse_log(lines) -> tuple[list[tuple[int, int, str]], int]:
    """natives.log v1/v2: a call line starts with its `%08d` sequence number; `K` lines set the frame; `C` records are skipped."""
    events, frame = [], -1
    for ln in lines:
        if not ln:
            continue
        if ln[0] == "K":
            frame = int(ln.split()[1])
        elif ln[0].isdigit():
            tok = ln.split(" ", 2)
            events.append((frame, int(tok[0]), _key(tok[1])))
    return events, 0


def report(events, unreg: int, reg: dict[str, dict]) -> list[str]:
    first: dict[str, tuple[int, int]] = {}
    calls: dict[str, int] = {}
    unregistered = unreg
    for frame, seq, key in events:
        if key not in reg:
            unregistered += 1
            continue
        first.setdefault(key, (frame, seq))
        calls[key] = calls.get(key, 0) + 1
    counts = {t: 0 for t in TIERS}
    for key in first:
        counts[reg[key]["tier"]] += 1
    head = "coverage: natives distinct %d (tier1 %d, tier2 %d, stub %d, mod %d, tier3 %d), calls %d, unregistered %d" % (
        len(first), counts["tier1"], counts["tier2"], counts["stub"], counts["mod"], counts["tier3"], len(events),
        unregistered)
    rows = [head]
    for key in sorted(first, key=lambda k: first[k]):
        rows.append("native %-48s tier %-5s first %6d calls %6d body %s" % (
            key, reg[key]["tier"], first[key][0], calls[key], reg[key]["body"]))
    return rows


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--trace")
    g.add_argument("--log")
    ap.add_argument("--overrides", help="default: data/osi/native_overrides.json if it exists")
    ap.add_argument("--natives", default=str(DATA / "natives.json"))
    ap.add_argument("--mods", default=str(DATA / "natives_beta150.json"))
    a = ap.parse_args(argv)
    overrides = a.overrides
    if overrides is None and (DATA / "native_overrides.json").exists():
        overrides = str(DATA / "native_overrides.json")
    reg = load_registry(a.natives, a.mods, overrides)
    path, parse = (a.trace, parse_trace) if a.trace else (a.log, parse_log)
    with open(path, encoding="ascii", errors="replace") as f:
        events, unreg = parse(l.rstrip("\n") for l in f)
    print("\n".join(report(events, unreg, reg)))
    if a.log and events and all(e[0] == -1 for e in events):
        print("coverage.py: this log has no K lines (natives.log v1): every call is attributed to frame -1; "
              "use --trace for first-hit frames", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
