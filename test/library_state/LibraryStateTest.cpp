#include <gtest/gtest.h>

#include "LibraryStateCodec.h"

// A v1 file was written when SortOrder meant {TitleAsc, TitleDesc, AuthorAsc,
// DateDesc}. Read under the new enum, its bytes turn "Recent" into "Title Z-A"
// without a word — and the old range guard would not catch it, because
// SORT_COUNT rose from 4 to 6. Version 2 refuses the file instead.
TEST(ShelfStateCodec, RejectsAVersion1FileRatherThanMisreadingIt) {
  uint8_t v1[library::SHELF_STATE_BYTES] = {};
  v1[0] = 1;  // the old STATE_VERSION
  v1[1] = 0;
  v1[2] = 3;  // old DateDesc; the new enum reads 3 as TitleDesc
  v1[3] = 3;

  library::LibraryShelfState out;
  EXPECT_FALSE(library::parseShelfState(v1, sizeof(v1), out));
}

TEST(ShelfStateCodec, RoundTripsDirectionThroughTheSortOrderAlone) {
  library::LibraryShelfState written;
  written.favoritesView = true;
  written.shelfSort = library::SortOrder::AuthorDesc;
  written.favSort = library::SortOrder::TitleAsc;
  written.selected = library::FavoriteKey{0xDEADBEEF, 4096};

  uint8_t bytes[library::SHELF_STATE_BYTES] = {};
  library::serializeShelfState(written, bytes);

  library::LibraryShelfState read;
  ASSERT_TRUE(library::parseShelfState(bytes, sizeof(bytes), read));
  EXPECT_TRUE(read.favoritesView);
  EXPECT_EQ(read.shelfSort, library::SortOrder::AuthorDesc);
  EXPECT_EQ(read.favSort, library::SortOrder::TitleAsc);
  EXPECT_EQ(read.selected.nameHash, 0xDEADBEEFu);
  EXPECT_EQ(read.selected.fileSize, 4096u);
}

TEST(ShelfStateCodec, RejectsASortValueOutsideTheEnum) {
  library::LibraryShelfState written;
  uint8_t bytes[library::SHELF_STATE_BYTES] = {};
  library::serializeShelfState(written, bytes);
  bytes[2] = 6;  // one past AuthorDesc

  library::LibraryShelfState read;
  EXPECT_FALSE(library::parseShelfState(bytes, sizeof(bytes), read));
}
