# An OTA beta channel, and a way back off it

Date: 2026-08-09
Status: design approved, not implemented.

## Problem

Every release today goes straight to every device: `OtaUpdater.cpp:30` points at
`https://api.github.com/repos/oreglio/CrossInkLibrary/releases/latest`, and that is the
only channel there is. Two consequences, both felt on 2026-08-09, when five releases went
out in a session and three carried a defect visible within three seconds on the device:

1. **No way to try a build on one device first.** A release is either published to
   everybody or it does not exist.
2. **No way back.** `OtaUpdater.cpp:326` refuses anything that is not strictly newer
   (`UPDATE_OLDER_ERROR`), so a device that took a broken release cannot return to the
   previous one from the device itself. The recovery path that day was a phone, the web
   portal, an SD card and a boot-time button combination.

The goal is a beta channel that is opt-in per check, and a supervised downgrade that gets
a device off a bad build in four button presses.

## What exists today, verified

- **The release URL is a compile-time constant** (`OtaUpdater.cpp:30`), with a comment
  warning that the OTA check gives no sign of which repository it asked.
- **Version comparison already understands release candidates.** `OtaUpdater.cpp:110`:
  `if (current.releaseCandidate && !latest.releaseCandidate) return 1;` — a device on
  `-rc` treats the stable of the same numbers as newer, so testers are carried onto the
  final release automatically. `containsRcMarker()` looks for a `-rc` substring. This is
  written, tested by nothing, and currently unreachable: no `-rc` build is ever offered.
- **Downgrade is blocked in software, not in hardware.** `installUpdate()` opens with
  `if (!isUpdateNewer()) return UPDATE_OLDER_ERROR;` (`OtaUpdater.cpp:325-327`), while
  `sdkconfig.defaults:333` and `:3394` show `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` and
  `CONFIG_APP_ANTI_ROLLBACK` are both unset. The bootloader will happily start an older
  image; only this guard stands in the way.
- **The JSON parser cannot read a list of releases.** `ReleaseJsonParser` recognises
  `tag_name` and the `assets` array only at `depth == 1`
  (`ReleaseJsonParser.cpp:83`, `:118`, `:209`), and `sOnArrayStart` increments `depth` for
  any top-level array that is not `assets` (`:212`). A `[ { ... } ]` body therefore shifts
  every field one level down and nothing is found.
- **`OtaUpdateActivity` is a small state machine** — `WIFI_SELECTION`,
  `CHECKING_FOR_UPDATE`, `WAITING_CONFIRMATION`, `UPDATE_IN_PROGRESS`, `NO_UPDATE`,
  `FAILED`, `FINISHED`, `SHUTTING_DOWN` (`OtaUpdateActivity.h:8-17`).
- **`OtaUpdater`'s signatures cannot change from this repository.**
  `platformio.ini:166` excludes `network/OtaUpdater.cpp` from the simulator build
  entirely — the `#ifdef SIMULATOR` block at its top is dead code — and the adjacent
  `crossink-simulator` repository defines `checkForUpdate` and `installUpdate`
  out-of-line in `simulator_ota.cpp`. Changing either signature breaks
  `pio run -e simulator`, and `AGENTS.md` puts simulator patches in that other repo.
  The channel and the downgrade intent are therefore carried as updater state set
  through **inline** setters, which also need no symbol to link.
- **`OptionSelectionActivity`** takes a title `StrId` and a `std::vector<std::string>`,
  and returns `OptionSelectionResult{index}` — the same component the countdown mode
  chooser uses.

## Design

### The channel is chosen per check, not stored

No new setting: nothing to persist, migrate or validate, and no extra row in Settings. A
new `CHANNEL_SELECTION` state runs **before** `WIFI_SELECTION`, so changing your mind
costs nothing and needs no network.

```
CHANNEL_SELECTION → WIFI_SELECTION → CHECKING_FOR_UPDATE → WAITING_CONFIRMATION → …
```

The chooser is an `OptionSelectionActivity` with two rows, `Stable` and `Beta`.

The cost is one extra press on every check, including for users who will never run a
beta. That is the accepted price of having no persistent setting.

### The channel is only a URL

| Channel | Route | Why |
|---|---|---|
| Stable | `/releases/latest` | GitHub excludes prereleases from this route natively, so the stable channel is protected without any code |
| Beta | `/releases?per_page=1` | Returns the newest release of any kind, prereleases included |

`setChannel()` is called before `checkForUpdate()`, which reads it. Nothing else in
the request changes.

Because `/releases` is ordered by creation date, a stable published after a beta is the
one a beta device sees — a beta channel is "everything, newest first", not "betas only".
That is the behaviour we want: a tester is never stranded behind the stable line.

### The parser learns to skip one array

`ReleaseJsonParser` gains `setExpectArray(bool)`. When set:

- the first top-level `[` sets `arrayEntered` and does **not** increment `depth`, so the
  release object inside it still lands at `depth == 1` and every existing rule applies
  unchanged;
- the matching `]` clears it;
- once a top-level release object has closed with a tag found, `completed` is set and
  later keys are ignored, so a body with more than one element still yields the first.

Everything else about the parser is untouched. `test/release_json_parser/` already exists
and gains cases for: an array-wrapped release parsing identically to the bare one, a
two-element array yielding the first, and `expectArray` off leaving current behaviour
exactly as it is.

### Downgrade, opened deliberately

`isUpdateNewer()` is not modified — it answers a true question and other code depends on
it. Two additions instead:

```cpp
void setChannel(OtaChannel value);   // inline; read by the next checkForUpdate()
void setAllowOlder(bool value);      // inline; read by the next installUpdate()
bool isDifferentVersion() const;     // inline; asset found and tag differs from ours
```

`installUpdate` keeps its guard, now written `if (!allowOlder && !isUpdateNewer())`,
reading the member rather than a parameter.

`OtaUpdateActivity.cpp:74` currently reads `if (!updater.isUpdateNewer()) state = NO_UPDATE;`
— an older release is reported as "no update" and there is no way past it. That single
branch becomes a three-way decision, and `WAITING_CONFIRMATION` phrases itself from the
two predicates:

| Condition | Prompt | `allowOlder` |
|---|---|---|
| `isUpdateNewer()` | "Update to X" | false |
| `isDifferentVersion()` only | "X is **older** than your Y. Install anyway?" | true |
| neither | falls through to `NO_UPDATE` as today | — |

The older-version prompt is a distinct string, not a reworded one, so a translator cannot
accidentally make a downgrade read like an upgrade.

### Publishing

A beta is tagged `vX.Y.Z-rcN` and published with `--prerelease`. `/releases/latest`
ignores it, so stable devices never see it. When `vX.Y.Z` ships, `OtaUpdater.cpp:110`
already carries every `-rc` device onto it. No publishing script changes.

## What this buys

The recovery path for a bad release stops being "find a computer, or a phone plus the web
portal plus an SD card plus a boot combination" and becomes:

```
Settings → Update → Stable → "v1.5.25 is older than yours — install?" → yes
```

## Risks

- **The downgrade is real.** Anti-rollback is off, so nothing behind the on-screen
  confirmation prevents installing an older image. That is the point, and it is also the
  reason the prompt must name the direction explicitly.
- **The adjacent simulator repository.** It defines two of these methods out-of-line,
  so their signatures are frozen from here. Both environments must be built on every
  change to `OtaUpdater`.
- **Rate limits are unchanged** — 60 anonymous requests per hour per IP, same as the
  current route.
- **A beta build can still brick the UI.** This design shortens the recovery, it does not
  remove the need to flash and look before publishing.

## Testing

Host tests in `test/release_json_parser/`, which needs no hardware:

- array-wrapped single release parses to the same tag, URL, size and digest as the bare
  object
- two-element array yields the first release
- `expectArray` off: existing behaviour byte-for-byte
- an empty array `[]` yields no tag and is reported as a parse failure rather than a crash

Channel selection, both prompts and the downgrade install are verified on device, on both
X3 and X4.

## Out of scope

- A persistent channel setting.
- Pinning to a specific version, or a list of past releases to choose from.
- Signature verification of firmware beyond the existing SHA-256 digest check.
- Any change to the publishing scripts or workflows.
