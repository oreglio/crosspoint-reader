# Text Anti-Aliasing Recalibration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make reader text render with cleaner glyph edges by decoupling the three
anti-aliasing thresholds, enabling FreeType's auto-hinter, and raising the font weight
axis — validated by an on-device A/B before anything is regenerated wholesale.

**Architecture:** All rendering-quality changes happen at *font conversion* time, in
Python, and cost zero firmware bytes. Both converters gain an explicit
`--aa-thresholds W,L,B` argument; `--darken-aa` becomes an alias for `3,6,10`. Five
candidate configurations — three for Lexend Deca, two for Bitter — are built as separately
named SD font families so the user can switch between them on the same book from the device
menu without flashing. **The two families take different settings**, established by
measurement, so the thresholds must be per-family rather than global. Only after the
hardware verdict do the built-in fonts change. Two independent renderer defects
(superscript/subscript resampling, small-caps resampling) are fixed in C++ alongside.

**Tech Stack:** Python 3 (freetype-py 2.5.1, fontTools), C++17 (ESP-IDF / Arduino-ESP32),
PlatformIO, googletest.

## Global Constraints

- Firmware must stay under **6,553,600 bytes** (`partitions.csv`, `app0`). Current:
  6,329,792 B — 223,808 B free. `pio run -e default` enforces this via
  `scripts/check_firmware_size.py`.
- ESP32-C3 targets (X3/X4): single core @160 MHz, **no PSRAM**, ~380 KB usable internal
  RAM, **one** framebuffer. Keep shared code C3-safe.
- **Four grey levels is a hardware ceiling.** Two controller RAM planes index a four-cell
  LUT. Do not attempt more levels or deeper glyph bitmaps.
- No new Python dependency. `lib/EpdFont/scripts/requirements.txt` is
  `fonttools>=4.62.1`, `freetype-py>=2.5.1`, `pyyaml>=6.0.3`.
- **There is no pytest in this repo and CI does not run one.** Python-side checks are
  `--self-test` subcommands runnable with plain `python3`. C++ checks go in `test/` as
  googletest, run via ctest (not `pio run -t unit-tests`, which is not wired up).
- Do not edit generated files. Do not commit unless explicitly asked.
- Branch prefixes: `feat/ fix/ docs/ refactor/ test/ chore/`. Commits: `<type>: <summary>`.
- Every user-facing change needs a `CHANGELOG.md` entry, grouped under Added / Changed /
  Fixed, written as a user-facing outcome.
- **Scope for execution now: Lexend Deca only.** Bitter's setting is settled by
  measurement (V8: one flag, no weight change, stem spread 0.33 -> 0.00) and needs no
  human judgement, so it ships as a follow-up without another hardware round.
- **Overall scope: Lexend Deca and Bitter only.** These are also the only reading families compiled
  into the firmware (`builtinFonts/all.h`); charein is generated but never included.
- Chosen configurations, measured 2026-08-08 — **they differ per family, deliberately**:

  | family | thresholds | weight | autohint | rationale |
  |---|---|---|---|---|
  | Lexend Deca | `5,6,10` | 450 | yes | V9: fidelity 24.8 → 17.4, more ink than today |
  | Bitter | `3,6,10` (unchanged) | 500 (unchanged) | yes | V8: fidelity 29.7 → 17.4, **stem spread 0.33 → 0.00** |

  Bitter must **not** take Lexend Deca's thresholds: raising the white cutoff drops its
  thin slab serifs a level and re-creates the `k`-narrower-than-`l` defect from `052f497b`
  that the autohinter otherwise removes.

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `lib/EpdFont/scripts/fontconvert.py` | built-in font conversion | add `--aa-thresholds`, keep `--darken-aa` as alias |
| `lib/EpdFont/scripts/fontconvert_sdcard.py` | SD `.cpfont` conversion | same argument, same alias |
| `lib/EpdFont/scripts/build-sd-fonts.py` | YAML-driven SD build | forward per-family `aa_thresholds` |
| `lib/EpdFont/scripts/sd-fonts.yaml` | SD family catalogue | add five temporary A/B families; then per-family settings for Lexend Deca and Bitter |
| `lib/EpdFont/scripts/convert-builtin-fonts.sh` | built-in generation | make render args **per-family** after the verdict |
| `lib/GfxRenderer/GfxRenderer.cpp` | glyph blitting | fix `renderCharScaled`, fix `renderCharSmallCaps` |
| `test/font_quantization/` | new googletest suite | assert the small-caps averaging |

Tasks 1–4 are the A/B track and must land in order. Tasks 5–6 are the renderer fixes and
are fully independent — they can be done in any order, before or after. Task 7 is gated on
the hardware verdict from Task 4.

---

### Task 1: Add `--aa-thresholds` to both converters

The threshold ladder is currently a boolean choice between two hard-coded tuples. The whole
point of the change is tuning the three cutoffs independently, so the argument must accept
all three.

**Files:**
- Modify: `lib/EpdFont/scripts/fontconvert.py:27` (argparse), `:39` (tuple)
- Modify: `lib/EpdFont/scripts/fontconvert_sdcard.py:613` (signature), `:622` (tuple), `:907` (signature), `:934` (call), `:1029` (argparse), `:1180` (call)

**Interfaces:**
- Consumes: nothing.
- Produces: CLI `--aa-thresholds W,L,B` on both scripts, where `W`, `L`, `B` are ints in
  `0..15`, strictly increasing. `--darken-aa` remains valid and means `3,6,10`. Default
  when neither is given stays `4,8,12`. Also produces the module-level helper
  `parse_aa_thresholds(text: str) -> tuple[int, int, int]` in **both** scripts (they share
  no module today; duplicating a six-line validator is preferable to inventing an import
  path between two standalone scripts).

- [ ] **Step 1: Write the failing self-test in `fontconvert.py`**

Add near the bottom of `lib/EpdFont/scripts/fontconvert.py`, before the main body runs:

```python
def parse_aa_thresholds(text):
    """Parse 'W,L,B' into a strictly increasing 3-tuple of 4-bit cutoffs.

    Values are compared against a 4-bit coverage value (alpha >> 4), so they must
    be 0..15. Strictly increasing is not pedantry: an out-of-order ladder makes an
    unreachable level, which would silently drop a grey from every glyph.
    """
    parts = text.split(",")
    if len(parts) != 3:
        raise argparse.ArgumentTypeError(f"expected W,L,B — got {text!r}")
    try:
        values = tuple(int(p) for p in parts)
    except ValueError:
        raise argparse.ArgumentTypeError(f"non-integer threshold in {text!r}")
    if not all(0 <= v <= 15 for v in values):
        raise argparse.ArgumentTypeError(f"thresholds must be 0..15 — got {values}")
    if not values[0] < values[1] < values[2]:
        raise argparse.ArgumentTypeError(f"thresholds must increase — got {values}")
    return values


def _self_test():
    assert parse_aa_thresholds("4,8,12") == (4, 8, 12)
    assert parse_aa_thresholds("5,6,10") == (5, 6, 10)
    for bad in ("1,2", "1,2,3,4", "a,b,c", "0,8,16", "8,4,12", "4,4,12"):
        try:
            parse_aa_thresholds(bad)
        except argparse.ArgumentTypeError:
            continue
        raise AssertionError(f"expected rejection for {bad!r}")
    print("fontconvert self-test OK")
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 -c "import sys; sys.argv=['x','--self-test']; exec(open('lib/EpdFont/scripts/fontconvert.py').read())"`

Expected: FAIL — `--self-test` is not a recognised argument yet.

- [ ] **Step 3: Wire the arguments**

Replace `lib/EpdFont/scripts/fontconvert.py:27` (the `--darken-aa` line) with:

```python
parser.add_argument("--darken-aa", dest="darken_aa", action="store_true", help="Alias for --aa-thresholds 3,6,10.")
parser.add_argument("--aa-thresholds", dest="aa_thresholds", type=parse_aa_thresholds, default=None,
                    help="2-bit anti-aliasing cutoffs as W,L,B (4-bit coverage, increasing). Default 4,8,12.")
parser.add_argument("--self-test", action="store_true", help="Run internal checks and exit.")
```

Replace `:39` with:

```python
if args.aa_thresholds is not None:
    aa_thresholds = args.aa_thresholds
elif args.darken_aa:
    aa_thresholds = (3, 6, 10)
else:
    aa_thresholds = (4, 8, 12)
```

Immediately after `args = parser.parse_args()`, add:

```python
if args.self_test:
    _self_test()
    sys.exit(0)
```

Note: `parse_aa_thresholds` and `_self_test` must be defined **above** the
`parser.add_argument` calls, because argparse resolves `type=` at call time.

- [ ] **Step 4: Run it to verify it passes**

Run: `python3 lib/EpdFont/scripts/fontconvert.py --self-test x 12 x.ttf`
Expected: `fontconvert self-test OK`, exit 0.

- [ ] **Step 5: Verify the alias produces byte-identical output**

This is the regression that matters: existing invocations must not change.

```bash
cd lib/EpdFont/scripts
python3 fontconvert.py t 12 ../builtinFonts/source/LexendDeca/LexendDeca-Regular.ttf \
  --2bit --compress --pnum --darken-aa > /tmp/aa_alias_a.h
python3 fontconvert.py t 12 ../builtinFonts/source/LexendDeca/LexendDeca-Regular.ttf \
  --2bit --compress --pnum --aa-thresholds 3,6,10 > /tmp/aa_alias_b.h
diff /tmp/aa_alias_a.h /tmp/aa_alias_b.h && echo "IDENTICAL"
```

Expected: `IDENTICAL`.

- [ ] **Step 6: Mirror all of the above into `fontconvert_sdcard.py`**

Same `parse_aa_thresholds` and `_self_test` (rename the print to
`"fontconvert_sdcard self-test OK"`). Then:

- `:613` — change `def rasterize_font_style(..., darken_aa=False)` to
  `def rasterize_font_style(..., aa_thresholds=(4, 8, 12))`
- `:622` — delete `aa_thresholds = (3, 6, 10) if darken_aa else (4, 8, 12)`; the value now
  arrives as a parameter
- `:907` — change `build_cpfont(..., darken_aa=False)` to
  `build_cpfont(..., aa_thresholds=(4, 8, 12))`
- `:934` — change `darken_aa=darken_aa` to `aa_thresholds=aa_thresholds`
- `:1029` — add the same two argparse lines as Step 3
- `:1180` — change `darken_aa=args.darken_aa` to resolve the same three-way precedence

- [ ] **Step 7: Run the SD self-test and alias check**

Run: `python3 lib/EpdFont/scripts/fontconvert_sdcard.py --self-test`
Expected: `fontconvert_sdcard self-test OK`.

- [ ] **Step 8: Commit**

```bash
git add lib/EpdFont/scripts/fontconvert.py lib/EpdFont/scripts/fontconvert_sdcard.py
git commit -m "feat: allow the three anti-aliasing thresholds to be set independently"
```

---

### Task 2: Forward per-family thresholds through `build-sd-fonts.py`

**Files:**
- Modify: `lib/EpdFont/scripts/build-sd-fonts.py:377-384`

**Interfaces:**
- Consumes: `--aa-thresholds` from Task 1.
- Produces: an optional `aa_thresholds: "W,L,B"` key on a family in `sd-fonts.yaml`. When
  absent, behaviour is byte-identical to today (`--darken-aa`).

- [ ] **Step 1: Replace the unconditional `--darken-aa` append**

At `lib/EpdFont/scripts/build-sd-fonts.py:380-384`, replace:

```python
    # SD-card fonts are reader fonts, so they get the same darkened anti-alias
    # thresholds as the built-in reader fonts in convert-builtin-fonts.sh
    # (READING_FONT_RENDER_ARGS). Without this the two look noticeably
    # different at the same size on the same panel.
    cmd.append("--darken-aa")
```

with:

```python
    # SD-card fonts are reader fonts, so by default they get the same darkened
    # anti-alias thresholds as the built-in reader fonts in
    # convert-builtin-fonts.sh (READING_FONT_RENDER_ARGS). Without this the two
    # look noticeably different at the same size on the same panel. A family may
    # override the ladder outright, which is how A/B candidates are built.
    aa_thresholds = family.get("aa_thresholds")
    if aa_thresholds:
        cmd.extend(["--aa-thresholds", str(aa_thresholds)])
    else:
        cmd.append("--darken-aa")
```

- [ ] **Step 2: Verify the default path is unchanged**

Run: `python3 lib/EpdFont/scripts/build-sd-fonts.py --only "Lexend Deca" --output-dir /tmp/sdfonts-baseline --no-package -j 1 --verbose 2>&1 | grep -o -- "--darken-aa"`

Expected: `--darken-aa` appears in the child command line, exactly as before.

- [ ] **Step 3: Commit**

```bash
git add lib/EpdFont/scripts/build-sd-fonts.py
git commit -m "feat: let an SD font family override its anti-aliasing thresholds"
```

---

### Task 3: Build the five A/B candidates as named SD families

The user reads the SD build of Lexend Deca (`/.fonts/Lexend Deca/`, sizes 8–20), so the
A/B rides the path they actually use. Three families, distinguishable in the device menu.

**Files:**
- Modify: `lib/EpdFont/scripts/sd-fonts.yaml` (append after the `Lexend Deca` entry at :337-359)

**Interfaces:**
- Consumes: `aa_thresholds` from Task 2, `force_autohint` (already supported at :377).
- Produces: three `.cpfont` family folders named `AA V0 Actuel`, `AA V8 Autohint`,
  `AA V9 Retenu`.

- [ ] **Step 1: Append the three candidate families**

The A/B is only meaningful if the three differ in exactly the intended dimensions, so every
other key is copied verbatim from the shipping `Lexend Deca` entry. Sizes are cut to
`[12, 14]` — the user's reading sizes — because building all eight triples the wait for no
extra information.

```yaml
  # --- Temporary A/B candidates for the anti-aliasing recalibration. ---
  # See docs/superpowers/specs/2026-08-08-text-aa-contrast-design.md.
  # DELETE all three once the hardware verdict is in; they must never ship.
  - name: AA V0 Actuel
    description: "A/B baseline - current settings"
    languages: "Latin"
    intervals: latin-ext
    sizes: [12, 14]
    aa_thresholds: "3,6,10"
    styles:
      regular:
        {
          url: "https://raw.githubusercontent.com/google/fonts/main/ofl/lexenddeca/LexendDeca%5Bwght%5D.ttf",
          variable: { wght: 400 },
        }
      bold:
        {
          url: "https://raw.githubusercontent.com/google/fonts/main/ofl/lexenddeca/LexendDeca%5Bwght%5D.ttf",
          variable: { wght: 700 },
        }

  - name: AA V8 Autohint
    description: "A/B candidate - autohint only"
    languages: "Latin"
    intervals: latin-ext
    sizes: [12, 14]
    aa_thresholds: "3,6,10"
    force_autohint: true
    styles:
      regular:
        {
          url: "https://raw.githubusercontent.com/google/fonts/main/ofl/lexenddeca/LexendDeca%5Bwght%5D.ttf",
          variable: { wght: 400 },
        }
      bold:
        {
          url: "https://raw.githubusercontent.com/google/fonts/main/ofl/lexenddeca/LexendDeca%5Bwght%5D.ttf",
          variable: { wght: 700 },
        }

  - name: AA V9 Retenu
    description: "A/B candidate - thresholds + weight + autohint"
    languages: "Latin"
    intervals: latin-ext
    sizes: [12, 14]
    aa_thresholds: "5,6,10"
    force_autohint: true
    styles:
      regular:
        {
          url: "https://raw.githubusercontent.com/google/fonts/main/ofl/lexenddeca/LexendDeca%5Bwght%5D.ttf",
          variable: { wght: 450 },
        }
      bold:
        {
          url: "https://raw.githubusercontent.com/google/fonts/main/ofl/lexenddeca/LexendDeca%5Bwght%5D.ttf",
          variable: { wght: 700 },
        }

  # Bitter takes a DIFFERENT setting: autohint only. Its baseline weight is 500,
  # not 400 (cff54d77), and V9's thresholds re-create the stem defect autohint
  # fixes. Everything except force_autohint is copied from the shipping Bitter
  # entry at sd-fonts.yaml:39-60 so the pair differs in exactly one dimension.
  - name: AA Bitter V0
    description: "A/B baseline - Bitter as shipped"
    languages: "Latin, Cyrillic"
    intervals: latin-ext,cyrillic
    sizes: [12, 14]
    aa_thresholds: "3,6,10"
    styles:
      regular:
        {
          url: "https://raw.githubusercontent.com/google/fonts/main/ofl/bitter/Bitter%5Bwght%5D.ttf",
          variable: { wght: 500 },
        }
      bold:
        {
          url: "https://raw.githubusercontent.com/google/fonts/main/ofl/bitter/Bitter%5Bwght%5D.ttf",
          variable: { wght: 700 },
        }

  - name: AA Bitter V8
    description: "A/B candidate - Bitter with autohint"
    languages: "Latin, Cyrillic"
    intervals: latin-ext,cyrillic
    sizes: [12, 14]
    aa_thresholds: "3,6,10"
    force_autohint: true
    styles:
      regular:
        {
          url: "https://raw.githubusercontent.com/google/fonts/main/ofl/bitter/Bitter%5Bwght%5D.ttf",
          variable: { wght: 500 },
        }
      bold:
        {
          url: "https://raw.githubusercontent.com/google/fonts/main/ofl/bitter/Bitter%5Bwght%5D.ttf",
          variable: { wght: 700 },
        }
```

- [ ] **Step 2: Build the three families**

```bash
python3 lib/EpdFont/scripts/build-sd-fonts.py \
  --only "Lexend Deca AA,Lexend Deca AA+" \
  --output-dir /tmp/aa-ab --no-package -j 2 --verbose
```

Expected: five folders under `/tmp/aa-ab/`, each with `*_12.cpfont` and `*_14.cpfont`.
Requires network (Google Fonts). Budget a few minutes.

- [ ] **Step 3: Verify the three actually differ**

A silent no-op here would waste a whole hardware session, so check before installing.

```bash
for f in "AA V0 Actuel" "AA V8 Autohint" "AA V9 Retenu" "AA Bitter V0" "AA Bitter V8"; do
  echo "$f: $(md5 -q "/tmp/aa-ab/$f/${f}_14.cpfont" 2>/dev/null || md5sum "/tmp/aa-ab/$f/${f}_14.cpfont")"
done
```

Expected: five **different** hashes, all files non-empty. The two Bitter hashes
differing is the load-bearing check — they vary only by `force_autohint`.

- [ ] **Step 4: Install to the SD card**

```bash
cp -R /tmp/aa-ab/AA\ * /Users/aurelien-edusign/Code/xteink/SD/.fonts/
ls /Users/aurelien-edusign/Code/xteink/SD/.fonts/
```

Expected: the five new folders alongside the existing families.

- [ ] **Step 5: Commit the YAML only**

```bash
git add lib/EpdFont/scripts/sd-fonts.yaml
git commit -m "chore: add temporary anti-aliasing A/B font families"
```

---

### Task 4: Hardware A/B on the X4 — **user step, blocking**

Nothing downstream is decided until this returns a verdict. The tier-1 comparator ranks
edge structure; it cannot rank perceived darkness on e-ink, and the simulator cannot
either (it approximates the greys as 200/96).

- [ ] **Step 1: Eject the card and boot the X4**

Insert the SD card, power on, open a Lexend Deca book at 12 pt.

- [ ] **Step 2: Cycle the three families on the same page**

Settings → Reader → Font Options → Font Family. Switch **without turning the page**, so
the same text is re-rendered each time.

Two independent comparisons, judged separately:

- **Lexend Deca:** `AA V0 Actuel` → `AA V8 Autohint` → `AA V9 Retenu`. V8 isolates the
  autohint contribution; V9 adds the thresholds and the weight on top.
- **Bitter:** `AA Bitter V0` → `AA Bitter V8`. One flag apart. Look specifically at whether
  the `k` still reads narrower than the `l` and `h` in V0 and stops doing so in V8 — that
  is the `052f497b` defect, measured at 0.33 px today.

Expected on device: all five appear in the family list. If they do not, check `/.fonts/`
casing and that each folder contains its `.cpfont` files.

- [ ] **Step 3: Judge, in daylight and in dim light**

For Lexend Deca, record whether V9 reads too light or too heavy against V0; if V8 and V9
are indistinguishable, prefer V8 — one flag instead of three changes. For Bitter, record
whether V8's stem regularity is visible at reading distance or only under magnification.

- [ ] **Step 4: Record the verdict in the spec**

Append the outcome to
`docs/superpowers/specs/2026-08-08-text-aa-contrast-design.md` under "Selected", naming
the winner and the lighting conditions judged.

---

### Task 5: Fix `renderCharScaled` to honour the render mode

Superscripts, subscripts and Japanese ruby are drawn through a 50 % resampler that receives
`renderMode` and never reads it, thresholding to binary instead. That text renders solid
black with no anti-aliasing. It does **not** corrupt the grey planes: in a grey pass it
calls `drawPixel(state=true)`, which clears a bit `clearScreen(0x00)` already left at zero.

Independent of Tasks 1–4.

**Files:**
- Modify: `lib/GfxRenderer/GfxRenderer.cpp:691-712` (2-bit branch), `:713-736` (1-bit branch)

**Interfaces:**
- Consumes: `draw2BitFontPixel(renderer, renderMode, x, y, raw, pixelState)` —
  `GfxRenderer.cpp:55-66`. Takes the **font** raw value (0=white … 3=black) and derives
  `bmpVal = 3 - raw` internally.
- Produces: nothing new.

- [ ] **Step 1: Replace the 2-bit branch's binary threshold with an area average**

At `lib/GfxRenderer/GfxRenderer.cpp:691-712`, replace the inner accumulation and the
`if (maxRaw >= 2 || coverage >= 2)` test with:

```cpp
        uint8_t coverage = 0;
        uint8_t samples = 0;
        for (int sampleY = 0; sampleY < 2 && srcY + sampleY < srcH; sampleY++) {
          for (int sampleX = 0; sampleX < 2 && srcX + sampleX < srcW; sampleX++) {
            const int pos = (srcY + sampleY) * srcW + srcX + sampleX;
            const uint8_t byte = bitmap[pos >> 2];
            coverage += (byte >> ((3 - (pos & 3)) * 2)) & 0x3;
            samples++;
          }
        }
        if (samples == 0) continue;
        // Area-weighted, rounded: a 2x2 box filter is the correct downsample for
        // coverage values. The previous max/threshold test discarded the grey
        // levels entirely, which is why sup/sub rendered solid black.
        const uint8_t avgRaw = static_cast<uint8_t>((coverage + samples / 2) / samples);
        draw2BitFontPixel(renderer, renderMode, baseX + dstX, baseY + dstY, avgRaw, pixelState);
```

Delete the now-unused `uint8_t maxRaw = 0;` declaration.

- [ ] **Step 2: Gate the 1-bit branch on BW**

1-bit fonts carry no grey, so they must contribute nothing to the grey planes. At
`:713-736`, change the draw to:

```cpp
        if (hasInk && renderMode == GfxRenderer::BW) {
          renderer.drawPixel(baseX + dstX, baseY + dstY, pixelState);
        }
```

- [ ] **Step 3: Format and build**

```bash
./bin/clang-format-fix
pio run -e simulator
```

Expected: clean build, no clang-format diff.

- [ ] **Step 4: Verify visually in the simulator**

`test/epubs/test_supsub.epub` exists as a fixture.

```bash
pio run -e simulator-X3
mkdir -p ./qa-artifacts
CROSSPOINT_SIM_INPUT_SCRIPT='900:DOWN;1250:ENTER;3000:QUIT' \
CROSSPOINT_SIM_SCREENSHOTS='2400:./qa-artifacts/supsub.bmp' \
  .pio/build/simulator-X3/program
```

Expected: superscripts show grey edge pixels rather than hard black stair-steps.

- [ ] **Step 5: Commit**

```bash
git add lib/GfxRenderer/GfxRenderer.cpp
git commit -m "fix: anti-alias superscript, subscript and ruby text"
```

---

### Task 6: Replace the small-caps MAX filter with an area average

`renderCharSmallCaps` resamples to 75 % by taking the **maximum** raw value in each source
footprint. Every edge pixel is biased to the darkest sample in its window, so small caps
render systematically heavier and blockier than the same glyphs at native size. The loop
already visits every source pixel, so averaging costs nothing extra.

Independent of every other task.

**Files:**
- Modify: `lib/GfxRenderer/GfxRenderer.cpp:764-772`
- Create: `test/font_quantization/SmallCapsResampleTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `scaled75SourceEnd(dst, srcLimit)` — `GfxRenderer.cpp:50-53`.
- Produces: free function `uint8_t smallCapsResample(const uint8_t* raw, int srcW, int srcH, int dstX, int dstY)`
  in an anonymous namespace, extracted so it is testable on the host. Returns the rounded
  area-weighted mean raw value (0..3) of the source footprint, 0 when the footprint is
  empty.

- [ ] **Step 1: Write the failing test**

Create `test/font_quantization/SmallCapsResampleTest.cpp`:

```cpp
#include <gtest/gtest.h>
#include "SmallCapsResample.h"

TEST(SmallCapsResample, AveragesRatherThanTakingMax) {
  // A 4x4 source where one sample is black (3) and the rest are white (0).
  // MAX would report 3 (solid black); the area average reports 1.
  const uint8_t src[16] = {3, 0, 0, 0,
                           0, 0, 0, 0,
                           0, 0, 0, 0,
                           0, 0, 0, 0};
  EXPECT_EQ(smallCapsResample(src, 4, 4, 0, 0), 1);
}

TEST(SmallCapsResample, SolidBlackStaysBlack) {
  uint8_t src[16];
  for (uint8_t& v : src) v = 3;
  EXPECT_EQ(smallCapsResample(src, 4, 4, 0, 0), 3);
}

TEST(SmallCapsResample, EmptyStaysWhite) {
  const uint8_t src[16] = {0};
  EXPECT_EQ(smallCapsResample(src, 4, 4, 1, 1), 0);
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release && cmake --build build/test
```

Expected: FAIL — `SmallCapsResample.h` does not exist.

- [ ] **Step 3: Extract the helper**

Create `lib/GfxRenderer/SmallCapsResample.h`:

```cpp
#pragma once
#include <stdint.h>

// 75% downsample of one destination pixel from a 2bpp source grid of raw
// coverage levels (0=white .. 3=black). Area-weighted mean, not max: taking the
// max biases every edge pixel toward the darkest sample in its window, which
// makes small caps render heavier and blockier than the same glyphs at native
// size. Shared with the host test suite.
inline int smallCapsSourceEnd(const int dst, const int srcLimit) {
  const int srcStart = dst * 4 / 3;
  const int lo = srcStart + 1;
  const int hi = ((dst + 1) * 4 + 2) / 3;
  return srcLimit < (lo > hi ? lo : hi) ? srcLimit : (lo > hi ? lo : hi);
}

inline uint8_t smallCapsResample(const uint8_t* raw, const int srcW, const int srcH, const int dstX,
                                 const int dstY) {
  const int y0 = dstY * 4 / 3;
  const int y1 = smallCapsSourceEnd(dstY, srcH);
  const int x0 = dstX * 4 / 3;
  const int x1 = smallCapsSourceEnd(dstX, srcW);
  uint32_t total = 0;
  uint32_t samples = 0;
  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) {
      total += raw[y * srcW + x];
      samples++;
    }
  }
  if (samples == 0) return 0;
  return static_cast<uint8_t>((total + samples / 2) / samples);
}
```

- [ ] **Step 4: Register the suite**

Add to `test/CMakeLists.txt`, following the pattern of the existing
`differential_rounding` suite:

```cmake
add_crossink_test(font_quantization font_quantization/SmallCapsResampleTest.cpp)
```

Read the file first and match the existing macro name and argument order exactly — do not
assume `add_crossink_test` is what it is called.

- [ ] **Step 5: Run to verify it passes**

```bash
cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release && cmake --build build/test
ctest --test-dir build/test --output-on-failure -R SmallCapsResample
```

Expected: 3 tests pass.

- [ ] **Step 6: Use the helper in the renderer**

In `lib/GfxRenderer/GfxRenderer.cpp`, add `#include "SmallCapsResample.h"` alongside the
existing includes, then replace the max accumulation at `:764-772` with:

```cpp
        const uint8_t avgRaw = smallCapsResampleFromPacked(bitmap, srcW, srcH, dstX, dstY);
        draw2BitFontPixel(renderer, renderMode, baseX + dstX, baseY + dstY, avgRaw, pixelState);
```

The glyph bitmap is 2bpp-packed, not one byte per pixel, so add a packed-source wrapper to
`SmallCapsResample.h`:

```cpp
// Same filter, reading the device's packed 2bpp glyph format (4 px/byte, MSB first).
inline uint8_t smallCapsResampleFromPacked(const uint8_t* bitmap, const int srcW, const int srcH,
                                           const int dstX, const int dstY) {
  const int y0 = dstY * 4 / 3;
  const int y1 = smallCapsSourceEnd(dstY, srcH);
  const int x0 = dstX * 4 / 3;
  const int x1 = smallCapsSourceEnd(dstX, srcW);
  uint32_t total = 0;
  uint32_t samples = 0;
  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) {
      const int pos = y * srcW + x;
      total += (bitmap[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3;
      samples++;
    }
  }
  if (samples == 0) return 0;
  return static_cast<uint8_t>((total + samples / 2) / samples);
}
```

Delete the now-unused `scaled75SourceEnd` at `:50-53` only if nothing else references it;
grep first.

- [ ] **Step 7: Format, build, test**

```bash
./bin/clang-format-fix
git diff --exit-code
pio run -e simulator && pio run -e default
ctest --test-dir build/test --output-on-failure
```

Expected: no format diff, both builds pass, firmware under the size limit, all tests green.

- [ ] **Step 8: Commit**

```bash
git add lib/GfxRenderer/SmallCapsResample.h lib/GfxRenderer/GfxRenderer.cpp \
        test/font_quantization/SmallCapsResampleTest.cpp test/CMakeLists.txt
git commit -m "fix: small caps no longer render heavier than native-size glyphs"
```

---

### Task 7: Apply the winning configuration to the built-in fonts — **gated on Task 4**

Do not start this until Task 4 has produced a verdict. Regenerating built-in fonts changes
every reading font's SHA-256-derived ID (`build-font-ids.sh`), which is written into every
book's section cache (`lib/Epub/Epub/Section.cpp:214,225`) — **every book on the card
re-indexes on first open afterwards.**

**Files:**
- Modify: `lib/EpdFont/scripts/convert-builtin-fonts.sh:149`
- Modify: `lib/EpdFont/scripts/sd-fonts.yaml` (winning ladder for all reader families; delete the three A/B families)
- Regenerate: `lib/EpdFont/builtinFonts/*.h`, `src/fontIds.h`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Make the built-in render arguments per-family**

`READING_FONT_RENDER_ARGS` at `lib/EpdFont/scripts/convert-builtin-fonts.sh:149` is global,
but the two families need different ladders. Replace the single array with a selector, and
have `generate_family` consult it by family slug.

```sh
# Anti-aliasing settings are per-family by measurement, not by preference.
# Bitter's thin slab serifs drop a level under Lexend Deca's raised white cutoff,
# which re-creates the k-narrower-than-l defect from 052f497b. See
# docs/superpowers/specs/2026-08-08-text-aa-contrast-design.md.
READING_FONT_RENDER_ARGS_COMMON=(--2bit --compress --pnum)

reading_font_render_args() {
  case "$1" in
    lexenddeca) echo "--aa-thresholds 5,6,10 --force-autohint" ;;
    bitter)     echo "--aa-thresholds 3,6,10 --force-autohint" ;;
    *)          echo "--darken-aa" ;;   # charein: generated, never compiled in
  esac
}
```

Then inside `generate_family` (whose first positional argument is the family slug — read
the function before editing and match its actual parameter names), replace the use of
`"${READING_FONT_RENDER_ARGS[@]}"` with:

```sh
  local family_aa_args
  read -r -a family_aa_args <<< "$(reading_font_render_args "$1")"
  # ... then use "${READING_FONT_RENDER_ARGS_COMMON[@]}" "${family_aa_args[@]}"
```

**Blocking asymmetry — do not paper over it.** The built-in LexendDeca and Bitter sources
are **static** TTFs (`lib/EpdFont/builtinFonts/source/`), so:

- **Bitter's setting applies cleanly.** V8 changes only `--force-autohint`; weight is
  untouched. Ship it.
- **Lexend Deca's does not.** V9 needs `wght: 450`, and a static Regular cannot be
  re-weighted. Shipping only its thresholds would give built-in Lexend Deca a *different*
  rendering from SD Lexend Deca — precisely the divergence commit `590d12bf` set out to
  eliminate. Either replace
  `lib/EpdFont/builtinFonts/source/LexendDeca/LexendDeca-Regular.ttf` with a 450 instance
  cut from `LexendDeca[wght].ttf` (mirroring what `build-sd-fonts.py:241-295` already does
  for SD), or leave built-in Lexend Deca at `--darken-aa` and record why. **Do not ship
  half of V9.**

- [ ] **Step 2: Regenerate, taking the known breakage into account**

`convert-builtin-fonts.sh` references a `source/IBMPlexSansHebrew/` directory that does not
exist, and fails at the UI-font stage. The reading-font stage has every source it needs.

```bash
cd lib/EpdFont/scripts && ./convert-builtin-fonts.sh
```

Expected: reading fonts regenerate (~3–5 min); the UI stage fails. That failure is
pre-existing and must not be "fixed" by regenerating UI fonts with new settings — UI fonts
are 1-bit and have no anti-aliasing at all.

- [ ] **Step 3: Rebuild font IDs and verify compression**

```bash
lib/EpdFont/scripts/build-font-ids.sh > src/fontIds.h
python3 lib/EpdFont/scripts/verify_compression.py lib/EpdFont/builtinFonts/
```

Expected: verification passes.

- [ ] **Step 4: Check the size budget**

```bash
pio run -e default
```

Expected: `Firmware image fits OTA app partition: N <= 6553600`. The measured cost of a
threshold change is ≈ +49 B per font (+0.33 %), so ≈ +5 KB total against 219 KB free — but
verify, do not assume.

- [ ] **Step 5: Apply the same ladder to the SD families and delete the A/B entries**

In `lib/EpdFont/scripts/sd-fonts.yaml`, remove the five `AA *` families added in Task 3.
Then apply the winning per-family settings to the two in-scope families only, so built-in
and SD text keep matching:

- `Lexend Deca` (:337-359): add `aa_thresholds: "5,6,10"` and `force_autohint: true`;
  change `regular` to `variable: { wght: 450 }`.
- `Bitter` (:39-60): add `force_autohint: true`. Leave its thresholds and its `wght: 500`
  exactly as they are.

**Leave the other 22 SD families untouched.** Scope is Lexend Deca and Bitter; nothing was
measured for the rest, and applying an unmeasured ladder to a font whose stroke contrast
was never examined is how the `052f497b` defect got shipped in the first place.

- [ ] **Step 6: Add the CHANGELOG entry**

Under `### Changed` in the current unreleased section of `CHANGELOG.md`:

```markdown
- Reader text now renders with cleaner glyph edges: the anti-aliasing thresholds were
  recalibrated against the panel's measured grey levels and font stems are aligned to the
  pixel grid. Books re-index once on first open after this update.
```

- [ ] **Step 7: Full verification**

```bash
./bin/clang-format-fix && git diff --exit-code
pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run -e simulator && pio run -e default
python3 scripts/run_simulator_smoke_test.py --env simulator --theme classic
```

- [ ] **Step 8: Hardware verification**

Flash X3 and X4. Open a book at 12 pt. Expect a one-time re-index per book. Compare against
the photographs taken before the change. If output looks stale, clear the affected
`.crosspoint/epub_<hash>/` cache.

- [ ] **Step 9: Commit**

```bash
git add lib/EpdFont/scripts/convert-builtin-fonts.sh lib/EpdFont/scripts/sd-fonts.yaml \
        lib/EpdFont/builtinFonts src/fontIds.h CHANGELOG.md
git commit -m "feat: recalibrate reader font anti-aliasing against the panel's grey levels"
```

---

## Self-Review

**Spec coverage.** Tier-1 comparator — already built (`preview_aa.py`), not re-planned.
Threshold decoupling — Task 1. Per-family override — Task 2. V0/V8/V9 hardware A/B —
Tasks 3–4. `renderCharScaled` — Task 5. `renderCharSmallCaps` — Task 6. Built-in
regeneration, cache-invalidation warning, CHANGELOG — Task 7. Acceptance criteria are
exercised by Task 4 (eye) and `preview_aa.py` (numbers).

**Gap accepted deliberately:** the spec's non-goal list excludes the UC8279d dead-grey
hypothesis and all waveform work; no task covers them, by design.

**Gap surfaced, not silently dropped:** V9's `wght: 450` cannot reach the built-in fonts
without swapping their static sources for variable ones. Task 7 Step 1 raises this
explicitly instead of shipping half the configuration.

**Type consistency.** `parse_aa_thresholds` returns a 3-tuple in both converters;
`aa_thresholds` is the parameter name through `rasterize_font_style` and `build_cpfont`;
`smallCapsResample` (unpacked, host-testable) and `smallCapsResampleFromPacked` (device)
are distinct names for distinct input formats, both defined in Task 6.

**Unverified assumption flagged in-plan:** Task 6 Step 4 assumes a registration macro in
`test/CMakeLists.txt` whose exact name I did not confirm; the step instructs the
implementer to read the file and match rather than paste blindly.

---

## Execution log — 2026-08-08, Tasks 1-3

Scope executed: **Lexend Deca only**. Bitter deferred (its setting is settled by
measurement and needs no human judgement).

### Verified

- `fontconvert.py` and `fontconvert_sdcard.py` both gained `--aa-thresholds W,L,B`,
  `--self-test`, and keep `--darken-aa` as an alias for `3,6,10`. Both self-tests pass.
- **Refactor proven neutral.** `--darken-aa` vs `--aa-thresholds 3,6,10`:
  - `fontconvert.py`: generated headers identical apart from the `Command used:`
    provenance line, which *should* differ — it records the actual invocation.
    Data sha (excluding that line): `7c77618b341f`, both.
  - `fontconvert_sdcard.py`: `.cpfont` **byte-identical**, sha `2a50aeefe1061a99`.
  - Control: `--aa-thresholds 5,6,10` differs from both, so the argument is live.
- Three families built with **identical coverage** (1634 glyphs / 55 intervals) and
  distinct content, at sizes 10/12/14/16, all four styles:
  `Lexend Deca REF` (691,546 B), `Lexend Deca AA` (672,140 B), `Lexend Deca AA+` (672,738 B).

### Deviation 1 — wrong pipeline: the user runs the DICTIONARY font variant

The plan assumed `build-sd-fonts.py` was the right builder. It is not, for this user.

The coverage gap (2237 glyphs / 67 intervals installed, against 1634 / 55 from
`sd-fonts.yaml`) was first attributed to an older published config revision. **That was
wrong.** The user downloaded from the *Dictionary Fonts* section of
`https://inky.crossink.dev/#downloads`, built by `build-dictionary-fonts.py`, which
replaces every family's interval list with:

    reading,(0x0250-0x02FF),(0x1D00-0x1DBF),(0x1DC0-0x1DFF),(0x20D0-0x20FF),(0xFE20-0xFE2F),builtin

i.e. the broad `reading` preset plus IPA and combining marks. Candidates built from
`sd-fonts.yaml`'s `latin-ext` would have silently dropped IPA and combining-mark coverage
— visible as missing characters in dictionary definitions, and easily misread as an
anti-aliasing regression.

Resolution: rebuild all three candidates with `build-dictionary-fonts.py --only ...`. It
wraps the shared builder and deep-copies the catalog, replacing only `intervals`, so the
per-family `aa_thresholds`, `force_autohint` and `variable.wght` keys carry through
unchanged.

**Strongest available validation:** `Lexend Deca REF` built this way is **byte-identical to
the font already installed on the user's card, at all four sizes**. That confirms both that
the pipeline was reproduced exactly and that the `--aa-thresholds` refactor is neutral on a
full real font, not merely on a synthetic case.

**Anyone re-running this must use `build-dictionary-fonts.py`, not `build-sd-fonts.py`,
whenever the target user is on a dictionary-variant font.**

### Deviation 2 — `wght: 450` crashed the instancer

`extract_static_instance` (`build-sd-fonts.py:241-295`) passed `updateFontNames=True`,
which rewrites the name table from the font's STAT table and therefore only accepts axis
positions having a *named* instance. `wght 450` is a valid point on a continuous axis with
no name, so it raised `Cannot find Axis Values {'wght': 450}`.

Resolution: catch `ValueError` specifically and retry with `updateFontNames=False`, logging
why. The names are cosmetic here — the `.cpfont` family name comes from `--name`. Narrowing
to `ValueError` keeps genuine I/O failures propagating instead of being silently retried.

### Not done

- The three dictionary-variant families are staged in `~/Code/xteink/fonts/`. No SD card
  was mounted, so they are not yet on hardware.
- **Cleanup owed to the user, not performed:** an earlier pass copied three
  `sd-fonts.yaml`-built (wrong, reading-variant) folders into `~/Code/xteink/SD/.fonts/`
  — `Lexend Deca REF`, `Lexend Deca AA`, `Lexend Deca AA+`. They must be removed or they
  will show up on the device with reduced coverage. Deletion is the user's call; never run
  `rm` against that directory.
- Task 4 (hardware A/B) is the user's, and blocks Tasks 7.
- Tasks 5 and 6 (renderer defects) untouched; they are independent.
- Nothing committed.
