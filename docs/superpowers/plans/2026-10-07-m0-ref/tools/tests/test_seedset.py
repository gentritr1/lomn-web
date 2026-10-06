from seedset import compute

FUNCS = [{"addr": a, "name": n} for a, n in [("0x1", "GcAreaLoader::LoadArea"), ("0x2", "ScBlockFile::Read"),
                                             ("0x3", "helper"), ("0x4", "ScBlockFile::Unused"),
                                             ("0x5", "Render::Draw"), ("0x6", "unrelated")]]
CALLS = [{"from": "0x1", "to": ["0x2"]}, {"from": "0x2", "to": ["0x3"]}, {"from": "0x5", "to": []}, {"from": "0x6", "to": ["0x1"]}]


def test_reachability_and_candidates():
    r = compute(FUNCS, CALLS, [{"addr": "0x5", "iface": "IDirect3DDevice8", "method": "DrawPrimitive"}], [r"^GcAreaLoader::"])
    assert r["roots"] == ["0x1"]
    assert r["render_sites"] == ["0x5"]
    assert r["m1-required"] == ["0x1", "0x2", "0x3", "0x5"]
    assert r["m1-candidate"] == ["0x4"]          # same class as a required function, not called directly
