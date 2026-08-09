#include "CountdownClock.h"

#include <algorithm>
#include <cstdio>

namespace {
constexpr int kSecondsPerHour = 3600;
// Below an hour the display carries seconds and has to keep up with them;
// above it the minute is the smallest thing shown.
constexpr int kFineTickSeconds = 10;
}  // namespace

int CountdownClock::spanTo(const int targetMinuteOfDay, const int fromMinuteOfDay) {
  int span = targetMinuteOfDay - fromMinuteOfDay;
  if (span <= 0) span += kMinutesPerDay;
  return span;
}

void CountdownClock::start(const unsigned long startMs, const int spanMinutes) {
  startMs_ = startMs;
  spanMinutes_ = std::max(1, spanMinutes);
  elapsedSeconds_ = 0;
}

void CountdownClock::update(const unsigned long nowMs) {
  // Unsigned subtraction, so the ~49-day millis() wrap resolves correctly.
  elapsedSeconds_ = static_cast<int>((nowMs - startMs_) / 1000UL);
}

int CountdownClock::remainingSeconds() const { return std::max(0, spanSeconds() - elapsedSeconds_); }

int CountdownClock::overshootSeconds() const { return std::max(0, elapsedSeconds_ - spanSeconds()); }

float CountdownClock::fractionRemaining() const {
  const int span = spanSeconds();
  if (span <= 0) return 0.0f;
  return std::clamp(static_cast<float>(remainingSeconds()) / static_cast<float>(span), 0.0f, 1.0f);
}

void formatCountdownRemaining(const int seconds, char* buf, const size_t len) {
  const int total = std::max(0, seconds);
  if (total >= kSecondsPerHour) {
    snprintf(buf, len, "%dh%02d", total / kSecondsPerHour, (total % kSecondsPerHour) / 60);
    return;
  }
  // Snapped to the same ten-second grid the repaint uses. The repaint fires one
  // second past each boundary, so the unsnapped value would always read 59, 49,
  // 39 — technically right and visibly wrong. Deriving both from one division
  // means the shown figure and the moment it changes cannot drift apart.
  const int snapped = total / kFineTickSeconds * kFineTickSeconds;
  snprintf(buf, len, "%d:%02d", snapped / 60, snapped % 60);
}

int countdownDisplayTick(const int seconds) {
  const int total = std::max(0, seconds);
  return total >= kSecondsPerHour ? total / 60 : total / kFineTickSeconds;
}
