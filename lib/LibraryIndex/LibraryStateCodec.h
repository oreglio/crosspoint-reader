#pragma once

// The byte layer of /.crosspoint/library.state, split out from the file I/O so
// it can be host-tested — the same split LibraryFavorites/LibraryFavoritesFile
// already uses, and for the same reason.
//
// Little-endian, exactly 12 bytes:
//   u8  version       currently 2
//   u8  flags         bit0 favorites view
//   u8  shelfSort     SortOrder of the tab strip, direction included
//   u8  favSort       SortOrder of the ★ view, direction included
//   u32 selNameHash   \ identity of the selected book — the same pair
//   u32 selFileSize   / favorites key by; 0,0 means none
//
// Version 1 is refused rather than reinterpreted: SortOrder's values were
// reordered when the index core came from upstream, so the same byte names a
// different sort now, and the old range check cannot see it because the enum
// also grew from four values to six.

#include <cstddef>
#include <cstdint>

#include "LibraryFavorites.h"
#include "LibraryIndexFile.h"

namespace library {

inline constexpr uint8_t SHELF_STATE_VERSION = 2;
inline constexpr size_t SHELF_STATE_BYTES = 12;

struct LibraryShelfState {
  bool favoritesView = false;
  SortOrder shelfSort = SortOrder::AddedDesc;
  SortOrder favSort = SortOrder::AddedDesc;
  FavoriteKey selected{};  // nameHash 0 and fileSize 0 = none
};

// False for a wrong version, a wrong length, or a sort value outside the enum.
// `out` is left untouched on failure, so the caller keeps its defaults.
bool parseShelfState(const uint8_t* data, size_t len, LibraryShelfState& out);

// Writes exactly SHELF_STATE_BYTES bytes.
void serializeShelfState(const LibraryShelfState& state, uint8_t* out);

}  // namespace library
