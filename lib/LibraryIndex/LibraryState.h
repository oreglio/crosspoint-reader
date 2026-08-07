#pragma once

// The shelf's remembered posture: which view, which orders, which way the
// titles run, and the book the cursor was on. Preferences rather than primary
// data — favorites must survive anything, this merely spares the reader from
// re-expressing choices after every sleep. This reader deep-sleeps between
// sessions and wakes through a full boot, so RAM statics alone forget the
// shelf several times a day; a lost or corrupt file costs nothing but
// defaults.
//
// /.crosspoint/library.state, little-endian, exactly 12 bytes:
//   u8  version       currently 1
//   u8  flags         bit0 favorites view, bit1 titles descending
//   u8  shelfSort     SortOrder of the tab strip
//   u8  favSort       SortOrder of the ★ view
//   u32 selNameHash   \ identity of the selected book — the same pair
//   u32 selFileSize   / favorites key by; 0,0 means none
//
// The selection anchor is an identity, not a row number, so it survives a
// sort change, a filter, and even an index rebuild between sessions.

#include <cstdint>

#include "LibraryFavorites.h"
#include "LibraryIndexFile.h"

namespace library {

struct LibraryShelfState {
  bool favoritesView = false;
  bool titleDescending = false;
  SortOrder shelfSort = SortOrder::DateDesc;
  SortOrder favSort = SortOrder::DateDesc;
  FavoriteKey selected{};  // nameHash 0 and fileSize 0 = none
};

// A missing file is defaults and success; a corrupt or implausible one is
// defaults too, logged. Same reject-don't-guess stance as everything else
// that reads this directory.
bool loadLibraryState(LibraryShelfState& out);
// Written through the same write-beside-then-rename install step the index
// and favorites use.
bool saveLibraryState(const LibraryShelfState& state);

const char* libraryStatePath();

}  // namespace library
