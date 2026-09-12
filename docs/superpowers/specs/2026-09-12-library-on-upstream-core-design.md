# Library — our shelf on upstream's index core

Status: designed, not started. 2026-09-12. Revised after review the same day.
Base: `sync/upstream-20260912`.

**Pinned upstream base: `crosspoint/feat/library-view` at `ad949bdd`**
(2026-09-12, "Simplify library navigation to unified ring model").

The verbatim boundary below is meaningless without a fixed point, so re-pin
deliberately and re-verify this spec's claims when doing so — never let a
`git fetch` silently change what "verbatim" means.

Re-pinned from `d4411be9` on 2026-09-12 and re-verified. The single intervening
commit touches only *their* screen — `LibraryListActivity.{cpp,h}`,
`src/components/icons/blocks.h`, `english.yaml`. A `git diff` across the whole
adopted boundary (`lib/LibraryIndex/`, `lib/Epub/`, the four test suites)
returns empty, so every claim below was confirmed unchanged:
`CLIX_FORMAT_VERSION = 2`, no `ClixFormat`, `Epub::loadMetadata` present,
`buildLibraryIndex(rootPath, stats, readMetadata)`, the watchdog feed at
`LibraryBuilder.cpp:86`, the `loadMetadata` call at line 348, 1333 builder
lines, and the `SortOrder` table below.

## Problem

Upstream merged our Library series (#2885-#2889, #3256) into their
`feat/library-view` branch and Uri-Tauber is integrating it as #3366. Since
that merge their copy of the index core has kept evolving while ours has too.
Three consequences:

1. Their core gained fixes we want — most concretely, `fold()` now covers
   Greek (`0x0370-0x03FF`, plus polytonic `0x1F00-0x1FFF`), Cyrillic
   (`0x0400-0x052F`) and Han (`0x3400-0x4DBF`, `0x4E00-0x9FFF`, plus
   compatibility ideographs `0xF900-0xFAFF`). Ours covers none of them, which
   is the known limitation that leaves search and sort dead on a non-Latin
   library.
2. Their screen is not the one we want. It scrolls where ours paginates, and
   it carries none of the v1.1 layer: favorites, delete, details, the
   long-press menu, the remembered shelf posture.
3. Every future upstream touch to the core is a merge conflict for as long as
   both copies drift.

## Decision

One Library. **Upstream's index core, our screen.** Their four core files are
adopted verbatim and never edited, which is what makes future syncs
conflict-free; everything of ours lives in files upstream does not have.

Rejected alternatives, with the reason:

- **Ship both screens, switchable in Settings.** Upstream's improvements would
  then only ever reach a screen we never open, so the stated benefit does not
  arrive. It also costs flash on the S3 targets, where only 382 KB (x4-pro) and
  476 KB (sticky) remain.
- **Keep our core and port their fixes by hand.** Zero conflicts, but every
  upstream fix becomes manual work, and the non-Latin fix alone is a rewrite.

## The boundary

Adopted verbatim from `crosspoint/feat/library-view@ad949bdd`, never edited:

```
lib/LibraryIndex/LibraryBuilder.{cpp,h}
lib/LibraryIndex/LibraryFormat.{cpp,h}
lib/LibraryIndex/LibraryIndexFile.{cpp,h}
lib/LibraryIndex/LibraryText.{cpp,h}
```

Their test suites come with them: `test/library_format/` and
`test/library_text/` replace ours; `test/library_builder/` and
`test/library_index_file/` are **new suites we do not currently have** —
`test/CMakeLists.txt:47-49` registers only `library_text`, `library_format`
and `library_favorites`, so two `add_subdirectory` lines are added.

Ours, which upstream has no copy of:

```
lib/LibraryIndex/LibraryFavorites{,File}.{cpp,h}
lib/LibraryIndex/LibraryState.{cpp,h}
lib/Epub/EpubQuickMetadata.{cpp,h}            <- KEPT, moved, see below
src/activities/library/LibraryListActivity.{cpp,h}
test/library_favorites/
```

The never-edit rule scopes to **those four core files only**. It is not a claim
about every file the feature touches; `lib/Epub/Epub.{h,cpp}` is a shared file
both forks already diverge on heavily, and it takes one deliberate edit
(below).

The boundary follows a real line: the index is a rebuildable cache, our
favorites and shelf posture are primary data. That is already the argument
`docs/file-formats.md` makes for keeping favorites out of the index.

### Why the graft fits

Their `LibraryIndexFile` API covers everything our screen calls and adds
`openForReconciliation()`, `ioFailed()`, `dedupDegraded()`, `readPathHash()`
and `readSourceAuthor()`.

It is **not** a drop-in superset, and the difference matters — see the
`SortOrder` section, which is where the one real migration hides.

Their permutation section has the same shape as ours — `authorOrder[N]` then
`arrivalOrder[N]`, both `uint16_t`, same `permStart` layout — so the O(1)
paging our screen depends on is intact.

Of the three record fields their `ClixRecord` lacks, `authorRank` and
`dateRank` are read only inside our `LibraryBuilder.cpp`; the screen never
touches them. `flags` is read in one place, the Details page
(`LibraryListActivity.cpp:1067-1094`).

## Blocker 1: their builder needs `Epub::loadMetadata()`, which we lack

`LibraryBuilder.cpp:348` calls `epub.loadMetadata(bookTitle, author)`, declared
at their `lib/Epub/Epub.h:49`. We have no such method.

Their implementation reaches it through
`parseContentOpf(metadata, /*writeSpineEntries=*/false, /*metadataOnly=*/true, &zip)`.
Our `Epub.h:101` signature is `(bookMetadata, writeSpineEntries, collectCssFiles)`
— no `metadataOnly`, no `sharedZip`. Porting their parser path means editing the
OPF parser in the most diverged shared file in the repo.

**Decision: keep the reader and delegate.** Add to `lib/Epub/Epub.h` one
declaration and to `Epub.cpp` a body that forwards to
`epub::readBookMetadata()`. Their builder then compiles unchanged.

The reader itself moves from `lib/LibraryIndex/LibraryMeta.{cpp,h}` to
`lib/Epub/EpubQuickMetadata.{cpp,h}`, namespace `library` → `epub`. It has to:
`LibraryBuilder.cpp` includes `<Epub.h>`, so defining the method against
`lib/LibraryIndex` would make the two libraries depend on each other. The
reader needs only `Logging`, `Memory`, `Print` and `ZipFile`, all independent.

This is better than porting their path, not merely cheaper:
`EpubQuickMetadata.h` documents the 8 KB bound and the stop at `</metadata>` as
the fix for a real `abort()` — 96 KB contiguous requested against a ~69 KB
largest free block on the C3. Adopting their parser would replace a measured
C3 guarantee with an unmeasured one.

Cost: `lib/Epub/Epub.{h,cpp}` carries one fork-only method, which is a future
conflict surface of roughly six lines. Accepted, and recorded here so it is not
rediscovered as a surprise.

## Blocker 2: `SortOrder` ordinals shift, and `library.state` stores them raw

`LibraryState.cpp` persists the enum ordinal directly (`raw[2] = shelfSort`,
`raw[3] = favSort`). The enum changes both content and order:

| value | ours        | theirs      |
|-------|-------------|-------------|
| 0     | `TitleAsc`  | `AddedAsc`  |
| 1     | `TitleDesc` | `AddedDesc` |
| 2     | `AuthorAsc` | `TitleAsc`  |
| 3     | `DateDesc`  | `TitleDesc` |
| 4     | —           | `AuthorAsc` |
| 5     | —           | `AuthorDesc`|

`STATE_VERSION` stays 1 and the `raw[2] >= SORT_COUNT` guard passes, because
`SORT_COUNT` rises to 6. So on the first boot after the update a reader who was
on `Recent` (3) silently reopens on Title Z-A, one on `Author` (2) reopens on
Title A-Z, and the favorites sort shifts the same way. No log, no symptom
anyone would connect to the update.

**Decision: bump `STATE_VERSION` to 2.** A v1 file then fails validation and
falls back to defaults, which is exactly what the format's own header already
promises: "a lost or corrupt file costs nothing but defaults". A remap table
was considered; the bump is more honest than silently reinterpreting a byte
whose meaning changed.

This also simplifies the state: their enum encodes direction in the value
itself (`TitleAsc` vs `TitleDesc`, `AuthorAsc` vs `AuthorDesc`), so `shelfSort`
alone carries the whole posture and the existing `flags` bit1 "titles
descending" becomes redundant. Drop it in the v2 layout.

`favorites.dat` is genuinely unaffected: its key `{fnv1a32(basename), fileSize}`
is computed at use from `readName()` and `record.fileSize`, both still present.
Verified.

## The author comes from the book's metadata, or there is none

**This section was rewritten during execution.** The original design assumed
upstream's index derived an author from the filename as ours does, and that
provenance merely collapsed from four states to two. That assumption was wrong,
and the correction is the largest behavioural change in this work.

Their `LibraryText.h` exports no `parseFilename`, no `looksLikeMetadata` and no
`preferFilenameTitle`. Their `LibraryBuilder.cpp` never parses a filename for an
author at all, and says why:

> An absent author is a fact, not a gap to fill: the row joins the Unknown
> group rather than borrowing a name from its surroundings.

Ours derives the author from the filename (`parseFilename`, our
`LibraryBuilder.cpp:167`) and from the folder
(`CLIX_AUTHOR_FROM_FOLDER`, :238), filtered by `looksLikeMetadata` — a guard
covering hex digests, ISBN prefixes, organisation credits and
"publisher, year" shapes.

**Decided: adopt their model.** The author comes from the book's own metadata
or the book has no author. The three parsing functions disappear with the file
they live in; nothing outside the replaced set calls them (verified — the only
other `parseFilename` in the tree is `SdCardFontRegistry`'s, an unrelated
function for font filenames).

### `libraryUseMetadata` must default to on

Their extraction is gated: `extractionExpected = st.readMetadata && hasEpubExtension`
(`LibraryBuilder.cpp:306`), and `buildLibraryIndex`'s `readMetadata` defaults to
false. Our `CrossPointSettings.h:640` has `libraryUseMetadata = 0`. Left alone,
the two combine to a shelf with **no authors at all** — author sort, author
section headings and the author line under every title all empty.

So the setting defaults to 1. Two things make that affordable, and neither was
true before this work:

- `Epub::loadMetadata` delegates to `EpubQuickMetadata`, which keeps the 8 KB
  inflate bound and stops at `</metadata>`. This is the fix for a real
  `abort()` — 96 KB contiguous requested against a ~69 KB largest free block.
- Their builder re-parses only books that actually changed: a prior record is
  reused when file size, modification time, fold version, `metadataEnabled` and
  `metadataStatus` all match (`LibraryBuilder.cpp:306-318`). The slow walk is a
  one-time cost, not a per-rebuild one.

The first Library entry on a large, never-opened library is still the slow
path, and it is unmeasured on the C3. That is device verification step 1.

### The provenance line is removed

With one source, the line can only ever say the same thing, so it stops being
information. It is deleted, and `STR_LIBRARY_PROV_FOLDER` / `_CACHE` / `_OPF`
go with it. Only `english.yaml` and `french.yaml` carry those keys — the
Library is a fork feature and other languages fall back to English.

Books whose metadata carries no author show no author line at all, which is
already what the Details page does today when the author string is empty.

### `ClixFormat` has to land somewhere

Their `LibraryFormat.h` has neither the `ClixFormat` enum
(`CLIX_FORMAT_EPUB/TXT/MD/XTC/OTHER`) nor `recordFormat()`. Both are used at
`LibraryListActivity.cpp:1094`. The enum and a `formatForName()` derived from
the filename move into our screen's own header — not just the function.

## The sort strip goes from five tabs to four

Today: `★ | Recent | A-Z | Z-A | Author`
Target: `★ | Time | Title ▾ | Author`

This finishes what the v1.1 spec already argued for — "Title Z-A pays a full
tab for a rare use", and a long-press on the focused strip flipping direction.

Their core now offers both directions for all three orders, so the rule is
uniform rather than Title-only: **a hold on any already-focused sort tab flips
its direction**, and the arrow follows. `Time` gets `AddedAsc`/`AddedDesc`,
`Author` gets `AuthorAsc`/`AuthorDesc`, both for free. A tap or short press
activates a tab.

The favorites sort menu (hold on `★`) keeps explicit entries. In a popup list,
spelling both directions out is clearer than an arrow to decode.

### Margin above the strip

Observed on our own firmware. The content margin is
`metrics.topPadding + TouchHeaderBackButton::height(...)`, and
`UiTabListActivity` then takes `tabLineHeight + 10` for the strip. The visible
gap is `metrics.topPadding`, and it is removed in the same pass.

## The builder signature loses more than a parameter

Ours: `buildLibraryIndex(rootPath, previousNextFirstSeen, stats, readMetadata, onProgress, progressCtx)`
Theirs: `buildLibraryIndex(rootPath, stats, readMetadata)`

Gone: `BuildProgressFn` and `previousNextFirstSeen`. The two call sites
(`SettingsActivity.cpp:1119`, `LibraryListActivity.cpp:215`) lose their
`previous.open()` / `carriedFirstSeen` preamble as well as the callback.

**The progress callback was our only watchdog feed.** Removing it is safe here
only because their builder feeds the watchdog itself —
`LibraryBuilder.cpp:86` is `if ((++workUnits & 0x1Fu) == 0) delay(1);`, with
further `delay(1)` calls at 690, 692, 858, 860 and 875. This is written down
because anyone following this spec without knowing it will reintroduce the
reboot-on-`libraryUseMetadata`-ON bug.

## What happens on a device that updates

The record layout changes, so the existing index fails version validation and
rebuilds on first entry to the Library. That is the designed migration
mechanism; the index is a cache and nothing else.

Note the format version goes **backwards**: `CLIX_FORMAT_VERSION` is 3 here and
2 there. Validation is an equality test, so the rebuild happens correctly — but
our version line stops being monotonic, and `docs/file-formats.md:670-676`
("Format version 3") becomes wrong. Updating that document is a deliverable of
this work, not an afterthought: `CLAUDE.md` requires binary layout changes to be
documented there.

`library.state` migrates by version bump (above). `favorites.dat` is untouched.

## Verification

Host tests: their four suites, two of which are additions to
`test/CMakeLists.txt`. `test/library_favorites` stays, because it covers our
primary data.

Build measurement, cheap and worth having: `pio run -e x4-pro` before and
after. Their builder is 1333 lines against our 1007, and the two-screen
alternative was rejected partly on flash; the actual delta should be a number,
not an assumption.

On an X4, in this order — each step tests one assumption of this design:

1. First Library entry after the update: the index rebuilds **and the
   favorites are still there**. Record free heap and largest allocatable block
   during the rebuild (see Known unknown).
2. The shelf reopens on **defaults**, not on a misread posture — the
   `STATE_VERSION` bump working as intended.
3. The four-tab strip: a hold flips direction on `Time`, `Title` and `Author`,
   the arrow follows, and the choice survives a power cycle.
4. Search on an accented title; then, on a Cyrillic or CJK book if one is
   available, the same search — the previously dead case.
5. Details on a book with real metadata shows its author and no provenance
   line; a book whose metadata carries no author shows no author line at all.
   A book whose author used to come from its filename now shows none — that is
   the accepted consequence of the metadata-only model, not a defect.
6. Delete a book and return to the shelf: no ghost row.
7. With `libraryUseMetadata` ON, rebuild over a large library and confirm no
   watchdog reboot.

## Known unknown

Index rebuild on a large library now runs through their builder, not ours. Its
memory behaviour on the C3 is unmeasured, and there is no recorded baseline for
our own builder to compare it against — the figures in `.claude/CONTEXT.md` are
for a reading session, not an index build. Step 1 is where both get a number. A
regression there is a finding to take upstream rather than a reason to fork the
core again.

Keeping our own metadata reader means that path holds its measured 8 KB bound,
so the riskiest part of the rebuild is the part we are *not* changing — and it
is what makes defaulting extraction on affordable at all.

## Non-goals

- Proposing this screen upstream. The fork keeps its own UI deliberately; only
  the core is shared.
- Touching the scrolling-versus-paginated question. Ours paginates and stays
  that way.
- Any change to the `favorites.dat` format.
