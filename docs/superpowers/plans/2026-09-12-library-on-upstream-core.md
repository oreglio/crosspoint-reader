# Library on upstream's index core — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace our Library index core with upstream's, keep our shelf screen, so future upstream syncs cost no conflicts and the non-Latin search fix arrives for free.

**Architecture:** Four core files are copied verbatim from
`crosspoint/feat/library-view@ad949bdd` and never edited again. Our screen,
favorites, shelf state and their tests live in files upstream has no copy of.
One shared file, `lib/Epub/Epub.{h,cpp}`, gains a single delegating method so
their builder compiles here.

**Tech Stack:** C++17, ESP-IDF / Arduino-ESP32 via PlatformIO, GoogleTest on the host via CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-09-12-library-on-upstream-core-design.md`

## Global Constraints

- Upstream base is pinned at `crosspoint/feat/library-view@ad949bdd`. Every
  `git show` in this plan uses that ref literally. Quote the ref in shell
  commands (`git show 'crosspoint/feat/library-view:path'`) — zsh eats
  `$VAR:l` as a modifier.
- The four adopted core files are **never edited**. If one seems to need a
  change, stop and raise it — editing them dissolves the only reason this
  work exists.
- `CrossPointSettings::saveToFile()` is private. Use `saveGlobalDefaults()`.
- No exceptions, no `abort()`. `new` is not nothrow: use
  `makeUniqueNoThrow<T>()` from `lib/Memory/Memory.h`.
- All user-facing strings go through `tr(STR_*)`. Logs stay hardcoded.
- Never edit generated files: `lib/I18n/I18nKeys.h`, `I18nStrings.{h,cpp}`
  come from `scripts/gen_i18n.py`.
- Formatting: `clang-format -i` every touched `.cpp`/`.h` before committing.
- Build gates: `pio run -e default`, `-e sticky`, `-e x4-pro`.
- Host tests: `cmake -S test -B test/build && cmake --build test/build -j8 && (cd test/build && ctest -j8)`.
  Two `SectionPersistenceTest` failures are a known upstream defect and are
  **not** yours — everything else must pass.

---

### Task 1: Move the bounded EPUB metadata reader into `lib/Epub`

`LibraryBuilder.cpp` (upstream's, arriving in Task 2) calls
`epub.loadMetadata(title, author)`. We have no such method. Defining it in
`Epub.cpp` in terms of `lib/LibraryIndex/LibraryMeta.h` would close a
dependency cycle — `LibraryBuilder.cpp:4` already includes `<Epub.h>`, so
`lib/Epub` must not depend on `lib/LibraryIndex`.

`LibraryMeta` only needs `Logging`, `Memory`, `Print` and `ZipFile`, all
independent libraries, and reading `dc:title` / `dc:creator` out of an EPUB
belongs with the EPUB code anyway. Move it, and rename it so nothing in
`lib/Epub` is called "Library".

**Files:**
- Create: `lib/Epub/EpubQuickMetadata.h` (moved from `lib/LibraryIndex/LibraryMeta.h`)
- Create: `lib/Epub/EpubQuickMetadata.cpp` (moved from `lib/LibraryIndex/LibraryMeta.cpp`)
- Delete: `lib/LibraryIndex/LibraryMeta.{h,cpp}`
- Modify: `lib/Epub/Epub.h` (declare `loadMetadata`)
- Modify: `lib/Epub/Epub.cpp` (define `loadMetadata`)
- Modify: `lib/LibraryIndex/LibraryBuilder.cpp:16` (our current builder's include, so the tree still builds)

**Interfaces:**
- Consumes: nothing.
- Produces: `bool Epub::loadMetadata(std::string& title, std::string& author)`;
  `epub::BookMetadata { std::string title; std::string author; bool opfTooLarge; }`;
  `bool epub::readBookMetadata(const std::string& epubPath, BookMetadata& out)`.

- [ ] **Step 1: Move the two files and rename the namespace**

```bash
cd /Users/aurelien-edusign/Code/xteink/x3/CrossInkLibrary
git mv lib/LibraryIndex/LibraryMeta.h   lib/Epub/EpubQuickMetadata.h
git mv lib/LibraryIndex/LibraryMeta.cpp lib/Epub/EpubQuickMetadata.cpp
sed -i '' 's/namespace library {/namespace epub {/; s|}  // namespace library|}  // namespace epub|' \
  lib/Epub/EpubQuickMetadata.h lib/Epub/EpubQuickMetadata.cpp
sed -i '' 's/#include "LibraryMeta.h"/#include "EpubQuickMetadata.h"/' lib/Epub/EpubQuickMetadata.cpp
```

- [ ] **Step 2: Explain the move at the top of the moved header**

Insert immediately below the `#pragma once` in `lib/Epub/EpubQuickMetadata.h`:

```cpp
// Lives in lib/Epub rather than lib/LibraryIndex on purpose. The library index
// builder includes <Epub.h>, so lib/Epub must not depend on lib/LibraryIndex or
// the two libraries form a cycle. Reading dc:title and dc:creator out of an
// EPUB is EPUB work regardless of who asked for it.
```

- [ ] **Step 3: Declare the method**

In `lib/Epub/Epub.h`, immediately after the `parseContentOpf` declaration (line 101-102):

```cpp
  // Title and author only, for the library index build. Delegates to
  // EpubQuickMetadata, whose 8 KB inflate bound is what keeps this off the
  // abort() path on the C3 — see EpubQuickMetadata.h for the measurement.
  // Returns false whenever the caller should fall back to the filename.
  bool loadMetadata(std::string& title, std::string& author);
```

- [ ] **Step 4: Define the method**

In `lib/Epub/Epub.cpp`, add the include beside the other local includes:

```cpp
#include "EpubQuickMetadata.h"
```

and the body at the end of the file, before any closing namespace:

```cpp
bool Epub::loadMetadata(std::string& title, std::string& author) {
  epub::BookMetadata metadata;
  if (!epub::readBookMetadata(getPath(), metadata)) {
    return false;
  }
  title = metadata.title;
  author = metadata.author;
  return true;
}
```

- [ ] **Step 5: Keep our current builder compiling**

In `lib/LibraryIndex/LibraryBuilder.cpp`, line 16, replace:

```cpp
#include "LibraryMeta.h"
```

with:

```cpp
#include <EpubQuickMetadata.h>
```

Then update every `library::readBookMetadata` / `library::BookMetadata` use in
that file to `epub::`:

```bash
sed -i '' 's/library::readBookMetadata/epub::readBookMetadata/g; s/library::BookMetadata/epub::BookMetadata/g' \
  lib/LibraryIndex/LibraryBuilder.cpp
grep -n "readBookMetadata\|BookMetadata" lib/LibraryIndex/LibraryBuilder.cpp
```

Expected: every hit reads `epub::`.

- [ ] **Step 6: Drop the two tests that referenced the old namespace**

`test/library_text/LibraryTextTest.cpp:223` and `:231` define
`LibraryMetadataGuard` tests. That whole suite is replaced in Task 2, so
delete just those two `TEST(...)` blocks now to keep the tree green:

```bash
grep -n "TEST(LibraryMetadataGuard" test/library_text/LibraryTextTest.cpp
```

Delete both blocks, from each `TEST(LibraryMetadataGuard` line through its
closing `}`.

- [ ] **Step 7: Build the firmware**

Run: `pio run -e default`
Expected: SUCCESS. Behaviour is unchanged — nothing calls `loadMetadata` yet.

- [ ] **Step 8: Run the host tests**

Run: `cmake -S test -B test/build && cmake --build test/build -j8 && (cd test/build && ctest -j8)`
Expected: only the two known `SectionPersistenceTest` failures.

- [ ] **Step 9: Commit**

```bash
clang-format -i lib/Epub/EpubQuickMetadata.h lib/Epub/EpubQuickMetadata.cpp lib/Epub/Epub.h lib/Epub/Epub.cpp lib/LibraryIndex/LibraryBuilder.cpp
git add lib/Epub lib/LibraryIndex/LibraryBuilder.cpp test/library_text/LibraryTextTest.cpp
git commit -m "refactor(epub): move the bounded metadata reader into lib/Epub

Upstream's library index builder calls Epub::loadMetadata(). Defining it
against lib/LibraryIndex would make lib/Epub depend on the library index,
which already includes <Epub.h> — a cycle. The reader only needs ZipFile,
Logging, Memory and Print, and reading dc:title/dc:creator is EPUB work, so
it moves and loses its Library- prefix.

No behaviour change: loadMetadata() has no caller yet."
```

---

### Task 2: Adopt upstream's index core and its four test suites

The firmware will **not** build at the end of this task — our screen still
calls the old API and is repaired in Task 5. The gate here is the host tests:
their suites passing on our tree is what proves the core works in this repo.

**Files:**
- Replace: `lib/LibraryIndex/LibraryBuilder.{cpp,h}`, `LibraryFormat.{cpp,h}`, `LibraryIndexFile.{cpp,h}`, `LibraryText.{cpp,h}`
- Replace: `test/library_format/`, `test/library_text/`
- Create: `test/library_builder/`, `test/library_index_file/`
- Modify: `test/CMakeLists.txt:47-49`

**Interfaces:**
- Consumes: `Epub::loadMetadata` from Task 1.
- Produces: `library::SortOrder { AddedAsc, AddedDesc, TitleAsc, TitleDesc, AuthorAsc, AuthorDesc }`;
  `bool library::buildLibraryIndex(const char* rootPath, BuildStats& stats, bool readMetadata = false)`;
  `library::ClixRecord` with `metadataStatus` and `modificationTime` and **without** `authorRank`, `dateRank`, `flags`;
  `bool LibraryIndexFile::readSourceAuthor(const ClixRecord&, std::string&)`;
  `library::ClixMetadataStatus { CLIX_METADATA_NOT_ATTEMPTED = 0, CLIX_METADATA_EXTRACTED = 1, CLIX_METADATA_FAILED = 2 }`.

- [ ] **Step 1: Copy the eight core files verbatim**

```bash
cd /Users/aurelien-edusign/Code/xteink/x3/CrossInkLibrary
for f in LibraryBuilder.cpp LibraryBuilder.h LibraryFormat.cpp LibraryFormat.h \
         LibraryIndexFile.cpp LibraryIndexFile.h LibraryText.cpp LibraryText.h; do
  git show "crosspoint/feat/library-view:lib/LibraryIndex/$f" > "lib/LibraryIndex/$f"
done
git status --short lib/LibraryIndex/
```

Expected: the eight files modified, and `LibraryFavorites*`, `LibraryState*` untouched.

- [ ] **Step 2: Copy the four test suites verbatim**

```bash
rm -rf test/library_format test/library_text
for d in library_builder library_format library_index_file library_text; do
  mkdir -p "test/$d"
  git ls-tree -r --name-only 'crosspoint/feat/library-view' -- "test/$d" | while read -r p; do
    mkdir -p "$(dirname "$p")"
    git show "crosspoint/feat/library-view:$p" > "$p"
  done
done
ls test/library_builder test/library_index_file
```

Expected: `CMakeLists.txt`, the test `.cpp`, and a `stubs/` directory in each.

- [ ] **Step 3: Register the two new suites**

In `test/CMakeLists.txt`, replace lines 47-49:

```cmake
add_subdirectory(library_text)
add_subdirectory(library_format)
add_subdirectory(library_favorites)
```

with:

```cmake
add_subdirectory(library_text)
add_subdirectory(library_format)
add_subdirectory(library_favorites)
add_subdirectory(library_builder)
add_subdirectory(library_index_file)
```

- [ ] **Step 4: Confirm the core is byte-identical to the pinned base**

```bash
git diff --stat 'crosspoint/feat/library-view' -- \
  lib/LibraryIndex/LibraryBuilder.cpp lib/LibraryIndex/LibraryBuilder.h \
  lib/LibraryIndex/LibraryFormat.cpp lib/LibraryIndex/LibraryFormat.h \
  lib/LibraryIndex/LibraryIndexFile.cpp lib/LibraryIndex/LibraryIndexFile.h \
  lib/LibraryIndex/LibraryText.cpp lib/LibraryIndex/LibraryText.h
```

Expected: **empty output**. Anything else means an edit crept in; undo it
rather than keeping it.

- [ ] **Step 5: Build and run the host tests**

Run: `cmake -S test -B test/build && cmake --build test/build -j8 && (cd test/build && ctest -j8)`
Expected: `LibraryBuilderTest`, `LibraryFormatTest`, `LibraryIndexFileTest`,
`LibraryTextTest` all pass, plus our `LibraryFavoritesTest`. Only the two
known `SectionPersistenceTest` failures remain.

Do **not** run `pio run` here; the firmware is knowingly broken until Task 5.

- [ ] **Step 6: Commit**

```bash
git add lib/LibraryIndex test/library_builder test/library_format test/library_index_file test/library_text test/CMakeLists.txt
git commit -m "feat(library): adopt upstream's index core verbatim

Eight files from crosspoint/feat/library-view@ad949bdd, copied unedited, plus
their four test suites — library_builder and library_index_file are new here.

This brings the non-Latin fold() (Greek incl. polytonic, Cyrillic, Han and
compatibility ideographs), which our copy never had, and makes every future
upstream fix to the core a copy rather than a merge.

The firmware does not build at this commit: the shelf screen still calls the
old API and is repaired two commits later. Host tests are the gate here."
```

---

### Task 3: Bump `library.state` to version 2

Their `SortOrder` reorders the values our state file persists raw
(`LibraryState.cpp` writes `raw[2] = shelfSort`). `STATE_VERSION` stays 1 and
the `raw[2] >= SORT_COUNT` guard still passes because `SORT_COUNT` rises to 6,
so a v1 file would be silently misread: `Recent` (3) reopens as Title Z-A,
`Author` (2) as Title A-Z.

Bumping the version makes a v1 file fail validation and fall back to defaults,
which the format's own header already promises costs nothing. Their enum also
encodes direction in the value, so `titleDescending` becomes redundant.

`LibraryState.cpp` mixes byte parsing with `HalStorage` I/O, so it cannot be
host-tested as it stands. The repo already solves this for the neighbouring
format: `LibraryFavorites.cpp` is pure (`parseFavorites` / `serializeFavorites`)
and `LibraryFavoritesFile.cpp` does the I/O. Mirror that split here rather than
building a storage harness for two asserts.

One wrinkle the split does not remove: the codec needs `SortOrder`, which lives
in `LibraryIndexFile.h`, and that header includes `<HalStorage.h>`. So the test
does need a storage stub after all — but not a new one. Task 2 brought
`test/library_index_file/stubs/{HalStorage.h,Logging.h}` into the tree, and the
repo already shares stub directories between suites (`test/epub_grayscale`
points at `../memory_policy/stubs`). Point at those rather than writing more.

**Files:**
- Create: `lib/LibraryIndex/LibraryStateCodec.h`
- Create: `lib/LibraryIndex/LibraryStateCodec.cpp`
- Modify: `lib/LibraryIndex/LibraryState.h`
- Modify: `lib/LibraryIndex/LibraryState.cpp:38-60` (load) and the `saveLibraryState` body below it
- Create: `test/library_state/CMakeLists.txt`
- Create: `test/library_state/LibraryStateTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `library::SortOrder` from Task 2.
- Produces: `library::LibraryShelfState { bool favoritesView; SortOrder shelfSort; SortOrder favSort; FavoriteKey selected; }` — **no** `titleDescending`;
  `constexpr size_t library::SHELF_STATE_BYTES = 12`;
  `bool library::parseShelfState(const uint8_t* data, size_t len, LibraryShelfState& out)`;
  `void library::serializeShelfState(const LibraryShelfState& state, uint8_t* out)`.

- [ ] **Step 1: Write the failing test**

Create `test/library_state/LibraryStateTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include "LibraryStateCodec.h"

// A v1 file was written when SortOrder meant {TitleAsc, TitleDesc, AuthorAsc,
// DateDesc}. Read under the new enum, its bytes turn "Recent" into "Title Z-A"
// without a word — and the old range guard would not catch it, because
// SORT_COUNT rose from 4 to 6. Version 2 refuses the file instead.
TEST(ShelfStateCodec, RejectsAVersion1FileRatherThanMisreadingIt) {
  uint8_t v1[library::SHELF_STATE_BYTES] = {};
  v1[0] = 1;  // the old STATE_VERSION
  v1[1] = 0;
  v1[2] = 3;  // old DateDesc; the new enum reads 3 as TitleDesc
  v1[3] = 3;

  library::LibraryShelfState out;
  EXPECT_FALSE(library::parseShelfState(v1, sizeof(v1), out));
}

TEST(ShelfStateCodec, RoundTripsDirectionThroughTheSortOrderAlone) {
  library::LibraryShelfState written;
  written.favoritesView = true;
  written.shelfSort = library::SortOrder::AuthorDesc;
  written.favSort = library::SortOrder::TitleAsc;
  written.selected = library::FavoriteKey{0xDEADBEEF, 4096};

  uint8_t bytes[library::SHELF_STATE_BYTES] = {};
  library::serializeShelfState(written, bytes);

  library::LibraryShelfState read;
  ASSERT_TRUE(library::parseShelfState(bytes, sizeof(bytes), read));
  EXPECT_TRUE(read.favoritesView);
  EXPECT_EQ(read.shelfSort, library::SortOrder::AuthorDesc);
  EXPECT_EQ(read.favSort, library::SortOrder::TitleAsc);
  EXPECT_EQ(read.selected.nameHash, 0xDEADBEEFu);
  EXPECT_EQ(read.selected.fileSize, 4096u);
}

TEST(ShelfStateCodec, RejectsASortValueOutsideTheEnum) {
  library::LibraryShelfState written;
  uint8_t bytes[library::SHELF_STATE_BYTES] = {};
  library::serializeShelfState(written, bytes);
  bytes[2] = 6;  // one past AuthorDesc

  library::LibraryShelfState read;
  EXPECT_FALSE(library::parseShelfState(bytes, sizeof(bytes), read));
}
```

Create `test/library_state/CMakeLists.txt`, modelled on the favorites suite:

```cmake
add_executable(LibraryStateTest
  LibraryStateTest.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryStateCodec.cpp
)

target_include_directories(LibraryStateTest PRIVATE
  ../library_index_file/stubs
  ${REPO_ROOT}/lib/LibraryIndex
)

target_link_libraries(LibraryStateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LibraryStateTest)
```

Register it in `test/CMakeLists.txt` beside the other library suites:

```cmake
add_subdirectory(library_state)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S test -B test/build && cmake --build test/build --target LibraryStateTest -j8`
Expected: FAIL — `LibraryStateCodec.h` does not exist. (If it instead fails on
a missing `Arduino.h` or `HalStorage.h`, the stub path above is wrong — fix the
include directory, not the codec.)

- [ ] **Step 3: Write the pure codec**

Create `lib/LibraryIndex/LibraryStateCodec.h`:

```cpp
#pragma once

// The byte layer of /.crosspoint/library.state, split out from the file I/O so
// it can be host-tested — the same split LibraryFavorites/LibraryFavoritesFile
// already uses, and for the same reason.
//
// Little-endian, exactly 12 bytes:
//   u8  version       currently 2
//   u8  flags         bit0 favorites view
//   u8  shelfSort     SortOrder of the tab strip, direction included
//   u8  favSort       SortOrder of the ★ view, direction included
//   u32 selNameHash   \ identity of the selected book — the same pair
//   u32 selFileSize   / favorites key by; 0,0 means none
//
// Version 1 is refused rather than reinterpreted: SortOrder's values were
// reordered when the index core came from upstream, so the same byte names a
// different sort now, and the old range check cannot see it because the enum
// also grew from four values to six.

#include <cstddef>
#include <cstdint>

#include "LibraryFavorites.h"
#include "LibraryIndexFile.h"

namespace library {

inline constexpr uint8_t SHELF_STATE_VERSION = 2;
inline constexpr size_t SHELF_STATE_BYTES = 12;

struct LibraryShelfState {
  bool favoritesView = false;
  SortOrder shelfSort = SortOrder::AddedDesc;
  SortOrder favSort = SortOrder::AddedDesc;
  FavoriteKey selected{};  // nameHash 0 and fileSize 0 = none
};

// False for a wrong version, a wrong length, or a sort value outside the enum.
// `out` is left untouched on failure, so the caller keeps its defaults.
bool parseShelfState(const uint8_t* data, size_t len, LibraryShelfState& out);

// Writes exactly SHELF_STATE_BYTES bytes.
void serializeShelfState(const LibraryShelfState& state, uint8_t* out);

}  // namespace library
```

Create `lib/LibraryIndex/LibraryStateCodec.cpp`:

```cpp
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
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build test/build --target LibraryStateTest -j8 && ./test/build/library_state/LibraryStateTest`
Expected: all three tests PASS.

- [ ] **Step 5: Point the file layer at the codec**

In `lib/LibraryIndex/LibraryState.h`, delete the `LibraryShelfState` struct and
the 12-byte layout comment — both now live in `LibraryStateCodec.h` — and
include the codec instead of `LibraryFavorites.h` / `LibraryIndexFile.h`:

```cpp
#include "LibraryStateCodec.h"
```

Leave the rest of that header alone: `loadLibraryState`, `saveLibraryState`,
`reanchorLibraryStateSelection`, `libraryStatePath`, `markShelfStaleIfBook` and
`takeShelfStale` keep their declarations and their comments.

In `lib/LibraryIndex/LibraryState.cpp`, delete the anonymous-namespace
constants that moved (`STATE_VERSION`, `STATE_BYTES`, `FLAG_FAV_VIEW`,
`FLAG_TITLE_DESC`, `SORT_COUNT`, `readU32`, `putU32`), keep the path constants,
and replace the body of `loadLibraryState` from line 38 with:

```cpp
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
```

Then rewrite `saveLibraryState`'s byte-packing to one call:

```cpp
  uint8_t raw[SHELF_STATE_BYTES];
  serializeShelfState(state, raw);
```

leaving its write-beside-then-rename install step exactly as it is.

- [ ] **Step 6: Confirm the firmware side still compiles in isolation**

The shelf screen still sets `state.titleDescending` at
`LibraryListActivity.cpp:187`; that line is removed in Task 5, and the firmware
stays knowingly broken until then. Do not `pio run` here.

Run: `cmake --build test/build -j8 && (cd test/build && ctest -j8)`
Expected: `LibraryStateTest` passes; only the two known
`SectionPersistenceTest` failures remain.

- [ ] **Step 7: Commit**

```bash
clang-format -i lib/LibraryIndex/LibraryStateCodec.h lib/LibraryIndex/LibraryStateCodec.cpp lib/LibraryIndex/LibraryState.h lib/LibraryIndex/LibraryState.cpp test/library_state/LibraryStateTest.cpp
git add lib/LibraryIndex/LibraryStateCodec.h lib/LibraryIndex/LibraryStateCodec.cpp lib/LibraryIndex/LibraryState.h lib/LibraryIndex/LibraryState.cpp test/library_state test/CMakeLists.txt
git commit -m "fix(library): refuse a version 1 shelf state instead of misreading it

SortOrder's values were reordered when the index core came from upstream, and
library.state persists the ordinal raw. A v1 file would have silently reopened
every shelf on the wrong sort — Recent as Title Z-A, Author as Title A-Z —
with SORT_COUNT rising to 6 so the range guard still passed.

Version 2 falls back to defaults, which this format's header already promises
costs nothing. The 'titles descending' flag bit goes too: direction now lives
in the SortOrder value itself."
```

---

### Task 4: Delete the provenance strings and default metadata on

The author now comes from the book's metadata or nowhere, so the provenance
line can only say one thing and is removed. The setting that gates extraction
must default to on, or the shelf has no authors at all.

**Files:**
- Modify: `lib/I18n/translations/english.yaml:279-281`
- Modify: `lib/I18n/translations/french.yaml:171-173`
- Modify: `src/CrossPointSettings.h:640`

**Interfaces:**
- Produces: `StrId::STR_LIBRARY_PROV_FOLDER`, `_CACHE` and `_OPF` no longer exist;
  `CrossPointSettings::libraryUseMetadata` defaults to 1.

- [ ] **Step 1: Delete the three keys**

Remove lines 279-281 of `lib/I18n/translations/english.yaml`
(`STR_LIBRARY_PROV_FOLDER`, `STR_LIBRARY_PROV_CACHE`, `STR_LIBRARY_PROV_OPF`)
and lines 171-173 of `lib/I18n/translations/french.yaml`. Only these two files
carry them.

- [ ] **Step 2: Default metadata extraction on**

In `src/CrossPointSettings.h`, line 640:

```cpp
  // Defaults on: upstream's index derives an author only from the book's own
  // metadata -- it never parses a filename -- so with extraction off the shelf
  // would have no authors at all. Affordable because Epub::loadMetadata keeps
  // an 8 KB inflate bound and the builder re-parses only changed books.
  uint8_t libraryUseMetadata = 1;
```

- [ ] **Step 3: Regenerate and confirm the keys are gone**

```bash
python3 scripts/gen_i18n.py
grep -rn "STR_LIBRARY_PROV" lib/I18n/ src/ || echo "provenance keys gone"
```

Expected: "provenance keys gone". If `src/` still references one, Task 5 has
not run yet — that is expected at this point and is not a failure of this task.

- [ ] **Step 4: Commit**

```bash
git add lib/I18n src/CrossPointSettings.h
git commit -m "feat(library): author comes from metadata, so default extraction on

Upstream's index never parses a filename for an author -- \"an absent author is
a fact, not a gap to fill\". Their extraction is gated on readMetadata, and our
libraryUseMetadata defaulted to 0, so the two together would have produced a
shelf with no authors at all.

The provenance line goes with the filename source: with one source left it
could only ever say the same thing."
```

---

### Task 5: Repair the build against the new core

**Files:**
- Modify: `src/activities/library/LibraryListActivity.h` (add the format enum)
- Modify: `src/activities/library/LibraryListActivity.cpp` (sort orders, provenance, format, builder calls)
- Modify: `src/activities/settings/SettingsActivity.cpp:1113-1124`

**Interfaces:**
- Consumes: everything Task 2 produces, `Task 4`'s two `StrId`s, Task 3's struct.
- Produces: a firmware that builds on all three targets.

- [ ] **Step 1: Give `HalFile` a modification time**

Upstream's builder calls `entry.modificationTime()` (`LibraryBuilder.cpp:467`)
and our `HalFile` has no such method, so the core does not compile here. This
is the same kind of seam as `Epub::loadMetadata` in Task 1: a small, deliberate
edit to a shared file, outside the verbatim boundary.

It is not cosmetic. `reuseMetadata` requires `modificationTime != 0`
(`LibraryBuilder.cpp:316`); without it no prior record is ever reused and every
rebuild re-parses every book's metadata — which is the cost argument for
defaulting `libraryUseMetadata` to 1.

Port upstream's implementation rather than inventing one. In
`lib/hal/HalStorage.h`, beside the other `HalFile` accessors:

```cpp
  // FAT modify date and time packed into one word, date in the high half.
  // Zero when the card carries no timestamp for this entry, which the library
  // index reads as "cannot be trusted for reuse".
  uint32_t modificationTime();
```

In `lib/hal/HalStorage.cpp`, beside the other wrapped calls:

```cpp
uint32_t HalFile::modificationTime() {
  HalStorage::StorageLock lock;
  uint16_t date = 0;
  uint16_t time = 0;
  if (!impl || !impl->file.getModifyDateTime(&date, &time) || date == 0) return 0;
  return (static_cast<uint32_t>(date) << 16) | time;
}
```

`FsFile::getModifyDateTime(uint16_t*, uint16_t*)` is SdFat's, declared at
`FsFile.h:305`; `HalFile::Impl` already wraps an `FsFile`.

- [ ] **Step 2: Give the format label a home**

Upstream's `LibraryFormat.h` has neither `ClixFormat` nor `recordFormat()`.
Add to `src/activities/library/LibraryListActivity.h`, above the class:

```cpp
// Upstream's index does not store a format field, so the shelf derives it from
// the file name — the only place it is used is the Details page's size line.
enum class ShelfFormat : uint8_t { Epub, Txt, Md, Xtc, Other };
ShelfFormat shelfFormatForName(std::string_view name);
```

and the definition in the anonymous namespace at the top of
`LibraryListActivity.cpp`:

```cpp
ShelfFormat shelfFormatForName(const std::string_view name) {
  const auto endsWith = [name](const std::string_view suffix) {
    return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  if (endsWith(".epub")) return ShelfFormat::Epub;
  if (endsWith(".txt")) return ShelfFormat::Txt;
  if (endsWith(".md")) return ShelfFormat::Md;
  if (endsWith(".xtc") || endsWith(".xtch")) return ShelfFormat::Xtc;
  return ShelfFormat::Other;
}
```

- [ ] **Step 3: Delete the provenance block, keep the format label**

In `LibraryListActivity.cpp`, delete the whole provenance `switch` and its
`drawBlock` (lines 1067-1083) — everything between the author `drawBlock` and
the closing brace of `if (!author.empty())`. The author line itself stays; only
the line naming where it came from goes.

Then replace `switch (library::recordFormat(record))` (line 1094) with
`switch (shelfFormatForName(name))`, renaming each case label from
`library::CLIX_FORMAT_EPUB` to `ShelfFormat::Epub` and so on.

- [ ] **Step 4: Move the sort orders onto their enum**

Replace every `library::SortOrder::DateDesc` with `library::SortOrder::AddedDesc`:

```bash
grep -rn "SortOrder::DateDesc" src lib
sed -i '' 's/SortOrder::DateDesc/SortOrder::AddedDesc/g' \
  src/activities/library/LibraryListActivity.cpp src/activities/library/LibraryListActivity.h
```

- [ ] **Step 5: Make both `SortOrder` switches exhaustive again**

The enum grew from four values to six, and neither switch has a `default:` on
purpose, so `-Werror=switch` will reject both.

First `sortTabIndex` (line 69). The five-slot strip is still in place here —
Task 6 is what collapses it — so the two new orders map onto the existing
tabs:

```cpp
int sortTabIndex(const library::SortOrder order) {
  switch (order) {
    case library::SortOrder::TitleAsc:
      return kTitleAscTab;
    case library::SortOrder::TitleDesc:
      return kTitleDescTab;
    case library::SortOrder::AuthorAsc:
    case library::SortOrder::AuthorDesc:
      return kAuthorTab;
    case library::SortOrder::AddedAsc:
    case library::SortOrder::AddedDesc:
      return kRecentTab;
  }
  return kRecentTab;
}
```

Then `LibraryListActivity::sortOrderLabel()` (around line 438):

```cpp
const char* LibraryListActivity::sortOrderLabel() const {
  switch (sSortOrder) {
    case library::SortOrder::AddedDesc:
      return tr(STR_LIBRARY_SORT_RECENT);
    case library::SortOrder::AddedAsc:
      return tr(STR_LIBRARY_SORT_OLDEST);
    case library::SortOrder::TitleAsc:
      return tr(STR_LIBRARY_SORT_TITLE_AZ);
    case library::SortOrder::TitleDesc:
      return tr(STR_LIBRARY_SORT_TITLE_ZA);
    case library::SortOrder::AuthorAsc:
      return tr(STR_LIBRARY_SORT_AUTHOR);
    case library::SortOrder::AuthorDesc:
      return tr(STR_LIBRARY_SORT_AUTHOR_ZA);
  }
  return "";
}
```

Add the two new keys to `lib/I18n/translations/english.yaml` and
`french.yaml` beside the existing `STR_LIBRARY_SORT_*` entries, then rerun
`python3 scripts/gen_i18n.py`:

```yaml
STR_LIBRARY_SORT_OLDEST: "Oldest first"
STR_LIBRARY_SORT_AUTHOR_ZA: "Author Z-A"
```

```yaml
STR_LIBRARY_SORT_OLDEST: "Les plus anciens d'abord"
STR_LIBRARY_SORT_AUTHOR_ZA: "Auteur Z-A"
```

- [ ] **Step 6: Stop writing the flag that no longer exists**

`LibraryListActivity.cpp:187` still sets `state.titleDescending`, which Task 3
removed from the struct. Delete that one line from `onExit()`; `state.shelfSort`
on the next line already carries the direction.

- [ ] **Step 7: Simplify both builder call sites**

In `src/activities/library/LibraryListActivity.cpp`, replace the block at
lines 208-222 with:

```cpp
  library::BuildStats stats;
  // Their builder feeds the task watchdog itself — LibraryBuilder.cpp:86 is
  // `if ((++workUnits & 0x1Fu) == 0) delay(1);`, with more delays through the
  // metadata and sort passes. The progress callback we used to pass for that
  // reason no longer exists, and re-adding one would be dead weight.
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
```

Delete the now-unused `carriedFirstSeen` preamble above it — upstream's builder
reconciles through `openForReconciliation()` internally.

In `src/activities/settings/SettingsActivity.cpp`, replace the call at lines
1117-1124 the same way:

```cpp
        library::BuildStats stats;
        const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
```

and delete its `carried` preamble.

- [ ] **Step 8: Prove no reference to a removed symbol survives**

The deletions above are described by content, not by symbol. This turns them
into something checkable. Every one of these names something the new core no
longer has, or that this task removes:

```bash
grep -n "recordAuthorProvenance\|recordFormat\|CLIX_FORMAT_\|CLIX_AUTHOR_\|SortOrder::DateDesc\|titleDescending\|nextFirstSeen\|STR_LIBRARY_PROV\|BuildProgressFn" \
  src/activities/library/LibraryListActivity.cpp src/activities/library/LibraryListActivity.h \
  src/activities/settings/SettingsActivity.cpp
```

Expected: **no output**. Any hit is a deletion you made partially — finish it
before building, because the compiler will only report the first few.

- [ ] **Step 9: Build all three targets**

```bash
pio run -e default && pio run -e sticky && pio run -e x4-pro
```

Expected: SUCCESS on all three. Fix whatever the compiler names; do **not**
resolve anything by editing the eight adopted core files.

- [ ] **Step 10: Run the host tests**

Run: `cmake --build test/build -j8 && (cd test/build && ctest -j8)`
Expected: only the two known `SectionPersistenceTest` failures.

- [ ] **Step 11: Commit**

```bash
clang-format -i src/activities/library/LibraryListActivity.h src/activities/library/LibraryListActivity.cpp src/activities/settings/SettingsActivity.cpp
git add src/activities/library src/activities/settings/SettingsActivity.cpp
git commit -m "feat(library): run the shelf on upstream's index core

SortOrder::DateDesc becomes AddedDesc; the format label is derived from the
file name now that the record has no format field; the provenance line is gone
with the filename-derived author it used to describe.

Both builder call sites lose their progress callback and firstSeen preamble.
The callback was our only watchdog feed — upstream's builder feeds it itself
at LibraryBuilder.cpp:86, which is the only reason dropping it is safe."
```

---

### Task 6: Four tabs, uniform hold-to-flip, no gap above the strip

**Files:**
- Modify: `src/activities/library/LibraryListActivity.cpp:62-110` and the strip handlers
- Modify: `src/activities/library/LibraryListActivity.cpp:1198-1204` (content margin)

**Interfaces:**
- Consumes: `library::SortOrder` with both directions per order.
- Produces: a four-slot strip, `★ | Time | Title ▾ | Author`.

- [ ] **Step 1: Replace the tab constants**

In `LibraryListActivity.cpp`, replace lines 62-67:

```cpp
constexpr int kFavTab = 0;
constexpr int kRecentTab = 1;
constexpr int kTitleAscTab = 2;
constexpr int kTitleDescTab = 3;
constexpr int kAuthorTab = 4;
constexpr int kTabSlots = kAuthorTab + 1;
```

with:

```cpp
constexpr int kFavTab = 0;
constexpr int kTimeTab = 1;
constexpr int kTitleTab = 2;
constexpr int kAuthorTab = 3;
constexpr int kTabSlots = kAuthorTab + 1;
```

- [ ] **Step 2: Fold direction into the tab, not into a slot**

Replace `sortTabIndex` (lines 69-80) and `orderForTab` (lines 83-88):

```cpp
int sortTabIndex(const library::SortOrder order) {
  switch (order) {
    case library::SortOrder::AddedAsc:
    case library::SortOrder::AddedDesc:
      return kTimeTab;
    case library::SortOrder::TitleAsc:
    case library::SortOrder::TitleDesc:
      return kTitleTab;
    case library::SortOrder::AuthorAsc:
    case library::SortOrder::AuthorDesc:
      return kAuthorTab;
  }
  return kTimeTab;
}

// Ascending is the resting state of every tab except Time, where "newest
// first" is what a reader means by recently added.
library::SortOrder orderForTab(const int tab, const bool descending) {
  if (tab == kTitleTab) return descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  if (tab == kAuthorTab) return descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
  return descending ? library::SortOrder::AddedDesc : library::SortOrder::AddedAsc;
}

bool orderIsDescending(const library::SortOrder order) {
  return order == library::SortOrder::AddedDesc || order == library::SortOrder::TitleDesc ||
         order == library::SortOrder::AuthorDesc;
}
```

- [ ] **Step 3: Label the tabs and carry the arrow**

Replace `tabLabelFor` (lines 92-98):

```cpp
// The arrow is appended by tabLabel(), which knows the active order; this
// returns the bare mode so an inactive tab stays quiet.
const char* tabLabelFor(const int tab) {
  if (tab == kTimeTab) return tr(STR_LIBRARY_TAB_RECENT);
  if (tab == kTitleTab) return tr(STR_LIBRARY_TAB_TITLES);
  if (tab == kAuthorTab) return tr(STR_LIBRARY_TAB_AUTHOR);
  return nullptr;
}
```

and replace `LibraryListActivity::tabLabel` (line 104):

```cpp
const char* LibraryListActivity::tabLabel(const int index) const {
  const char* base = tabLabelFor(index);
  if (base == nullptr || index != activeTab()) return base;
  // Only the active tab shows which way it runs; a row of arrows reads as noise.
  // That invariant is what makes one shared buffer safe here:
  // UiTabListActivity.cpp:121 collects every tab's pointer into one array and
  // renders them together, so a second arrow-bearing tab would overwrite the
  // first. The inactive tabs return stable tr() pointers instead.
  static char withArrow[64];
  snprintf(withArrow, sizeof(withArrow), "%s %s", base, orderIsDescending(sSortOrder) ? "▾" : "▴");
  return withArrow;
}
```

- [ ] **Step 4: Give each tab a resting direction when it is activated**

`onTabAction` (line 413) calls the old single-argument `orderForTab`. Replace
that call:

```cpp
    sSortOrder = orderForTab(index, /*descending=*/index == kTimeTab);
```

Time rests on newest-first because that is what "recently added" means to a
reader; Title and Author rest ascending. Leave the rest of `onTabAction`
untouched — `applyFilter()`, the nav reset, `app.clearTapFlash()` and
`requestUpdate()` all still apply.

- [ ] **Step 5: Make a hold on the focused tab flip its direction**

Replace `LibraryListActivity::onTabLongPress` (line 430) in full:

```cpp
void LibraryListActivity::onTabLongPress(const int index) {
  if (index == kFavTab) {
    openFavoritesSortMenu();
    return;
  }
  // A hold on the tab you are already on flips its direction; a hold on
  // another tab is just a slow tap, and onTabAction has already run for it.
  if (index != activeTab()) return;
  sSortOrder = orderForTab(index, !orderIsDescending(sSortOrder));
  applyFilter();
  app.clearTapFlash();
  requestUpdate();
}
```

Nothing writes the state here: `onExit()` is the single write per visit, and it
reads `sSortOrder` directly.

- [ ] **Step 6: Remove the gap above the strip**

Replace lines 1202-1204:

```cpp
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing), 0});
```

with:

```cpp
  // No topPadding: the header band already ends where the strip begins, and
  // the extra pad read as a stray gap above the tabs on the device.
  screen.setContentMargin(
      fui::Insets{TouchHeaderBackButton::height(metrics, mappedInput), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing), 0});
```

- [ ] **Step 7: Build and run the simulator**

```bash
pio run -e default && pio run -e simulator
```

Expected: SUCCESS on both.

- [ ] **Step 8: Commit**

```bash
clang-format -i src/activities/library/LibraryListActivity.cpp
git add src/activities/library/LibraryListActivity.cpp
git commit -m "feat(library): four sort tabs, direction on a hold

A-Z and Z-A stop paying for a tab each. The strip is now star, Time, Title and
Author; a hold on the tab you are already on flips its direction and the arrow
follows. Uniform across all three orders because upstream's index offers both
directions for each, where ours only had them for titles.

The stray gap above the strip goes with it: the header band already ends where
the tabs begin."
```

---

### Task 7: Document the format, measure the cost

**Files:**
- Modify: `docs/file-formats.md:670-727` (the CLX1 section)
- Modify: `.claude/CONTEXT.md`

- [ ] **Step 1: Correct the CLX1 section**

`docs/file-formats.md:676` says "Format version 3". The adopted core is
version 2. Replace that paragraph with:

```markdown
Format version 2, and deliberately lower than the 3 this fork used to write:
the core now comes verbatim from upstream (crosspoint `feat/library-view`), so
its version line is theirs, not ours. Validation is an equality test, so a
version 3 index written by an older build of this firmware fails it and is
rebuilt — which is the entire migration mechanism.
```

Then reconcile the record layout table in that section with the adopted
`ClixRecord`: `authorRank`, `dateRank` and `flags` are gone; `metadataStatus`
and `modificationTime` are new. Read
`lib/LibraryIndex/LibraryFormat.h` and describe what is actually there.

- [ ] **Step 2: Record the boundary where the next session will look**

Append to the "Divergences assumées vis-à-vis d'upstream" section of
`.claude/CONTEXT.md`:

```markdown
- Le noyau d'index Library (`LibraryBuilder`, `LibraryFormat`,
  `LibraryIndexFile`, `LibraryText`) vient **verbatim** de
  `crosspoint/feat/library-view` et ne doit jamais être édité : c'est ce qui
  rend les syncs suivantes gratuites. Nos ajouts vivent dans
  `LibraryFavorites*`, `LibraryState*` et `LibraryListActivity`. Vérifier avec
  `git diff crosspoint/feat/library-view -- lib/LibraryIndex/LibraryBuilder.cpp ...`
  — la sortie doit être vide.
- `lib/Epub/EpubQuickMetadata.{cpp,h}` est là plutôt que dans `lib/LibraryIndex`
  parce que le builder inclut `<Epub.h>` : l'inverse créerait un cycle.
```

- [ ] **Step 3: Add the CHANGELOG entry**

`CLAUDE.md` requires a user-facing entry for every feature change. Add under
`## [Unreleased]`, in the existing `### Changed` and `### Removed` sections:

```markdown
- The Library now takes a book's author from the book's own metadata rather
  than guessing it from the file name, and reads that metadata by default. A
  book that carries no author of its own joins the Unknown group instead of
  borrowing a name from its file name or its folder. Searching and sorting now
  work on Greek, Cyrillic and CJK libraries, which they never did before.
- The Library sort strip is four tabs instead of five: ★, Time, Title and
  Author. Hold the tab you are already on to reverse its direction — the arrow
  on the tab shows which way it runs.
```

```markdown
- The Library book details no longer name where the author came from. With the
  author now taken only from the book itself, the line could only ever say one
  thing.
```

- [ ] **Step 4: Measure the flash delta**

```bash
pio run -e x4-pro 2>&1 | grep "Flash:"
```

Record the number in the commit message against the pre-change 94.2 %
(6,171,977 of 6,553,600 bytes). The two-screen alternative was rejected partly
on flash headroom, so the real delta belongs on the record.

- [ ] **Step 5: Commit**

```bash
git add docs/file-formats.md .claude/CONTEXT.md CHANGELOG.md
git commit -m "docs(library): CLX1 is version 2 now, and the core is upstream's

The adopted core writes format version 2 where this fork wrote 3. Validation is
an equality test so old indexes still rebuild correctly, but the version line
is no longer monotonic and the record layout changed — authorRank, dateRank and
flags out, metadataStatus and modificationTime in.

x4-pro flash after: <fill from step 3> (was 94.2%)."
```

---

## Device verification

Not a task — this is the gate before promoting past a beta. On an X4, in this
order, because each step tests one assumption:

1. First Library entry after the update: the index rebuilds **and the
   favorites are still there**. Record free heap and largest allocatable block
   during the rebuild — there is no prior baseline for either builder, so this
   is the first number we will have.
2. The shelf reopens on **defaults**, not on a misread sort — Task 3 working.
3. A hold flips direction on `Time`, `Title` and `Author`, the arrow follows,
   and the choice survives a power cycle.
4. Search an accented title; then a Cyrillic or CJK title if one is to hand —
   the case that was dead before.
5. Details on a book whose author came from its file name, then on one with
   real metadata: the two lines must differ, and **every** book must show one.
6. Delete a book, return to the shelf: no ghost row.
7. With `libraryUseMetadata` ON, rebuild over a large library: no watchdog
   reboot.

Clear `.crosspoint/epub_<hash>/` caches only if a book behaves oddly; the
library index rebuilds itself and needs no manual clearing.
