#include <gtest/gtest.h>

#include "PomodoroSchedule.h"

namespace {
constexpr PomodoroDurations kClassic{25, 5, 15};
constexpr PomodoroDurations kShort{15, 3, 10};
}  // namespace

TEST(PomodoroScheduleSteps, EvenStepsAreWork) {
  EXPECT_EQ(PomodoroSchedule::stepAt(kClassic, 0).phase, PomodoroPhase::Work);
  EXPECT_EQ(PomodoroSchedule::stepAt(kClassic, 0).minutes, 25);
  EXPECT_EQ(PomodoroSchedule::stepAt(kClassic, 2).phase, PomodoroPhase::Work);
  EXPECT_EQ(PomodoroSchedule::stepAt(kClassic, 2).minutes, 25);
}

TEST(PomodoroScheduleSteps, BreaksAfterTheFirstThreePomodorosAreShort) {
  EXPECT_EQ(PomodoroSchedule::stepAt(kClassic, 1).phase, PomodoroPhase::ShortBreak);
  EXPECT_EQ(PomodoroSchedule::stepAt(kClassic, 1).minutes, 5);
  EXPECT_EQ(PomodoroSchedule::stepAt(kClassic, 3).phase, PomodoroPhase::ShortBreak);
  EXPECT_EQ(PomodoroSchedule::stepAt(kClassic, 5).phase, PomodoroPhase::ShortBreak);
}

TEST(PomodoroScheduleSteps, EveryFourthBreakIsLong) {
  // step 7 is the break after the 4th work phase
  EXPECT_EQ(PomodoroSchedule::stepAt(kClassic, 7).phase, PomodoroPhase::LongBreak);
  EXPECT_EQ(PomodoroSchedule::stepAt(kClassic, 7).minutes, 15);
  // step 15 is the break after the 8th
  EXPECT_EQ(PomodoroSchedule::stepAt(kClassic, 15).phase, PomodoroPhase::LongBreak);
  // and the one before it is not
  EXPECT_EQ(PomodoroSchedule::stepAt(kClassic, 13).phase, PomodoroPhase::ShortBreak);
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
  EXPECT_EQ(PomodoroSchedule::stepAt(kClassic, -1).phase, PomodoroPhase::Work);
  EXPECT_EQ(PomodoroSchedule::pomodoroNumber(-1), 1);
}

TEST(PomodoroScheduleDurations, UsesTheDurationsItIsGiven) {
  EXPECT_EQ(PomodoroSchedule::stepAt(kShort, 0).minutes, 15);
  EXPECT_EQ(PomodoroSchedule::stepAt(kShort, 1).minutes, 3);
  EXPECT_EQ(PomodoroSchedule::stepAt(kShort, 7).minutes, 10);
  EXPECT_EQ(PomodoroSchedule::stepAt(kShort, 7).phase, PomodoroPhase::LongBreak);
}

TEST(PomodoroScheduleDurations, ClampsNonsenseToSomethingUsable) {
  const PomodoroDurations zero{0, 0, 0};
  EXPECT_GE(PomodoroSchedule::stepAt(zero, 0).minutes, 1);
  EXPECT_GE(PomodoroSchedule::stepAt(zero, 1).minutes, 1);
  EXPECT_GE(PomodoroSchedule::stepAt(zero, 7).minutes, 1);
}
