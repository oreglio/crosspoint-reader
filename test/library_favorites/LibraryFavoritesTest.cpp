#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "LibraryIndex/LibraryFavorites.h"

using library::FavoriteKey;
using library::favoriteNameHash;
using library::FAVORITES_ENTRY_BYTES;
using library::FAVORITES_HEADER_BYTES;
using library::FAVORITES_MAX;
using library::FAVORITES_VERSION;
using library::parseFavorites;
using library::serializeFavorites;

namespace {

std::vector<uint8_t> bytesFor(const std::vector<FavoriteKey>& keys) {
  std::vector<uint8_t> out;
  serializeFavorites(keys, out);
  return out;
}

}  // namespace

// The favorites identity leans on this hash matching the index builder's
// reconciliation hash. The builder's fnv1a32 is file-local, so the agreement
// is pinned by value: these are the published FNV-1a 32-bit test vectors, and
// both implementations must keep producing them.
TEST(FavoriteNameHash, MatchesPublishedFnv1aVectors) {
  EXPECT_EQ(favoriteNameHash("", 0), 0x811C9DC5u);
  EXPECT_EQ(favoriteNameHash("a", 1), 0xE40C292Cu);
  const std::string foobar = "foobar";
  EXPECT_EQ(favoriteNameHash(foobar.data(), foobar.size()), 0xBF9CF968u);
}

TEST(FavoritesFormat, EmptyRoundTrip) {
  const std::vector<uint8_t> bytes = bytesFor({});
  ASSERT_EQ(bytes.size(), FAVORITES_HEADER_BYTES);
  EXPECT_EQ(bytes[0], FAVORITES_VERSION);

  std::vector<FavoriteKey> parsed{{1, 2}};
  ASSERT_TRUE(parseFavorites(bytes.data(), bytes.size(), parsed));
  EXPECT_TRUE(parsed.empty());
}

TEST(FavoritesFormat, RoundTripKeepsEveryKey) {
  const std::vector<FavoriteKey> keys = {{0x11111111, 100}, {0x22222222, 200}, {0x22222222, 300}};
  const std::vector<uint8_t> bytes = bytesFor(keys);
  ASSERT_EQ(bytes.size(), FAVORITES_HEADER_BYTES + keys.size() * FAVORITES_ENTRY_BYTES);

  std::vector<FavoriteKey> parsed;
  ASSERT_TRUE(parseFavorites(bytes.data(), bytes.size(), parsed));
  ASSERT_EQ(parsed.size(), keys.size());
  for (size_t i = 0; i < keys.size(); i++) EXPECT_TRUE(parsed[i] == keys[i]);
}

TEST(FavoritesFormat, LittleEndianOnDisk) {
  const std::vector<uint8_t> bytes = bytesFor({{0x04030201u, 0x08070605u}});
  ASSERT_EQ(bytes.size(), FAVORITES_HEADER_BYTES + FAVORITES_ENTRY_BYTES);
  EXPECT_EQ(bytes[1], 1);  // count lo
  EXPECT_EQ(bytes[2], 0);  // count hi
  const uint8_t expected[FAVORITES_ENTRY_BYTES] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  for (size_t i = 0; i < FAVORITES_ENTRY_BYTES; i++) EXPECT_EQ(bytes[FAVORITES_HEADER_BYTES + i], expected[i]);
}

TEST(FavoritesFormat, ParseSortsAndDeduplicates) {
  // Written unsorted and with a duplicate, as a foreign or damaged writer
  // might: contains() is a binary search, so parse must normalise.
  const std::vector<FavoriteKey> unsorted = {{9, 9}, {1, 2}, {9, 9}, {1, 1}};
  const std::vector<uint8_t> bytes = bytesFor(unsorted);

  std::vector<FavoriteKey> parsed;
  ASSERT_TRUE(parseFavorites(bytes.data(), bytes.size(), parsed));
  ASSERT_EQ(parsed.size(), 3u);
  EXPECT_TRUE(parsed[0] == (FavoriteKey{1, 1}));
  EXPECT_TRUE(parsed[1] == (FavoriteKey{1, 2}));
  EXPECT_TRUE(parsed[2] == (FavoriteKey{9, 9}));
}

TEST(FavoritesFormat, RejectsTruncatedHeader) {
  const uint8_t two[2] = {FAVORITES_VERSION, 0};
  std::vector<FavoriteKey> parsed{{1, 2}};
  EXPECT_FALSE(parseFavorites(two, sizeof(two), parsed));
  EXPECT_TRUE(parsed.empty());
  EXPECT_FALSE(parseFavorites(nullptr, 100, parsed));
}

TEST(FavoritesFormat, RejectsUnknownVersion) {
  std::vector<uint8_t> bytes = bytesFor({{1, 2}});
  bytes[0] = FAVORITES_VERSION + 1;
  std::vector<FavoriteKey> parsed;
  EXPECT_FALSE(parseFavorites(bytes.data(), bytes.size(), parsed));
}

TEST(FavoritesFormat, RejectsLengthCountMismatch) {
  std::vector<uint8_t> bytes = bytesFor({{1, 2}, {3, 4}});
  std::vector<FavoriteKey> parsed;

  // One byte short: an interrupted write.
  EXPECT_FALSE(parseFavorites(bytes.data(), bytes.size() - 1, parsed));
  EXPECT_TRUE(parsed.empty());

  // One byte of trailing garbage: not our file any more.
  std::vector<uint8_t> padded = bytes;
  padded.push_back(0);
  EXPECT_FALSE(parseFavorites(padded.data(), padded.size(), parsed));

  // Count claims more entries than the bytes hold.
  std::vector<uint8_t> lying = bytes;
  lying[1] = 3;
  EXPECT_FALSE(parseFavorites(lying.data(), lying.size(), parsed));
}

TEST(FavoritesFormat, ParseKeepsOnlyTheFirstMaxEntries) {
  // A file claiming FAVORITES_MAX + 1 entries, all bytes present. Built by
  // hand because serialize itself refuses to write past the cap.
  const uint16_t claimed = FAVORITES_MAX + 1;
  std::vector<uint8_t> bytes;
  bytes.push_back(FAVORITES_VERSION);
  bytes.push_back(static_cast<uint8_t>(claimed & 0xFF));
  bytes.push_back(static_cast<uint8_t>(claimed >> 8));
  for (uint16_t i = 0; i < claimed; i++) {
    for (int b = 0; b < 4; b++) bytes.push_back(static_cast<uint8_t>((i >> (8 * b)) & 0xFF));
    for (int b = 0; b < 4; b++) bytes.push_back(0);
  }

  std::vector<FavoriteKey> parsed;
  ASSERT_TRUE(parseFavorites(bytes.data(), bytes.size(), parsed));
  // The first FAVORITES_MAX stored entries survive; the tail is dropped.
  ASSERT_EQ(parsed.size(), static_cast<size_t>(FAVORITES_MAX));
  EXPECT_EQ(parsed.front().nameHash, 0u);
  EXPECT_EQ(parsed.back().nameHash, static_cast<uint32_t>(FAVORITES_MAX - 1));
}

TEST(FavoritesFormat, SerializeCapsAtMax) {
  std::vector<FavoriteKey> tooMany;
  for (uint32_t i = 0; i < FAVORITES_MAX + 10u; i++) tooMany.push_back({i, i});
  const std::vector<uint8_t> bytes = bytesFor(tooMany);
  EXPECT_EQ(bytes.size(), FAVORITES_HEADER_BYTES + static_cast<size_t>(FAVORITES_MAX) * FAVORITES_ENTRY_BYTES);
  EXPECT_EQ(bytes[1] | (bytes[2] << 8), FAVORITES_MAX);
}
