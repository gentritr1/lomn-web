// SAGE blockfile (.blk) reader. Reproduces ScBlockFile (LEGOBionicle.exe, addresses below) on untrusted bytes.
// Layout (little-endian): "BLKF", u32 version (never checked by the game), u32 count, then count x 56-byte
// ScBlock entries { char mName[40]; u32 mBlkSize; u32 mFullSize; u32 mFlags; u32 offset }.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lomn::sage {

struct BlockEntry {
  std::string name;          // mName up to the first NUL, at most 40 bytes (an unterminated name is cut at 40)
  // What ScBlock::StrCmp actually compares: for an unterminated mName the C string runs on into mBlkSize,
  // mFullSize, ... (and the following entries) up to the next NUL byte of the in-memory table. Two Beta entries
  // are like this, which makes them unreachable by their 40-character name in the game. Bounded here by the
  // end of the table (the original would read past it).
  std::string lookup_name;
  uint32_t stored_size = 0;  // mBlkSize: bytes on disk when compressed
  uint32_t full_size = 0;    // mFullSize: bytes after decoding (and bytes read when stored)
  uint32_t flags = 0;        // bit 0: LZSS-compressed; bit 1: "f is a file offset" (every Beta entry has 2)
  uint32_t offset = 0;       // f: byte offset of the entry inside the .blk
  bool name_terminated = true;
};

constexpr size_t kBlockHeaderSize = 12;
constexpr size_t kBlockEntrySize = 56;
constexpr size_t kBlockNameSize = 40;
// LZSS can expand at most 144 output bytes per 17 input bytes (one flag byte + 8 pairs of 18 bytes): a declared
// full size above 9 x stored size cannot come from a valid stream and is rejected before any allocation.
constexpr uint64_t kLzssMaxRatio = 9;

// ScBlock::StrCmp (0x0060fd70): byte compare that folds only 'A'..'Z' with |0x60; a shorter string sorts first.
int block_strcmp(std::string_view a, std::string_view b);
// ScBlockFile::Find (0x0060fec0) drops everything up to the last '/' or '\\' of the request.
std::string_view block_basename(std::string_view request);

// SrDecodeLZSS (0x004d2920): Okumura LZSS, N=4096, F=18, threshold 2, ring pre-filled with 0x20, r=0xFEE.
// Bounded (spec §5): input never read past in_size, output never written past out_size. Unlike the original
// (which ignores its return value), a stream that does not produce exactly out_size bytes is an error.
bool decode_lzss(const uint8_t* in, size_t in_size, uint32_t out_size, std::vector<uint8_t>& out, std::string* err);

class BlockFile {
 public:
  // Reads [offset, offset + size) of the container; returns false on I/O failure.
  using RangeReader = std::function<bool(uint64_t offset, uint64_t size, std::vector<uint8_t>& out)>;

  // ScBlockFile::LoadHeader (0x00610000): magic "BLKF", count at +8, table at +12. Only the table is kept.
  // Bounds: 12 + count*56 <= file_size (u64).
  bool open(uint64_t file_size, RangeReader reader, std::string* err);
  // Convenience: the whole container already in memory.
  bool open_bytes(std::vector<uint8_t> bytes, std::string* err);

  bool is_open() const { return open_; }
  uint32_t version() const { return version_; }
  uint64_t file_size() const { return file_size_; }
  const std::vector<BlockEntry>& entries() const { return entries_; }
  // True when the table is strictly ascending by lookup_name under block_strcmp (Find's search requires it).
  bool is_sorted() const;

  // ScBlockFile::Find (0x0060fec0): basename, then the original's binary search with block_strcmp over
  // lookup_name. -1 if absent.
  int find(std::string_view request) const;
  // ScBlockFile::LoadDiskBlock (0x006100f0) for an index from find(): flags bit 0 -> read mBlkSize bytes and
  // LZSS-decode into mFullSize; otherwise read mFullSize bytes. offset+size is overflow-checked against the file.
  bool read(size_t index, std::vector<uint8_t>& out, std::string* err) const;

 private:
  bool open_ = false;
  uint32_t version_ = 0;
  uint64_t file_size_ = 0;
  std::vector<BlockEntry> entries_;
  RangeReader reader_;
};

}  // namespace lomn::sage
