#include "PomodoroActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "CountdownLayout.h"
#include "CountdownRing.h"
#include "CrossPointState.h"
#include "IntervalSelectionActivity.h"
#include "OptionSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PomodoroActivity::onEnter() {
  Activity::onEnter();
  durations = {APP_STATE.pomodoroWorkMinutes, APP_STATE.pomodoroShortBreakMinutes, APP_STATE.pomodoroLongBreakMinutes};
  manualStart = APP_STATE.pomodoroManualStart;
  askPreset();
}

namespace {
// "25 / 5 / 15" — the three lengths of a preset, in the order they are lived.
void formatPreset(const PomodoroDurations& d, char* buf, const size_t len) {
  snprintf(buf, len, "%u / %u / %u", static_cast<unsigned>(d.work), static_cast<unsigned>(d.shortBreak),
           static_cast<unsigned>(d.longBreak));
}

bool sameDurations(const PomodoroDurations& a, const PomodoroDurations& b) {
  return a.work == b.work && a.shortBreak == b.shortBreak && a.longBreak == b.longBreak;
}
}  // namespace

// No suppressNextConfirmRelease() here. ActivityManager calls the current
// activity's loop() before it processes pending actions, so an activity pushed
// from a result handler first runs a frame later, by which time the release is
// gone. Arming the flag would have nothing to swallow and would eat the user's
// next genuine press instead — which is exactly what made Select look dead on
// the chained pickers.
void PomodoroActivity::askPreset() {
  const PomodoroDurations presets[3] = {PomodoroSchedule::kClassic, PomodoroSchedule::kShort, PomodoroSchedule::kLong};
  const StrId names[3] = {StrId::STR_POMODORO_PRESET_CLASSIC, StrId::STR_POMODORO_PRESET_SHORT,
                          StrId::STR_POMODORO_PRESET_LONG};

  std::vector<std::string> rows;
  rows.reserve(4);
  char line[48];
  char lengths[24];
  for (int i = 0; i < 3; ++i) {
    formatPreset(presets[i], lengths, sizeof(lengths));
    snprintf(line, sizeof(line), "%s  %s", I18N.get(names[i]), lengths);
    rows.emplace_back(line);
  }
  formatPreset(durations, lengths, sizeof(lengths));
  snprintf(line, sizeof(line), "%s  %s", tr(STR_POMODORO_PRESET_CUSTOM), lengths);
  rows.emplace_back(line);

  // The chaining mode carries its own state on the row. A screen of its own would
  // cost every launch a press, including for anyone who never changes it.
  snprintf(line, sizeof(line), "%s  %s", tr(STR_POMODORO_CHAINING),
           manualStart ? tr(STR_POMODORO_CHAINING_MANUAL) : tr(STR_POMODORO_CHAINING_AUTO));
  rows.emplace_back(line);

  // Start on whichever row matches what was last used, so the common case is a
  // single press and the custom row shows the lengths it would reuse.
  uint8_t selected = 3;
  for (int i = 0; i < 3; ++i) {
    if (sameDurations(durations, presets[i])) {
      selected = static_cast<uint8_t>(i);
      break;
    }
  }

  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "PomodoroPreset",
                                                StrId::STR_POMODORO_PRESET_TITLE, std::move(rows), selected),
      [this, presets](const ActivityResult& result) {
        const auto* choice = std::get_if<OptionSelectionResult>(&result.data);
        if (result.isCancelled || !choice) {
          finish();
          return;
        }
        if (choice->index < 3) {
          durations = presets[choice->index];
          applyAndStart();
          return;
        }
        if (choice->index == 4) {
          // Flip and come straight back, so the new state is visible before a
          // length is chosen to start with.
          manualStart = !manualStart;
          APP_STATE.pomodoroManualStart = manualStart;
          APP_STATE.saveToFile();
          askPreset();
          return;
        }
        askCustom(CustomField::Work);
      });
}

void PomodoroActivity::askCustom(const CustomField field) {
  StrId title = StrId::STR_POMODORO_CUSTOM_WORK;
  int initial = durations.work;
  if (field == CustomField::ShortBreak) {
    title = StrId::STR_POMODORO_CUSTOM_SHORT;
    initial = durations.shortBreak;
  } else if (field == CustomField::LongBreak) {
    title = StrId::STR_POMODORO_CUSTOM_LONG;
    initial = durations.longBreak;
  }

  startActivityForResult(std::make_unique<IntervalSelectionActivity>(
                             renderer, mappedInput, "PomodoroCustom", title, initial, PomodoroSchedule::kMinMinutes,
                             PomodoroSchedule::kMaxMinutes,
                             /*smallStep=*/1, /*largeStep=*/5, StrId::STR_SLEEP_TIMER_VALUE_FORMAT),
                         [this, field](const ActivityResult& result) {
                           const auto* picked = std::get_if<IntervalResult>(&result.data);
                           if (result.isCancelled || !picked) {
                             // Back out one field at a time, and off the screen from the first.
                             if (field == CustomField::Work) {
                               finish();
                             } else {
                               askCustom(field == CustomField::LongBreak ? CustomField::ShortBreak : CustomField::Work);
                             }
                             return;
                           }
                           const auto minutes = static_cast<uint8_t>(picked->value);
                           if (field == CustomField::Work) {
                             durations.work = minutes;
                             askCustom(CustomField::ShortBreak);
                           } else if (field == CustomField::ShortBreak) {
                             durations.shortBreak = minutes;
                             askCustom(CustomField::LongBreak);
                           } else {
                             durations.longBreak = minutes;
                             applyAndStart();
                           }
                         });
}

void PomodoroActivity::applyAndStart() {
  APP_STATE.pomodoroWorkMinutes = durations.work;
  APP_STATE.pomodoroShortBreakMinutes = durations.shortBreak;
  APP_STATE.pomodoroLongBreakMinutes = durations.longBreak;
  APP_STATE.pomodoroManualStart = manualStart;
  APP_STATE.saveToFile();
  // The first step waits to be started. Later ones begin on the press that
  // acknowledges the previous one, so a cycle still costs one press per change.
  prepareStep(0, Gate::Ready);
}

void PomodoroActivity::prepareStep(const int index, const Gate initialGate) {
  stepIndex = index;
  gate = initialGate;
  const PomodoroStep step = PomodoroSchedule::stepAt(durations, stepIndex);
  clock.start(millis(), step.minutes);
  lastDisplayTick = -1;
  repaintsSinceFullRefresh = 0;
  pendingFullRefresh = true;
  LOG_INF("PMD", "step %d: %s %d min (%s)", stepIndex, step.phase == PomodoroPhase::Work ? "work" : "break",
          step.minutes, initialGate == Gate::Ready ? "ready" : "running");
  requestUpdate();
}

void PomodoroActivity::beginRunning() {
  gate = Gate::Running;
  blinkUntilMs = millis() + kStartBlinkMs;
  // Restart the clock here, not when the step was prepared: the countdown must
  // measure from the press, however long the screen sat waiting for it.
  clock.start(millis(), PomodoroSchedule::stepAt(durations, stepIndex).minutes);
  lastDisplayTick = -1;
  repaintsSinceFullRefresh = 0;
  pendingFullRefresh = true;
  requestUpdate();
}

const char* PomodoroActivity::phaseLabel() const {
  switch (PomodoroSchedule::stepAt(durations, stepIndex).phase) {
    case PomodoroPhase::Work:
      return tr(STR_POMODORO_WORK);
    case PomodoroPhase::LongBreak:
      return tr(STR_POMODORO_LONG_BREAK);
    case PomodoroPhase::ShortBreak:
    default:
      return tr(STR_POMODORO_BREAK);
  }
}

void PomodoroActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (gate == Gate::Ready) {
      beginRunning();
      return;
    }
    if (gate == Gate::Finished || gate == Gate::Running) {
      // Running too: a step you no longer need should not have to be waited out.
      // In manual chaining the next step waits for its own press rather than
      // starting on the one that acknowledged this one.
      prepareStep(stepIndex + 1, manualStart ? Gate::Ready : Gate::Running);
      return;
    }
  }

  if (gate != Gate::Running) return;  // nothing ticks while a gate is held

  clock.update(millis());

  // One repaint to end the start cue, then the normal cadence takes over.
  if (blinkUntilMs != 0 && millis() >= blinkUntilMs) {
    blinkUntilMs = 0;
    requestUpdate();
    return;
  }

  if (clock.finished()) {
    gate = Gate::Finished;
    pendingFullRefresh = true;  // the layout gains the "OK for next" hint
    requestUpdate();
    return;
  }

  const int tick = countdownDisplayTick(clock.finished() ? countdownShownElapsed(clock.overshootSeconds())
                                                         : countdownShownRemaining(clock.remainingSeconds()));
  if (tick != lastDisplayTick) {
    lastDisplayTick = tick;
    requestUpdate();
  }
}

void PomodoroActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
  Rect header = TouchHeaderBackButton::standardHeaderRect(renderer);
  header.x = safe.x;
  header.width = safe.width;
  GUI.drawHeader(renderer, header, tr(STR_POMODORO));

  const CountdownLayout layout = computeCountdownLayout(renderer, mappedInput);
  drawCountdownRing(renderer, layout.cx, layout.cy, layout.outerRadius, layout.stroke,
                    gate == Gate::Ready ? 1.0f : clock.fractionRemaining());

  char bigValue[16];
  if (gate == Gate::Ready) {
    // Show the whole step ahead of it, not a countdown that has not begun.
    formatCountdownSeconds(PomodoroSchedule::stepAt(durations, stepIndex).minutes * 60, bigValue, sizeof(bigValue));
  } else if (clock.finished()) {
    char span[12];
    formatCountdownSeconds(countdownShownElapsed(clock.overshootSeconds()), span, sizeof(span));
    snprintf(bigValue, sizeof(bigValue), "+%s", span);
  } else {
    formatCountdownSeconds(countdownShownRemaining(clock.remainingSeconds()), bigValue, sizeof(bigValue));
  }

  drawCountdownCentre(renderer, layout, bigValue, phaseLabel(), millis() < blinkUntilMs);

  char line[80];
  if (gate == Gate::Ready) {
    snprintf(line, sizeof(line), "%s", tr(STR_POMODORO_START_HINT));
  } else if (gate == Gate::Finished) {
    snprintf(line, sizeof(line), "%s", tr(STR_POMODORO_NEXT_HINT));
  } else {
    snprintf(line, sizeof(line), I18N.get(StrId::STR_POMODORO_COUNT_FORMAT),
             PomodoroSchedule::pomodoroNumber(stepIndex));
  }
  const std::string context = renderer.truncatedText(SMALL_FONT_ID, line, layout.contextMaxWidth);
  renderer.drawCenteredText(SMALL_FONT_ID, layout.contextY, context.c_str(), true);

  const char* confirmLabel = tr(STR_SELECT);
  if (gate == Gate::Running) confirmLabel = tr(STR_POMODORO_SKIP);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (++repaintsSinceFullRefresh >= kRepaintsPerFullRefresh) {
    repaintsSinceFullRefresh = 0;
    pendingFullRefresh = true;
  }
  renderer.displayBuffer(pendingFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  pendingFullRefresh = false;
}
