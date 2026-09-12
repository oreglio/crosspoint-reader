#include "LibraryStateCodec.h"

namespace library {

namespace {
constexpr uint8_t FLAG_FAV_VIEW = 1 << 0;
// SortOrder now carries direction in the value itself, so there is no separate
// "titles descending" bit any more.
constexpr uint8_t SORT_COUNT = 6;

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void putU32(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>(v >> 24);
}
}  // namespace

bool parseShelfState(const uint8_t* data, const size_t len, LibraryShelfState& out) {
  if (data == nullptr || len != SHELF_STATE_BYTES) return false;
  if (data[0] != SHELF_STATE_VERSION) return false;
  if (data[2] >= SORT_COUNT || data[3] >= SORT_COUNT) return false;

  out.favoritesView = (data[1] & FLAG_FAV_VIEW) != 0;
  out.shelfSort = static_cast<SortOrder>(data[2]);
  out.favSort = static_cast<SortOrder>(data[3]);
  out.selected.nameHash = readU32(data + 4);
  out.selected.fileSize = readU32(data + 8);
  return true;
}

void serializeShelfState(const LibraryShelfState& state, uint8_t* out) {
  out[0] = SHELF_STATE_VERSION;
  out[1] = static_cast<uint8_t>(state.favoritesView ? FLAG_FAV_VIEW : 0);
  out[2] = static_cast<uint8_t>(state.shelfSort);
  out[3] = static_cast<uint8_t>(state.favSort);
  putU32(out + 4, state.selected.nameHash);
  putU32(out + 8, state.selected.fileSize);
}

}  // namespace library
