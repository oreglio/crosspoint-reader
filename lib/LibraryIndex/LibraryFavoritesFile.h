#pragma once

#include <vector>

#include "LibraryFavorites.h"

namespace library {

// The reader's favorites, loaded once and held as a sorted vector: membership
// is a binary search, and at the 512-entry cap the whole set is 4 KB. A toggle
// writes the file through at once — it is an explicit user action, not a hot
// path, so the debounce rule for per-page-turn writes does not apply.
class LibraryFavoritesFile {
 public:
  // A missing file is an empty set and success. Only a present-but-implausible
  // or corrupt file returns false, and it fails EMPTY rather than half-loaded.
  bool load();
  bool contains(const FavoriteKey& key) const;
  // Flips membership and writes through. Returns the NEW state, honestly: an
  // add refused at FAVORITES_MAX logs and returns false — still not a favorite.
  bool toggle(const FavoriteKey& key);
  // In-place file replacement changed the size half of a key: move the star
  // to the new identity. Count is unchanged, so the cap cannot refuse it.
  bool reanchor(const FavoriteKey& from, const FavoriteKey& to);
  uint16_t count() const { return static_cast<uint16_t>(keys.size()); }

 private:
  bool save() const;
  std::vector<FavoriteKey> keys;
};

const char* libraryFavoritesPath();

}  // namespace library
