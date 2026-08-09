#include <gtest/gtest.h>

#include <cstring>

#include "CountdownClock.h"

TEST(CountdownClockSpan, TargetLaterTodayIsTheDifference) { EXPECT_EQ(CountdownClock::spanTo(700, 600), 100); }

TEST(CountdownClockSpan, TargetEarlierTodayMeansTomorrow) { EXPECT_EQ(CountdownClock::spanTo(600, 700), 1340); }

TEST(CountdownClockSpan, TargetEqualToNowMeansAFullDay) { EXPECT_EQ(CountdownClock::spanTo(600, 600), 1440); }

TEST(CountdownClock, CountsDownToTheSecond) {
  CountdownClock clock;
  clock.start(/*startMs=*/1000, /*spanMinutes=*/25);
  EXPECT_EQ(clock.remainingSeconds(), 25 * 60);
  EXPECT_FALSE(clock.finished());

  clock.update(1000 + 20'000);
  EXPECT_EQ(clock.elapsedSeconds(), 20);
  EXPECT_EQ(clock.remainingSeconds(), 25 * 60 - 20);
  EXPECT_EQ(clock.elapsedMinutes(), 0);

  clock.update(1000 + 90'000);
  EXPECT_EQ(clock.elapsedSeconds(), 90);
  EXPECT_EQ(clock.elapsedMinutes(), 1);
}

TEST(CountdownClock, OvershootCountsUpPastTheTarget) {
  CountdownClock clock;
  clock.start(0, 2);
  clock.update(150'000);  // 2m30 into a 2m span
  EXPECT_TRUE(clock.finished());
  EXPECT_EQ(clock.remainingSeconds(), 0);
  EXPECT_EQ(clock.overshootSeconds(), 30);
}

TEST(CountdownClock, SurvivesTheMillisWrap) {
  CountdownClock clock;
  const unsigned long nearMax = 0xFFFFFF00UL;
  clock.start(nearMax, 25);
  clock.update(nearMax + 120'000UL);  // wraps through zero
  EXPECT_EQ(clock.elapsedSeconds(), 120);
}

TEST(CountdownClockFraction, GoesFromOneToZeroAndStopsThere) {
  CountdownClock clock;
  clock.start(0, 20);
  EXPECT_FLOAT_EQ(clock.fractionRemaining(), 1.0f);
  clock.update(10 * 60'000);
  EXPECT_FLOAT_EQ(clock.fractionRemaining(), 0.5f);
  clock.update(30 * 60'000);
  EXPECT_FLOAT_EQ(clock.fractionRemaining(), 0.0f);
}

TEST(CountdownClockFormat, ARemainingValueHoldsUntilAWholeStepHasPassed) {
  // The bug this replaces: a 10-minute countdown read 9:55 one second in,
  // because the shown value was floored. Counting down, the figure must hold
  // until a whole step has actually elapsed — that is the ceiling, not the floor.
  char buf[16];
  formatCountdownSeconds(countdownShownRemaining(600), buf, sizeof(buf));
  EXPECT_STREQ(buf, "10:00");
  formatCountdownSeconds(countdownShownRemaining(599), buf, sizeof(buf));
  EXPECT_STREQ(buf, "10:00");
  formatCountdownSeconds(countdownShownRemaining(596), buf, sizeof(buf));
  EXPECT_STREQ(buf, "10:00");
  formatCountdownSeconds(countdownShownRemaining(595), buf, sizeof(buf));
  EXPECT_STREQ(buf, "9:55");
  formatCountdownSeconds(countdownShownRemaining(591), buf, sizeof(buf));
  EXPECT_STREQ(buf, "9:55");
  formatCountdownSeconds(countdownShownRemaining(590), buf, sizeof(buf));
  EXPECT_STREQ(buf, "9:50");
}

TEST(CountdownClockFormat, AnElapsedValueRoundsTheOtherWay) {
  // Counting up, the last step actually reached is the floor: an overshoot of
  // one second is not yet five seconds of overtime.
  char buf[16];
  formatCountdownSeconds(countdownShownElapsed(0), buf, sizeof(buf));
  EXPECT_STREQ(buf, "0:00");
  formatCountdownSeconds(countdownShownElapsed(4), buf, sizeof(buf));
  EXPECT_STREQ(buf, "0:00");
  formatCountdownSeconds(countdownShownElapsed(5), buf, sizeof(buf));
  EXPECT_STREQ(buf, "0:05");
  formatCountdownSeconds(countdownShownElapsed(9), buf, sizeof(buf));
  EXPECT_STREQ(buf, "0:05");
}

TEST(CountdownClockFormat, EverythingLandsOnFivesAndZeroes) {
  char buf[16];
  for (int s = 0; s <= 600; ++s) {
    formatCountdownSeconds(countdownShownRemaining(s), buf, sizeof(buf));
    const int lastDigit = buf[strlen(buf) - 1] - '0';
    EXPECT_TRUE(lastDigit == 0 || lastDigit == 5) << "remaining " << s << " showed " << buf;
  }
}

TEST(CountdownClockFormat, ShowsSecondsBelowAnHourAndHoursAbove) {
  char buf[16];
  formatCountdownSeconds(countdownShownRemaining(0), buf, sizeof(buf));
  EXPECT_STREQ(buf, "0:00");
  formatCountdownSeconds(countdownShownRemaining(70), buf, sizeof(buf));
  EXPECT_STREQ(buf, "1:10");
  formatCountdownSeconds(countdownShownRemaining(25 * 60), buf, sizeof(buf));
  EXPECT_STREQ(buf, "25:00");
  // At an hour and beyond the seconds stop earning their place.
  formatCountdownSeconds(countdownShownRemaining(3600), buf, sizeof(buf));
  EXPECT_STREQ(buf, "1h00");
  formatCountdownSeconds(countdownShownRemaining(3600 + 5 * 60 + 40), buf, sizeof(buf));
  EXPECT_STREQ(buf, "1h05");
  // Just under the hour the ceiling reaches it, and must read as an hour rather
  // than as sixty minutes.
  formatCountdownSeconds(countdownShownRemaining(3599), buf, sizeof(buf));
  EXPECT_STREQ(buf, "1h00");
}

TEST(CountdownClockFormat, TheRepaintTickFollowsTheFormat) {
  // Below an hour the display carries seconds, so it must tick every 10s;
  // above it only the hour and minute show, so once a minute is enough.
  // The tick is taken from the already-snapped value, so it changes exactly when
  // the string does and never once more.
  EXPECT_EQ(countdownDisplayTick(countdownShownRemaining(600)), countdownDisplayTick(countdownShownRemaining(596)));
  EXPECT_NE(countdownDisplayTick(countdownShownRemaining(600)), countdownDisplayTick(countdownShownRemaining(595)));
  // Crossing the hour must not repaint twice for the same string.
  EXPECT_EQ(countdownDisplayTick(countdownShownRemaining(3600)), countdownDisplayTick(countdownShownRemaining(3599)));
  EXPECT_NE(countdownDisplayTick(countdownShownRemaining(3600)), countdownDisplayTick(countdownShownRemaining(3590)));
}
