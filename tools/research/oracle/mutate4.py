"""M1b (REF4) mutation bite check: each deliberate defect in the new logic must make at least one named check fail.
Run ONLY in a scratch copy of the tree (it edits sources in place and restores them):

    python3 tools/research/oracle/mutate4.py <scratch_tree> [--only N,M] [--skip-browser]

<scratch_tree> needs build/native (Release, LOMN_WEBGPU=ON) and build/wasm already built, the toolchain symlinks
(.toolchain, web/node_modules) and PATH with node >= 23 and emsdk. Checks:
  unit     build/native/lomn_tests (doctest; includes the M1b null-backend, prefetch, fly-camera cases)
  gpu      build/native/lomn_gpu_destroy_test (Dawn)
  node     node --test web/tests/{unit,unit_r2,network_check,lib_wait,a13_checks}_test.mjs
  browser  node web/tests/byo_synthetic.mjs (Playwright, real GPU) after rebuilding lomn_web when engine code changed
A mutation is CAUGHT when a listed check fails (or the build fails), MISSED otherwise. The no-op control must be MISSED.
"""
from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

M = [
    # (id, file, old, new, checks)
    ("prefetch: containment rejects a read ending at the range end", "engine/src/fs/prefetch_fs.cpp",
     "offset - r->offset <= r->length - length) return r.get();", "offset - r->offset < r->length - length) return r.get();", ["unit"]),
    ("prefetch: a hit does not stamp the touch generation", "engine/src/fs/prefetch_fs.cpp",
     "  r->touched_gen = generation_;\n  const uint8_t* first", "  const uint8_t* first", ["unit"]),
    ("prefetch: release_untouched frees pinned (boot) ranges", "engine/src/fs/prefetch_fs.cpp",
     "if (r.committed && !r.pinned && r.touched_gen != generation_) {", "if (r.committed && r.touched_gen != generation_) {", ["unit"]),
    ("prefetch: misses are not deduplicated", "engine/src/fs/prefetch_fs.cpp",
     "  if (miss_set_.count(b)) return;\n", "", ["unit"]),
    ("prefetch: reserve accepts a range past the end of the file", "engine/src/fs/prefetch_fs.cpp",
     "if (offset > f.size || length > f.size - offset) return set_err(err, \"range outside the file: \" + n), nullptr;",
     "if (offset > f.size) return set_err(err, \"range outside the file: \" + n), nullptr;", ["unit"]),
    ("prefetch: an empty read is a miss", "engine/src/fs/prefetch_fs.cpp",
     "  if (length == 0) {  // nothing to move", "  if (length == 0 && false) {  // nothing to move", ["unit"]),
    ("rhi registry: a texture used by a live bind group may be destroyed", "engine/src/rhi/registry.h",
     "    if (BindGroupId g = bind_group_using_texture(t)) return \"in use by bind group \" + std::to_string(g);\n", "", ["unit", "gpu"]),
    ("rhi null: destroy does not remove the object", "engine/src/rhi/null_device.cpp",
     "    table.erase(id);\n    rec(\"%s %u\", what, id);", "    rec(\"%s %u\", what, id);", ["unit"]),
    ("rhi webgpu: destroy_bind_group leaves the registry entry", "engine/src/rhi/webgpu_device.cpp",
     "    reg_.bind_groups.erase(g);\n    return true;", "    return true;", ["gpu"]),
    ("area: release destroys textures/samplers before the bind groups", "engine/src/view/area.cpp",
     "    for (const auto& kv : impl_->groups) count(dev.destroy_bind_group(kv.second));\n    for (const auto& kv : impl_->pipelines) count(dev.destroy_pipeline(kv.second));\n    for (const auto& kv : impl_->samplers) count(dev.destroy_sampler(kv.second));\n  }\n  for (rhi::TextureId t : created_textures_) count(dev.destroy_texture(t));",
     "    for (const auto& kv : impl_->pipelines) count(dev.destroy_pipeline(kv.second));\n    for (const auto& kv : impl_->samplers) count(dev.destroy_sampler(kv.second));\n  }\n  for (rhi::TextureId t : created_textures_) count(dev.destroy_texture(t));\n  if (impl_)\n    for (const auto& kv : impl_->groups) count(dev.destroy_bind_group(kv.second));",
     ["unit", "gpu"]),
    ("area: upload does not record created textures (release leaks them)", "engine/src/view/area.cpp",
     "    created_textures_.push_back(id);  // M1b", "    // created_textures_.push_back(id);  // M1b", ["unit", "gpu"]),
    ("fly camera: forward moves backwards", "engine/src/view/fly_camera.cpp",
     "const float f = (in.forward ? 1.0f : 0.0f) - (in.back ? 1.0f : 0.0f);", "const float f = (in.back ? 1.0f : 0.0f) - (in.forward ? 1.0f : 0.0f);", ["unit"]),
    ("list_areas: the level blockfile is listed as an area", "engine/src/view/prefetch.cpp",
     "if (stem == lev || lev.empty()", "if (lev.empty()", ["unit"]),
    ("js fingerprint: blockfiles not sorted", "web/app/fingerprint.js",
     "    .sort((a, b) => compareBytes(a.bytes, b.bytes));", "    ;", ["node"]),
    ("js fingerprint: table length without the 12-byte header", "web/app/fingerprint.js",
     "const len = BLK_HEADER + count * BLK_ENTRY;", "const len = count * BLK_ENTRY;", ["node"]),
    ("js classify: accepted list checked before the rejected list", "web/app/fingerprint.js",
     "  const rej = builds.rejected.find((r) => r.exe_sha256 === exeSha);", "  const rej = builds.accepted.length < 0 ? null : null;", ["node"]),
    ("js byo: DLLs are copied / indexed", "web/app/byo.js",
     "return !(k.endsWith('.exe') || k.endsWith('.dll'));", "return !k.endsWith('.exe');", ["node"]),
    ("js engine: negative range offsets accepted", "web/app/engine.js",
     "|| offset < 0 || length < 0)", ")", ["node"]),
    ("js engine: chunk written one byte late", "web/app/engine.js",
     "this.M.HEAPU8.set(buf, ptr + pos);", "this.M.HEAPU8.set(buf.subarray(1), ptr + pos + 1);", ["browser"]),
    ("js byo: quota check always passes", "web/app/byo.js",
     "return { ok: free >= needBytes,", "return { ok: true,", ["browser"]),
    ("webgpu: the surface depth buffer is never attached", "engine/src/rhi/webgpu_device.cpp",
     "WGPUTextureView depth_view = tgt ? tgt->depth_view : surface_depth_view_;", "WGPUTextureView depth_view = tgt ? tgt->depth_view : nullptr;",
     ["browser"]),
    ("main_web: area switch never releases the previous area's ranges", "engine/src/app/main_web.cpp",
     "  s.fs_released = s.fs.release_untouched();", "  s.fs_released = 0;", ["browser"]),
    # ---- REF4 revision 2 ----
    ("C1 main_web: a lost device does not reach the page state", "engine/src/app/main_web.cpp",
     "      emscripten_cancel_main_loop();\n      set_state(\"error\", why);", "      emscripten_cancel_main_loop();", ["browser"]),
    ("C1 webgpu: the first uncaptured error is not forwarded", "engine/src/rhi/webgpu_device.cpp",
     "      static_cast<LostSink*>(u1)->notify(std::string(\"GPU error (\") + error_type_name(type) + \"): \" + str(m));\n", "", ["gpu"]),
    ("C2 classify: accepted exe + unknown tables hard-rejected again", "web/app/fingerprint.js",
     "return { ok: true, build: acc.id, data: 'unknown', warning,", "return { ok: false, build: acc.id, data: 'unknown', warning,", ["node"]),
    ("C2 page: the soft-accept warning is not shown", "web/app/main.js",
     "    $('warning').hidden = false;\n", "", ["browser"]),
    ("I1 webgpu: textures released but never destroyed (left to GC)", "engine/src/rhi/webgpu_device.cpp",
     # (anchor re-pointed after 4b4ad7b, T8 P5: the Destroy lives in gpu_destroy_and_release; "released, never destroyed" = Release only)
     "    if (frame_open_) return deferred_textures_.push_back(t);\n    gpu_destroy_and_release(t);\n", "    if (frame_open_) return deferred_textures_.push_back(t);\n    wgpuTextureRelease(t);\n", ["browser"]),
    ("I1 webgpu: Destroy not deferred inside an open frame", "engine/src/rhi/webgpu_device.cpp",
     "    if (frame_open_) return deferred_textures_.push_back(t);\n", "", ["gpu"]),
    ("I2 main_web: expectation from the snapped p50 again (circular)", "engine/src/app/main_web.cpp",
     "fps < 0.95 * s.display_hz;", "fps < 0.95 * nominal_hz(p50);", ["browser"]),
    ("I3 page: a moved remembered folder is not caught", "web/app/main.js",
     "        try {\n          src = await handleSource(handle, 'handle');\n        } catch (err) {",
     "        try {\n          src = await handleSource(handle, 'handle');\n        } finally {}\n        if (false) { const err = null;", ["browser"]),
    ("I4 page: no Content-Security-Policy", "web/index.html",
     "<meta http-equiv=\"Content-Security-Policy\"", "<meta name=\"x-no-csp\"", ["browser"]),
    ("I6 byo: the copy is never compared with its manifest", "web/app/byo.js",
     "  const have = new Map(entries.map((e) => [lowerAscii(e.path), e.size]));\n", "  const have = new Map(entries.map((e) => [lowerAscii(e.path), e.size]));\n  return '';\n", ["node", "browser"]),
    ("M3 page: keys move the camera without canvas focus", "web/app/main.js",
     "const ours = () => document.activeElement === c || document.pointerLockElement === c;", "const ours = () => true;", ["browser"]),
    # ---- M1 close-out (task 10) ----
    ("C-N1 page: a failed area read is final (fail) instead of a recoverable areaError", "web/app/main.js",
     "    if (!dead) areaError(err, name);", "    if (!dead) fail(err);", ["browser"]),
    ("C-N2 network check: the per-path fetch limit is ignored", "web/tests/lib.mjs",
     "    .filter((r) => r.count > r.max);", "    .filter((r) => false);", ["node"]),
    ("C-N4 page: the requestAdapter() pre-check is removed", "web/app/main.js",
     "      if (!(await navigator.gpu.requestAdapter())) adapterWhy = 'it returned no graphics adapter';\n", "", ["browser"]),
    ("C-m1 webgpu: the surface depth texture is never explicitly destroyed", "engine/src/rhi/webgpu_device.cpp",
     "    wgpuTextureDestroy(t);\n    ++gpu_destroys_;\n    if (surface_depth) --undestroyed_depth_;",
     "    if (!surface_depth) wgpuTextureDestroy(t);\n    ++gpu_destroys_;\n    if (surface_depth) --undestroyed_depth_;", ["browser"]),
    ("no-op control (comment only; must be MISSED)", "engine/src/fs/prefetch_fs.cpp",
     "// outside the file: a caller error, not a miss", "// outside the file: a caller error (not a miss)", ["unit", "node"]),
]


LOGDIR = None


# A browser run counts as CAUGHT only if a check this mutation targets failed (the run has ~70 checks; an unrelated
# failure, e.g. a timing check on a loaded host, must not pass for a bite). Keyed by the mutation's name.
EXPECT = {
    "js engine: chunk written one byte late": r"OPFS path: copied",
    "js byo: quota check always passes": r"quota",
    "webgpu: the surface depth buffer is never attached": r"pixels",
    "main_web: area switch never releases the previous area's ranges": r"own read set",
    "C1 main_web: a lost device does not reach the page state": r"C1",
    "C2 page: the soft-accept warning is not shown": r"C2",
    "I1 webgpu: textures released but never destroyed (left to GC)": r"I1",
    # T8 P17: only the two spin checks ("I2: a N ms spin per frame -> ... DROPPING", "I2: ?debug=1&debug_spin=N makes DROPPING
    # fire"). A bare r"I2" also matched the load-sensitive "every unspun page ... NOT DROPPING" check, so under load this
    # mutation could be scored CAUGHT with no spin check failing (tools/tests/test_mutate4_expect.py).
    "I2 main_web: expectation from the snapped p50 again (circular)": r"I2: (a \d+ ms spin per frame|\?debug=1&debug_spin=\d+ makes DROPPING)",
    "I3 page: a moved remembered folder is not caught": r"I3",
    "I4 page: no Content-Security-Policy": r"CSP",
    "I6 byo: the copy is never compared with its manifest": r"I6",
    "M3 page: keys move the camera without canvas focus": r"M3",
    # M1 close-out: the sections' own check labels (byo_synthetic.mjs). C-N2 is a node check (network_check_test.mjs positive controls).
    "C-N1 page: a failed area read is final (fail) instead of a recoverable areaError": r"C-N1",
    "C-N4 page: the requestAdapter() pre-check is removed": r"C-N4",
    "C-m1 webgpu: the surface depth texture is never explicitly destroyed": r"C-m1",
}


def run(cmd, cwd, timeout, log_name=None):
    t0 = time.time()
    try:
        p = subprocess.run(cmd, cwd=cwd, shell=True, capture_output=True, text=True, timeout=timeout)
        out = p.stdout + p.stderr
        if LOGDIR and log_name:
            (LOGDIR / log_name).write_text(out)
        # Every FAIL line (a browser run prints many ok lines after the first failure, so the tail is not enough).
        fails = [l for l in out.splitlines() if "FAIL" in l or "ERROR" in l or "not ok" in l]
        # ALL of them: a mutation that breaks several checks must not lose its first (targeted) FAIL line to a length
        # cap (#23 was scored MISSED that way, 2026-10-08). Callers shorten only what they print.
        return p.returncode, "\n".join(fails) or out[-1500:], time.time() - t0
    except subprocess.TimeoutExpired:
        return 124, "timeout", time.time() - t0


def main() -> int:
    tree = Path(sys.argv[1]).resolve()
    only = None
    if "--only" in sys.argv:
        only = {int(x) for x in sys.argv[sys.argv.index("--only") + 1].split(",")}
    skip_browser = "--skip-browser" in sys.argv
    global LOGDIR
    LOGDIR = tree / "build" / "mutlogs"  # one full log per (mutation, check): what made it CAUGHT
    LOGDIR.mkdir(parents=True, exist_ok=True)
    caught = missed = 0
    for i, (name, rel, old, new, checks) in enumerate(M, 1):
        if only and i not in only:
            continue
        if skip_browser and checks == ["browser"]:
            print(f"[{i}] SKIPPED (browser) {name}", flush=True)
            continue
        f = tree / rel
        src = f.read_text()
        if src.count(old) != 1:
            print(f"[{i}] ERROR pattern not found exactly once in {rel}: {name}", flush=True)
            return 2
        f.write_text(src.replace(old, new))
        verdict, why = "MISSED", ""
        try:
            engine_change = rel.startswith("engine/")
            if engine_change and any(c in ("unit", "gpu") for c in checks):
                rc, out, _ = run("timeout 900 cmake --build build/native -j4", tree, 960)
                if rc:
                    verdict, why = "CAUGHT", "native build failed"
            if engine_change and "browser" in checks and verdict == "MISSED":
                rc, out, _ = run("timeout 900 cmake --build build/wasm -j4", tree, 960)
                if rc:
                    verdict, why = "CAUGHT", "wasm build failed"
            for c in checks:
                if verdict == "CAUGHT":
                    break
                cmd = {
                    "unit": "timeout 300 build/native/lomn_tests",
                    "gpu": "timeout 120 build/native/lomn_gpu_destroy_test",
                    "node": "cd web && timeout 60 node --test tests/unit_test.mjs tests/unit_r2_test.mjs tests/network_check_test.mjs tests/lib_wait_test.mjs tests/a13_checks_test.mjs",
                    "browser": "cd web && timeout 600 node tests/byo_synthetic.mjs ../build/wasm ../build/mut-byo ../build/native/lomn_view",
                }[c]
                rc, out, secs = run(cmd, tree, 900, f"{i:02d}-{c}.log")
                if rc != 0:
                    fails = [l.strip() for l in out.splitlines() if "FAIL" in l or "ERROR" in l or "not ok" in l]
                    want = EXPECT.get(name) if c == "browser" else None
                    if want and not any(__import__("re").search(want, l) for l in fails):
                        why = f"{c} rc={rc} but not on a targeted check ({want}): " + " | ".join(fails)[:300]
                        continue  # not a bite: try the next check, else MISSED
                    verdict, why = "CAUGHT", f"{c} rc={rc} ({secs:.0f} s): " + " | ".join(fails)[:300]
        finally:
            f.write_text(src)
            # Rebuild from the restored source at once, so no later (web-only) mutation runs on a mutated build.
            if rel.startswith("engine/"):
                run("timeout 900 cmake --build build/native -j4", tree, 960)
                if "browser" in checks:
                    run("timeout 900 cmake --build build/wasm -j4", tree, 960)
        if verdict == "CAUGHT":
            caught += 1
        else:
            missed += 1
        print(f"[{i}] {verdict} {name}{' :: ' + why if why else ''}", flush=True)
    # Restore the builds to the unmutated sources.
    rc1, _, _ = run("timeout 900 cmake --build build/native -j4", tree, 960)
    rc2, _, _ = run("timeout 900 cmake --build build/wasm -j4", tree, 960)
    rc3, out, _ = run("timeout 300 build/native/lomn_tests", tree, 400)
    print(f"restored: native build rc={rc1}, wasm build rc={rc2}, lomn_tests rc={rc3}")
    print(f"mutations: {caught} CAUGHT, {missed} MISSED (the no-op control must be among the MISSED)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
