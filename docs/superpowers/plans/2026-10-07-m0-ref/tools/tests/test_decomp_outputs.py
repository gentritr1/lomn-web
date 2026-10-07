"""Acceptance checks over a real `make decomp` run (skipped until it exists). Floors measured 2026-10-07 (two runs,
identical output) after the decompiler payload fix that makes GcLegoCamera::Process decompile."""
import json


def test_metrics_floors(gen_decomp):
    m = json.loads((gen_decomp / "metrics.json").read_text())
    assert m["functions"] == 5618
    assert m["functions_named"] >= 5429
    assert m["decompiled_ok"] == 5618          # every function, incl. GcLegoCamera::Process (37 KB)
    assert m["structs"] == 473 and m["members"] == 1817
    assert m["procs_with_locals"] == 580 and m["named_vars"] == 1265
    assert m["x87_nan_noise"] < m["x87_rewrites"]


def test_ghidra_side_applied(gen_decomp):
    """These come from Ghidra's own script logs and from the decompiled text, so a half-applied analysis fails here."""
    m = json.loads((gen_decomp / "metrics.json").read_text())
    assert m["ghidra_structs"] == 473 and m["ghidra_members_applied"] >= 1814
    assert m["ghidra_labels_applied"] >= 7042
    assert m["ghidra_local_funcs"] >= 429 and m["ghidra_params_applied"] >= 259 and m["ghidra_locals_applied"] >= 551
    assert m["ghidra_d3d8_globals"] == 4
    assert m["member_access"] >= 45891 and m["raw_offset_deref"] <= 620 and m["x87_rewrites"] >= 723


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
