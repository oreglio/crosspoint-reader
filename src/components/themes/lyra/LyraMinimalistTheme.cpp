#include "LyraMinimalistTheme.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int cornerRadius = 6;
constexpr int rowGap = 4;
constexpr int textInset = 14;
constexpr int titleAuthorGap = 2;
constexpr int labelRuleHeight = 2;
}  // namespace

void LyraMinimalistTheme::drawRecentBookCover(
    GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks, int selectorIndex,
    bool& /*coverRendered*/, bool& /*coverBufferStored*/, bool& /*bufferRestored*/,
    const std::function<bool()>& /*storeCoverBuffer*/, const BookReadingStats* /*stats*/, float /*progressPercent*/,
    const GlobalReadingStats* /*globalStats*/, const char* /*currentChapterTitle*/) const {
  // Titles are cheap to redraw, so nothing here touches the cover snapshot:
  // coverRendered stays false and Home simply draws this list every time.
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = rect.x + metrics.contentSidePadding;
  const int width = rect.width - 2 * metrics.contentSidePadding;
  const int textWidth = width - 2 * textInset;

  const int labelLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int ruleY = rect.y + LyraMinimalistMetrics::kLabelHeight - labelRuleHeight - rowGap;
  renderer.drawText(UI_10_FONT_ID, x + textInset, ruleY - labelLineHeight - 4, tr(STR_CONTINUE_READING), true,
                    EpdFontFamily::BOLD);
  renderer.fillRect(x, ruleY, width, labelRuleHeight, true);

  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int authorLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  // Rows are sized for the configured count, not for how many books exist, so
  // the menu below stays put as recents come and go.
  const int rowPitch = LyraMinimalistMetrics::rowHeight(metrics.homeRecentBooksCount);
  const int rowHeight = rowPitch - rowGap;
  const int count = std::min(static_cast<int>(recentBooks.size()), metrics.homeRecentBooksCount);

  for (int i = 0; i < count; ++i) {
    const RecentBook& book = recentBooks[i];
    const int rowY = rect.y + LyraMinimalistMetrics::kLabelHeight + i * rowPitch;
    TouchRegistry::getInstance().add(Rect{x, rowY, width, rowHeight}, i, TouchRegistry::Cover);

    if (selectorIndex == i) {
      renderer.fillRoundedRect(x, rowY, width, rowHeight, cornerRadius, Color::LightGray);
    }

    const bool hasAuthor = !book.author.empty();
    const int blockHeight = titleLineHeight + (hasAuthor ? titleAuthorGap + authorLineHeight : 0);
    const int titleY = rowY + (rowHeight - blockHeight) / 2;
    const std::string title = renderer.truncatedText(UI_12_FONT_ID, book.title.c_str(), textWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + textInset, titleY, title.c_str(), true, EpdFontFamily::BOLD);

    if (hasAuthor) {
      const std::string author = renderer.truncatedText(SMALL_FONT_ID, book.author.c_str(), textWidth);
      renderer.drawText(SMALL_FONT_ID, x + textInset, titleY + titleLineHeight + titleAuthorGap, author.c_str(), true);
    }
  }
}
