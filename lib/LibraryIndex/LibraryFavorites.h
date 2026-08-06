#pragma once

// Favorites are primary data, and the one rule that shaped this file is that
// they must NOT live in the CLX index. The index is a derived cache: it is
// rebuilt from a walk, atomically replaced, and "delete library.idx" is the
// documented corruption remedy — a flag stored inside it would not survive the
// reader's own repair advice. Favorites live in their own tiny file and
// survive all of that.
//
// /.crosspoint/favorites.dat, little-endian:
//   u8  version        currently 1
//   u16 count
//   count x { u32 nameHash, u32 fileSize }
//
// The pair {fnv1a32(on-disk basename), fileSize} is the same identity the
// index rebuild uses to recognise a book across walks, so a favorite survives
// an index rebuild and a move to another folder. A rename loses the flag —
// the same accepted trade firstSeen makes — and there is deliberately no
// size-only fallback: exact match never guesses.
//
// This header and its .cpp are pure (no storage, no SDK) so the format is
// host-testable; file I/O lives in LibraryFavoritesFile, the same split
// LibraryFormat / LibraryIndexFile already uses.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace library {

inline constexpr uint8_t FAVORITES_VERSION = 1;
// Read bound, in the same spirit as LIBRARY_MAX_SORTED: a corrupt count must
// not turn into an unbounded allocation. It is also simply enough favorites.
inline constexpr uint16_t FAVORITES_MAX = 512;
inline constexpr size_t FAVORITES_HEADER_BYTES = 3;
inline constexpr size_t FAVORITES_ENTRY_BYTES = 8;

struct FavoriteKey {
  uint32_t nameHash = 0;
  uint32_t fileSize = 0;
};
inline bool operator<(const FavoriteKey& a, const FavoriteKey& b) {
  if (a.nameHash != b.nameHash) return a.nameHash < b.nameHash;
  return a.fileSize < b.fileSize;
}
inline bool operator==(const FavoriteKey& a, const FavoriteKey& b) {
  return a.nameHash == b.nameHash && a.fileSize == b.fileSize;
}

// FNV-1a over the on-disk basename. It must keep producing the same values as
// the builder's reconciliation hash (fnv1a32 in LibraryBuilder.cpp): the whole
// design leans on the two agreeing. Duplicated rather than shared for now so
// the upstream-reviewed index files stay byte-identical; fold both into one
// header when an upstream follow-up touches the lib anyway. The known-vector
// test in LibraryFavoritesTest pins the constants.
uint32_t favoriteNameHash(const char* data, size_t len);

// Parse an untrusted byte buffer. Rejects a missing or wrong-version header
// and any length that disagrees with the count the header itself claims — the
// same reject-don't-guess stance validateHeader takes. At most FAVORITES_MAX
// entries are kept (the first ones stored); the output is sorted and
// deduplicated whatever order the file held. On rejection returns false and
// leaves `out` empty.
bool parseFavorites(const uint8_t* data, size_t len, std::vector<FavoriteKey>& out);

// The canonical bytes for `keys`: expects them sorted (serialize does not
// re-sort), writes at most FAVORITES_MAX entries.
void serializeFavorites(const std::vector<FavoriteKey>& keys, std::vector<uint8_t>& out);

}  // namespace library
