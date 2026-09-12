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
}  // namespace

const char* libraryStatePath() { return STATE_PATH; }

bool loadLibraryState(LibraryShelfState& out) {
  out = LibraryShelfState{};
  HalFile file;
  if (!Storage.openFileForRead("LIBST", STATE_PATH, file)) return true;  // no file yet: defaults

  uint8_t raw[SHELF_STATE_BYTES];
  const bool sizeOk = file.fileSize64() == SHELF_STATE_BYTES;
  const int got = sizeOk ? file.read(raw, SHELF_STATE_BYTES) : -1;
  file.close();
  if (!sizeOk || got != static_cast<int>(SHELF_STATE_BYTES) || !parseShelfState(raw, SHELF_STATE_BYTES, out)) {
    LOG_ERR("LIBST", "shelf state implausible or from an older version; using defaults");
    return false;
  }
  return true;
}

bool saveLibraryState(const LibraryShelfState& state) {
  uint8_t raw[SHELF_STATE_BYTES];
  serializeShelfState(state, raw);

  HalFile file;
  if (!Storage.openFileForWrite("LIBST", STATE_NEW_PATH, file)) {
    LOG_ERR("LIBST", "cannot open %s for write", STATE_NEW_PATH);
    return false;
  }
  const size_t written = file.write(raw, SHELF_STATE_BYTES);
  file.close();
  if (written != SHELF_STATE_BYTES) {
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
