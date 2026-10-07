# M0b spikes (2026-10-07, MacBook M1 Pro, macOS 26.5.2)

| Spike | Script | Result |
|---|---|---|
| (a) async WebGPU device in the browser, no Asyncify | `spikes/web_triangle.cpp` (em++ 6.0.11 + emdawnwebgpu v20261002.154047) | Works: `main()` requests adapter/device with `WGPUCallbackMode_AllowSpontaneous`; the frame loop starts from the device callback. 31 KB wasm. No JS-side device import needed. |
| (b) headless Chromium WebGPU launch config | `spikes/spike_b.mjs` | headless-shell, no flags: no adapter. headless-shell + `--enable-unsafe-webgpu`: SwiftShader, state "drawn" but canvas screenshot **blank white**. `channel:'chromium'` + `--enable-unsafe-webgpu`: Apple Metal, centre `ff8000`, corner `000040`. Use the last; always check pixels. |
| (c) `setInputFiles(dir)` on `<input webkitdirectory>` + persistent profile | `spikes/spike_c.mjs` | Real install: 529 files / 1,102,338,267 bytes seen by the page = disk. 1,182 ms first run, 209 ms second run (same profile). Playwright passes paths, not bytes. |
