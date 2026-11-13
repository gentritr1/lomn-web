// M2a-3 T5a: Bionicle.ini through the VFS with Win32 semantics. RED first: written before engine/src/fs/ini.cpp. The crafted
// table is data/ini_crafted.ini + data/ini_crafted.expect (one row per case: case, quotes, duplicates, a 300-byte value, a
// missing section, comment lines); the expected column is written from the contract in lomn/ini.h, not from the reader.
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "lomn/fs.h"
#include "lomn/ini.h"
#include "lomn/osi/directors.h"
#include "lomn/osi/host.h"
#include "lomn/osi/services.h"
#include "lomn/osi/trace.h"
#include "osi_test_asm.h"

using namespace lomn;
using lomn::test::Asm;

namespace {
std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
std::string tests_data(const char* name) { return std::string(LOMN_TESTS_DIR) + "/data/" + name; }

const char* const kRows = R"([
 {"cls": "GcSaver", "name": "CheatMenu", "min": 0, "max": 0, "types": [], "ret": "int", "winner": true},
 {"cls": "GcGame", "name": "GetResolutionWidth", "min": 0, "max": 0, "types": [], "ret": "float", "winner": true},
 {"cls": "GcGame", "name": "GetResolutionHeight", "min": 0, "max": 0, "types": [], "ret": "float", "winner": true}
])";

// Runs Setup of `a` over a VFS holding `ini` as Root/Bionicle.ini (empty ptr: no file) and returns the Setup result.
osi::Variant run_setup(Asm& a, const std::string* ini) {
  sage::OsiFile file;
  std::string err;
  REQUIRE_MESSAGE(file.parse(a.build(), &err), err);
  fs::MemoryFileSystem mem;
  if (ini) REQUIRE(mem.mount("Root/Bionicle.ini", std::vector<uint8_t>(ini->begin(), ini->end())));
  osi::ScriptHost::Options opt;
  opt.real_natives = true;
  osi::StringTrace tr, log;
  osi::ScriptHost host(file, kRows, "", R"({"types": []})", "", &mem, &tr, &log, opt);
  osi::CallResult r = host.setup();
  REQUIRE_MESSAGE(r.ok, (host.vm().last_error() ? host.vm().last_error()->message : std::string("?")));
  return r.result;
}
}  // namespace

TEST_CASE("ini crafted table: every row of ini_crafted.expect") {
  const std::string text = slurp(tests_data("ini_crafted.ini"));
  REQUIRE(!text.empty());
  const ini::File f(text);
  std::istringstream rows(slurp(tests_data("ini_crafted.expect")));
  std::string line;
  int n = 0;
  while (std::getline(rows, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> c;
    size_t p = 0;
    for (;;) {
      const size_t t = line.find('\t', p);
      if (t == std::string::npos) {
        c.push_back(line.substr(p));
        break;
      }
      c.push_back(line.substr(p, t - p));
      p = t + 1;
    }
    REQUIRE(c.size() == 6);
    INFO("row ", n, ": ", c[0], " [", c[1], "] ", c[2]);
    if (c[0] == "S")
      CHECK(f.get_string(c[1], c[2], c[3], std::stoul(c[4])) == c[5]);
    else
      CHECK(f.get_int(c[1], c[2], static_cast<uint32_t>(std::stoul(c[3]))) == std::stoul(c[5]));
    ++n;
  }
  CHECK(n == 32);  // a row cannot be dropped silently
}

TEST_CASE("ini: an empty or missing file gives the defaults; load() reads Root/Bionicle.ini and never writes it") {
  CHECK(ini::File("").get_string("Misc", "CheatMenu", "dflt", 255) == "dflt");
  fs::MemoryFileSystem mem;
  ini::File f;
  CHECK_FALSE(ini::load(mem, f));
  const std::string text = "[Misc]\nCheatMenu=LEGOTester\n";
  REQUIRE(mem.mount("Root/Bionicle.ini", std::vector<uint8_t>(text.begin(), text.end())));
  REQUIRE(ini::load(mem, f));
  CHECK(f.get_string("misc", "cheatmenu", "", 255) == "LEGOTester");
  std::vector<uint8_t> after;
  REQUIRE(mem.read("Root/Bionicle.ini", after) == fs::Error::None);
  CHECK(std::string(after.begin(), after.end()) == text);  // the mount is read-only; the host has no write path
  CHECK(mem.write("Root/Bionicle.ini", nullptr, 0) == fs::Error::ReadOnly);
}

TEST_CASE("ini natives: gcsaver.cheatmenu and gcgame.getresolutionwidth/height read the INI (asm: hud.md 6.2)") {
  auto cheat = [](const std::string* ini) {
    Asm a;
    a.fn("setup", 0).call("gcsaver", "cheatmenu", 0).ret();
    return run_setup(a, ini);
  };
  auto wh = [](const std::string* ini) {
    Asm a;
    a.fn("setup", 0).call("gcgame", "getresolutionwidth", 0).pop().call("gcgame", "getresolutionheight", 0).ret();
    return run_setup(a, ini);
  };
  auto width = [](const std::string* ini) {
    Asm a;
    a.fn("setup", 0).call("gcgame", "getresolutionwidth", 0).ret();
    return run_setup(a, ini);
  };
  const std::string yes = "[misc]\r\nCHEATMENU = \"LEGOTester\"\r\n", no = "[Misc]\r\nCheatMenu=legotester\r\n",
                    big = "[GraphicsOptions]\r\nResolutionW=800\r\nResolutionH=600\r\n",
                    zero = "[GraphicsOptions]\r\nResolutionW=0\r\nResolutionH=7\r\n",
                    keyless = "[GraphicsOptions]\r\nWindowed=1\r\n", wide = "[GraphicsOptions]\r\nResolutionW=65541\r\n";
  CHECK(cheat(nullptr) == osi::make_int(0));
  CHECK(cheat(&yes) == osi::make_int(1));
  CHECK(cheat(&no) == osi::make_int(0));  // case-sensitive compare, hud.md 6.2
  CHECK(wh(nullptr) == osi::make_float(480.0f));
  CHECK(width(nullptr) == osi::make_float(640.0f));
  CHECK(wh(&big) == osi::make_float(600.0f));
  CHECK(width(&big) == osi::make_float(800.0f));
  CHECK(width(&keyless) == osi::make_float(640.0f));  // GetScreenData defaults 0x280 / 0x1e0 (hud.md 6.2)
  CHECK(wh(&keyless) == osi::make_float(480.0f));
  CHECK(width(&zero) == osi::make_float(640.0f));     // LoadIni rewrites a 0 ResolutionW with the defaults (recorded, not run)
  CHECK(wh(&zero) == osi::make_float(480.0f));
  CHECK(width(&wide) == osi::make_float(5.0f));       // stored as u16 (GetScreenData)
}

TEST_CASE("ini: the owner's Bionicle.ini: cheatmenu 0, the Urinate gate true, 640x480 (F12/F13)") {
  const char* ld = std::getenv("LOMN_DATA");
  if (!ld || !*ld) {
    MESSAGE("SKIPPED (no LOMN_DATA)");
    return;
  }
  const std::string root = std::string(ld) + "/install";
  auto disk = fs::open_disk(root, root);  // the install doubles as the user dir here: nothing is ever written
  REQUIRE(disk);
  ini::File f;
  REQUIRE(ini::load(*disk, f));
  CHECK(f.get_string("Misc", "CheatMenu", "", 255) == "");            // hud.md 6.2: CheatMenu false -> gcsaver.cheatmenu 0
  CHECK(f.get_string("Cheat", "Urinate", "", 255) == "Pohatu");      // BadCheat 0x5d4e40 true: the block is not script-visible
  CHECK(f.get_string("Cheat", "Cheat", "", 255) == "");              // Cheat / CheatDebug false
  CHECK(f.get_int("GraphicsOptions", "ResolutionW", 0x280) == 640);
  CHECK(f.get_int("GraphicsOptions", "ResolutionH", 0x1e0) == 480);
  const std::string text = slurp(root + "/Bionicle.ini");
  CHECK(text.size() == 250);                                           // read, byte count equal to the file's
}
