#include "LibraryFavoritesFile.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

namespace library {

namespace {
constexpr char FAV_PATH[] = "/.crosspoint/favorites.dat";
constexpr char FAV_NEW_PATH[] = "/.crosspoint/favorites.new";
// Anything past this is not a favorites file: the largest set a v1 writer
// produces is 3 + 512*8 = 4099 bytes, and the bound keeps a corrupt size
// field from turning into a large transient allocation on the C3.
constexpr uint64_t FAV_MAX_FILE_BYTES = 16 * 1024;
}  // namespace

const char* libraryFavoritesPath() { return FAV_PATH; }

bool LibraryFavoritesFile::load() {
  keys.clear();
  HalFile file;
  // No file yet simply means nothing has been favorited: an empty set, not an
  // error. The file first appears on the first toggle.
  if (!Storage.openFileForRead("LIBFAV", FAV_PATH, file)) return true;

  const uint64_t size = file.fileSize64();
  if (size < FAVORITES_HEADER_BYTES || size > FAV_MAX_FILE_BYTES) {
    LOG_ERR("LIBFAV", "favorites file implausible (%u bytes); starting empty", static_cast<unsigned>(size));
    file.close();
    return false;
  }

  auto raw = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(size));
  if (!raw) {
    LOG_ERR("LIBFAV", "no memory to read favorites (%u bytes)", static_cast<unsigned>(size));
    file.close();
    return false;
  }
  const int got = file.read(raw.get(), static_cast<size_t>(size));
  file.close();
  if (got != static_cast<int>(size)) {
    LOG_ERR("LIBFAV", "short favorites read (%d of %u)", got, static_cast<unsigned>(size));
    return false;
  }

  if (!parseFavorites(raw.get(), static_cast<size_t>(size), keys)) {
    LOG_ERR("LIBFAV", "favorites file corrupt; starting empty");
    return false;
  }
  const uint16_t claimed = static_cast<uint16_t>(raw[1] | (raw[2] << 8));
  if (claimed > FAVORITES_MAX) {
    LOG_INF("LIBFAV", "%u favorites on disk, keeping the first %u", claimed, FAVORITES_MAX);
  }
  return true;
}

bool LibraryFavoritesFile::contains(const FavoriteKey& key) const {
  return std::binary_search(keys.begin(), keys.end(), key);
}

bool LibraryFavoritesFile::toggle(const FavoriteKey& key) {
  const auto it = std::lower_bound(keys.begin(), keys.end(), key);
  if (it != keys.end() && *it == key) {
    keys.erase(it);
    // The RAM state already changed; the log is the only trace if the card
    // refused the write, and the next successful toggle re-persists everything.
    if (!save()) LOG_ERR("LIBFAV", "favorite removed in RAM but not saved");
    return false;
  }
  if (keys.size() >= FAVORITES_MAX) {
    LOG_ERR("LIBFAV", "favorites full (%u); not adding", static_cast<unsigned>(FAVORITES_MAX));
    return false;
  }
  keys.insert(it, key);
  if (!save()) LOG_ERR("LIBFAV", "favorite added in RAM but not saved");
  return true;
}

bool LibraryFavoritesFile::reanchor(const FavoriteKey& from, const FavoriteKey& to) {
  if (!reanchorFavoriteKey(keys, from, to)) return false;
  if (!save()) LOG_ERR("LIBFAV", "favorite re-anchored in RAM but not saved");
  return true;
}

bool LibraryFavoritesFile::save() const {
  std::vector<uint8_t> bytes;
  serializeFavorites(keys, bytes);

  HalFile file;
  if (!Storage.openFileForWrite("LIBFAV", FAV_NEW_PATH, file)) {
    LOG_ERR("LIBFAV", "cannot open %s for write", FAV_NEW_PATH);
    return false;
  }
  const size_t written = file.write(bytes.data(), bytes.size());
  file.close();
  if (written != bytes.size()) {
    LOG_ERR("LIBFAV", "short favorites write (%u of %u)", static_cast<unsigned>(written),
            static_cast<unsigned>(bytes.size()));
    Storage.remove(FAV_NEW_PATH);
    return false;
  }

  // The same install step the index build ends with: written beside, renamed
  // over. A power cut leaves either the old set or the new one, never a
  // half-written file.
  Storage.remove(FAV_PATH);
  if (!Storage.rename(FAV_NEW_PATH, FAV_PATH)) {
    LOG_ERR("LIBFAV", "cannot install %s", FAV_PATH);
    return false;
  }
  return true;
}

}  // namespace library
