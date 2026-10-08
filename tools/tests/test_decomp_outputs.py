"""Acceptance checks over a real `make decomp` run (skipped until it exists). Floors measured 2026-10-07 (two runs,
identical output) after the decompiler payload fix that makes GcLegoCamera::Process decompile; re-pinned 2026-10-08
from gen/decomp/metrics.json after the M2a-0 seeded run (1,448 seeds created, 63 CreateVariant sret signatures)."""
import json


def test_metrics_floors(gen_decomp):
    m = json.loads((gen_decomp / "metrics.json").read_text())
    assert m["functions"] == 6940              # was 5618 before M2a-0 seeding
    assert m["functions_named"] >= 5429        # unchanged: every seeded function is FUN_ (no CV public symbol)
    assert m["decompiled_ok"] == 6940          # every function, incl. GcLegoCamera::Process (37 KB); was 5618
    assert m["structs"] == 473 and m["members"] == 1817
    assert m["procs_with_locals"] == 580 and m["named_vars"] == 1265
    assert m["x87_nan_noise"] < m["x87_rewrites"]


def test_ghidra_side_applied(gen_decomp):
    """These come from Ghidra's own script logs and from the decompiled text, so a half-applied analysis fails here."""
    m = json.loads((gen_decomp / "metrics.json").read_text())
    assert m["ghidra_structs"] == 473 and m["ghidra_members_applied"] >= 1814
    assert m["ghidra_labels_applied"] >= 7042
    # 429/551/65-failed before M2a-0 seeding; +151 CV procs at seeded functions (150 natives/initializers + 0x61d6a0),
    # 376 param lines unapplied there (no stack params on natives), 14 locals applied. params_applied == 259 is the
    # guard that the M1 functions are unchanged.
    assert m["ghidra_local_funcs"] == 580 and m["ghidra_params_applied"] == 259 and m["ghidra_locals_applied"] == 565
    assert m["ghidra_d3d8_globals"] == 4
    assert m["ghidra_seeds_created"] == 1448 and m["ghidra_seeds_failed"] == 0
    assert m["ghidra_sret_retyped"] == 63 and m["ghidra_sret_failed"] == 0
    # was >= 45891 / <= 620 / >= 723 before M2a-0 (5,618 functions)
    assert m["member_access"] >= 47792 and m["raw_offset_deref"] <= 574 and m["x87_rewrites"] >= 725
    # was 3649 before M2a-0: the CreateVariant sret fix removes 450 (452 -> 2) despite 1,322 more functions
    assert m["in_stack_artifact"] <= 3209


def test_device_surface(gen_decomp):
    uses = [json.loads(l) for l in (gen_decomp / "vtable_uses.jsonl").read_text().splitlines() if l]
    methods = {u["method"] for u in uses if u["iface"] == "IDirect3DDevice8"}
    assert len(methods) == 30
    assert {"SetRenderState", "SetTransform", "DrawIndexedPrimitive", "SetTextureStageState"} <= methods


def test_seeds_and_corpus(gen_decomp):
    seeds = json.loads((gen_decomp / "seeds.json").read_text())
    assert len(seeds["roots"]) >= 40 and len(seeds["m1-required"]) > len(seeds["roots"])
    fmap = json.loads((gen_decomp / "function_map.json").read_text())
    assert fmap and (gen_decomp / "corpus" / "Character" / "Bosses" / "GfMudBoss.cpp").is_file()
