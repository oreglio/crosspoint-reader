#include "CountdownActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "CountdownLayout.h"
#include "CountdownRing.h"
#include "CrossPointSettings.h"
#include "DeviceCapabilities.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

int CountdownActivity::localMinuteOfDay() {
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (!halClock.getTime(hour, minute)) return -1;

  // getTime() returns the raw RTC value, which is kept in UTC. The user picks a
  // target on the wall clock they can see, so both sides have to be local.
  uint8_t biasedOffset = SETTINGS.clockUtcOffsetQ;
  if (biasedOffset > 104) biasedOffset = 104;
  const int offsetQuarterHours = static_cast<int>(biasedOffset) - 48;
  const int total = static_cast<int>(hour) * 60 + static_cast<int>(minute) + offsetQuarterHours * 15;
  return ((total % kMinutesPerDay) + kMinutesPerDay) % kMinutesPerDay;
}

void CountdownActivity::onEnter() {
  Activity::onEnter();

  const int now = localMinuteOfDay();
  // The X3 carries a DS3231; the X4 has no RTC and cannot even sync over NTP
  // (HalClock::syncFromNTP bails on !_available). Both run the same binary, so
  // this is a runtime decision: without a clock we have to ask what time it is.
  useWallClock = now >= 0;

  if (useWallClock) {
    nowHour = now / 60;
    nowMinute = now % 60;
    phase = Phase::PickTargetHour;
  } else {
    LOG_INF("CDN", "no RTC on this device, asking the user for the current time");
    nowHour = 12;
    nowMinute = 0;
    phase = Phase::PickNowHour;
  }

  // Seed the target a sensible span ahead so it is a few presses away, not a
  // dozen. On clockless devices this is re-seeded once the user states the time.
  const int seed = (nowHour * 60 + nowMinute + kDefaultSpanMinutes) % kMinutesPerDay;
  targetHour = seed / 60;
  targetMinute = (seed % 60) / kMinuteStep * kMinuteStep;

  requestUpdate();
}

void CountdownActivity::adjustField(const int delta) {
  int& hourField = editingNow() ? nowHour : targetHour;
  int& minuteField = editingNow() ? nowMinute : targetMinute;

  if (editingHourField()) {
    hourField = ((hourField + delta) % 24 + 24) % 24;
  } else {
    const int slots = 60 / kMinuteStep;
    int slot = minuteField / kMinuteStep + delta;
    slot = ((slot % slots) + slots) % slots;
    minuteField = slot * kMinuteStep;
  }
  requestUpdate();
}

void CountdownActivity::advancePhase() {
  switch (phase) {
    case Phase::PickNowHour:
      phase = Phase::PickNowMinute;
      break;
    case Phase::PickNowMinute: {
      // Now that the current time is known, re-seed the target ahead of it.
      const int seed = (nowHour * 60 + nowMinute + kDefaultSpanMinutes) % kMinutesPerDay;
      targetHour = seed / 60;
      targetMinute = (seed % 60) / kMinuteStep * kMinuteStep;
      phase = Phase::PickTargetHour;
      break;
    }
    case Phase::PickTargetHour:
      phase = Phase::PickTargetMinute;
      break;
    case Phase::PickTargetMinute:
      startCountdown();
      return;
    case Phase::Running:
      return;
  }
  requestUpdate();
}

bool CountdownActivity::retreatPhase() {
  switch (phase) {
    case Phase::PickNowMinute:
      phase = Phase::PickNowHour;
      break;
    case Phase::PickTargetHour:
      if (useWallClock) return false;  // nothing was asked before this step
      phase = Phase::PickNowMinute;
      break;
    case Phase::PickTargetMinute:
      phase = Phase::PickTargetHour;
      break;
    case Phase::PickNowHour:
    case Phase::Running:
      return false;
  }
  requestUpdate();
  return true;
}

void CountdownActivity::startCountdown() {
  const int target = targetHour * 60 + targetMinute;
  int reference;

  if (useWallClock) {
    const int now = localMinuteOfDay();
    // The RTC answered in onEnter(); if it stops answering now, the stated time
    // is the best reference left rather than a failed countdown.
    reference = now >= 0 ? now : nowHour * 60 + nowMinute;
    clock.start(millis(), CountdownClock::spanTo(target, reference));
  } else {
    reference = nowHour * 60 + nowMinute;
    // millis() is a safe time base here: the activity blocks deep sleep while
    // counting, so the counter cannot be reset under us, and esp_timer is
    // compensated across the CPU frequency drop to LOW_POWER_FREQ.
    clock.start(millis(), CountdownClock::spanTo(target, reference));
  }

  lastDisplayTick = -1;
  wasOvertime = false;
  repaintsSinceFullRefresh = 0;
  blinkUntilMs = millis() + kStartBlinkMs;
  // The picker's hint lines sit almost exactly where the running screen puts
  // "Elapsed", so a fast refresh here superimposes the two.
  pendingFullRefresh = true;
  phase = Phase::Running;
  LOG_INF("CDN", "countdown started: %02d:%02d -> %02d:%02d (%d min, %s)", reference / 60, reference % 60, targetHour,
          targetMinute, clock.spanMinutes(), useWallClock ? "RTC" : "millis");
  requestUpdate();
}

void CountdownActivity::refreshElapsed() {
  clock.update(millis());

  // One repaint to end the start cue, then the normal cadence takes over.
  if (blinkUntilMs != 0 && millis() >= blinkUntilMs) {
    blinkUntilMs = 0;
    requestUpdate();
    return;
  }

  // Repaint exactly when the rendered string would change: every 10s while the
  // display carries seconds, every minute once it only shows hours.
  const int tick = countdownDisplayTick(clock.finished() ? countdownShownElapsed(clock.overshootSeconds())
                                                         : countdownShownRemaining(clock.remainingSeconds()));
  if (tick != lastDisplayTick) {
    lastDisplayTick = tick;
    requestUpdate();
  }
}

void CountdownActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!retreatPhase()) finish();
    return;
  }

  if (phase == Phase::Running) {
    refreshElapsed();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    advancePhase();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustField(-1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustField(1); });

  // On edge-button boards (X3, X4 Pro) the side buttons sit on the left/right edges of the screen
  // rather than as a vertical up/down rocker (X4), so BTN_UP is physically the left button. Flip the
  // large-step direction there so the left button always decreases, matching IntervalSelectionActivity.
  const int largeStep = editingHourField() ? kHourLargeStep : kMinuteLargeSlots;
  const int upDelta = deviceHasEdgeSideButtons(gpio) ? -largeStep : largeStep;
  const int downDelta = deviceHasEdgeSideButtons(gpio) ? largeStep : -largeStep;
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this, upDelta] { adjustField(upDelta); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                       [this, downDelta] { adjustField(downDelta); });
}

void CountdownActivity::renderPicker() {
  const int screenWidth = renderer.getScreenWidth();
  const bool onNow = editingNow();

  char hourText[4];
  char minuteText[4];
  snprintf(hourText, sizeof(hourText), "%02d", onNow ? nowHour : targetHour);
  snprintf(minuteText, sizeof(minuteText), "%02d", onNow ? nowMinute : targetMinute);

  const int hourWidth = renderer.getTextWidth(UI_12_FONT_ID, hourText, EpdFontFamily::BOLD);
  const int sepWidth = renderer.getTextWidth(UI_12_FONT_ID, ":", EpdFontFamily::BOLD);
  const int minuteWidth = renderer.getTextWidth(UI_12_FONT_ID, minuteText, EpdFontFamily::BOLD);

  constexpr int valueY = 140;
  const int startX = std::max(0, (screenWidth - (hourWidth + sepWidth + minuteWidth)) / 2);
  renderer.drawText(UI_12_FONT_ID, startX, valueY, hourText, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, startX + hourWidth, valueY, ":", true, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, startX + hourWidth + sepWidth, valueY, minuteText, true, EpdFontFamily::BOLD);

  // Underline marks which half the buttons are currently driving.
  const int underlineY = valueY + renderer.getLineHeight(UI_12_FONT_ID) + 4;
  if (editingHourField()) {
    renderer.fillRect(startX, underlineY, hourWidth, 3);
  } else {
    renderer.fillRect(startX + hourWidth + sepWidth, underlineY, minuteWidth, 3);
  }

  StrId promptId;
  switch (phase) {
    case Phase::PickNowHour:
      promptId = StrId::STR_COUNTDOWN_PICK_NOW_HOUR;
      break;
    case Phase::PickNowMinute:
      promptId = StrId::STR_COUNTDOWN_PICK_NOW_MINUTE;
      break;
    case Phase::PickTargetHour:
      promptId = StrId::STR_COUNTDOWN_PICK_HOUR;
      break;
    default:
      promptId = StrId::STR_COUNTDOWN_PICK_MINUTE;
      break;
  }
  renderer.drawCenteredText(SMALL_FONT_ID, underlineY + 28, I18N.get(promptId), true);

  // Two-line step hint, same shape as IntervalSelectionActivity: front buttons do
  // the small step, side buttons the coarse one. Built from separate label and
  // value strings so the layout doesn't depend on a translated separator.
  const bool onHour = editingHourField();
  const char unit = onHour ? 'h' : 'm';
  const int smallStep = onHour ? 1 : kMinuteStep;
  const int largeStep = onHour ? kHourLargeStep : kMinuteStep * kMinuteLargeSlots;
  char hint[64];
  snprintf(hint, sizeof(hint), "%s %d%c", tr(STR_STEP_HINT_FRONT), smallStep, unit);
  renderer.drawCenteredText(SMALL_FONT_ID, underlineY + 56, hint, true);
  snprintf(hint, sizeof(hint), "%s %d%c", tr(STR_STEP_HINT_SIDE), largeStep, unit);
  renderer.drawCenteredText(SMALL_FONT_ID, underlineY + 78, hint, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CountdownActivity::renderRunning() {
  const CountdownLayout layout = computeCountdownLayout(renderer, mappedInput);
  const bool overtime = clock.finished();

  drawCountdownRing(renderer, layout.cx, layout.cy, layout.outerRadius, layout.stroke, clock.fractionRemaining());

  char bigValue[16];
  if (overtime) {
    char span[12];
    formatCountdownSeconds(countdownShownElapsed(clock.overshootSeconds()), span, sizeof(span));
    snprintf(bigValue, sizeof(bigValue), "+%s", span);
  } else {
    formatCountdownSeconds(countdownShownRemaining(clock.remainingSeconds()), bigValue, sizeof(bigValue));
  }

  drawCountdownCentre(renderer, layout, bigValue, overtime ? tr(STR_COUNTDOWN_OVERTIME) : tr(STR_COUNTDOWN_REMAINING),
                      millis() < blinkUntilMs);

  char elapsedText[12];
  formatCountdownSeconds(countdownShownElapsed(std::min(clock.elapsedSeconds(), clock.spanSeconds())), elapsedText,
                         sizeof(elapsedText));
  char line[80];
  snprintf(line, sizeof(line), "%s %02d:%02d \xC2\xB7 %s %s", tr(STR_COUNTDOWN_TARGET), targetHour, targetMinute,
           tr(STR_COUNTDOWN_ELAPSED), elapsedText);
  const std::string context = renderer.truncatedText(SMALL_FONT_ID, line, layout.contextMaxWidth);
  renderer.drawCenteredText(SMALL_FONT_ID, layout.contextY, context.c_str(), true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CountdownActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
  Rect header = TouchHeaderBackButton::standardHeaderRect(renderer);
  header.x = safe.x;
  header.width = safe.width;
  GUI.drawHeader(renderer, header, tr(STR_COUNTDOWN_TITLE));

  if (phase == Phase::Running) {
    // The label flips from "remaining" to "over time" and the value gains a "+",
    // so the overtime crossing is a layout change like any other.
    const bool overtime = clock.finished();
    if (overtime != wasOvertime) {
      wasOvertime = overtime;
      pendingFullRefresh = true;
    }
    // Counted in repaints, not minutes: at one every ten seconds the residue
    // from fast refreshes builds six times faster than it used to.
    if (++repaintsSinceFullRefresh >= kRepaintsPerFullRefresh) {
      repaintsSinceFullRefresh = 0;
      pendingFullRefresh = true;
    }
    renderRunning();
  } else {
    renderPicker();
  }

  renderer.displayBuffer(pendingFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  pendingFullRefresh = false;
}
