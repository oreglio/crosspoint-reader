#include "LibraryFavorites.h"

#include <algorithm>

namespace library {

namespace {

// Explicit little-endian byte access, so the on-disk format is defined by
// this file and not by whatever the compiler laid a struct out as. The host
// tests then hold on any machine.
uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void putU16(std::vector<uint8_t>& out, const uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>(v >> 8));
}

void putU32(std::vector<uint8_t>& out, const uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>(v >> 24));
}

}  // namespace

uint32_t favoriteNameHash(const char* data, const size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 16777619u;
  }
  return hash;
}

bool parseFavorites(const uint8_t* data, const size_t len, std::vector<FavoriteKey>& out) {
  out.clear();
  if (data == nullptr || len < FAVORITES_HEADER_BYTES) return false;
  if (data[0] != FAVORITES_VERSION) return false;
  const uint16_t count = readU16(data + 1);
  // The length must match the count the header claims. A truncated or padded
  // file is a corrupt file; guessing which half to trust helps nobody.
  if (len != FAVORITES_HEADER_BYTES + static_cast<size_t>(count) * FAVORITES_ENTRY_BYTES) return false;

  const uint16_t kept = count < FAVORITES_MAX ? count : FAVORITES_MAX;
  out.reserve(kept);
  const uint8_t* p = data + FAVORITES_HEADER_BYTES;
  for (uint16_t i = 0; i < kept; i++, p += FAVORITES_ENTRY_BYTES) {
    FavoriteKey key;
    key.nameHash = readU32(p);
    key.fileSize = readU32(p + 4);
    out.push_back(key);
  }
  // Normalised here rather than trusted from disk: contains() is a binary
  // search, and a file another writer left unsorted must not break it.
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return true;
}

void serializeFavorites(const std::vector<FavoriteKey>& keys, std::vector<uint8_t>& out) {
  const size_t n = keys.size() < FAVORITES_MAX ? keys.size() : FAVORITES_MAX;
  out.clear();
  out.reserve(FAVORITES_HEADER_BYTES + n * FAVORITES_ENTRY_BYTES);
  out.push_back(FAVORITES_VERSION);
  putU16(out, static_cast<uint16_t>(n));
  for (size_t i = 0; i < n; i++) {
    putU32(out, keys[i].nameHash);
    putU32(out, keys[i].fileSize);
  }
}

bool reanchorFavoriteKey(std::vector<FavoriteKey>& keys, const FavoriteKey& from, const FavoriteKey& to) {
  if (from == to) return false;
  const auto it = std::lower_bound(keys.begin(), keys.end(), from);
  if (it == keys.end() || !(*it == from)) return false;
  keys.erase(it);
  const auto dst = std::lower_bound(keys.begin(), keys.end(), to);
  if (dst == keys.end() || !(*dst == to)) keys.insert(dst, to);
  return true;
}

}  // namespace library
