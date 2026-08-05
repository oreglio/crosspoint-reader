# Memory and flash figures for the upstream PR

Measured on 2026-08-05 at commit 038df0f1. The upstream template asks for these
and the project's current focus is exactly this, so they should be quoted with
their method rather than as bare numbers.

## Flash

**49 KB of linked code and data**, from `riscv32-esp-elf-size` on the feature's
own objects:

| Object | Bytes |
|---|---|
| LibraryListActivity | 15,726 |
| LibraryText | 14,142 |
| LibraryBuilder | 13,038 |
| LibraryMeta | 4,383 |
| LibraryIndexFile | 3,156 |
| LibraryFormat | 272 |
| **Total** | **50,717** |

The whole image grew 41 KB against upstream v1.5.0-rc-3 (6,304,384 vs 6,262,432),
which is LESS than 49 KB because the feature reuses symbols already linked in. That
41 KB also contains the macOS CssParser fix and the file browser's Full Name mode,
both of which are being split into their own PRs — so quote 49 KB as the feature's
cost and the 41 KB only if the split is explained.

App slot is 6,553,600 bytes, leaving 250 KB (3.8%) free after the feature.

## RAM while reading — ~440 bytes, independent of library size

This is the number that matters, and it is a property of the format rather than a
measurement: the reader never holds the library.

| | Bytes |
|---|---|
| Cached header | 64 |
| One record | 128 |
| One row's strings (title + author, freed at the next row) | ~250 |

Records are a fixed 128-byte stride, so reaching record *n* is a seek, not a scan
of an offset table, and a screenful is one 4 KB block.

## RAM while building — 22 KB, capped

Bounded by `LIBRARY_MAX_SORTED = 512`, not by the number of books:

| | Bytes |
|---|---|
| Title order (u16 × cap) | 1,024 |
| Author SortKey (key 16 + ordinal 2) | 9,216 |
| Date SortKey | 9,216 |
| canonicalFrom | 1,024 |
| Resolved firstSeen | 1,024 |
| Staging buffers (heap since 46d452df) | 1,536 |
| **Total** | **23,040** |

Past 512 books the permutations degrade, `CLIX_FLAG_RANKS_DEGRADED` is set and the
shelf says "Library (unsorted)" rather than passing walk order off as an alphabet.
That is what bounds the peak at any library size.

Measured on real hardware (X3, 58 books, metadata on): **min heap 35,304 bytes,
maxAlloc 86,004** — the extra over 22 KB is the OPF reader's 8 KB buffer plus the
reader's own working set.

## Index on disk

**Measured: 438,831 bytes (428 KB) for 2,060 books**, built in 461 ms of walk time,
0 duplicates dropped, 0 unreadable. Generated library, filenames averaging ~48
characters.

Size is dominated by filename length, so it must be quoted with that assumption.
On the maintainer's real card the median filename is 148 characters against 23 for
the title, which projects to roughly **630 KB at 2,000 books** — about 60% of the
index being filenames.

Layout at 2,000 books: header and folders ~1 KB, records 256,000 (2,000 × 128),
permutations 8,000 (two u16 arrays), the rest the name blob. Since format v3 the
blob holds the filename AND the title — 24 bytes per book more — because
`readPath` rebuilds a book's path from the name slot, and putting the display title
there made enriched books impossible to open.

## Method notes for anyone re-measuring

- Generate a stress library into `fs_/` and open the shelf in the simulator; the
  build logs `built: N books, F folders, D dup dropped, U unreadable, Xms`.
- The simulator reports a fake constant heap (1048576), so RAM figures must come
  from the device or from the capped array arithmetic above.
- Rebuild from Settings > System > Library after any format bump: an index from an
  older version fails validation and is rebuilt, but one written by an
  intermediate v3 build passes validation while being wrong.
