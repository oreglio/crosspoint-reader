#include "LibraryListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryState.h>
#include <LibraryText.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "activities/home/BookActions.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/libraryIcons.h"

namespace fui = freeink::ui;

namespace {
// Which way the Titles tab runs. One direction for the whole power-on session:
// it survives this activity's delete-on-exit on purpose, and it is deliberately
// not a setting — a persisted field would be one more thing to migrate for a
// preference the next boot can simply re-express in one hold.
bool sTitleDescending = false;
// The ★ view survives the shelf the same way: with the star leading the strip,
// leaving in the favorites view and returning to Recent read as a bug on the
// device. A boot still starts on Recent — the full shelf is the honest default.
bool sFavoritesView = false;
// And the sort itself, for the same reason: choosing A-Z, leaving and coming
// back to Recent read as "the filters do not save". One session-long shelf
// state, statics only, zero settings.
library::SortOrder sSortOrder = library::SortOrder::DateDesc;
// The ★ view carries its OWN remembered order: sorting favorites by author and
// then browsing Recent must not cost the favorites their order — which it did,
// on the device, while they shared one variable. The title direction stays
// shared: one triangle, one truth.
library::SortOrder sFavSortOrder = library::SortOrder::DateDesc;
// Every row lookup goes through the order of the ACTIVE view.
library::SortOrder currentOrder() { return sFavoritesView ? sFavSortOrder : sSortOrder; }

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
  // Books arrived (or left) since the last build: every ingestion path marks
  // one flag, consumed here. The rebuild reconciles, so the newcomer tops
  // Recently added and every other book keeps its place.
  if (library::takeShelfStale() && indexReady) {
    index.close();
    indexReady = false;
  }
  if (!indexReady) {
    // Held across the popup and the build, for the same reason the Settings
    // rebuild holds it: the render task's SD-loaded fonts read glyph data at
    // draw time, and the walk needs the card to itself.
    RenderLock lock(*this);
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    indexReady = rebuildIndex() && openIndex();
  }
  degraded = indexReady && index.ranksDegraded();
  // Corrupt or unreadable favorites degrade to an empty set, logged inside;
  // the shelf itself must never be held up by its smallest file.
  favorites.load();
  // The shelf's posture comes back from disk, not from the statics alone:
  // this reader deep-sleeps between sessions and wakes through a full boot,
  // so RAM state forgets the shelf several times a day — which read as "the
  // filters do not save" on the device.
  library::LibraryShelfState state;
  library::loadLibraryState(state);
  sFavoritesView = state.favoritesView;
  sTitleDescending = state.titleDescending;
  sSortOrder = state.shelfSort;
  sFavSortOrder = state.favSort;
  // `filtered` belongs to THIS instance: without a rebuild here, a restored ★
  // view opened on an empty list until the reader wiggled the tabs.
  applyFilter();
  restoreSelection(state.selected);
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  // One write per leave — cursor moves never touch the card. Captured before
  // the index closes, because the selection anchor needs it; when the index is
  // already gone (a book was just opened), the anchor openSelectedBook staged
  // is the right answer anyway: the shelf should reopen on that book.
  library::LibraryShelfState state;
  state.favoritesView = sFavoritesView;
  state.titleDescending = sTitleDescending;
  state.shelfSort = sSortOrder;
  state.favSort = sFavSortOrder;
  if (indexReady) {
    rowKeyFor(selectedIndex, state.selected);
  } else {
    state.selected = exitSelection;
  }
  library::saveLibraryState(state);
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
  const uint16_t ordinal = index.ordinalForRow(currentOrder(), static_cast<uint16_t>(rowFor(selectedIndex)));
  if (ordinal == 0xFFFF) return;

  library::ClixRecord record{};
  std::string path;
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", selectedIndex);
    return;
  }
  // Staged for onExit: once the index closes, the selection can no longer be
  // resolved, and the book being opened is exactly the one to come back to.
  rowKeyFor(selectedIndex, exitSelection);
  // Release the index handle first: on hardware SdFat allows one open reader per
  // path at a time, and the reader is about to open files of its own.
  index.close();
  indexReady = false;
  activityManager.goToReader(std::move(path));
}

void LibraryListActivity::restoreSelection(const library::FavoriteKey& sel) {
  if (sel.nameHash == 0 && sel.fileSize == 0) return;
  const int count = rowCount();
  for (int entry = 0; entry < count; entry++) {
    library::FavoriteKey key;
    if (rowKeyFor(entry, key) && key == sel) {
      selectedIndex = entry;
      topIndex = entry;
      return;
    }
  }
  // Gone, filtered out, or renamed: the cursor stays at the top rather than
  // landing somewhere that merely shares a row number with the past.
}

bool LibraryListActivity::rowKeyFor(const int entry, library::FavoriteKey& key) {
  const uint16_t ordinal = index.ordinalForRow(currentOrder(), static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  std::string name;
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readName(record, name)) return false;
  key.nameHash = library::favoriteNameHash(name.data(), name.size());
  key.fileSize = record.fileSize;
  return true;
}

void LibraryListActivity::toggleFavoriteAt(const int entry) {
  library::FavoriteKey key;
  if (!rowKeyFor(entry, key)) return;
  const bool nowFavorite = favorites.toggle(key);
  BookActions::drawToast(renderer, nowFavorite ? tr(STR_LIBRARY_FAV_ADDED) : tr(STR_LIBRARY_FAV_REMOVED));
  delay(1200);
  // Removing while looking AT the ★ view must take the row out of it.
  if (sFavoritesView) applyFilter();
  requestUpdate(true);
}

void LibraryListActivity::openBookMenu() {
  if (!indexReady || rowCount() == 0) return;
  std::string title;
  std::string author;
  bool isFavorite = false;
  rowTextFor(selectedIndex, title, author, &isFavorite);
  const std::vector<std::string> options{tr(STR_LIBRARY_MENU_OPEN),
                                         isFavorite ? tr(STR_LIBRARY_MENU_FAV_REMOVE) : tr(STR_LIBRARY_MENU_FAV_ADD),
                                         tr(STR_LIBRARY_MENU_DETAILS), tr(STR_DELETE)};
  popup.show(title.c_str(), options, 0, [this](const int choice) {
    if (choice == 0) openSelectedBook();
    if (choice == 1) toggleFavoriteAt(selectedIndex);
    if (choice == 2) {
      detailsView = true;
      requestUpdate(true);
    }
    if (choice == 3) promptDeleteSelectedBook();
  });
  requestUpdate();
}

void LibraryListActivity::promptDeleteSelectedBook() {
  const uint16_t ordinal = index.ordinalForRow(currentOrder(), static_cast<uint16_t>(rowFor(selectedIndex)));
  library::ClixRecord record{};
  std::string path;
  std::string title;
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path)) return;
  if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
  library::FavoriteKey key;
  const bool hasKey = rowKeyFor(selectedIndex, key);

  const std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, title),
                         [this, path, key, hasKey](const ActivityResult& res) {
                           if (res.isCancelled) {
                             requestUpdate(true);
                             return;
                           }
                           // Closed before the delete and rebuild: the rebuild renames a fresh
                           // index over this one, which must not happen under an open reader.
                           index.close();
                           indexReady = false;
                           // The FILE goes first — the reverse of the Recent Books order, on
                           // purpose: if the remove fails, a book that still exists must not
                           // have lost its bookmarks and cache to a cleanup that ran ahead.
                           if (!Storage.remove(path.c_str())) {
                             LOG_ERR("LIB", "failed to delete %s", path.c_str());
                             indexReady = openIndex();
                             requestUpdate(true);
                             return;
                           }
                           // Reader cache, bookmarks and clippings — the same helper Recent
                           // Books uses, per file type. Reading stats are deliberately KEPT,
                           // as everywhere else: deleting a book does not rewrite history.
                           BookActions::clearFileMetadata(path);
                           RECENT_BOOKS.removeByPath(path);
                           // The two cleanups only this screen knows about. toggle() removes and
                           // writes through; the contains() guard keeps it from re-adding.
                           if (hasKey && favorites.contains(key)) favorites.toggle(key);
                           // Reconciled on the spot, so the shelf never lists a ghost row. Same
                           // lock-and-popup as the entry rebuild: the walk needs the card.
                           {
                             RenderLock lock(*this);
                             GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
                             indexReady = rebuildIndex() && openIndex();
                           }
                           degraded = indexReady && index.ranksDegraded();
                           applyFilter();
                           selectedIndex = 0;
                           topIndex = 0;
                           requestUpdate(true);
                         });
}

// The sort menu the strip cannot offer here: its sort tabs would leave the ★
// view. This is the view's own secondary action — hold on the focused star —
// and the one place the old standalone sort menu earned its way back.
void LibraryListActivity::openFavoritesSortMenu() {
  const std::string titles = tr(STR_LIBRARY_TAB_TITLES);
  const std::vector<std::string> options{tr(STR_LIBRARY_TAB_RECENT), titles + " A-Z", titles + " Z-A",
                                         tr(STR_LIBRARY_TAB_AUTHOR)};
  const int current = sFavSortOrder == library::SortOrder::DateDesc    ? 0
                      : sFavSortOrder == library::SortOrder::TitleAsc  ? 1
                      : sFavSortOrder == library::SortOrder::TitleDesc ? 2
                                                                       : 3;
  popup.show(tr(STR_LIBRARY_FAV_SORT_TITLE), options, current, [this](const int choice) {
    switch (choice) {
      case 0:
        sFavSortOrder = library::SortOrder::DateDesc;
        break;
      // The Titles tab's triangle keeps telling the truth if the reader
      // later leaves ★: direction state follows the choice made here.
      case 1:
        sFavSortOrder = library::SortOrder::TitleAsc;
        sTitleDescending = false;
        break;
      case 2:
        sFavSortOrder = library::SortOrder::TitleDesc;
        sTitleDescending = true;
        break;
      case 3:
        sFavSortOrder = library::SortOrder::AuthorAsc;
        break;
      default:
        return;
    }
    applyFilter();
    selectedIndex = 0;
    topIndex = 0;
    requestUpdate(true);
  });
  requestUpdate();
}

// The strip's tab order, which is also the cycle order: the ★ first, then
// Recent, Titles, Author, Search. The star sits at the edge because a glyph
// wedged between two words breaks the row's reading — first device feedback —
// and an edge is where the eye expects the special place anyway. Moving onto
// ★ or a sort tab applies it at once; Search waits for Confirm, since opening
// a keyboard is not something a sideways press should do by itself.
//
// Both title directions live on ONE tab. Z-A paid a full strip slot for a rare
// use, and French labels were already overflowing the right edge; direction is
// state on the tab (the drawn triangle), not a place in the row.
constexpr int kFavTab = 0;
constexpr int kRecentTab = 1;
constexpr int kTitlesTab = 2;
constexpr int kAuthorTab = 3;
constexpr int kSearchTab = 4;
constexpr int kTabSlots = kSearchTab + 1;

// No longer one-to-one: both title orders map to the Titles tab.
int sortTabIndex(const library::SortOrder order) {
  switch (order) {
    case library::SortOrder::TitleAsc:
    case library::SortOrder::TitleDesc:
      return kTitlesTab;
    case library::SortOrder::AuthorAsc:
      return kAuthorTab;
    case library::SortOrder::DateDesc:
      return kRecentTab;
  }
  return kRecentTab;
}

library::SortOrder orderForTab(const int tab) {
  if (tab == kTitlesTab) return sTitleDescending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  if (tab == kAuthorTab) return library::SortOrder::AuthorAsc;
  return library::SortOrder::DateDesc;
}

// Direction is a flip of existing state, not a different tab: the strip keeps
// its width and the reader keeps their place in the row of tabs. The header
// keeps announcing the full truth ("Library · Title Z-A").
void LibraryListActivity::flipTitleDirection() {
  sTitleDescending = !sTitleDescending;
  const library::SortOrder order = sTitleDescending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
  // Written to whichever order is live: the grid's mode line can flip inside
  // the ★ view, where the shelf's own sort must stay untouched.
  if (sFavoritesView) {
    sFavSortOrder = order;
  } else {
    sSortOrder = order;
  }
  // Same invalidation as activating a tab: the filter holds POSITIONS in the
  // old order, and applyFilter also drops every remembered page boundary.
  applyFilter();
  selectedIndex = 0;
  topIndex = 0;
  requestUpdate(true);
}

void LibraryListActivity::cycleSortOrder(const bool forward) {
  tabCursor = (tabCursor + (forward ? 1 : kTabSlots - 1)) % kTabSlots;
  if (tabCursor == kFavTab) {
    // Landing on ★ applies it at once, like any sort tab. The order itself is
    // untouched: favorites compose with whatever order is current.
    sFavoritesView = true;
    applyFilter();
    selectedIndex = 0;
    topIndex = 0;
  } else if (tabCursor != kSearchTab) {
    sFavoritesView = false;
    sSortOrder = orderForTab(tabCursor);
    applyFilter();
    selectedIndex = 0;
    topIndex = 0;
  }
  requestUpdate();
}

// The strip needs the mode alone. The header strings carry a "Library ·" prefix
// that reads as four copies of the word once they sit side by side.
const char* tabLabelFor(const int tab) {
  if (tab == kRecentTab) return tr(STR_LIBRARY_TAB_RECENT);
  if (tab == kTitlesTab) return tr(STR_LIBRARY_TAB_TITLES);
  if (tab == kAuthorTab) return tr(STR_LIBRARY_TAB_AUTHOR);
  return tr(STR_LIBRARY_SEARCH);
}

const char* LibraryListActivity::sortOrderLabel() const {
  switch (sSortOrder) {
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
  const bool filteredView = !query.empty() || sFavoritesView;
  return filteredView ? static_cast<int>(filtered.size()) : static_cast<int>(index.bookCount());
}

// Entry position on screen to row position in the sort order. Identity while
// unfiltered, so the shelf costs nothing when nothing is typed.
int LibraryListActivity::rowFor(const int entry) const {
  if (query.empty() && !sFavoritesView) return entry;
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
  if (query.empty() && !sFavoritesView) return;

  // Folded the same way the stored folds were, articles removed included —
  // otherwise "the hobbit" searches for a word no record contains.
  const std::string needle = query.empty() ? std::string() : library::fold(query, /*stripArticle=*/true);
  const int total = static_cast<int>(index.bookCount());
  filtered.reserve(static_cast<size_t>(total));
  for (int row = 0; row < total; row++) {
    const uint16_t ordinal = index.ordinalForRow(currentOrder(), static_cast<uint16_t>(row));
    library::ClixRecord record{};
    if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) continue;
    // ★ narrows first, and composes with the query rather than replacing it:
    // searching within favorites is the natural reading of having both on.
    if (sFavoritesView) {
      std::string name;
      if (!index.readName(record, name)) continue;
      const library::FavoriteKey key{library::favoriteNameHash(name.data(), name.size()), record.fileSize};
      if (!favorites.contains(key)) continue;
    }
    if (query.empty()) {
      filtered.push_back(static_cast<uint16_t>(row));
      continue;
    }
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
  if (currentOrder() == library::SortOrder::AuthorAsc) {
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
    const uint16_t ordinal = index.ordinalForRow(currentOrder(), static_cast<uint16_t>(rowFor(entry)));
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
  const bool descending = currentOrder() == library::SortOrder::TitleDesc;
  const int total = rowCount();
  for (int entry = 0; entry < total; entry++) {
    const uint16_t ordinal = index.ordinalForRow(currentOrder(), static_cast<uint16_t>(rowFor(entry)));
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
  // Sorted by author the choice is which WORD the letters mean; sorted by title
  // it is the direction. One line, one idiom, one rule to learn. "A-Z"/"Z-A"
  // are letter symbols rather than words, so they carry no translation.
  const bool titleOrder = currentOrder() != library::SortOrder::AuthorAsc;
  const char* labels[2] = {titleOrder ? "A-Z" : tr(STR_LIBRARY_JUMP_GIVEN),
                           titleOrder ? "Z-A" : tr(STR_LIBRARY_JUMP_SURNAME)};
  const int active = titleOrder ? (sTitleDescending ? 1 : 0) : (jumpByGivenName ? 0 : 1);
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
bool LibraryListActivity::rowTextFor(const int entry, std::string& title, std::string& author, bool* isFavorite) {
  title.clear();
  author.clear();
  if (isFavorite != nullptr) *isFavorite = false;
  const uint16_t ordinal = index.ordinalForRow(currentOrder(), static_cast<uint16_t>(rowFor(entry)));
  library::ClixRecord record{};
  std::string name;
  if (ordinal != 0xFFFF && index.readRecord(ordinal, record) && index.readName(record, name)) {
    if (isFavorite != nullptr) {
      const library::FavoriteKey key{library::favoriteNameHash(name.data(), name.size()), record.fileSize};
      *isFavorite = favorites.contains(key);
    }
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
  // The menu owns every button while it is up, including the touch layer.
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finishAfterBackPress();
    return;
  }
  const int count = rowCount();

  // Details is a reading page: the only thing to do on it is leave.
  if (detailsView) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      detailsView = false;
      requestUpdate(true);
    }
    return;
  }

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
        if (currentOrder() == library::SortOrder::AuthorAsc) {
          jumpByGivenName = !jumpByGivenName;
          // The letters present as first names are not those present as
          // surnames. The cursor is on the mode line, not on a letter, so
          // nothing needs re-seating here — Down does that entering the grid.
          computeLettersPresent();
        } else {
          // In title order the mode line is the direction — the novice path to
          // the same flip the strip's hold offers. The letter SET is direction
          // blind (first letters do not change), so nothing needs recomputing.
          flipTitleDirection();
        }
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
  // Back in the ★ view exits the Library WITH the view intact — deliberately
  // not the search idiom. A query is a transient filter and Back undoes it
  // (above); the star is a PLACE — first tab, restored on return — and the
  // first cut treated it as a filter, so the reader's exit habit silently
  // wiped their view on every leave. The strip is the way to another view.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Dispatched on release, by held time, so one press means exactly one
    // thing. A hold is the secondary action of the FOCUSED context — the rule
    // the whole gesture map follows.
    if (mappedInput.getHeldTime() >= 800) {
      // The hold is the secondary action of what is focused. On the strip's
      // Titles tab it flips the direction — the triangle and the header both
      // change, so the flip explains itself. On a book row it opens the row's
      // own menu.
      if (tabsFocused) {
        if (tabCursor == kTitlesTab) flipTitleDirection();
        if (tabCursor == kFavTab) openFavoritesSortMenu();
        // Search's secondary action clears its own filter, without a trip
        // through the keyboard.
        if (tabCursor == kSearchTab && !query.empty()) {
          query.clear();
          applyFilter();
          selectedIndex = 0;
          topIndex = 0;
          requestUpdate(true);
        }
      } else {
        openBookMenu();
      }
      return;
    }
    if (tabsFocused) {
      if (tabCursor == kSearchTab) {
        openSearch();
      } else if (currentOrder() != library::SortOrder::DateDesc) {
        // Only where an alphabet exists to jump through. Sorted by date there
        // is no letter order to walk, so the press stays inert rather than
        // opening a grid whose every choice would land somewhere arbitrary.
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
    if (count > 0) openSelectedBook();
    return;
  }

  // Left and Right page. On this hardware ButtonNavigator maps them as aliases of
  // Up and Down (util/ButtonNavigator.h:47-53), so the second axis is unused and
  // paging is free — which matters at 69 books, where scrolling one row at a time
  // is 34 presses to the middle and paging is 5.
  // The strip stays navigable at zero rows — an empty ★ view is exactly when
  // the reader needs to move to another tab rather than being trapped.
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) && (tabsFocused || count > 0)) {
    if (tabsFocused) {
      cycleSortOrder(/*forward=*/true);
    } else {
      nextPage();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && (tabsFocused || count > 0)) {
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
    } else if (selectedIndex == 0 && !degraded) {
      // Degraded hides the strip, so it must not take focus either: cycling
      // orders behind a hidden strip would repaint the same walk-order list
      // under a different title.
      tabsFocused = true;
      tabCursor = sFavoritesView ? kFavTab : sortTabIndex(sSortOrder);
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
  const int slot = width / kTabSlots;
  for (int i = 0; i < kTabSlots; i++) {
    if (i == kFavTab) {
      // A drawn mark, not a word: nothing to translate, and it is the same
      // star the favorite rows carry, so the strip teaches the marker.
      constexpr int starW = 16;
      const int x = i * slot + (slot - starW) / 2;
      const int iconY = top + 4 + (lineH - starW) / 2;
      const bool selected = tabsFocused ? i == tabCursor : sFavoritesView;
      if (selected && tabsFocused) {
        renderer.fillRoundedRect(x - 6, top + 2, starW + 12, height - 4, 4, Color::Black);
        renderer.drawIconInverted(icon_star_16_bits, x, iconY, starW);
      } else {
        renderer.drawIcon(icon_star_16_bits, x, iconY, starW);
      }
      if (selected && !tabsFocused) renderer.fillRect(x, top + 4 + lineH + 1, starW, 1, true);
      continue;
    }
    const char* label = tabLabelFor(i);
    const int w = renderer.getTextWidth(SMALL_FONT_ID, label);
    // The active Titles tab carries its direction as a small drawn triangle, so
    // direction is never hidden state. Drawn with fillRect rather than a font
    // glyph: ▴/▾ are not guaranteed in this face at this size, and a 7-pixel
    // triangle needs no glyph coverage at all.
    const bool activeTitles = i == kTitlesTab && sortTabIndex(sSortOrder) == kTitlesTab;
    // An active query filters every view, so its tab says so: the same
    // state-on-the-tab idiom as the triangle, a dot for "filtered".
    const bool queryDot = i == kSearchTab && !query.empty();
    constexpr int triW = 7;
    constexpr int triH = 4;
    constexpr int triGap = 5;
    constexpr int dotW = 5;
    const int blockW = activeTitles ? w + triGap + triW : (queryDot ? w + triGap + dotW : w);
    const int x = i * slot + (slot - blockW) / 2;
    // Focused, the cursor marks the pill; unfocused, the active VIEW does —
    // which is ★ while the favorites view is on, not the sort composing it.
    const bool selected =
        tabsFocused ? i == tabCursor : (!sFavoritesView && i != kSearchTab && sortTabIndex(sSortOrder) == i);

    // Focused, the pill inverts, which is the strongest signal this panel has
    // that Left/Right now belong to the strip. Unfocused it stays a plain
    // underline, so the list keeps the reader's attention.
    if (selected && tabsFocused) {
      renderer.fillRoundedRect(x - 6, top + 2, blockW + 12, height - 4, 4, Color::Black);
    }
    renderer.drawText(SMALL_FONT_ID, x, top + 4, label, !(selected && tabsFocused));
    if (activeTitles) {
      // Four one-pixel rows stack into ▴ (A-Z) or ▾ (Z-A), centred beside the
      // label, in the same colour as the text so it survives the inverted pill.
      const bool dark = !(selected && tabsFocused);
      const int triX = x + w + triGap;
      const int triY = top + 4 + (lineH - triH) / 2;
      for (int row = 0; row < triH; row++) {
        const int rowW = sTitleDescending ? triW - 2 * row : 1 + 2 * row;
        renderer.fillRect(triX + (triW - rowW) / 2, triY + row, rowW, 1, dark);
      }
    }
    if (queryDot) {
      // The rounded rect at radius 2 on a 5-px square is a circle at this
      // size; same colour as the text so it survives the inverted pill.
      renderer.fillRoundedRect(x + w + triGap, top + 4 + (lineH - dotW) / 2, dotW, dotW, 2,
                               !(selected && tabsFocused) ? Color::Black : Color::White);
    }
    if (selected && !tabsFocused) renderer.fillRect(x, top + 4 + lineH + 1, blockW, 1, true);
  }
}

void LibraryListActivity::render(RenderLock&&) {
  // The menu paints over the retained frame — no clear, no page redraw, the
  // same overlay idiom Settings uses for its popups.
  if (popup.processRender(renderer, mappedInput)) return;
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const char* title = detailsView ? tr(STR_LIBRARY_MENU_DETAILS)
                      // The ★ view announces itself: a list that changes under an unchanged
                      // title reads as an indexing bug — it did, on the device.
                      : sFavoritesView ? tr(STR_LIBRARY_SORT_FAVORITES)
                      : degraded       ? tr(STR_LIBRARY_TITLE_UNSORTED)
                                       : sortOrderLabel();
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, title, true);
  } else {
    GUI.drawHeader(renderer, header, title);
  }

  if (detailsView) {
    measureRows();
    drawDetails();
  } else if (letterGrid) {
    drawLetterGrid();
  } else if (rowCount() == 0) {
    // The strip stays visible over an empty FILTERED view — it is the way out.
    // Only a card with no books at all drops it: there is nothing to sort.
    if (!degraded && index.bookCount() > 0) {
      measureRows();
      drawSortTabs(tabsTop);
    }
    // The empty ★ view teaches the gesture that fills it.
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2,
                              sFavoritesView ? tr(STR_LIBRARY_FAVORITES_EMPTY) : tr(STR_LIBRARY_EMPTY));
  } else {
    measureRows();
    if (!degraded) drawSortTabs(tabsTop);
    drawRows();
  }

  if (!detailsView) drawPositionReadout();
  // The bottom pair delivers Left/Right on this hardware, so labelling it
  // "Up/Down" describes the wrong axis — it pages the list, switches tabs and
  // steps letters, none of which is vertical. mapLabels takes previous/next
  // precisely so the caller can say what they do here. On the details page the
  // pair does nothing, and an empty label is how the hints bar says so.
  const char* prevLabel = detailsView ? "" : (letterGrid || tabsFocused ? tr(STR_DIR_LEFT) : tr(STR_LIBRARY_PAGE_PREV));
  const char* nextLabel =
      detailsView ? "" : (letterGrid || tabsFocused ? tr(STR_DIR_RIGHT) : tr(STR_LIBRARY_PAGE_NEXT));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), prevLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

// The Details page: where the stored author provenance finally reaches the
// reader. Every record has carried "where this author string came from" since
// the first build — folder name, the book's own reading cache, or the EPUB
// package document — and this is the screen honest enough to say it.
void LibraryListActivity::drawDetails() {
  const uint16_t ordinal = index.ordinalForRow(currentOrder(), static_cast<uint16_t>(rowFor(selectedIndex)));
  library::ClixRecord record{};
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record)) return;

  std::string title;
  std::string author;
  std::string name;
  std::string path;
  if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
  index.readAuthor(record, author);
  index.readName(record, name);
  index.readPath(record, path);
  // The folder is the path with the basename cut off; the root keeps its '/'.
  std::string folder = "/";
  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0) folder = path.substr(0, slash);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = LIBRARY_SIDE_PADDING;
  const int w = renderer.getScreenWidth() - 2 * LIBRARY_SIDE_PADDING;
  int y = listTop;

  const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, title.c_str(), w, 3);
  for (const auto& line : titleLines) {
    renderer.drawText(UI_12_FONT_ID, x, y, line.c_str(), true);
    y += renderer.getLineHeight(UI_12_FONT_ID);
  }

  if (!author.empty()) {
    y += metrics.verticalSpacing;
    renderer.drawText(UI_10_FONT_ID, x, y, renderer.truncatedText(UI_10_FONT_ID, author.c_str(), w).c_str(), true);
    y += renderer.getLineHeight(UI_10_FONT_ID);
    const char* provenance = nullptr;
    switch (library::recordAuthorProvenance(record)) {
      case library::CLIX_AUTHOR_FROM_FOLDER:
        provenance = tr(STR_LIBRARY_PROV_FOLDER);
        break;
      case library::CLIX_AUTHOR_FROM_CACHE:
        provenance = tr(STR_LIBRARY_PROV_CACHE);
        break;
      case library::CLIX_AUTHOR_FROM_OPF:
        provenance = tr(STR_LIBRARY_PROV_OPF);
        break;
      case library::CLIX_AUTHOR_UNKNOWN:
        // A guess pulled from the filename pattern: naming a source for it
        // would claim more than the build knows.
        break;
    }
    if (provenance != nullptr) {
      renderer.drawText(SMALL_FONT_ID, x, y, provenance, true);
      y += renderer.getLineHeight(SMALL_FONT_ID);
    }
  }

  // The file itself: name, folder, then size and format on one line.
  y += metrics.verticalSpacing * 2;
  const auto nameLines = renderer.wrappedText(SMALL_FONT_ID, name.c_str(), w, 2);
  for (const auto& line : nameLines) {
    renderer.drawText(SMALL_FONT_ID, x, y, line.c_str(), true);
    y += renderer.getLineHeight(SMALL_FONT_ID);
  }
  renderer.drawText(SMALL_FONT_ID, x, y, renderer.truncatedText(SMALL_FONT_ID, folder.c_str(), w).c_str(), true);
  y += renderer.getLineHeight(SMALL_FONT_ID);

  const char* formatToken = "";
  switch (library::recordFormat(record)) {
    case library::CLIX_FORMAT_EPUB:
      formatToken = "EPUB";
      break;
    case library::CLIX_FORMAT_TXT:
      formatToken = "TXT";
      break;
    case library::CLIX_FORMAT_MD:
      formatToken = "MD";
      break;
    case library::CLIX_FORMAT_XTC:
      formatToken = "XTC";
      break;
    case library::CLIX_FORMAT_OTHER:
      break;
  }
  char sizeLine[48];
  if (record.fileSize >= 1024u * 1024u) {
    const unsigned whole = record.fileSize / (1024u * 1024u);
    const unsigned tenth = (record.fileSize % (1024u * 1024u)) * 10u / (1024u * 1024u);
    snprintf(sizeLine, sizeof(sizeLine), "%u.%u MB %s", whole, tenth, formatToken);
  } else {
    snprintf(sizeLine, sizeof(sizeLine), "%u KB %s", static_cast<unsigned>(record.fileSize / 1024u), formatToken);
  }
  renderer.drawText(SMALL_FONT_ID, x, y, sizeLine, true);
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
  const bool grouped = currentOrder() == library::SortOrder::AuthorAsc;
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
    bool isFavorite = false;
    rowTextFor(entry, title, author, &isFavorite);
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
    if (isFavorite && !sFavoritesView) {
      // The mark replaces the row's book icon outright: the icon is decoration
      // every row shares, the star is information, and reusing the slot moves
      // no text. Skipped in the ★ view itself, where every row would carry it
      // and it would say nothing.
      renderer.drawIcon(icon_star_24_bits, LIBRARY_SIDE_PADDING + groupIndent, y + (height - LIBRARY_ICON_SIZE) / 2,
                        LIBRARY_ICON_SIZE);
    } else {
      // The pre-rotated copy, not listIcons' book: the raw blit shows that one
      // lying on its side (see gen_star_icon.py).
      renderer.drawIcon(icon_book_upright_24_bits, LIBRARY_SIDE_PADDING + groupIndent,
                        y + (height - LIBRARY_ICON_SIZE) / 2, LIBRARY_ICON_SIZE);
    }

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
