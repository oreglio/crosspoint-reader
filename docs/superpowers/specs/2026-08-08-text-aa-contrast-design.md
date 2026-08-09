# Text anti-aliasing & contrast — evaluation harness and threshold recalibration

Date: 2026-08-08
Status: tier-1 comparator built (`lib/EpdFont/scripts/preview_aa.py`); two levers tested
on hardware and REJECTED (force-autohint, wght 450 — see "Hardware verdicts" at the end,
which supersedes the earlier V9 selection); threshold-only candidates S1/S2/S3 built and
awaiting the on-device verdict.

## Problem

Reported symptoms, on both X3 and X4: body text reads as **blurry/mushy** and, at the
same time, **jagged on curves**. These look contradictory but share one root cause —
four grey levels whose coverage thresholds were never matched to the panel's actual
response.

Contrast was part of the original brief but is **not** a deficit: hardware photographs
(below) show black density is already good. The work is therefore entirely about where the
four levels are placed, not about how hard the panel is driven.

No tool in this repository can render a glyph offline or compare two renderings. Every
anti-aliasing decision to date was therefore taken without side-by-side evidence. Closing
that gap is the primary deliverable; the threshold change is the secondary one.

## Evidence

All facts below were verified directly against the tree, not inferred.

### The panel's four levels are not evenly spaced, and the font path assumes they are

`lib/GfxRenderer/BitmapHelpers.h:147-161` carries a measured calibration of the panel,
under the comment `// fine-tuned to X4 eink display`:

| level | measured output (0 = black, 255 = white) |
|---|---|
| black | 15 |
| dark grey | 30 |
| light grey | 80 |
| paper white | 210 |

Paper white is 210, not 255, and **both intermediate levels sit in the dark half**. This
calibration is used only by the image ditherer. The font quantiser
(`lib/EpdFont/scripts/fontconvert.py:39,380-390`) places its cutoffs as though the four
levels were evenly spaced.

Consequence, for a pixel at coverage `c` on paper (ideal output = `210 - 195c`):

| coverage | ideal | rendered | error |
|---|---|---|---|
| 10 % | 191 | 210 | mild |
| 20 % | 171 | 80 | −91 |
| 40 % | 132 | 30 | −102 |
| 60 % | 93 | 30 | −63 |
| 70 % | 74 | 15 | −59 |

A pixel grazed at 20 % coverage is rendered more than halfway to black. That fattens
every glyph edge (**mushy**), and the 210 → 80 step consumes 67 % of the available ink
range in one jump (**jagged**).

### Photographic evidence (X4, Lexend Deca, macro)

Two close-up photographs of the reported defect on real hardware, supplied 2026-08-08.
They narrow the problem decisively:

- **Black density is adequate.** Ink is dense and cleanly separated from the paper tone.
  There is no contrast deficit to recover. This is the primary reason waveform/LUT/VCOM
  work is a non-goal below — the evidence says there is nothing there to win.
- **Vertical stems are crisp.** Clean edges on `h`, `b`, `l`. Hinting is working; there is
  no structural blur in the raster.
- **The defect is confined to curves and diagonals.** `o`, `s`, `g` carry a thick,
  stepped dark fringe rather than a graded transition.
- **Apparent weight reads heavier than Regular** — closer to a Medium.

This is the exact signature the threshold arithmetic predicts. A vertical stem has two
edge pixels, both at high coverage, both landing in solid black — hence crisp. A curve has
many pixels at 10–40 % coverage, all of which are pushed to 80 or 30 against a paper tone
of 210 — hence a heavy, stepped fringe. The anti-aliasing is not failing on curves; it is
overloading them.

Unresolved from photographs alone: the second image shows greyed text at the upper left,
co-located with a specular highlight. Most likely glare. If the user confirms the same
greying by eye without a reflection, that is a separate defect (retention/ghosting) and
needs its own investigation.

### The countervailing evidence: thin stems wash out

Two commits document the opposite failure mode, and they are why the current thresholds
were lowered:

- `cff54d77` (2026-05-10): *"Bitter font at regular weight can become washed out when AA
  is on due to some of the thinner stems and posts of the font."* Fixed by raising the
  variable-font weight axis 400 → 500 for SD Bitter (`sd-fonts.yaml:48,58`).
- `052f497b` (2026-02-24): *"the 'k' stem (8-bit fringe: 0x38=56) falling just below the
  2-bit quantization threshold while 'l' and 'h' stems (fringes: 76, 64) land above it —
  making 'k' visibly narrower (2.00px vs 2.33px effective width)."* Fixed with
  `FT_LOAD_FORCE_AUTOHINT`.

Both failure modes are real. They are the two ends of one badly-placed threshold ladder.

### `--darken-aa` moves all three thresholds together

Introduced 2026-04-28 in `a3c1ef62` ("feat: make fonts render darker"), **with an empty
commit body — no recorded rationale**. It lowers the tuple from `(4, 8, 12)` to
`(3, 6, 10)`, i.e. all three cutoffs at once. It is applied unconditionally to every
built-in reading font (`convert-builtin-fonts.sh:149`) and every SD font
(`build-sd-fonts.py:384`). It is a CrossInk-only divergence; upstream CrossPoint still
uses the hard-coded `(4, 8, 12)`.

Measured flash cost of the flag: **+49 bytes** on LexendDeca 12 regular (+0.33 %).
Threshold work is effectively free.

**The three thresholds have never been tuned independently, on any branch.** Nor have
`FT_LOAD_TARGET_LIGHT`, FreeType stem darkening, or oversampling — `git log -S` across
`--all` returns zero hits for each.

### Two confirmed rendering defects, scoped to non-body text

- `lib/GfxRenderer/GfxRenderer.cpp:672-711` — `renderCharScaled` (superscript, subscript,
  Japanese ruby) takes `renderMode` as a parameter and **never reads it**, thresholding to
  binary instead. That text is drawn with no anti-aliasing at all: solid black, maximally
  jagged. It does **not** corrupt the grey planes — in a grey pass it calls
  `drawPixel(state=true)`, which clears a bit that `clearScreen(0x00)` already left at
  zero, so the effect is a no-op rather than a wrong value. The fix is the same either
  way; the severity is lower than a corruption would be.
- `lib/GfxRenderer/GfxRenderer.cpp:763-771` — `renderCharSmallCaps` resamples with a MAX
  filter rather than an area-weighted average, so small caps come out systematically
  heavier.

### One unvalidated hardware hypothesis, deliberately out of scope here

In `freeink-sdk/.../lut/Uc8279X3Luts.h:90-96`, the `XTF_AA` rows for VCOM, WW and BB are
byte-identical. Dark grey maps to WW, so on X3 units carrying the UC8279d controller dark
grey would receive no drive and collapse to black — three effective levels, not four. The
file itself is marked `PENDING HARDWARE VALIDATION`, and an alternative table (`XTH4`)
exists in the tree with no caller.

This is tracked separately (see Non-goals). It is hardware-specific, needs a flash to
test, and mixing it into a font-side change would confound both.

## Constraints

| constraint | value | source |
|---|---|---|
| Firmware headroom | 223,808 B of 6,553,600 (3.4 %) | `partitions.csv`, measured |
| Font data share | 2.17 MB = 36 % of the image | measured |
| RAM (C3, X3/X4) | ~380 KB, no PSRAM, single 48 KB framebuffer | `AGENTS.md` |
| Grey levels | **4, hardware ceiling** — two RAM planes index a four-cell LUT | driver code |
| Cache invalidation | font IDs are SHA-256 of generated headers; regeneration re-indexes every book on the card | `build-font-ids.sh`, `Section.cpp:214,225` |

Measured cost of 4 bpp glyphs, after DEFLATE: **×1.80–1.98**, i.e. ≈ +860 KB against
219 KB available — and the renderer would re-quantise to four levels at draw time anyway.

## Non-goals

- 4 bpp glyph bitmaps or any increase in stored font data.
- More than four grey levels.
- Waveform / LUT / VCOM byte-tuning. Two reasons. First, the photographs show black
  density is already adequate, so there is no contrast to recover. Second, history warns
  against it: two independent waveform retunes (`a479e21`, `3475bd2f`) failed, and the
  real cause turned out to be a single CDI byte (`e52d480`). Instrument first.
- The UC8279d dead-grey hypothesis — separate track, separate change.
- Any on-device UI. Upstream's live-preview Text Settings screen was deliberately
  reverted here (`d556a29a`); the harness is a developer tool only.

## Design

### Three-tier evaluation ladder

Each tier answers a question the tier below cannot, and costs an order of magnitude more.
We only climb when the cheaper tier has stopped discriminating.

| tier | tool | decides | cost per variant |
|---|---|---|---|
| 1 | offline comparator (this spec) | edge shape, stem consistency, weight | ~1 s |
| 2 | `pio run -e simulator-X3` | full-page layout in context | ~2 min |
| 3 | flash to X3 + X4 | **black density in real light** | ~10 min |

Tier 2 is confirmed viable: the simulator composes the real LSB/MSB planes into a
four-level preview, including the tiled strip path. It shows AA *structure* faithfully
but approximates the greys as 200/96, so it **cannot** rank two candidates that differ
only in perceived darkness. Tier 3 is mandatory before shipping a threshold change, and
must be judged by eye — the on-device screenshot writes from the 1-bpp framebuffer and
captures the B/W pass only.

### Tier 1: the comparator

`lib/EpdFont/scripts/preview_aa.py`, a standalone developer script. Not referenced by the
firmware build, not shipped.

**Dependencies:** `freetype-py` and `fontTools`, both already in
`lib/EpdFont/scripts/requirements.txt`. PNG output is written with `zlib` from the
standard library — no new dependency.

**Pipeline**, mirroring the production path exactly:

1. Rasterise with FreeType using the same flags as `fontconvert.py:40`
   (`FT_LOAD_RENDER | FT_LOAD_NO_BITMAP`), at the same size basis
   (`set_char_size(pt<<6, pt<<6, 150, 150)`).
2. Reduce 8-bit alpha to 4-bit and quantise to 2 bpp with the variant's threshold tuple,
   reproducing `fontconvert.py:352-390`.
3. Apply the renderer's mapping from `GfxRenderer.cpp:865-883` — `bmpVal = 3 - raw`, then
   the BW/LSB/MSB plane composition — so what is displayed is what the device would
   compose, not the raw glyph.
4. Paint with the **measured panel levels** `15 / 30 / 80 / 210`, not idealised greys.

**Output:** one PNG containing, per variant, a labelled row with

- a paragraph of French prose at 12 pt and 14 pt,
- an ×8 nearest-neighbour zoom of `o`, `m`, `k`, `e`,
- the three metrics below, printed as text.

**Metrics**, so judgement is not purely subjective. These are derived from the acceptance
criteria and are the harness's main non-visual output:

- *stem-width spread* — effective stem width of `k`, `l`, `h` (the exact measurement from
  `052f497b`). Lower spread is better; that commit treats 2.00 vs 2.33 px as a defect.
- *halo mass* — count of light-grey pixels 4-adjacent to paper white, per glyph. A proxy
  for edge fattening.
- *ink mass* — sum of coverage levels per glyph. A proxy for overall weight, to confirm a
  candidate has not simply been lightened into illegibility.

### Variants in the first sheet

Lexend Deca, the user's reading font, at 12 and 14 pt.

| id | change | tests |
|---|---|---|
| V0 | `(3, 6, 10)` — current, `--darken-aa` | baseline |
| V1 | `(4, 8, 12)` — upstream default | is the darkening helping at all? |
| V2 | `(5, 6, 10)` | raise the white cutoff only: fewer faint halos, stems untouched |
| V3 | `(5, 8, 10)` | asymmetric ladder, both ends |
| V4 | `(5, 13, 15)` | strict match to the measured panel response |
| V5 | V0 + `FT_LOAD_FORCE_AUTOHINT` | the fix that worked in `052f497b`, currently unused |
| V6 | V0 + `FT_LOAD_TARGET_LIGHT` | less horizontal grid-fitting, truer curves |
| V7 | V0 + FreeType stem darkening | thicken stems without darkening halos |

V2 and V3 are the design's central bet: they decouple the three thresholds, which no
prior change has done.

### Results (measured 2026-08-08, `preview_aa.py`, Lexend Deca variable)

Fidelity is the mean absolute error against FreeType's exact area coverage blended
linearly between paper and ink, in panel units; the ink range is 195, so 24.8 is a 12.7 %
average error. Ink is coverage summed per glyph — the weight proxy.

| id | config | fidelity | err % | ink |
|---|---|---|---|---|
| V0 | `(3,6,10)` w400 — **current** | **24.77** | 12.7 % | 119.6 |
| V1 | `(4,8,12)` w400 — upstream | 20.17 | 10.3 % | 113.0 |
| V2 | `(5,6,10)` w400 | 22.82 | 11.7 % | 116.5 |
| V3 | `(5,8,10)` w400 | 20.18 | 10.3 % | 113.7 |
| V4 | `(5,13,15)` w400 — colorimetric | **13.02** | 6.7 % | **99.0** |
| V5 | `(5,8,10)` w450 | 20.23 | 10.4 % | 123.7 |
| V6 | `(5,8,10)` w500 | 18.75 | 9.6 % | 134.4 |
| V7 | `(4,8,12)` w500 | 18.27 | 9.4 % | 132.0 |
| V8 | `(3,6,10)` w400 + autohint | **15.29** | 7.8 % | 120.8 |
| V9 | `(5,6,10)` w450 + autohint | 17.42 | 8.9 % | 129.0 |

Three findings:

1. **The current configuration is the worst of the ten.** The diagnosis holds
   quantitatively, not just visually.
2. **`--force-autohint` alone (V8) cuts the error 38 % at unchanged weight** — ink moves
   119.6 → 120.8. The autohinter aligns stems to the pixel grid before quantisation, so
   fewer pixels land in the ambiguous coverage band where four levels cannot decide. The
   flag is already plumbed end-to-end through both converters and used by **zero** fonts.
   This is the single highest leverage-to-risk change available.
3. **Thresholds and weight compose, as predicted.** V9 reaches 17.42 while carrying *more*
   ink than today (129.0 vs 119.6): cleaner edges and a sturdier stroke simultaneously,
   because the two levers act in different places — thresholds clean the edge, weight
   fills the stroke.

V4 has the best number and the least ink (−17 %), failing acceptance criterion 3. It is
the `cff54d77` washed-out failure mode arriving by a different road, and is the reason the
numeric score is an input to the decision rather than the decision.

### Bitter measured separately — and it needs a different setting

Scope was narrowed by the user to **Lexend Deca and Bitter**. Those are, in fact, the only
reading families compiled into the firmware (`builtinFonts/all.h` includes bitter and
lexenddeca; charein is generated but never included), so the narrowing costs no coverage.

Bitter was measured from its real baseline of `wght: 500` — it was re-weighted from 400 in
`cff54d77` — because comparing it against 400 would measure that earlier fix rather than
this one.

| id | Lexend Deca fidelity | Bitter fidelity | Bitter stem spread |
|---|---|---|---|
| V0 — current | 24.77 | **29.73** | **0.33** |
| V8 — autohint only | 15.29 | **17.40** | **0.00** |
| V9 — thresholds + weight + autohint | 17.42 | 20.43 | 0.33 |

**On Bitter, V8 beats V9 on both metrics.** Two findings follow:

1. **The `052f497b` defect is still live, in Bitter, today.** The stem-spread metric stayed
   at 0.00 for every Lexend Deca variant — it is a geometric sans with uniform strokes, so
   the metric never engaged. On Bitter, V0 measures 0.33 px: `k` is narrower than `l` and
   `h`, exactly the failure that commit diagnosed in February. It was never fixed
   elsewhere; the font it was found on was simply removed, and `--force-autohint` was never
   applied to anything else.
2. **V9's threshold shift is actively wrong for Bitter.** Raising the white cutoff from 3
   to 5 drops its thin slab serifs a level, re-creating the stem irregularity that the
   autohinter had just removed. Lexend Deca has no comparably fragile feature, so it
   absorbs the same shift without harm.

**Therefore: per-family settings, not a global ladder.**

| family | thresholds | weight | autohint |
|---|---|---|---|
| Lexend Deca | `5,6,10` | 450 | yes |
| Bitter | `3,6,10` (unchanged) | 500 (unchanged) | yes |

Bitter's is also the more conservative change: one flag moves, weight does not.

### Selected: V9 for Lexend Deca, V8 for Bitter, validated against V0 on hardware

User selected V9 by eye. V9 changes three things at once (thresholds, weight, hinting),
so a hardware regression would be hard to attribute. Mitigation, and the shape of the
implementation: build **V0, V8 and V9 as three separately-named SD fonts** and install all
three. That allows switching between them on the same book from the device menu, needs no
flash, risks nothing, and isolates the autohint contribution (V8 vs V9). Only after the
hardware verdict do the built-in fonts get regenerated.

### Error handling

The script fails loudly and does nothing else. A missing font file, an unmappable
codepoint, or a variant whose FreeType property call returns non-zero must abort with the
offending variant id and font path named — a silently-skipped variant would corrupt a
visual comparison in the least detectable way. `fontconvert.py:352-367` ignores
`bitmap.pitch`; the comparator must use `abs(bitmap.pitch)` as
`fontconvert_sdcard.py:730-746` does, because `FT_LOAD_TARGET_*` experiments can return
`pitch != width` and would otherwise corrupt output silently.

## Acceptance criteria

A candidate replaces V0 only if it satisfies all of:

1. **Stem-width spread across `k`, `l`, `h` no worse than V0** at both sizes. This is the
   regression `052f497b` fixed; re-introducing it is not acceptable at any halo benefit.
2. **Halo mass strictly lower than V0.** This is the reported defect.
3. **Ink mass no more than 15 % below V0.** One-sided: the photographs show current text
   reading heavier than Regular, so some lightening is the goal. The bound guards against
   overshooting into the `cff54d77` washed-out failure mode.
4. Judged better by eye at tier 1, then confirmed at tier 3 on both X3 and X4.

## Testing

- Host tests: add a golden test asserting the quantiser maps a known coverage ramp to the
  expected level for each variant tuple. No test in the repo currently asserts anything
  about glyph pixels — this is greenfield (`test/differential_rounding/` is the only
  font-adjacent suite).
- `./bin/clang-format-fix` and `pio check` are not triggered by a Python-only change, but
  must run for the two renderer defect fixes if those land in the same branch.
- Regeneration path, after any accepted threshold change:
  `convert-builtin-fonts.sh` → `build-font-ids.sh > src/fontIds.h` →
  `verify_compression.py` → `pio run -e default` (which enforces the size limit).
- **Hardware verification:** flash X3 and X4, open a Lexend Deca book at 12 pt, compare
  against a photo of the same page before the change. Expect every book to re-index on
  first open (font-ID change). Clear `.crosspoint/epub_<hash>/` if output looks stale.

## Known defects to fix alongside

Independent of the threshold work, both confirmed by reading the code:

- `renderCharScaled` must honour `renderMode` (`GfxRenderer.cpp:673`). ~10 lines. Fixes
  superscript/subscript/ruby jaggedness and stops it writing wrong values into both grey
  planes.
- `renderCharSmallCaps` should use an area-weighted average rather than MAX
  (`GfxRenderer.cpp:763-771`). Same loop, same cost.

## Risks

| risk | mitigation |
|---|---|
| The comparator diverges from the real pipeline and we tune against a fiction | Step 3 replicates `GfxRenderer.cpp:865-883` exactly; tier 2 cross-checks a full page against the simulator before any regeneration |
| We re-introduce the washed-out-stems regression | Acceptance criterion 1, measured with the exact metric from `052f497b` |
| Simulator greys mislead on darkness | Explicit: tier 2 cannot rank darkness; tier 3 is mandatory |
| Every book re-indexes after regeneration | Stated up front; a one-time cost, flagged in the CHANGELOG entry |
| `convert-builtin-fonts.sh` is broken as written — it references a missing `IBMPlexSansHebrew` source directory at the UI-font stage | Reading fonts have every source they need; run families individually, and fix or gate the UI stage separately |

## How the accepted thresholds ship

If a variant wins, `fontconvert.py` and `fontconvert_sdcard.py` gain an explicit
`--aa-thresholds W,L,B` argument. `--darken-aa` is kept as an alias for `3,6,10` so
existing invocations and the upstream diff stay legible, and the default remains
`4,8,12`. The generation scripts pass the winning tuple explicitly. Independent tuning is
the whole point of the change, so a second boolean flag would be the wrong shape.

## Open questions

- Which controller do the user's X3 and X4 units actually carry? The driver is chosen by a
  boot probe and printed to serial (`[XTDET] promoted …`). Needed before the dead-grey
  hypothesis can be scheduled — not blocking for this spec.
- Whether the greyed region in the second photograph is glare or retention. Needs a
  by-eye confirmation from the user, not a photograph. Separate defect if confirmed.

## Hardware verdicts — 2026-08-08

Two of the three levers were rejected on device. Both rejections came from the user's eye,
and both exposed a limitation in the tier-1 harness.

### Rejected: `--force-autohint`

Reported as changed letter spacing in "kilometers". Confirmed and quantified: the
autohinter snaps stems to the pixel grid, which **thins the narrow verticals**.

| pt | `i` width | `l` | `t` |
|---|---|---|---|
| 10 | 4 → 3 | 3 → 3 | 8 → 8 |
| 12 | **5 → 3** | 4 → 3 | 10 → 9 |
| 14 | 5 → 4 | 4 → 4 | 11 → 10 |

At 12 pt the `i` loses 40 % of its width. Ink-gap spacing changes on 22–25 of 39 letter
pairs at 10–12 pt. Advances are untouched (`linearHoriAdvance` is unhinted), so lines do
not reflow — only the ink moves.

**This invalidates the earlier "V8 is a free win" claim.** Autohint did not render the
typeface more accurately; it rendered a pixel-snapped approximation of it.

**Harness blind spot, now documented:** the `fidelity` metric scores rendered output
against *its own* rasterised coverage. Grid-fitting removes fractional coverage entirely,
so there is nothing left to quantise badly and the score collapses — rewarding the
elimination of ambiguity rather than fidelity to the design. The metric cannot penalise a
hinting mode that deforms the letterform. A second metric measuring gap deviation from the
unhinted design was added and *also* favoured autohint (rhythm RMS 1.07 → 0.79 at 12 pt),
because it measured gaps rather than stroke widths. **Stroke-width stability across sizes
is the check that catches this**; use it before accepting any hinting change.

### Rejected: `wght: 450`

Judged too heavy on device (+8 % ink over baseline). The weight axis is therefore out for
Lexend Deca; the original "washed out" problem that motivated `--darken-aa` does not apply
to this typeface, whose strokes are uniform by design.

### Remaining lever: thresholds only

Thresholds apply *after* rasterisation, so glyph dimensions, bearings and advances are
bit-identical to what ships today — only pixel shading changes. Confirmed empirically: all
three candidates are **exactly 876,523 bytes, the same as the shipping font**, with
different content.

| id | thresholds | fidelity | ink vs today |
|---|---|---|---|
| — | `3,6,10` (current) | 24.77 | — |
| S1 | `5,6,10` | 22.82 | −2.6 % |
| S2 | `5,7,10` | 21.44 | −3.9 % |
| S3 | `5,8,10` | 20.18 | −5.0 % |

Recommended: **S3**. The black cutoff stays at 10 so stems remain solid; only halo pixels
lighten. Raising the black cutoff (e.g. `5,8,12`, −7.0 %) starts thinning stems and
re-opens the `cff54d77` washed-out risk.

Bitter is unaffected by these verdicts and still wants autohint on its own merits
(stem spread 0.33 → 0.00) — but that must now be re-checked against stroke-width stability
before shipping.
