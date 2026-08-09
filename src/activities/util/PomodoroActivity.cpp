#include "PomodoroActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "CountdownLayout.h"
#include "CountdownRing.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PomodoroActivity::onEnter() {
  Activity::onEnter();
  startStep(0);
}

void PomodoroActivity::startStep(const int index) {
  stepIndex = index;
  waitingForNext = false;
  const PomodoroStep step = PomodoroSchedule::stepAt(stepIndex);
  clock.startMonotonic(millis(), step.minutes);
  lastShownMinute = -1;
  lastFullRefreshMinute = 0;
  pendingFullRefresh = true;
  LOG_INF("PMD", "step %d: %s %d min", stepIndex, step.phase == PomodoroPhase::Work ? "work" : "break", step.minutes);
  requestUpdate();
}

const char* PomodoroActivity::phaseLabel() const {
  switch (PomodoroSchedule::stepAt(stepIndex).phase) {
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

  if (waitingForNext && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startStep(stepIndex + 1);
    return;
  }

  clock.updateMonotonic(millis());

  if (clock.finished() && !waitingForNext) {
    waitingForNext = true;
    pendingFullRefresh = true;  // the layout gains the "OK for next" hint
    requestUpdate();
    return;
  }

  if (clock.elapsedMinutes() != lastShownMinute) {
    lastShownMinute = clock.elapsedMinutes();
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
  drawCountdownRing(renderer, layout.cx, layout.cy, layout.outerRadius, layout.stroke, clock.fractionRemaining());

  char bigValue[16];
  if (clock.finished()) {
    char span[12];
    formatCountdownSpan(clock.overshootMinutes(), span, sizeof(span));
    snprintf(bigValue, sizeof(bigValue), "+%s", span);
  } else {
    formatCountdownSpan(clock.remainingMinutes(), bigValue, sizeof(bigValue));
  }

  const int valueHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int blockTop = layout.cy - (valueHeight + 4 + labelHeight) / 2;

  const std::string value = renderer.truncatedText(UI_12_FONT_ID, bigValue, layout.centerMaxWidth, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, blockTop, value.c_str(), true, EpdFontFamily::BOLD);

  const std::string label = renderer.truncatedText(SMALL_FONT_ID, phaseLabel(), layout.centerMaxWidth);
  renderer.drawCenteredText(SMALL_FONT_ID, blockTop + valueHeight + 4, label.c_str(), true);

  char line[80];
  if (waitingForNext) {
    snprintf(line, sizeof(line), "%s", tr(STR_POMODORO_NEXT_HINT));
  } else {
    snprintf(line, sizeof(line), I18N.get(StrId::STR_POMODORO_COUNT_FORMAT),
             PomodoroSchedule::pomodoroNumber(stepIndex));
  }
  const std::string context = renderer.truncatedText(SMALL_FONT_ID, line, layout.contextMaxWidth);
  renderer.drawCenteredText(SMALL_FONT_ID, layout.contextY, context.c_str(), true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), waitingForNext ? tr(STR_SELECT) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (clock.elapsedMinutes() - lastFullRefreshMinute >= kFullRefreshEveryMinutes) {
    lastFullRefreshMinute = clock.elapsedMinutes();
    pendingFullRefresh = true;
  }
  renderer.displayBuffer(pendingFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  pendingFullRefresh = false;
}
