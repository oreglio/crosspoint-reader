# Countdown: a Pomodoro mode, a draining ring, and a testable clock

Date: 2026-08-09
Status: design approved, not implemented.

## Problem

`CountdownActivity` shipped today (v1.5.21 → v1.5.25) counts down to a wall-clock time
the user names, then counts the overshoot as `+mm`. Three things are now wanted:

1. A **Pomodoro mode** beside the existing one, chosen from a small menu.
2. A **ring that drains** in place of the progress bar, in both modes.
3. **Labels that cannot overflow** once the countdown is running.

The existing activity is 363 lines across five phases
(`src/activities/util/CountdownActivity.cpp`). Grafting a second phase machine onto it
would produce a ~600-line file carrying two interleaved state machines. That file is
also where four of today's five releases went wrong, twice through layout mistakes.
The decomposition below is therefore part of the deliverable, not a flourish.

## What exists today, verified

- **The time base is device-dependent and decided at runtime.** The X3 carries a DS3231
  (`BoardConfig.h:768`, `RtcType::Ds3231`); the X4 has `rtcAddr = 0` and no RTC, and
  cannot sync one over the network either — `HalClock::syncFromNTP()` opens with
  `if (!_available) return false`. Both devices run one binary, so `halClock.getTime()`
  answering or not is the only usable discriminator.
- **`getTime()` returns UTC**, not local time; callers must apply
  `SETTINGS.clockUtcOffsetQ` (`lib/hal/HalClock.h:45`, and `formatTime` for the
  arithmetic). It exposes hours and minutes only — there are no seconds — so the whole
  feature works at one-minute granularity.
- **`GfxRenderer::drawArc()` cannot draw a partial ring.** Its signature takes `xDir` /
  `yDir` quadrant selectors, not angles (`lib/GfxRenderer/GfxRenderer.h:234`,
  implementation at `GfxRenderer.cpp:1311`). It fills whole quadrants.
- **`GUI.drawProgressBar()` draws more than a bar.** Its last statement writes a
  percentage caption at `rect.y + rect.height + 15`
  (`src/components/themes/BaseTheme.cpp:164-166`). That caption landing five pixels from
  the "Elapsed" line was the overlap bug fixed in v1.5.25. The bar is already drawn
  locally in `CountdownActivity` for this reason, as `IntervalSelectionActivity` does.
- **`renderer.truncatedText(fontId, text, maxWidth, style)` exists**
  (`lib/GfxRenderer/GfxRenderer.h:287`) and truncates UTF-8-safely with an ellipsis.
  `drawCenteredText` never wraps, so the real failure mode is running off-screen, not
  wrapping.
- **`OptionSelectionActivity`** takes a title `StrId` and a `std::vector<std::string>`,
  and returns `OptionSelectionResult{index}` (`src/activities/util/OptionSelectionActivity.h:16`).
- **Power.** `preventAutoSleep()` keeps the panel up; `allowPowerSavingWhileAwake()`
  (added today, `src/activities/Activity.h`) lets the CPU drop to `LOW_POWER_FREQ`,
  which is **10 MHz** on C3 devices (`lib/hal/HalPowerManager.h:35`).
- **Flash budget is tight.** `app0` is `0x640000` = 6,553,600 bytes (`partitions.csv`)
  and v1.5.25 occupies 96.7% of it, leaving ~218 KB.

## Design

### Decomposition

```
src/activities/util/
  CountdownClock.{h,cpp}     time base + elapsed arithmetic   ← host-testable, no I/O
  CountdownRing.{h,cpp}      ring + centred text drawing
  CountdownActivity.{h,cpp}  "Target time" mode (existing, slimmed)
  PomodoroActivity.{h,cpp}   new
```

Home → `Countdown` → `OptionSelectionActivity(["Target time", "Pomodoro"])` → the chosen
activity. The chooser is pushed by `HomeActivity::onCountdownOpen()`, which today pushes
`CountdownActivity` directly.

### CountdownClock

The only piece with non-trivial arithmetic, and the only one worth testing. It owns the
RTC-versus-`millis()` choice so neither activity has to.

It performs **no I/O**: the caller reads the clock and hands the reading in. That is what
makes it testable on the host with no stubs at all, and it keeps the HAL dependency in
the activity layer where it belongs.

```cpp
class CountdownClock {
 public:
  // Pure: the next occurrence of a target, in minutes from the reference.
  static int spanTo(int targetMinuteOfDay, int fromMinuteOfDay);

  void startWallClock(int startMinuteOfDay, int spanMinutes);
  void startMonotonic(unsigned long startMs, int spanMinutes);

  void updateWallClock(int nowMinuteOfDay);   // caller supplies the RTC reading
  void updateMonotonic(unsigned long nowMs);  // caller supplies millis()

  int elapsedMinutes() const;   // monotonic, survives midnight
  int spanMinutes() const;
  bool finished() const;        // elapsed >= span
  int overshootMinutes() const; // 0 until finished
};
```

Reading the RTC and applying `SETTINGS.clockUtcOffsetQ` stays outside, as a free
`localMinuteOfDay()` in the activity layer returning -1 when no RTC answers.

Two behaviours it must preserve from the current implementation:

- **A target at or before the reference means tomorrow.** `spanTo` returns
  `target - from`, plus 1440 when that is `<= 0`. Picking exactly the current time is
  therefore a 24-hour span, not an instantly finished countdown.
- **Midnight rollover is absorbed.** `(now - start) mod 1440` returns to zero exactly 24
  hours after the start; each wrap adds 1440 to an accumulator so a long overshoot keeps
  climbing instead of restarting.

`millis()` is a sound time base for the clockless path: the activity blocks deep sleep
while counting, so the counter cannot be reset underneath it, and `esp_timer` is
compensated across the drop to 10 MHz. Subtraction is unsigned so the ~49-day wrap
resolves correctly.

### Pomodoro state machine

Fixed 25 / 5 / 15. A long break replaces the short one after every fourth work phase.
Each phase ends by waiting for a press — nothing advances on its own.

```
        ┌──────────────────────────────────────────┐
        │                                          │
        ▼                    span elapsed          │
  Work (25m) ─────────────► WaitingBreak ──OK──► Break (5m, or 15m every 4th)
                                                   │
                            span elapsed           │
  WaitingWork ◄────────────────────────────────────┘
        │
        └──OK──► Work (next pomodoro)
```

- While waiting, the `+mm` overshoot keeps climbing. It answers the question you actually
  have when you come back: how long has this been finished?
- Every phase change is a layout change and takes a `FULL_REFRESH`; per-minute repaints
  stay on `FAST_REFRESH`.
- The completed-pomodoro count is displayed and drives the long break. It is not
  persisted: quitting the screen ends the session.
- `Back` leaves immediately, without confirmation.

### Ring

`drawArc()` is unusable here, so the ring is drawn by sweeping the angle: from 12
o'clock, clockwise, one radial segment (`drawLine` from inner to outer radius) per step,
with step `1 / outerRadius` radians so the outer arc has no gaps. For a 100 px radius
that is ~630 segments of ~14 px, once a minute. The C3 has no FPU, so the sines are
software — a few milliseconds, invisible at this cadence.

```cpp
void drawCountdownRing(const GfxRenderer& r, int cx, int cy, int outerRadius,
                       int stroke, float fractionRemaining);
```

The ring *drains*: it starts whole and the drawn sweep shrinks to nothing, which reads
as a countdown without thinking about it. In overtime the ring is empty and the centre
carries `+12m`.

Geometry is derived, never hardcoded: the centre and radius come from
`getScreenWidth()` / `getScreenHeight()` and `getOrientedViewableTRBL()`, with
`stroke = outerRadius / 7`. Today's activity hardcodes Y offsets (120, 230, 266, 290),
which `AGENTS.md` forbids; that goes away.

### Screen layout, both modes

```
┌──────────────────────────┐
│ Pomodoro            89 % │   header via GUI.drawHeader
├──────────────────────────┤
│        ╭────────╮        │
│      ╱            ╲      │
│     │     14m      │     │   remaining, large, inside the ring
│     │   Travail    │     │   phase label under it
│      ╲            ╱      │
│        ╰────────╯        │
│      Pomodoro 3 · 25m    │   context line
│  « Back            OK »  │   GUI.drawButtonHints
└──────────────────────────┘
```

Target time mode shows `restant` / `de dépassement` in place of the phase label, and
`Cible 11:40 · écoulé 2m` on the context line.

### Label overflow

Every string is passed through `truncatedText` before drawing, with an explicit budget:

| text | maximum width |
|---|---|
| centre value and label | inner ring diameter − 2×`stroke` |
| context line | safe-area width − 2×side margin |

This matters most in French, where `de dépassement` is far longer than `over time`, and
translators can make any of these longer still.

### Power

Unchanged from today, and it now also covers the waiting states: the panel stays up and
the CPU idles at 10 MHz. A Pomodoro session left waiting for a press will therefore never
auto-sleep. Accepted deliberately — manual advance means the user intends to come back —
but it is the one behaviour that can flatten a battery unattended.

## Testing

A GoogleTest host test at `test/countdown_clock/`, following the existing per-directory
pattern (`CMakeLists.txt` linking `crosspoint_test_common` and `GTest::gtest_main`, as in
`test/reader_progress_save_debouncer/`). Because `CountdownClock` takes its readings as
arguments, the test needs no stubs directory and no HAL at all.

Cases:

- span to a target later today
- span to a target earlier today → tomorrow
- span to exactly the current time → 1440 minutes
- elapsed across midnight, twice, without restarting
- `millis()` path: elapsed in whole minutes, and the wrap
- Pomodoro sequence: which phase and which duration for pomodoros 1 through 9,
  including that the 4th and 8th breaks are long ones

This is the first test in this feature area. The Settings-unreachable regression of
v1.5.21 would have been caught by an equivalent test on the home menu, which is noted as
follow-up work below, not part of this change.

## Risks

- **Flash.** ~218 KB free. A new activity, the ring, and new strings across 28 languages
  will eat into it — the i18n cost alone was ~4.5 KB for eight keys. Measure after
  implementation. If it does not fit, `partitions.csv` has to be discussed before going
  further; the symptom of overflowing is `FIRMWARE_TOO_LARGE` at update time, not a build
  error.
- **Scope.** `SCOPE.md:41` rejects "interactive apps … this is a reader, not a PDA". A
  Pomodoro timer sits further from that line than a reading-session countdown does. Noted
  and accepted as the fork owner's call; it is a reason not to send this upstream.
- **Verification.** Every layout defect in this feature so far has been invisible to the
  compiler and obvious within three seconds on the device. This change must be flashed
  and looked at before any release is published.

## Out of scope

- Configurable Pomodoro durations. Fixed 25 / 5 / 15.
- Persisting a session across sleep or reboot.
- Sound or frontlight signalling at phase changes.
- A home-menu regression test (worth doing, unrelated to this change).
