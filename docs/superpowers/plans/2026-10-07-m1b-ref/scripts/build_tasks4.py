"""REF4 task chain: writes <out>/Tn/files/<repo paths> (the full content of every file the task carries, as of the END of
that task) and <out>/Tn.txt (file list, full/partial, what a partial holds). Applied cumulatively over BASE (af5a77c),
T1..T6 reproduce the reference tree exactly (checked here for every multi-task file: the last task's version must equal
the final file).

Usage: python3 build_tasks4.py <final_tree> <out_dir>
Multi-task files are derived BACKWARDS from the final file by exact substring edits (each asserted to match once).
"""
from __future__ import annotations

import shutil
import sys
from pathlib import Path

FINAL = Path(sys.argv[1]).resolve()
OUT = Path(sys.argv[2]).resolve()


def final(rel: str) -> str:
    return (FINAL / rel).read_text()


def edit(text: str, edits: list[tuple[str, str]], what: str) -> str:
    for old, new in edits:
        n = text.count(old)
        if n != 1:
            raise SystemExit(f"{what}: pattern found {n} times: {old[:90]!r}")
        text = text.replace(old, new)
    return text


def cut(text: str, start: str, end: str | None, what: str, keep_end: bool = True) -> str:
    i = text.find(start)
    if i < 0 or text.count(start) != 1:
        raise SystemExit(f"{what}: cut start not unique: {start[:80]!r}")
    if end is None:
        return text[:i]
    j = text.find(end, i)
    if j < 0:
        raise SystemExit(f"{what}: cut end missing: {end[:80]!r}")
    return text[:i] + (text[j:] if keep_end else text[j + len(end):])


# ---------------------------------------------------------------- per-file variants (backwards from the final file)
def cmake(task: int) -> str:
    t = final("engine/CMakeLists.txt")
    if task < 6:
        pass
    if task < 5:  # before T5 (browser viewer): BASE lomn_web target, no INITIAL_MEMORY
        t = cut(t, "# M1b: the browser viewer's initial wasm memory", "\n\nadd_library(lomn_core", "cmake T<5", keep_end=True)
        t = t.replace('option(LOMN_WEBGPU "build the WebGPU backend" ON)\n\n\nadd_library', 'option(LOMN_WEBGPU "build the WebGPU backend" ON)\nenable_testing()\n\nadd_library')
        t = edit(t, [(
            """    # M1b viewer: an ES module factory (web/app/engine.js imports it); INVOKE_RUN=0 so the boot set is prefetched into
    # the heap before main(). INITIAL_MEMORY is set from measurement (REF4 REPORT "Memory budget").
    # Optimisation (REF4 r2 decision D14, amends parent spec §4.6 "-O2 + wasm-opt"): the Release build (-O3; emcc runs
    # wasm-opt at link for both levels) measured against -O2, 4 interleaved reps per arm (web/tests/measure_arms.mjs,
    # loadavg 13-20): load of 4 areas median 613 vs 605 ms (spread 236 / 525 ms), frame CPU p50 0.7 vs 0.7 ms (spread
    # 0.1): indistinguishable; wasm 332,641 vs 344,390 B, the only difference beyond the (zero) null spread: -O3 kept.
    # -sDYNAMIC_EXECUTION=0: no eval/new Function in the glue (the page's CSP allows only 'wasm-unsafe-eval').
    add_executable(lomn_web src/app/main_web.cpp)
    target_link_libraries(lomn_web PRIVATE lomn_area lomn_webgpu)
    target_include_directories(lomn_web PRIVATE src/app)
    # The measured configuration, readable at runtime (lomn_build_info -> window.__lomn.metrics.build and the HUD).
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _lomn_bt)
    string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_lomn_bt}}" _lomn_flags)
    target_compile_definitions(lomn_web PRIVATE "LOMN_BUILD_TYPE=\\"${CMAKE_BUILD_TYPE}\\"" "LOMN_BUILD_FLAGS=\\"${_lomn_flags}\\"")
    target_link_options(lomn_web PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB -sINITIAL_MEMORY=${LOMN_WEB_INITIAL_MEMORY}
                        -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createLomn -sINVOKE_RUN=0 -sENVIRONMENT=web -sDYNAMIC_EXECUTION=0
                        "SHELL:-sEXPORTED_FUNCTIONS=_main,_malloc,_free"
                        "SHELL:-sEXPORTED_RUNTIME_METHODS=callMain,UTF8ToString,stringToNewUTF8,HEAPU8")""",
            """    add_executable(lomn_web src/app/main_web.cpp)
    target_link_libraries(lomn_web PRIVATE lomn_webgpu)
    target_link_options(lomn_web PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB)""")], "cmake T<5")
    if task < 4:  # before T4 (fly camera)
        t = edit(t, [("            src/view/fly_camera.cpp src/view/prefetch.cpp)  # M1b: free-fly camera, prefetch helpers",
                      "            src/view/prefetch.cpp)  # M1b: prefetch helpers"),
                     ("tests/test_rhi_destroy.cpp tests/test_prefetch_fs.cpp tests/test_fly_camera.cpp)  # M1b",
                      "tests/test_rhi_destroy.cpp tests/test_prefetch_fs.cpp)  # M1b")], "cmake T<4")
    if task < 3:  # before T3 (prefetch fs + manifest)
        t = cut(t, "  # REF4 r2 M6: --manifest lists only the areas", "  # C8: a survey that finds no area", "cmake T<3 M6 test", keep_end=True)
        t = edit(t, [("src/fs/memory_fs.cpp src/fs/prefetch_fs.cpp src/rhi/null_device.cpp", "src/fs/memory_fs.cpp src/rhi/null_device.cpp"),
                     ("src/view/synthetic.cpp\n            src/view/prefetch.cpp)  # M1b: prefetch helpers", "src/view/synthetic.cpp)"),
                     ("tests/test_rhi_destroy.cpp tests/test_prefetch_fs.cpp)  # M1b", "tests/test_rhi_destroy.cpp)  # M1b")], "cmake T<3")
    return t


def makefile(task: int) -> str:
    t = final("Makefile")
    if task < 6:  # before T6: no a13-real target, check without it
        t = edit(t, [("datacheck datacheck-wasm fuzz-osi a9-sweep a13-real manifest", "datacheck datacheck-wasm fuzz-osi a9-sweep manifest"),
                     ("""# M1b A13 on the real install (LOMN_DATA; skipped otherwise): OPFS copy of the install into a throwaway Playwright
# profile (removed afterwards), lev1/vllg, lev2/ghut, lev4/crss vs native renders, switches, revisit, handle path, fps.
a13-real: site native
	@if [ -z "$(LOMN_DATA)" ]; then echo "a13-real SKIPPED (no LOMN_DATA)"; exit 0; fi
	cd web && timeout 1800 node tests/a13_real.mjs ../build/site ../build/a13-real ../build/native/lomn_view ../build/wasm

""", ""),
                     ("""	@if [ -n "$(LOMN_DATA)" ]; then $(MAKE) datacheck datacheck-wasm a13-real; else echo "datacheck, a13-real SKIPPED (no LOMN_DATA)"; fi""",
                      """	@if [ -n "$(LOMN_DATA)" ]; then $(MAKE) datacheck datacheck-wasm; else echo "datacheck SKIPPED (no LOMN_DATA)"; fi""")], "make T<6")
    if task < 5:  # before T5 (viewer page): BASE site; test-web = BASE + the node unit tests (T4 = T3 here)
        t = edit(t, [("""# M1b: index.html + web/app/*.js + the wasm build + builds.json, and asset-manifest.json (the network-discipline list).
site: wasm
	timeout 120 node web/tools/build_site.mjs build/wasm build/site""",
                      """site: wasm
	rm -rf build/site && mkdir -p build/site && cp web/index.html build/wasm/lomn_web.js build/wasm/lomn_web.wasm build/site/"""),
                     ("""# M1b: + the data-free BYO/viewer test on a synthetic install (needs the native lomn_view for the fixture and the
# reference renders) and the fingerprint/JS unit tests.
test-web: site native
	cd web && timeout 60 node --test tests/serve_test.mjs tests/unit_test.mjs tests/unit_r2_test.mjs
	cd web && timeout 180 node tests/smoke.mjs ../build/site ../build/pw-profile
	cd web && timeout 600 node tests/byo_synthetic.mjs ../build/wasm ../build/byo-syn ../build/native/lomn_view""",
                      """# M1b T5a: + the BYO validation unit tests (node --test).
test-web: site
	cd web && timeout 60 node --test tests/serve_test.mjs tests/unit_test.mjs tests/unit_r2_test.mjs
	cd web && timeout 180 node tests/smoke.mjs ../build/site ../build/pw-profile""")], "make T<5")
    if task < 5 and task >= 3:
        # T5a carries the unit-test line; T3/T4 have BASE test-web
        pass
    return t


def makefile_t3() -> str:
    t = makefile(4)
    return edit(t, [("""# M1b T5a: + the BYO validation unit tests (node --test).
test-web: site
	cd web && timeout 60 node --test tests/serve_test.mjs tests/unit_test.mjs tests/unit_r2_test.mjs""", """test-web: site
	cd web && timeout 60 node --test tests/serve_test.mjs""")], "make T3")


def area_h(task: int) -> str:
    t = final("engine/include/lomn/view/area.h")
    if task < 3:
        t = edit(t, [("""constexpr float kWorldRotX = -1.5707982f;
// M1b: the two files every area load reads first (GcGame::LoadArtBlockHeader's Art.blk table, the script file whose
// area class supplies fog): one definition, shared by load_area and the browser's boot prefetch (lomn/view/prefetch.h).
constexpr const char* kArtBlkPath = "Root/Data/Art/Art.blk";
constexpr const char* kBaseOsiPath = "Root/Data/Base.osi";""", "constexpr float kWorldRotX = -1.5707982f;")], "area.h T<3")
    return t


def area_cpp(task: int) -> str:
    t = final("engine/src/view/area.cpp")
    if task < 3:
        t = edit(t, [("chain_.open_art(kArtBlkPath, &e)", 'chain_.open_art("Root/Data/Art/Art.blk", &e)'),
                     ("fs_.read(kBaseOsiPath, bytes)", 'fs_.read("Root/Data/Base.osi", bytes)')], "area.cpp T<3")
    return t


def test_rhi_destroy(task: int) -> str:
    t = final("engine/tests/test_rhi_destroy.cpp")
    if task < 2:
        t = cut(t, 'TEST_CASE("AreaRenderer::release: N upload/render/release cycles', None, "test_rhi_destroy T1")
        t = edit(t, [("// live_objects(), the surface calls on a headless device, and AreaRenderer::release (area switching without leaks).",
                      "// live_objects() and the surface calls on a headless device (AreaRenderer::release follows in T2).")], "test_rhi_destroy T1")
    return t


def test_gpu_destroy(task: int) -> str:
    t = final("engine/tests/test_gpu_destroy.cpp")
    if task < 2:
        t = cut(t, "  // 5. AreaRenderer::release cycles on the synthetic A7 scene.\n", "  // 6. (REF4 r2, C1)", "test_gpu_destroy T1")
        t = edit(t, [("""//  5. AreaRenderer::release: the synthetic A7 scene uploaded, rendered, read back and released 8 times on one device:
//     pixels byte-identical every cycle, live counts back to the baseline after every release.
""", ""), ("""  print("area shown", shown);""", "")] if False else [("""//  5. AreaRenderer::release: the synthetic A7 scene uploaded, rendered, read back and released 8 times on one device:
//     pixels byte-identical every cycle, live counts back to the baseline after every release.
""", "")], "test_gpu_destroy T1")
    return t


def unit_test_mjs(task: int) -> str:
    t = final("web/tests/unit_test.mjs")
    if task < 5:
        t = edit(t, [("import { parseRanges } from '../app/engine.js';\n", ""),
                     ("// copy/index file rule, <input webkitdirectory> path handling and the engine's range-list parser.",
                      "// copy/index file rule and <input webkitdirectory> path handling.")], "unit_test T5a")
        t = cut(t, "test('parseRanges: integers only, malformed lines rejected'", None, "unit_test T5a")
        t = t.rstrip("\n") + "\n"
    return t


def unit_r2_mjs(task: int) -> str:
    t = final("web/tests/unit_r2_test.mjs")
    if task < 5:  # engine.js (filterCached) arrives with T5b
        t = cut(t, "test('M2 filterCached:", None, "unit_r2 T5a").rstrip("\n") + "\n"
    return t


# ---------------------------------------------------------------- the tasks
TASKS = [
    ("T1", "rhi: destroy_* (dependency rule; r2 I1: explicit Destroy, deferred to end_frame inside a frame), live_objects, surface depth + resize, r2 C1 set_lost_handler; null logs + Dawn probes 1-4 and 6", [
        ("engine/include/lomn/rhi.h", None, "full"),
        ("engine/src/rhi/registry.h", None, "full"),
        ("engine/src/rhi/null_device.cpp", None, "full"),
        ("engine/src/rhi/webgpu_device.cpp", None, "full"),
        ("engine/tests/test_rhi_destroy.cpp", lambda: test_rhi_destroy(1), "partial: without the AreaRenderer::release cases (T2)"),
        ("engine/tests/test_gpu_destroy.cpp", lambda: test_gpu_destroy(1), "partial: probes 1-4 and 6 (probe 5 = AreaRenderer, T2)"),
        ("engine/CMakeLists.txt", lambda: cmake(1), "partial: lomn_tests += test_rhi_destroy.cpp; gpu_destroy ctest (no lomn_web r2 flags, no M6 ctest)"),
    ]),
    ("T2", "AreaRenderer::release (+ created-object ownership, replay/submit timing); leak checks", [
        ("engine/include/lomn/view/area.h", lambda: area_h(2), "partial: without the T3 path constants"),
        ("engine/src/view/area.cpp", lambda: area_cpp(2), "partial: without the T3 path constants"),
        ("engine/tests/test_rhi_destroy.cpp", None, "full"),
        ("engine/tests/test_gpu_destroy.cpp", None, "full"),
    ]),
    ("T3", "fs range logger, PrefetchFileSystem, boot/record/list helpers, lomn_view --manifest (r2 M6: addressable areas only)", [
        ("engine/include/lomn/fs.h", None, "full"),
        ("engine/src/fs/memory_fs.cpp", None, "full"),
        ("engine/src/fs/disk_fs.cpp", None, "full"),
        ("engine/include/lomn/prefetch_fs.h", None, "full"),
        ("engine/src/fs/prefetch_fs.cpp", None, "full"),
        ("engine/include/lomn/view/prefetch.h", None, "full"),
        ("engine/src/view/prefetch.cpp", None, "full"),
        ("engine/include/lomn/view/area.h", None, "full"),
        ("engine/src/view/area.cpp", None, "full"),
        ("engine/tools/lomn_view.cpp", None, "full"),
        ("engine/tests/test_prefetch_fs.cpp", None, "full"),
        ("engine/CMakeLists.txt", lambda: cmake(3), "partial: + prefetch_fs.cpp, prefetch.cpp, test_prefetch_fs.cpp"),
        ("Makefile", makefile_t3, "partial: + the manifest target"),
    ]),
    ("T4", "free-fly camera", [
        ("engine/include/lomn/view/fly_camera.h", None, "full"),
        ("engine/src/view/fly_camera.cpp", None, "full"),
        ("engine/tests/test_fly_camera.cpp", None, "full"),
        ("engine/CMakeLists.txt", lambda: cmake(4), "partial: + fly_camera.cpp, test_fly_camera.cpp"),
    ]),
    ("T5a", "BYO validation and storage modules (no page yet): fingerprint (r2 C2 soft accept, D13), sources (r2 M11), paths (r2 M12), copy-manifest checks (r2 I6), OPFS worker, IndexedDB, builds.json data entry, oracle", [
        ("web/app/fingerprint.js", None, "full"),
        ("web/app/byo.js", None, "full"),
        ("web/app/opfs_worker.js", None, "full"),
        ("web/app/idb.js", None, "full"),
        ("web/app/paths.js", None, "full"),
        ("web/tools/fingerprint_cli.mjs", None, "full"),
        ("web/tests/unit_r2_test.mjs", lambda: unit_r2_mjs(4), "partial: without the filterCached test (engine.js is T5b)"),
        ("web/tests/unit_test.mjs", lambda: unit_test_mjs(4), "partial: without the parseRanges test (engine.js is T5b)"),
        ("tools/builds.json", None, "full"),
        ("tools/byo_fingerprint.py", None, "full"),
        ("tools/tests/test_byo_fingerprint.py", None, "full"),
        ("Makefile", lambda: makefile(4), "partial: test-web += unit_test.mjs"),
    ]),
    ("T5b", "browser viewer: main_web bridge (r2 C1 loss -> error, I2 display rate + DROPPING), page (r2 I3, I6, I7, M3, M4, M13, CSP I4), engine.js (r2 M2), site builder, smoke + synthetic A13 test (red-first r2 sections)", [
        ("engine/src/app/main_web.cpp", None, "full"),
        ("engine/CMakeLists.txt", None, "full"),
        ("web/index.html", None, "full"),
        ("web/app/main.js", None, "full"),
        ("web/app/engine.js", None, "full"),
        ("web/tools/build_site.mjs", None, "full"),
        ("web/tests/serve.mjs", None, "full"),
        ("web/tests/smoke.mjs", None, "full"),
        ("web/tests/lib.mjs", None, "full"),
        ("web/tests/byo_synthetic.mjs", None, "full"),
        ("web/tests/unit_test.mjs", None, "full"),
        ("web/tests/unit_r2_test.mjs", None, "full"),
        ("Makefile", lambda: makefile(5), "partial: site via build_site.mjs; test-web += byo_synthetic"),
    ]),
    ("T6", "real-data A13 smoke (r2 I5 sweep + rounds cap, I1 GPU-object probe, M14), A/B harness (M10/M1), mutation script, docs", [
        ("web/tests/a13_real.mjs", None, "full"),
        ("web/tests/README.md", None, "full"),
        ("web/tests/measure_arms.mjs", None, "full"),
        ("tools/research/oracle/mutate4.py", None, "full"),
        ("docs/knowledge/web-porting-playbook.md", None, "full"),
        ("docs/knowledge/measurements.md", None, "full"),
        ("Makefile", None, "full"),
    ]),
]


def main() -> None:
    if OUT.exists():
        shutil.rmtree(OUT)
    seen: dict[str, str] = {}
    for name, title, files in TASKS:
        lines = [f"{name}: {title}"]
        for rel, fn, kind in files:
            text = fn() if fn else final(rel)
            dst = OUT / name / "files" / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(text)
            seen[rel] = text
            lines.append(f"  {kind:<8} {rel}" if kind == "full" else f"  partial  {rel}  ({kind.split(': ', 1)[1]})")
        (OUT / f"{name}.txt").write_text("\n".join(lines) + "\n")
    # Every multi-task file ends at its final content.
    bad = [rel for rel, text in seen.items() if text != final(rel)]
    if bad:
        raise SystemExit(f"last task's version differs from the final file: {bad}")
    print(f"tasks: {len(TASKS)} -> {OUT}")


main()
