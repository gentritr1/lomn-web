# Third-party components

| Component | Version (deps.lock) | License | How it is used |
|---|---|---|---|
| Dawn (native WebGPU) | v20261002.154047 | BSD-3-Clause | linked into native builds |
| emdawnwebgpu | v20261002.154047 | BSD-3-Clause | Emscripten port, linked into the browser build |
| Emscripten / emsdk | 6.0.11 | MIT / University of Illinois NCSA | toolchain; runtime JS is part of the browser build |
| doctest | 2.5.3 | MIT | test framework (tests only) |
| stb_image_write | stb@2c980bb | MIT or Public Domain | PNG output in native test tools |
| SDL3 | release-3.4.18 | zlib | pinned for M1 (window/input/audio); not linked in M0 |
| Ghidra | 12.1.4 | Apache-2.0 | offline decompilation tool (not shipped) |
| Playwright | 1.63.0 | Apache-2.0 | browser tests (not shipped) |
| pytest / Pillow / NumPy | 9.0.3 / 12.3.0 / 2.5.3 | MIT / MIT-CMU / BSD-3-Clause | tool tests and image comparison (not shipped) |
| mingw-w64 d3d8.h | commit 2bcf06a | public domain (mingw-w64 headers) | read once to list D3D8 vtable method order (data only) |

Not shipped, never committed: the game's data files and `LEGOBionicle.exe` (LEGO/Saffire). Players provide their own.
Generated from the player's own exe and never committed: `gen/` (decompiled corpus, original-layout headers).
