#include "LibraryBuilder.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include <Epub.h>
#include <FsHelpers.h>

#include "LibraryIndexFile.h"
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
constexpr size_t STAGE_AUTHOR_BYTES = 128;
struct StagedEntry {
  ClixRecord record;
  char name[STAGE_NAME_BYTES];
  // Display spelling as this one filename gave it. The spelling actually shown
  // is chosen later, across every book by the same person.
  uint8_t authorLen;
  char author[STAGE_AUTHOR_BYTES];
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

// One book as the PREVIOUS index knew it, kept only long enough to recognise the
// same book in the new walk.
//
// The name is held as a 32-bit hash rather than as text: 512 real names are
// ~40 KB, the hashes are 6 KB, and the size check beside it makes a hash
// collision harmless. Identity is (name, size) — a name whose size changed is a
// different file, and gets re-read.
struct PriorEntry {
  uint32_t nameHash;
  uint32_t size;
  uint16_t firstSeen;
  bool matched;
};

uint32_t fnv1a32(const char* data, const size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 16777619u;
  }
  return hash;
}

// Sentinel written into a staged record whose book matched nothing by (name,
// size). A second pass decides whether it is a rename or genuinely new.
constexpr uint16_t FIRST_SEEN_UNRESOLVED = 0xFFFF;

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
  bool readMetadata = false;
  uint16_t enriched = 0;
  HalFile folders;  // folder section, staged separately then copied in
  BuildProgressFn onProgress = nullptr;
  void* progressCtx = nullptr;
  // Books the previous index knew. Empty on a first build, in which case every
  // book is new and gets a fresh firstSeen.
  PriorEntry* prior = nullptr;
  uint16_t priorCount = 0;
  uint16_t reused = 0;
};

// Find the previous record for this exact file. Linear because the array is at
// most a few hundred entries and this runs once per book during a walk that is
// already dominated by SD seeks.
int findPrior(WalkState& st, const uint32_t nameHash, const uint32_t size) {
  for (uint16_t i = 0; i < st.priorCount; i++) {
    if (!st.prior[i].matched && st.prior[i].nameHash == nameHash && st.prior[i].size == size) return i;
  }
  return -1;
}

void stageRecord(WalkState& st, const std::string& name, const uint32_t fileSize, const uint16_t folderId,
                 const std::string& parentBasename, const int depth, const std::string& fullPath) {
  StagedEntry entry{};
  const std::string stem = stemOf(name);
  ParsedName parsed = parseFilename(stem);
  bool titleFromBook = false;

  // A book the reader has already opened carries its own title and author in a
  // cache beside it. Reading that is a small file read; building it is the
  // reader's whole indexing pass, so buildIfMissing stays false and a book never
  // opened simply keeps the name it has on disk.
  if (st.readMetadata && FsHelpers::hasEpubExtension(name)) {
    Epub epub(fullPath, CACHE_DIR);
    if (epub.load(false, true, Epub::XLocationLoadMode::Skip)) {
      if (!epub.getTitle().empty()) {
        parsed.title = epub.getTitle();
        titleFromBook = true;
      }
      if (!epub.getAuthor().empty()) {
        parsed.author = epub.getAuthor();
        titleFromBook = true;
      }
    }
  }
  if (titleFromBook) st.enriched++;

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

  // Reuse the arrival order this book already had. Without this every rebuild
  // renumbers the whole library in disk-walk order, and "Recently added" silently
  // becomes "whatever order the card enumerates in".
  const int priorIndex = findPrior(st, fnv1a32(name.data(), name.size()), fileSize);
  if (priorIndex >= 0) {
    st.prior[priorIndex].matched = true;
    entry.record.firstSeen = st.prior[priorIndex].firstSeen;
    st.reused++;
  } else {
    // Might be a rename rather than a new book; resolved after the walk, when
    // the set of genuinely unmatched previous entries is known.
    entry.record.firstSeen = FIRST_SEEN_UNRESOLVED;
  }
  entry.record.folderId = folderId;
  entry.record.nameLen = static_cast<uint8_t>(std::min<size_t>(name.size(), STAGE_NAME_BYTES));
  entry.record.foldLen = static_cast<uint8_t>(std::min(folded.size(), CLIX_FOLD_BYTES));
  entry.record.authorKeyLen = static_cast<uint8_t>(std::min(key.size(), CLIX_AUTHOR_KEY_BYTES));
  entry.record.flags = makeRecordFlags(formatForName(name), provenance, titleFromBook, false);
  memcpy(entry.record.fold, folded.data(), entry.record.foldLen);
  memcpy(entry.record.authorKey, key.data(), entry.record.authorKeyLen);
  memcpy(entry.name, name.data(), entry.record.nameLen);

  const std::string displayAuthor = cleanPersonName(author);
  entry.authorLen = static_cast<uint8_t>(std::min(displayAuthor.size(), STAGE_AUTHOR_BYTES));
  memcpy(entry.author, displayAuthor.data(), entry.authorLen);

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
    stageRecord(st, name, size, myFolderId, basename, depth, path + "/" + name);
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
bool emitIndex(const char* folderStagePath, WalkState& st, const uint16_t* order,
               const uint16_t* resolvedFirstSeen, BuildStats& stats) {
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
  // The blob is the LAST section, so its size affects only selfSize — every
  // section offset is already fixed by the counts. Lay out with a placeholder
  // and correct selfSize once the blob has actually been written, since the
  // author spelling each record ends up carrying is not known until the
  // one-spelling-per-person pass has run.
  layoutSections(header, st.folderBytes, 0);

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

  // --- one spelling per person --------------------------------------------
  //
  // The author KEY already merges "Xiaolong, Qiu", "Qiu Xiaolong_" and
  // "Qiu Xiaolong [Xiaolong, Qiu]" into one identity, because its tokens are
  // sorted. The displayed STRING is still whatever each filename happened to
  // carry, so one person appears under several spellings in the same list.
  //
  // Fix: within each key group show the spelling that occurs most often, ties
  // broken by the shortest and then alphabetically. It never invents or reorders
  // a name — it picks one of the strings that actually exist — which is what
  // keeps "Qiu Xiaolong" and "Min Jin Lee" safe from a forename/surname rule
  // that would confidently get them backwards.
  //
  // authorSort is already grouped: books by one person are contiguous in it. So
  // this is one walk over the runs, holding only the current run's spellings.
  auto canonicalFrom = makeUniqueNoThrow<uint16_t[]>(n == 0 ? 1 : n);
  if (canonicalFrom) {
    for (uint16_t i = 0; i < n; i++) canonicalFrom[i] = i;
  }
  if (canonicalFrom && authorSort && n > 1) {
    uint16_t runStart = 0;
    while (runStart < n) {
      uint16_t runEnd = runStart + 1;
      while (runEnd < n && memcmp(authorSort[runEnd].key, authorSort[runStart].key,
                                  sizeof(authorSort[runStart].key)) == 0) {
        runEnd++;
      }
      // A run of one has nothing to reconcile, and the unknown-author run (key
      // all 0xFF) must not be collapsed onto one arbitrary empty string.
      const bool unknownRun = static_cast<unsigned char>(authorSort[runStart].key[0]) == 0xFF;
      if (!unknownRun && runEnd - runStart > 1) {
        uint16_t bestOrdinal = authorSort[runStart].ordinal;
        int bestScore = -1;
        size_t bestLen = 0;
        std::string bestText;
        for (uint16_t a = runStart; a < runEnd; a++) {
          StagedEntry ea{};
          stage.seekSet(static_cast<uint64_t>(order[authorSort[a].ordinal]) * STAGE_STRIDE);
          stage.read(reinterpret_cast<uint8_t*>(&ea), STAGE_STRIDE);
          if (ea.authorLen == 0) continue;
          const std::string textA(ea.author, ea.authorLen);

          int score = 0;
          for (uint16_t b = runStart; b < runEnd; b++) {
            StagedEntry eb{};
            stage.seekSet(static_cast<uint64_t>(order[authorSort[b].ordinal]) * STAGE_STRIDE);
            stage.read(reinterpret_cast<uint8_t*>(&eb), STAGE_STRIDE);
            if (eb.authorLen == ea.authorLen && memcmp(eb.author, ea.author, ea.authorLen) == 0) score++;
          }
          const bool better = score > bestScore || (score == bestScore && textA.size() < bestLen) ||
                              (score == bestScore && textA.size() == bestLen && textA < bestText);
          if (better) {
            bestScore = score;
            bestLen = textA.size();
            bestText = textA;
            bestOrdinal = authorSort[a].ordinal;
          }
        }
        for (uint16_t a = runStart; a < runEnd; a++) canonicalFrom[authorSort[a].ordinal] = bestOrdinal;
      }
      runStart = runEnd;
    }
  }

  // --- arrival order -------------------------------------------------------
  //
  // firstSeen values now come from the PREVIOUS index, so they are no longer a
  // dense sequence in walk order: a rebuild reuses each book's original number
  // and only hands out new ones for books it has never seen. The date order has
  // to be SORTED rather than assumed, or "Recently added" silently degrades into
  // "the order the card enumerates in" — which is exactly the bug reconciliation
  // exists to prevent.
  auto dateSort = makeUniqueNoThrow<SortKey[]>(n == 0 ? 1 : n);
  auto dateRankOf = makeUniqueNoThrow<uint16_t[]>(n == 0 ? 1 : n);
  if (dateSort && dateRankOf) {
    for (uint16_t i = 0; i < n; i++) {
      ClixRecord r{};
      stage.seekSet(static_cast<uint64_t>(order[i]) * STAGE_STRIDE);
      stage.read(reinterpret_cast<uint8_t*>(&r), sizeof(r));
      // Big-endian into the key so memcmp orders numerically.
      const uint16_t seen = resolvedFirstSeen ? resolvedFirstSeen[order[i]] : r.firstSeen;
      memset(dateSort[i].key, 0, sizeof(dateSort[i].key));
      dateSort[i].key[0] = static_cast<char>(seen >> 8);
      dateSort[i].key[1] = static_cast<char>(seen & 0xFF);
      dateSort[i].ordinal = i;
    }
    if (n > 1) std::sort(dateSort.get(), dateSort.get() + n, sortKeyLess);
    for (uint16_t k = 0; k < n; k++) dateRankOf[dateSort[k].ordinal] = k;
  } else {
    stats.ranksDegraded = true;
  }

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
    // The blob holds the basename, then one length byte, then the chosen author
    // spelling. Keeping them adjacent means no second offset has to live in the
    // record, which is exactly full at 128 bytes.
    StagedEntry canonical{};
    const uint16_t from = canonicalFrom ? canonicalFrom[i] : i;
    stage.seekSet(static_cast<uint64_t>(order[from]) * STAGE_STRIDE);
    stage.read(reinterpret_cast<uint8_t*>(&canonical), STAGE_STRIDE);
    // Must match the blob loop below exactly, or every name after the first
    // divergence renders as a slice of its neighbours.
    nameCursor += entry.record.nameLen + 1u + canonical.authorLen;
    if (resolvedFirstSeen) entry.record.firstSeen = resolvedFirstSeen[order[i]];
    entry.record.dateRank = dateRankOf ? dateRankOf[i] : i;
    entry.record.authorRank = authorRankOf ? authorRankOf[i] : i;
    out.write(reinterpret_cast<const uint8_t*>(&entry.record), sizeof(ClixRecord));
  }
  padTo(header.permStart);

  for (uint16_t k = 0; k < n; k++) {
    const uint16_t ordinal = authorSort ? authorSort[k].ordinal : k;
    out.write(reinterpret_cast<const uint8_t*>(&ordinal), sizeof(ordinal));
  }
  for (uint16_t k = 0; k < n; k++) {
    const uint16_t ordinal = dateSort ? dateSort[k].ordinal : newOrdinalOf[k];
    out.write(reinterpret_cast<const uint8_t*>(&ordinal), sizeof(ordinal));
  }
  padTo(header.nameStart);

  uint32_t blobWritten = 0;
  for (uint16_t i = 0; i < n; i++) {
    StagedEntry entry{};
    stage.seekSet(static_cast<uint64_t>(order[i]) * STAGE_STRIDE);
    stage.read(reinterpret_cast<uint8_t*>(&entry), STAGE_STRIDE);
    out.write(reinterpret_cast<const uint8_t*>(entry.name), entry.record.nameLen);

    StagedEntry canonical{};
    const uint16_t from = canonicalFrom ? canonicalFrom[i] : i;
    stage.seekSet(static_cast<uint64_t>(order[from]) * STAGE_STRIDE);
    stage.read(reinterpret_cast<uint8_t*>(&canonical), STAGE_STRIDE);
    out.write(&canonical.authorLen, 1);
    if (canonical.authorLen > 0) out.write(reinterpret_cast<const uint8_t*>(canonical.author), canonical.authorLen);
    blobWritten += entry.record.nameLen + 1u + canonical.authorLen;
  }
  header.nameLen = blobWritten;
  header.selfSize = header.nameStart + blobWritten;
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
                       const bool readMetadata, const BuildProgressFn onProgress, void* progressCtx) {
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

  // Load what the previous index knew, so the walk can recognise the same books.
  // Failure here is not fatal: the build simply treats every book as new.
  std::unique_ptr<PriorEntry[]> priorList;
  uint16_t priorCount = 0;
  {
    LibraryIndexFile previous;
    if (previous.open(INDEX_PATH)) {
      priorCount = previous.bookCount();
      priorList = makeUniqueNoThrow<PriorEntry[]>(priorCount == 0 ? 1 : priorCount);
      if (priorList) {
        uint16_t kept = 0;
        for (uint16_t i = 0; i < priorCount; i++) {
          ClixRecord r{};
          std::string name;
          if (!previous.readRecord(i, r) || !previous.readName(r, name)) continue;
          priorList[kept].nameHash = fnv1a32(name.data(), name.size());
          priorList[kept].size = r.fileSize;
          priorList[kept].firstSeen = r.firstSeen;
          priorList[kept].matched = false;
          kept++;
        }
        priorCount = kept;
      } else {
        priorCount = 0;
      }
    }
  }

  WalkState st;
  st.nameBuf = nameBuf.get();
  st.nextFirstSeen = previousNextFirstSeen;
  st.prior = priorList.get();
  st.priorCount = priorList ? priorCount : 0;
  st.readMetadata = readMetadata;
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

  // --- second pass: renames, then genuinely new books ----------------------
  //
  // Resolved into RAM, never by rewriting the staging file: openFileForWrite
  // opens with O_TRUNC (SDCardManager.cpp:308), so reopening the staging file to
  // patch it empties it, and every record read afterwards comes back blank.
  // Two bytes per book is a cheaper price than that failure mode.
  //
  // A book that matched nothing by (name, size) is either renamed or new. Match
  // it against the leftover previous entries by SIZE alone: across a real
  // library, two different books sharing a byte-exact size is implausible, and
  // being wrong only costs one book its place in "Recently added" and one
  // re-read. A content hash would settle it properly but would read ~12 KB per
  // book on every single verification, to decide a case that arises when someone
  // renames a file.
  auto resolvedFirstSeen = makeUniqueNoThrow<uint16_t[]>(st.books == 0 ? 1 : st.books);
  if (!resolvedFirstSeen) {
    LOG_ERR("LIBIDX", "firstSeen array alloc failed");
    Storage.remove(STAGE_PATH);
    Storage.remove(folderStagePath.c_str());
    return false;
  }
  if (!st.aborted && st.books > 0) {
    HalFile read;
    if (Storage.openFileForRead("LIBIDX", STAGE_PATH, read)) {
      for (uint16_t i = 0; i < st.books; i++) {
        ClixRecord r{};
        read.seekSet(static_cast<uint64_t>(i) * STAGE_STRIDE);
        if (read.read(reinterpret_cast<uint8_t*>(&r), sizeof(r)) != static_cast<int>(sizeof(r))) break;
        if (r.firstSeen != FIRST_SEEN_UNRESOLVED) {
          resolvedFirstSeen[i] = r.firstSeen;
          continue;
        }
        int renamed = -1;
        for (uint16_t q = 0; q < priorCount; q++) {
          if (priorList && !priorList[q].matched && priorList[q].size == r.fileSize) {
            renamed = q;
            break;
          }
        }
        if (renamed >= 0) {
          priorList[renamed].matched = true;
          resolvedFirstSeen[i] = priorList[renamed].firstSeen;
          stats.renamed++;
        } else {
          resolvedFirstSeen[i] = st.nextFirstSeen++;
          stats.added++;
        }
      }
      read.close();
    }
    for (uint16_t q = 0; q < priorCount; q++) {
      if (priorList && !priorList[q].matched) stats.removed++;
    }
  }

  stats.books = st.books;
  stats.folders = st.folderId;
  stats.duplicatesDropped = st.duplicatesDropped;
  stats.unreadableSkipped = st.unreadableSkipped;
  stats.booksAtRoot = st.booksAtRoot;
  stats.unchanged = st.reused;
  stats.enriched = st.enriched;

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

  const bool ok = emitIndex(folderStagePath.c_str(), st, order.get(), resolvedFirstSeen.get(), stats);
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
