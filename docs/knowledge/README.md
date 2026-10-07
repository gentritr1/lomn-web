# Knowledge base

What this project has learned, written so the next port (or the next session) starts from measured facts instead of guesses. Every number names how it was measured; anything not measured is marked INFERRED.

| Document | Use it for |
|---|---|
| [reverse-engineering-playbook.md](reverse-engineering-playbook.md) | Triage a binary, decode CodeView NB11, run the Ghidra pipeline, decompiler traps, readability metrics |
| [web-porting-playbook.md](web-porting-playbook.md) | Route choice (emulate / rebuild / decompile), WebGPU toolchain traps, bring-your-own files, wasm32 determinism, headless WebGPU testing, perf method |
| [measurements.md](measurements.md) | The raw measured facts (dated) the playbooks cite |
| [audits/](audits/) | Security and performance audits, each with evidence and the commit it applies to |

Rules for adding knowledge:

1. A claim needs the command, file or run that produced it, plus the date.
2. A trap gets the symptom someone would actually see ("blank canvas while state says drawn"), not only the fix.
3. When a later measurement contradicts an entry, update the entry and keep a one-line note of what changed.
