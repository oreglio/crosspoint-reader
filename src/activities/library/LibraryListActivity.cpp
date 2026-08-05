#include "LibraryListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryText.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/listIcons.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {

std::string stemOf(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  return (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
}

}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Library", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void LibraryListActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  app.setTheme(uiThemeTokens(uiTarget));

  // Optimistic open: if an index exists, paint from it immediately and let the
  // user decide when to refresh. Only a missing or unreadable index forces the
  // walk, so entering the screen is normally instant.
  indexReady = openIndex();
  if (!indexReady) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    indexReady = rebuildIndex() && openIndex();
  }
  degraded = indexReady && index.ranksDegraded();
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  index.close();
  rowText.clear();
  rowText.shrink_to_fit();
  uiItems.clear();
  uiItems.shrink_to_fit();
  Activity::onExit();
}

bool LibraryListActivity::openIndex() {
  index.close();
  return index.open(library::libraryIndexPath());
}

bool LibraryListActivity::rebuildIndex() {
  // Carry the monotonic counter forward so "recently added" ordering survives a
  // rebuild: a book that was already on the card must not jump to the top.
  uint16_t carriedFirstSeen = 0;
  {
    library::LibraryIndexFile previous;
    if (previous.open(library::libraryIndexPath())) carriedFirstSeen = previous.header().nextFirstSeen;
  }
  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex("/", carriedFirstSeen, stats);
  if (ok && (stats.duplicatesDropped > 0 || stats.unreadableSkipped > 0)) {
    LOG_INF("LIB", "card has %u duplicate and %u unreadable entries",
            static_cast<unsigned>(stats.duplicatesDropped), static_cast<unsigned>(stats.unreadableSkipped));
  }
  return ok;
}

void LibraryListActivity::openSelectedBook() {
  if (!indexReady) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(selectedIndex));
  if (ordinal == 0xFFFF) return;

  library::ClixRecord record{};
  std::string path;
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", selectedIndex);
    return;
  }
  // Release the index handle first: on hardware SdFat allows one open reader per
  // path at a time, and the reader is about to open files of its own.
  index.close();
  indexReady = false;
  activityManager.goToReader(std::move(path));
}

void LibraryListActivity::cycleSortOrder() {
  switch (sortOrder) {
    case library::SortOrder::DateDesc:
      sortOrder = library::SortOrder::TitleAsc;
      break;
    case library::SortOrder::TitleAsc:
      sortOrder = library::SortOrder::TitleDesc;
      break;
    case library::SortOrder::TitleDesc:
      sortOrder = library::SortOrder::AuthorAsc;
      break;
    case library::SortOrder::AuthorAsc:
      sortOrder = library::SortOrder::DateDesc;
      break;
  }
  selectedIndex = 0;
  topIndex = 0;
}

const char* LibraryListActivity::sortOrderLabel() const {
  switch (sortOrder) {
    case library::SortOrder::DateDesc:
      return tr(STR_LIBRARY_SORT_RECENT);
    case library::SortOrder::TitleAsc:
      return tr(STR_LIBRARY_SORT_TITLE_AZ);
    case library::SortOrder::TitleDesc:
      return tr(STR_LIBRARY_SORT_TITLE_ZA);
    case library::SortOrder::AuthorAsc:
      return tr(STR_LIBRARY_SORT_AUTHOR);
  }
  return "";
}

// Row geometry, computed from the live renderer so it follows the UI font-size
// setting. Two title lines plus one author line, uniform for every row: uniform
// is what keeps the author at the same x and y on every row, which is the whole
// reason this screen exists.
// Height of one row, given how many title lines it actually needs. Rows are
// variable: reserving a second title line for a one-line title leaves a hole
// between the title and its own author, which reads as a layout bug.
//
// The author still sits at a fixed LEFT edge on every row — that is the column
// the eye sweeps. Its vertical position follows its title, which is what makes
// the pair read as one object.
int LibraryListActivity::rowHeightFor(const int titleLines, const bool hasAuthor) const {
  return titleLineH * titleLines + (hasAuthor ? authorLineH : 0) + LIBRARY_ROW_PADDING;
}

void LibraryListActivity::measureRows() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
  authorLineH = renderer.getLineHeight(SMALL_FONT_ID);
  listTop = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) + metrics.verticalSpacing;
  listHeight = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - listTop;
}

// Title and author for one entry, read straight from the index. Only ever called
// for rows about to be drawn, so at most a screenful of strings exists at once.
bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author) {
  title.clear();
  author.clear();
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(entry));
  library::ClixRecord record{};
  std::string name;
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record) && index.readName(record, name)) {
    const library::ParsedName parsed = library::parseFilename(stemOf(name));
    title = parsed.title;
    author = library::cleanPersonName(parsed.author);
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

void LibraryListActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finishAfterBackPress();
    return;
  }
  const int count = static_cast<int>(index.bookCount());

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }
  if (count > 0 && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelectedBook();
    return;
  }
  // Left and Right are aliases of Up and Down on this hardware, so the sort
  // control lives on a Confirm hold rather than a second axis that does not
  // exist.
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= 800) {
    cycleSortOrder();
    requestUpdate(true);
    return;
  }

  // Left and Right page. On this hardware ButtonNavigator maps them as aliases of
  // Up and Down (util/ButtonNavigator.h:47-53), so the second axis is unused and
  // paging is free — which matters at 69 books, where scrolling one row at a time
  // is 34 presses to the middle and paging is 5.
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) && count > 0) {
    selectedIndex = std::min(count - 1, selectedIndex + visibleRows);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, count);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && count > 0) {
    selectedIndex = std::max(0, selectedIndex - visibleRows);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, count);
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up) && selectedIndex > 0) {
    selectedIndex--;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, count);
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down) && selectedIndex < count - 1) {
    selectedIndex++;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, count);
    requestUpdate();
  }
}

void LibraryListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const char* title = degraded ? tr(STR_LIBRARY_TITLE_UNSORTED) : sortOrderLabel();
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, title, true);
  } else {
    GUI.drawHeader(renderer, header, title);
  }

  if (index.bookCount() == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_LIBRARY_EMPTY));
  } else {
    drawRows();
  }

  drawPositionReadout();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void LibraryListActivity::drawRows() {
  measureRows();
  const int count = static_cast<int>(index.bookCount());
  if (topIndex > selectedIndex) topIndex = selectedIndex;
  const int width = renderer.getScreenWidth();
  const int textX = LIBRARY_SIDE_PADDING + LIBRARY_ICON_SIZE + LIBRARY_ICON_GAP;
  const int textW = width - textX - LIBRARY_SIDE_PADDING;

  // Rows have content-dependent heights, so the page is filled by accumulating
  // them rather than by dividing the band. A row is only drawn if it fits whole:
  // a half-drawn row at the bottom edge would look like a rendering fault.
  std::string title;
  std::string author;
  int y = listTop;
  int drawn = 0;
  for (int entry = topIndex; entry < count; entry++) {
    rowTextFor(entry, title, author);
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, title.c_str(), textW, LIBRARY_TITLE_LINES);
    const int height = rowHeightFor(static_cast<int>(lines.size()), !author.empty());
    if (drawn > 0 && y + height > listTop + listHeight) break;

    if (entry == selectedIndex) {
      renderer.fillRoundedRect(LIBRARY_SIDE_PADDING / 2, y, width - LIBRARY_SIDE_PADDING, height - 2, 6,
                               Color::LightGray);
    }
    renderer.drawIcon(icon_book_24_bits, LIBRARY_SIDE_PADDING, y + (height - LIBRARY_ICON_SIZE) / 2,
                      LIBRARY_ICON_SIZE);

    int textY = y + LIBRARY_ROW_PADDING / 2;
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, textX, textY, line.c_str(), true);
      textY += titleLineH;
    }
    if (!author.empty()) {
      const std::string fitted = renderer.truncatedText(SMALL_FONT_ID, author.c_str(), textW);
      renderer.drawText(SMALL_FONT_ID, textX, textY, fitted.c_str(), true);
    }

    y += height;
    drawn++;

    // Dotted, not solid: on a 1-bit panel every-other-pixel is how a rule reads
    // grey. A solid line would outweigh the text it separates.
    if (entry + 1 < count && y + LIBRARY_ROW_PADDING < listTop + listHeight) {
      for (int x = LIBRARY_SIDE_PADDING; x < width - LIBRARY_SIDE_PADDING; x += 2) {
        renderer.drawPixel(x, y - 1, true);
      }
    }
  }

  // Paging and Up/Down both need to know how much of the list one screen holds.
  // It is only knowable after drawing, since it depends on the titles on screen.
  visibleRows = drawn > 0 ? drawn : 1;
  if (selectedIndex >= topIndex + visibleRows) topIndex = selectedIndex - visibleRows + 1;
}

// "3/6" at the bottom right. Rows are variable height, so a page is not a fixed
// number of books and the count has to be estimated from what the current screen
// actually holds — an honest approximation is worth more here than a precise
// number nobody can act on. It also tells the user how far the list goes, which
// a scrollbar on a 1-bit panel says far less clearly.
void LibraryListActivity::drawPositionReadout() {
  const int count = static_cast<int>(index.bookCount());
  if (count <= 0 || visibleRows <= 0) return;

  const int page = selectedIndex / visibleRows + 1;
  const int pages = (count + visibleRows - 1) / visibleRows;
  if (pages <= 1) return;

  char buf[16];
  snprintf(buf, sizeof(buf), "%d/%d", page, pages);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
  const int x = renderer.getScreenWidth() - width - LIBRARY_SIDE_PADDING;
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, x, y, buf, true);
}
