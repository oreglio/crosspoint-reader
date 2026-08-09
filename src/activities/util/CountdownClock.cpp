#include "CountdownClock.h"

#include <algorithm>
#include <cstdio>

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
