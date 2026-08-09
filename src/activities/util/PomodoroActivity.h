#pragma once

#include "CountdownClock.h"
#include "MappedInputManager.h"
#include "PomodoroSchedule.h"
#include "activities/Activity.h"

class GfxRenderer;

// The 25/5/15 sequence, advanced by hand: each step counts down, then waits for
// OK before the next one starts. Nothing advances on its own, so a step that has
// finished keeps counting "+mm" — that is the useful figure when you come back
// to the device and want to know how long it has been sitting done.
//
// millis() is the time base regardless of device: a pomodoro is a duration, not
// a time of day, so the X3's RTC buys nothing here and the X4 needs no fallback.
class PomodoroActivity final : public Activity {
 public:
  PomodoroActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Pomodoro", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  // The panel stays up for the whole session, including while a finished step
  // waits for a press — so an unattended session never auto-sleeps.
  bool preventAutoSleep() override { return true; }
  // Nothing happens between frames but a millis() read, so the CPU can idle at
  // LOW_POWER_FREQ. The first button press restores full speed.
  bool allowPowerSavingWhileAwake() const override { return true; }

 private:
  // A screen repainting once a minute for hours accumulates fast-refresh
  // residue; clear the panel properly twice an hour.
  static constexpr int kFullRefreshEveryMinutes = 30;

  // Three distinct situations, not two flags: a step can be waiting to be
  // started, running, or finished and waiting to be acknowledged.
  enum class Gate : uint8_t { Ready, Running, Finished };
  // Which of the three lengths the custom flow is currently asking for.
  enum class CustomField : uint8_t { Work, ShortBreak, LongBreak };

  PomodoroDurations durations = PomodoroSchedule::kClassic;
  int stepIndex = 0;
  Gate gate = Gate::Ready;
  CountdownClock clock;
  int lastShownMinute = -1;
  int lastFullRefreshMinute = 0;
  bool pendingFullRefresh = true;

  void askPreset();
  void askCustom(CustomField field);
  void applyAndStart();
  void prepareStep(int index, Gate initialGate);
  void beginRunning();
  const char* phaseLabel() const;
};
