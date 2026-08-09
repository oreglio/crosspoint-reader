#pragma once

#include <cstddef>

// Elapsed/remaining arithmetic for a countdown, with no I/O of its own: the
// caller reads millis() and hands the reading in. That keeps the HAL out of here
// and makes the whole class testable on the host with no stubs.
//
// One time base for every device. The X3 has a DS3231 and the X4 has no clock at
// all, but that difference only decides how the *target* is expressed — once a
// countdown is running the screen blocks deep sleep, so millis() cannot be reset
// underneath it and is the same on both. It also carries seconds, which the RTC
// does not expose at all.
class CountdownClock {
 public:
  static constexpr int kMinutesPerDay = 1440;

  // Minutes from `fromMinuteOfDay` to the next occurrence of
  // `targetMinuteOfDay`. Equal or earlier means tomorrow, so this never returns
  // zero or a negative: picking the current time reads as a 24-hour span rather
  // than an instantly finished countdown.
  static int spanTo(int targetMinuteOfDay, int fromMinuteOfDay);

  void start(unsigned long startMs, int spanMinutes);
  void update(unsigned long nowMs);

  int spanMinutes() const { return spanMinutes_; }
  int spanSeconds() const { return spanMinutes_ * 60; }
  int elapsedSeconds() const { return elapsedSeconds_; }
  int elapsedMinutes() const { return elapsedSeconds_ / 60; }
  int remainingSeconds() const;
  int overshootSeconds() const;
  bool finished() const { return elapsedSeconds_ >= spanSeconds(); }

  // 1.0 at the start, 0.0 at the target and beyond.
  float fractionRemaining() const;

 private:
  int spanMinutes_ = 1;
  int elapsedSeconds_ = 0;
  unsigned long startMs_ = 0;
};

// A countdown must show the last grid point it has actually reached. Counting
// down that is the ceiling — 10:00 holds until a whole step has elapsed — and
// counting up it is the floor, since one second of overtime is not yet five.
// Rounding both the same way is what made a fresh 10-minute countdown read 9:55
// a second after it started.
//
// Above an hour the value passes through untouched: only hours and minutes are
// shown there, so there is nothing to snap.
int countdownShownRemaining(int seconds);
int countdownShownElapsed(int seconds);

// Formats an already-snapped value: "9:55" below an hour, "1h05" at an hour and
// above, where seconds would be noise.
void formatCountdownSeconds(int shownSeconds, char* buf, size_t len);

// Changes exactly when the formatted string would change, and never once more —
// both are taken from the same snapped value, so the panel cadence cannot drift
// from what is written on it.
int countdownDisplayTick(int shownSeconds);
