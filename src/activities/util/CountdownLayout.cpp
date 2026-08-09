#include "CountdownLayout.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

CountdownLayout computeCountdownLayout(const GfxRenderer& renderer, const MappedInputManager& mappedInput) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
  const Rect header = TouchHeaderBackButton::standardHeaderRect(renderer);

  constexpr int kSideMargin = 24;
  constexpr int kContextGap = 16;

  const int contextHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int ringTop = header.y + header.height + 8;
  const int ringBottom = safe.y + safe.height - metrics.buttonHintsHeight - contextHeight - kContextGap;
  const int ringHeight = std::max(0, ringBottom - ringTop);

  CountdownLayout layout{};
  // Capped well below the space available. Filling the screen made the ring the
  // subject and the figure inside it an afterthought — the opposite of what the
  // screen is for.
  const int radiusCap = std::min(safe.width, safe.height) * 3 / 10;
  layout.outerRadius = std::max(0, std::min({safe.width / 2 - kSideMargin, ringHeight / 2, radiusCap}));
  layout.stroke = std::max(4, layout.outerRadius / 7);
  layout.cx = safe.x + safe.width / 2;
  layout.cy = ringTop + ringHeight / 2;
  layout.centerMaxWidth = std::max(16, 2 * (layout.outerRadius - layout.stroke) - 16);
  layout.contextY = ringBottom + kContextGap;
  layout.contextMaxWidth = std::max(16, safe.width - 2 * kSideMargin);
  return layout;
}

void drawCountdownCentre(const GfxRenderer& renderer, const CountdownLayout& layout, const char* value,
                         const char* label, const bool inverted) {
  const int valueHeight = renderer.getLineHeight(COUNTDOWN_VALUE_FONT_ID);
  const int labelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int blockTop = layout.cy - (valueHeight + 4 + labelHeight) / 2;

  const std::string shownValue =
      renderer.truncatedText(COUNTDOWN_VALUE_FONT_ID, value, layout.centerMaxWidth, EpdFontFamily::BOLD);
  renderer.drawCenteredText(COUNTDOWN_VALUE_FONT_ID, blockTop, shownValue.c_str(), true, EpdFontFamily::BOLD);

  const std::string shownLabel = renderer.truncatedText(SMALL_FONT_ID, label, layout.centerMaxWidth);
  renderer.drawCenteredText(SMALL_FONT_ID, blockTop + valueHeight + 4, shownLabel.c_str(), true);

  if (!inverted) return;

  // The start cue. Inverting the figure's own box rather than the whole screen
  // says "this timer began", where a full-screen flash only says "the screen
  // redrew" — and the panel already does that on the waveform change.
  const int valueWidth = renderer.getTextWidth(COUNTDOWN_VALUE_FONT_ID, shownValue.c_str(), EpdFontFamily::BOLD);
  constexpr int kPad = 8;
  renderer.invertRect(layout.cx - valueWidth / 2 - kPad, blockTop - kPad / 2, valueWidth + 2 * kPad,
                      valueHeight + kPad);
}
