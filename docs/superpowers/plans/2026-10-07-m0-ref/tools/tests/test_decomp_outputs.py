"""Acceptance checks over a real `make decomp` run (skipped until it exists). Floors are the 2026-10-07 probe values."""
import json


def test_metrics_floors(gen_decomp):
    m = json.loads((gen_decomp / "metrics.json").read_text())
    assert m["functions"] == 5618
    assert m["functions_named"] >= 5429
    assert m["decompiled_ok"] >= 5617
    assert m["structs"] == 473 and m["members"] == 1817
    assert m["procs_with_locals"] == 580 and m["named_vars"] == 1265
    assert m["x87_nan_noise"] < m["x87_rewrites"]


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
