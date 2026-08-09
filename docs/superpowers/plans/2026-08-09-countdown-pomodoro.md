# Countdown Pomodoro Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Pomodoro mode beside the existing target-time countdown, replace the progress bar with a ring that drains, and extract the timing arithmetic into a host-tested class.

**Architecture:** Four units. `CountdownClock` holds the elapsed/span arithmetic and performs no I/O, so it is unit-tested on the host. `PomodoroSchedule` is a pure function table for the 25/5/15 sequence, also host-tested. `CountdownRing` draws the ring. `CountdownActivity` (target time) and `PomodoroActivity` are thin screens over those three. A mode chooser sits between Home and both.

**Tech Stack:** C++20, PlatformIO, ESP-IDF/Arduino-ESP32, GoogleTest for host tests, CMake.

## Global Constraints

- Target `pio run -e default` — **one binary serves both X3 and X4**. Device differences are runtime-only: X3 has a DS3231 (`BoardConfig.h:768`), X4 has `rtcAddr = 0` and no NTP fallback (`HalClock::syncFromNTP` returns false when `!_available`).
- Never hardcode screen coordinates. Derive every position from `renderer.getScreenWidth()`, `getScreenHeight()`, `UITheme::getInstance().getScreenSafeArea(...)` and `getMetrics()`. (`AGENTS.md`, UI And Input.)
- All user-facing strings go through `tr(STR_*)` / `I18N.get(...)`. Logs stay hardcoded.
- New i18n keys go in `lib/I18n/translations/english.yaml` and `french.yaml`, then `python3 scripts/gen_i18n.py`. Never edit `lib/I18n/I18n*.h/cpp` by hand.
- Run `clang-format -i` on every touched C++ file before committing.
- No exceptions, no bare `new`. Use `makeUniqueNoThrow<T>()` from `lib/Memory/Memory.h` for fallible heap allocations.
- Fixed Pomodoro durations: **work 25 min, short break 5 min, long break 15 min, long break after every 4th work phase.**
- Flash budget: `app0` is 6,553,600 bytes and v1.5.25 uses 96.7%. Measure after Task 5 and report.
- Do not publish an OTA release from this plan. Build, flash, verify on both devices first.

---

### Task 1: CountdownClock, host-tested

**Files:**
- Create: `src/activities/util/CountdownClock.h`
- Create: `src/activities/util/CountdownClock.cpp`
- Create: `test/countdown/CMakeLists.txt`
- Create: `test/countdown/CountdownClockTest.cpp`
- Modify: `test/CMakeLists.txt` (add `add_subdirectory(countdown)` next to the other subdirectories)

**Interfaces:**
- Consumes: nothing.
- Produces: `class CountdownClock` with `static int spanTo(int targetMinuteOfDay, int fromMinuteOfDay)`, `void startWallClock(int startMinuteOfDay, int spanMinutes)`, `void startMonotonic(unsigned long startMs, int spanMinutes)`, `void updateWallClock(int nowMinuteOfDay)`, `void updateMonotonic(unsigned long nowMs)`, `int elapsedMinutes() const`, `int spanMinutes() const`, `int remainingMinutes() const`, `int overshootMinutes() const`, `bool finished() const`, `bool usesWallClock() const`, `float fractionRemaining() const`.

- [ ] **Step 1: Write the failing test**

Create `test/countdown/CountdownClockTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include "CountdownClock.h"

TEST(CountdownClockSpan, TargetLaterTodayIsTheDifference) {
  EXPECT_EQ(CountdownClock::spanTo(700, 600), 100);
}

TEST(CountdownClockSpan, TargetEarlierTodayMeansTomorrow) {
  EXPECT_EQ(CountdownClock::spanTo(600, 700), 1340);
}

TEST(CountdownClockSpan, TargetEqualToNowMeansAFullDay) {
  EXPECT_EQ(CountdownClock::spanTo(600, 600), 1440);
}

TEST(CountdownClockWallClock, ElapsedTracksTheReading) {
  CountdownClock clock;
  clock.startWallClock(/*startMinuteOfDay=*/600, /*spanMinutes=*/30);
  clock.updateWallClock(612);
  EXPECT_EQ(clock.elapsedMinutes(), 12);
  EXPECT_EQ(clock.remainingMinutes(), 18);
  EXPECT_FALSE(clock.finished());
  EXPECT_EQ(clock.overshootMinutes(), 0);
  EXPECT_TRUE(clock.usesWallClock());
}

TEST(CountdownClockWallClock, CrossesMidnightWithoutRestarting) {
  CountdownClock clock;
  clock.startWallClock(/*startMinuteOfDay=*/1400, /*spanMinutes=*/100);
  clock.updateWallClock(1430);
  EXPECT_EQ(clock.elapsedMinutes(), 30);
  clock.updateWallClock(10);  // 00:10, past midnight
  EXPECT_EQ(clock.elapsedMinutes(), 50);
  EXPECT_TRUE(clock.finished());
  EXPECT_EQ(clock.overshootMinutes(), 0);
  clock.updateWallClock(1399);
  EXPECT_EQ(clock.elapsedMinutes(), 1439);
  clock.updateWallClock(1400);  // exactly 24h after the start
  EXPECT_EQ(clock.elapsedMinutes(), 1440);
  clock.updateWallClock(1410);
  EXPECT_EQ(clock.elapsedMinutes(), 1450);
  // and a second midnight, to prove the rollover accumulates rather than toggles
  clock.updateWallClock(1399);
  EXPECT_EQ(clock.elapsedMinutes(), 2879);
  clock.updateWallClock(1400);
  EXPECT_EQ(clock.elapsedMinutes(), 2880);
}

TEST(CountdownClockFormat, ReadsAsHoursOnlyPastTheHour) {
  char buf[16];
  formatCountdownSpan(0, buf, sizeof(buf));
  EXPECT_STREQ(buf, "0m");
  formatCountdownSpan(42, buf, sizeof(buf));
  EXPECT_STREQ(buf, "42m");
  formatCountdownSpan(59, buf, sizeof(buf));
  EXPECT_STREQ(buf, "59m");
  formatCountdownSpan(60, buf, sizeof(buf));
  EXPECT_STREQ(buf, "1h00");
  formatCountdownSpan(65, buf, sizeof(buf));
  EXPECT_STREQ(buf, "1h05");
  formatCountdownSpan(1440, buf, sizeof(buf));
  EXPECT_STREQ(buf, "24h00");
}

TEST(CountdownClockWallClock, OvershootCountsPastTheSpan) {
  CountdownClock clock;
  clock.startWallClock(600, 30);
  clock.updateWallClock(645);
  EXPECT_TRUE(clock.finished());
  EXPECT_EQ(clock.remainingMinutes(), 0);
  EXPECT_EQ(clock.overshootMinutes(), 15);
}

TEST(CountdownClockMonotonic, ElapsedIsWholeMinutes) {
  CountdownClock clock;
  clock.startMonotonic(/*startMs=*/1000, /*spanMinutes=*/25);
  clock.updateMonotonic(1000 + 59'999);
  EXPECT_EQ(clock.elapsedMinutes(), 0);
  clock.updateMonotonic(1000 + 60'000);
  EXPECT_EQ(clock.elapsedMinutes(), 1);
  EXPECT_FALSE(clock.usesWallClock());
}

TEST(CountdownClockMonotonic, SurvivesTheMillisWrap) {
  CountdownClock clock;
  const unsigned long nearMax = 0xFFFFFF00UL;
  clock.startMonotonic(nearMax, 25);
  clock.updateMonotonic(nearMax + 120'000UL);  // wraps through zero
  EXPECT_EQ(clock.elapsedMinutes(), 2);
}

TEST(CountdownClockFraction, GoesFromOneToZeroAndStopsThere) {
  CountdownClock clock;
  clock.startWallClock(600, 20);
  EXPECT_FLOAT_EQ(clock.fractionRemaining(), 1.0f);
  clock.updateWallClock(610);
  EXPECT_FLOAT_EQ(clock.fractionRemaining(), 0.5f);
  clock.updateWallClock(630);
  EXPECT_FLOAT_EQ(clock.fractionRemaining(), 0.0f);
}
```

Create `test/countdown/CMakeLists.txt`:

```cmake
add_executable(CountdownTest
  CountdownClockTest.cpp
  ${REPO_ROOT}/src/activities/util/CountdownClock.cpp
)

target_include_directories(CountdownTest PRIVATE
  ${REPO_ROOT}/src/activities/util
)

target_link_libraries(CountdownTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CountdownTest)
```

Add to `test/CMakeLists.txt`, alongside the existing `add_subdirectory(...)` lines:

```cmake
add_subdirectory(countdown)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B test/build -DCMAKE_BUILD_TYPE=Release
cmake --build test/build --target CountdownTest
```

Expected: FAIL at configure or compile time with `CountdownClock.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `src/activities/util/CountdownClock.h`:

```cpp
#pragma once

// Elapsed/remaining arithmetic for a countdown, with no I/O of its own: the
// caller reads the clock and hands the reading in. That keeps the HAL out of
// here and makes the whole class testable on the host with no stubs.
//
// Two time bases, because the devices differ. The X3 has a DS3231 and counts in
// minute-of-day; the X4 has no clock at all and counts with millis(), which is
// safe here only because the countdown screen blocks deep sleep while running.
class CountdownClock {
 public:
  static constexpr int kMinutesPerDay = 1440;

  // Minutes from `fromMinuteOfDay` to the next occurrence of
  // `targetMinuteOfDay`. Equal or earlier means tomorrow, so this never returns
  // zero or a negative: picking the current time reads as a 24-hour span rather
  // than an instantly finished countdown.
  static int spanTo(int targetMinuteOfDay, int fromMinuteOfDay);

  void startWallClock(int startMinuteOfDay, int spanMinutes);
  void startMonotonic(unsigned long startMs, int spanMinutes);

  void updateWallClock(int nowMinuteOfDay);
  void updateMonotonic(unsigned long nowMs);

  int elapsedMinutes() const { return elapsed_; }
  int spanMinutes() const { return span_; }
  int remainingMinutes() const;
  int overshootMinutes() const;
  bool finished() const { return elapsed_ >= span_; }
  bool usesWallClock() const { return wallClock_; }

  // 1.0 at the start, 0.0 at the target and beyond.
  float fractionRemaining() const;

 private:
  bool wallClock_ = true;
  int span_ = 1;
  int elapsed_ = 0;
  int startMinuteOfDay_ = 0;
  int lastRawElapsed_ = 0;
  int rollover_ = 0;
  unsigned long startMs_ = 0;
};

// "1h05" once past the hour, "42m" below it. Lives here rather than in each
// screen's anonymous namespace so the two cannot drift apart. Callers add any
// sign or prefix themselves.
void formatCountdownSpan(int minutes, char* buf, size_t len);
```

The header needs `#include <cstddef>` for `size_t`.

Create `src/activities/util/CountdownClock.cpp`:

```cpp
#include "CountdownClock.h"

#include <algorithm>

int CountdownClock::spanTo(const int targetMinuteOfDay, const int fromMinuteOfDay) {
  int span = targetMinuteOfDay - fromMinuteOfDay;
  if (span <= 0) span += kMinutesPerDay;
  return span;
}

void CountdownClock::startWallClock(const int startMinuteOfDay, const int spanMinutes) {
  wallClock_ = true;
  startMinuteOfDay_ = startMinuteOfDay;
  span_ = std::max(1, spanMinutes);
  elapsed_ = 0;
  lastRawElapsed_ = 0;
  rollover_ = 0;
}

void CountdownClock::startMonotonic(const unsigned long startMs, const int spanMinutes) {
  wallClock_ = false;
  startMs_ = startMs;
  span_ = std::max(1, spanMinutes);
  elapsed_ = 0;
  lastRawElapsed_ = 0;
  rollover_ = 0;
}

void CountdownClock::updateWallClock(const int nowMinuteOfDay) {
  const int raw = ((nowMinuteOfDay - startMinuteOfDay_) % kMinutesPerDay + kMinutesPerDay) % kMinutesPerDay;
  // raw returns to zero exactly 24h after the start. Absorb each wrap so a long
  // overshoot keeps climbing instead of restarting the countdown.
  if (raw < lastRawElapsed_) rollover_ += kMinutesPerDay;
  lastRawElapsed_ = raw;
  elapsed_ = raw + rollover_;
}

void CountdownClock::updateMonotonic(const unsigned long nowMs) {
  // Unsigned subtraction, so the ~49-day millis() wrap resolves correctly.
  elapsed_ = static_cast<int>((nowMs - startMs_) / 60000UL);
}

int CountdownClock::remainingMinutes() const { return std::max(0, span_ - elapsed_); }

int CountdownClock::overshootMinutes() const { return std::max(0, elapsed_ - span_); }

float CountdownClock::fractionRemaining() const {
  if (span_ <= 0) return 0.0f;
  const float fraction = static_cast<float>(remainingMinutes()) / static_cast<float>(span_);
  return std::clamp(fraction, 0.0f, 1.0f);
}

void formatCountdownSpan(const int minutes, char* buf, const size_t len) {
  if (minutes >= 60) {
    snprintf(buf, len, "%dh%02d", minutes / 60, minutes % 60);
  } else {
    snprintf(buf, len, "%dm", minutes);
  }
}
```

The `.cpp` needs `#include <cstdio>` for `snprintf`.

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build test/build --target CountdownTest && ctest --test-dir test/build -R "Countdown|Pomodoro" --output-on-failure
```

Expected: all 10 tests PASS.

- [ ] **Step 5: Commit**

```bash
clang-format -i src/activities/util/CountdownClock.cpp src/activities/util/CountdownClock.h
git add src/activities/util/CountdownClock.h src/activities/util/CountdownClock.cpp \
        test/countdown/CMakeLists.txt test/countdown/CountdownClockTest.cpp test/CMakeLists.txt
git commit -m "feat: extract countdown timing into a host-tested CountdownClock"
```

---

### Task 2: PomodoroSchedule, host-tested

**Files:**
- Create: `src/activities/util/PomodoroSchedule.h`
- Create: `test/countdown/PomodoroScheduleTest.cpp`
- Modify: `test/countdown/CMakeLists.txt` (add the new source to `add_executable`)

**Interfaces:**
- Consumes: nothing.
- Produces: `enum class PomodoroPhase : uint8_t { Work, ShortBreak, LongBreak }`, `struct PomodoroStep { PomodoroPhase phase; int minutes; }`, `struct PomodoroSchedule` with `static PomodoroStep stepAt(int stepIndex)` and `static int pomodoroNumber(int stepIndex)`, plus the constants `kWorkMinutes = 25`, `kShortBreakMinutes = 5`, `kLongBreakMinutes = 15`, `kPomodorosPerLongBreak = 4`.

- [ ] **Step 1: Write the failing test**

Create `test/countdown/PomodoroScheduleTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include "PomodoroSchedule.h"

TEST(PomodoroScheduleSteps, EvenStepsAreWork) {
  EXPECT_EQ(PomodoroSchedule::stepAt(0).phase, PomodoroPhase::Work);
  EXPECT_EQ(PomodoroSchedule::stepAt(0).minutes, 25);
  EXPECT_EQ(PomodoroSchedule::stepAt(2).phase, PomodoroPhase::Work);
  EXPECT_EQ(PomodoroSchedule::stepAt(2).minutes, 25);
}

TEST(PomodoroScheduleSteps, BreaksAfterTheFirstThreePomodorosAreShort) {
  EXPECT_EQ(PomodoroSchedule::stepAt(1).phase, PomodoroPhase::ShortBreak);
  EXPECT_EQ(PomodoroSchedule::stepAt(1).minutes, 5);
  EXPECT_EQ(PomodoroSchedule::stepAt(3).phase, PomodoroPhase::ShortBreak);
  EXPECT_EQ(PomodoroSchedule::stepAt(5).phase, PomodoroPhase::ShortBreak);
}

TEST(PomodoroScheduleSteps, EveryFourthBreakIsLong) {
  // step 7 is the break after the 4th work phase
  EXPECT_EQ(PomodoroSchedule::stepAt(7).phase, PomodoroPhase::LongBreak);
  EXPECT_EQ(PomodoroSchedule::stepAt(7).minutes, 15);
  // step 15 is the break after the 8th
  EXPECT_EQ(PomodoroSchedule::stepAt(15).phase, PomodoroPhase::LongBreak);
  // and the one before it is not
  EXPECT_EQ(PomodoroSchedule::stepAt(13).phase, PomodoroPhase::ShortBreak);
}

TEST(PomodoroScheduleNumbering, CountsTheWorkPhase) {
  EXPECT_EQ(PomodoroSchedule::pomodoroNumber(0), 1);  // during the 1st work
  EXPECT_EQ(PomodoroSchedule::pomodoroNumber(1), 1);  // during the break after it
  EXPECT_EQ(PomodoroSchedule::pomodoroNumber(2), 2);
  EXPECT_EQ(PomodoroSchedule::pomodoroNumber(3), 2);
  EXPECT_EQ(PomodoroSchedule::pomodoroNumber(6), 4);
  EXPECT_EQ(PomodoroSchedule::pomodoroNumber(7), 4);
  EXPECT_EQ(PomodoroSchedule::pomodoroNumber(8), 5);
}

TEST(PomodoroScheduleSteps, NegativeIndexIsClampedToTheFirstWorkPhase) {
  EXPECT_EQ(PomodoroSchedule::stepAt(-1).phase, PomodoroPhase::Work);
  EXPECT_EQ(PomodoroSchedule::pomodoroNumber(-1), 1);
}
```

Update `test/countdown/CMakeLists.txt` so `add_executable` reads:

```cmake
add_executable(CountdownTest
  CountdownClockTest.cpp
  PomodoroScheduleTest.cpp
  ${REPO_ROOT}/src/activities/util/CountdownClock.cpp
)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build test/build --target CountdownTest
```

Expected: FAIL with `PomodoroSchedule.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `src/activities/util/PomodoroSchedule.h` (header-only — it is a pure table, no state):

```cpp
#pragma once

#include <cstdint>

enum class PomodoroPhase : uint8_t { Work, ShortBreak, LongBreak };

struct PomodoroStep {
  PomodoroPhase phase;
  int minutes;
};

// The classic 25/5/15 sequence, expressed as a pure function of the step index.
// Even indices are work phases, odd indices the break that follows; every fourth
// break is the long one. Nothing here is persisted — leaving the screen ends the
// session — so an index is all the state the activity has to keep.
struct PomodoroSchedule {
  static constexpr int kWorkMinutes = 25;
  static constexpr int kShortBreakMinutes = 5;
  static constexpr int kLongBreakMinutes = 15;
  static constexpr int kPomodorosPerLongBreak = 4;

  static PomodoroStep stepAt(const int stepIndex) {
    const int index = stepIndex < 0 ? 0 : stepIndex;
    if (index % 2 == 0) {
      return {PomodoroPhase::Work, kWorkMinutes};
    }
    const int completedWork = index / 2 + 1;
    if (completedWork % kPomodorosPerLongBreak == 0) {
      return {PomodoroPhase::LongBreak, kLongBreakMinutes};
    }
    return {PomodoroPhase::ShortBreak, kShortBreakMinutes};
  }

  // Which pomodoro this step belongs to, counting from 1. A break belongs to the
  // work phase it follows, so the number does not jump mid-cycle.
  static int pomodoroNumber(const int stepIndex) {
    const int index = stepIndex < 0 ? 0 : stepIndex;
    return index / 2 + 1;
  }
};
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build test/build --target CountdownTest && ctest --test-dir test/build -R "Countdown|Pomodoro" --output-on-failure
```

Expected: all 15 tests PASS (10 from Task 1 plus 5 here).

- [ ] **Step 5: Commit**

```bash
clang-format -i src/activities/util/PomodoroSchedule.h
git add src/activities/util/PomodoroSchedule.h test/countdown/PomodoroScheduleTest.cpp test/countdown/CMakeLists.txt
git commit -m "feat: add a host-tested 25/5/15 pomodoro schedule"
```

---

### Task 3: The draining ring

**Files:**
- Create: `src/activities/util/CountdownRing.h`
- Create: `src/activities/util/CountdownRing.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `void drawCountdownRing(const GfxRenderer& renderer, int cx, int cy, int outerRadius, int stroke, float fractionRemaining)`.

There is no host test here: it draws into a framebuffer and its correctness is visual. It is verified on device in Task 4.

- [ ] **Step 1: Write the header**

Create `src/activities/util/CountdownRing.h`:

```cpp
#pragma once

class GfxRenderer;

// A ring that starts whole and drains clockwise from 12 o'clock as the countdown
// runs. fractionRemaining is clamped to [0, 1]; at 0 nothing is drawn.
//
// GfxRenderer::drawArc() cannot serve here: it takes xDir/yDir quadrant
// selectors rather than angles and fills whole quadrants only.
void drawCountdownRing(const GfxRenderer& renderer, int cx, int cy, int outerRadius, int stroke,
                       float fractionRemaining);
```

- [ ] **Step 2: Write the implementation**

Create `src/activities/util/CountdownRing.cpp`:

```cpp
#include "CountdownRing.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;
}  // namespace

void drawCountdownRing(const GfxRenderer& renderer, const int cx, const int cy, const int outerRadius,
                       const int stroke, const float fractionRemaining) {
  if (outerRadius <= 0 || stroke <= 0) return;

  const float fraction = std::clamp(fractionRemaining, 0.0f, 1.0f);
  if (fraction <= 0.0f) return;

  const int innerRadius = std::max(outerRadius - stroke, 1);
  const float sweep = fraction * kTwoPi;

  // One radial spoke per step. Half a pixel of arc between spokes at the outer
  // edge (step = 0.5 / outerRadius radians) is what keeps the rim solid instead
  // of dotted. For a 100px radius that is ~1250 spokes of a few pixels each,
  // drawn once a minute — the C3 has no FPU so the sines are software, which
  // costs single-digit milliseconds at this cadence.
  const float step = 0.5f / static_cast<float>(outerRadius);

  for (float angle = 0.0f; angle < sweep; angle += step) {
    const float s = sinf(angle);
    const float c = cosf(angle);
    // Clockwise from 12 o'clock: x follows sin, y follows -cos.
    const int x0 = cx + static_cast<int>(s * static_cast<float>(innerRadius));
    const int y0 = cy - static_cast<int>(c * static_cast<float>(innerRadius));
    const int x1 = cx + static_cast<int>(s * static_cast<float>(outerRadius));
    const int y1 = cy - static_cast<int>(c * static_cast<float>(outerRadius));
    renderer.drawLine(x0, y0, x1, y1, true);
  }
}
```

- [ ] **Step 3: Verify it compiles**

```bash
pio run -e default
```

Expected: SUCCESS. (The function is not called yet; this only proves it builds for the C3.)

- [ ] **Step 4: Commit**

```bash
clang-format -i src/activities/util/CountdownRing.cpp src/activities/util/CountdownRing.h
git add src/activities/util/CountdownRing.h src/activities/util/CountdownRing.cpp
git commit -m "feat: add a draining countdown ring drawn by angular sweep"
```

---

### Task 4: Rebuild CountdownActivity on the clock and the ring

**Files:**
- Modify: `src/activities/util/CountdownActivity.h` (replace the timing fields with a `CountdownClock`)
- Modify: `src/activities/util/CountdownActivity.cpp` (replace `renderRunning()`; keep the picker as-is)

**Interfaces:**
- Consumes: `CountdownClock` (Task 1), `drawCountdownRing(...)` (Task 3).
- Produces: a private helper reused by Task 5 — `struct CountdownLayout { int cx; int cy; int outerRadius; int stroke; int contextY; int contextMaxWidth; }` and `static CountdownLayout computeCountdownLayout(const GfxRenderer&, const MappedInputManager&)`, declared in a new `src/activities/util/CountdownLayout.h`.

- [ ] **Step 1: Extract the shared layout**

Create `src/activities/util/CountdownLayout.h`:

```cpp
#pragma once

class GfxRenderer;
class MappedInputManager;

// Where the ring, its centred text and the context line go. Derived from the
// renderer and the theme rather than hardcoded, so orientation and device
// profiles carry over. Shared by the countdown and pomodoro screens so the two
// cannot drift apart.
struct CountdownLayout {
  int cx;
  int cy;
  int outerRadius;
  int stroke;
  int centerMaxWidth;   // truncation budget inside the ring
  int contextY;         // top of the single context line under the ring
  int contextMaxWidth;  // truncation budget for that line
};

CountdownLayout computeCountdownLayout(const GfxRenderer& renderer, const MappedInputManager& mappedInput);
```

Create `src/activities/util/CountdownLayout.cpp`:

```cpp
#include "CountdownLayout.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

CountdownLayout computeCountdownLayout(const GfxRenderer& renderer, const MappedInputManager& mappedInput) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
  const Rect header = TouchHeaderBackButton::standardHeaderRect(renderer);

  constexpr int kSideMargin = 24;
  constexpr int kContextGap = 16;

  const int contextHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int ringTop = header.y + header.height + 8;
  const int ringBottom = safe.y + safe.height - metrics.buttonHintsHeight - contextHeight - kContextGap;
  const int ringHeight = std::max(0, ringBottom - ringTop);

  CountdownLayout layout{};
  layout.outerRadius = std::max(0, std::min(safe.width / 2 - kSideMargin, ringHeight / 2));
  layout.stroke = std::max(4, layout.outerRadius / 7);
  layout.cx = safe.x + safe.width / 2;
  layout.cy = ringTop + ringHeight / 2;
  layout.centerMaxWidth = std::max(16, 2 * (layout.outerRadius - layout.stroke) - 16);
  layout.contextY = ringBottom + kContextGap;
  layout.contextMaxWidth = std::max(16, safe.width - 2 * kSideMargin);
  return layout;
}
```

- [ ] **Step 2: Replace the timing fields in the header**

In `src/activities/util/CountdownActivity.h`, delete the fields `startMs`, `startMinuteOfDay`, `spanMinutes`, `elapsedMinutes`, `lastRawElapsed`, `rolloverMinutes` and add:

```cpp
#include "CountdownClock.h"
```

```cpp
  CountdownClock clock;
```

Keep `useWallClock`, `nowHour`, `nowMinute`, `targetHour`, `targetMinute`, `lastShownMinute`, `pendingFullRefresh`, `wasOvertime`, `lastFullRefreshMinute`, `kFullRefreshEveryMinutes` and the phase enum unchanged.

- [ ] **Step 3: Route the activity through the clock**

In `startCountdown()`, replace the span arithmetic with:

```cpp
  const int target = targetHour * 60 + targetMinute;
  int reference;

  if (useWallClock) {
    const int now = localMinuteOfDay();
    reference = now >= 0 ? now : nowHour * 60 + nowMinute;
    clock.startWallClock(reference, CountdownClock::spanTo(target, reference));
  } else {
    reference = nowHour * 60 + nowMinute;
    clock.startMonotonic(millis(), CountdownClock::spanTo(target, reference));
  }
```

In `refreshElapsed()`, replace the body with:

```cpp
  if (useWallClock) {
    const int now = localMinuteOfDay();
    if (now < 0) return;
    clock.updateWallClock(now);
  } else {
    clock.updateMonotonic(millis());
  }

  if (clock.elapsedMinutes() != lastShownMinute) {
    lastShownMinute = clock.elapsedMinutes();
    requestUpdate();
  }
```

- [ ] **Step 4: Replace renderRunning() with the ring**

Replace the whole body of `CountdownActivity::renderRunning()` with:

```cpp
void CountdownActivity::renderRunning() {
  const CountdownLayout layout = computeCountdownLayout(renderer, mappedInput);
  const bool overtime = clock.finished();

  drawCountdownRing(renderer, layout.cx, layout.cy, layout.outerRadius, layout.stroke, clock.fractionRemaining());

  char bigValue[16];
  if (overtime) {
    char span[12];
    formatCountdownSpan(clock.overshootMinutes(), span, sizeof(span));
    snprintf(bigValue, sizeof(bigValue), "+%s", span);
  } else {
    formatCountdownSpan(clock.remainingMinutes(), bigValue, sizeof(bigValue));
  }

  const int valueHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int blockTop = layout.cy - (valueHeight + 4 + labelHeight) / 2;

  const std::string value = renderer.truncatedText(UI_12_FONT_ID, bigValue, layout.centerMaxWidth,
                                                   EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, blockTop, value.c_str(), true, EpdFontFamily::BOLD);

  const char* labelText = overtime ? tr(STR_COUNTDOWN_OVERTIME) : tr(STR_COUNTDOWN_REMAINING);
  const std::string label = renderer.truncatedText(SMALL_FONT_ID, labelText, layout.centerMaxWidth);
  renderer.drawCenteredText(SMALL_FONT_ID, blockTop + valueHeight + 4, label.c_str(), true);

  char elapsedText[12];
  formatCountdownSpan(std::min(clock.elapsedMinutes(), clock.spanMinutes()), elapsedText, sizeof(elapsedText));
  char line[80];
  snprintf(line, sizeof(line), "%s %02d:%02d \xC2\xB7 %s %s", tr(STR_COUNTDOWN_TARGET), targetHour, targetMinute,
           tr(STR_COUNTDOWN_ELAPSED), elapsedText);
  const std::string context = renderer.truncatedText(SMALL_FONT_ID, line, layout.contextMaxWidth);
  renderer.drawCenteredText(SMALL_FONT_ID, layout.contextY, context.c_str(), true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
```

Note `\xC2\xB7` is a UTF-8 middle dot. Add `#include "CountdownLayout.h"`, `#include "CountdownRing.h"` and `#include <string>` at the top of the file. **Delete the anonymous-namespace `formatSpan` helper** from this file — it now lives in `CountdownClock` as `formatCountdownSpan` and is shared with the pomodoro screen. In `render()`, replace `elapsedMinutes >= spanMinutes` with `clock.finished()`.

- [ ] **Step 5: Build and run the host tests**

```bash
clang-format -i src/activities/util/CountdownActivity.cpp src/activities/util/CountdownActivity.h \
                src/activities/util/CountdownLayout.cpp src/activities/util/CountdownLayout.h
pio run -e default
ctest --test-dir test/build -R "Countdown|Pomodoro" --output-on-failure
```

Expected: both SUCCESS.

- [ ] **Step 6: Flash and verify on hardware — both devices**

```bash
pio run -e default -t upload
```

On **X4** (no RTC, up/down rocker): Home → Countdown → it asks the current time first → set a target 3 minutes out → the ring is whole, drains visibly each minute, the centre reads `3m` then `2m`, the context line reads `Cible HH:MM · Écoulé 0m` on one line without running off the screen. At the target the centre flips to `+0m` and the ring is empty.

On **X3** (DS3231, edge side buttons): Home → Countdown → it goes straight to the target picker → the side buttons still step 6 h / 10 min in the correct direction → same ring behaviour.

- [ ] **Step 7: Commit**

```bash
git add src/activities/util/CountdownActivity.h src/activities/util/CountdownActivity.cpp \
        src/activities/util/CountdownLayout.h src/activities/util/CountdownLayout.cpp
git commit -m "feat: draw the countdown as a draining ring on a shared layout"
```

---

### Task 5: Pomodoro mode and the mode chooser

**Files:**
- Create: `src/activities/util/PomodoroActivity.h`
- Create: `src/activities/util/PomodoroActivity.cpp`
- Modify: `src/activities/home/HomeActivity.cpp` (`onCountdownOpen()` pushes the chooser)
- Modify: `lib/I18n/translations/english.yaml`, `lib/I18n/translations/french.yaml`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: `CountdownClock` (Task 1), `PomodoroSchedule` (Task 2), `drawCountdownRing` (Task 3), `computeCountdownLayout` (Task 4).
- Produces: `class PomodoroActivity final : public Activity` with the usual `onEnter/loop/render` plus `preventAutoSleep()` and `allowPowerSavingWhileAwake()` both true while a step is active or waiting.

- [ ] **Step 1: Add the strings**

Append to `lib/I18n/translations/english.yaml`:

```yaml
STR_COUNTDOWN_MODE_TITLE: "Countdown"
STR_COUNTDOWN_MODE_TARGET: "Target time"
STR_POMODORO: "Pomodoro"
STR_POMODORO_WORK: "Work"
STR_POMODORO_BREAK: "Break"
STR_POMODORO_LONG_BREAK: "Long break"
STR_POMODORO_NEXT_HINT: "Done - OK for next"
STR_POMODORO_COUNT_FORMAT: "Pomodoro %d"
```

Append to `lib/I18n/translations/french.yaml`:

```yaml
STR_COUNTDOWN_MODE_TITLE: "Compte à rebours"
STR_COUNTDOWN_MODE_TARGET: "Heure de fin"
STR_POMODORO: "Pomodoro"
STR_POMODORO_WORK: "Travail"
STR_POMODORO_BREAK: "Pause"
STR_POMODORO_LONG_BREAK: "Pause longue"
STR_POMODORO_NEXT_HINT: "Terminé - OK pour la suite"
STR_POMODORO_COUNT_FORMAT: "Pomodoro %d"
```

Then:

```bash
python3 scripts/gen_i18n.py
```

- [ ] **Step 2: Write the Pomodoro activity header**

Create `src/activities/util/PomodoroActivity.h`:

```cpp
#pragma once

#include <cstdint>

#include "CountdownClock.h"
#include "MappedInputManager.h"
#include "PomodoroSchedule.h"
#include "activities/Activity.h"

class GfxRenderer;

// The 25/5/15 sequence, advanced by hand: each step counts down, then waits for
// OK before the next one starts. Nothing advances on its own, so a step that has
// finished keeps counting "+mm" — that is the useful figure when you come back
// to the device and want to know how long it has been sitting done.
//
// millis() is the time base regardless of device: a pomodoro is a duration, not
// a time of day, so the X3's RTC buys nothing here and the X4 needs no fallback.
class PomodoroActivity final : public Activity {
 public:
  PomodoroActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Pomodoro", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool preventAutoSleep() override { return true; }
  bool allowPowerSavingWhileAwake() const override { return true; }

 private:
  static constexpr int kFullRefreshEveryMinutes = 30;

  int stepIndex = 0;
  bool waitingForNext = false;
  CountdownClock clock;
  int lastShownMinute = -1;
  int lastFullRefreshMinute = 0;
  bool pendingFullRefresh = true;

  void startStep(int index);
  void advance();
  const char* phaseLabel() const;
};
```

- [ ] **Step 3: Write the Pomodoro activity implementation**

Create `src/activities/util/PomodoroActivity.cpp`:

```cpp
#include "PomodoroActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "CountdownLayout.h"
#include "CountdownRing.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PomodoroActivity::onEnter() {
  Activity::onEnter();
  startStep(0);
}

void PomodoroActivity::startStep(const int index) {
  stepIndex = index;
  waitingForNext = false;
  const PomodoroStep step = PomodoroSchedule::stepAt(stepIndex);
  clock.startMonotonic(millis(), step.minutes);
  lastShownMinute = -1;
  lastFullRefreshMinute = 0;
  pendingFullRefresh = true;
  LOG_INF("PMD", "step %d: %s %d min", stepIndex,
          step.phase == PomodoroPhase::Work ? "work" : "break", step.minutes);
  requestUpdate();
}

void PomodoroActivity::advance() { startStep(stepIndex + 1); }

const char* PomodoroActivity::phaseLabel() const {
  switch (PomodoroSchedule::stepAt(stepIndex).phase) {
    case PomodoroPhase::Work:
      return tr(STR_POMODORO_WORK);
    case PomodoroPhase::LongBreak:
      return tr(STR_POMODORO_LONG_BREAK);
    case PomodoroPhase::ShortBreak:
    default:
      return tr(STR_POMODORO_BREAK);
  }
}

void PomodoroActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (waitingForNext && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    advance();
    return;
  }

  clock.updateMonotonic(millis());

  if (clock.finished() && !waitingForNext) {
    waitingForNext = true;
    pendingFullRefresh = true;  // the layout gains the "OK for next" hint
    requestUpdate();
    return;
  }

  if (clock.elapsedMinutes() != lastShownMinute) {
    lastShownMinute = clock.elapsedMinutes();
    requestUpdate();
  }
}

void PomodoroActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
  Rect header = TouchHeaderBackButton::standardHeaderRect(renderer);
  header.x = safe.x;
  header.width = safe.width;
  GUI.drawHeader(renderer, header, tr(STR_POMODORO));

  const CountdownLayout layout = computeCountdownLayout(renderer, mappedInput);
  drawCountdownRing(renderer, layout.cx, layout.cy, layout.outerRadius, layout.stroke, clock.fractionRemaining());

  char bigValue[16];
  if (clock.finished()) {
    char span[12];
    formatCountdownSpan(clock.overshootMinutes(), span, sizeof(span));
    snprintf(bigValue, sizeof(bigValue), "+%s", span);
  } else {
    formatCountdownSpan(clock.remainingMinutes(), bigValue, sizeof(bigValue));
  }

  const int valueHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int blockTop = layout.cy - (valueHeight + 4 + labelHeight) / 2;

  const std::string value =
      renderer.truncatedText(UI_12_FONT_ID, bigValue, layout.centerMaxWidth, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, blockTop, value.c_str(), true, EpdFontFamily::BOLD);

  const std::string label = renderer.truncatedText(SMALL_FONT_ID, phaseLabel(), layout.centerMaxWidth);
  renderer.drawCenteredText(SMALL_FONT_ID, blockTop + valueHeight + 4, label.c_str(), true);

  char line[80];
  if (waitingForNext) {
    snprintf(line, sizeof(line), "%s", tr(STR_POMODORO_NEXT_HINT));
  } else {
    snprintf(line, sizeof(line), I18N.get(StrId::STR_POMODORO_COUNT_FORMAT),
             PomodoroSchedule::pomodoroNumber(stepIndex));
  }
  const std::string context = renderer.truncatedText(SMALL_FONT_ID, line, layout.contextMaxWidth);
  renderer.drawCenteredText(SMALL_FONT_ID, layout.contextY, context.c_str(), true);

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), waitingForNext ? tr(STR_SELECT) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (clock.elapsedMinutes() - lastFullRefreshMinute >= kFullRefreshEveryMinutes) {
    lastFullRefreshMinute = clock.elapsedMinutes();
    pendingFullRefresh = true;
  }
  renderer.displayBuffer(pendingFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  pendingFullRefresh = false;
}
```

- [ ] **Step 4: Put the chooser between Home and the two modes**

In `src/activities/home/HomeActivity.cpp`, add `#include "../util/PomodoroActivity.h"` and
`#include "../util/OptionSelectionActivity.h"` next to the existing
`#include "../util/CountdownActivity.h"`, then replace `onCountdownOpen()` with:

```cpp
void HomeActivity::onCountdownOpen() {
  std::vector<std::string> modes{tr(STR_COUNTDOWN_MODE_TARGET), tr(STR_POMODORO)};
  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "CountdownMode",
                                                StrId::STR_COUNTDOWN_MODE_TITLE, std::move(modes), 0),
      [this](const ActivityResult& modeResult) {
        mappedInput.suppressNextConfirmRelease();
        const auto* choice = std::get_if<OptionSelectionResult>(&modeResult.data);
        if (modeResult.isCancelled || !choice) {
          requestUpdate();
          return;
        }
        auto done = [this](const ActivityResult&) {
          mappedInput.suppressNextConfirmRelease();
          requestUpdate();
        };
        if (choice->index == 0) {
          startActivityForResult(std::make_unique<CountdownActivity>(renderer, mappedInput), done);
        } else {
          startActivityForResult(std::make_unique<PomodoroActivity>(renderer, mappedInput), done);
        }
      });
}
```

- [ ] **Step 5: Build and run the host tests**

```bash
clang-format -i src/activities/util/PomodoroActivity.cpp src/activities/util/PomodoroActivity.h \
                src/activities/home/HomeActivity.cpp
pio run -e default
ctest --test-dir test/build -R "Countdown|Pomodoro" --output-on-failure
```

Expected: both SUCCESS.

- [ ] **Step 6: Measure the flash budget**

```bash
pio run -e default 2>&1 | grep -i "Flash:\|RAM:"
ls -l .pio/build/default/firmware.bin
```

`app0` is 6,553,600 bytes. Report the binary size and the percentage used. If it exceeds
the partition, stop and raise `partitions.csv` before going further — the symptom on device
would be `FIRMWARE_TOO_LARGE` at update time, not a build error.

- [ ] **Step 7: Flash and verify on hardware — both devices**

```bash
pio run -e default -t upload
```

Check on **X4** and again on **X3**:
1. Home → Countdown shows a two-line chooser: `Heure de fin` / `Pomodoro`.
2. `Heure de fin` behaves exactly as before the change.
3. `Pomodoro` starts a 25-minute work step immediately, ring whole, centre `25m`, label `Travail`, context `Pomodoro 1`.
4. Nothing overflows the screen edges on either line.
5. At the end of a step the screen does a full refresh, the centre shows `+0m` and the context line reads `Terminé - OK pour la suite`; the count keeps rising if left alone.
6. Pressing OK starts the break: label `Pause`, ring whole again.
7. `Back` leaves to Home from any state.

To exercise the long break without waiting two hours, temporarily set `kWorkMinutes` and the
break constants to 1 in `PomodoroSchedule.h`, walk through eight steps, and confirm the 8th
is `Pause longue` — then restore the constants, rebuild, and re-run the host tests.

- [ ] **Step 8: Update the changelog and commit**

Add under `## [Unreleased]` → `### Added` in `CHANGELOG.md`:

```markdown
- A `Pomodoro` mode in the Countdown screen, chosen from a small menu alongside the existing `Target time`. It runs the classic 25-minute work and 5-minute break cycle, with a 15-minute break after every fourth, and waits for a button press between each step rather than moving on by itself — a step that has finished keeps counting `+mm` so you can see how long it has been sitting done. Both modes now show a ring that drains instead of a bar that fills, with the remaining time inside it.
```

```bash
git add src/activities/util/PomodoroActivity.h src/activities/util/PomodoroActivity.cpp \
        src/activities/home/HomeActivity.cpp \
        lib/I18n/translations/english.yaml lib/I18n/translations/french.yaml CHANGELOG.md
git commit -m "feat: add a pomodoro mode behind a countdown mode chooser"
```

---

## Verification summary

| What | How |
|---|---|
| Timing arithmetic | `ctest --test-dir test/build -R "Countdown|Pomodoro"` — 15 tests (the filter must cover both suite name prefixes) |
| Builds for C3 | `pio run -e default` |
| Ring, layout, overflow | Flash and look, on **both** X3 and X4 |
| Flash budget | Task 5 Step 6, against 6,553,600 bytes |

No OTA release is published by this plan. Publishing happens only after the hardware checks
in Task 4 Step 6 and Task 5 Step 7 have passed on both devices.
