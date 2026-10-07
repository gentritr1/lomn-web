"""Mutation smoke: each mutation must make lomn_tests fail. Builds a scratch copy of engine/ per mutation."""
import os, shutil, subprocess, sys
W = sys.argv[1]
M = [
 ("route: spec-table order (0x8 first)", "src/sage/slb.cpp", "Route route(uint32_t flags) {\n", "Route route(uint32_t flags) {\n  if (flags & 0x08) return Route::Back;\n"),
 ("exists: load order instead of Art first", "src/sage/blockchain.cpp", "Lookup BlockChain::find_exists(std::string_view request) const {\n", "Lookup BlockChain::find_exists(std::string_view request) const {\n  return find_load(request);\n"),
 ("bc1: bit-replicated endpoints", "src/sage/image.cpp", "    pal[0][k] = unorm8(a[k], maxv[k]);", "    pal[0][k] = static_cast<uint8_t>(maxv[k] == 63 ? (a[k] << 2 | a[k] >> 4) : (a[k] << 3 | a[k] >> 2));"),
 ("strcmp: fold 0x40..0x5f", "src/sage/blockfile.cpp", "return (c >= 0x41 && c <= 0x5a)", "return (c >= 0x40 && c <= 0x5f)"),
 ("slb: slot offset not masked", "src/sage/slb.cpp", "uint32_t field = le32(bytes_.data() + m - 4 - 4 * k) & ~3u;", "uint32_t field = le32(bytes_.data() + m - 4 - 4 * k);"),
 ("blk: no StrCmp overrun for 40-byte names", "src/sage/blockfile.cpp", "    if (!be.name_terminated)  // StrCmp", "    if (false)  // StrCmp"),
 ("x: no vertex split", "src/sage/xfile.cpp", "      if (assigned[p] == UINT32_MAX || assigned[p] == n) {", "      if (true) {"),
 ("x: depth cap off by one", "src/sage/xfile.cpp", "if (depth > kXMaxDepth) return bad(\"nesting deeper than 64\");", "if (depth > kXMaxDepth + 1) return bad(\"nesting deeper than 64\");"),
 ("lzss: no output cap", "src/sage/blockfile.cpp", "      if (len > out_size - op) return fail(err, \"lzss: output overrun\");", "      if (op + len > out_size) { op += len; break; }"),
 ("tga: RLE run not bounded", "src/sage/image.cpp", "      if (run > npix - pix) return fail(err, \"tga: RLE packet runs past the image\");", "      if (run > npix - pix) run = npix - pix;"),
]
env = dict(os.environ, PATH="/opt/homebrew/bin:" + os.environ["PATH"])
ok = True
for name, f, old, new in M[int(sys.argv[2]) if len(sys.argv) > 2 else 0:]:
    d = os.path.join(W, "scratch", "mut")
    shutil.rmtree(d, ignore_errors=True)
    shutil.copytree(os.path.join(W, "engine"), os.path.join(d, "engine"))
    os.symlink(os.path.join(W, ".toolchain"), os.path.join(d, ".toolchain"))
    p = os.path.join(d, "engine", f)
    s = open(p).read()
    assert old in s, name
    open(p, "w").write(s.replace(old, new, 1))
    b = os.path.join(d, "build")
    r = subprocess.run(f'cmake -S engine -B build -G Ninja -DLOMN_WEBGPU=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null && ninja -C build lomn_tests >/dev/null && ./build/lomn_tests', shell=True, cwd=d, env=env, capture_output=True, text=True, errors="replace")
    failed = r.returncode != 0 and "test cases" in r.stdout
    summary = [l for l in r.stdout.splitlines() if "test cases:" in l]
    print(("CAUGHT " if failed else "MISSED ") + name + "  " + (summary[0] if summary else r.stderr[-200:]), flush=True)
    ok &= failed
sys.exit(0 if ok else 1)
