#include "lomn/view/prefetch.h"

#include <algorithm>
#include <set>

#include "lomn/sage/blockchain.h"
#include "lomn/view/area.h"

namespace lomn::view {

void boot_reads(const fs::FileSystem& fs) {
  sage::BlockChain chain(fs);
  chain.open_art(kArtBlkPath, nullptr);  // header + table (GcGame::LoadArtBlockHeader); absent on a synthetic install
  std::vector<uint8_t> bytes;
  fs.read(kBaseOsiPath, bytes);
}

std::vector<fs::ByteRange> record_reads(fs::FileSystem& fs, const std::function<void()>& fn) {
  std::set<fs::ByteRange> seen;
  // T8 P7: a zero-length read moves no bytes, so it is no prefetch work and PrefetchFileSystem::touched() can never list
  // it (no resident range); excluded here so the native manifest and the browser's discovery agree (lev4/haka.blk has an
  // empty entry table: BlockFile::open reads a 0-byte table at offset 12).
  fs.set_range_logger([&seen](const std::string& p, uint64_t off, uint64_t len) {
    if (len != 0) seen.insert({p, off, len});
  });
  fn();
  fs.set_range_logger(nullptr);
  return std::vector<fs::ByteRange>(seen.begin(), seen.end());
}

std::vector<AreaRef> list_areas(const std::vector<std::string>& paths) {
  static const std::string kPrefix = "root/data/levels/";
  std::set<AreaRef> out;
  for (const std::string& p : paths) {
    if (p.compare(0, kPrefix.size(), kPrefix) != 0) continue;
    const std::string rest = p.substr(kPrefix.size());
    const size_t slash = rest.find('/');
    if (slash == std::string::npos || rest.find('/', slash + 1) != std::string::npos) continue;
    const std::string lev = rest.substr(0, slash), file = rest.substr(slash + 1);
    if (file.size() < 5 || file.compare(file.size() - 4, 4, ".blk") != 0) continue;
    const std::string stem = file.substr(0, file.size() - 4);
    if (stem == lev || lev.empty() || lev.size() > 4 || stem.size() > 4) continue;
    out.insert({lev, stem});
  }
  return std::vector<AreaRef>(out.begin(), out.end());
}

std::string ranges_text(const std::vector<fs::ByteRange>& ranges) {
  std::string o;
  for (const fs::ByteRange& r : ranges) o += r.path + "\t" + std::to_string(r.offset) + "\t" + std::to_string(r.length) + "\n";
  return o;
}

}  // namespace lomn::view
