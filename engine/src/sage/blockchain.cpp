#include "lomn/sage/blockchain.h"

#include "bytes.h"

namespace lomn::sage {
using detail::fail;

const char* source_name(Source s) {
  switch (s) {
    case Source::Current: return "current";
    case Source::Second: return "second";
    case Source::Art: return "art";
    case Source::Disk: return "disk";
    default: return "none";
  }
}

bool BlockChain::open_into(std::string_view path, BlockFile& bf, std::string& stored_path, std::string* err) {
  bf = BlockFile();
  stored_path.clear();
  uint64_t size = 0;
  if (fs_.size(path, size) != fs::Error::None) return fail(err, "blockchain: cannot open " + std::string(path));
  std::string p(path);
  const fs::FileSystem& fs = fs_;
  bool ok = bf.open(size, [&fs, p](uint64_t off, uint64_t n, std::vector<uint8_t>& out) {
    return fs.read_range(p, off, n, out) == fs::Error::None;
  }, err);
  if (ok) stored_path = p;
  return ok;
}

bool BlockChain::open_art(std::string_view path, std::string* err) { return open_into(path, art_, art_path_, err); }
bool BlockChain::open_current(std::string_view path, std::string* err) { return open_into(path, current_, current_path_, err); }
bool BlockChain::open_second(std::string_view path, std::string* err) { return open_into(path, second_, second_path_, err); }

bool BlockChain::enter_area(std::string_view level, std::string_view area, std::string* err) {
  // "%.4s": the game truncates both ids to 4 characters.
  std::string lev(level.substr(0, 4)), are(area.substr(0, 4));
  // A failed entry leaves NO area entered (not the previous one, not half of the new one).
  clear_current();
  clear_second();
  if (!open_second("Root/Data/Levels/" + lev + "/" + lev + ".blk", err) ||
      !open_current("Root/Data/Levels/" + lev + "/" + are + ".blk", err)) {
    clear_current();
    clear_second();
    return false;
  }
  return true;
}

const BlockFile* BlockChain::blockfile(Source s) const {
  switch (s) {
    case Source::Current: return current_.is_open() ? &current_ : nullptr;
    case Source::Second: return second_.is_open() ? &second_ : nullptr;
    case Source::Art: return art_.is_open() ? &art_ : nullptr;
    default: return nullptr;
  }
}

const std::string& BlockChain::path(Source s) const {
  switch (s) {
    case Source::Current: return current_path_;
    case Source::Second: return second_path_;
    case Source::Art: return art_path_;
    default: return none_;
  }
}

Lookup BlockChain::find_load(std::string_view request) const {
  if (current_.is_open()) {
    for (Source s : {Source::Current, Source::Second, Source::Art}) {
      const BlockFile* bf = blockfile(s);
      if (!bf) continue;
      int i = bf->find(request);
      if (i >= 0) return {s, i};
    }
    return {};
  }
  if (art_.is_open()) {
    int i = art_.find(request);
    if (i >= 0) return {Source::Art, i};
  }
  if (fs_.exists(request)) return {Source::Disk, -1};
  return {};
}

Lookup BlockChain::find_exists(std::string_view request) const {
  if (art_.is_open()) {
    int i = art_.find(request);
    if (i >= 0) return {Source::Art, i};
  }
  if (current_.is_open()) {
    int i = current_.find(request);
    if (i >= 0) return {Source::Current, i};
    if (second_.is_open()) {
      i = second_.find(request);
      if (i >= 0) return {Source::Second, i};
    }
    return {};
  }
  if (fs_.exists(request)) return {Source::Disk, -1};
  return {};
}

bool BlockChain::load(std::string_view request, std::vector<uint8_t>& out, Source* from, std::string* err) const {
  if (from) *from = Source::None;
  std::string last_err = "not found: " + std::string(request);
  auto try_bf = [&](Source s) {
    const BlockFile* bf = blockfile(s);
    if (!bf) return false;
    int i = bf->find(request);
    if (i < 0) return false;
    std::string e;
    if (!bf->read(static_cast<size_t>(i), out, &e)) {
      last_err = e;
      return false;
    }
    if (from) *from = s;
    return true;
  };
  if (current_.is_open()) {
    if (try_bf(Source::Current) || try_bf(Source::Second) || try_bf(Source::Art)) return true;
    return fail(err, last_err);
  }
  if (try_bf(Source::Art)) return true;
  if (fs_.read(request, out) == fs::Error::None) {
    if (from) *from = Source::Disk;
    return true;
  }
  return fail(err, last_err);
}

}  // namespace lomn::sage
