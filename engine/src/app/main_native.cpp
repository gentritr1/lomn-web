// lomn_native --backend null|webgpu --frames N [--shot a,b,c] --out DIR
// Exit codes: 0 ok, 2 bad args, 3 no WebGPU adapter/device (treat as "skip" in tests), 4 run failed.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "lomn/loop.h"
#include "lomn/rhi.h"

int main(int argc, char** argv) {
  std::string backend = "null";
  lomn::LoopOptions opts;
  if ((argc - 1) % 2 != 0) {  // every option takes a value; a dangling trailing flag is a usage error
    std::fprintf(stderr, "option %s needs a value\n", argv[argc - 1]);
    std::fprintf(stderr, "usage: lomn_native --backend null|webgpu --frames N [--shot a,b] --out DIR\n");
    return 2;
  }
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i], v = argv[i + 1];
    if (k == "--backend") backend = v;
    else if (k == "--frames") opts.frames = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    else if (k == "--out") opts.out_dir = v;
    else if (k == "--shot") {
      std::stringstream ss(v);
      for (std::string t; std::getline(ss, t, ',');) opts.shots.push_back(static_cast<uint32_t>(std::strtoul(t.c_str(), nullptr, 10)));
    } else {
      std::fprintf(stderr, "unknown option %s\n", k.c_str());
      return 2;
    }
  }
  if (opts.out_dir.empty() || opts.frames == 0) {
    std::fprintf(stderr, "usage: lomn_native --backend null|webgpu --frames N [--shot a,b] --out DIR\n");
    return 2;
  }
  std::unique_ptr<lomn::rhi::Device> dev;
  std::vector<std::string> calls;
  if (backend == "null") {
    dev = lomn::rhi::create_null_device(&calls);
#if defined(LOMN_HAS_WEBGPU)
  } else if (backend == "webgpu") {
    std::string err;
    lomn::rhi::create_webgpu_device(nullptr, 0, 0, [&](std::unique_ptr<lomn::rhi::Device> d, std::string e) {
      dev = std::move(d);
      err = std::move(e);
    });
    if (!dev) {
      std::fprintf(stderr, "webgpu unavailable: %s\n", err.c_str());
      return 3;
    }
#endif
  } else {
    std::fprintf(stderr, "unknown backend %s\n", backend.c_str());
    return 2;
  }
  if (!lomn::run_deterministic(*dev, opts, nullptr)) return 4;
  if (backend == "null") {
    std::ofstream f(opts.out_dir + "/calls.txt");
    for (const auto& c : calls) f << c << '\n';
  }
  std::printf("ok backend=%s adapter=%s frames=%u out=%s\n", dev->backend().c_str(), dev->adapter().c_str(), opts.frames,
              opts.out_dir.c_str());
  return 0;
}
