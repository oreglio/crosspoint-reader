# CrossInkLibrary — Library Index, Search, Constrained Text Entry and Library UX

**Final integrated design. Version 1.0. Implementation spec.**

Target: Xteink X3 (ESP32‑C3, UC8253 792×528 1‑bit panel, SD over shared SPI, buttons only), CrossInkLibrary fork of CrossInk 1.5.
Design range: 50–2000 books. Optimisation point: 100–250 books. Headroom point: 2000 books.

---

## 0. Verdict: spine, grafts, and what was dropped

**The spine is CLIX** — the streamed, rank‑inlined index. It scored highest across two of three lenses (7.5 / 9 / 7 against 6 / 6 / 6 and 4.5 / 6 / 7.5), and it is the only one of the three whose screen geometry matches the device. That last point is not a detail: I re‑verified it and it is decisive.

`GfxRenderer`'s constructor sets `orientation(Portrait)` (`lib/GfxRenderer/GfxRenderer.h:132`), and in Portrait `getScreenWidth()` returns `panelHeight` while `getScreenHeight()` returns `panelWidth` (`lib/GfxRenderer/GfxRenderer.cpp:2472-2484`, `:2486-2498`). The X3's logical screen is therefore **528 wide × 792 tall**. The fork's own most recent commit (`5f88bef3`, "feat: add a Full Name file browser display mode") states the same thing and adds a measured consequence: *"At the X3's 528 px logical width … a row holds roughly 45 characters"*, and four label lines hold ~180 characters, showing 7 rows per screen against 13 in two‑line mode. Designs 2 and 3 computed against 792×528 and, in Design 2's case, **explicitly surrendered requirement 1 (never truncate)** on the basis of an arithmetic error. Their geometry-derived conclusions are void; their non-geometric ideas are not, and several are grafted below.

### Taken from Design 2 (LibraryStore)

- **Up from keyboard row 0 enters the result rows, bottom‑most first; Up from the top result wraps to grid row 4.** This is the correct axis for reaching results from a d‑pad keyboard, and it removes CLIX's Left/Right top‑row trap.
- **Every sort comparator ends in the record ordinal**, so ties are impossible and rebuilds are deterministic. This deletes CLIX's stated "two same‑prefix books swap places between rebuilds" risk at zero cost.
- **List `/.crosspoint` once, sequentially, and binary‑search the resulting sorted hash array** instead of calling `Epub::hasCache()` per book. `Epub::hasCache` and `Epub::cachePathForFilePath` exist (`lib/Epub/Epub.h:112`, `:109`); the naive per‑book form re‑scans a directory holding ~2N subdirectories.
- **`OptionSelectionActivity` for the sort/filter menu** (`src/activities/util/OptionSelectionActivity.h:14-23`) rather than hand‑driving a popup. Zero new UI code.
- **The term‑by‑term latency table format**, adopted as the mandatory form for every latency claim (§7.4).
- **Its warning that `BookMetadataCache::load()` deletes `book.bin`** on magic/version/truncation mismatch. Verified at `lib/Epub/Epub/BookMetadataCache.cpp:504, :510, :518, :525, :537`.

### Taken from Design 3 (One Shelf)

- **Default sort = date added, newest first.** The book just copied onto the card is row 0. Highest value‑per‑byte idea in the set; costs one enum default.
- **Optimistic open**: paint from the cached index immediately, run the verification walk afterwards in `loop()` slices, redraw only if something changed. This removes CLIX's blocking pre‑paint walk *and* its cleansing HALF refresh on entry.
- **The naive‑skip unreachability finding.** Its BFS result (42/200 targets unreachable at depth 3) is a real correctness bug that CLIX and Design 2 both ship. Its *fix* (row‑major Left/Right) is rejected as spatially wrong; §9.3 gives a fix that is both spatially sane and provably reachable.
- **An explicit provenance field with a "tried and failed" sentinel**, surfaced in a per‑book Details row ("author: from folder name").
- **A walk‑cost floor**: if the last walk exceeded a threshold, stop walking automatically and expose an explicit Refresh action.

### Dropped from all three

- CLIX's **hand‑rolled key grid and hand‑drawn list rows**. `fui::keyGrid` already renders `enabled == false` as `StateDisabled` (`freeink-sdk/libs/ui/FreeInkUI/include/components/keyboard/key-grid.h:64`, `:32`), and `fui::list` gained everything we need when the fork added `FILE_BROWSER_DISPLAY_FULL`: a uniform row height derived from theme metrics plus `props.labelText.maxLines` (`freeink-sdk/.../FreeInkUICore.h:422`). Hand‑drawing buys ~2 KB we do not need and guarantees theme drift.
- CLIX's **rank cursor** as the paging mechanism. It made a page turn cost a full index pass — the worst‑scaling action in the design, on the *most frequent* interaction. Replaced by a fixed‑stride record section plus two `uint16_t` permutation arrays (§3), which makes paging O(1) in every sort order at every N.
- CLIX's **`BufferedFileReader` for the scan.** Verified: `lib/Serialization/BufferedFile.h:83-104` issues `file.read(buf, cap)` at whatever the current position is, and `seek()` at `:113-124` sets `bufStart = target` with no rounding. It performs no 512‑byte alignment, and `FatFile` only takes its multi‑sector fast path on aligned, ≥2‑sector reads. Replaced by an owned 4096‑byte buffer read at 512‑aligned offsets, with the format padded so that alignment is structural rather than incidental.
- Design 2's **fully resident model** and its 56 KB open gate, and Design 3's **interleaved name+fold blob** (which forces a search scan to read display names it does not use).
- Design 3's **`listNav{120,400}`** repeat timing. It outruns the panel. House default 500/500 (`src/util/ButtonNavigator.h:20`).
- The **letter rail, two‑step letter index, 1‑D wheel, author‑first navigation, trigram/n‑gram indexes, tries/DAFSA/marisa/LOUDS/FM‑index, bloom and quotient filters, persisted-per-order offset tables beyond the two permutations, and dynamic key reordering.** All three designs rejected these with converging arithmetic; the rejections stand and are not re‑argued here.

---

## 1. The verified device envelope

Everything below is arithmetic on these numbers. Nothing here is assumed.

### 1.1 Geometry (logical, Portrait)

| Quantity | Value | Source |
|---|---|---|
| Logical screen | 528 × 792 | `GfxRenderer.h:132`; `GfxRenderer.cpp:2472-2498` |
| `topPadding` | 5 | `src/components/themes/BaseTheme.h:148` |
| CompactHeader height / top gap | 67 / 6 | `src/components/CompactHeader.cpp:13`, `:14` |
| `contentTop` | 78 | `CompactHeader.cpp:32-34` (`topPadding + 67 + 6`) |
| `buttonHintsHeight` | 40 | `BaseTheme.h:183` → hints occupy y 752..792 |
| `verticalSpacing` | 10 | `BaseTheme.h:151` |
| **List band** | **y 88..752 = 664 px** | derived |
| `contentSidePadding` / `listScrollWidth` | 20 / 4 | `BaseTheme.h:154`, `:162` |
| **Row text width** | **≈ 480 px ≈ 45 characters** | derived; corroborated by commit `5f88bef3` |
| `listRowHeight` / `listWithSubtitleRowHeight` | 30 / 50 | `BaseTheme.h:155`, `:156` |
| `keyboardKeyHeight` / `keyboardKeySpacing` / `keyboardVerticalOffset` / `keyboardWidthPercent` | 48 / 0 / −13 / 94 | `BaseTheme.h:189-194` |

Row height for *n* label lines follows the helper the fork just introduced for the file browser: `singleLine` for 1, `withSubtitle` for 2, and each further line costs `withSubtitle − singleLine`. On X3 that is **30 / 50 / 70 / 90 px** for 1 / 2 / 3 / 4 lines, giving **22 / 13 / 9 / 7 rows** in the 664 px band. An 80‑character filename is two lines: **13 untruncated rows per page** is the expected default.

Keyboard rect, recomputed from `KeyboardEntryActivity::keyboardRect()` (`src/activities/util/KeyboardEntryActivity.cpp:490-503`) with a 5‑row grid:
`y = 792 − 40 − 10 − (5 × 48) − 13 = 489`; `width = 528 × 94 / 100 = 496`; `x = 16`; 6 columns → **82 × 48 px cells**. **411 px of usable space sits above the keyboard** (489 − 78), not the 147 px Design 2 computed. That is what makes untruncated live results possible while typing, and it is the single most consequential correction in this document.

### 1.2 Refresh

There is no reachable windowed refresh on X3: `PanelDriver::displayWindow`'s base implementation degrades to a full‑screen `RefreshMode::Fast` (`freeink-sdk/libs/display/FreeInkDisplay/src/driver/PanelDriver.h:53-56`) and `Uc8253X3Driver` does not override it; `GfxRenderer::displayWindow` is commented out (`GfxRenderer.h:215`) and `HalDisplay` has no windowed entry point. **Refresh cost is area‑independent.** Two costs exist:

- **FAST ≈ 185 ms** (26 ms DTM2 SPI + ~133 ms waveform + 26 ms DTM1 post‑sync), plus up to 50 ms of bounded BUSY poll and up to 10 ms of main‑loop tail.
- **HALF ≈ 770–1000 ms with a visible flash**, because `HalDisplay` escalates HALF on X3 via `einkDisplay.requestResync(1)` (`lib/hal/HalDisplay.cpp:68`, `:76`, `:92`) into the full OEM bank plus a conditioning pass.

There is no tier below FAST. The only levers are the **number** of refreshes and the **count of flipped pixels** (which drives ghost accumulation, not time).

### 1.3 Storage

SD shares the display's SPI bus and runs SHARED_SPI; ESP32‑C3 has no SDMMC host. Design throughput band: **400 KB/s pessimistic, 800 KB/s conservative, 1500 KB/s expected.** Reads must be issued at 512‑byte‑aligned offsets in ≥1 KB (preferably 4 KB) chunks or SdFat silently degrades to one 512‑byte transaction per sector.

`HalFile` exposes no timestamp accessor — its complete API is `lib/hal/HalStorage.h:79-101` (`flush/getName/size/fileSize/fileSize64/seek/seek64/seekCur/seekSet/available/position/read/write/sync/rename/isDirectory/rewindDirectory/close/openNextFile/isOpen`). Invalidation therefore cannot use mtime. `Storage.remove()` / `Storage.rename()` are at `HalStorage.h:40-41`.

**Never keep two files open across the read path.** `lib/Serialization/BufferedFile.h:11-18` records a measured 31 s regression caused by exactly that interleaving against SdFat's single 512‑byte volume cache.

### 1.4 Heap and flash

Measured steady state ~85–90 KB free / ~49 KB maxAlloc. With WiFi+TLS up the floor is 35,000 B free / 20,000 B maxAlloc (`lib/KOReaderSync/KOReaderSyncClient.cpp:122-123`). House no‑gate contiguous ceiling is 8,000 B (`lib/GfxRenderer/GfxRenderer.h:45`). Arena slab size is universally 4096 B (`lib/Memory/Arena.h:61-69`). Flash headroom is 291,168 B of the 6,553,600 B app slot; commit `5f88bef3` measured the current build at 6,262,880 B.

---

## 2. Every fatal flaw, resolved

| # | Flaw (judge) | Resolution |
|---|---|---|
| 1 | CLIX: page turn costs a full index pass (J1) | **Fixed structurally.** Fixed 128‑byte record stride + two `uint16_t` permutation arrays make row *p* in any order a direct seek. §3, §8. |
| 2 | CLIX: mandates aligned reads but names `BufferedFileReader`, which does not align (J1) | **Fixed.** Every section is padded to a 512‑byte boundary and the scanner owns a 4096‑byte buffer read at 512‑aligned offsets with `HalFile::read` directly. `BufferedFileWriter` is retained for the *build* only, where alignment is irrelevant. §3, §7.1. |
| 3 | CLIX: build RAM is O(N), ~46 KB at 2000, fails the WiFi‑up floor (J1) | **Fixed.** Sort keys are fixed 16 B and sorted in 512‑key chunks (8,192 B, one allocation) with a 2‑way external merge above 512 books. Build peak ≤ 14 KB at any N. §5.3. |
| 4 | CLIX: blocking whole‑card walk before first pixel on every entry (J1, J3) | **Fixed** by Design 3's optimistic open, plus a walk‑cost floor. §5.5. |
| 5 | CLIX: ~770–1000 ms flashing HALF on every Library entry (J3) | **Fixed.** Entry paints with one FAST. The cleansing HALF is idle‑triggered and never on a transition. §10.4. |
| 6 | CLIX: search bound to unlabelable long‑press Up (J3) | **Fixed.** Search is a visible pinned row at list index 0, reachable in 2 presses. §10.1. |
| 7 | CLIX: Left/Right overloaded on grid row 0, blocking horizontal movement (J3) | **Fixed.** Left/Right never leave the grid. Results are reached with Up. §9.3, §10.2. |
| 8 | CLIX: hand‑rolls a key grid the SDK already renders; window sized two ways (J2) | **Fixed.** `fui::keyGrid` with a 30‑entry RAM array. Every buffer is itemised once in §6.3. |
| 9 | L: 56 KB open gate makes the feature unavailable with WiFi up (J1) | **Fixed.** The streamed tier's gate is `9,216 + 20 KB free / 16,384 maxAlloc`, justified because the activity allocates nothing after `onEnter()`. The 48 KB house residual applies only to the optional pin. §6.2. |
| 10 | L: single contiguous 20 B × N record table; 96 B × N resident fold blob; truncation at scale (J1) | **Dropped with the design.** Nothing resident scales with N except the optional pin, which is capped at 32,768 B by construction. |
| 11 | L: truncates preview names; 48 px row breaks requirement 1 past ~101 chars; no route to search (J2, J3) | **Dropped with the design.** §1.1 and §10.2 give 411 px of untruncated result space and a visible search entry point. |
| 12 | One Shelf: O(N²) reconciliation on the entry path (J1) | **Dropped.** Reconciliation is an O(N) merge‑join over two already‑sorted sequences. §5.4. |
| 13 | One Shelf: un‑yielded 30–900 ms zip work on the input path (J1) | **Fixed.** All EPUB access is opt‑in, behind a progress popup with cancel, never on an automatic path. §5.6. |
| 14 | One Shelf: Tier‑2 scan undercounts because the blob interleaves names with fold keys (J1) | **Fixed structurally.** Display names live in their own section; the search scan never reads a display name. §3. |
| 15 | One Shelf: keyboard can disable a key leading to a currently displayed result (J3) | **Fixed** by CLIX's rule: the mask is the union over every displayed tier. §9.2. |
| 16 | One Shelf: row‑major Left/Right moves the cursor down‑and‑left (J3) | **Fixed** by a traversal that is spatially sane *and* provably reachable. §9.3. |
| 17 | One Shelf: `listNav{120,400}` outruns the panel (J3) | **Fixed.** House 500/500. |
| 18 | All three: naive skip orphans live keys (One Shelf's BFS finding) | **Fixed and proved.** §9.3. |
| 19 | All three: geometry computed in landscape | **Fixed.** §1.1. |

Two flaws are **accepted rather than fixed**, with reasoning, in §14: the `(name, size)` signature's blind spot, and `BookMetadataCache::load()`'s destructive behaviour.

---

## 3. On-disk format: `CLX1`

One file, `/.crosspoint/library.idx`. Staging: `/.crosspoint/library.stage`, `/.crosspoint/library.sortA`, `/.crosspoint/library.sortB`. Install by writing `/.crosspoint/library.new`, then `Storage.remove(final); Storage.rename(new, final)` (`HalStorage.h:40-41`) — the `Dictionary.cpp:485-499` idiom, so an interrupted build never leaves a half index.

All integers little‑endian. All structs `#pragma pack(push,1)` with `static_assert` on size, the discipline `lib/FileIndex/FileIndex.h:36-59` already uses. **Every section starts on a 512‑byte boundary**; padding bytes are zero.

### 3.1 Header — 64 bytes, padded to 512

```
off  sz  type      field               purpose
  0   4  char[4]   magic               "CLX1"
  4   1  u8        formatVersion       1. Checked FIRST; unknown => rebuild, never partial-parse.
  5   1  u8        foldVersion         1. Bumped when the fold or article table changes =>
                                       rebuild fold+ranks, preserve firstSeen.
  6   1  u8        flags               b0 walkComplete, b1 ranksDegraded, b2 enrichComplete,
                                       b3 booksAtRoot (author sort meaningless), b4-7 reserved 0
  7   1  u8        reserved0           0
  8   2  u16       bookCount           N, <= LIBRARY_MAX_RECORDS (4096)
 10   2  u16       folderCount         F, folder ids are dense ordinals 0..F-1
 12   2  u16       knownAuthorCount    books with a resolvable author key; lets author-descending
                                       reverse only the known block so unknown authors stay LAST
                                       in both directions with no extra field
 14   2  u16       nextFirstSeen       monotonic "first seen by this device" allocator
 16   2  u16       enrichCursor        resume ordinal for the optional metadata pass
 18   2  u16       longestName[3]      -- 3 x u16 = 6 bytes: ordinals of the three longest
 24                                       display names by BYTE length (row-height probe, §10.1)
 24   4  u32       folderStart         = 512
 28   4  u32       folderLen           unpadded length
 32   4  u32       recordStart         512-aligned
 36   4  u32       permStart           512-aligned; two u16 arrays of N entries
 40   4  u32       nameStart           512-aligned
 44   4  u32       nameLen             unpadded length of the display-name blob
 48   4  u32       selfSize            expected total file size; validity check
 52   4  u32       scanSignature       FNV-1a32 over the walk (§5.4)
 56   4  u32       lastWalkMs          duration of the walk that produced this index (§5.5)
 60   4  u32       totalBookBytes      sum of every fileSize; free secondary sanity check
```

`static_assert(sizeof(ClixHeader) == 64)`. Written as a placeholder, then seeked back and rewritten once the counts are known (`Dictionary.cpp:436-441`, `:479-482`).

**Validity, at zero read cost beyond one sector:** `magic == "CLX1" && formatVersion == 1 && foldVersion == 1 && file.fileSize64() == selfSize` (`HalStorage.h:83`). Anything else means rebuild. The exact‑size check is a free truncation guard against power loss, exactly `FileIndex.cpp:160-166`.

### 3.2 Folder section — F variable records, id = ordinal

```
  0   1  u8        pathLen        1..255
  1   n  char[n]   path           absolute directory path, no trailing '/', UTF-8 as on disk
```

Only directories containing at least one accepted book are emitted, so `folderId` is dense. This is the "paths pooled in one contiguous buffer" constraint, satisfied on SD at zero resident cost. It earns its place for the subtree filter (a `u16` compare instead of a string prefix compare) and for the author key and author group headers — not for compression, which at 3–4 books per author recovers only ~10% of the file.

### 3.3 Record section — N × **exactly 128 bytes**, in folded title order

```
off  sz  type       field
  0   4  u32        nameOff        byte offset from nameStart into the display-name blob
  4   4  u32        fileSize       captured while the dirent was still open
  8   2  u16        authorRank     position in author order, 0..N-1
 10   2  u16        dateRank       position in first-seen order, oldest = 0
 12   2  u16        firstSeen      monotonic id
 14   2  u16        folderId       ordinal into the folder section
 16   1  u8         nameLen        display basename length, 1..255
 17   1  u8         foldLen        bytes of `fold` in use, 0..96
 18   1  u8         authorKeyLen   bytes of `authorKey` in use, 0..12
 19   1  u8         flags          b0-2 format (0 epub, 1 txt, 2 md, 3 xtc, 4 other)
                                   b3-4 author provenance (0 folder, 1 book.bin, 2 OPF,
                                        3 tried and failed)
                                   b5 titleFromOpf, b6 opfTooLarge, b7 reserved
 20  96  char[96]   fold           folded, article-stripped search text (§4.1). NOT NUL-terminated.
116  12  char[12]   authorKey      folded author sort key (§4.3). NOT NUL-terminated.
```

`static_assert(sizeof(ClixRecord) == 128)`.

The fixed 128‑byte stride is the load‑bearing decision of this format and it buys four things at once:

1. **Record *k* is at `recordStart + 128k`** — O(1) random access with no offset table.
2. **32 records fit exactly in a 4096‑byte buffer**, so a streaming scan has **no straddle handling at all** — no spill buffer, no partial‑record state machine, and a whole class of off‑by‑one bugs does not exist.
3. Since `recordStart` is 512‑aligned and 128 divides 512, **every chunk read is 512‑aligned by construction**, not by remembering to align it.
4. The pinned tier (§6.1) is a straight `memcpy` of the section into an Arena with no re‑layout.

The cost is ~38% more search I/O than a variable‑length packing (128 B vs ~93 B per record). §7 shows this is bought back many times over by removing the per‑page pass.

`titleRank` costs zero bytes — it is the record ordinal, because the section *is* in title order. Title Z‑A costs zero bytes — it is `N−1−ordinal`.

### 3.4 Permutation section — 2 arrays × N × `u16`

```
permStart + 0        : authorOrder[N]  -- record ordinal of the k-th book in author order
permStart + 2N       : dateOrder[N]    -- record ordinal of the k-th book in first-seen order
```

Padded to a 512‑byte boundary. This is `FileIndex`'s "blob in discovery order plus a separate sorted permutation array" primitive (`FileIndex.h:36-58`), which both the performance and feasibility judges named as the right shape. It exists solely so that **paging is one seek in every order**, and it is what kills CLIX's worst flaw. Reverse orders need no storage: iterate the array backwards, stopping at `knownAuthorCount` for author‑descending so unknown authors stay last.

### 3.5 Display-name blob

`N × nameLen` raw basenames **including the extension**, in record order (= title order). No NULs; lengths are explicit. This is the only string the UI ever draws, so requirement 1 is met by storing nothing else and never shortening it. Because record order equals default display order, **one page of 13 rows is one contiguous ~1 KB read.**

### 3.6 Deliberately not stored

No trie, no automaton, no n‑gram postings, no bloom filter, no suffix structure, no successor bitmap, no content hash, no mtime, no per‑order offset table beyond the two permutations, no cover data.

### 3.7 Exact size arithmetic

Assumptions, stated once: display basename 80 B mean; folded search text 66 B mean (extension stripped, separator runs collapsed, leading article stripped on ~18% of titles, author key appended); author key present on 70% of books; folder path 28 B; one folder per author with ~4 books each, so F = N/4.

| Section | Formula | N=200, F=50 | N=1000, F=250 | N=2000, F=500 |
|---|---|---|---|---|
| Header | 512 | 512 | 512 | 512 |
| Folder | `ceil(29F / 512) × 512` | 1,536 | 7,680 | 14,848 |
| Records | `128N` (already 512‑aligned for N mod 4 = 0; else padded) | 25,600 | 128,000 | 256,000 |
| Permutations | `ceil(4N / 512) × 512` | 1,024 | 4,096 | 8,192 |
| Names | `80N` | 16,000 | 80,000 | 160,000 |
| **Total (`selfSize`)** | | **44,672 B** | **220,288 B** | **439,552 B** |
| | | 43.6 KiB | 215.1 KiB | 429.2 KiB |

**Bytes read by one search pass** = the record section only: **25,600 / 128,000 / 256,000 B**. The folder section, permutation arrays and name blob are never touched during matching.

At 50 and 100 books (the real corpus) the file is 11,776 B and 22,528 B, and the record section is 6,400 B and 12,800 B.

---

## 4. Fold, keys and heuristics

### 4.1 The fold — one function, one file, `src/activities/library/LibraryFold.cpp`

Defined once and called by both the writer and every reader. The stored bytes **are** the sort order, which deletes the fragile "is the file sorted the same way I compare?" invariant that pins `Dictionary` to ASCII (`src/util/Dictionary.cpp:390-402`).

```
fold(basename) ->
  1. strip the trailing extension (.epub/.txt/.md/.xtc)
  2. utf8ComposeNfc (lib/Utf8) so decomposed accents fold identically
  3. map U+00C0..U+017F through a 192-byte flash table to unaccented ASCII lowercase,
     with the two-character expansions ae, oe, ss, th, d
  4. ASCII A-Z -> a-z; digits kept
  5. every other codepoint -> one space; collapse space runs; trim
  6. strip a leading article: "the ", "a ", "an ", "le ", "la ", "les ", "l'",
     "der ", "die ", "das ", "el ", "los ", "las ", "il ", "un ", "une "
  7. if an author key is known, append 0x20 then the folded author key
  8. truncate to 96 bytes
```

Step 6 is not cosmetic: keeping leading articles leaves a mean of 37.9 matches at query depth 3 versus 2.7 stripped, because ~18% of titles collapse under "the". Step 7 is what makes "sanderson" find books whose filename does not contain the author.

Because separator runs collapse to exactly one `0x20`, the **word‑initial test is a single byte compare**: `p == 0 || fold[p-1] == ' '`. No token table, no token offset array.

`foldVersion` in the header exists so that changing this function forces a rebuild of the fold and ranks while preserving `firstSeen` — the "rebuild author keys" action the prior art says to budget from day one.

### 4.2 Sort keys — fixed 16 bytes, ties impossible

```
struct SortKey {          // 16 B, static_assert
  uint8_t  key[12];       // title: fold[0..11]; author: authorKey[0..11]; date: firstSeen BE + zeros
  uint16_t seq;           // walk-order ordinal -- the tiebreak that makes ties impossible
  uint16_t slot;          // staging slot index
};
```

Comparison is `memcmp(key, 12)` then `seq`. **No comparator ever touches SD.** That is the explicit fix for `FileIndex`'s documented build‑time cliff, where tie groups are resolved by `sortSegmentByName` doing two seek‑and‑read operations per comparison (`lib/FileIndex/FileIndex.cpp:492-508`) with a 64‑entry cap and an external merge above it.

Ending every key in `seq` (Design 2's graft) makes the sort deterministic and reproducible across rebuilds, which removes the "adjacent same‑prefix books swap places" risk entirely.

`FsHelpers::naturalSortKey(const char*, uint8_t*, size_t)` (`lib/FsHelpers/NaturalSort.h:9`) is **not** used. Its digit‑run marker is `0x30`, which is also the literal character `'0'`, and the aliasing is a real ordering hazard on titles containing digits. Plain `memcmp` over the fold is correct, predictable and host‑testable. Natural (numeric‑aware) ordering of series volumes is explicitly out of scope for v1 (§13).

### 4.3 Author key

Ported line for line from CrumBLE's `lastNameLowerForKey`, credited at the call site, in exactly **one** place (their two byte‑identical copies were a documented trap). Three of its four rules are field bug fixes:

```
trim -> split on ';' FIRST (the standard multi-author dc:creator separator)
     -> take the part before ',' if present, else the token after the last space
     -> strip trailing . , ; : ! ?
     -> fold (§4.1 steps 2-4)
     -> strip embedded tabs
     -> truncate to 12 bytes
```

Source, in priority order: **(a) the parent folder basename** (free, correct for the stated one‑folder‑per‑author layout, provenance 0); **(b) `BookMetadataCache::coreMetadata.author`** when a `book.bin` already exists (provenance 1); **(c) a targeted OPF read** (provenance 2); **(d) tried and failed** (provenance 3), a sentinel so a corrupt OPF is never re‑peeked forever. (b), (c) and (d) are opt‑in only — §5.6.

If more than 60% of records share one `folderId`, the build sets header flag b3 and the sort menu labels author sort "Author (unavailable — books are not in author folders)" and offers the opt‑in metadata pass instead of silently sorting by folder name.

---

## 5. Build, invalidation and staleness

### 5.1 Walk

Recursive DFS from `/`, using the open‑child‑handle form (`child.openNextFile()`, `child.isDirectory()`, `HalStorage.h:99`, `:96`) rather than closing and re‑opening by path — re‑opening re‑scans the parent directory on every descent and is roughly a 10× cost on the walk. Depth cap 8. Skip any name starting `.` (so `/.crosspoint` is excluded), macOS/Windows metadata entries, and a case‑insensitive directory blacklist. Accept via `isSupportedBrowserFile`‑equivalent logic: `FsHelpers::hasEpubExtension / hasTxtExtension / hasMarkdownExtension / hasXtcExtension` (`lib/FsHelpers/FsHelpers.h:52-63`), excluding the image formats the browser accepts. A non‑book stem blacklist (readme, license, changelog, authors, cover, metadata) keeps publisher `.txt` files out.

Name buffer **512 bytes**, matching `FileIndex::MAX_NAME = 511` (`lib/FileIndex/FileIndex.h:11`) and `FileBrowserActivity`'s `NAME_BUFFER_SIZE`. Capture `fileSize()` while the entry is still open. Close each *file* entry before recursing so handles do not pile up. `maybeYield` every 256 entries, the `lib/FileIndex/FileIndex.cpp:41-43` idiom (`if ((++counter & 0xFF) == 0) delay(1);`).

Because the walk is depth‑first, all books in a folder are consecutive, so **folder lookup is O(1)** — a new folder is appended when we descend. Folder paths accumulate in an Arena of 4096‑byte slabs (1.5 / 7.3 / 14.5 KB at 200 / 1000 / 2000 books) and are written to the final file after the walk, so **only one output file is ever open during the walk**.

### 5.2 Staging

One file, `/.crosspoint/library.stage`, fixed **384‑byte slots** per book: `{128 B ClixRecord with ranks unset, u8 nameLen, char name[255]}`. Fixed slots make staging directly addressable by walk ordinal with no index, at the cost of 768 KB of transient SD at 2000 books. Written through `BufferedFileWriter(file, 4096)` (`lib/Serialization/BufferedFile.h:26`), which degrades to unbuffered passthrough on OOM — so there is **no OOM branch to write**.

### 5.3 Sort

Three orders, each over the 16‑byte `SortKey` (§4.2), run **sequentially in one Arena** (`Arena.h:61-69`; `clear()` at `:103-115` keeps the first slab so reuse cannot fragment):

- **N ≤ 512**: read all keys into one 8,192‑byte block and `std::sort`. Zero merge passes. This is the entire real corpus.
- **N > 512**: chunked external merge — `std::sort` 512 keys at a time (8,192 B), append each sorted run to `library.sortA`, then bottom‑up 2‑way merge passes ping‑ponging A/B with run length doubling. At N = 2000 that is 4 runs and 2 passes. Streams sequentially through 4 KB buffers; **never seeks per record**, unlike `FileIndex::mergeRuns` (`FileIndex.cpp:344-444`), and never resolves ties (they cannot occur).

Build RAM peak: 8,192 (key chunk) + 4,096 (writer) + 4,096 (merge reader) + 512 (name) + 288 (path) + ~960 (8 nested directory handles) + Arena slabs ≈ **≤ 18 KB at 2000 books, ≤ 14 KB at 200**, largest single block 8,192 B. That clears the 20,000 B WiFi‑up maxAlloc floor and is one order below the 44 KB EPUB text‑layout gate (`lib/MemoryBudget/MemoryBudget.h:34-35`).

**Degrade:** if the 8,192‑byte chunk cannot be allocated, fall back to a 2,048‑byte chunk (128 keys, more merge passes). If even that fails, write in walk order, set header flag b1 (`ranksDegraded`), and draw the author and date sort options disabled with an explanation. Never fatal.

### 5.4 Emit and reconciliation

Emit is two sequential passes over the title‑ordered key list, each seeking into the fixed‑slot staging file: pass A writes the record section (computing `nameOff` as a running sum, since names are emitted in the same order) and the two permutation arrays; pass B writes the name blob. 2N staging seeks total.

When a valid previous index exists, reconciliation is an **O(N) merge‑join**: both the old index and the newly sorted staging are in the same folded‑title order, so one sequential pass over each matches on `(folderId path, name)`.

- equal name, equal size → inherit `firstSeen`, `authorKey` and provenance
- equal name, different size → treated as replaced: fresh `firstSeen`, author key invalidated to provenance 0
- present only in the new set → `firstSeen = nextFirstSeen++`
- present only in the old set → dropped

There is **no hash map** anywhere in this path. A node‑based container is one heap allocation per element and is the fragmentation this codebase fights; a merge‑join over two already‑sorted sequences is both cheaper and allocation‑free.

`nextFirstSeen` overflow at 0xFFFF triggers a renumber by current `dateRank` and a reset. This path will essentially never run in the field, which is exactly why it needs a host unit test seeding a near‑overflow header.

### 5.5 Staleness policy

`scanSignature` = FNV‑1a32 over, per accepted book **in walk order**: folder path bytes, `0x00`, basename bytes, `0x00`, `fileSize` as 4 LE bytes. Compared alongside `bookCount` and `folderCount`. It detects add, remove, rename, resize and move.

The policy, which is scale‑adaptive rather than heuristic:

1. **On every Library entry**, read the header (one sector), validate, and **paint the list immediately**. Nothing blocks the first pixel.
2. **If `lastWalkMs` ≤ `LIBRARY_AUTOWALK_MAX_MS` (600)**, run the verification walk afterwards in `loop()` slices, one directory per tick. If the signature matches — the normal case — nothing redraws at all. If it differs, rebuild behind `GUI.drawPopup(renderer, tr(STR_LIBRARY_INDEXING))` under a `RenderLock` (the `FileBrowserActivity.cpp:313-316` pattern) and repaint once. Redraw requests coalesce (`ActivityManager.cpp:196-202`, `:71`), so one change costs one refresh.
3. **If `lastWalkMs` > 600** (roughly, above ~500 books or on a slow card), auto‑walk only on the **first Library entry after each boot**. Otherwise the index is refreshed only by the explicit **Refresh** action.
4. **"Refresh library"** is the first item of the sort/filter menu and is the documented guarantee.
5. Power loss mid‑build leaves either the previous valid index or orphan staging files; recovery is `Storage.remove()` on both and a re‑walk. **Walking is not resumable** — it is a sub‑second‑to‑few‑second operation whose install is a rename, so checkpointing it is pure cost.

At the real corpus size the walk is ~250 ms, rule 2 applies, and the index is never stale. In the headroom range rule 3 applies and staleness is bounded by an always‑visible one‑press action. That is honest and it is the right trade at each scale.

### 5.6 Optional metadata enrichment (M4)

Never automatic, never on the cold path, never blocking. Triggered by the menu action "Get titles and authors from books" or by the free ride (the user opens a book; the reader builds `book.bin` anyway; on return the Library patches that one record in place — one seek, one 128‑byte write).

The bulk pass, when N ≤ 1024, first lists `/.crosspoint` **once, sequentially**, `strtoull`s each `epub_<hash>` directory name into a sorted `u64` array (2N × 8 B ≤ 16 KB, in Arena slabs), and turns "does this book have a cache?" into a RAM binary search. The naive alternative — `Epub::hasCache(path, "/.crosspoint")` per book (`lib/Epub/Epub.h:112`) — re‑scans a directory holding ~2N subdirectories every time. Above 1024 books the array is skipped and the pass runs one book per idle `loop()` tick using `hasCache` directly.

On a hit: `BookMetadataCache mc(Epub::cachePathForFilePath(path, "/.crosspoint")); mc.load();` then `mc.coreMetadata.author`. On a miss: a targeted OPF read via `ZipFile::readFileToMemory` for `META-INF/container.xml` and then the OPF, gated at `LIBRARY_OPF_MAX_BYTES = 16384` so the one‑shot inflate path is taken and the 32 KB streaming window is never claimed. `Epub::load(buildIfMissing = true)` is **never** called — it builds the whole spine/TOC/CSS cache, seconds per book.

Cadence and guards: `vTaskDelay(1)` every 8 books; heap gate before each peek requiring `free ≥ 16384 + 11264 + 44 KB` and `maxAlloc ≥ 32 KB` (`MemoryBudget.h:34-35`); `shouldCancel` on Back; resumable via the in‑place `enrichCursor` `u16`, rewritten every 16 books.

---

## 6. Runtime model

### 6.1 Two tiers, one code path

The scanner consumes a `const ClixRecord*` sequence. Where those pointers come from is the only difference between tiers.

- **Tier P (pinned)**, when `128N ≤ LIBRARY_PIN_MAX_BYTES (32,768)` — i.e. **N ≤ 256**, covering the entire real corpus — the record section is `memcpy`'d at open into an Arena of 4096‑byte slabs. Every subsequent search and page turn is pure CPU with **zero SD reads**.
- **Tier S (streamed)**, above that, the scanner reads the section in 4096‑byte chunks at 512‑aligned offsets into one owned buffer, 32 records at a time, with no straddle.

There is no third tier and no fallback logic beyond the heap gate. The scan loop is byte‑identical in both.

### 6.2 Heap gates — named constants, house arithmetic

```
LIBRARY_OPEN_MIN_FREE       = 9,216 + 20*1024 = 29,696   // must remain free after opening
LIBRARY_OPEN_MIN_MAX_ALLOC  = 4,096 + 12*1024 = 16,384
LIBRARY_PIN_MAX_BYTES       = 32,768
LIBRARY_PIN_MIN_FREE_AFTER  = 48*1024                     // house residual, FileBrowserActivity.cpp:39
LIBRARY_PIN_MIN_MAX_ALLOC   = 4,096 + 16*1024
```

The open gate deliberately does **not** demand the house 48 KB residual, and the justification is structural rather than optimistic: **the Library allocates nothing after `onEnter()` returns.** Every buffer in §6.3 is allocated once, the scan loop calls only `memchr`, `memcmp` and `HalFile::read`, and no `std::string` or `std::vector` is constructed on any interactive path. The 48 KB residual exists in `FileBrowserActivity` because it grows an unbounded vector per entry (`FileBrowserActivity.cpp:83-92`); we do not. This is what lets the Library open with WiFi up at 35,000 B free, which is precisely where Design 2 failed.

The pin is the only large allocation and it keeps the full house residual. If the pin gate fails, Tier S runs — slower, never absent. If the open gate fails, the screen renders `tr(STR_MEMORY_ERROR)` with a "Browse files" exit, the `FileBrowserActivity.cpp:1046-1049` degrade verbatim. **The Library is never the only route to a book.**

### 6.3 Resident footprint, itemised

| Buffer | Bytes | Note |
|---|---|---|
| Record scan buffer (Tier S only) | 4,096 | 32 records, 512‑aligned reads |
| Name page buffer | 4,096 | holds one page of display names, NUL‑terminated in place |
| Row label pointer array | 24 × 4 = 96 | `const char*` into the name page, fed to `fui::ListItem` |
| Viewport window | 24 × 12 = 288 | `{u32 effRank, u32 nameOff, u16 recId, u8 nameLen, u8 pad}` |
| Mutable key grid | 30 × ~48 = 1,440 | `fui::KeyGridKey[30]`, allocated in `onEnter()` |
| Next‑char mask | 32 | 256‑bit |
| Folded query | 64 | |
| Header copy | 64 | |
| Folder id → offset cache | 16 × 8 = 128 | direct‑mapped |
| Permutation page | 512 | one sector of `authorOrder`/`dateOrder` |
| Cursor, counters, selection, flags | ~128 | |
| One open `HalFile` | ~120 | |
| **Tier S total** | **≈ 11.1 KB** | largest single allocation **4,096 B** |
| **Tier P total** | **≈ 11.1 KB + 128N** | ≤ 43.9 KB at N=256, in ≤ 9 slabs of 4,096 B |

At the real corpus (100 books) Tier P is **11.1 + 12.8 = 23.9 KB**, largest block 4,096 B, and everything is released in `onExit()`.

---

## 7. The per-keystroke algorithm, with a proven bound

### 7.1 The pass

One forward pass over the record section produces, simultaneously and with no second data structure: the **match count**, the **viable next‑character set**, the **tier classification**, and the **first page of results in the active sort order**.

```c
// State: all fixed-size, allocated in onEnter(). No allocation occurs below.
uint16_t matchCount = 0;
uint8_t  nextChars[32] = {0};                       // 256-bit mask
Slot     window[LIBRARY_VIEWPORT_MAX];              // 24 slots x 12 B
uint8_t  windowFill = 0;

for (uint16_t seq = 0; seq < hdr.bookCount; ++seq) {
    const ClixRecord* r = tierP ? &pinned[seq] : chunkRecord(seq);   // 4 KB chunk, 32/chunk

    if (!formatAllowed(r->flags))   continue;       // 1 bit test
    if (!folderAllowed(r->folderId)) continue;      // 1 bit test

    const char* f = r->fold;
    const uint8_t n = r->foldLen;
    uint8_t tier = 3;                               // 3 = no match

    if (qlen == 0) {
        tier = 0;
        for (uint8_t p = 0; p < n; ++p) setbit(nextChars, (uint8_t)f[p]);
    } else {
        const char* p = f;
        const char* end = f + n;
        while ((p = (const char*)memchr(p, q[0], (size_t)(end - p))) != NULL) {
            if ((size_t)(end - p) < qlen) break;
            if (qlen == 1 || memcmp(p + 1, q + 1, qlen - 1) == 0) {
                const bool wordInitial = (p == f) || (p[-1] == ' ');
                const uint8_t t = (p == f) ? 0 : (wordInitial ? 1 : 2);
                if (t < tier) tier = t;
                if (p + qlen < end) setbit(nextChars, (uint8_t)p[qlen]);
            }
            ++p;
        }
    }

    if (tier == 3) continue;
    ++matchCount;
    const uint32_t effRank = ((uint32_t)tier << 24) | rankOf(seq, r, order);
    windowInsert(window, &windowFill, effRank, r, seq);   // <= 24 compares
}
```

`rankOf` is the only thing the six orders differ by — a five‑line switch with no branches in the hot loop:

| Order | `rankOf` |
|---|---|
| Title A‑Z | `seq` |
| Title Z‑A | `N − 1 − seq` |
| Author A‑Z | `r->authorRank` |
| Author Z‑A | `authorKeyLen == 0 ? r->authorRank : knownAuthorCount − 1 − r->authorRank` |
| Date newest | `N − 1 − r->dateRank` |
| Date oldest | `r->dateRank` |

Packing `tier` into the high byte of `effRank` means the existing selection window orders results for free: strict‑prefix hits first, then word‑initial, then interior substring, each block internally in the user's chosen sort order. **There is no search‑mode setting**, and the interior case was already being computed.

### 7.2 Proven bound on work

Let `N` = `bookCount`, `L` = `foldLen` of a record (≤ 96), `V` = `LIBRARY_VIEWPORT_MAX` = 24, `Q` = query length (≤ 32, enforced by the input buffer).

- **Byte comparisons.** `memchr` scans each record's fold at most once end to end: ≤ `L` probes. Each probe that hits costs at most `Q − 1` `memcmp` bytes. Worst case per record is `L + (L × (Q−1))`, but the query bytes are drawn from the folded alphabet and the measured first‑byte hit rate never exceeds 0.58 per blob byte, so the practical bound is `L × (1 + 0.58 × (Q−1))`. **Adopt the hard bound `L × Q` for budgeting**, at 6 cycles/byte.
- **Window inserts.** At most `V` compares per *matching* record, so ≤ `N × V` compares total, ~4 cycles each.
- **Total** ≤ `6 × Σ L × Q + 4 × N × V` cycles at 160 MHz.
- **I/O**, Tier S: exactly `ceil(128N / 4096)` aligned reads of 4096 B, plus one seek. Tier P: zero.
- **Early exit: none.** There is no data‑dependent termination, so **worst case equals average case**. Query `"e"` costs exactly what query `"zq"` costs. This is the property that makes the design safe to budget.

**Allocations during the pass: zero.** Proven by construction — every buffer is allocated in `onEnter()`; the loop body calls only `memchr`, `memcmp`, `setbit` and, in Tier S, `HalFile::read` into a pre‑allocated buffer. No `std::string`, no `std::vector`, no `new`. Under `-fno-exceptions` this matters: an allocation failure here would be an `abort()`, not an error path.

Worked numbers at `Q = 2` (the measured optimal typing depth) and mean `L = 66`:

| N | Fold bytes | CPU (6 cyc/B, Q=2) | Window compares | Tier S I/O @400 / 800 / 1500 KB/s |
|---|---|---|---|---|
| 200 | 13,200 | 0.99 ms | 4,800 → 0.12 ms | *pinned: 0* |
| 1000 | 66,000 | 4.95 ms | 24,000 → 0.60 ms | 320 / 160 / 85 ms |
| 2000 | 132,000 | 9.90 ms | 48,000 → 1.20 ms | 640 / 320 / 171 ms |

### 7.3 Rendering the page

The window holds offsets, not names. After the pass, walk the window in display order and read the display names. In title order (the default and the common case) the `nameOff` values are **contiguous**, so one ~1 KB read fills the entire page: 1.3–2.6 ms. In author or date order they are scattered: ≤ 13 seeks ≈ 5.2 ms. Names land NUL‑terminated in the 4096‑byte name page buffer, and the row label array holds `const char*` into it — exactly what `fui::ListItem::label` requires (`freeink-sdk/.../components/lists/list.h:9`).

If a page's names would exceed the 4096‑byte page buffer (13 × 81 = 1,053 B typical; the bound is only reached with 16+ rows of 250‑byte names), the visible row count for that page is reduced. Documented; effectively unreachable.

### 7.4 Term-by-term keystroke latency

Every latency claim in this project must be stated in this form.

| Term | N=200 (Tier P) | N=2000 (Tier S @800 KB/s) | Source |
|---|---|---|---|
| Input tick | 0–10 ms | 0–10 ms | `src/main.cpp` `delay(10)` while active |
| Record I/O | 0 | 320 ms | §7.2 |
| Scan CPU | 0.99 ms | 9.90 ms | §7.2 |
| Window inserts | 0.12 ms | 1.20 ms | §7.2 |
| Key‑mask → 30 `enabled` flags | <0.01 ms | <0.01 ms | 30 bit tests |
| Name reads for visible rows | 1.3–5.2 ms | 1.3–5.2 ms | §7.3 |
| **Render CPU** | **15–40 ms (UNMEASURED)** | **15–40 ms (UNMEASURED)** | §12, measurement 1 |
| DTM2 SPI | 26 ms | 26 ms | 52,272 B at 16 MHz |
| BUSY confirm poll | 1–50 ms (typ. 1–2) | 1–50 ms | bounded 50 ms poll in `displayStart` |
| `_fast` waveform | ~133 ms | ~133 ms | 19 LUT frames × ~7 ms |
| DTM1 post‑sync | 26 ms | 26 ms | driver `displayFinish` |
| Loop tail | 0–10 ms | 0–10 ms | `main.cpp` |
| **Typical** | **≈ 225 ms** | **≈ 545 ms** | |
| **Worst** | **≈ 301 ms** | **≈ 631 ms** | |

At N=200 the application owns **1.1 ms of a 225 ms keystroke — 0.5%.** The panel owns 82%. **The index is not the bottleneck and cannot be made one**, which is the formal statement of why no index structure appears anywhere in this design.

At N=2000 the record I/O becomes 59% of the keystroke, and there are two escalations, both gated on measurement rather than pre‑emptively built:

- **Async overlap.** Start the refresh with `renderer.displayBufferAsync(mode)` and run the scan during the ~133 ms waveform, joining with `waitRefreshComplete()` — copying `src/activities/reader/ReaderUtils.h:183-195` verbatim, including its baseline‑rebuild caveat. This hides 133 ms, bringing N=2000 to ~412 ms. Must stay single‑threaded: the async plane send takes no SPI bus lock, and the SD shares that bus. Silently disabled when the user's sunlight fading fix is on, so nothing may *depend* on it.
- **Refine pin.** After any pass, if `matchCount ≤ 96`, copy the survivors' fold text into a pre‑allocated 96 × 104 = 9,984 B buffer. Because the query only grows, subsequent keystrokes scan that buffer instead (96 × 66 = 6.3 KB → 0.25 ms). Backspace, Clear, sort change and filter change invalidate it and force a full pass. This caps a whole search session at two slow keystrokes.

Both are **M5, conditional on measurement 3** (§12).

---

## 8. Paging and sorting — O(1), in every order

Row *p* in the current order resolves as:

```
title A-Z   : ordinal = p
title Z-A   : ordinal = N-1-p
author asc  : ordinal = authorOrder[p]                    // one u16 read
author desc : ordinal = authorOrder[p < unknownCount ? N-1-p : ...]   // see below
date newest : ordinal = dateOrder[N-1-p]
date oldest : ordinal = dateOrder[p]
```

Then the record is at `recordStart + 128 × ordinal` and the name at `nameStart + r->nameOff`. Author‑descending iterates `authorOrder[knownAuthorCount-1-p]` for `p < knownAuthorCount` and then the unknown‑author tail in forward order, so **unknown authors sort last in both directions** with no extra field and no extra pass.

A page turn with **no active query** therefore costs: one 512‑byte permutation‑page read (often cached), 13 record reads (one contiguous 1,664 B read in title order; 13 scattered reads otherwise), and one name read. **3–10 ms.** No pass. This is the fix for the spine's worst flaw, and it holds at N=2000 exactly as it holds at N=50.

A page turn **with** an active query costs one pass, because the match set must be re‑derived. In Tier P that is 1.1 ms. In Tier S it is a full pass — which is why the refine pin (§7.4) targets exactly that case.

Changing sort order costs one permutation read and a repaint. Changing a filter costs one pass. Neither rewrites the index.

---

## 9. The constrained keyboard

### 9.1 Grid

**6 columns × 5 rows = 30 cells**, at 82 × 48 px, occupying y 489..729, drawn with `fui::keyGrid` (`freeink-sdk/libs/ui/FreeInkUI/include/components/keyboard/key-grid.h:52`), which already maps `!key.enabled || key.kind == KeyKind::Disabled` to `StateDisabled` and its 25% dither foreground (`key-grid.h:64`, `:76`). The visual half of the constraint is free with no SDK change.

```
a b c d e f
g h i j k l
m n o p q r
s t u v w x
y z ␣ ⌫ CLR OK
```

Alphabetical, near‑square, and **positions never move.** Near‑square alphabetical beats the shipped 41‑key 5‑row QWERTY by ~16–20% on mean travel (2.70 vs 3.20 presses by cyclic‑distance arithmetic; 2.49 vs 3.13 by simulation). Reordering keys by probability is a known failure — FOCL cut theoretical KSPC 35% with no measured speed gain, because visual search cost swamps travel savings. So: **skip dead keys, never relocate live ones.**

The SDK's built‑in layouts are `static const` in flash, so the activity owns a mutable `fui::KeyGridKey keys[30]` array (~1,440 B) allocated in `onEnter()`, with 26 single‑character labels from a `static const char kLetters[26][2]` table (52 B of flash).

`␣`, `⌫`, `CLR` and `OK` are **never disabled and never skipped.** That guarantees the bottom row always has live cells, which is load‑bearing for the reachability proof below, and it is the escape hatch that makes every state recoverable. (KOReader's `FocusManager` does the same thing, looping past `is_inactive` items.)

Initial cursor position is the letter with the highest depth‑0 match count, which the pass already computed for free. MacKenzie's KSPC work measures cursor‑reset policy as worth more than prediction (10.66 → 6.45 from a central start plus snap‑home alone). After each keystroke the cursor **stays on the key just pressed**, so a mis‑press is one Left away from correction.

### 9.2 Viable-next-character set

Computed inside the same pass as everything else, into the 32‑byte mask (§7.1). Total keyboard state: **32 bytes**.

**Correctness rule, non-negotiable: the mask is the union over *every tier displayed*.** Skipping a key that would have produced a result the user can currently see is a bug; offering a key that produces nothing costs one wasted press. This is the direct fix for the flaw where a substring‑only result is on screen but the key leading to it is dead.

Consequence: **from keystroke 2 onward there is no reachable zero‑result query.** That property — not the press saving — is the feature's payoff. It deletes an entire error state, an entire "no results" screen, and the back‑out‑and‑retry trap. Measured: 0 reachable dead‑end prefixes. Automotive precedent: Audi MMI's speller states that illogical letters "are not available for selection", i.e. it skips rather than dims.

At depth 0 every letter is live (24–26 of 26 at these corpus sizes), so the constraint is invisible on the very first keystroke. Three safety nets cover it: the match count is drawn from keystroke 1; `⌫` and `CLR` are never skipped; and Back from the search screen returns to the **unfiltered list**, never to Home.

Honest accounting: **dimming alone saves exactly zero presses**, because a dimmed key still occupies a grid cell. Skipping saves 0.3 / 1.8 / 3.6 presses at query depth 1 / 2 / 3. At the optimal depth of 2 that is ~1.8 presses. Dimming ships alongside skipping only so the cursor's jumps look intentional.

### 9.3 D-pad traversal — spatially sane and provably reachable

`KeyboardEntryActivity::moveSelectionCol` is a bare `(selCol + delta + cols) % cols` with no `enabled` check (`src/activities/util/KeyboardEntryActivity.cpp:213-219`), and `moveSelectionRow` likewise (`:199-211`). Per the duplicate‑rather‑than‑modify rule, the Library search screen owns its own traversal:

```c
static bool rowHasLive(uint8_t r);   // OR of enabled[] across the row

void moveCol(int d) {                                    // Left / Right: stays in the row
    for (int k = 1; k < COLS; ++k) {
        int c2 = (col + d*k + COLS*COLS) % COLS;
        if (live(row, c2)) { col = c2; return; }
    }
    // row has exactly one live cell (the current one): no-op
}

void moveRow(int d) {                                    // Up / Down: next NON-EMPTY row
    for (int k = 1; k < ROWS; ++k) {
        int r2 = (row + d*k + ROWS*ROWS) % ROWS;
        if (!rowHasLive(r2)) continue;
        col = nearestLiveCol(r2, col);                   // min |c - col|, ties toward smaller c
        row = r2;
        return;
    }
    // only this row has live cells: no-op
}
```

**Reachability theorem.** *Every live cell is reachable from every other live cell in at most `(ROWS−1) + (COLS−1) = 9` presses.*

*Proof.* The action row is always fully live, so the set `R` of rows containing at least one live cell is non‑empty. `moveRow(+1)` visits the members of `R` in cyclic order and skips no member, so from any row in `R` every other row in `R` is reachable in at most `|R| − 1 ≤ ROWS − 1` presses. Within a row, `moveCol(+1)` visits every live cell of that row in cyclic order, so every live cell in the row is reachable in at most `COLS − 1` presses. Any live cell lies in some row of `R`. ∎

This is precisely what the naive implementation fails: the naive `moveRow` requires the *current column* to be live in the destination row, which orphans live cells whose column is dead everywhere on the path (measured: 42 of 200 targets unreachable at depth 3, 9 of 200 at depth 2). Making `moveRow` seek the next **non‑empty row** and then slide to the nearest live column fixes it while keeping vertical movement column‑preserving whenever possible — so the mental map stays stable, and Left/Right never wander into another row. Row‑major Left/Right (the alternative fix) is rejected: it makes a Right press move the cursor down‑and‑left every sixth time, which on a physical d‑pad reads as a malfunction.

**Wrap‑around is preserved in both axes.** This is not optional: losing wrap costs ~25% more travel on a 6×5 grid (2.56 → 3.21), which is more than skipping gains. A skip rewrite that drops the modulo is a net regression.

---

## 10. Interaction model

### 10.1 Screen A — `LibraryListActivity` (the default entry point)

Opening the Library lands directly on the list. There is no intermediate screen.

**Header** — `CompactHeader::drawTitle(renderer, tr(STR_LIBRARY), false)` (`src/components/CompactHeader.cpp:35`), with a right‑aligned status readout: `"37 of 214 · Newest"` or `"9 matches · sand"`. This readout is **mandatory**, not decorative: `drawListScrollIndicator` clamps the thumb to a 12 px minimum (`freeink-sdk/.../components/lists/list.h:135`), so at 214 items it moves 1.9 px per item and cannot express position. It costs ~0.3% of the panel.

**Rows** — `fui::list` with a uniform row height and `props.labelText.maxLines = libraryLineCount`, exactly the mechanism the fork shipped for `FILE_BROWSER_DISPLAY_FULL`. `libraryLineCount` is computed **once per screen entry**, not per row: read the three `longestName[3]` ordinals from the header, seek each name, call `fui::measureWrappedText(target, name, style, 480)` (`freeink-sdk/.../FreeInkUICore.h:740`), take the maximum line count, clamp to `[1, 4]`. Three text measurements per entry, never per render.

This matters because `getTextWidth()` allocates a `std::string` and does two map lookups per call (`lib/GfxRenderer/GfxRenderer.cpp:1042-1053`) — deriving row height from per‑item wrapped line counts would measure every entry on every render, which is the shape of the slow‑file‑browser reports the fork's own commit cites.

Row height then follows the theme, duplicating the ~8‑line helper the browser uses (it is file‑local in an anonymous namespace, so this is an additive copy, not a shared edit): `uiListRowHeight(tokens, SingleLine)` for 1 line, `WithSubtitle` for 2, and `withSubtitle + (lines−2) × (withSubtitle − singleLine)` beyond (`src/components/UIThemeTokens.h:37-48`). **On X3: 30 / 50 / 70 / 90 px → 22 / 13 / 9 / 7 rows.**

Names longer than 4 lines (~180 characters, above the firmware's own 150‑byte web‑upload limit) are the only ones that can truncate, matching the behaviour the fork just shipped for the browser. Requirement 1 is met for every name the device can itself create.

**Selection marker** — `props.selectionMarker = fui::SelectionMarker::Underline` plus `props.rowStyles = fui::plainStyles()`, overriding the theme default. `BaseTheme.h:161` sets `listSelectionStyle = 0 = InvertFill` (`freeink-sdk/.../FreeInkUICore.h:506-511`), which fills the selected row solid black. On a 50 px row at 480 px that flips `2 × 480 × 50 = 48,000` of 418,176 pixels = **11.5% of the panel on every selection move**; a 2 px underline plus a 3 px left bar flips `2 × (480 + 150) = 1,260` px = **0.30%**. A 38× reduction in the FAST‑mode residue that drives ghosting, on the most frequent action in the screen. The SDK explicitly honours a caller‑set marker over the theme's implication (`FreeInkUICore.h:503-505`), so this is a per‑activity prop with **no shared‑code change**.

**Pinned action rows** — the list's first two rows are always:

```
[0]  "Search…"                          (UIIcon::Search)
[1]  "Newest first · All formats"       (UIIcon::Settings) -- current sort and filter
[2…] books
```

Initial selection is row 2. This is the resolution of the discoverability flaw: search is **visible**, not behind an unlabelable long‑press. Note that `MappedInputManager::mapLabels(back, confirm, previous, next)` (`src/MappedInputManager.h:141`) returns exactly four labels for the four front buttons; Up and Down are side buttons with no hint slot anywhere in the firmware, so **no primary feature may live on a side‑button gesture.**

**Buttons — house semantics, unchanged.** `ButtonNavigator::getNextButtons()` = `{Down, Right}` and `getPreviousButtons()` = `{Up, Left}` (`src/util/ButtonNavigator.h:47-53`), and the established list idiom is tap = one item, hold = one page on the *same* buttons (`src/activities/reader/EpubReaderClippingListActivity.cpp:475-489`). We do not rebind Left/Right to paging: that would break the Left≡Up equivalence the user has learned everywhere else.

| Input | Action |
|---|---|
| Next tap (Down / Right) | selection +1, wrapping (`ButtonNavigator::nextIndex`) |
| Next hold (500 ms, repeat 500 ms) | next page (`ButtonNavigator::nextPageIndex(sel, total, visibleRows)`) |
| Previous tap / hold | mirror |
| Confirm tap | open book (`onSelectBook(folderPath + "/" + name)`, `src/activities/Activity.h:83`) / activate action row |
| Confirm hold | per‑book context menu: Open · Show only this folder · Details · Delete |
| Back tap | clear an active search filter if any; else finish to Home |
| Back hold | finish to Home |
| Hints | `mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_PREVIOUS), tr(STR_NEXT))` → `GUI.drawButtonHints` (`BaseTheme.h:238`) |

The **Details** row surfaces provenance: full path, size, format, `"added #<firstSeen>"`, and `"author: from folder name"` / `"from book file"` / `"unknown"`. This is what stops the folder heuristic failing silently.

In **author sort only**, a `ListItem::isHeader` row (`list.h:19`) is inserted whenever the author key changes. That doubles as the position readout for that order and is the letter rail's entire value at zero extra widget cost.

### 10.2 Screen B — `LibrarySearchActivity`

A fork of `KeyboardEntryActivity` under `src/activities/library/`, per the duplicate‑rather‑than‑modify rule.

```
y  78..108   query line, UI_12 (30 px line height), with caret
y 108..133   live match count at SMALL: "17 books"
y 133..489   RESULT ROWS -- 356 px -> 7 rows at 50 px, UNTRUNCATED, same row rules as the list
y 489..729   6x5 constrained key grid, 496 px wide, 82 x 48 px cells
y 752..792   button hints
```

This is where the geometry correction pays. Seven full‑length untruncated results are visible **while typing**. At 100–250 books the entire match set is usually on screen by the second character, and the user opens the book without ever leaving the search screen — no mode switch, no second refresh. Both rival designs surrendered this to landscape arithmetic (3 truncated previews and 2 previews respectively).

**Traversal.** The vertical axis is one ring: `[result 0 … result R−1] → [grid row 0 … grid row 4] → back to result 0`. Up from grid row 0 enters the results bottom‑most first; Up from the top result wraps to grid row 4. Down mirrors. **Left/Right never leave the grid** — inside the grid they are `moveCol`; when focus is in the results they page the result list. This is a true d‑pad screen, which is what `KeyboardEntryActivity` already is (`:199-219`), so the Left≡Up convention is not violated by anything the list screen establishes.

Confirm presses the focused key or opens the focused result. `OK` applies the query as a filter on the list and finishes. Back clears a non‑empty query, else returns to the **unfiltered list** — never to Home.

**Never auto-switch screens** when the match count gets small. An unrequested mode change costs a full panel refresh and disorients; the automotive convention (Seat/Audi speller) is a visible match count plus an always‑reachable List affordance, which is exactly what the `OK` key and the always‑live result rows provide.

### 10.3 Screen C — sort, filter, refresh

A plain `OptionSelectionActivity` (`src/activities/util/OptionSelectionActivity.h:16-18`), reached from list row 1 or a long‑press on Confirm over an action row. Zero new UI code.

```
Refresh library                     <- first item, the correctness guarantee
Sort: Newest first / Oldest first / Title A-Z / Title Z-A / Author A-Z / Author Z-A
Filter by format: All / EPUB / TXT & MD / XTC
Filter by folder: <subtree picker>
Get titles and authors from books   <- the opt-in metadata pass (M4)
```

When header flag b1 (`ranksDegraded`) is set, the author and date options render disabled with an explanatory line. When flag b3 (`booksAtRoot`) is set, author sort is labelled unavailable.

### 10.4 E-ink refresh policy

| Event | Work | Mode | Flipped px |
|---|---|---|---|
| Enter Library | header read + paint from cache | **FAST** | ~100% |
| Selection move | marker only | FAST | 0.30% |
| Page turn | one permutation + record + name read (3–10 ms) | FAST | ~77% |
| Sort / filter change | permutation read or one pass | FAST | ~77% |
| Keystroke / backspace | one pass → count + mask + top 7 | FAST | ~35% |
| Verification walk, no change | none | **none** | 0 |
| Background enrichment | none (row text never changes) | **none** | 0 |
| Ghost budget tripped **and** idle ≥ 1.5 s | full repaint, no state change | **HALF** | 100% |

The cleansing pass reuses the existing user setting rather than inventing a constant: a counter seeded from `SETTINGS.getRefreshFrequency()` (`src/CrossPointSettings.h:621`, default `REFRESH_15`) driven by the `displayWithRefreshCycle` shape at `src/activities/reader/ReaderUtils.h:183-194`. **Only high‑churn refreshes count** — page turns, sort/filter changes, result‑set changes, screen transitions. Selection‑only moves at 0.30% never do. That mirrors KOReader's "only partial counts toward a flashing promotion" rule through the changed‑area proxy X3 forces on us.

**A cleanse never lands between two keystrokes and never on entry.** A ~900 ms flash mid‑typing blows Nielsen's 1 s flow limit — the one HCI boundary this device can actually violate — and on entry it is 92% of a ~1 s transition for no benefit.

Coalescing is free: `requestUpdate()` defers to one notify per main‑loop iteration and the render task's `ulTaskNotifyTake(pdTRUE, …)` collapses bursts into one render (`src/activities/ActivityManager.cpp:196-202`, `:71`). Held buttons repeat at 2 Hz (`ButtonNavigator.h:20`, 500/500), which is already slower than one FAST refresh, so typing can never outrun the panel and **no debounce machinery is needed.**

### 10.5 Press budget — three realistic tasks

Assumptions: N=200, 13 rows per page, two pinned action rows, default sort newest‑first, selection starting on row 2, Home menu positioned so the Library entry is one move from the initial selection. Selection wraps; hold = page.

**Task A — "open the book I just copied onto the card."**

| Step | Presses |
|---|---|
| Home → Library (1 move + Confirm) | 2 |
| Selection is already on the newest book | 0 |
| Confirm | 1 |
| **Total** | **3 presses · 3 refreshes · ≈ 0.6 s of panel time** |

This is the highest‑frequency task in the feature and it is the reason date‑added‑newest is the default.

**Task B — "open a book I can see in the list", 200 books, browsing.**

| Step | Presses |
|---|---|
| Home → Library | 2 |
| 16 pages; mean bidirectional page distance 16/4 | 4 |
| Mean within‑page position, 13 rows, bidirectional | 3.25 |
| Confirm | 1 |
| **Total** | **≈ 10 presses · 10 refreshes · ≈ 1.9 s** |

Compare the same task today via Files: ~12.5 presses **plus a folder descent**, with names capped at two lines.

**Task C — "find the Sanderson one", 1000 books, search.**

| Step | Presses |
|---|---|
| Home → Library | 2 |
| Up ×2 to the Search row, Confirm | 3 |
| Type 3 characters, cursor seeded at the highest‑count letter, with skip: 2.7 + 2.2 + 1.8 travel + 3 Confirms | 10 |
| Up into the results, mean 2 within a 7‑row band, Confirm | 4 |
| **Total** | **≈ 19 presses** |

Browsing the same 1000‑book library: 2 + 19 page presses + 3.25 + 1 = **≈ 25 presses**.

**The honest conclusion, stated plainly: search does not beat browsing below roughly 400–500 books.** At 50–250 books the flat list wins and the Library's value is the untruncated names, the newest‑first default, and the absence of folder descent. Search is justified on **recall** ("the Sanderson one") and the constrained keyboard on **eliminating dead ends** — not on travel. The design says this rather than implying a speedup it does not deliver.

---

## 11. Files and the shared-code footprint

### 11.1 New files (all additive, `src/activities/library/`)

```
LibraryIndex.h / .cpp          format structs, header validation, record/name/permutation readers,
                               the tier-P pin, the folder cache
LibraryBuild.h / .cpp          walk, staging, sort (chunked + external merge), emit, merge-join
LibraryFold.h / .cpp           the fold, the article table, the author-key heuristic
LibrarySearch.h / .cpp         the single pass: match tiers, next-char mask, viewport window
LibraryListActivity.h / .cpp   screen A
LibrarySearchActivity.h / .cpp screen B (fork of KeyboardEntryActivity)
LibraryRowMetrics.h            the row-height helper duplicated from FileBrowserActivity
test/library_fold/             host unit tests (see §12)
test/library_index/
```

### 11.2 Shared-code edits — 10 added lines, all pure additions

1. `src/activities/ActivityManager.h:28` — add `LIBRARY` to `enum class HomeMenuItem`.
2. `src/activities/home/HomeActivity.cpp:52-61` — add `Library` to `enum class HomeMenuAction`.
3. `HomeActivity.cpp:257` — `items.push({tr(STR_LIBRARY), Library, HomeMenuAction::Library});` immediately after `BrowseFiles`.
4. `HomeActivity.cpp:280-282` — the same push in `buildMinimalMenuItems`.
5. `HomeActivity.cpp:1645` region — `case HomeMenuAction::Library: onLibraryOpen(); break;`
6. `HomeActivity.cpp:308-324` — `case HomeMenuItem::LIBRARY: return HomeMenuAction::Library;`
7. One `#include` and a 4‑line `onLibraryOpen()` helper.

`UIIcon::Library` already exists and already has 24/32 px assets (used at `HomeActivity.cpp:261` for the OPDS browser), so there is **no new asset and no icon flash cost**. The visual collision with OPDS Browser is accepted: that entry only appears when OPDS servers are configured, and the labels differ.

### 11.3 Flash

~24 new `tr(STR_*)` strings ≈ 720 B of English blob plus 2 B × 28 languages × 24 ≈ 1.3 KB of offsets ≈ **2 KB**. The fold table is ~200 B. Two activities plus the index and build code, at roughly 2,200 lines, is an estimated 35–55 KB of `.text`. Against 291,168 B of headroom (commit `5f88bef3` measured 6,262,880 B against the 6,553,600 B slot) this fits with room, **but `pio run -e default` must be measured before and after at every milestone**, not at the end. Historical precedent (v1.3.3 shipped 43,744 B under the limit) shows the only lever when it does not fit is deleting fonts, which is a product decision outside this feature.

---

## 12. Phased build order

**M1 — the shelf. Ships alone and is worth shipping alone.**
Index format, walk, sort, emit, tmp+rename install, header validation, optimistic open, verification walk in `loop()` slices, `LibraryListActivity` with untruncated rows, the underline selection marker, the position readout, date‑added‑newest default, Title A‑Z / Z‑A, the Refresh action, the Details context row, the three‑tier heap degrade, and the Home entry point.
*Value alone:* one flat, fully legible, newest‑first list of every book on the card, with no folder descent and no truncation. That already beats the file browser for the primary task, at 3 presses.
*No search, no keyboard, no author sort, no enrichment.*

**M2 — search.**
`LibrarySearchActivity`, the single pass, tiered matching, the next‑char mask, `fui::keyGrid` with the skip traversal, the seven untruncated live result rows, the up‑ring traversal, the match count.

**M3 — sorts and filters.**
Author and date permutation arrays exercised, author group headers, format filter, folder subtree filter, `OptionSelectionActivity` menu, the `ranksDegraded` and `booksAtRoot` degrades.

**M4 — metadata enrichment.**
Opt‑in bulk pass with the `/.crosspoint` listing trick, the free ride on book open, provenance surfacing, resumability via `enrichCursor`, cancel and heap gating.

**M5 — conditional performance work.** Only if §13 measurements demand it: async overlap on the keystroke path, the refine pin at `matchCount ≤ 96`.

**Out of this spec entirely:** the cover‑thumbnail grid (a separate M‑number, tracked separately).

---

## 13. Measurements that would falsify this design

Each row gives the measurement, the threshold, and the action. **Take measurements 1–3 before writing M2.**

| # | Measure | How | Threshold | Action if crossed |
|---|---|---|---|---|
| 1 | **Render CPU** for one list page and one keyboard frame | `millis()` around the draw block in `render()` | > 60 ms | This is the only unmeasured term in §7.4 and it is 15–40× larger than the search. If it exceeds 60 ms, snapshot the static key grid once with `GfxRenderer::readFramebufferRegion` / `writeFramebufferRegion` (`GfxRenderer.h:262-263`, already used for the Home cover tile) and repaint only changed key cells. This cuts render CPU without touching the 185 ms panel cost. **Do not pre‑optimise; measure first.** |
| 2 | **SD sequential throughput** | time a read of a known‑size file at 4096 B aligned; `LOG_INF` the achieved KB/s | < 400 KB/s | Every I/O figure here assumes 400–1500 KB/s. Below 400 KB/s, a Tier‑S pass at N=1000 exceeds 320 ms. Raise `LIBRARY_PIN_MAX_BYTES` if heap allows, and promote M5's refine pin from conditional to required. |
| 3 | **Tier‑S pass wall time at N=1000** | timestamp the pass with a synthetic 1000‑book card | > 400 ms | Ship M5 (async overlap + refine pin) as part of M2 rather than later. |
| 4 | **Verification walk duration** at the user's real corpus | `lastWalkMs`, logged | > 600 ms | Already handled by the §5.5 policy, but if it exceeds 600 ms at **100 books** the walk model is wrong by 2.5×; re‑derive it and consider per‑folder signatures. |
| 5 | **Real filename length distribution** on the user's card | one‑off script over the SD contents: mean, p90, max byte length; and folded length after §4.1 | p90 > 96 folded bytes, **or** max > 180 display characters | The 96‑byte fold cap would be truncating real search text — widen the record to 160 B stride (still divides 4096 as 25.6… so use 256 B stride, 16 records/chunk) and re‑derive §3.7. If display names exceed 180 chars, raise the row cap to 5 lines and accept 6 rows/page. |
| 6 | **Author‑from‑folder hit rate** | fraction of books whose parent folder yields a plausible surname | < 60% | The Tier‑A heuristic is wrong for this user's layout. Promote M4 ahead of M3 and label author sort unavailable until the metadata pass has run. |
| 7 | **Heap at Library open**, pinned and streamed | `ESP.getFreeHeap()` / `getMaxAllocHeap()` logged in `onEnter()` and `onExit()` | free after open < 30 KB, or any leak across enter/exit | Reduce `LIBRARY_PIN_MAX_BYTES`; if it leaks, the Arena is not being released in `onExit()`. |
| 8 | **Build wall time** at N=200 / 1000 / 2000 | timestamp the cold build | > 2 s at N=200, > 8 s at N=2000 | The 2N staging seeks dominate. Switch the name blob to walk order (halving the seeks) and accept scattered page reads, or raise the staging slot size so adjacent records share sectors. |
| 9 | **Constraint strength** — mean live keys at depth 1, 2, 3 on the real corpus | instrument the mask, log `popcount` | > 15 live keys at depth 2 | The union‑over‑tiers mask is too permissive at this corpus. Add a prefix‑and‑word‑initial‑only toggle to the menu (never make it the default silently, and never let the mask disable a key leading to a displayed result). |
| 10 | **Flash delta** | `pio run -e default` before and after each milestone; `scripts/check_firmware_size.py` prints remaining bytes | remaining < 100,000 B | Stop adding strings and code; the only remaining lever is deleting fonts, which is a product decision outside this feature. |

**Host unit tests required before M1 merges** (these cover the paths that will otherwise be broken when they finally run):

- `fold()` round‑trip: accents, NFC decomposition, article stripping in every listed language, separator collapse, the 96‑byte truncation boundary.
- `lastNameLowerForKey`: the `';'`‑first split, the comma form, trailing punctuation, embedded tabs.
- Sort key ordering: `memcmp` over 12 bytes then `seq`; assert no two keys ever compare equal.
- External merge at N = 2000 synthetic records, 512‑key chunks, 4 runs, 2 passes — the code that never runs in the field at the real corpus size.
- `nextFirstSeen` overflow and renumber, seeded at 0xFFFE.
- Header validation rejects: wrong magic, wrong `formatVersion`, wrong `foldVersion`, `selfSize` mismatch, truncated file.
- Merge‑join reconciliation: add, remove, rename, resize, move; assert `firstSeen` preservation and invalidation semantics.
- The traversal reachability property: for 200 random `enabled` masks, BFS from every live cell and assert every other live cell is reachable in ≤ 9 presses.

---

## 14. Risks accepted, with reasoning

**The `(name, size)` signature has a blind spot.** A file replaced by a *different* file of identical name **and** identical byte size in the same folder is invisible to `scanSignature`, so the index keeps the old `firstSeen` and author key. No mtime can close the gap (`HalFile` exposes no timestamp accessor — its complete API is `lib/hal/HalStorage.h:79-101`) and content hashing means reading the whole card. *Accepted:* probability is very low, consequence is cosmetic (wrong date‑added position), and "Refresh library" is the documented guarantee, placed **first** in the menu rather than buried.

**`BookMetadataCache::load()` deletes `book.bin` on magic, version or truncation mismatch** (`lib/Epub/Epub/BookMetadataCache.cpp:504, :510, :518, :525, :537`). The M4 enrichment pass therefore performs a cross‑subsystem destructive write from a screen the user thinks is read‑only: after a firmware downgrade, merely indexing would wipe reader caches. *Accepted, with three mitigations:* the pass is opt‑in behind an explicit menu action; it is gated on `Epub::hasCache()` first; and the reader rebuilds those caches on next open, which is exactly what it would do anyway. The alternative — hand‑rolling a `book.bin` reader — would duplicate `BOOK_CACHE_VERSION`, a private `.cpp` constant (`BookMetadataCache.cpp:15`) that can drift silently. That is a worse bug.

**The fold is Latin‑only, and the damage is baked in at build time.** Any codepoint outside U+00C0..U+017F that is not `[a-z0-9]` folds to a space boundary, so CJK, Cyrillic, Greek and Hebrew filenames are unsearchable by their native title and sort as empty. They remain browsable, sortable by date, and searchable by any Latin words in their path. *Accepted:* the stated corpus is Latin script and the keyboard is Latin‑only, so there would be no way to type the query. Recorded here so a future fix knows it must bump `foldVersion` and rebuild.

**Author‑from‑folder is only as good as the user's folder layout.** Flat `/Books`, genre‑first and series‑per‑folder trees all break it. *Accepted:* unknown authors sort last in both directions so a bad key never heads the list, the `booksAtRoot` flag detects the flat case and says so, and M4 exists to repair it. Budget a "rebuild author keys" action from day one — the heuristic *will* change.

**The 4‑line row cap can truncate names above ~180 characters.** *Accepted:* that is above the firmware's own 150‑byte web‑upload limit, and it matches the behaviour the fork shipped for the file browser one commit ago. Consistency with an existing, deliberate product decision beats a special case.

**Depth‑0 is unconstrained.** Every letter a–z appears somewhere at any realistic corpus size, so all 26 keys are live on the first keystroke and the "GPS keyboard" feel arrives at depth 2–3, not depth 1. Further, the constraint estimate is a bound on a *mean*: a user typing a very common bigram will see far more live keys than average. *Accepted*, with the three safety nets in §9.2 and measurement 9 in §13.

**M5's async overlap must never be depended upon.** `displayBufferAsync` is silently disabled when the user's sunlight fading fix is on, and it has exactly one other call site in the entire firmware. Every latency budget above is quoted **without** it.

---

## 15. Explicitly not in scope

- **Cover thumbnails and the grid view.** Separate milestone, separate spec.
- **Collections, shelves, tags, reading progress columns, star ratings, "finished" state.** The index knows what the filesystem knows plus a derived author key; filters are limited to that, per requirement 5.
- **Series parsing and natural (numeric‑aware) volume ordering.** `FsHelpers::naturalSortKey` is deliberately not used (§4.2). "Mistborn 2" sorts before "Mistborn 10". Revisit only with a measured complaint.
- **Non‑Latin search, CJK/Cyrillic folding, collation, ICU.**
- **Fuzzy / edit‑distance matching.** `Dictionary::editDistance` exists and could be copied, but at these corpus sizes the constrained keyboard makes zero‑result queries unreachable, which is what fuzzy matching is for.
- **Any modification to `lib/FileIndex`.** It is structurally per‑directory for five verified reasons (bare names with no path or parent at `FileIndex.cpp:273-282`; `key[0]` hardcoding dirs‑before‑files at `:169-173`; a filename derived from one directory path at `:137`; global merge scratch files at `:12-20`; a double walk that hard‑fails on disagreement at `:292`). We write a sibling and keep its shape.
- **Any SDK modification**, including implementing `Uc8253X3Driver::displayWindow`. It would need three edits across shared code and, because Portrait maps a screen‑horizontal band onto the full 528‑gate range, would save ~15 ms of the ~26 ms SPI payload and none of the ~133 ms waveform.
- **Touch.** X3 has none. `fui::keyGrid` and `fui::list` give X4 touch routing for free; nothing beyond that is designed or tested.
- **Networked or cloud library sources.** OPDS remains a separate feature with a separate entry point.
- **Multi‑card, multi‑volume, or non‑SD sources.**
- **Libraries above `LIBRARY_MAX_RECORDS` (4096).** Above that the build refuses, sets no index, and the Library shows a message pointing at Browse Files. `u16` ranks and `u16 firstSeen` are the binding constraint and 4096 is 2× the stated headroom target.