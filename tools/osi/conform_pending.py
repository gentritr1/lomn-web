"""The "pending" marker of a conformance program (docs/osi/conformance.md "Pending programs"), shared by the oracle
(tools/research/osi_vm.py --conform), tools/osi/conform_assemble.py and tools/osi/conformance_report.py. The C++ runner
(engine/tools/lomn_osi_conform.cpp) applies the same rule: only a top-level `"pending"` that is a NON-EMPTY STRING counts
(the task id, e.g. "T3"). Any other value (absent, "", null, a number, a list) leaves the program an ordinary one, so a typo
runs the program and can fail; it never skips it silently."""
from __future__ import annotations

import json
from pathlib import Path


def pending_of(spec) -> str | None:
    """The task id of a parsed expect.json when it is pending, else None."""
    p = spec.get("pending") if isinstance(spec, dict) else None
    return p if isinstance(p, str) and p else None


def pending_task(expect_path) -> str | None:
    """The same for a file; an unreadable or invalid file is not pending (the runner reports it as it always did)."""
    try:
        return pending_of(json.loads(Path(expect_path).read_text()))
    except (OSError, ValueError):
        return None
