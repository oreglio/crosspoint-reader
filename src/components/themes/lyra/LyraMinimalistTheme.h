#pragma once

#include "components/themes/lyra/Lyra3CoversTheme.h"

class GfxRenderer;

// Lyra Extended without the covers: the top of Home lists the recent books by
// the title and author their metadata carries, one line each.
namespace LyraMinimalistMetrics {
constexpr int kMaxBooks = 5;
constexpr int kLabelHeight = 34;

// Home's menu shows up to seven rows per page whatever room is left, so the
// list must stay within 330 px or the last row lands on the button hints.
// Five books therefore get slightly tighter rows than four.
constexpr int rowHeight(int books) { return books >= 5 ? 58 : 66; }

// Height of the book list above the menu. The count comes from a setting, so
// UITheme patches it into the metrics it hands out (see UITheme::getMetrics).
constexpr int topAreaHeight(int books) { return kLabelHeight + books * rowHeight(books); }
static_assert(topAreaHeight(kMaxBooks) <= 330, "the book list would push Home's menu onto the button hints");

constexpr ThemeMetrics values = [] {
  ThemeMetrics v = Lyra3CoversMetrics::values;
  v.homeRecentBooksCount = kMaxBooks;
  v.homeCoverTileHeight = topAreaHeight(kMaxBooks);
  return v;
}();
}  // namespace LyraMinimalistMetrics

class LyraMinimalistTheme : public LyraTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;
};
