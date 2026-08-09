#pragma once

#include <cstdint>

enum class PomodoroPhase : uint8_t { Work, ShortBreak, LongBreak };

struct PomodoroStep {
  PomodoroPhase phase;
  int minutes;
};

// The three lengths a session is made of. Persisted in CrossPointState rather
// than in the settings list: these are the last values chosen, not a knob to go
// browsing for.
struct PomodoroDurations {
  uint8_t work;
  uint8_t shortBreak;
  uint8_t longBreak;
};

// The sequence, expressed as a pure function of the step index. Even indices are
// work phases, odd indices the break that follows; every fourth break is the long
// one. Nothing is persisted per session — leaving the screen ends it — so an
// index is all the state the activity has to keep.
struct PomodoroSchedule {
  static constexpr PomodoroDurations kClassic{25, 5, 15};
  static constexpr PomodoroDurations kShort{15, 3, 10};
  static constexpr PomodoroDurations kLong{50, 10, 20};

  static constexpr int kMinMinutes = 1;
  static constexpr int kMaxMinutes = 120;
  static constexpr int kPomodorosPerLongBreak = 4;

  static PomodoroStep stepAt(const PomodoroDurations& durations, const int stepIndex) {
    const int index = stepIndex < 0 ? 0 : stepIndex;
    if (index % 2 == 0) {
      return {PomodoroPhase::Work, clamp(durations.work)};
    }
    const int completedWork = index / 2 + 1;
    if (completedWork % kPomodorosPerLongBreak == 0) {
      return {PomodoroPhase::LongBreak, clamp(durations.longBreak)};
    }
    return {PomodoroPhase::ShortBreak, clamp(durations.shortBreak)};
  }

  // Which pomodoro this step belongs to, counting from 1. A break belongs to the
  // work phase it follows, so the number does not jump mid-cycle.
  static int pomodoroNumber(const int stepIndex) {
    const int index = stepIndex < 0 ? 0 : stepIndex;
    return index / 2 + 1;
  }

  // A stored zero — an empty state file, a hand-edited JSON — must not produce a
  // step that finishes the instant it starts.
  static int clamp(const uint8_t minutes) {
    if (minutes < kMinMinutes) return kMinMinutes;
    if (minutes > kMaxMinutes) return kMaxMinutes;
    return minutes;
  }
};
