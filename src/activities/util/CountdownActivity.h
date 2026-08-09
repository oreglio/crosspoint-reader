#pragma once

#include <cstdint>

#include "CountdownClock.h"
#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class GfxRenderer;

// Counts down to a wall-clock time the user picks, then keeps counting the
// overshoot as "+mm".
//
// The X3 carries a DS3231, so it knows the current time and only asks for the
// end time. The X4 has no RTC at all — and both run the same binary — so there
// it asks for the current time first and counts with millis() instead. Either
// way the user states an end time and the screen works out the span.
//
// Nothing is accumulated frame to frame: the elapsed count is recomputed from
// the time base on every tick, at one-minute granularity, which also keeps the
// e-ink panel down to one refresh per minute.
class CountdownActivity final : public Activity {
 public:
  CountdownActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Countdown", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Keep the panel alive while counting; the user is watching the screen.
  bool preventAutoSleep() override { return phase == Phase::Running; }
  // Nothing happens between frames but a clock read, so the CPU can idle at
  // LOW_POWER_FREQ. The first button press restores full speed.
  bool allowPowerSavingWhileAwake() const override { return phase == Phase::Running; }

 private:
  enum class Phase : uint8_t {
    PickNowHour,       // clockless devices only: what time is it right now
    PickNowMinute,     //   "
    PickTargetHour,    // the hour to count down to
    PickTargetMinute,  // the minutes to count down to
    Running,           // counting down, then counting the overshoot
  };

  static constexpr int kMinutesPerDay = 1440;
  static constexpr int kMinuteStep = 1;
  // Front buttons move one unit, side buttons a coarse one. With wrap-around in
  // both directions that puts any hour within 5 presses and any minute within 7,
  // against 12 and 30 if both pairs did the same thing. A coarse minute step of
  // ten also means the side buttons drive the tens digit and the front buttons
  // the units, which is how people read a clock anyway.
  static constexpr int kHourLargeStep = 6;
  static constexpr int kMinuteLargeSlots = 10;  // 10 slots of 1 min = 10 min
  static constexpr int kDefaultSpanMinutes = 30;

  Phase phase = Phase::PickTargetHour;
  // True when this device has an RTC (X3). False (X4) means the user supplied
  // the current time by hand and millis() is the time base.
  bool useWallClock = true;

  int nowHour = 0;  // clockless devices: the current time as the user stated it
  int nowMinute = 0;
  int targetHour = 0;
  int targetMinute = 0;

  CountdownClock clock;      // owns the time base and the elapsed arithmetic
  int lastShownMinute = -1;  // suppresses redundant e-ink refreshes
  // clearScreen() wipes the framebuffer, not the panel: a fast refresh leaves
  // the previous screen's ink behind wherever the new one draws nothing. Every
  // change of layout therefore has to ask for a full waveform.
  bool pendingFullRefresh = true;
  bool wasOvertime = false;
  int lastFullRefreshMinute = 0;
  // A countdown is stared at for hours at one repaint a minute, so fast-refresh
  // residue would pile up. Clear the panel properly twice an hour.
  static constexpr int kFullRefreshEveryMinutes = 30;

  ButtonNavigator buttonNavigator;

  // Local minute-of-day from the RTC, or -1 when no RTC is available.
  static int localMinuteOfDay();

  bool editingHourField() const { return phase == Phase::PickNowHour || phase == Phase::PickTargetHour; }
  bool editingNow() const { return phase == Phase::PickNowHour || phase == Phase::PickNowMinute; }

  void adjustField(int delta);
  void advancePhase();
  bool retreatPhase();  // false when there is nowhere left to go back to
  void startCountdown();
  void refreshElapsed();

  void renderPicker();
  void renderRunning();
};
