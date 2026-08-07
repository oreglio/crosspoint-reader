#include "LibraryState.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <string>

namespace library {

namespace {
constexpr char STATE_PATH[] = "/.crosspoint/library.state";
constexpr char STATE_NEW_PATH[] = "/.crosspoint/library.state.new";
constexpr char STALE_PATH[] = "/.crosspoint/library.stale";
constexpr uint8_t STATE_VERSION = 1;
constexpr size_t STATE_BYTES = 12;
constexpr uint8_t FLAG_FAV_VIEW = 1 << 0;
constexpr uint8_t FLAG_TITLE_DESC = 1 << 1;
constexpr uint8_t SORT_COUNT = 4;  // SortOrder has four values; anything else is corruption

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

const char* libraryStatePath() { return STATE_PATH; }

bool loadLibraryState(LibraryShelfState& out) {
  out = LibraryShelfState{};
  HalFile file;
  if (!Storage.openFileForRead("LIBST", STATE_PATH, file)) return true;  // no file yet: defaults

  uint8_t raw[STATE_BYTES];
  const bool sizeOk = file.fileSize64() == STATE_BYTES;
  const int got = sizeOk ? file.read(raw, STATE_BYTES) : -1;
  file.close();
  if (!sizeOk || got != static_cast<int>(STATE_BYTES) || raw[0] != STATE_VERSION || raw[2] >= SORT_COUNT ||
      raw[3] >= SORT_COUNT) {
    LOG_ERR("LIBST", "shelf state implausible; using defaults");
    return false;
  }

  out.favoritesView = (raw[1] & FLAG_FAV_VIEW) != 0;
  out.titleDescending = (raw[1] & FLAG_TITLE_DESC) != 0;
  out.shelfSort = static_cast<SortOrder>(raw[2]);
  out.favSort = static_cast<SortOrder>(raw[3]);
  out.selected.nameHash = readU32(raw + 4);
  out.selected.fileSize = readU32(raw + 8);
  return true;
}

bool saveLibraryState(const LibraryShelfState& state) {
  uint8_t raw[STATE_BYTES];
  raw[0] = STATE_VERSION;
  raw[1] =
      static_cast<uint8_t>((state.favoritesView ? FLAG_FAV_VIEW : 0) | (state.titleDescending ? FLAG_TITLE_DESC : 0));
  raw[2] = static_cast<uint8_t>(state.shelfSort);
  raw[3] = static_cast<uint8_t>(state.favSort);
  putU32(raw + 4, state.selected.nameHash);
  putU32(raw + 8, state.selected.fileSize);

  HalFile file;
  if (!Storage.openFileForWrite("LIBST", STATE_NEW_PATH, file)) {
    LOG_ERR("LIBST", "cannot open %s for write", STATE_NEW_PATH);
    return false;
  }
  const size_t written = file.write(raw, STATE_BYTES);
  file.close();
  if (written != STATE_BYTES) {
    LOG_ERR("LIBST", "short shelf state write");
    Storage.remove(STATE_NEW_PATH);
    return false;
  }
  Storage.remove(STATE_PATH);
  if (!Storage.rename(STATE_NEW_PATH, STATE_PATH)) {
    LOG_ERR("LIBST", "cannot install %s", STATE_PATH);
    return false;
  }
  return true;
}

bool reanchorLibraryStateSelection(const FavoriteKey& from, const FavoriteKey& to) {
  LibraryShelfState state;
  loadLibraryState(state);
  if (!(state.selected == from)) return false;
  state.selected = to;
  return saveLibraryState(state);
}

void markShelfStaleIfBook(const char* path) {
  if (path == nullptr) return;
  const std::string name(path);
  const bool book = FsHelpers::hasEpubExtension(name) || FsHelpers::checkFileExtension(name, ".txt") ||
                    FsHelpers::checkFileExtension(name, ".md") || FsHelpers::checkFileExtension(name, ".xtc");
  if (!book) return;
  // An empty file is the whole message. Failure is not worth failing the
  // transfer over — the manual rebuild button still exists.
  HalFile marker;
  if (Storage.openFileForWrite("LIBST", STALE_PATH, marker)) {
    marker.close();
    LOG_INF("LIBST", "shelf marked stale by %s", name.c_str());
  }
}

bool takeShelfStale() {
  if (!Storage.exists(STALE_PATH)) return false;
  Storage.remove(STALE_PATH);
  return true;
}

}  // namespace library
