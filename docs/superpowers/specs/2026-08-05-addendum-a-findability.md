I have everything I need. Here is the addendum.

---

# ADDENDUM A — Findability: the Two-Slot Shelf

**Addendum to `docs/superpowers/specs/2026-08-05-library-index-search-ux-design.md` (v1.0).**
Status: decided, implementable. Supersedes §4.1 step 7, §4.3, §5.6, §10.1 rows/headers, §12 milestone order, and the fold width in §3.3. Everything else in v1.0 stands unchanged.

Measured against the user's real card (`/Volumes/NO NAME`, read-only throughout). Scripts: `/private/tmp/claude-502/-Users-aurelien-edusign-Code-xteink-x3/86ab4b99-44a6-426e-ab63-28e21fb35e2a/scratchpad/final/{rule.py,measure.py,merge.py,press.py}`.

---

## A0. Verdict — spine, grafts, drops

**The spine is Two Rows**: every book is a two-slot row — `fui::ListItem::label` = title, `fui::ListItem::subtitle` = author (`freeink-sdk/libs/ui/FreeInkUI/include/components/lists/list.h:9`, `:10`) — with a filename-derived fallback at t=0 and EPUB metadata replacing the text in place later. It wins because it is the only candidate that puts the author at a **fixed column on every row**. The user's sentence is "I cannot find my books because I do not know all the authors." The fix for that is a column of author names they can sweep, not an author buried at a different offset on each row. Measured: author at line 2 / column 0 on 69 of 69 rows, against a single em-dash-joined label where the author starts at column 0–41 (median 18) and lands on line 1 for 38 rows and line 2 for 24.

**Grafted from Shelf Labels:**

1. **Unconditional drop of `" -- "` segments 2 and beyond.** No classify-then-strip ladder, no publisher/place/year/ISBN regexes. Position gating gets the same length win with zero content classifiers and a strictly better worst case. The ladder's dangerous rule false-hits 24 of 59 real authors, because `"Michaelides, Alex"` is structurally identical to `"Paris, France"`; and one unrecognised tail segment blocks the whole strip to its left, which is what leaves `"The Gentile Times Reconsidered … -- 86bb"` at 143 characters.
2. **The structural depth ≥ 2 gate** on folder-as-author, replacing every hardcoded genre allow-list.

**Grafted from The Shelf:**

3. **Sorted-token author key** (order-insensitive, initials dropped, 12 bytes) plus **modal-spelling canonicalisation** of the displayed author within each key group.

**Grafted from the misfire judge:**

4. **No build-time truncation of the title, ever.** Both rival designs cut the title at a subtitle boundary; both collapse series. On a 10-title series control (Narnia ×4, Knuth ×3, Wheel of Time ×2, Culture ×1) each produced 8 identical rows out of 10. The pixel-accurate renderer already does this correctly and at every theme and UI scale: with a subtitle present the label band is exactly one line high (`list.h:271-279`), `maxLines == 1` routes to `renderer.truncatedText` (`FreeInkUIGfxRenderer.h:166-169` → `GfxRenderer.h:287`, `GfxRenderer.cpp:2387`), which cuts on **measured width**, not on a character count baked into the index.
5. **A series-folder corroboration guard** on the folder-as-author fallback.
6. **`More by <author>` keyed on the author key, not a substring of the display string.**

**Dropped:**

- **The subtitle cut / `HEAD()` rule.** See graft 4.
- **Per-author section headers in author sort** (spec §10.1). 40 author keys over 69 books, 28 of them singletons — one header per 1.7 books. Measured: mean 9.2 → 10.3 presses, p90 14 → 16. A measured regression, not a judgement call.
- **A genre filter screen and an author browse screen.** Both measured worse than the unfiltered flat list on this corpus.
- **Series as any browse or filter dimension.** 7 of 64 books carry it, two of the seven values are junk (`"Litterature #7198.0"`, `"Novels #5"`).
- **A digit page on the keyboard.** 0 of 69 books are unreachable by letters alone.
- **`opf:file-as` as a canonicalisation source.** It yields *more* distinct author keys than the plain text (40 vs 39) and is a byte-identical duplicate in 18 of the 58 books that carry it.

---

## A1. What this corpus proves the v1.0 spec got wrong

| Spec | What it says | What the corpus measures | Correction |
|---|---|---|---|
| §4.3 | Author source priority: **(a) parent folder basename first** | 26 of 69 books sit directly in a genre folder, so folder-first prints `"Romans"` / `"Study"` as the author for 35% of the library. Its own safety flag — ">60% of records share one `folderId`" — does **not** trip: the largest top folder is Romans at 52% | Invert the priority (OPF → filename segment 1 → folder). Gate the folder source on **path depth ≥ 2 AND sibling corroboration** (§A2 step 6). Replace the >60% guard with the depth test |
| §4.3 | Key rule: "take the part before `','`, else the token after the last space" | Wrong on 8 of 22 author folders, affecting 25 of the 49 foldered books: `"Giebel Karine"` → `karine`, `"Qiu Xiaolong"` → `xiaolong`, `"Lee Min Jin"` → `jin`. Raw `dc:creator` splits Qiu Xiaolong's 9 books into two keys, 6 and 5 | Sorted-token key (§A2 step 7). Takes 45 raw spellings to 40 keys and merges Qiu, Manook and Lennox |
| §4.1 step 2 | `utf8ComposeNfc` **then** table-map | The card contains **zero** precomposed accents. An NFC-first or `ascii-ignore` fold therefore passes on this card by accident and breaks the instant one NFC file arrives from Windows or Calibre (`pandémie` → `pandmie`). U+00F8 has no canonical decomposition at all and yields `Nesb` | Normalise to **NFD first**, strip combining marks, then apply an explicit expansion map. `’ → '` is load-bearing: without it the `"Anna's Archive"` test misses all 54 files that carry U+2019 |
| §4.1 step 7 + §3.3 | Fold the filename, cap 96 bytes | 50 of 75 records hit the cap — the fold is *truncated* for two-thirds of the library — and ~6% of its tokens are md5/ISBN noise a 3-char query can match. 60% of the raw index is text no row ever shows | Fold the **displayed text ∪ filename words**, cap 90 bytes with the author key right-anchored. Measured: median 57, p90 90, max 90; 9 of 69 at the cap; **0 noise tokens**; 0 records lose the author key |
| §5.6 | `LIBRARY_OPF_MAX_BYTES = 16384`, `readFileToMemory` | Rejects 5 of 64 books (inflated OPFs of 19,689 / 17,296 / 17,296 / 26,432 / **44,531** B) and its largest allocation is the full inflated size | Gate on `compressedSize` from the central directory (`lib/ZipFile/ZipFile.h:40`, `:57`) — median 1,968 B, max 3,681 B — and inflate a **bounded one-shot prefix** (§A5) |
| §10.1 | `libraryLineCount` from `longestName[3]` probes; 4-line rows | The three longest names are always ≥ 4 lines on this card, so the probe always resolves to 4 lines / 7 rows per page. Every geometry claim downstream of "13 rows" is void | Row type is **fixed** `WithSubtitle`; the probe and `longestName[3]` are deleted. Rows/page is derived at runtime from `uiListRowHeight` |
| §10.1 | Header row per author key in author sort | +1.1 presses mean, +2 on p90 | Drop |
| §12 | Enrichment is M4, opt-in, behind a menu | 2 books have no author anywhere in the filename; those are exactly the user's complaint. The pass costs 2.6–5.6 s total | Enrichment becomes **M2**, automatic, page-first, resumable, one coalesced repaint (§A5) |
| §5.4 | Reconcile on `(folderId path, name)` | The card holds **6 duplicate FAT directory entries** resolving to the same inode; `ls -bi` prints inode 6268 twice for `"Amour, meurtre et pandémie"`. `readdir` emits each twice, so the flat list shows 6 phantom books | Dedup on `(folderId, name, fileSize)` during the merge-join. 75 dirents → **69 distinct** |
| §5.1 | Accept via `isSupportedBrowserFile`-equivalent logic | The 2 PDFs on the card match no accept predicate — there is no `hasPdfExtension` anywhere and `isSupportedBrowserFile` (`src/activities/home/FileBrowserActivity.cpp:137`) has no PDF branch | Exclude PDFs explicitly, so the header count never advertises a book the firmware cannot open. **69 distinct → 67 indexed** |

Two things the spec got **right** and that this corpus confirms as load-bearing: the **provenance-3 "tried and failed" sentinel** (one EPUB, present twice, lists 92 entries and reads `container.xml` fine but throws `invalid distance too far back` on 28 of 92 deflate streams including the OPF), and the honest statement that **search does not beat browsing at this scale**.

---

## A2. The display-name rule — final and exact

Runs **once per book at index-build time**, inside `LibraryBuild`'s staging step (§5.2), into a 256-byte scratch buffer beside the 512-byte dirent name buffer §5.1 already allocates. Its output feeds the display blob **and** the fold. Zero resident RAM, zero cost on any interactive path, no regex engine — six hand-rolled byte scanners. **On by default with no setting** (justification in §A2.9).

Input: absolute path, plus `(dc:title, dc:creator[])` when enrichment has run for that record.
Output: `(title, author, authorKey, titleProvenance, authorProvenance)`.

### A2.1 — The fold (correction to §4.1)

```
fold(s):
  1. for each codepoint:
       explicit map first:  ø→o Ø→O æ→ae Œ/œ→oe ß→ss ł→l ð→d þ→th ’‘→' “”→" –—→-
       else NFD-decompose and DROP combining marks (U+0300..U+036F)
       else if ASCII alnum -> lowercase
       else -> one space
  2. collapse space runs, trim
  3. (sort/search only) strip a leading article, table unchanged from §4.1 step 6
```

`lib/Utf8/Utf8.h` composes rather than decomposes, so `LibraryFold.cpp` owns its own decompose-and-strip. Host-unit-tested against **both** NFC and NFD inputs — a test against this card alone is a false pass.

### A2.2 — Filename parse (always runs; it is the t=0 row *and* the fold's union source)

```
stem  = basename minus .epub/.txt/.md/.xtc   (FsHelpers.h:52-67)
segs  = split(stem, " -- ")                   // 4 ASCII bytes; never " – ", never " — "
```

If `len(segs) >= 2`: `fnTitle = segs[0]`, `fnAuthor = segs[1]`, and **segments 2..n are discarded unread**. This is safe here and the evidence is measured: segment counts on this card are 1, 3, 4, 5, 6, 7 — never 2 — and **no title contains `" -- "`**. Because segments 0 and 1 are never classified, the rule cannot mangle a title or an author.

One guard on `segs[1]`, the only content classifier in the whole rule, applied only at index 1:

```
NOT_AUTHOR = ^( [0-9a-f]{4,32}
              | isbn1[03]\b.*
              | 97[89][0-9]{10}
              | [0-9]{9}[0-9xX]
              | (1[5-9]|20)[0-9]{2}
              | .{0,80}?[,;#]\s*(DL\s*)?(1[5-9]|20)[0-9]{2}
              | anna'?s? archive ) \s*-?\s*$          // matched against fold(segs[1])
```
Measured: 0 false hits on 69 titles and 59 authors. The trailing `\s*-?\s*$` tolerance is required — both PDFs end in `"Paris, 1985 -"`.

### A2.3 — Ad-hoc shapes (the 10 files with no `" -- "`)

Tried in order, each validated against the parent folder's author key so a wrong split cannot happen silently:

- `" - "` split: strip a trailing volume digit run from each side; if `authorKey(right) == authorKey(parent)` the right side is the author, else if `authorKey(left) == authorKey(parent)` the left side is. This is what rescues `"Kazuo Ishiguro - Linconsole"`, `"Amour, meurtre et pandémie - Xiaolong Qiu"` and `"Inspecteur Harry Hole T13 _ Éclipse totale - Jo Nesbø 2"`.
- Dot-separated (≥ 2 dots, no `" -- "`): last part is the author if its key matches the folder's. Rescues `"Le juge Ti.T2.Le juge Ti et le poète révolutionnaire.Xiaolong Qiu"`.
- Lowercase web slug (no space, has `-`, no uppercase): `-` → space, no author. `"normal-people"`, `"un-diner-chez-min"`.
- Otherwise the whole stem is the title and there is no author.

### A2.4 — Underscore restoration (47 underscores across 32 files, 95% correct)

```
s/(?<=[A-Za-z0-9]) _(?= )/ :/          "Dark Matter _ A Novel"     -> " : "
s/(?<=[ ][A-Z])_(?=\s|$)/./            "Douglas R_ Hofstadter"     -> "R."
s/(?<=[a-z0-9])_(?= )/:/               "The Mom Test_ how to talk" -> "Test: how"
then rstrip("_ ")                      kills the "Qiu Xiaolong_" misfire
```

**The initial rule requires a preceding *space* plus capital, never start-of-string.** Without that lookbehind, `A_Brief_History_of_Time.epub` renders as `A.Brief_History_of_Time` — a full stop inserted mid-title with the other underscores left intact, which is strictly uglier than the raw name. The underscore-as-space convention is common in downloaded ebooks and this misfire is the reason the rule is written this way.

### A2.5 — Title cleanup

`restore_underscores` → drop a whole parenthesised `(French|English|German|Spanish|Italian|Dutch) Edition` span → collapse spaces → trim `" ;,-\t"`. Nothing else. **No length cap, no subtitle cut, no ellipsis.** The renderer decides what fits, in pixels, at the shipped theme and UI scale.

The `\bedition\b` word must never be substring-matched: a publisher word-list containing it false-hits 7 real titles via `"(French Edition)"`.

### A2.6 — Person cleanup (author strings only, never the title)

Drop one `\[[^\[\]]{2,60}\]` span (8 hits, all pure duplication: `"Karine Giebel [Giebel, Karine]"`) → drop a trailing MARC relator `\((aut|edt|trl|ill|nrt|com)\)` → drop `\s*&\s*\S+\.(com|net|org)\S*` (kills the `"Jiang, Jia & chenjin5.com"` spam) → take the part before the first `';'` → `restore_underscores` → collapse spaces → strip trailing `" ;,.\t"`.

For `dc:creator` specifically: **dedupe identical creators first** (5 of the 6 multi-creator books repeat the same name, e.g. `['Xiaolong Qiu','Xiaolong Qiu']`), then take `creator[0]`. If more than one *distinct* creator survives, render `"First Author +N"`.

### A2.7 — Author source, in priority order

| Prov | Source | Count (post-enrich, n=69) |
|---|---|---|
| 2 | cleaned `dc:creator[0]` | 59 |
| 1 | `fnAuthor` from segment 1 or the ad-hoc split | 10 |
| 0 | parent folder basename, gated (below) | 0 |
| 3 | none — row shows `tr(STR_LIBRARY_AUTHOR_UNKNOWN)` | 0 |

Pre-enrichment the same table reads: filename 63, folder 4, none 2.

**Folder gate — structural, no word lists.** Accept the parent folder basename `P` as this book's author only if **both**:

- **(G1) `depth(book) >= 2`** — `P` is not a top-level directory of the card. One integer comparison. This is what stops `"Romans"` / `"Study"` / `"read"` becoming authors for the 26 books that sit directly in a genre folder, and it is correct on any card: `./Books/Dune.epub` and `./Fiction/1984.epub` get no folder author, where a hardcoded `{romans, scifi, litt, study, read}` allow-list would print `"Books"` and `"Fiction"`.
- **(G2) sibling corroboration** — at least one *other* book under the same parent has an author from provenance 1 or 2 whose sorted-token key equals `authorKey(P)`. This is the only version of the Yeruldelgger fix that works when the folder name appears nowhere in the filenames: it suppresses `./SciFi/Expanse/01 - Leviathan Wakes.epub` → `"Expanse"` while preserving `./Fiction/Mystery/Agatha Christie/Murder on the Orient Express.epub`.

The failure direction is showing *less*, never showing something false, and Details always shows the full path.

### A2.8 — Author key and canonical display spelling

```
authorKey(s) = drop [..] spans -> part before ';' -> fold -> drop 1-char tokens (initials)
               -> SORT tokens -> join ' ' -> truncate 12 bytes
```

Order-insensitive by construction. Measured: 40 keys over 69 books; merges Qiu Xiaolong's 9 books across 6 spellings (`"Qiu, Xiaolong"`, `"Xiaolong, Qiu"`, `"Qiu Xiaolong_"`, `"Qiu Xiaolong [Xiaolong, Qiu]"`, …), plus Manook/Ian Manook and Lennox/John C Lennox. Truncation to 12 bytes causes 0 collisions.

**Canonical display spelling.** Within each key group, line 2 for every member is the **modal** spelling that actually occurs in that group; ties broken by shortest, then lexicographic. It never invents or reorders a name — it picks one of the strings that exist. This is the whole of the name-order handling; there is no First/Last heuristic anywhere, which is what keeps Qiu Xiaolong and Lee Min Jin safe. Measured: rewrites 27 of 69 rows post-enrichment (`"Crouch, Blake"` → `"Blake Crouch"`), so an author-sorted list does not stutter between two forms of one person.

### A2.9 — `TITLE_MERGE`, and why the title is not simply `dc:title`

`dc:title` is thinner than the filename's title segment for 14 books. Prefer the filename segment when **all** of:

1. `fold(dc:title)` is a prefix of `fold(fnTitle)` ending at a word boundary,
2. `fnTitle` adds ≥ 2 further tokens of ≥ 3 letters,
3. the last token of `fnTitle` is not in a 34-entry stop-tail set (`a an the of to for at if in on and or with from your is as by de du des la le les un une et pour dans sur …`).

Measured: **fires on 4, all gains, 0 regressions.**

```
DC '2084'                                -> '2084 : Artificial Intelligence and the Future of Humanity'
DC 'Love and Murder in the Time of Covid'-> 'Love and Murder in the Time of Covid (Inspector Chen Cao'
DC 'Cosmic Chemistry'                    -> 'Cosmic Chemistry: Do God and Science Mix?'
DC "God's Monsters"                      -> "GOD'S MONSTERS : vengeful spirits, deadly angels, hybrid"
```

Rule 3 is what earns it. It **blocks** the 4 cases where the filename is the exporter's mid-phrase truncation — `"Galileo's error : foundations for a new science of"`, `"Mere Christianity: a revised and amplified edition, with a"` — and correctly blocks `"Dark Matter : A Novel"`. It costs one marginal loss: `"L'Oiseau bleu d'Erzeroum - tome 1"` keeps `dc:title` and drops `"tome 1"`, because that adds only one token of ≥ 3 letters. The union fold covers it (`tome` → 3 hits).

The first line of this rule is the user's own named target. Without it the row for "the one about AI" reads `2084` and the words *artificial*, *intelligence*, *future*, *humanity* leave the search index entirely.

### A2.10 — The disambiguator

If two records render an identical `(line1, line2)` pair, append `" · <top-level folder>"` to line 2 for every member of the collision set. Measured: 4 colliding rows → 2, and the 2 survivors are the genuine typo-duplicate pair (`"Pachinko -- Lee, Min Jin"` and `"Min Lee Jin-Pachonko.epub"` — same book, same folder), which no rule can separate and which the user must resolve. Longest line 2 after the suffix: 30 characters. 16 rows gain the suffix.

### A2.11 — The fold, restated as an invariant

> **The searchable text must be a superset of the visible text. Never a subset.**

```
fold = fold(displayTitle, article-stripped)
     ∪ words of the filename's title segment not already present
     ∪ words of the filename's author segment not already present
     ∪ words of dc:title not already present
     + ' ' + authorKey, RIGHT-ANCHORED so a 90-byte truncation eats the title tail, never the key
```

Measured: median 57 B, p90 90, max 90; **9 of 69 at the cap, 0 losing the author key, 0 md5/ISBN noise tokens.** A metadata-only fold makes 18 of 59 enriched books unsearchable by words their filenames carried — including `enquete`/`inspecteur`/`chen`, `chroniques`/`radch`, `foundations`/`science`, `god`/`science`/`mix`. The union costs ~3 bytes of median fold length and recovers all of them.

### A2.12 — Measured effect over all 75 real filenames

**Judgement criterion**, task-grounded, not cosmetic: *worse* = the row as rendered is not uniquely identifying among all books, or line 1 ellipsises with no author line to compensate. *Improved* = uniquely identifying **and** (today's row needs > 2 wrapped lines, or gains an author not legible in today's first 180 characters, or recovers an exporter-truncated title).

```
n = 69 distinct books (75 dirents − 6 duplicate FAT entries)

IMPROVED   63
UNCHANGED   4
WORSE       2   <- both are the Pachinko duplicate pair, same book, same folder
```

| | today (raw filename, 4-line cap) | Two-Slot Shelf |
|---|---|---|
| Wrapped text lines to sweep | **239** | **138** (69 titles + 69 authors) |
| Characters to sweep | 9,551 | **3,245** (2.9× less) |
| Books ellipsised | 32 / 69 | line 1: 18 / 69 · line 2: **0 / 69** |
| Row height / rows per page | 90 px / 7 | 50 px / **13** |
| Pages for the whole library | 10 | **6** |
| Rows carrying a legible author | ~37 | **69 / 69** (67/69 before enrichment) |

Line 1: median 26 / p90 68 / max 112 characters. Line 2: median 12 / max 30.

**The three worst outputs, verbatim, post-enrichment** (each is a long title the renderer ellipsises — none is a mangling, and every one has its author on line 2):

```
FILE : Combating Cult Mind Control_ The #1 Best-Selling Guide to -- Steven Hassan -- 2015 -- …
ROW1 : Combating Cult Mind Control: The #1 Best-Selling Guide to Protection, Rescue and Recovery…
ROW2 : Steven Hassan

FILE : The Mom Test_ how to talk to customers and learn if your -- Rob Fitzpatrick -- 2013 -- …
ROW1 : The Mom Test: how to talk to customers and learn if your business is a good idea when…
ROW2 : Rob Fitzpatrick

FILE : Rejection Proof_ How I Beat Fear and Became Invincible -- Jiang, Jia -- 2015 -- cj5_2679 -- …
ROW1 : Rejection Proof: How I Beat Fear and Became Invincible Through 100 Days of Rejection
ROW2 : Jia Jiang
```

**The two worst rows *before* enrichment** — what the user actually sees on first paint:

```
FILE : 1 - Pines-_The-Wayward-Pines-Trilogy_-Blake-Crouch-2012-AmazonEncore_Thomas-_-Mercer-d8fc…epub
ROW1 : the 104-char stem, ellipsised     ROW2 : (empty)
FILE : Les refuges.epub
ROW1 : Les refuges                       ROW2 : (empty)
```

These two are the only rows in the library with no machine-readable author in the filename or the path. Enrichment fixes both — `Blake Crouch` and `Jérôme Loubry` — and that is the concrete, measured case for moving it to M2.

### A2.13 — On by default, no setting

Nothing on disk moves. The raw basename is stored verbatim in its own blob and shown in Details, one Confirm-hold away. The row never *claims* a field is the author — it fills two labelled slots whose provenance Details reports (`from the book file` / `from the file name` / `from the folder name` / `unknown`). A setting would double the test matrix for a transform whose worst case is "the row still shows the right title".

**The one measurement that would force a setting:** if a token-loss audit on real cards shows > 2% of names losing a title or author token to the unconditional segment-2+ drop, gate it behind `Library display: Clean / Raw filename` in the existing `OptionSelectionActivity` (`src/activities/util/OptionSelectionActivity.h:16-18`) — one enum, no new screen. Measured on this corpus: **0 genuine token losses in 69**; on a 27-name synthetic control set, 2 (both of the shape `Title -- Subtitle -- Author`, which is the documented behaviour).

---

## A3. The findability model — final

### A3.1 The row and the geometry

```c
props.labelText.maxLines = 1;                    // MUST be 1 — see below
props.rowHeight = uiListRowHeight(tokens, UiListRowType::WithSubtitle);   // set BEFORE
props.selectionMarker = fui::SelectionMarker::Underline;   // list.h:387-395
props.rowStyles = fui::plainStyles();
const uint16_t rows = configureUiList(props, screen.theme(), listRect,
                                      UiListRowType::WithSubtitle);
```

Three ordering facts, all in `src/components/UIThemeTokens.h`: `configureUiList` only derives a row height when `props.rowHeight <= 0` (:60), so setting it first wins — the exact precedent is `FileBrowserActivity.cpp:1095-1100`; `maxLines > 1` promotes the row to `tokens.rowHeight` (:63), which is why `maxLines` must be 1; and `uiListRowHeight` returns `tokens.rowHeight` immediately on a touch device (:38).

**Do not hardcode 13 rows anywhere.** `listWithSubtitleRowHeight` is 50 in `src/components/themes/BaseTheme.h:156`, **60** in `themes/lyra/LyraTheme.h:23` and **69** in `themes/roundedraff/RoundedRaffTheme.h:18`, and `scaledListMetric` (`UIThemeTokens.h:17-29`) multiplies by 1.2 under `UI_SCALE_LARGE`, which additionally swaps the body font `UI_10 → UI_12`. The shipped values are 50/60/69/60/72/83 px → **13/11/9/11/9/8 rows** in the 664 px band. Take the row count from `configureUiList`'s return value and derive pages from it. Every press figure below is quoted at 13 rows (BaseTheme, `UI_SCALE_SMALL`) and moves by less than 0.4 presses at 9 rows — mean travel is `n/(4P) + P/4`, which is 4.73 at P=13 and 4.39 at P=9.

The list is **flat**: every book on the card, no folder descent, no intermediate screen. Two pinned rows, initial selection on row 2:

```
[0]  "Search…"                                  UIIcon::Search
[1]  "Newest first · All books · 67 books"      UIIcon::Settings
[2…] books, each 50 px:   line 1 = title
                          line 2 = author
```

Buttons are house semantics, **unchanged** — tap = ±1, hold 500/500 = ±1 page, on `{Down,Right}` and `{Up,Left}` (`src/util/ButtonNavigator.h:47-53`). Left/Right stay aliases of Up/Down. Rebinding them to paging was proposed and is rejected: it is the one convention the user has learned everywhere else in the firmware, and a page turn already costs one press as a hold-tick.

### A3.2 Scenario (i) — "I remember one word of the title"

Search. `Search…` is list row 0, two presses away.

| Key | Presses |
|---|---|
| Home → Library (1 move + Confirm) | 2 |
| Up ×2 to `Search…`, Confirm | 3 |
| Type the word: mean 2.4 characters, grid travel + Confirm each | ~7.8 |
| Up into the 7-row result band, mean position within it | ~1.0 |
| Confirm | 1 |
| **Total** | **mean 14.8 · median 15 · p90 18 · max 19 · 0 of 69 unreachable** |

At ~185 ms per FAST refresh (§1.2) that is **2.7 s of panel time**. Three independent simulators land at 14.0 / 14.4 / 14.8 — this figure is solid.

**Three characters is the design point**, measured deterministically over every query prefix in the final fold:

| mode | k=1 | k=2 | k=3 | k=4 |
|---|---|---|---|---|
| strict prefix | 3.5 (89% fit 7 rows) | 1.6 (100%) | 1.1 (100%) | 1.1 |
| word-initial | 17.8 (8%) | 3.8 (88%) | 1.8 (98%) | 1.4 (100%) |
| interior substring | 44.1 (0%) | 10.1 (48%) | **2.5 (97%)** | 1.5 (100%) |

A fourth character buys 1.0 books. Budget the latency table at **Q=3**, not the spec's Q=2 — the spec's Q=2 is right for prefix queries, and this user's stated failure mode is the substring case.

The constrained keyboard passes spec measurement 9 with margin: **mean live letter keys 11.4 / 4.2 / 1.3 at depth 1 / 2 / 3**, against the >15-at-depth-2 threshold. §9.1's 6×5 grid, §9.2's union-over-tiers mask and §9.3's skip traversal with its reachability proof are unchanged.

**Digits:** the grid has none and four titles start with one, including `"2084"` — the user's own example. Do **not** add a sixth grid row: it costs 48 px and drops the untruncated result band from 7 rows to 6, the single most valuable property of that screen. Digits stay in the fold (harmless, never typed) and `TITLE_MERGE` plus the union fold guarantee every one of the four is reachable by a letter word. Measured: **0 of 69 unreachable**, `artificial` → 1 hit.

**Search is not sold as a speedup.** It is 1.6× slower than browsing on this corpus and the release note must say so. Its job is the case where position in a sorted list carries no information at all.

### A3.3 Scenario (ii) — "I remember the author"

Two routes, both correct, and the cheap one is **not** search.

| Route | Presses |
|---|---|
| Sort menu → **Author A-Z**, then browse | mean **9.2** · median 8 · p90 14 |
| Type the author into search | mean 15.5, and it fails to converge to ≤ 7 results for 14 of 69 books — `qiu` leaves 9 |
| **`More by <author>` from any book by that author** | **2 presses** (Confirm-hold, Down×1, Confirm) |

`More by <author>` sits in the per-book Confirm-hold menu §10.1 already specifies — zero new screens. It applies the record's **`authorKey`** as an equality filter, **not a substring of the display string**. That distinction is the whole feature: measured over this card, keying on the sorted-token key returns Qiu Xiaolong 9, Blake Crouch 6, John C Lennox 4, Alex Michaelides 4, Karine Giebel 3, John Scalzi 3 — always the complete set. A substring of the display string returns **0** for Lennox (the display spelling is `"John C. Lennox"` and the folded row text carries `"john lennox"` in a different order) and **2 of 4** for Michaelides. A user who invokes it and sees a partial set will reasonably conclude those are all the books they own by that author.

This is the honest answer to "it would be good to offer filters or something else for browsing": one menu string, two presses, complete results.

**No author browse screen, no per-author headers.** 40 keys over 69 books, 28 of them holding exactly one book — an author list is the same list with the titles removed, and it demands precisely the knowledge the user says they lack.

### A3.4 Scenario (iii) — "I remember nothing, I am browsing"

| | Presses | Panel |
|---|---|---|
| Flat list, newest first | mean **9.2** · median 8 · p90 14 | 1.70 s |
| Open the book just copied onto the card | **3** (2 to enter, selection already on row 2, Confirm) | 0.56 s |
| Sweep the entire library end to end | 5 page holds | 0.93 s (vs 1.85 s at today's 4-line rows) |
| Today's browser, knows the folder | ~10.2 | 1.89 s |
| **Today's browser, does not know the author** | **25.8 · p90 53** | **4.77 s** |

**The headline is 25.8 → 9.2, and the p90 collapse from 53 to 14.** The entire measurable win is deleting the folder descent; sort order is worth almost nothing at this scale (title A-Z, author A-Z and date-newest all land within 0.1 presses of each other). Say this to the user plainly rather than claiming a speedup from reordering rows.

What the press model is blind to, and what actually changed: it assumes the reader recognises the target the instant its row is on screen. That is true for a title+author row and false for a 180-character filename wall — which is why the lived experience is "sometimes I just cannot find it" rather than a uniform 26 presses.

### A3.5 Filters — they ship, and they are labelled honestly

All are one bit-test inside the pass §7.1 already runs, so each costs **zero** scan time.

- **Format** (EPUB / TXT & MD / XTC) — spec, unchanged.
- **Folder subtree** — spec, unchanged; one `u16` compare. This is where genre survives.
- **Language EN / FR** (new) — `dc:language` is present on 64 of 64 parsed books across 7 spellings (`en, fr, en-us, fr-fr, eng, fra, en-ca`); normalised to the primary subtag it is **38 English / 26 French**. It is the cleanest binary partition this corpus offers, it costs 2 bits, and it exists **only** in metadata — no filename carries it.

**The honest accounting, in the menu itself:** setting a filter costs ~10 presses and halving the list saves ~0.5 presses of travel. Filters are sticky preferences for narrowing an author's 9 books, not a step on the path to a book, and they start paying above ~300 books. **Genre is deliberately not promoted to a screen**: Romans alone is 52% of the library, `read` is a reading status that cross-cuts every genre, and 5 authors already straddle two top folders.

---

## A4. Index delta — changes to CLX1 only

### A4.1 `ClixRecord` — still exactly 128 bytes

```
off  sz  field            change
  0   4  dispOff   u32    was nameOff; now offsets into the DISPLAY blob ("title\0author")
  4   4  pathOff   u32    NEW: offsets into the PATH blob (raw basename, for open + Details)
  8   4  fileSize  u32    unchanged
 12   2  authorRank u16   unchanged
 14   2  dateRank  u16    unchanged
 16   2  firstSeen u16    unchanged
 18   2  folderId  u16    unchanged
 20   1  titleLen  u8     was nameLen
 21   1  authorLen u8     NEW
 22   1  pathLen   u8     NEW
 23   1  foldLen   u8     0..90 (was 0..96)
 24   1  authorKeyLen u8  low nibble 0..12; HIGH NIBBLE now carries
                          b4-5 language (0 unknown, 1 en, 2 fr, 3 other), b6 neverOpened, b7 rsvd
 25   1  flags     u8     b0-2 format, b3-4 authorProvenance, b5 titleFromOpf,
                          b6 titleMerged, b7 reserved
 26  90  fold[90]         was fold[96]
116  12  authorKey[12]    unchanged
```

`static_assert(sizeof(ClixRecord) == 128)` holds, so the whole §3.3 argument survives untouched: 32 records per 4 KB chunk, no straddle, 512-aligned by construction, `memcpy` pin. **Six bytes move from `fold` into the three new length fields.** Justified: the old 96-byte fold over raw filenames was truncated for 50 of 75 records and 6% noise; the new 90-byte fold over display text is truncated for 9 of 69 and 0% noise. `authorKeyLen` never exceeds 12, so its high nibble was dead space — the two new filters cost literally nothing.

### A4.2 Header

`longestName[3]` (offset 18, 6 bytes) is **deleted** — the row-height probe it fed is gone, since the row type is fixed `WithSubtitle`. Replace with `enrichedCount u16` at offset 18 and `reserved[4]` at 20. Header flag `b2 enrichComplete` keeps its meaning; `b3 booksAtRoot` is **removed** (the depth ≥ 2 gate makes it unnecessary and its >60% threshold does not trip on this card anyway).

### A4.3 Blob split, and why it matters

§3.5's single "display-name blob" becomes **two** sections:

- **Display blob** — `title\0author` in record order (= title order). Measured: 3,410 B, median 40 B/book.
- **Path blob** — raw basename with a NUL, in record order. Measured: 9,754 B, median 140 B/book.

The split is what keeps a page turn cheap: one 13-row page needs **642 B** of display text — one 512-byte read plus change — instead of 2,249 B if the basenames shared the blob. A single `memchr` at render time splits `title` from `author`; the interactive path does no other string work.

### A4.4 Size at the real corpus (N=67 indexed, F=26)

```
header       512
folder     1,024
records    9,216   <- the ONLY section a search pass reads; pinned in Tier P => 0 SD reads/keystroke
perms        512   (3 u16 arrays: author, date, shelf — 6N = 402 B, one sector)
display    3,584
paths     10,240
selfSize  25,088 B  = 24.5 KiB
```

Against the spec's raw-basename layout at the same N: 21,504 B. **The metadata costs +3,584 B and shrinks the fold every keystroke scans.** Dedup on `(folderId, name, fileSize)` in the merge-join removes the 6 phantom books.

### A4.5 Per-keystroke, restated in §7.4's mandatory form

N=67, Tier P (`128 × 67 = 8,576 ≤ 32,768`):

| Term | ms |
|---|---|
| Input tick | 0–10 |
| Record I/O | **0** (pinned) |
| Scan CPU — 67 × 57 fold bytes × Q=3 × 6 cyc @160 MHz | **0.43** (worst case at the 90 B cap: 0.68) |
| Window inserts | 0.05 |
| Key mask → 30 flags | <0.01 |
| Display reads for 7 result rows | 0.3–1.3 |
| **Render CPU** | **15–40 (UNMEASURED)** |
| DTM2 SPI / BUSY / `_fast` / DTM1 | 26 / 1–50 / ~133 / 26 |
| **Typical / worst** | **≈ 207 / 262** |

The application owns **0.5 ms of a ~207 ms keystroke**. The panel owns 88%. There is no index-structure argument to be had and none is made.

---

## A5. Metadata enrichment — moves to M2, automatic, page-first

### A5.1 The honest re-baselining, first

Every candidate design measured metadata against *today's raw filename row*, which credits metadata with gains the display transform alone delivers. Measured against the **filename fallback** instead, over the 69 distinct books:

- **Authors gained that did not exist at all: 2** — the 104-char Pines blob → `Blake Crouch`, and `./read/Les refuges.epub` → `Jérôme Loubry` (the only book on the card with no author anywhere in its path).
- **Exporter-truncated titles recovered: 7.** The exporter hard-truncates the title field at exactly 60 characters and 16 of 59 are cut mid-phrase; no filename rule recovers them.
- **Author strings canonicalised: 27.**
- **Titles materially shortened: 14**, of which `TITLE_MERGE` rescues 4 and 3 are correct blocks.
- **`dc:language`**, which no filename carries.

That is a real but modest marginal gain, and it is stated here so nobody later claims "64 of 75 titles" as if the alternative were the raw filename.

### A5.2 Why it still moves to M2

Three reasons, in order of weight:

1. It takes author coverage from **67/69 to 69/69**, and the 2 books it fixes are exactly the user's complaint — books whose author is nowhere in the filename.
2. **The fold changes when enrichment lands.** Building search (M3) against a fold that 59 of 69 records will later rewrite means shipping search twice. Enriching first means search is built once, against its final index.
3. The cost the spec was avoiding does not exist. §A5.3.

### A5.3 The bounded OPF prefix read — replaces `readFileToMemory` + `LIBRARY_OPF_MAX_BYTES`

Never inflate a whole OPF.

- **EOCD** is within the last 24 bytes on all 66 EPUBs (zero zip comments). A fixed 512-byte tail read always captures it — no 64 KB scan.
- **`META-INF/container.xml`** sits at central-directory ordinal 2 (median) and the OPF at ordinal 3, so the two lookups scan a median 177 + 282 bytes.
- **Gate on `compressedSize`** from the central directory (`lib/ZipFile/ZipFile.h:40`, `:57`) — known before any inflate. Measured: median 1,968 B, p90 2,751, **max 3,681**; 0 of 64 exceed 4,096. The spec's `LIBRARY_OPF_MAX_BYTES = 16384` gates on the *inflated* size (median 9,938, max **44,531**) and silently drops 5 of 64 books for no memory benefit.
- **Inflate a one-shot prefix into a 10,240-byte dest.** `InflateStream::init(false)` is documented as: *"the destination buffer holds the ENTIRE output, so back-references resolve inside it and no 32KB window is allocated"* (`lib/miniz/src/InflateStream.h:24-28`), and `Status::Ok` means *"Output buffer full; more decompressed data remains"* (`:40`) — a supported early stop. Correct by construction: every back-reference inside the first 10,240 output bytes has distance ≤ its own offset and therefore resolves inside the dest. `requiredStorageSize(false)` is state-only, ~11 KB with no window (`InflateStream.cpp:20-22`).
- **Do not use `readFileToStream` with `allowEarlyStop`** (`ZipFile.h:137`): `init(true)` claims the 32,768-byte sliding window for output we discard after 3 KB.
- **Do not use expat or `ContentOpfParser`** — it opens a temp item store, builds an Arena-backed manifest hash index and collects CSS hrefs, all for three tags. A ~1.2 KB byte-level 3-tag state machine under `src/activities/library/` is the additive answer.

Transient peak while the pass runs: dest 10,240 + tinfl state ~11,264 + zip read buffer 1,024 + path 288 ≈ **22.8 KB, largest single block 11,264 B** — inside `LIBRARY_OPEN_MIN_MAX_ALLOC` (16,384) and inside the WiFi-up 20,000 B `maxAlloc` floor. Against the spec's path, whose largest allocation on this card is 44,531 B.

Total SD bytes per book: median 3,666, p90 8,036, max 24,738 → **~310 KB for the whole library**. Wall time 35–75 ms/book: ~25–55 ms of FATFS open-by-path and 3 seeks (dominant), ~4–9 ms of payload, ~4–8 ms inflate, ~2–4 ms scan.

**Do not treat `META-INF/encryption.xml` as a skip condition.** Two books have one; both reference only `fonts/0000N.otf` under `ns.adobe.com/pdf/enc#RC` — Adobe font obfuscation, not DRM — and both parse fine. Only a `CipherReference` pointing at the OPF or a content document matters.

### A5.4 What the user sees before it has run

**A complete, correct list from the first paint.** Every row already has a title line and an author line, from the filename: 63 get title+author from the `" -- "` grammar, 4 more get an author from the folder, and exactly 2 rows lack an author. There is no "pending" badge, no half-populated layout, no per-row spinner — because **the row shape never changes**. Enrichment replaces *text inside existing rows*; row height, row count, scroll offset and sort position in Title and Date order are all unchanged, so the list never reshuffles under the cursor.

Two consequences are designed for explicitly:

- **Author A-Z is the one order enrichment can reorder.** Until header flag `b2 enrichComplete` is set, the sort menu labels it `"Author A-Z (from file names)"` and Details says `"author: from the file name"`. Nothing fails silently.
- **The pass runs visible-page-first.** On Library entry with `enrichComplete` clear, the cursor starts at the current viewport's `topIndex`, enriches those 13 books (0.5–1.0 s), repaints **once**, then continues over the rest in `loop()` slices at one book per idle tick — **with no further repaint unless the viewport moves ahead of the cursor.** With no partial refresh on X3 (`GfxRenderer::displayWindow` is commented out at `lib/GfxRenderer/GfxRenderer.h:214-215`; `Uc8253X3Driver` does not override `PanelDriver::displayWindow`) every repaint is a ~185 ms full flash, so the naive per-book repaint would be 67 flashes and 12 s of flickering. One flash on entry, none after, is the specification.
- Guards, unchanged from §5.6: `vTaskDelay(1)` every 8 books, a heap gate before each peek, Back cancels, `enrichCursor` rewritten every 16 books. An inflate failure mid-book sets **provenance 3** and keeps the filename row — this is not hypothetical: one EPUB on this card lists 92 entries and reads `container.xml` fine but throws `invalid distance too far back` on 28 of them, including the OPF.
- **The free ride is unchanged and stays gated on `Epub::hasCache` first** (`lib/Epub/Epub.h:112`), because `BookMetadataCache::load()` deletes `book.bin` on a version mismatch (`lib/Epub/Epub/BookMetadataCache.cpp:504, :510, :518, :525, :537`).

**No renaming, ever.** `KOReaderDocumentId::calculate` is content-based so a rename would be safe for kosync, but `Epub::cachePathForFilePath` is `"epub_" + fnvHash64(filepath)` (`lib/Epub/Epub.cpp:253-256`), so a rename orphans the cache and every renamed book re-indexes from scratch. A display transform pays neither cost and needs no user decision.

---

## A6. Revised milestone order

| M | Ships | Worth alone |
|---|---|---|
| **M1 — The Shelf** | CLX1 with the §A4 record, walk with `(folderId, name, fileSize)` dedup and PDF exclusion, sort, emit, tmp+rename install, optimistic open, verification walk in `loop()` slices, the §A2 display rule **from filenames only**, `LibraryListActivity` with 50 px two-slot rows, the Underline marker, the position readout, newest-first default, Title A-Z/Z-A, Refresh, the Details/provenance row, the heap degrades, the Home entry | **This is the feature.** One flat list of every book, no folder descent, an author on 67 of 69 rows, 6 pages instead of 10, 3,245 characters instead of 9,551. Browsing goes from 25.8 presses to 9.2, p90 from 53 to 14. Opening the book just copied on is 3 presses |
| **M2 — Enrichment** *(was M4)* | The bounded OPF prefix read, the 3-tag scanner, page-first resumable cursor, `TITLE_MERGE`, modal canonicalisation, `dc:language` into the record, the free ride, provenance surfacing | Author coverage 67/69 → **69/69**, 7 exporter-truncated titles recovered, 27 author spellings canonicalised. Locks the fold so M3 is built once |
| **M3 — Search** *(was M2)* | `LibrarySearchActivity`, the §7.1 pass, tiered matching, the union next-char mask, `fui::keyGrid` with the §9.3 skip traversal, 7 untruncated live result rows, the up-ring | Converts "I remember a word from the middle" from a linear scan into a bounded 14.8 presses, 0 of 69 unreachable. **Not** a speedup — it is 1.6× slower than browsing here |
| **M4 — Sorts, filters, More by** | Author/date/shelf permutations, `More by <author>` on the author key, format + folder-subtree + language filters, `OptionSelectionActivity` menu, the `ranksDegraded` degrade | The 2-press complete author set. Genre survives as the folder-subtree filter, language as the one clean binary partition |
| **M5 — Conditional perf** | Only if §A7 demands it: async overlap, the refine pin at `matchCount ≤ 96`, the key-grid framebuffer snapshot | — |

**Explicitly out of this addendum:** the cover-thumbnail grid (separate spec).

---

## A7. Measurements on the real device that would falsify this

Take A1–A3 **before writing M2**.

| # | Measure | How | Threshold | Action |
|---|---|---|---|---|
| **A1** | **Render CPU** for one 13-row two-slot page | `millis()` around the draw block in `render()` | **> 60 ms** | Still the only unmeasured term and 35–90× the search. A 13-row two-slot page issues 26 text draws against the 4-line list's ~28 wrapped lines, so this should be a wash — but *measure it*, do not assume. If crossed, snapshot the static key grid with `GfxRenderer::readFramebufferRegion`/`writeFramebufferRegion` (`GfxRenderer.h:262-263`) and repaint only changed cells |
| **A2** | **Characters the label band actually holds** at the shipped theme × UI scale | `renderer.getTextWidth` on a 45-`M` string in the row's content rect | **< 34 characters** | At `UI_SCALE_LARGE` + RoundedRaff the band is ~34 chars and the ellipsis rate rises from 18/69 to ~30/69. Only then re-open the subtitle-cut question — and only as a *runtime, pixel-measured* cut, never one baked into the index |
| **A3** | **Rows per page**, derived not assumed | log `configureUiList`'s return value at each of the 6 theme × scale combinations | outside **8..13** | Every press figure in §A3 is quoted at 13. Assert the value is derived at runtime; a hardcoded 13 is wrong for 3 of the 6 shipped combinations |
| **A4** | **OPF prefix hit rate** | fraction of EPUBs yielding a non-empty `dc:title` from a 10,240-byte one-shot prefix | **< 95%** | Escalate the dest to 16,384 B behind a menu action. Measured `</metadata>` offsets on this card: median 2,583, p90 4,775, max 9,176 — the margin is real but proven on one exporter |
| **A5** | **Enrichment wall time per book** on device | timestamp 20 books, `LOG_INF` the mean | **> 150 ms/book** (> 10 s for 67) | Demote the pass from automatic back to opt-in behind the menu action. Budget is 35–75 ms/book |
| **A6** | **Author coverage** | count records with `authorProvenance == 3` after M2 | **> 10% of records** | The G2 sibling-corroboration guard is too strict for this card. Relax it to also accept a parent folder whose folded tokens all occur in the book's own stem |
| **A7** | **Duplicate dirents** | count records dropped by the `(folderId, name, fileSize)` dedup on the real card | **≠ 6** | If 0, the on-device DFS does not reproduce the host's duplicate `readdir`; if > 6, the signature is not discriminating and the phantom rows will ship |
| **A8** | **Unopenable dirents** | count names `readdir` returns that `open()` then fails on | **> 0** | The walk must already tolerate this (drop, do not index, never abort). But 7 of 75 names on this card are unopenable from macOS — all NFD — so if FATFS reproduces it, coverage is 76% not 85% and §A5's headline changes |
| **A9** | **Fold at the cap** | count records with `foldLen == 90` | **> 25%** | Widen the record to a 256-byte stride (16 per 4 KB chunk, still straddle-free) and re-derive §3.7. Measured here: 9 of 69 = 13% |
| **A10** | **Constraint strength** | instrument the mask, log `popcount` at depth 1/2/3 | **> 15 live keys at depth 2** | Unchanged from spec measurement 9. Measured here: 11.4 / 4.2 / 1.3 — passes with margin |
| **A11** | **Heap at Library open**, pinned and streamed, and peak during the enrichment pass | `ESP.getFreeHeap()` / `getMaxAllocHeap()` in `onEnter()`, `onExit()`, and each 8-book slice | free < 30 KB, `maxAlloc` < 16 KB, or any leak across enter/exit | The 11,264 B tinfl state is the largest allocation the feature ever makes. If `buildscratch::claim()` is unavailable and the heap is fragmented, the pass must degrade to "not enriched, try Refresh" and **must never block the list** |
| **A12** | **Flash delta** | `pio run -e default` + `scripts/check_firmware_size.py` before and after **each** milestone | remaining **< 100,000 B** | Estimated delta over the spec's own M1–M3: +4 to +7 KB of `.text` (prefix scanner ~1.2 KB, cleanup ~1.5 KB, filename fallback ~1.8 KB, canonicalisation ~0.3 KB) **minus** ~1 KB of the publisher/place/edition classifier this design deletes, plus ~10 new `tr(STR_*)` ≈ 0.9 KB. Against 291,168 B of headroom |

**Host unit tests required before M1 merges**, in addition to §13's list: `fold()` against **both NFC and NFD** forms of the same 20 names (a test against this card alone is a false pass, since it contains zero precomposed accents); `authorKey()` order-insensitivity over the 6 Qiu Xiaolong spellings; the underscore rules against `A_Brief_History_of_Time`, `war_and_peace`, `Douglas R_ Hofstadter`, `Paris, B_A_` and `Qiu Xiaolong_`; the `NOT_AUTHOR` guard against all 69 titles and 59 authors asserting 0 false hits; `TITLE_MERGE` asserting it fires on 4 and blocks the 4 exporter-truncated cases; the G1/G2 folder gate against `./Books/Dune.epub`, `./SciFi/Expanse/01 - Leviathan Wakes.epub` and `./Fiction/Mystery/Agatha Christie/Murder.epub`.

---

## A8. Explicitly not in scope

Additions to §15, all decided against on measurement:

- **Renaming any file on the card.** Ever, under any setting. The display transform pays neither the kosync nor the cache cost and needs no user decision.
- **Any build-time truncation of the display title** — no subtitle cut, no character cap, no ellipsis written into the index. The pixel-accurate renderer decides, at runtime, at the user's theme and scale.
- **A classify-then-strip filename cleanup ladder** (Anna's Archive / md5 / ISBN / year / place-year / publisher word list). Position gating achieves the same length win with zero content classifiers and a strictly better worst case.
- **Inverting `"Last, First"` to `"First Last"` for display.** It merges nothing (`Qiu, Xiaolong` → `Xiaolong Qiu` and `Xiaolong, Qiu` → `Qiu Xiaolong` remain two strings), cannot handle the multi-author `';'` form, and `"Michaelides, Alex"` is already perfectly readable. Modal-spelling canonicalisation does the job without a heuristic.
- **`opf:file-as` as a canonicalisation source.**
- **An author browse screen; per-author section headers in author sort.**
- **A genre filter screen.** Genre survives only as the folder-subtree filter and in Details.
- **Series as a browse dimension, a filter, or a sort.** 7 of 64 books carry it; 2 of the 7 values are junk. Store it if free; render no UI on it.
- **A digit page on the search keyboard.**
- **Rebinding Left/Right to paging.** Left ≡ Up everywhere else in the firmware; a hold-tick already pages in one press.
- **PDF support.** Both PDFs are the same book; there is no `hasPdfExtension` anywhere and `isSupportedBrowserFile` (`FileBrowserActivity.cpp:137`) has no PDF branch. They are excluded from the index, not shown greyed, and this belongs in the release note.
- **Fuzzy / edit-distance matching** for the `Pachinko` / `Min Lee Jin-Pachonko` typo pair and the `Dans son silence` / `The Silent Patient` translation pair. Two cases in 69; the human resolves them, and a title-sorted flat list already puts the first pair adjacent, which is the delete-the-copy affordance.
- **Non-Latin folding.** The damage is baked in at build time and metadata-first makes it marginally worse (`dc:title` can be in a script the filename transliterated). Recorded so a future fix knows it must bump `foldVersion` and rebuild.
- **Cover thumbnails.** Separate spec. Recognition-by-cover is the strongest browse mechanism for physical books and nearly worthless here — these are exports of books the reader has never seen as objects.