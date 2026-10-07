// M1b: what the engine reads, at (path, offset, length) granularity. Used two ways:
//  - natively, `lomn_view --manifest 1` records every area's reads over the disk file system (the measurement and the
//    oracle: boot set vs the census, per-area prefetch bytes);
//  - in the browser, the same reads drive the prefetch through PrefetchFileSystem misses (lomn/prefetch_fs.h).
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "lomn/fs.h"
#include "lomn/prefetch_fs.h"

namespace lomn::view {

// The boot reads: what every area load reads before anything area-specific (load_area: the Art.blk header and table,
// and Base.osi). The browser prefetches and pins them before main() (spec §4.3 "boot set").
void boot_reads(const fs::FileSystem& fs);

// Runs fn with a range logger installed on fs and returns the distinct ranges it read, sorted (path, offset, length).
// Zero-length reads are left out (T8 P7): they move no bytes, so there is nothing to prefetch, and the browser's
// PrefetchFileSystem::touched() cannot list them either; the raw range logger still reports them. The previous range
// logger is removed afterwards.
std::vector<fs::ByteRange> record_reads(fs::FileSystem& fs, const std::function<void()>& fn);

struct AreaRef {
  std::string level, area;  // lower-case, as the paths are normalised
  bool operator<(const AreaRef& o) const { return level != o.level ? level < o.level : area < o.area; }
  bool operator==(const AreaRef& o) const { return level == o.level && area == o.area; }
};
// Areas of an install from its normalised file paths: every root/data/levels/<lev>/<area>.blk except <lev>.blk itself
// (the rule lomn_view --survey uses). Sorted, distinct. Ids are cut to their first 4 characters by load_area, so only
// names of 1..4 characters are listed; longer ones are ignored (none in Beta 1.5.0).
std::vector<AreaRef> list_areas(const std::vector<std::string>& normalised_paths);

// One line per range, "path\toffset\tlength\n" (decimal): the text the browser bridge hands to JavaScript.
std::string ranges_text(const std::vector<fs::ByteRange>& ranges);

}  // namespace lomn::view
