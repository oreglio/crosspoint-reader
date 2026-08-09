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
