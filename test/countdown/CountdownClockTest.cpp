#include <gtest/gtest.h>

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

TEST(CountdownClockFormat, ShowsSecondsBelowAnHourAndHoursAbove) {
  char buf[16];
  formatCountdownRemaining(0, buf, sizeof(buf));
  EXPECT_STREQ(buf, "0:00");
  formatCountdownRemaining(9, buf, sizeof(buf));
  EXPECT_STREQ(buf, "0:09");
  formatCountdownRemaining(70, buf, sizeof(buf));
  EXPECT_STREQ(buf, "1:10");
  formatCountdownRemaining(25 * 60, buf, sizeof(buf));
  EXPECT_STREQ(buf, "25:00");
  formatCountdownRemaining(59 * 60 + 59, buf, sizeof(buf));
  EXPECT_STREQ(buf, "59:59");
  // At an hour and beyond the seconds stop earning their place.
  formatCountdownRemaining(3600, buf, sizeof(buf));
  EXPECT_STREQ(buf, "1h00");
  formatCountdownRemaining(3600 + 5 * 60 + 40, buf, sizeof(buf));
  EXPECT_STREQ(buf, "1h05");
}

TEST(CountdownClockFormat, TheRepaintTickFollowsTheFormat) {
  // Below an hour the display carries seconds, so it must tick every 10s;
  // above it only the hour and minute show, so once a minute is enough.
  // 1500..1509 all floor into the same ten-second bucket; 1499 does not.
  EXPECT_EQ(countdownDisplayTick(1500), countdownDisplayTick(1509));
  EXPECT_NE(countdownDisplayTick(1500), countdownDisplayTick(1499));
  // Past the hour the bucket is a whole minute.
  EXPECT_EQ(countdownDisplayTick(7200), countdownDisplayTick(7259));
  EXPECT_NE(countdownDisplayTick(7200), countdownDisplayTick(7199));
}
