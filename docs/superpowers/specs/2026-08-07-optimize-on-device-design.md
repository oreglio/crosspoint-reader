# Optimize on Device — Design

Date: 2026-08-07 · Status: validated with the user (this session) · Target: fork branch, release v1.5.16

Optimize books that are already on the SD card, from the web portal, without ever
holding fewer than one complete copy of a book — and give the File Manager a
mobile card layout worthy of the feature. Companion mockup (interactive, the
approved "variante 3" = whole-row select with ring outline):
https://claude.ai/code/artifact/689915c0-e57d-49e0-826e-c906bb923e73

## Why

Books uploaded before the optimizer existed (or uploaded unoptimized) never get
its benefits: device-sized grayscale JPEGs, split sections, x-locations page
metadata — and since v1.5.15, the embedded sync identity. Re-uploading from a
computer by hand is 40 round trips, and the File Manager's mobile view is an
unreadable squeezed table. One chantier fixes both, because both live in the
same two files (`web/pages/files.{js,css,html}`) plus one new firmware endpoint.

## Decisions (all settled with the user)

1. **UI scope**: toolbar button `⚡ Optimize (N)` next to Delete Selected,
   reusing the existing multi-select machinery (`getSelectedItems()`), plus a
   per-row ⚡ action for a single book. No dedicated page in v1.
2. **Mobile card layout, "variante 3"**: below ~520 px the table becomes cards —
   whole-row tap selects (ring outline + sage wash + ✓ before the title, no
   checkbox column), serif titles (Iowan Old Style/Palatino/Georgia system
   stack) clamped to 3 lines, meta row with EPUB chip + tabular size, quiet
   40 px icon actions. Because row-tap now selects, a ⬇ download button joins
   the card actions (mobile layout only). Desktop table is unchanged in v1.
3. **Swap mechanics — approach A**: a dedicated firmware endpoint does the whole
   replacement in one round trip (see Flow). Never a direct overwrite, never
   delete-then-upload: the optimized copy is uploaded BESIDE the original as
   `md5(path).optimizing` (a hash of the book's own path, not its name, so any
   filename length works), and the swap window (delete + rename) lives entirely
   inside the firmware, milliseconds long, Wi-Fi-independent.
4. **Identity hook**: the firmware re-anchors `favorites.dat` and
   `library.state` from `{nameHash, oldSize}` to `{nameHash, newSize}` — the
   star and the remembered selection survive the swap. `firstSeen` is NOT
   preserved: the swapped book resurfaces atop Added, accepted and defensible
   (it did just change). Local reading progress already survives via
   `clearBookCachePreservingUserState` (progress.bin + reader settings + stats).
5. **Already-optimized books are skipped**, detected for free after download
   (zip contains `META-INF/crossink-sync.json` or `META-INF/x-locations.json`),
   counted separately in the summary. No forced re-optimize in v1 (repeated
   JPEG passes degrade generationally).
6. **Sync identity is always embedded** in this flow regardless of the upload
   modal's toggle — the book on the card IS the original; that's the point.
   Other conversion settings (quality, split, device target…) come from the
   upload modal's Advanced Mode as saved.
7. **Folders in the selection are walked recursively** browser-side
   (`/api/files` per folder, sequential), EPUBs only.
8. **The WS freshness bug ships with this chantier** (Task 1): WebSocket upload
   completion must call `library::markShelfStaleIfBook` like the HTTP path does
   (`CrossPointWebServer.cpp:953`); today WS-uploaded books stay invisible until
   a manual index rebuild (user-reported on device).

## Flow (per selected book, sequential queue)

```
GET /download?path=<book>            → Blob
JSZip probe: optimized markers?      → yes: count "already optimized", next book
convertEpubFile(blob, settings)      → optimized Blob (identity embedded, from the
                                       downloaded original's bytes)
[leftover md5(path).optimizing?]     → POST /delete it (orphan from a failed run)
WS upload as md5(path).optimizing    → staging beside the original
POST /replace {path, staging}        → firmware does the swap (below)
next book
```

Staging cleanup only happens for failures *before* `/replace` is sent (best
effort, e.g. an orphan from a previous failed run). Once a `/replace` attempt
has been sent, staging is never auto-deleted: the firmware's post-removal
failure cases can leave it as the book's only surviving copy, so the client
leaves it in place and names it in the error instead of guessing. Either way
the book is counted failed, the queue continues. Progress reuses the
upload modal (per-book phases: download → optimize → upload → replace) with the
existing log; summary line "N optimized · M already optimized · K failed".
Nice-to-have, not required: a `.busy` hatched state on the current row/card.

## Firmware: `POST /replace` (CrossPointWebServer)

Args: `path` (the original), `staging` (the uploaded `.optimizing` file).
Validations, each a distinct 4xx: staging exists, non-empty, ends with
`.optimizing`, same directory as `path`; `path` exists, is a file, not
protected. Then, in order:

1. capture `oldSize` = size of `path`, `newSize` = size of staging
2. `clearBookCachePreservingUserState(path)` — derived cache dropped, progress
   kept (same path ⇒ same cache dir after the swap)
3. `Storage.remove(path)` then `rename(staging → path)` — the danger window
4. re-anchor: `LibraryFavoritesFile::reanchor({h,oldSize},{h,newSize})` where
   `h = favoriteNameHash(basename)`; same substitution on
   `LibraryShelfState.selected` (load/patch/save)
5. `library::markShelfStaleIfBook(path)`
6. 200 with a small JSON `{ok:true}`

New lib API: `bool LibraryFavoritesFile::reanchor(const FavoriteKey& from,
const FavoriteKey& to)` — single save, count unchanged so cap-safe, no-op
(false) if `from` absent; if `to` already present, just remove `from`.
Host-tested (extends the existing `library_favorites` gtest suite). The
`library.state` re-anchor gets a host test too.

## Out of scope (v1)

Forced re-optimization; desktop table redesign; `firstSeen` preservation
(builder reconcile untouched); a "keep original as copy" option (replace only);
in-card live progress as a requirement; translations (portal is English-only).

## Risks & mitigations

- Interrupted flow leaves a `md5(path).optimizing` orphan: opaque but
  deterministic, still visible in the File Manager; the next optimize run of
  that book recomputes the same hash and deletes it first.
- Staging names are a fixed-length hash, not the book's name, so the server's
  150-byte filename cap (`sanitizeFilename`) can never truncate one into a
  name that mismatches the `staging` arg sent to `/replace` — the length risk
  that motivated an earlier client-side fail-fast guard no longer exists. The
  `.optimizing` suffix still keeps every book mechanism dormant (stale marker,
  cache, epub badge).
- One WS upload at a time server-side: the queue is strictly sequential by
  construction (`uploadNextFile` idiom).
- `/rename` and `/move` never touch the target's cache — irrelevant here because
  `/replace` clears it explicitly, but noted so nobody "simplifies" the endpoint
  away onto rename.

## Verification

Host: gtest for `reanchor` + state re-anchor; `node --check`; Task 1 harness
still `ALL OK`; `python3 scripts/build_web.py`. Firmware: 4 envs + `pio check`.
Device matrix (user, X4/X3): swap a favorited, in-progress book → star intact,
reading position intact, resurfaces in Added, KOSync still paired (same embedded
identity); already-optimized book skipped with count; folder recursion; orphan
`.optimizing` cleanup; mobile cards — row-tap select, ring outline, ⬇ download,
toolbar counts; WS-uploaded book now appears on the shelf without manual rebuild.
