# Review of `feat/library`, 2026-08-05

An eight-axis review with adversarial verification, run by the maintainer. Kept
here because it is a work list, not an opinion: one critical defect, about
fourteen major, a dozen minor. All four environments build; one host test fails.

Ordered by what blocks use, not by where it sits in the code.

## Critical

**1. OOM abort on a large flat folder.** `LibraryBuilder.cpp:274` — the walk's
dedup keeps every book's full name in a `std::vector<std::string> seen`. Two
thousand books in one folder is ~360 KB, more than the C3 has free, and the growth
goes through throwing `new`, so it aborts and reboots on every rebuild. Store
`fnv1a32` hashes, already in the file, and reserve.

The lesson generalises: anything that grows with user data must be bounded or
reduced to a fixed size. The simulator has unlimited RAM and will never show this.

## Major, functional

**2. The metadata title overwrites the basename, so an enriched book cannot be
opened.** `LibraryBuilder.cpp:210/249` writes the DISPLAY title into the name blob,
but `readPath` (`LibraryIndexFile.cpp:133`) rebuilds the file path from that same
blob, and `openSelectedBook` uses it. With metadata on, an enriched book resolves
to `/Books/Pachinko` instead of its real filename. It also breaks firstSeen
reconciliation, which hashes the dirent name on one side and the stored title on
the other. Basename and title must be separate fields; bump the format version.

**3. Back does not close the A-Z grid.** `LibraryListActivity.cpp:479-491` — both
Back handlers run before the `letterGrid` block, so the close at :492 is
unreachable. Back leaves the activity with the grid still open.

**4. Undefined behaviour, `1u << -1`.** :518 — on the mode line `letterCursor` is
-1, and every first-name/surname toggle shifts by it. The "first present letter"
scan belongs in the Down handler at :529, which currently sets 0 even when A is
absent.

**5. Confirm reopens the grid after a jump.** `tabsFocused` is never cleared after
`jumpToLetter`, so the next Confirm reopens the grid instead of opening the book.

**6. The long-press sort menu does not re-run `applyFilter`.** A search plus a sort
change leaves a stale filtered list. `cycleSortOrder` does it correctly.

**7. `pageStarts` is never invalidated** on filter, sort or jump, so paging back
lands on boundaries that no longer exist.

**8. "the hobbit" finds nothing.** `applyFilter` folds the query with
`stripArticle=false` while the stored folds strip articles. One line.

## Major, robustness — the index is untrusted input

**9. `foldLen` and `authorKeyLen` are read from disk and never bounded.** A
corrupt or forged index with `foldLen=255` runs a `string_view` 159 bytes past the
end of a 128-byte record. Clamp in `readRecord`, at the source. `validateHeader`
should also recompute `layoutSections` in 64-bit: a forged `folderLen` can wrap
u32.

**10. A full SD card hangs or produces a wrong index.** `padTo` loops forever if
`write` returns 0, and no staging or emit write is checked, so a build on a full
card "succeeds" and produces an index that is rejected or wrong. `padTo` must
return a result, writes must be checked, and the final remove/rename must not run
on failure — the old index should survive.

**11. An aborted or capped walk still installs an index flagged WALK_COMPLETE,**
and degradations decided in `emitIndex` never reach the flags, which are frozen
at :366.

## Efficiency

`emitIndex` peaks near 209 KB at 4096 books: the author and date SortKey arrays
ignore the `LIBRARY_MAX_SORTED` cap that the title sort respects. The author
spelling vote is O(k²) 512-byte SD reads per run. Enrichment is not incremental —
every rebuild re-reads every cache and OPF, and `enrichCursor` and
`CLIX_FLAG_ENRICH_COMPLETE` are written but never read. `applyFilter` and
`computeLettersPresent` read record by record instead of the 4 KB chunks the
format was designed to allow.

## Before any upstream PR

Exclude `0d0ba2d6` (OTA URL — keep it fork-side via a build flag) and
`docs/superpowers/specs/*`. Split out the macOS CssParser fix and the file
browser's Full Name mode as separate PRs; neither depends on the Library.

Dead code to remove: the whole disabled-keys keyboard mechanism
(`setAllowedLettersFilter`, `allowedLettersFor`) now that the UX was reverted, plus
`RowText`/`rowText`/`uiItems`, declared and cleared but never filled, and a
duplicated `stemOf`. Git keeps it all if we revisit.

Repo obligations: document CLX1 in `docs/file-formats.md` as AGENTS.md requires;
add search and the A-Z grid to the CHANGELOG; fix `LibraryFormatTest.cpp:112` to
use `CLIX_FORMAT_VERSION + 1` rather than a hardcoded 2 — it fails today.

## Verified sound

CLX reads are memory-safe by construction (u8 lengths into sized buffers), every
allocation is nothrow and logged, staging is cleaned on every failure path,
settings persist correctly through the JSON registry, the settings enum is
extended at the end, and EN/FR are complete. The broken test targets (DictLayout,
TextPool) pre-date this branch.
