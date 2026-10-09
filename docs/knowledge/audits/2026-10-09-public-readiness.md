# Public-readiness audit (2026-10-09)

Scope: every commit on `main` (330 commits, through 3ee7700), the current tree, and the third-party notices. The scans ran on the full `git log -p` of `main` (16 MB of patches) and on every path ever added (2,003 paths).

## Results

| Check | Method (executed) | Result |
|---|---|---|
| Game data in history | every path ever added, matched against `.blk .slb .osi .x .dds .tga .exe .dll .bik .wav .mp3 .ogg .fnt .bmp .png .bin .dat`, `gen/`, `fixtures/` | **Pass.** None, apart from the 3 README screenshots in `docs/media/*.jpg` (renders of game assets; owner decision). |
| Large blobs | every blob over 1 MB in `rev-list --objects main` | **Pass.** None. |
| Secrets | added lines matched against Anthropic, GitHub, AWS key patterns, private-key headers and `api_key=` literals | **Pass.** 0 hits. |
| Disassembly listings | added lines shaped `address: bytes mnemonic` | **Pass.** 0 hits. |
| Decompiled code blocks | added lines with Ghidra tokens (`FUN_`, `DAT_`, `LAB_`, `undefined4`, `in_stack_`, `unaff_`, `extraout_`, `CONCAT`) and decompiler-style statements (`local_N`, `iVar`, `uVar`, `param_N`) | **Pass.** The hits are of three kinds, none of them a real decompiled function: the artefact *name* table in `docs/knowledge/research/2026-10-08-m2a0-artefacts.md`; short address citations such as `DAT_007032ac = 1` in comments and tests; and one hand-written synthetic decompiler-format fixture in `tools/tests/test_osi_registry.py` (its addresses point into a synthetic PE that the test builds itself). |
| Personal paths | added and removed lines matched against `/Users/<name>`, the owner's names and email | **Note.** Commit cc752ae added a scratch path containing the macOS username to `tools/research/slb.py` and `vastr.py`; 9c3ad84 removed it. It is still visible in history. |
| Commit metadata | `git log --format=%ae/%ce` | **Note.** All 330 commits carry the owner's personal email as author and committer. A public repo shows it. |
| Third-party licences | `THIRD_PARTY.md` | **Pass.** BSD-3, MIT, NCSA, zlib, Apache-2.0 and public domain. All of them are compatible with GPL-3.0 or MIT for our own code. Only Dawn, emdawnwebgpu, the Emscripten runtime and stb ship in builds. |
| Licence for our code | `LICENSE` file | **Open.** Not chosen yet; README §Licence says so. |
| Trademark and disclaimer | README | **Pass.** The repo name has no trademark. The README credits LEGO and says the project is not endorsed. |

## Decisions for the owner before going public
1. **Licence:** GPL-3.0 (recommended, as OpenMW uses) or MIT. Add `LICENSE` and update the README §Licence.
2. **Email and old path in history:** either accept them, or rewrite history once to use the GitHub no-reply address and drop the old path. A rewrite needs a force-push. Do it before the repo goes public, while no one else has a clone.
3. **README screenshots:** keep the 3 renders, or replace them with a diagram or our own UI.
4. **Optional:** a heads-up to the LOMN community (TheLegendOfMataNui).
5. **Then:** push, and switch the repository's visibility to public.

## Owner decisions (2026-10-09)
- Licence: **GPL-3.0**, added as `LICENSE`, with the README updated.
- History: **rewrite once**. The author email becomes the GitHub no-reply address, the old scratch path is removed, and one force-push follows before the repo goes public.
- README screenshots: **keep**.
