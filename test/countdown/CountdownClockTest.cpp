#include <gtest/gtest.h>

#include "CountdownClock.h"

TEST(CountdownClockSpan, TargetLaterTodayIsTheDifference) { EXPECT_EQ(CountdownClock::spanTo(700, 600), 100); }

TEST(CountdownClockSpan, TargetEarlierTodayMeansTomorrow) { EXPECT_EQ(CountdownClock::spanTo(600, 700), 1340); }

TEST(CountdownClockSpan, TargetEqualToNowMeansAFullDay) { EXPECT_EQ(CountdownClock::spanTo(600, 600), 1440); }

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
  EXPECT_FALSE(clock.finished());  // 50 of a 100-minute span
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
