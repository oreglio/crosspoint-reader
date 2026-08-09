#pragma once

#include <cstddef>

// Elapsed/remaining arithmetic for a countdown, with no I/O of its own: the
// caller reads the clock and hands the reading in. That keeps the HAL out of
// here and makes the whole class testable on the host with no stubs.
//
// Two time bases, because the devices differ. The X3 has a DS3231 and counts in
// minute-of-day; the X4 has no clock at all and counts with millis(), which is
// safe here only because the countdown screens block deep sleep while running.
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
