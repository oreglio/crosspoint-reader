#include "LibraryListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryText.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/listIcons.h"

namespace fui = freeink::ui;

namespace {}  // namespace

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
    // Held across the popup and the build, for the same reason the Settings
    // rebuild holds it: the render task's SD-loaded fonts read glyph data at
    // draw time, and the walk needs the card to itself.
    RenderLock lock(*this);
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    indexReady = rebuildIndex() && openIndex();
  }
  degraded = indexReady && index.ranksDegraded();
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  index.close();
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
  const bool ok = library::buildLibraryIndex(
      "/", carriedFirstSeen, stats, SETTINGS.libraryUseMetadata != 0,
      [](const uint16_t booksSoFar, const char*, void*) {
        // Let the idle task run so the task watchdog stays fed: its panic
        // timeout is 5 s and a metadata walk can run longer than that.
        if ((booksSoFar & 31u) == 0) delay(1);
        return true;
      },
      nullptr);
  if (ok) {
    LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed (%u dup, %u unreadable)",
            static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
            static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
            static_cast<unsigned>(stats.duplicatesDropped), static_cast<unsigned>(stats.unreadableSkipped));
  } else {
    LOG_ERR("LIB", "index build failed");
  }
  return ok;
}

void LibraryListActivity::openSelectedBook() {
  if (!indexReady) return;
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(selectedIndex)));
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

void LibraryListActivity::openSortMenu() {
  // Degraded means every order IS discovery order: the strip is hidden, and
  // the hold must not offer a choice that would repaint the same list under a
  // different title.
  if (degraded) return;
  std::vector<std::string> options{tr(STR_LIBRARY_SORT_RECENT), tr(STR_LIBRARY_SORT_TITLE_AZ),
                                   tr(STR_LIBRARY_SORT_TITLE_ZA), tr(STR_LIBRARY_SORT_AUTHOR)};
  const auto current = static_cast<uint8_t>(sortOrder == library::SortOrder::DateDesc    ? 0
                                            : sortOrder == library::SortOrder::TitleAsc  ? 1
                                            : sortOrder == library::SortOrder::TitleDesc ? 2
                                                                                         : 3);
  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "LibrarySort", StrId::STR_LIBRARY_SORT_TITLE,
                                                std::move(options), current),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          requestUpdate(true);
          return;
        }
        switch (std::get<OptionSelectionResult>(result.data).index) {
          case 1:
            sortOrder = library::SortOrder::TitleAsc;
            break;
          case 2:
            sortOrder = library::SortOrder::TitleDesc;
            break;
          case 3:
            sortOrder = library::SortOrder::AuthorAsc;
            break;
          default:
            sortOrder = library::SortOrder::DateDesc;
            break;
        }
        // A new order invalidates every remembered page boundary: the same
        // ordinal is now somewhere else entirely. It also invalidates the filter,
        // which holds POSITIONS in the old order — cycleSortOrder refilters and
        // this path did not, so sorting while a search was active left the shelf
        // showing whichever books happened to sit at those positions.
        applyFilter();
        pageStarts.clear();
        selectedIndex = 0;
        topIndex = 0;
        requestUpdate(true);
      });
}

// The strip's tab order, which is also the cycle order.
constexpr library::SortOrder kSortTabs[] = {library::SortOrder::DateDesc, library::SortOrder::TitleAsc,
                                            library::SortOrder::TitleDesc, library::SortOrder::AuthorAsc};
constexpr int kSortTabCount = static_cast<int>(sizeof(kSortTabs) / sizeof(kSortTabs[0]));

int sortTabIndex(const library::SortOrder order) {
  for (int i = 0; i < kSortTabCount; i++) {
    if (kSortTabs[i] == order) return i;
  }
  return 0;
}

// The strip holds the four sort modes plus Search, which is not one. Moving onto
// a sort mode applies it at once; Search waits for Confirm, since opening a
// keyboard is not something a sideways press should do by itself.
constexpr int kSearchTab = kSortTabCount;

void LibraryListActivity::cycleSortOrder(const bool forward) {
  const int slots = kSortTabCount + 1;
  tabCursor = (tabCursor + (forward ? 1 : slots - 1)) % slots;
  if (tabCursor != kSearchTab) {
    sortOrder = kSortTabs[tabCursor];
    applyFilter();
    selectedIndex = 0;
    topIndex = 0;
  }
  requestUpdate();
}

// The strip needs the mode alone. The header strings carry a "Library ·" prefix
// that reads as four copies of the word once they sit side by side.
const char* sortLabelFor(const library::SortOrder order) {
  switch (order) {
    case library::SortOrder::DateDesc:
      return tr(STR_LIBRARY_TAB_RECENT);
    case library::SortOrder::TitleAsc:
      return tr(STR_LIBRARY_TAB_TITLE_AZ);
    case library::SortOrder::TitleDesc:
      return tr(STR_LIBRARY_TAB_TITLE_ZA);
    case library::SortOrder::AuthorAsc:
      return tr(STR_LIBRARY_TAB_AUTHOR);
  }
  return "";
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
  // The sort strip sits between the header and the list, and takes its height
  // from the list rather than overlaying it.
  tabsTop = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput);
  listTop = tabsTop + LIBRARY_TABS_HEIGHT + metrics.verticalSpacing;
  listHeight = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - listTop;
}

int LibraryListActivity::rowCount() const {
  return query.empty() ? static_cast<int>(index.bookCount()) : static_cast<int>(filtered.size());
}

// Entry position on screen to row position in the sort order. Identity while
// unfiltered, so the shelf costs nothing when nothing is typed.
int LibraryListActivity::rowFor(const int entry) const {
  if (query.empty()) return entry;
  if (entry < 0 || entry >= static_cast<int>(filtered.size())) return 0;
  return filtered[entry];
}

// One pass over the sort order, keeping what matches. No index, no cache: at the
// 512-book cap this is 512 comparisons of at most 96 bytes, which is far below
// the cost of the panel repaint that will follow it anyway.
void LibraryListActivity::applyFilter() {
  filtered.clear();
  // Cleared even on the empty-query path: dropping a filter changes the list just
  // as much as applying one.
  pageStarts.clear();
  if (query.empty()) return;

  // Folded the same way the stored folds were, articles removed included —
  // otherwise "the hobbit" searches for a word no record contains.
  const std::string needle = library::fold(query, /*stripArticle=*/true);
  const int total = static_cast<int>(index.bookCount());
  filtered.reserve(static_cast<size_t>(total));
  for (int row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    if (library::matchesQuery(std::string_view(record.fold, record.foldLen), needle)) {
      filtered.push_back(static_cast<uint16_t>(row));
      continue;
    }
    // The stored fold covers the title only, so the author has to be read and
    // folded here. That is the search most worth having: the reader who knows the
    // author usually also knows where the book is, while "emily" finding Emily
    // Bronte is the case the shelf exists to answer.
    std::string author;
    if (index.readAuthor(record, author) && library::matchesQuery(library::fold(author), needle)) {
      filtered.push_back(static_cast<uint16_t>(row));
    }
  }
  selectedIndex = 0;
  topIndex = 0;
}

// 26 letters over 5 columns. A grid rather than a strip because reaching a letter
// costs presses, and each press is a full ~185 ms panel repaint on this panel:
// linear travel averages 13 presses, two dimensions average about 4.5.
constexpr int kLetterCols = 5;
constexpr int kLetterCount = 26;

// Which letter a book files under, matching the column the reader is looking at:
// the title's when sorted by title, the author's when sorted by author. Using the
// title fold in author order — which the first cut did — sent "Emily Bronte" to
// wherever her book's title happened to fall.
char LibraryListActivity::letterOf(const library::ClixRecord& record) {
  // Must be the key the rows are ORDERED by, not the text they display. The jump
  // scans for the first row at or past the chosen letter, which is only valid
  // while the letters ascend — and the displayed name does not always ascend with
  // the sort. "Sand George" is filed under G, because authorKey sorts a name's
  // words so that "George Sand" and "Sand George" group as one person; reading
  // the display letter there gives S, the scan meets it early, and every letter
  // between G and S stops on that one row.
  if (sortOrder == library::SortOrder::AuthorAsc) {
    // The shelf is ordered by surname now, so the jump reads the same key. It is
    // derived from the displayed name, which after harmonisation is one string
    // per author — so the letters ascend down the list, which is what makes the
    // scan valid.
    std::string author;
    if (!index.readAuthor(record, author)) return '\0';
    if (jumpByGivenName) {
      const std::string folded = library::fold(author);
      return folded.empty() ? '\0' : folded[0];
    }
    const std::string key = library::surnameKey(author);
    return key.empty() ? '\0' : key[0];
  }
  return record.foldLen == 0 ? '\0' : record.fold[0];
}

void LibraryListActivity::computeLettersPresent() {
  lettersPresent = 0;
  const int total = rowCount();
  for (int entry = 0; entry < total; entry++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    const char c = letterOf(record);
    if (c >= 'a' && c <= 'z') lettersPresent |= 1u << (c - 'a');
  }
}

// The fold has already dropped accents and leading articles, so "L'Odyssee"
// lands under O and "Éluard" under E — which is what a reader looking under a
// letter expects, and what the raw title would get wrong.
void LibraryListActivity::jumpToLetter(const char letter) {
  const bool descending = sortOrder == library::SortOrder::TitleDesc;
  const int total = rowCount();
  for (int entry = 0; entry < total; entry++) {
    const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    // Ordered by surname, the letters ascend, so "at or past" lands correctly
    // even on a letter no book has. Jumping by GIVEN name they do not ascend at
    // all — the As are scattered down the whole shelf — so that mode must match
    // exactly, and lands on the first such book in shelf order.
    const char c = letterOf(record);
    // The scan has to follow the direction the shelf runs in. Title Z-A descends,
    // so "at or past" stopped on the very first row every time — its letter is
    // always at or past anything asked for. Given-name order does not run
    // alphabetically at all, so that one matches exactly.
    const bool hit = jumpByGivenName ? c == letter : descending ? c <= letter : c >= letter;
    if (hit) {
      selectedIndex = entry;
      topIndex = entry;
      pageStarts.clear();
      return;
    }
  }
}

void LibraryListActivity::drawLetterGrid() {
  const int width = renderer.getScreenWidth();
  const int cell = (width - 2 * LIBRARY_SIDE_PADDING) / kLetterCols;
  const int rows = (kLetterCount + kLetterCols - 1) / kLetterCols;
  const int cellH = listHeight / (rows + 1);
  const int top = listTop + cellH / 2;
  // Both modes shown, not just the active one. Printing only the current choice
  // hides the fact that there IS a choice — the same reason the sort strip lists
  // every mode. On a panel that refreshes whole, the second label is free.
  // First name before last, because the words themselves carry that order and
  // reading them the other way round jars. "Last name" rather than "surname":
  // parallel vocabulary, and the plainer of the two for anyone reading English as
  // a second language.
  const char* labels[2] = {tr(STR_LIBRARY_JUMP_GIVEN), tr(STR_LIBRARY_JUMP_SURNAME)};
  const int active = jumpByGivenName ? 0 : 1;
  const int gap = 20;
  int labelW[2];
  for (int i = 0; i < 2; i++) labelW[i] = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
  const int modeH = renderer.getLineHeight(SMALL_FONT_ID);
  int mx = (width - (labelW[0] + labelW[1] + gap)) / 2;
  const int modeY = listTop + 2;

  for (int i = 0; i < 2; i++) {
    const bool on = i == active;
    // Focused, the active choice inverts — the strongest signal this panel has
    // that Left/Right are about to change it. Unfocused it keeps an underline, so
    // the line stays quiet while the grid holds attention.
    if (on && letterCursor < 0) {
      renderer.fillRoundedRect(mx - 5, modeY - 2, labelW[i] + 10, modeH + 4, 4, Color::Black);
    }
    renderer.drawText(SMALL_FONT_ID, mx, modeY, labels[i], !(on && letterCursor < 0));
    if (on && letterCursor >= 0) renderer.fillRect(mx, modeY + modeH + 1, labelW[i], 1, true);
    mx += labelW[i] + gap;
  }

  // Centre the block itself. Laying it out from the left margin left the last
  // column hanging off the right edge, since 26 letters do not fill 5 columns
  // evenly and the remainder all landed on one side.
  const int originX = (width - kLetterCols * cell) / 2;

  for (int i = 0; i < kLetterCount; i++) {
    const int cx = originX + (i % kLetterCols) * cell;
    const int cy = top + (i / kLetterCols) * cellH;
    const bool present = (lettersPresent & (1u << i)) != 0;

    // The pill and the glyph share one centre, so the letter sits in the middle
    // of its square rather than in a corner of it.
    const int pillW = cell - 6;
    const int pillH = cellH - 6;
    const int pillX = cx + (cell - pillW) / 2;
    // A letter no book starts with is simply not drawn. Its slot stays empty and
    // nothing moves, because the grid's positions come from the alphabet's index
    // and not from what happens to be painted — the earlier worry about losing
    // one's place was unfounded.
    if (!present) continue;
    if (i == letterCursor) {
      renderer.fillRoundedRect(pillX, cy, pillW, pillH, 4, Color::Black);
    }
    // Three attempts at showing an unavailable letter failed on a 1-bit panel: a
    // smaller font read as inconsistent typography, dithering the glyph erased it,
    // and an outlined cursor said nothing legible. Not drawing it says it plainly.
    char label[2] = {static_cast<char>('A' + i), 0};
    const int tw = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int th = renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, pillX + (pillW - tw) / 2, cy + (pillH - th) / 2, label,
                      !(present && i == letterCursor));
  }
}

void LibraryListActivity::openSearch() {
  // No key filtering here on purpose. Greying out the letters that lead nowhere
  // was built, tested on device and removed: a letter you can see but cannot reach
  // reads as a broken keyboard, and the eye keeps returning to it. The one press
  // it saves is not worth that. Git holds the implementation if it is revisited.
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query,
                                                                 48, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           query = std::get<KeyboardResult>(result.data).text;
                           applyFilter();
                           tabsFocused = false;
                           requestUpdate();
                         });
}

// Title and author for one entry, read straight from the index. Only ever called
// for rows about to be drawn, so at most a screenful of strings exists at once.
bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author) {
  title.clear();
  author.clear();
  const uint16_t ordinal = index.ordinalForRow(sortOrder, static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  std::string name;
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record) && index.readName(record, name)) {
    // The build already decided both fields — from the book's own metadata when
    // it has any, and with one spelling chosen per author across the library.
    // Re-parsing the name here would throw that away, and only works while the
    // name still looks like "Title - Author".
    if (!index.readAuthor(record, author)) author.clear();
    // The stored title when the book gave one, the filename otherwise. `name` is
    // the filename now and must stay so: openSelectedBook rebuilds the path from
    // it.
    if (!index.readTitle(record, title) || title.empty()) title = name;
  }
  if (title.empty()) title = tr(STR_LIBRARY_UNKNOWN_TITLE);
  return true;
}

void LibraryListActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finishAfterBackPress();
    return;
  }
  const int count = rowCount();

  // The grid owns every button while it is open, so its block runs FIRST. Sitting
  // below the Back handlers, its own Back was unreachable: Back left the activity
  // with the grid still on screen.
  if (letterGrid) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      letterGrid = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Refused on a letter no book has. Jumping to where it WOULD fall is a
      // correct answer to a question the reader did not ask, and the outlined
      // cursor has already said the key is inert.
      if (letterCursor >= 0 && (lettersPresent & (1u << letterCursor))) {
        jumpToLetter(static_cast<char>('a' + letterCursor));
        letterGrid = false;
        // Hand focus back to the list, or the next Confirm reopens the grid
        // instead of opening the book just jumped to.
        tabsFocused = false;
        requestUpdate();
      }
      return;
    }
    // letterCursor == -1 is the mode line above the grid, reached by pressing Up
    // from the top row — the same idiom the sort strip uses, so there is one rule
    // to learn rather than two.
    if (letterCursor < 0) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
          mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        jumpByGivenName = !jumpByGivenName;
        // The letters present as first names are not those present as surnames.
        // The cursor is on the mode line, not on a letter, so nothing needs
        // re-seating here — Down does that when it enters the grid.
        computeLettersPresent();
        requestUpdate();
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
        // Land on a letter that exists. Dropping onto "a" when no book starts
        // with one puts the cursor on a blank cell, which is the state the grid
        // is built to never show.
        letterCursor = 0;
        for (int i = 0; i < kLetterCount; i++) {
          if (lettersPresent & (1u << i)) {
            letterCursor = i;
            break;
          }
        }
        requestUpdate();
      }
      return;
    }

    int delta = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) delta = 1;
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) delta = -1;
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) delta = kLetterCols;
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (letterCursor < kLetterCols) {
        letterCursor = -1;
        requestUpdate();
        return;
      }
      delta = -kLetterCols;
    }
    if (delta != 0) {
      // Skip cells with nothing drawn in them — the cursor must never sit on a
      // blank. Keeping the SAME delta is what makes this safe: an earlier attempt
      // stepped by one regardless of direction, so Down walked sideways and the
      // grid stopped being two-dimensional. Down still travels a whole row, it
      // just keeps travelling until it finds a letter.
      int next = letterCursor;
      for (int guard = 0; guard < kLetterCount; guard++) {
        next = (next + delta + kLetterCount) % kLetterCount;
        if (lettersPresent & (1u << next)) {
          letterCursor = next;
          break;
        }
      }
      requestUpdate();
    }
    return;
  }

  // Back clears the filter before it leaves. A shelf showing 7 of 60 books is a
  // state the reader must be able to undo, and giving it the press they would
  // reach for anyway costs no screen space and needs no explaining.
  if (!query.empty() && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    query.clear();
    filtered.clear();
    selectedIndex = 0;
    topIndex = 0;
    // Boundaries measured against the filtered list mean nothing once the whole
    // shelf is back.
    pageStarts.clear();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }
  if (tabsFocused && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (tabCursor == kSearchTab) {
      openSearch();
    } else if (sortOrder != library::SortOrder::DateDesc) {
      // Only where an alphabet exists to jump through. Sorted by date there is no
      // letter order to walk, so the press stays inert rather than opening a grid
      // whose every choice would land somewhere arbitrary.
      jumpByGivenName = false;
      computeLettersPresent();
      letterCursor = 0;
      for (int i = 0; i < kLetterCount; i++) {
        if (lettersPresent & (1u << i)) {
          letterCursor = i;
          break;
        }
      }
      letterGrid = true;
      requestUpdate();
    }
    return;
  }
  // The hold threshold below also gates this release: when the sort menu
  // declines to show (degraded), the long hold falls through to here, and the
  // release of a menu-length hold must not open a book instead.
  if (count > 0 && mappedInput.wasReleased(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() < 800) {
    openSelectedBook();
    return;
  }
  // A Confirm hold opens the sort menu. It is a small selection screen rather
  // than a value cycled in place, because that is how this codebase changes an
  // enum everywhere else (OptionSelectionActivity, used by Settings) — and
  // because Left and Right are already spent on paging.
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= 800) {
    openSortMenu();
    return;
  }

  // Left and Right page. On this hardware ButtonNavigator maps them as aliases of
  // Up and Down (util/ButtonNavigator.h:47-53), so the second axis is unused and
  // paging is free — which matters at 69 books, where scrolling one row at a time
  // is 34 presses to the middle and paging is 5.
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) && count > 0) {
    if (tabsFocused) {
      cycleSortOrder(/*forward=*/true);
    } else {
      nextPage();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && count > 0) {
    if (tabsFocused) {
      cycleSortOrder(/*forward=*/false);
    } else {
      previousPage();
    }
    return;
  }

  // Scrolling keeps the selection inside the window, using the row count the last
  // frame actually held. One frame stale and self-correcting, which is the only
  // honest option when row heights depend on the titles being shown.
  // The list PAGES, it does not scroll. On e-ink moving one row costs the same
  // full-panel refresh as turning a whole page, so scrolling spends the panel's
  // most expensive operation on its smallest possible result. Up and Down move
  // within the page; at an edge they turn it and land on the far row, so the
  // reader never loses the sense of a fixed frame.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (tabsFocused) {
      // already at the top
    } else if (selectedIndex == 0) {
      tabsFocused = true;
      tabCursor = sortTabIndex(sortOrder);
      requestUpdate();
    } else if (selectedIndex > topIndex) {
      selectedIndex--;
      requestUpdate();
    } else if (topIndex > 0) {
      previousPage(/*selectLast=*/true);
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down) && count > 0) {
    if (tabsFocused) {
      tabsFocused = false;
      requestUpdate();
    } else if (selectedIndex < topIndex + visibleRows - 1 && selectedIndex < count - 1) {
      selectedIndex++;
      requestUpdate();
    } else if (topIndex + visibleRows < count) {
      nextPage();
    }
  }
}

// The sort strip: every mode visible at once, the active one underlined. On a
// panel that refreshes whole, showing the alternatives costs nothing per frame
// and saves a menu round-trip to discover them.
void LibraryListActivity::drawSortTabs(const int top) {
  const int width = renderer.getScreenWidth();
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int height = lineH + 8;

  // No band fill. On a 1-bit panel the dithered grey is a literal checkerboard,
  // and it is the same pattern the selected row uses — two things competing for
  // the eye with one texture. The pill alone carries the state.

  // Equal-width slots, as in Settings, so the tabs do not shift as labels change.
  const int slots = kSortTabCount + 1;
  const int slot = width / slots;
  for (int i = 0; i < slots; i++) {
    const char* label = i == kSearchTab ? tr(STR_LIBRARY_SEARCH) : sortLabelFor(kSortTabs[i]);
    const int w = renderer.getTextWidth(SMALL_FONT_ID, label);
    const int x = i * slot + (slot - w) / 2;
    // Focused, the cursor marks the pill; unfocused, the active sort does.
    const bool selected = tabsFocused ? i == tabCursor : (i != kSearchTab && kSortTabs[i] == sortOrder);

    // Focused, the pill inverts, which is the strongest signal this panel has
    // that Left/Right now belong to the strip. Unfocused it stays a plain
    // underline, so the list keeps the reader's attention.
    if (selected && tabsFocused) {
      renderer.fillRoundedRect(x - 6, top + 2, w + 12, height - 4, 4, Color::Black);
    }
    renderer.drawText(SMALL_FONT_ID, x, top + 4, label, !(selected && tabsFocused));
    if (selected && !tabsFocused) renderer.fillRect(x, top + 4 + lineH + 1, w, 1, true);
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

  if (letterGrid) {
    drawLetterGrid();
  } else if (rowCount() == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_LIBRARY_EMPTY));
  } else {
    measureRows();
    if (!degraded) drawSortTabs(tabsTop);
    drawRows();
  }

  drawPositionReadout();
  // The bottom pair delivers Left/Right on this hardware, so labelling it
  // "Up/Down" describes the wrong axis — it pages the list, switches tabs and
  // steps letters, none of which is vertical. mapLabels takes previous/next
  // precisely so the caller can say what they do here.
  const char* prevLabel = letterGrid || tabsFocused ? tr(STR_DIR_LEFT) : tr(STR_LIBRARY_PAGE_PREV);
  const char* nextLabel = letterGrid || tabsFocused ? tr(STR_DIR_RIGHT) : tr(STR_LIBRARY_PAGE_NEXT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), prevLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void LibraryListActivity::drawRows() {
  measureRows();
  const int count = rowCount();
  if (topIndex > selectedIndex) topIndex = selectedIndex;
  const int width = renderer.getScreenWidth();
  const int textX = LIBRARY_SIDE_PADDING + LIBRARY_ICON_SIZE + LIBRARY_ICON_GAP;
  const int textW = width - textX - LIBRARY_SIDE_PADDING;

  // Rows have content-dependent heights, so the page is filled by accumulating
  // them rather than by dividing the band. A row is only drawn if it fits whole:
  // a half-drawn row at the bottom edge would look like a rendering fault.
  std::string title;
  std::string author;
  std::string previousAuthor;
  // Sorted by author, the permutation already places one author's books
  // consecutively, so grouping costs one comparison per row and no extra pass.
  // The author then appears once above the run instead of under every title,
  // which is what makes the shelf answer "what else has this person written".
  const bool grouped = sortOrder == library::SortOrder::AuthorAsc;
  // Proximity does the grouping. The heading sits close to the books it names and
  // far from the run above, so it reads as belonging downward; equal gaps on both
  // sides — which is what the first cut had — leave it attached to nothing.
  const int groupGapAbove = LIBRARY_ROW_PADDING + 6;
  const int groupGapBelow = 3;
  const int groupH = grouped ? authorLineH + groupGapAbove + groupGapBelow : 0;
  // Books indent under their heading, so the left edge itself says which rows
  // belong to whom, without a box or a rule doing the work.
  const int groupIndent = grouped ? LIBRARY_ICON_GAP : 0;

  int y = listTop;
  int drawn = 0;
  for (int entry = topIndex; entry < count; entry++) {
    rowTextFor(entry, title, author);
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, title.c_str(), textW, LIBRARY_TITLE_LINES);
    const int height = rowHeightFor(static_cast<int>(lines.size()), !grouped && !author.empty());
    // The first row of a page always carries its heading: without it a page can
    // open on books whose author was named on the page before.
    const bool startsGroup = grouped && !author.empty() && (drawn == 0 || author != previousAuthor);
    previousAuthor = author;
    if (drawn > 0 && y + height + (startsGroup ? groupH : 0) > listTop + listHeight) break;

    if (startsGroup) {
      // Written surname-first, as a catalogue does. The shelf is ORDERED by
      // surname, and printing "Anton Chekhov" above a run that sits between
      // Chateaubriand and Crane makes the order look arbitrary — the eye reads
      // F, A, S while the sort follows Ch, Ch, Cr. Inverting it here makes the ordering
      // visible in the column that carries it. Only in author order: elsewhere
      // the natural spelling reads better.
      // Into a local, NOT back into `author`. Overwriting it cost the first row of
      // every group its separator: the comparison below reads the next row's
      // author straight from the index, so "Xun, Lu" never matched
      // "Lu Xun" and the rows read as separate groups.
      std::string inverted = author;
      const size_t lastSpace = inverted.find_last_of(' ');
      if (lastSpace != std::string::npos && lastSpace + 1 < inverted.size()) {
        inverted = inverted.substr(lastSpace + 1) + ", " + inverted.substr(0, lastSpace);
      }
      // The first heading on a page needs no gap above it: the strip already
      // bounds the list there.
      const int gap = drawn == 0 ? 2 : groupGapAbove;
      const std::string heading = renderer.truncatedText(SMALL_FONT_ID, inverted.c_str(), textW);
      renderer.drawText(SMALL_FONT_ID, LIBRARY_SIDE_PADDING, y + gap, heading.c_str(), true);
      y += authorLineH + gap + groupGapBelow;
    }

    if (entry == selectedIndex) {
      renderer.fillRoundedRect(LIBRARY_SIDE_PADDING / 2 + groupIndent, y, width - LIBRARY_SIDE_PADDING - groupIndent,
                               height - 2, 6, Color::LightGray);
    }
    renderer.drawIcon(icon_book_24_bits, LIBRARY_SIDE_PADDING + groupIndent, y + (height - LIBRARY_ICON_SIZE) / 2,
                      LIBRARY_ICON_SIZE);

    int textY = y + LIBRARY_ROW_PADDING / 2;
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, textX + groupIndent, textY, line.c_str(), true);
      textY += titleLineH;
    }
    if (!grouped && !author.empty()) {
      const std::string fitted = renderer.truncatedText(SMALL_FONT_ID, author.c_str(), textW);
      renderer.drawText(SMALL_FONT_ID, textX, textY, fitted.c_str(), true);
    }

    y += height;
    drawn++;

    // Dotted, not solid: on a 1-bit panel every-other-pixel is how a rule reads
    // grey. A solid line would outweigh the text it separates.
    // Within a group only. Across a boundary the whitespace and the next heading
    // already separate, and a rule there would compete with them.
    std::string nextAuthor;
    bool sameGroup = true;
    if (grouped && entry + 1 < count) {
      std::string nextTitle;
      rowTextFor(entry + 1, nextTitle, nextAuthor);
      sameGroup = nextAuthor == author;
    }
    if (entry + 1 < count && sameGroup && y + LIBRARY_ROW_PADDING < listTop + listHeight) {
      for (int x = LIBRARY_SIDE_PADDING + groupIndent; x < width - LIBRARY_SIDE_PADDING; x += 2) {
        renderer.drawPixel(x, y - 1, true);
      }
    }
  }

  // Report how much this screen held, for the next input pass to page by. Do NOT
  // adjust topIndex here: loop() already scrolled it to contain the selection
  // before asking for this frame, and correcting it afterwards meant the frame
  // just drawn could omit the selected row — which is what made Up/Down followed
  // by Left/Right jump somewhere unrelated.
  visibleRows = drawn > 0 ? drawn : 1;
  // previousPage() aims past the end because a page's size is only known once it
  // has been measured; clamp now that it has been.
  if (selectedIndex >= topIndex + visibleRows) selectedIndex = topIndex + visibleRows - 1;
  if (selectedIndex >= count) selectedIndex = count - 1;
}

// "12/69" at the bottom right: which book is selected, out of how many.
//
// NOT a page count. Rows are variable height, so how many fit depends on the
// titles currently on screen — a page total derived from that grows and shrinks
// as you scroll, which is exactly the "6/6 then 8/8" the first version produced.
// The book position is stable by construction, and it answers the question the
// reader actually has: how far in am I, and how much is left.
void LibraryListActivity::drawPositionReadout() {
  const int count = rowCount();
  if (count <= 0) return;

  // "12/60 books", not "2/6". Rows are variable height, so how many fit depends
  // on the titles currently on screen; a page total derived from that grows and
  // shrinks as you scroll. Real pages would mean wrapping every title up front,
  // once per sort order — the per-render cost this screen is built to avoid, for
  // a number that answers a smaller question than "how far in am I".
  char buf[32];
  snprintf(buf, sizeof(buf), tr(STR_LIBRARY_POSITION), selectedIndex + 1, count);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
  const int x = renderer.getScreenWidth() - width - LIBRARY_SIDE_PADDING;
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, x, y, buf, true);
}

// Page boundaries are content-dependent, so they cannot be computed from an
// index — a page holds as many rows as its own titles allow. They are therefore
// remembered as the reader moves forward, which makes going back exact rather
// than an estimate that would drift on every turn.
void LibraryListActivity::nextPage() {
  const int count = rowCount();
  const int next = topIndex + visibleRows;
  if (next >= count) return;
  if (pageStarts.empty()) pageStarts.push_back(0);
  pageStarts.push_back(next);
  topIndex = next;
  selectedIndex = topIndex;
  requestUpdate();
}

void LibraryListActivity::previousPage(const bool selectLast) {
  if (topIndex <= 0) return;
  if (pageStarts.size() > 1) {
    pageStarts.pop_back();
    topIndex = pageStarts.back();
  } else {
    // No recorded history — the reader jumped here by some other route. Fall back
    // to a screenful back; it may not land on a boundary this pass, but the next
    // render re-measures and nothing is lost.
    topIndex = std::max(0, topIndex - visibleRows);
    pageStarts.assign(1, topIndex);
  }
  // selectLast is only known to be right after the render that measures this
  // page, so aim past the end and let drawRows clamp it.
  selectedIndex = selectLast ? topIndex + visibleRows - 1 : topIndex;
  requestUpdate();
}
