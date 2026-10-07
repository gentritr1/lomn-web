#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "doctest.h"
#include "lomn/loop.h"

static std::string slurp(const std::filesystem::path& p) {
  std::ifstream f(p);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

TEST_CASE("fake delta time is the original game's constant") {
  CHECK(lomn::kFakeDeltaTime == 0.006666666828095913);
  CHECK(static_cast<float>(lomn::kFakeDeltaTime) == static_cast<float>(1.0 / 150.0));
}

TEST_CASE("deterministic loop: two null runs give identical trace and call log") {
  namespace fs = std::filesystem;
  fs::path base = fs::temp_directory_path() / ("lomn_loop_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::remove_all(base);
  std::string traces[2];
  std::vector<std::string> logs[2];
  for (int run = 0; run < 2; ++run) {
    auto dev = lomn::rhi::create_null_device(&logs[run]);
    lomn::LoopOptions o;
    o.frames = 120;
    o.shots = {0, 60};
    o.out_dir = (base / std::to_string(run)).string();
    std::vector<lomn::FrameRecord> rec;
    REQUIRE(lomn::run_deterministic(*dev, o, &rec));
    REQUIRE(rec.size() == 120);
    CHECK(rec[0].sim_time == lomn::kFakeDeltaTime);
    traces[run] = slurp(base / std::to_string(run) / "trace.jsonl");
    CHECK(slurp(base / std::to_string(run) / "metrics.json").find("\"frames\": 120") != std::string::npos);
  }
  CHECK(traces[0] == traces[1]);
  CHECK(logs[0] == logs[1]);
  CHECK(traces[0].rfind("{\"frame\":0,\"sim_time\":0.0066666668280959129,", 0) == 0);
  fs::remove_all(base);
}

#ifndef __EMSCRIPTEN__
TEST_CASE("deterministic loop: unwritable out_dir returns false instead of throwing") {
  namespace fs = std::filesystem;
  fs::path base = fs::temp_directory_path() / ("lomn_loop_badout_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::remove_all(base);
  fs::create_directories(base);
  fs::path blocker = base / "plain_file";
  { std::ofstream(blocker) << "x"; }
  auto dev = lomn::rhi::create_null_device(nullptr);
  lomn::LoopOptions o;
  o.frames = 3;
  o.out_dir = (blocker / "sub").string();  // parent exists but is a regular file
  bool ok = true;
  CHECK_NOTHROW(ok = lomn::run_deterministic(*dev, o, nullptr));
  CHECK_FALSE(ok);
  fs::remove_all(base);
}
#endif
