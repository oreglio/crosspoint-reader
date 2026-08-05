#include "LibraryBuilder.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "LibraryText.h"

namespace library {
namespace {

constexpr char INDEX_PATH[] = "/.crosspoint/library.idx";
constexpr char NEW_PATH[] = "/.crosspoint/library.new";
constexpr char STAGE_PATH[] = "/.crosspoint/library.stage";
constexpr char CACHE_DIR[] = "/.crosspoint";

// Matches lib/FileIndex's buffer so a name this walk accepts is one the file
// browser could also show.
constexpr size_t NAME_BUF_SIZE = 512;

// One staged entry: the record as far as the filename can fill it, followed by
// the display name. Fixed stride keeps the second pass a seek rather than a scan.
constexpr size_t STAGE_NAME_BYTES = 255;
struct StagedEntry {
  ClixRecord record;
  char name[STAGE_NAME_BYTES];
};
constexpr size_t STAGE_STRIDE = sizeof(StagedEntry);

// Sort array element. Holding a 12-byte key prefix rather than the whole fold
// keeps this at 14 bytes per book; ties fall back to the ordinal, so the order
// is total and a rebuild cannot shuffle equal-prefix books between runs.
struct SortKey {
  char key[12];
  uint16_t ordinal;
};
static_assert(sizeof(SortKey) == 14, "SortKey must stay small: it is the only per-book resident cost");

bool sortKeyLess(const SortKey& a, const SortKey& b) {
  const int cmp = memcmp(a.key, b.key, sizeof(a.key));
  if (cmp != 0) return cmp < 0;
  return a.ordinal < b.ordinal;
}

ClixFormat formatForName(const std::string& name) {
  if (FsHelpers::checkFileExtension(name, ".epub")) return CLIX_FORMAT_EPUB;
  if (FsHelpers::checkFileExtension(name, ".txt")) return CLIX_FORMAT_TXT;
  if (FsHelpers::checkFileExtension(name, ".md")) return CLIX_FORMAT_MD;
  if (FsHelpers::checkFileExtension(name, ".xtc")) return CLIX_FORMAT_XTC;
  return CLIX_FORMAT_OTHER;
}

bool isBookName(const std::string& name) {
  return FsHelpers::checkFileExtension(name, ".epub") || FsHelpers::checkFileExtension(name, ".txt") ||
         FsHelpers::checkFileExtension(name, ".md") || FsHelpers::checkFileExtension(name, ".xtc");
}

// macOS AppleDouble sidecars and hidden entries. The file browser already hides
// these (FileBrowserActivity isMacOSMetadataEntry); the shelf must agree, or a
// card written on a Mac shows every book twice.
bool isHiddenOrSidecar(const char* name) { return name[0] == '.'; }

std::string stemOf(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  return (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
}

// State threaded through the recursive walk. Passed by reference rather than
// captured, so the walk stays a plain function and its stack frame stays small.
struct WalkState {
  HalFile stage;
  char* nameBuf = nullptr;
  uint16_t books = 0;
  uint16_t folderId = 0;
  uint32_t folderBytes = 0;
  uint32_t nameBytes = 0;
  uint32_t totalBookBytes = 0;
  uint16_t nextFirstSeen = 0;
  uint16_t duplicatesDropped = 0;
  uint16_t unreadableSkipped = 0;
  bool booksAtRoot = false;
  bool aborted = false;
  HalFile folders;  // folder section, staged separately then copied in
  BuildProgressFn onProgress = nullptr;
  void* progressCtx = nullptr;
};

void stageRecord(WalkState& st, const std::string& name, const uint32_t fileSize, const uint16_t folderId,
                 const std::string& parentBasename, const int depth) {
  StagedEntry entry{};
  const std::string stem = stemOf(name);
  const ParsedName parsed = parseFilename(stem);

  // The author may still be absent here. M2 fills it from the book's own
  // metadata; until then the row shows the title alone rather than guessing.
  std::string author = parsed.author;
  ClixAuthorProvenance provenance = author.empty() ? CLIX_AUTHOR_UNKNOWN : CLIX_AUTHOR_FROM_CACHE;

  // A parent folder becomes an author only below the top level. Without that
  // gate, a card organised by genre prints "Romans" and "Study" as authors —
  // measured at 35% of a real library.
  if (author.empty() && depth >= 2 && !parentBasename.empty()) {
    author = parentBasename;
    provenance = CLIX_AUTHOR_FROM_FOLDER;
  }

  const std::string folded = fold(parsed.title, true);
  const std::string key = authorKey(author);

  entry.record.nameOff = st.nameBytes;
  entry.record.fileSize = fileSize;
  entry.record.firstSeen = st.nextFirstSeen++;
  entry.record.folderId = folderId;
  entry.record.nameLen = static_cast<uint8_t>(std::min<size_t>(name.size(), STAGE_NAME_BYTES));
  entry.record.foldLen = static_cast<uint8_t>(std::min(folded.size(), CLIX_FOLD_BYTES));
  entry.record.authorKeyLen = static_cast<uint8_t>(std::min(key.size(), CLIX_AUTHOR_KEY_BYTES));
  entry.record.flags = makeRecordFlags(formatForName(name), provenance, false, false);
  memcpy(entry.record.fold, folded.data(), entry.record.foldLen);
  memcpy(entry.record.authorKey, key.data(), entry.record.authorKeyLen);
  memcpy(entry.name, name.data(), entry.record.nameLen);

  st.stage.write(reinterpret_cast<const uint8_t*>(&entry), STAGE_STRIDE);
  st.nameBytes += entry.record.nameLen;
  st.totalBookBytes += fileSize;
  st.books++;
}

void walk(WalkState& st, const std::string& path, const int depth) {
  if (st.aborted || depth > LIBRARY_MAX_DEPTH || st.books >= CLIX_MAX_RECORDS) return;

  HalFile dir = Storage.open(path.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  dir.rewindDirectory();

  // Names already staged from THIS directory. A damaged FAT can enumerate the
  // same entry twice; the second one would be a phantom book the user cannot
  // open. Bounded by the directory's own book count, and freed on return.
  std::vector<std::string> seen;
  std::vector<std::string> subdirs;

  const std::string basename = path.substr(path.find_last_of('/') + 1);
  bool folderEmitted = false;
  uint16_t myFolderId = 0;

  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (st.aborted || st.books >= CLIX_MAX_RECORDS) {
      entry.close();
      break;
    }
    st.nameBuf[0] = '\0';
    entry.getName(st.nameBuf, NAME_BUF_SIZE);
    const bool isDir = entry.isDirectory();
    const uint32_t size = isDir ? 0 : static_cast<uint32_t>(entry.fileSize());
    entry.close();

    if (st.nameBuf[0] == '\0' || isHiddenOrSidecar(st.nameBuf)) continue;
    const std::string name(st.nameBuf);

    if (isDir) {
      subdirs.push_back(name);
      continue;
    }
    if (!isBookName(name)) continue;

    // A zero-length book is a dangling directory entry: the name enumerates but
    // the contents do not exist. Counted rather than silently dropped.
    if (size == 0) {
      st.unreadableSkipped++;
      continue;
    }
    if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
      st.duplicatesDropped++;
      continue;
    }
    seen.push_back(name);

    if (!folderEmitted) {
      // Folders are emitted lazily, so only directories that actually hold a
      // book get an id and the ids stay dense.
      myFolderId = st.folderId++;
      const uint8_t pathLen = static_cast<uint8_t>(std::min<size_t>(path.size(), 255));
      st.folders.write(&pathLen, 1);
      st.folders.write(reinterpret_cast<const uint8_t*>(path.data()), pathLen);
      st.folderBytes += 1u + pathLen;
      folderEmitted = true;
      if (depth == 0) st.booksAtRoot = true;
    }
    stageRecord(st, name, size, myFolderId, basename, depth);
  }
  dir.close();

  if (st.onProgress != nullptr && !st.onProgress(st.books, path.c_str(), st.progressCtx)) {
    st.aborted = true;
    return;
  }

  // Subdirectories are walked after this directory's own handle is closed:
  // SdFat on hardware allows only one open reader at a time per path, and a
  // deep tree would otherwise hold a handle per level.
  for (const std::string& sub : subdirs) {
    walk(st, path + "/" + sub, depth + 1);
    if (st.aborted) return;
  }
}

// Assemble the final file from the two staging files and the title order.
//
// Written to a scratch path and renamed at the end, so a power cut during this
// leaves the previous index intact rather than a header claiming records that
// were never written. The header goes down twice: once as a placeholder, and
// once for real when the counts are known — the Dictionary.cpp idiom.
bool emitIndex(const char* folderStagePath, WalkState& st, const uint16_t* order, BuildStats& stats,
               const uint16_t previousNextFirstSeen) {
  const uint16_t n = st.books;

  // newOrdinalOf[stagingIndex] = position in title order. Needed because both
  // permutation arrays index the FINAL record order, not the walk order.
  auto newOrdinalOf = makeUniqueNoThrow<uint16_t[]>(n == 0 ? 1 : n);
  if (!newOrdinalOf) return false;
  for (uint16_t i = 0; i < n; i++) newOrdinalOf[order[i]] = i;

  ClixHeader header{};
  memcpy(header.magic, CLIX_MAGIC, sizeof(CLIX_MAGIC));
  header.formatVersion = CLIX_FORMAT_VERSION;
  header.foldVersion = CLIX_FOLD_VERSION;
  header.bookCount = n;
  header.folderCount = st.folderId;
  header.nextFirstSeen = st.nextFirstSeen;
  header.totalBookBytes = st.totalBookBytes;
  header.flags = CLIX_FLAG_WALK_COMPLETE | (stats.ranksDegraded ? CLIX_FLAG_RANKS_DEGRADED : 0) |
                 (stats.booksAtRoot ? CLIX_FLAG_BOOKS_AT_ROOT : 0);
  layoutSections(header, st.folderBytes, st.nameBytes);
  (void)previousNextFirstSeen;

  HalFile stage;
  HalFile out;
  if (!Storage.openFileForRead("LIBIDX", STAGE_PATH, stage)) return false;
  if (!Storage.openFileForWrite("LIBIDX", NEW_PATH, out)) {
    stage.close();
    return false;
  }

  const auto padTo = [&out](const uint32_t target) {
    static const uint8_t zeros[64] = {0};
    while (out.position() < target) {
      const uint32_t gap = target - static_cast<uint32_t>(out.position());
      out.write(zeros, std::min<uint32_t>(gap, sizeof(zeros)));
    }
  };

  // Header placeholder; rewritten below once authorRank/dateRank are known.
  out.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
  padTo(header.folderStart);

  {
    HalFile folders;
    if (Storage.openFileForRead("LIBIDX", folderStagePath, folders)) {
      uint8_t buf[256];
      size_t got = 0;
      while ((got = folders.read(buf, sizeof(buf))) > 0) out.write(buf, got);
      folders.close();
    }
  }
  padTo(header.recordStart);

  // Author order has to be known BEFORE the records are written, because each
  // record carries its own authorRank. So: read the keys, sort, invert, then
  // write. Books with no key sort last in both directions, which is why
  // knownAuthorCount is recorded rather than a second array being stored.
  auto authorSort = makeUniqueNoThrow<SortKey[]>(n == 0 ? 1 : n);
  auto authorRankOf = makeUniqueNoThrow<uint16_t[]>(n == 0 ? 1 : n);
  uint16_t known = 0;
  if (authorSort && authorRankOf) {
    for (uint16_t i = 0; i < n; i++) {
      ClixRecord r{};
      stage.seekSet(static_cast<uint64_t>(order[i]) * STAGE_STRIDE);
      stage.read(reinterpret_cast<uint8_t*>(&r), sizeof(r));
      if (r.authorKeyLen == 0) {
        // 0xFF outranks every folded byte, so unknown authors land at the end.
        memset(authorSort[i].key, 0xFF, sizeof(authorSort[i].key));
      } else {
        memset(authorSort[i].key, 0, sizeof(authorSort[i].key));
        memcpy(authorSort[i].key, r.authorKey,
               std::min<size_t>(r.authorKeyLen, sizeof(authorSort[i].key)));
        known++;
      }
      authorSort[i].ordinal = i;
    }
    if (n > 1) std::sort(authorSort.get(), authorSort.get() + n, sortKeyLess);
    for (uint16_t k = 0; k < n; k++) authorRankOf[authorSort[k].ordinal] = k;
  } else {
    for (uint16_t i = 0; i < n; i++) {
      if (authorRankOf) authorRankOf[i] = i;
    }
    stats.ranksDegraded = true;
  }
  header.knownAuthorCount = known;

  // Records, in title order, with both ranks and the name offset filled in.
  //
  // nameOff MUST be recomputed here. The walk assigns offsets in discovery
  // order, but the name blob below is written in title order, so a staged offset
  // points at whatever name happened to be staged at that position — which
  // renders as the tail of one name glued to the head of the next.
  uint32_t nameCursor = 0;
  for (uint16_t i = 0; i < n; i++) {
    StagedEntry entry{};
    stage.seekSet(static_cast<uint64_t>(order[i]) * STAGE_STRIDE);
    stage.read(reinterpret_cast<uint8_t*>(&entry), STAGE_STRIDE);
    entry.record.nameOff = nameCursor;
    nameCursor += entry.record.nameLen;
    // firstSeen is handed out sequentially during the walk and carried across
    // rebuilds, so the position in first-seen order is the offset from the value
    // this build started at — not the raw counter.
    entry.record.dateRank = static_cast<uint16_t>(entry.record.firstSeen - previousNextFirstSeen);
    entry.record.authorRank = authorRankOf ? authorRankOf[i] : i;
    out.write(reinterpret_cast<const uint8_t*>(&entry.record), sizeof(ClixRecord));
  }
  padTo(header.permStart);

  for (uint16_t k = 0; k < n; k++) {
    const uint16_t ordinal = authorSort ? authorSort[k].ordinal : k;
    out.write(reinterpret_cast<const uint8_t*>(&ordinal), sizeof(ordinal));
  }
  // dateOrder: staging index order IS first-seen order, so the k-th oldest book
  // is whatever title-order position the k-th staged record ended up at.
  for (uint16_t k = 0; k < n; k++) {
    const uint16_t ordinal = newOrdinalOf[k];
    out.write(reinterpret_cast<const uint8_t*>(&ordinal), sizeof(ordinal));
  }
  padTo(header.nameStart);

  for (uint16_t i = 0; i < n; i++) {
    StagedEntry entry{};
    stage.seekSet(static_cast<uint64_t>(order[i]) * STAGE_STRIDE);
    stage.read(reinterpret_cast<uint8_t*>(&entry), STAGE_STRIDE);
    out.write(reinterpret_cast<const uint8_t*>(entry.name), entry.record.nameLen);
  }
  stage.close();

  out.seekSet(0);
  out.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
  out.close();

  // Rename last. Until this line the previous index is still the live one.
  Storage.remove(INDEX_PATH);
  if (!Storage.rename(NEW_PATH, INDEX_PATH)) {
    LOG_ERR("LIBIDX", "rename %s -> %s failed", NEW_PATH, INDEX_PATH);
    Storage.remove(NEW_PATH);
    return false;
  }
  return true;
}

}  // namespace

const char* libraryIndexPath() { return INDEX_PATH; }
const char* libraryStagePath() { return STAGE_PATH; }

bool buildLibraryIndex(const char* rootPath, const uint16_t previousNextFirstSeen, BuildStats& stats,
                       const BuildProgressFn onProgress, void* progressCtx) {
  const uint32_t startMs = millis();
  stats = BuildStats{};

  Storage.mkdir(CACHE_DIR);
  Storage.remove(STAGE_PATH);
  const std::string folderStagePath = std::string(STAGE_PATH) + ".f";
  Storage.remove(folderStagePath.c_str());

  auto nameBuf = makeUniqueNoThrow<char[]>(NAME_BUF_SIZE);
  if (!nameBuf) {
    LOG_ERR("LIBIDX", "name buffer alloc failed (%u bytes)", static_cast<unsigned>(NAME_BUF_SIZE));
    return false;
  }

  WalkState st;
  st.nameBuf = nameBuf.get();
  st.nextFirstSeen = previousNextFirstSeen;
  st.onProgress = onProgress;
  st.progressCtx = progressCtx;

  if (!Storage.openFileForWrite("LIBIDX", STAGE_PATH, st.stage) ||
      !Storage.openFileForWrite("LIBIDX", folderStagePath, st.folders)) {
    LOG_ERR("LIBIDX", "cannot open staging files");
    if (st.stage) st.stage.close();
    if (st.folders) st.folders.close();
    return false;
  }

  walk(st, rootPath, 0);
  st.stage.close();
  st.folders.close();

  if (st.aborted) {
    LOG_INF("LIBIDX", "build cancelled after %u books", static_cast<unsigned>(st.books));
    Storage.remove(STAGE_PATH);
    Storage.remove(folderStagePath.c_str());
    return false;
  }

  stats.books = st.books;
  stats.folders = st.folderId;
  stats.duplicatesDropped = st.duplicatesDropped;
  stats.unreadableSkipped = st.unreadableSkipped;
  stats.booksAtRoot = st.booksAtRoot;

  // --- title order -----------------------------------------------------------
  // Read the staged fold prefixes back and sort ordinals. Only 14 bytes per book
  // stays resident, and past the cap the index is still complete — just in walk
  // order, which the header records so the screen can say so.
  const bool sortable = st.books <= LIBRARY_MAX_SORTED;
  auto order = makeUniqueNoThrow<uint16_t[]>(st.books == 0 ? 1 : st.books);
  if (!order) {
    LOG_ERR("LIBIDX", "order array alloc failed (%u books)", static_cast<unsigned>(st.books));
    Storage.remove(STAGE_PATH);
    Storage.remove(folderStagePath.c_str());
    return false;
  }
  for (uint16_t i = 0; i < st.books; i++) order[i] = i;

  if (sortable && st.books > 1) {
    auto keys = makeUniqueNoThrow<SortKey[]>(st.books);
    HalFile stage;
    if (keys && Storage.openFileForRead("LIBIDX", STAGE_PATH, stage)) {
      for (uint16_t i = 0; i < st.books; i++) {
        ClixRecord r{};
        stage.seekSet(static_cast<uint64_t>(i) * STAGE_STRIDE);
        stage.read(reinterpret_cast<uint8_t*>(&r), sizeof(r));
        memset(keys[i].key, 0, sizeof(keys[i].key));
        memcpy(keys[i].key, r.fold, std::min<size_t>(r.foldLen, sizeof(keys[i].key)));
        keys[i].ordinal = i;
      }
      stage.close();
      std::sort(keys.get(), keys.get() + st.books, sortKeyLess);
      for (uint16_t i = 0; i < st.books; i++) order[i] = keys[i].ordinal;
    } else {
      stats.ranksDegraded = true;
      LOG_ERR("LIBIDX", "sort skipped: key array alloc or stage reopen failed");
    }
  } else if (!sortable) {
    stats.ranksDegraded = true;
    LOG_INF("LIBIDX", "%u books exceeds sort cap %u; index built in walk order",
            static_cast<unsigned>(st.books), static_cast<unsigned>(LIBRARY_MAX_SORTED));
  }

  const bool ok = emitIndex(folderStagePath.c_str(), st, order.get(), stats, previousNextFirstSeen);
  Storage.remove(STAGE_PATH);
  Storage.remove(folderStagePath.c_str());

  stats.walkMs = millis() - startMs;
  LOG_INF("LIBIDX", "%s: %u books, %u folders, %u dup dropped, %u unreadable, %ums",
          ok ? "built" : "FAILED", static_cast<unsigned>(stats.books), static_cast<unsigned>(stats.folders),
          static_cast<unsigned>(stats.duplicatesDropped), static_cast<unsigned>(stats.unreadableSkipped),
          static_cast<unsigned>(stats.walkMs));
  return ok;
}

}  // namespace library
