#include "doctest.h"
#include "lomn/sage/blockfile.h"
#include "sage_test_util.h"

using namespace lomn::sage;
using namespace sage_test;

namespace {

// Independent greedy Okumura LZSS encoder (same ring conventions as the decoder: N=4096, F=18, THRESHOLD=2,
// ring pre-filled with spaces, r starts at 0xFEE). Brute-force match search, so only for small inputs.
Bytes lzss_encode(const Bytes& in) {
  uint8_t ring[4096];
  for (int i = 0; i < 4096; ++i) ring[i] = i < 0xfee ? 0x20 : 0;
  uint32_t r = 0xfee;
  Bytes out;
  size_t i = 0;
  while (i < in.size()) {
    size_t flag_at = out.size();
    out.push_back(0);
    for (int bit = 0; bit < 8 && i < in.size(); ++bit) {
      size_t best_len = 0;
      uint32_t best_pos = 0;
      for (uint32_t p = 0; p < 4096; ++p) {
        size_t k = 0;
        uint8_t emitted[18];
        while (k < 18 && i + k < in.size()) {
          uint32_t idx = (p + k) & 0xfff;
          uint32_t d = (idx - r) & 0xfff;  // reading a byte this match already wrote?
          uint8_t v = d < k ? emitted[d] : ring[idx];
          if (v != in[i + k]) break;
          emitted[k] = v;
          ++k;
        }
        if (k > best_len) {
          best_len = k;
          best_pos = p;
        }
      }
      if (best_len >= 3) {
        out.push_back(static_cast<uint8_t>(best_pos & 0xff));
        out.push_back(static_cast<uint8_t>(((best_pos >> 4) & 0xf0) | (best_len - 3)));
        for (size_t k = 0; k < best_len; ++k) {
          ring[r] = in[i + k];
          r = (r + 1) & 0xfff;
        }
        i += best_len;
      } else {
        out[flag_at] |= static_cast<uint8_t>(1u << bit);
        out.push_back(in[i]);
        ring[r] = in[i];
        r = (r + 1) & 0xfff;
        ++i;
      }
    }
  }
  return out;
}

BlockFile open_ok(const Bytes& b) {
  BlockFile bf;
  std::string err;
  REQUIRE_MESSAGE(bf.open_bytes(b, &err), err);
  return bf;
}

}  // namespace

TEST_CASE("blockfile: StrCmp folds only A-Z with |0x60, shorter sorts first") {
  CHECK(block_strcmp("MAIN.X", "main.x") == 0);
  CHECK(block_strcmp("a", "ab") == -1);
  CHECK(block_strcmp("ab", "a") == 1);
  CHECK(block_strcmp("", "") == 0);
  CHECK(block_strcmp("a_b", "aab") == -1);  // '_' 0x5f < 'a' 0x61, and 'A'..'Z' compare as lower case
  CHECK(block_strcmp("A_B", "aab") == -1);
  CHECK(block_strcmp("[", "a") == -1);       // 0x5b is not folded (not in 0x41..0x5a)
  CHECK(block_strcmp("@", "`") == -1);       // 0x40 / 0x60 are not letters: no fold
  CHECK(block_strcmp(std::string("ab\0zz", 5), "ab") == 0);  // C-string semantics
  CHECK(block_strcmp("\xe9", "z") == 1);     // bytes compare unsigned
}

TEST_CASE("blockfile: basename drops everything up to the last / or \\") {
  CHECK(block_basename("Root/Data/Levels/lev1/vllg/Xs/main.x") == "main.x");
  CHECK(block_basename("Root\\Data/lev1\\textures\\a.dds") == "a.dds");
  CHECK(block_basename("plain.slb") == "plain.slb");
  CHECK(block_basename("dir/") == "");
}

TEST_CASE("blockfile: open, find (case-insensitive, directories ignored), read stored entries") {
  Bytes b = make_blk({stored("a.dds", "AAA"), stored("Main.x", "xof data"), stored("vllg_OBJ.slb", "slb!")});
  BlockFile bf = open_ok(b);
  CHECK(bf.version() == 1);
  CHECK(bf.entries().size() == 3);
  CHECK(bf.is_sorted());
  CHECK(bf.find("MAIN.X") == 1);
  CHECK(bf.find("Root/Data/Levels/lev1/vllg/Xs/main.x") == 1);
  CHECK(bf.find("C:\\x\\VLLG_obj.SLB") == 2);
  CHECK(bf.find("a.dds") == 0);
  CHECK(bf.find("b.dds") == -1);
  CHECK(bf.find("") == -1);
  CHECK(bf.find("zzz") == -1);
  std::vector<uint8_t> out;
  std::string err;
  REQUIRE(bf.read(1, out, &err));
  CHECK(std::string(out.begin(), out.end()) == "xof data");
  CHECK_FALSE(bf.read(3, out, &err));
}

TEST_CASE("blockfile: Find reproduces the original binary search, quirks included") {
  // Unsorted table: the search window never reaches index 0, so "c.x" is unreachable, as in the game.
  BlockFile bf = open_ok(make_blk({stored("c.x", "1"), stored("a.x", "2"), stored("b.x", "3")}));
  CHECK_FALSE(bf.is_sorted());
  CHECK(bf.find("a.x") == 1);
  CHECK(bf.find("b.x") == 2);
  CHECK(bf.find("c.x") == -1);
  // Every entry of larger sorted tables is reachable, and absent names between entries are not found.
  for (int n = 1; n <= 40; ++n) {
    std::vector<BlkEntrySpec> es;
    for (int i = 0; i < n; ++i) es.push_back(stored("n" + std::to_string(100 + 2 * i), "x"));
    BlockFile s = open_ok(make_blk(es));
    for (int i = 0; i < n; ++i) {
      CHECK(s.find("N" + std::to_string(100 + 2 * i)) == i);
      CHECK(s.find("n" + std::to_string(101 + 2 * i)) == -1);
    }
    CHECK(s.find("n000") == -1);
  }
  BlockFile empty = open_ok(make_blk({}));
  CHECK(empty.find("x") == -1);
}

TEST_CASE("blockfile: a 40-byte name without NUL runs on into the size fields, as StrCmp reads it") {
  std::string longname(36, 'q');
  longname += ".dds";  // exactly 40 chars, no terminator in the file
  // mBlkSize = 0x10080 -> bytes 80 00 ...: the game compares against longname + "\x80" (lev3.blk entry 161).
  Bytes payload(0x10080, 7);
  BlockFile bf = open_ok(make_blk({stored("a.dds", "1"), BlkEntrySpec{longname, payload, 0x10080, 2}, stored("zz.dds", "2")}));
  const BlockEntry& e = bf.entries()[1];
  CHECK_FALSE(e.name_terminated);
  CHECK(e.name == longname);
  CHECK(e.lookup_name == longname + "\x80");
  CHECK(bf.is_sorted());
  CHECK(bf.find(longname) == -1);          // unreachable by its visible name, like in the game
  CHECK(bf.find(longname + "\x80") == 1);
  CHECK(bf.find("zz.dds") == 2);
  // Size bytes without any NUL: the lookup name continues into the next entry, bounded by the table end.
  Bytes raw = make_blk({stored(longname, "x")});
  for (size_t k = 12 + 40; k < 12 + 56; ++k) raw[k] = static_cast<uint8_t>('A' + (k - 52) / 4);
  BlockFile last = open_ok(raw);
  CHECK(last.entries()[0].lookup_name == longname + "AAAABBBBCCCCDDDD");
  CHECK(last.find(longname + "aaaabbbbccccdddd") == 0);
  std::vector<uint8_t> out;
  std::string err;
  CHECK_FALSE(last.read(0, out, &err));  // offset 0x44444444 is outside the file
}

TEST_CASE("blockfile: unterminated name runs are capped (no quadratic table copy)") {
  // Hostile table with no NUL anywhere: each name would run to the end of the table, count^2 * 28 bytes in total.
  // 12000 entries (672 KB) aborted wasm32 before the cap; it must be a prompt error now.
  {
    Bytes b;
    putstr(b, "BLKF");
    put32(b, 1);
    put32(b, 12000);
    b.insert(b.end(), size_t{12000} * 56, 'A');
    BlockFile bf;
    std::string err;
    CHECK_FALSE(bf.open_bytes(std::move(b), &err));
    CHECK(err.find("unterminated name run") != std::string::npos);
  }
  // A short, real-looking run (the Beta files have 1 and 2 bytes) still resolves exactly as before.
  {
    std::string longname(36, 'q');
    longname += ".dds";
    Bytes raw = make_blk({stored("a.dds", "1"), stored(longname, "x")});
    // entry 1: mBlkSize bytes "ABC\0" -> the C string is the 40 name bytes + "ABC"
    set32(raw, 12 + 56 + 40, 0x00434241u);
    BlockFile bf = open_ok(raw);
    CHECK(bf.entries()[1].lookup_name == longname + "ABC");
    CHECK(bf.find(longname + "abc") == 1);
  }
  // Boundary: a run of exactly 512 bytes after the name is accepted, 513 is rejected.
  for (size_t ext : {size_t{512}, size_t{513}}) {
    Bytes b;
    putstr(b, "BLKF");
    put32(b, 1);
    put32(b, 10);
    b.resize(12 + 560, 0);
    for (size_t k = 0; k < 40; ++k) b[12 + k] = 'q';
    for (size_t k = 0; k < ext; ++k) b[12 + 40 + k] = 'A';
    BlockFile bf;
    std::string err;
    CHECK(bf.open_bytes(std::move(b), &err) == (ext == 512));
    if (ext == 513) CHECK(err.find("unterminated name run") != std::string::npos);
  }
}

TEST_CASE("blockfile: malformed containers are rejected") {
  BlockFile bf;
  std::string err;
  CHECK_FALSE(bf.open_bytes({}, &err));
  CHECK_FALSE(bf.open_bytes(Bytes{'B', 'L', 'K'}, &err));
  Bytes b = make_blk({stored("a", "1")});
  Bytes bad = b;
  bad[0] = 'X';
  CHECK_FALSE(bf.open_bytes(bad, &err));
  CHECK(err.find("magic") != std::string::npos);
  // count*56 beyond the file, including a count whose product overflows 32 bits.
  for (uint32_t count : {2u, 0x04924925u, 0xffffffffu}) {
    Bytes c = b;
    set32(c, 8, count);
    CHECK_FALSE(bf.open_bytes(c, &err));
  }
  // Truncated table: every prefix shorter than header+table fails, none crash.
  for (size_t n = 0; n < 12 + 56; ++n) CHECK_FALSE(bf.open_bytes(Bytes(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n)), &err));
}

TEST_CASE("blockfile: entry offset/size are overflow-checked against the file") {
  Bytes b = make_blk({stored("a", "12345678")});
  std::vector<uint8_t> out;
  std::string err;
  {
    Bytes c = b;
    set32(c, 12 + 52, 0xfffffff0u);  // offset near 4 GiB: offset + size wraps in 32 bits
    BlockFile bf = open_ok(c);
    CHECK_FALSE(bf.read(0, out, &err));
  }
  {
    Bytes c = b;
    set32(c, 12 + 44, 9);  // full size one past the end
    BlockFile bf = open_ok(c);
    CHECK_FALSE(bf.read(0, out, &err));
  }
  {
    Bytes c = b;
    set32(c, 12 + 44, 0);  // zero-length entry (6 such .dds exist in the Beta data) reads as empty
    BlockFile bf = open_ok(c);
    REQUIRE(bf.read(0, out, &err));
    CHECK(out.empty());
  }
}

TEST_CASE("blockfile: an entry larger than kMaxEntrySize is refused before any allocation") {
  Bytes b = make_blk({stored("a", "12345678")});
  std::vector<uint8_t> out(3, 9);
  std::string err;
  {
    Bytes c = b;
    set32(c, 12 + 44, static_cast<uint32_t>(kMaxEntrySize + 1));  // stored (flags 2): full_size bytes would be read
    BlockFile bf = open_ok(c);
    CHECK_FALSE(bf.read(0, out, &err));
    CHECK(err.find("entry too large") != std::string::npos);
  }
  {
    Bytes c = b;
    set32(c, 12 + 48, 3);                                          // compressed: full_size is the output size
    set32(c, 12 + 40, 8);
    set32(c, 12 + 44, static_cast<uint32_t>(kMaxEntrySize + 1));
    BlockFile bf = open_ok(c);
    CHECK_FALSE(bf.read(0, out, &err));
    CHECK(err.find("entry too large") != std::string::npos);
  }
}

TEST_CASE("blockfile: a failed open leaves the object empty, not partially filled") {
  BlockFile bf = open_ok(make_blk({stored("a.dds", "1")}));
  REQUIRE(bf.find("a.dds") == 0);
  Bytes b;  // entry 0 is fine, entry 1 is an unterminated name whose run exceeds the cap
  putstr(b, "BLKF");
  put32(b, 1);
  put32(b, 12);
  b.resize(12 + 12 * 56, 'A');
  std::memset(b.data() + 12, 0, 40);
  std::memcpy(b.data() + 12, "b.dds", 5);
  std::string err;
  CHECK_FALSE(bf.open_bytes(std::move(b), &err));
  CHECK_FALSE(bf.is_open());
  CHECK(bf.entries().empty());
  CHECK(bf.find("b.dds") == -1);
  CHECK(bf.find("a.dds") == -1);
}

TEST_CASE("blockfile: a compressed entry's stored_size is capped too") {
  Bytes c = make_blk({stored("a", "12345678")});
  set32(c, 12 + 48, 3);                                          // compressed
  set32(c, 12 + 40, static_cast<uint32_t>(kMaxEntrySize + 1));   // huge stored_size, small full_size
  set32(c, 12 + 44, 8);
  BlockFile bf = open_ok(c);
  std::vector<uint8_t> out(2, 7);
  std::string err;
  CHECK_FALSE(bf.read(0, out, &err));
  CHECK(err.find("entry too large") != std::string::npos);
}

TEST_CASE("lzss: hand-built streams decode per SrDecodeLZSS") {
  std::vector<uint8_t> out;
  std::string err;
  // 8 literals.
  Bytes lit = {0xff, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
  REQUIRE(decode_lzss(lit.data(), lit.size(), 8, out, &err));
  CHECK(std::string(out.begin(), out.end()) == "ABCDEFGH");
  // A reference into the space-filled ring: pos 0, len 3.
  Bytes sp = {0x00, 0x00, 0x00};
  REQUIRE(decode_lzss(sp.data(), sp.size(), 3, out, &err));
  CHECK(std::string(out.begin(), out.end()) == "   ");
  // "ab" then an overlapping copy from r=0xFEE of length 4 -> "ababab".
  Bytes ov = {0x03, 'a', 'b', 0xee, 0xf1};
  REQUIRE(decode_lzss(ov.data(), ov.size(), 6, out, &err));
  CHECK(std::string(out.begin(), out.end()) == "ababab");
  // Maximum length 18 from one pair.
  Bytes mx = {0x00, 0x00, 0x0f};
  REQUIRE(decode_lzss(mx.data(), mx.size(), 18, out, &err));
  CHECK(out == Bytes(18, 0x20));
}

TEST_CASE("lzss: bounds -- output cap, short streams, ratio cap, input bound") {
  std::vector<uint8_t> out;
  std::string err;
  Bytes mx = {0x00, 0x00, 0x0f};                          // produces 18 bytes
  CHECK_FALSE(decode_lzss(mx.data(), mx.size(), 17, out, &err));  // would overrun the declared size
  CHECK(err.find("overrun") != std::string::npos);
  CHECK_FALSE(decode_lzss(mx.data(), mx.size(), 19, out, &err));  // stream ends early
  Bytes half = {0x00, 0x00};                              // pair missing its second byte: stops like the original
  CHECK_FALSE(decode_lzss(half.data(), half.size(), 3, out, &err));
  CHECK(decode_lzss(half.data(), 0, 0, out, &err));
  CHECK_FALSE(decode_lzss(mx.data(), mx.size(), 28, out, &err));  // 28 > 9 * 3: ratio cap, no allocation
  CHECK(err.find("ratio") != std::string::npos);
  CHECK_FALSE(decode_lzss(mx.data(), mx.size(), 0xffffffffu, out, &err));
}

TEST_CASE("lzss: round trip through an independent encoder, and compressed blockfile entries") {
  Bytes data;
  uint32_t seed = 12345;
  for (int i = 0; i < 1200; ++i) {
    seed = seed * 1103515245u + 12345u;
    data.push_back(i % 97 < 60 ? static_cast<uint8_t>("the quick brown fox "[i % 20]) : static_cast<uint8_t>(seed >> 24));
  }
  Bytes enc = lzss_encode(data);
  CHECK(enc.size() < data.size());
  std::vector<uint8_t> out;
  std::string err;
  REQUIRE(decode_lzss(enc.data(), enc.size(), static_cast<uint32_t>(data.size()), out, &err));
  CHECK(out == data);

  BlkEntrySpec comp{"c.bin", enc, static_cast<uint32_t>(data.size()), 3};
  BlockFile bf = open_ok(make_blk({comp, stored("d.bin", "plain")}));
  REQUIRE(bf.read(0, out, &err));
  CHECK(out == data);
  // A declared size beyond the ratio cap is refused before reading.
  Bytes b = make_blk({BlkEntrySpec{"c.bin", enc, static_cast<uint32_t>(enc.size() * 9 + 1), 3}});
  BlockFile bad = open_ok(b);
  CHECK_FALSE(bad.read(0, out, &err));
}

TEST_CASE("lzss / blockfile: a failed decode or read leaves the output empty, never partial (M1a-4 fuzz_formats finding)") {
  std::string err;
  Bytes mx = {0x00, 0x00, 0x0f};  // one pair: 18 bytes of output
  std::vector<uint8_t> out = {1, 2, 3};
  CHECK_FALSE(decode_lzss(mx.data(), mx.size(), 17, out, &err));  // overrun after 0 bytes written
  CHECK(out.empty());
  out = {1, 2, 3};
  CHECK_FALSE(decode_lzss(mx.data(), mx.size(), 19, out, &err));  // stream ends after 18 of 19 bytes
  CHECK(out.empty());
  out = {1, 2, 3};
  CHECK_FALSE(decode_lzss(mx.data(), mx.size(), 28, out, &err));  // ratio cap, before any decoding
  CHECK(out.empty());
  // BlockFile::read: a compressed entry whose stream is short, and an index outside the table.
  BlockFile bf = open_ok(make_blk({BlkEntrySpec{"c.bin", mx, 19, 3}}));
  out = {9};
  CHECK_FALSE(bf.read(0, out, &err));
  CHECK(err.find("stream ended") != std::string::npos);
  CHECK(out.empty());
  out = {9};
  CHECK_FALSE(bf.read(1, out, &err));
  CHECK(out.empty());
}
