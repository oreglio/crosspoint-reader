#include "CountdownLayout.h"

#include <GfxRenderer.h>

#include <algorithm>

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
  layout.outerRadius = std::max(0, std::min(safe.width / 2 - kSideMargin, ringHeight / 2));
  layout.stroke = std::max(4, layout.outerRadius / 7);
  layout.cx = safe.x + safe.width / 2;
  layout.cy = ringTop + ringHeight / 2;
  layout.centerMaxWidth = std::max(16, 2 * (layout.outerRadius - layout.stroke) - 16);
  layout.contextY = ringBottom + kContextGap;
  layout.contextMaxWidth = std::max(16, safe.width - 2 * kSideMargin);
  return layout;
}
