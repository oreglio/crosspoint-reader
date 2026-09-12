# Library — our shelf on upstream's index core

Status: designed, not started. 2026-09-12.
Base: `sync/upstream-20260912`. The index core is taken from
`crosspoint/feat/library-view` (PR #3366, head `d4411be9`), which is not in
`develop` and therefore not in the upstream sync.

## Problem

Upstream merged our Library series (#2885-#2889, #3256) into their
`feat/library-view` branch and Uri-Tauber is integrating it as #3366. Since
that merge their copy of the index core has kept evolving while ours has too.
Three consequences:

1. Their core gained fixes we want — most concretely, `fold()` now covers
   Greek, Cyrillic and Han (`0x0370-0x03FF`, `0x0400-0x052F`,
   `0x3400-0x4DBF` and `0x4E00-0x9FFF`). Ours covers none of them, which is the known limitation
   that leaves search and sort dead on a non-Latin library.
2. Their screen is not the one we want. It scrolls where ours paginates, and
   it carries none of the v1.1 layer: favorites, delete, details, the
   long-press menu, the remembered shelf posture.
3. Every future upstream touch to the core is a merge conflict for as long as
   both copies drift.

## Decision

One Library. **Upstream's index core, our screen.** Their core files are
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

Adopted verbatim from `crosspoint/feat/library-view`, never edited:

```
lib/LibraryIndex/LibraryBuilder.{cpp,h}
lib/LibraryIndex/LibraryFormat.{cpp,h}
lib/LibraryIndex/LibraryIndexFile.{cpp,h}
lib/LibraryIndex/LibraryText.{cpp,h}
test/library_builder/  test/library_format/
test/library_index_file/  test/library_text/
```

Ours, which upstream has no copy of:

```
lib/LibraryIndex/LibraryFavorites{,File}.{cpp,h}
lib/LibraryIndex/LibraryState.{cpp,h}
src/activities/library/LibraryListActivity.{cpp,h}
test/library_favorites/
```

Deleted: `lib/LibraryIndex/LibraryMeta.{cpp,h}`. Its only includer is
`LibraryBuilder.cpp`, which is being replaced; its only other references are
two `LibraryMetadataGuard` tests inside `library_text`, which are replaced
along with that suite.

The boundary follows a real line rather than a convenient one: the index is a
rebuildable cache, our favorites and shelf posture are primary data. That is
already the argument `docs/file-formats.md` makes for keeping favorites out of
the index.

### Why the graft fits

Their `LibraryIndexFile` API is a strict superset of the one our screen calls.
Everything we use is present, plus `openForReconciliation()`, `ioFailed()`,
`dedupDegraded()`, `readPathHash()` and `readSourceAuthor()`.

Their permutation section has the same shape as ours — `authorOrder[N]` then
`arrivalOrder[N]`, both `uint16_t`, same `permStart` layout — so the O(1)
paging our screen depends on is intact.

Of the three record fields their `ClixRecord` lacks:

- `authorRank` and `dateRank` are read only inside our `LibraryBuilder.cpp`.
  The screen never touches them, so losing them costs nothing.
- `flags` is read in exactly one place: the Details page
  (`LibraryListActivity.cpp:1067-1094`). `recordTitleFromOpf()` and
  `recordOpfTooLarge()` have no caller at all.

## Author provenance collapses to two states

Ours records four (`FROM_FOLDER`, `FROM_CACHE`, `FROM_OPF`, `UNKNOWN`). Theirs
records `metadataStatus` (`NOT_ATTEMPTED`, `EXTRACTED`, `FAILED`) and exposes
`readSourceAuthor()`.

`EXTRACTED` means the author came from the book itself; the other two mean it
came from the filename. The Details line therefore becomes **"from the book"**
versus **"from the filename"**.

This serves the line's stated purpose in full — our own comment gives it as
"so the screen never silently claims a folder name is an author". Whether a
filename-derived guess had been cached is an implementation detail, not
something a reader acts on. A sidecar file storing one byte per book was
considered and rejected: it reintroduces a file to maintain and a migration,
for a distinction nobody reads.

The format label (EPUB / TXT / XTC) is re-derived from the filename.
`formatForName()` moves from the index builder into our screen.

## The sort strip goes from five tabs to four

Today: `★ | Recent | A-Z | Z-A | Author`
Target: `★ | Time | Title ▾ | Author`

This finishes what the v1.1 spec already argued for — "Title Z-A pays a full
tab for a rare use", and a long-press on the focused strip flipping direction.

`kTitleAscTab` and `kTitleDescTab` merge into one `kTitleTab`; `orderForTab()`
consults the stored direction bit; the label carries the arrow. A tap or short
press activates a tab; a hold on the already-focused `Title` tab flips the
direction.

No format change and no migration: `library.state` already stores `shelfSort`
and a separate `flags` bit1 "titles descending", so the persisted model is
already the target model. The change is confined to the UI layer.

The favorites sort menu (hold on `★`) keeps its four explicit entries. In a
popup list, spelling both directions out is clearer than an arrow to decode.

### Margin above the strip

Observed on our own firmware. The content margin is
`metrics.topPadding + TouchHeaderBackButton::height(...)`, and
`UiTabListActivity` then takes `tabLineHeight + 10` for the strip. The visible
gap is `metrics.topPadding`, and it is removed in the same pass.

## What happens on a device that updates

The record layout changes, so the existing index fails version validation and
rebuilds on first entry to the Library. That is the designed migration
mechanism; the index is a cache and nothing else.

`favorites.dat` and `library.state` are untouched and stay valid. Their key is
`{fnv1a32(basename), fileSize}`, computed at use from `readName()` and
`record.fileSize` — both still present — rather than stored in the index. No
data migration is required.

## Adaptations on our side only

`libraryUseMetadata` reaches the index builder from two call sites,
`SettingsActivity.cpp:1119` and `LibraryListActivity.cpp:215`. They are
adjusted to their builder's signature. Both files are ours, so the
never-edit-their-files rule holds without exception.

## Verification

Host tests: their four suites replace ours where they overlap;
`test/library_favorites` stays, because it covers our primary data.

On an X4, in this order — each step tests one assumption of this design:

1. First Library entry after the update: the index rebuilds **and the
   favorites are still there**. This is what proves the identity survives the
   core swap.
2. The four-tab strip: `Title` flips direction on hold, the arrow follows, and
   the chosen direction is still set after a power cycle.
3. Search on an accented title; then, on a Cyrillic or CJK book if one is
   available, the same search — the previously dead case.
4. Details on a book whose author came from its filename, then on one with
   real metadata: the two states must read differently.
5. Delete a book and return to the shelf: no ghost row.

## Known unknown

Index rebuild on a large library now runs through their builder, not ours. Its
memory behaviour on the C3 is unmeasured, and there is no recorded baseline for
our own builder to compare it against — the figures in `.claude/CONTEXT.md` are
for a reading session, not an index build. Step 1 above is where both get a
number: record free heap and largest allocatable block during the rebuild. A
regression there is a finding to take upstream rather than a reason to fork the
core again.

## Non-goals

- Proposing this screen upstream. The fork keeps its own UI deliberately; only
  the core is shared.
- Touching the scrolling-versus-paginated question. Ours paginates and stays
  that way.
- Any change to `favorites.dat` or `library.state` formats.
