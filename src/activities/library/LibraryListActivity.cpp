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
  uiReady = false;
  app.setTheme(uiThemeTokens(uiTarget));
  app.on(ACTION_ROW, &LibraryListActivity::onRowEvent, this);
  app.setScreen(&LibraryListActivity::listScreen, this);

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

void LibraryListActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LibraryListActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->index.bookCount())) return;
  self->selectedIndex = event.value;
  self->app.clearTapFlash();
  self->openSelectedBook();
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

void LibraryListActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<LibraryListActivity*>(user)->buildListScreen(screen);
}

void LibraryListActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) +
                                       metrics.verticalSpacing),
                  0, static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing), 0});

  const int count = static_cast<int>(index.bookCount());
  fui::ListProps props;
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  // One line for the title, one for the author, always. A wrapped title would
  // push the author column out of alignment, which is the one property this
  // screen exists to provide; the renderer ellipsises on measured width instead.
  // One line, and it has to stay one line. FreeInkUI sizes the label band as
  // exactly lineHeight(labelFont) whenever a subtitle is present
  // (components/lists/list.h:269-279), so a second title line is drawn outside
  // its band and lands on top of the author. A wrapped title and a fixed author
  // column are mutually exclusive in this widget; the column wins, because it is
  // the thing that makes the list scannable. Long titles ellipsise on measured
  // width, which stays correct at every theme and UI scale.
  // The author gets its own, visibly smaller style. With both lines at the same
  // weight the rows read as one undifferentiated block and it stops being
  // obvious which line is the title — the separation is what makes the column
  // scannable, which is the entire point of the two-slot row.
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 1;
  // Remember the geometry so render() can rule between rows: the list widget has
  // no separator of its own, and reproducing its layout after the fact is the
  // only way to add one without editing shared SDK code.
  lastListTop = screen.body().y;
  lastRowStep = 0;  // filled in below, once configureUiList has sized the row
  const auto rows = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  lastRowStep = props.rowHeight + props.rowGap;
  topIndex = scrollListBy(topIndex, 0, visibleRows, count);

  // Materialise only the visible window. Titles and authors are derived here
  // rather than stored, so the index keeps one copy of each name.
  const int drawCount = std::min(visibleRows, count - topIndex);
  rowText.assign(drawCount <= 0 ? 0 : static_cast<size_t>(drawCount), RowText{});
  uiItems.clear();
  uiItems.reserve(rowText.size());

  for (int i = 0; i < drawCount; i++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(topIndex + i));
    library::ClixRecord record{};
    std::string name;
    if (ordinal != 0xFFFF && index.readRecord(ordinal, record) && index.readName(record, name)) {
      const library::ParsedName parsed = library::parseFilename(stemOf(name));
      rowText[i].title = parsed.title;
      rowText[i].author = library::cleanPersonName(parsed.author);
    }
    if (rowText[i].title.empty()) rowText[i].title = tr(STR_LIBRARY_UNKNOWN_TITLE);

    fui::ListItem item;
    // A book glyph on every row anchors the eye at a fixed left edge, so a row
    // reads as one object rather than two loose lines.
    item.icon = listIconFor(Book, 24);
    item.label = rowText[i].title.c_str();
    // An empty subtitle still reserves the line, so rows stay a uniform height
    // and the author column stays where the eye expects it.
    item.subtitle = rowText[i].author.c_str();
    item.actionValue = static_cast<int16_t>(topIndex + i);
    uiItems.push_back(item);
  }

  props.items = uiItems.data();
  props.count = static_cast<uint16_t>(uiItems.size());
  props.topIndex = 0;  // the window is already the visible slice
  props.selectedIndex = static_cast<int16_t>(selectedIndex - topIndex);
  screen.list(props);
}

void LibraryListActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finishAfterBackPress();
    return;
  }
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
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
    uiReady = false;
    app.render();
    uiReady = true;
    drawRowSeparators();
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void LibraryListActivity::drawRowSeparators() {
  if (lastRowStep <= 0) return;
  const int drawn = std::min(visibleRows, static_cast<int>(index.bookCount()) - topIndex);
  const int width = renderer.getScreenWidth();
  // Dotted, not solid: on a 1-bit panel every-other-pixel is how you get a rule
  // that reads as grey. A solid black line would weigh more than the text it is
  // meant to separate.
  for (int row = 1; row < drawn; row++) {
    const int y = lastListTop + row * lastRowStep - 1;
    for (int x = LIBRARY_SEPARATOR_INSET; x < width - LIBRARY_SEPARATOR_INSET; x += 2) {
      renderer.drawPixel(x, y, true);
    }
  }
}
