// Process memory high-water mark for the crafted-input probes (probe_osi.cpp, probe_slb.cpp): peak RSS natively, the
// wasm32 linear-memory size under Node (it only grows when the allocator runs out of the initial 16 MiB).
#pragma once

#if defined(__EMSCRIPTEN__)
inline double probe_peak_mb() { return __builtin_wasm_memory_size(0) * 65536.0 / 1048576.0; }
#else
#include <sys/resource.h>
inline double probe_peak_mb() {
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
#if defined(__APPLE__)
  return u.ru_maxrss / 1048576.0;  // bytes
#else
  return u.ru_maxrss / 1024.0;  // KiB
#endif
}
#endif
