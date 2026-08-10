# CrossPoint Reader — Durable Context

Keep this file focused on repo-specific gotchas that are worth reusing in future sessions.

## Upstream Sync

Run `scripts/sync-upstream.sh`; it encodes everything below. Three repos are in play:

| Remote | Repo | Role |
| --- | --- | --- |
| `crosspoint` | `crosspoint-reader/crosspoint-reader` | Original project, upstream of CrossInk |
| `upstream` | `uxjulia/CrossInk` | What this fork actually tracks |
| `fork` | `oreglio/crosspoint-reader` | This fork (named after the original; it follows CrossInk) |

- Merge `upstream/development`, never `upstream/main` or a release tag. `main`
  receives one squashed commit per release, sharing no history with this fork, so
  merging it replays code already present here as conflicts. Measured on the
  v1.5.0 sync: **90 conflicting files from `main`, 2 from `development`**.
- A merge moves the `freeink-sdk` submodule pointer but leaves the checked-out
  copy behind. `pio run` then compiles the **old** SDK and still reports SUCCESS.
  Always `git submodule update --recursive` before trusting a post-merge build.
- Tag rule: a bare `vX.Y.Z` tag is CrossInk's. Crosspoint's own tags are the
  legacy `0.x.y` names, plus `crosspoint/*` for anything fetched from now on
  (`remote.crosspoint.tagOpt=--no-tags` with a namespaced refspec). Before this
  rule was set, bare `v1.5.0` pointed at Crosspoint's release, not CrossInk's,
  and `git fetch upstream --tags` failed with a "would clobber" rejection.
- Fork release numbering (`v1.5.36`, …) is this fork's OTA build counter on
  `oreglio/CrossInkLibrary` and does not track upstream's `v1.5.0`. The two are
  not comparable; `platformio.ini` carries the upstream-facing `version`.

## FreeInk SDK

Refer to https://freeink.org/llms.txt for guidance.

## Simulator

- Simulator patches belong in the adjacent `crossink-simulator` repo.
- The valid local simulator env in this repo is `simulator`, and `pio run -e simulator` currently builds cleanly.
- The simulator `PNGdec` stub in `crossink-simulator/src/PNGdec.h` needs to mirror the real API shape used by app code, including `hasAlpha()` and `getTransparentColor()`, even though decode still fails intentionally.
- Known simulator limits:
  - No image rendering: `platformio.ini` ignores `hal`, `PNGdec`, and `JPEGDEC`, so image decoders are intentionally absent.
  - JPEGDEC stub always fails; `JPEGDEC fallback: open failed (err=-1)` is expected in simulator.
  - `esp_deep_sleep_start()` is a no-op in simulator.
  - `HalStorage` uses POSIX file access under `./fs_` and allows multiple readers, unlike real hardware.

## Real Hardware / Storage

- SdFat on hardware allows only one open reader per file path at a time. If a fallback needs to reopen the same file, close the first handle before reopening.

## Rendering / Reader Pipeline

- `lib/Epub/Epub/Page.cpp`: images must render only in `GfxRenderer::BW`; grayscale passes are text anti-aliasing passes only.
- Kindle EPUBs may contain paired high-res and old-Kindle fallback images. `ChapterHtmlSlimParser` should skip `<img>` nodes with `data-AmznRemoved-M8` to avoid duplicate stacked images.
- After image/layout pipeline changes that affect cached EPUB output, clear the affected `.crosspoint/epub_<hash>/` cache if behavior looks stale.

## UI Consistency

- Use FreeInkUI SDK components and input routing for list-style screens where possible. Row rendering, touch targets,
  hit testing, and pagination should share the same FreeInkUI list configuration instead of custom touch scaling.

## Heap Baselines (X4 hardware, SD card font)

- A normal resume-into-partial reading session runs at ~85-90KB free / ~49KB maxAlloc by
  the first watermark crossing (Epub metadata + x-locations + resident glyph caches).
  Do not read mid-range heap numbers as session degradation without checking the scenario.
- SD-font section builds cost ~38-50KB at cold start; the 4-style advance-table prewarm
  (~30KB incl. 16KB contiguous scratch) dominates and is skipped below 80KB free.

## Misc Repo Gotchas

- `GfxRenderer::drawIcon`/`drawIconInverted` (raw-bits path) display the stored
  bitmap rotated 90° CW on the portrait X3/X4 (settled empirically: upright-
  stored art leans, CW-stored art stands on its head). Near-rotation-invariant
  icons (book, cog, search) hid this for the whole icon set; asymmetric art
  must be stored pre-rotated 90° CCW (see `scripts/gen_star_icon.py`, which
  also carries an upright book for the Library rows). `fillRect`-drawn marks
  are unaffected — rect primitives go through the correct transform.

- POSIX TZ signs are inverted from ISO 8601 in `TimeStore::applyTimezone()`: `"UTC-1"` means UTC+1.
- `LyraTheme::drawHeader()` does not call `BaseTheme::drawHeader()`, so header changes in the base theme must be duplicated in Lyra if needed.
