#include "LibraryListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryState.h>
#include <LibraryText.h>
#include <Logging.h>

#include <algorithm>

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

// One threshold for every hold on this screen, so the gesture feels the same
// wherever the reader tries it.
constexpr unsigned long kHoldMs = 800;

}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    // Long-press opt-in: rows and tabs carry InputLongPress, so a held tap is
    // the row menu / the tab's secondary action, mirroring the button holds.
    : UiTabListActivity("Library", renderer, mappedInput, /*wantsTouchLongPress=*/true) {
  // The uiScale spec binds the SMALL slot to the body font so settings rows
  // read label and value at one size — which on this screen erases the
  // title/author hierarchy the shelf exists for. Rebind THIS activity's small
  // slot to the real small font: author lines, strip labels, group headings
  // and the grid's mode line keep their pre-conversion size. Scoped to the
  // shelf — each activity owns its target, and the shared theme tokens carry
  // slot numbers, not sizes.
  uiTarget.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
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

// The strip needs the mode alone. The header strings carry a "Library ·" prefix
// that reads as four copies of the word once they sit side by side.
const char* tabLabelFor(const int tab) {
  if (tab == kRecentTab) return tr(STR_LIBRARY_TAB_RECENT);
  if (tab == kTitlesTab) return tr(STR_LIBRARY_TAB_TITLES);
  if (tab == kAuthorTab) return tr(STR_LIBRARY_TAB_AUTHOR);
  return tr(STR_LIBRARY_SEARCH);
}

int LibraryListActivity::tabCount() const { return kTabSlots; }

// The ACTIVE VIEW's tab — the ★ or the sort composing the shelf. Never Search:
// Search is an action on the strip, not a view, so the strip's own cursor
// (tabCursor) is the only state that can sit on it.
int LibraryListActivity::activeTab() const { return sFavoritesView ? kFavTab : sortTabIndex(sSortOrder); }

const char* LibraryListActivity::tabLabel(const int index) const { return tabLabelFor(index); }

int LibraryListActivity::selectedEntry() const {
  // Ring 0 (strip focused) keeps row 0 as the working selection, exactly as
  // the pre-ring code kept selectedIndex at 0 while the strip held the focus.
  const int entry = ringPos() - 1;
  return entry < 0 ? 0 : entry;
}

void LibraryListActivity::resetListPosition() {
  auto& n = activeNav();
  n.top = 0;
  if (n.selected != 0) n.selected = 1;
  pageStarts.clear();
}

void LibraryListActivity::onEnter() {
  // The shelf's posture comes back from disk before the base sizes the ring:
  // this reader deep-sleeps between sessions and wakes through a full boot,
  // so RAM state forgets the shelf several times a day — which read as "the
  // filters do not save" on the device. activeTab() reads these statics, and
  // the base's per-tab state must bind to the restored view, not the default.
  library::LibraryShelfState state;
  library::loadLibraryState(state);
  sFavoritesView = state.favoritesView;
  sTitleDescending = state.titleDescending;
  sSortOrder = state.shelfSort;
  sFavSortOrder = state.favSort;

  {
    // One lock across the base lifecycle AND the data phase: the base onEnter
    // schedules a paint, and the render task must not read the index or the
    // filter before they are in place. The rebuild also needs the lock for
    // the same reason the Settings rebuild does: the render task's SD-loaded
    // fonts read glyph data at draw time, and the walk needs the card to
    // itself.
    RenderLock lock(*this);
    UiTabListActivity::onEnter();
    app.on(ACTION_LETTER, &LibraryListActivity::letterActionTrampoline, this);
    app.on(ACTION_LETTER_MODE, &LibraryListActivity::letterModeActionTrampoline, this);
    tabCursor = activeTab();
    // Optimistic open: if an index exists, paint from it immediately and let
    // the user decide when to refresh. Only a missing or unreadable index
    // forces the walk, so entering the screen is normally instant.
    indexReady = openIndex();
    // Books arrived (or left) since the last build: every ingestion path marks
    // one flag, consumed here. The rebuild reconciles, so the newcomer tops
    // Recently added and every other book keeps its place.
    if (library::takeShelfStale() && indexReady) {
      index.close();
      indexReady = false;
    }
    if (!indexReady) {
      GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      indexReady = rebuildIndex() && openIndex();
    }
    degraded = indexReady && index.ranksDegraded();
    // Corrupt or unreadable favorites degrade to an empty set, logged inside;
    // the shelf itself must never be held up by its smallest file.
    favorites.load();
    // `filtered` belongs to THIS instance: without a rebuild here, a restored ★
    // view opened on an empty list until the reader wiggled the tabs.
    applyFilter();
    auto& n = activeNav();
    n.selected = 1;
    n.top = 0;
    restoreSelection(state.selected);
  }
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
    rowKeyFor(selectedEntry(), state.selected);
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
  const uint16_t ordinal = index.ordinalForRow(currentOrder(), static_cast<uint16_t>(rowFor(selectedEntry())));
  if (ordinal == 0xFFFF) return;

  library::ClixRecord record{};
  std::string path;
  if (!index.readRecord(ordinal, record) || !index.readPath(record, path)) {
    LOG_ERR("LIB", "cannot resolve path for row %d", selectedEntry());
    return;
  }
  // Staged for onExit: once the index closes, the selection can no longer be
  // resolved, and the book being opened is exactly the one to come back to.
  rowKeyFor(selectedEntry(), exitSelection);
  // A tap flash on the row would gray an unrelated element of the reader
  // screen this navigation opens.
  app.clearTapFlash();
  // Release the index handle first: on hardware SdFat allows one open reader per
  // path at a time, and the reader is about to open files of its own.
  index.close();
  indexReady = false;
  activityManager.goToReader(std::move(path));
}

void LibraryListActivity::activateIndex(int) { openSelectedBook(); }

void LibraryListActivity::onRowLongPress(int) { openBookMenu(); }

void LibraryListActivity::restoreSelection(const library::FavoriteKey& sel) {
  if (sel.nameHash == 0 && sel.fileSize == 0) return;
  const int count = rowCount();
  for (int entry = 0; entry < count; entry++) {
    library::FavoriteKey key;
    if (rowKeyFor(entry, key) && key == sel) {
      auto& n = activeNav();
      n.selected = entry + 1;
      n.top = entry;
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
  if (sFavoritesView) {
    applyFilter();
    resetListPosition();
  }
  requestUpdate(true);
}

void LibraryListActivity::openBookMenu() {
  if (!indexReady || rowCount() == 0) return;
  std::string title;
  std::string author;
  bool isFavorite = false;
  rowTextFor(selectedEntry(), title, author, &isFavorite);
  const std::vector<std::string> options{tr(STR_LIBRARY_MENU_OPEN),
                                         isFavorite ? tr(STR_LIBRARY_MENU_FAV_REMOVE) : tr(STR_LIBRARY_MENU_FAV_ADD),
                                         tr(STR_LIBRARY_MENU_DETAILS), tr(STR_DELETE)};
  popup.show(title.c_str(), options, 0, [this](const int choice) {
    if (choice == 0) openSelectedBook();
    if (choice == 1) toggleFavoriteAt(selectedEntry());
    if (choice == 2) {
      detailsView = true;
      requestUpdate(true);
    }
    if (choice == 3) promptDeleteSelectedBook();
  });
  requestUpdate();
}

void LibraryListActivity::promptDeleteSelectedBook() {
  const uint16_t ordinal = index.ordinalForRow(currentOrder(), static_cast<uint16_t>(rowFor(selectedEntry())));
  library::ClixRecord record{};
  std::string path;
  std::string title;
  if (ordinal == 0xFFFF || !index.readRecord(ordinal, record) || !index.readPath(record, path)) return;
  if (!index.readTitle(record, title) || title.empty()) index.readName(record, title);
  library::FavoriteKey key;
  const bool hasKey = rowKeyFor(selectedEntry(), key);

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
                           auto& n = activeNav();
                           n.selected = 1;
                           n.top = 0;
                           pageStarts.clear();
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
    resetListPosition();
    requestUpdate(true);
  });
  requestUpdate();
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
  // old order, and every remembered page boundary died with them.
  applyFilter();
  resetListPosition();
  requestUpdate(true);
}

void LibraryListActivity::cycleSortOrder(const bool forward) {
  tabCursor = (tabCursor + (forward ? 1 : kTabSlots - 1)) % kTabSlots;
  if (tabCursor == kFavTab) {
    // Landing on ★ applies it at once, like any sort tab. The order itself is
    // untouched: favorites compose with whatever order is current.
    sFavoritesView = true;
    applyFilter();
    // The view changed, so the ring below belongs to the NEW tab; the strip
    // keeps the focus it had (ring 0), the list under it starts at the top.
    auto& n = activeNav();
    n.selected = 0;
    n.top = 0;
    pageStarts.clear();
  } else if (tabCursor != kSearchTab) {
    sFavoritesView = false;
    sSortOrder = orderForTab(tabCursor);
    applyFilter();
    auto& n = activeNav();
    n.selected = 0;
    n.top = 0;
    pageStarts.clear();
  }
  requestUpdate();
}

void LibraryListActivity::stepTab(const int direction) { cycleSortOrder(direction > 0); }

// Touch tap on a strip pill: the same application the button cycle does, minus
// the travel. Search still waits for nothing — a tap IS the deliberate act the
// sideways press wasn't.
void LibraryListActivity::onTabAction(const int index) {
  if (index == kSearchTab) {
    tabCursor = kSearchTab;
    app.clearTapFlash();
    openSearch();
    return;
  }
  tabCursor = index;
  if (index == kFavTab) {
    sFavoritesView = true;
  } else {
    sFavoritesView = false;
    sSortOrder = orderForTab(index);
  }
  applyFilter();
  // Tab taps land with the tab bar focused, like the button cycle leaves it.
  auto& n = activeNav();
  n.selected = 0;
  n.top = 0;
  pageStarts.clear();
  app.clearTapFlash();
  requestUpdate();
}

// A held tap on a tab pill is the tab's secondary action — the same map the
// Confirm hold follows on the focused strip.
void LibraryListActivity::onTabLongPress(const int index) {
  if (index == kTitlesTab) {
    flipTitleDirection();
    return;
  }
  if (index == kFavTab) {
    openFavoritesSortMenu();
    return;
  }
  if (index == kSearchTab && !query.empty()) {
    // Search's secondary action clears its own filter, without a trip
    // through the keyboard.
    query.clear();
    applyFilter();
    resetListPosition();
    requestUpdate(true);
  }
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

int LibraryListActivity::rowCount() const {
  const bool filteredView = !query.empty() || sFavoritesView;
  return filteredView ? static_cast<int>(filtered.size()) : static_cast<int>(index.bookCount());
}

int LibraryListActivity::listCount() const { return rowCount(); }

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
// Pure data: the ring/viewport reset moved to the callers, which know whether
// the strip keeps the focus and which tab's state the reset lands on.
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
      auto& n = activeNav();
      n.selected = entry + 1;
      n.top = entry;
      pageStarts.clear();
      return;
    }
  }
}

void LibraryListActivity::openLetterGrid() {
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

// The mode line above the grid. Sorted by author the choice is which WORD the
// letters mean; sorted by title it is the direction — the novice path to the
// same flip the strip's hold offers.
void LibraryListActivity::toggleLetterGridMode() {
  if (currentOrder() == library::SortOrder::AuthorAsc) {
    jumpByGivenName = !jumpByGivenName;
    // The letters present as first names are not those present as surnames.
    // The cursor is on the mode line, not on a letter, so nothing needs
    // re-seating here — Down does that entering the grid.
    computeLettersPresent();
  } else {
    // The letter SET is direction blind (first letters do not change), so
    // nothing needs recomputing.
    flipTitleDirection();
  }
  requestUpdate();
}

void LibraryListActivity::letterActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LibraryListActivity*>(user);
  if (!self->letterGrid) return;
  if (event.value < 0 || event.value >= kLetterCount) return;
  if (!(self->lettersPresent & (1u << event.value))) return;
  self->jumpToLetter(static_cast<char>('a' + event.value));
  self->letterGrid = false;
  self->app.clearTapFlash();
  self->requestUpdate();
}

void LibraryListActivity::letterModeActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LibraryListActivity*>(user);
  if (!self->letterGrid) return;
  // Tapping the already-active choice states nothing new.
  const bool titleOrder = currentOrder() != library::SortOrder::AuthorAsc;
  const int active = titleOrder ? (sTitleDescending ? 1 : 0) : (self->jumpByGivenName ? 0 : 1);
  if (event.value == active) return;
  self->toggleLetterGridMode();
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
                           // The result belongs to the list: the cursor lands
                           // on the first surviving row, not on the strip.
                           auto& n = activeNav();
                           n.selected = 1;
                           n.top = 0;
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

bool LibraryListActivity::handleCustomInput() {
  // Opened by a long press on a button pair, that button is still down: its
  // release belongs to the gesture that got us here, not to this screen.
  if (openingGestureLock_.consumeRelease(mappedInput)) return true;

  // The menu owns every button while it is up, including the touch layer.
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finishAfterBackPress();
    return true;
  }

  // Details is a reading page: the only thing to do on it is leave.
  if (detailsView) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      detailsView = false;
      requestUpdate(true);
    }
    return true;
  }

  // The grid owns every button while it is open, so its block runs FIRST:
  // sitting below the Back handlers, its own Back was unreachable.
  if (letterGrid) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      letterGrid = false;
      requestUpdate();
      return true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Refused on a letter no book has. Jumping to where it WOULD fall is a
      // correct answer to a question the reader did not ask, and the grid has
      // already said the key is inert by not drawing it.
      if (letterCursor >= 0 && (lettersPresent & (1u << letterCursor))) {
        jumpToLetter(static_cast<char>('a' + letterCursor));
        letterGrid = false;
        requestUpdate();
      }
      return true;
    }
    // letterCursor == -1 is the mode line above the grid, reached by pressing Up
    // from the top row — the same idiom the sort strip uses, so there is one rule
    // to learn rather than two.
    if (letterCursor < 0) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
          mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        toggleLetterGridMode();
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
      // Touch reaches the letters and the mode line through the app's hit
      // rects even while a button cursor sits on the mode line.
      routeModalTouch();
      return true;
    }

    int delta = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) delta = 1;
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) delta = -1;
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) delta = kLetterCols;
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (letterCursor < kLetterCols) {
        letterCursor = -1;
        requestUpdate();
        return true;
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
    routeModalTouch();
    return true;
  }

  // Swipes page the viewport without moving the selection, like every FUI
  // list — but this list's back-paging history describes a path the viewport
  // just left, so the boundaries die with the swipe.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    auto& n = activeNav();
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? n.visibleRows : -n.visibleRows;
    if (n.scrollBy(delta, rowCount())) {
      pageStarts.clear();
      requestUpdate();
    }
    return true;
  }

  return false;
}

bool LibraryListActivity::handleButtons() {
  const int count = rowCount();
  auto& n = activeNav();

  // Back clears the filter before it leaves. A shelf showing 7 of 60 books is a
  // state the reader must be able to undo, and giving it the press they would
  // reach for anyway costs no screen space and needs no explaining.
  // Back in the ★ view exits the Library WITH the view intact — deliberately
  // not the search idiom. A query is a transient filter and Back undoes it;
  // the star is a PLACE — first tab, restored on return. The strip is the way
  // to another view.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!query.empty()) {
      query.clear();
      applyFilter();
      resetListPosition();
      requestUpdate();
    } else {
      finishAfterBackPress();
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Dispatched on release, by held time, so one press means exactly one
    // thing. A hold is the secondary action of the FOCUSED context — the rule
    // the whole gesture map follows.
    if (mappedInput.getHeldTime() >= kHoldMs) {
      // On the strip's Titles tab the hold flips the direction — the triangle
      // and the header both change, so the flip explains itself. On a book row
      // it opens the row's own menu.
      if (tabsFocused()) {
        if (tabCursor == kTitlesTab) flipTitleDirection();
        if (tabCursor == kFavTab) openFavoritesSortMenu();
        if (tabCursor == kSearchTab && !query.empty()) {
          query.clear();
          applyFilter();
          resetListPosition();
          requestUpdate(true);
        }
      } else {
        openBookMenu();
      }
      return true;
    }
    if (tabsFocused()) {
      if (tabCursor == kSearchTab) {
        openSearch();
      } else if (currentOrder() != library::SortOrder::DateDesc) {
        openLetterGrid();
      }
      return true;
    }
    if (count > 0) openSelectedBook();
    return true;
  }

  // Left and Right page. On this hardware ButtonNavigator maps them as aliases of
  // Up and Down (util/ButtonNavigator.h:47-53), so the second axis is unused and
  // paging is free — which matters at 69 books, where scrolling one row at a time
  // is 34 presses to the middle and paging is 5.
  // The strip stays navigable at zero rows — an empty ★ view is exactly when
  // the reader needs to move to another tab rather than being trapped.
  // Holding Left goes all the way back instead of one page: the top of the list
  // is an exact place. Its end deliberately has no such shortcut — a page's size
  // is only known once drawn, so a jump there would land near the last book
  // rather than on it, which is not what "last page" promises.
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) && (tabsFocused() || count > 0)) {
    if (tabsFocused()) {
      cycleSortOrder(/*forward=*/true);
    } else {
      nextPage();
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && (tabsFocused() || count > 0)) {
    if (tabsFocused()) {
      cycleSortOrder(/*forward=*/false);
    } else if (mappedInput.getHeldTime() >= kHoldMs) {
      // The visited-page history is a path; jumping off it makes those
      // boundaries meaningless, exactly as the letter jump does.
      n.selected = 1;
      n.top = 0;
      pageStarts.clear();
      requestUpdate();
    } else {
      previousPage();
    }
    return true;
  }

  // The list PAGES, it does not scroll. On e-ink moving one row costs the same
  // full-panel refresh as turning a whole page, so scrolling spends the panel's
  // most expensive operation on its smallest possible result. Up and Down move
  // within the page; at an edge they turn it and land on the far row, so the
  // reader never loses the sense of a fixed frame.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (tabsFocused()) {
      // already at the top
    } else if (n.selected == 1 && !degraded) {
      // Degraded hides the strip, so it must not take focus either: cycling
      // orders behind a hidden strip would repaint the same walk-order list
      // under a different title.
      n.selected = 0;
      tabCursor = activeTab();
      requestUpdate();
    } else if (n.selected - 1 > n.top) {
      n.selected--;
      requestUpdate();
    } else if (n.top > 0) {
      previousPage(/*selectLast=*/true);
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down) && count > 0) {
    if (tabsFocused()) {
      n.selected = 1;
      requestUpdate();
    } else if (n.selected - 1 < n.top + n.visibleRows - 1 && n.selected - 1 < count - 1) {
      n.selected++;
      requestUpdate();
    } else if (n.top + n.visibleRows < count) {
      nextPage();
    }
    return true;
  }

  return false;
}

// Touch routing for the modal grid: handleCustomInput consumes the whole pass
// while a mode is up, so the base's routeListTouch never runs — the app's hit
// rects (letters, mode line) are routed here instead.
void LibraryListActivity::routeModalTouch() {
  const auto route = UiAppHost::routeTouch(mappedInput, /*withLongPress=*/true);
  if (route.routed && app.invalidated()) requestUpdate();
}

// The strip takes its height from its own label line, exactly as the manual
// renderer did (lineH + 8): a fixed band clipped the labels as soon as the UI
// scale grew the small font. Details and the grid consume the same height so
// every mode's content starts at the same y.
int16_t LibraryListActivity::sortStripHeight(UiScreen& screen) const {
  return static_cast<int16_t>(screen.target().lineHeight(screen.theme().smallText.font) + 8);
}

// The sort strip: every mode visible at once, the active one underlined. On a
// panel that refreshes whole, showing the alternatives costs nothing per frame
// and saves a menu round-trip to discover them.
//
// The band is a fui::tabBar — which is what makes the pills tappable — with
// the shelf's own two-state treatment: focused, the cursor's pill inverts
// (the strongest signal this panel has that Left/Right now belong to the
// strip); unfocused, the active VIEW carries a plain underline, so the list
// keeps the reader's attention. No band fill: on a 1-bit panel the dithered
// grey is a literal checkerboard, the same pattern the selected row uses.
void LibraryListActivity::buildSortTabs(UiScreen& screen) {
  fui::TabItem tabs[kTabSlots];
  for (int i = 0; i < kTabSlots; i++) {
    if (i == kFavTab) {
      // A drawn mark, not a word: nothing to translate, and it is the same
      // star the favorite rows carry, so the strip teaches the marker.
      tabs[i].icon = fui::bitmapFromIcon(icon_star_16_fui);
    } else {
      tabs[i].label = tabLabelFor(i);
    }
    tabs[i].value = static_cast<int16_t>(i);
    // Focused, the cursor marks the pill; unfocused, the active VIEW does —
    // which is ★ while the favorites view is on, not the sort composing it.
    tabs[i].selected = tabsFocused()    ? i == tabCursor
                       : sFavoritesView ? i == kFavTab
                                        : (i != kSearchTab && sortTabIndex(sSortOrder) == i);
  }

  fui::TabBarProps props;
  props.tabs = tabs;
  props.count = kTabSlots;
  props.action = ACTION_TAB;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.text = screen.theme().smallText;
  props.tabInset = fui::Insets{2, 0, 2, 0};
  props.contentInset = fui::Insets{2, 6, 2, 6};
  props.minTouchSize = screen.theme().minTouchSize;

  fui::StyleSet styles;
  styles.explicitlySet = true;
  styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  if (tabsFocused()) {
    styles.selected.background = fui::Paint::solid(fui::Color::Black);
    styles.selected.foreground = fui::Paint::solid(fui::Color::White);
    styles.selected.radius = 4;
  } else {
    styles.selected.foreground = fui::Paint::solid(fui::Color::Black);
    props.selectedUnderline = 1;
  }
  styles.focused = styles.selected;
  styles.active = styles.selected;
  props.tabStyles = styles;

  const fui::Rect band = screen.takeTop(sortStripHeight(screen));
  fui::tabBar(screen.frame(), band, props);

  // The two state decorations no component slot carries, drawn on the band
  // through the same target. The active Titles tab tells its direction as a
  // small triangle, so direction is never hidden state; an active query
  // filters every view, so its tab says so with a dot — the same
  // state-on-the-tab idiom.
  const int16_t slot = static_cast<int16_t>(band.width / kTabSlots);
  const int16_t lineH = screen.target().lineHeight(props.text.font);
  const bool activeTitles = sortTabIndex(sSortOrder) == kTitlesTab;
  if (activeTitles) {
    const int16_t w = screen.target().measureText(props.text.font, tabLabelFor(kTitlesTab), props.text).width;
    const int16_t x = static_cast<int16_t>(band.x + kTitlesTab * slot + (slot - w) / 2 + w + 5);
    const int16_t midY = static_cast<int16_t>(band.y + 2 + (lineH + 4) / 2);
    constexpr int16_t triW = 7;
    constexpr int16_t triH = 4;
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    if (sTitleDescending) {
      screen.target().triangle(fui::Point{x, static_cast<int16_t>(midY - triH / 2)},
                               fui::Point{static_cast<int16_t>(x + triW), static_cast<int16_t>(midY - triH / 2)},
                               fui::Point{static_cast<int16_t>(x + triW / 2), static_cast<int16_t>(midY + triH / 2)},
                               ink);
    } else {
      screen.target().triangle(fui::Point{static_cast<int16_t>(x + triW / 2), static_cast<int16_t>(midY - triH / 2)},
                               fui::Point{x, static_cast<int16_t>(midY + triH / 2)},
                               fui::Point{static_cast<int16_t>(x + triW), static_cast<int16_t>(midY + triH / 2)}, ink);
    }
  }
  if (!query.empty()) {
    const int16_t w = screen.target().measureText(props.text.font, tabLabelFor(kSearchTab), props.text).width;
    const int16_t x = static_cast<int16_t>(band.x + kSearchTab * slot + (slot - w) / 2 + w + 5);
    constexpr int16_t dotW = 5;
    const int16_t y = static_cast<int16_t>(band.y + 2 + (lineH + 4 - dotW) / 2);
    screen.target().fill(fui::Rect{x, y, dotW, dotW}, fui::Paint::solid(fui::Color::Black), 2);
  }
}

// The list itself. Only the visible window is materialized — strings and
// ListItems for at most one page — and the window is laid out with the same
// accumulation the widget uses (uniform rows, shorter section headers), so
// the page the reader sees is exactly the page navigation counts. The widget
// then receives the window as a list that fits whole: count = what was built,
// topIndex = 0, and the true position stays in the activity's ListNav.
void LibraryListActivity::buildRows(UiScreen& screen) {
  auto& n = activeNav();
  const int count = rowCount();
  // Sorted by author, the permutation already places one author's books
  // consecutively, so grouping costs one header row per run and no extra
  // pass. The author then appears once above the run instead of under every
  // title, which is what makes the shelf answer "what else has this person
  // written".
  const bool grouped = currentOrder() == library::SortOrder::AuthorAsc;

  fui::ListProps props;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  // One line per title, truncated by the widget. Decided with upstream: "more
  // books in the screen, even if half the name is hidden" — the SDK-side
  // per-item height follow-up brings the wrapped titles back.
  props.labelText.maxLines = 1;
  // Author headings never carried a rule; whitespace and proximity group.
  props.headerUnderline = false;
  // The position readout carries "where am I"; a scroll track beside it would
  // say the same thing twice.
  props.scrollIndicator = false;
  const auto rowType = grouped ? UiListRowType::SingleLine : UiListRowType::WithSubtitle;
  props.rowHeight = uiListRowHeight(screen.theme(), rowType);
  if (props.rowGap < 0) props.rowGap = screen.theme().listRowGap;

  const fui::Rect band = screen.body();
  const int16_t rowH = props.rowHeight;
  const int16_t rowGap = props.rowGap;
  const uint16_t visibleCap = fui::listVisibleRows(band, rowH, rowGap);
  // Header geometry, mirroring components/lists/list.h exactly: header row =
  // small line + 4, sectionGap above every non-first header.
  const int16_t headerLh = screen.target().lineHeight(screen.theme().smallText.font);
  const int16_t headerH = static_cast<int16_t>(headerLh + 4);

  if (n.top < 0) n.top = 0;
  if (n.top > count - 1) n.top = count - 1;

  // The window buffers are sized once per build and never grow while items
  // hold pointers into them: c_str() stability is what makes the borrow safe.
  const size_t cap = static_cast<size_t>(visibleCap) + 1;
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (winHeaders.size() < cap) winHeaders.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap * 2) winItems.reserve(cap * 2);

  const fui::BitmapRef bookIcon = listIconFor(UIIcon::Book);
  const fui::BitmapRef starIcon = fui::bitmapFromIcon(icon_star_24_fui);

  int16_t cursorY = 0;
  int books = 0;
  int headers = 0;
  int selItem = -1;
  int lastBookItem = -1;
  const int displayEntry = n.selected > 0 ? n.selected - 1 : 0;
  for (int entry = n.top; entry < count; entry++) {
    bool isFavorite = false;
    std::string& title = winTitles[static_cast<size_t>(books)];
    std::string& author = winAuthors[static_cast<size_t>(books)];
    rowTextFor(entry, title, author, &isFavorite);

    // The first row of a page always carries its heading: without it a page
    // can open on books whose author was named on the page before.
    const bool startsGroup =
        grouped && !author.empty() && (books == 0 || author != winAuthors[static_cast<size_t>(books - 1)]);

    // Fit check, mirroring the widget's own accumulation — a heading is never
    // emitted without the book it names (the widget's height break would
    // strand it as a trailing orphan).
    const int itemIdx = static_cast<int>(winItems.size());
    int16_t needed = static_cast<int16_t>(rowH);
    if (startsGroup) {
      const int16_t pad = itemIdx != 0 ? props.sectionGap : 0;
      needed = static_cast<int16_t>(needed + pad + headerH + rowGap);
    }
    const int bookItemIdx = itemIdx + (startsGroup ? 1 : 0);
    if (cursorY + needed > band.height || books >= static_cast<int>(visibleCap) ||
        bookItemIdx >= static_cast<int>(visibleCap)) {
      break;
    }

    if (startsGroup) {
      // Written surname-first, as a catalogue does: the shelf is ORDERED by
      // surname, and printing "Anton Chekhov" above a run that sits between
      // Chateaubriand and Crane makes the order look arbitrary. Only in author
      // order: elsewhere the natural spelling reads better.
      // Into its OWN storage, NOT back into the author slot: the run
      // comparison above reads the next row's author straight from the index,
      // so "Xun, Lu" would never match "Lu Xun" and every row after a heading
      // would start a fresh group.
      std::string& heading = winHeaders[static_cast<size_t>(headers++)];
      heading = author;
      const size_t lastSpace = heading.find_last_of(' ');
      if (lastSpace != std::string::npos && lastSpace + 1 < heading.size()) {
        heading = heading.substr(lastSpace + 1) + ", " + heading.substr(0, lastSpace);
      }
      fui::ListItem header;
      header.isHeader = true;
      header.label = heading.c_str();
      winItems.push_back(header);
      const int16_t pad = itemIdx != 0 ? props.sectionGap : 0;
      cursorY = static_cast<int16_t>(cursorY + pad + headerH + rowGap);
    }

    fui::ListItem item;
    item.label = title.c_str();
    if (!grouped && !author.empty()) item.subtitle = author.c_str();
    // The mark replaces the row's book icon outright: the icon is decoration
    // every row shares, the star is information, and reusing the slot moves
    // no text. Skipped in the ★ view itself, where every row would carry it
    // and it would say nothing.
    item.icon = (isFavorite && !sFavoritesView) ? starIcon : bookIcon;
    item.actionValue = static_cast<int16_t>(entry);
    if (entry == displayEntry) selItem = static_cast<int>(winItems.size());
    lastBookItem = static_cast<int>(winItems.size());
    winItems.push_back(item);
    cursorY = static_cast<int16_t>(cursorY + rowH + rowGap);
    books++;
  }

  // Report how much this page held, for the next input pass to page by; then
  // put the ring back inside it. previousPage() aims past the end because a
  // page's size is only known once built — clamp now that it has been.
  n.visibleRows = books > 0 ? books : 1;
  if (n.selected - 1 >= n.top + n.visibleRows) {
    n.selected = n.top + n.visibleRows;
    selItem = lastBookItem;
  }
  if (n.selected - 1 >= count) n.selected = count;

  props.items = winItems.data();
  props.count = static_cast<uint16_t>(winItems.size());
  props.topIndex = 0;
  props.selectedIndex = static_cast<int16_t>(selItem);
  screen.list(props);
}

// The Details page: where the stored author provenance finally reaches the
// reader. Every record has carried "where this author string came from" since
// the first build — folder name, the book's own reading cache, or the EPUB
// package document — and this is the screen honest enough to say it.
void LibraryListActivity::buildDetails(UiScreen& screen) {
  const uint16_t ordinal = index.ordinalForRow(currentOrder(), static_cast<uint16_t>(rowFor(selectedEntry())));
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
  auto& target = screen.target();
  const fui::Rect body = screen.body();
  const int16_t x = static_cast<int16_t>(body.x + LIBRARY_SIDE_PADDING);
  const int16_t w = static_cast<int16_t>(body.width - 2 * LIBRARY_SIDE_PADDING);
  int16_t y = body.y;

  // Each block is drawn into a rect of exactly its measured height, so the
  // vertical centering layoutText applies is a no-op and the page reads
  // top-down as it always has.
  const auto drawBlock = [&](const char* text, fui::TextStyle style) {
    const fui::Size size = fui::measureWrappedText(target, text, style, w);
    target.text(fui::Rect{x, y, w, size.height}, text, style);
    y = static_cast<int16_t>(y + size.height);
  };

  fui::TextStyle titleStyle = screen.theme().titleText;
  titleStyle.maxLines = 3;
  drawBlock(title.c_str(), titleStyle);

  if (!author.empty()) {
    y = static_cast<int16_t>(y + metrics.verticalSpacing);
    drawBlock(author.c_str(), screen.theme().bodyText);
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
    if (provenance != nullptr) drawBlock(provenance, screen.theme().smallText);
  }

  // The file itself: name, folder, then size and format on one line.
  y = static_cast<int16_t>(y + metrics.verticalSpacing * 2);
  fui::TextStyle fileStyle = screen.theme().smallText;
  fileStyle.maxLines = 2;
  drawBlock(name.c_str(), fileStyle);
  drawBlock(folder.c_str(), screen.theme().smallText);

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
  drawBlock(sizeLine, screen.theme().smallText);
}

// The A-Z grid, as components: one button per PRESENT letter (an absent letter
// is simply not drawn — its slot stays empty and nothing moves, because the
// grid's positions come from the alphabet's index), and two mode buttons above
// it. Buttons are what make the letters tappable.
void LibraryListActivity::buildLetterGrid(UiScreen& screen) {
  const fui::Rect body = screen.body();
  auto& target = screen.target();
  const int cell = (body.width - 2 * LIBRARY_SIDE_PADDING) / kLetterCols;
  const int rows = (kLetterCount + kLetterCols - 1) / kLetterCols;
  const int cellH = body.height / (rows + 1);

  // Both modes shown, not just the active one. Printing only the current choice
  // hides the fact that there IS a choice — the same reason the sort strip lists
  // every mode. Sorted by author the choice is which WORD the letters mean;
  // sorted by title it is the direction. "A-Z"/"Z-A" are letter symbols rather
  // than words, so they carry no translation.
  const bool titleOrder = currentOrder() != library::SortOrder::AuthorAsc;
  const char* labels[2] = {titleOrder ? "A-Z" : tr(STR_LIBRARY_JUMP_GIVEN),
                           titleOrder ? "Z-A" : tr(STR_LIBRARY_JUMP_SURNAME)};
  const int active = titleOrder ? (sTitleDescending ? 1 : 0) : (jumpByGivenName ? 0 : 1);
  const fui::TextStyle modeText = screen.theme().smallText;
  const int16_t modeH = target.lineHeight(modeText.font);
  constexpr int16_t kModeGap = 20;
  int16_t labelW[2];
  for (int i = 0; i < 2; i++) labelW[i] = target.measureText(modeText.font, labels[i], modeText).width;
  int16_t mx = static_cast<int16_t>(body.x + (body.width - (labelW[0] + labelW[1] + kModeGap)) / 2);
  const int16_t modeY = static_cast<int16_t>(body.y + 2);

  for (int i = 0; i < 2; i++) {
    // Focused, the active choice inverts — the strongest signal this panel has
    // that Left/Right are about to change it. Unfocused it keeps an underline,
    // so the line stays quiet while the grid holds attention.
    const bool on = i == active;
    fui::ButtonProps mode;
    mode.label = labels[i];
    mode.action = ACTION_LETTER_MODE;
    mode.value = static_cast<int16_t>(i);
    mode.text = modeText;
    mode.styles.explicitlySet = true;
    mode.styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
    if (on && letterCursor < 0) {
      mode.styles.normal.background = fui::Paint::solid(fui::Color::Black);
      mode.styles.normal.foreground = fui::Paint::solid(fui::Color::White);
      mode.styles.normal.radius = 4;
    }
    const fui::Rect slot{static_cast<int16_t>(mx - 5), static_cast<int16_t>(modeY - 2),
                         static_cast<int16_t>(labelW[i] + 10), static_cast<int16_t>(modeH + 4)};
    screen.button(mode, slot);
    if (on && letterCursor >= 0) {
      target.fill(fui::Rect{mx, static_cast<int16_t>(modeY + modeH + 1), labelW[i], 1},
                  fui::Paint::solid(fui::Color::Black));
    }
    mx = static_cast<int16_t>(mx + labelW[i] + kModeGap);
  }

  // Centre the block itself: 26 letters do not fill 5 columns evenly, and laid
  // out from the left margin the remainder all landed on one side.
  const int16_t top = static_cast<int16_t>(body.y + cellH / 2);
  const int16_t originX = static_cast<int16_t>(body.x + (body.width - kLetterCols * cell) / 2);
  char letterLabels[kLetterCount][2];

  for (int i = 0; i < kLetterCount; i++) {
    // A letter no book starts with is simply not drawn. Three attempts at
    // showing an unavailable letter failed on a 1-bit panel: a smaller font
    // read as inconsistent typography, dithering the glyph erased it, and an
    // outlined cursor said nothing legible. Not drawing it says it plainly.
    if (!(lettersPresent & (1u << i))) continue;
    const int16_t cx = static_cast<int16_t>(originX + (i % kLetterCols) * cell);
    const int16_t cy = static_cast<int16_t>(top + (i / kLetterCols) * cellH);
    const int16_t pillW = static_cast<int16_t>(cell - 6);
    const int16_t pillH = static_cast<int16_t>(cellH - 6);
    letterLabels[i][0] = static_cast<char>('A' + i);
    letterLabels[i][1] = '\0';
    fui::ButtonProps key;
    key.label = letterLabels[i];
    key.action = ACTION_LETTER;
    key.value = static_cast<int16_t>(i);
    key.text = screen.theme().bodyText;
    key.styles.explicitlySet = true;
    key.styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
    if (i == letterCursor) {
      key.styles.normal.background = fui::Paint::solid(fui::Color::Black);
      key.styles.normal.foreground = fui::Paint::solid(fui::Color::White);
      key.styles.normal.radius = 4;
    }
    screen.button(key, fui::Rect{static_cast<int16_t>(cx + (cell - pillW) / 2), cy, pillW, pillH});
  }
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the header band, above the button hints. The strip sits
  // between the header and the list, and takes its height from the list
  // rather than overlaying it.
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing), 0});

  if (detailsView) {
    screen.spacer(static_cast<int16_t>(sortStripHeight(screen) + metrics.verticalSpacing));
    buildDetails(screen);
    return;
  }
  if (letterGrid) {
    screen.spacer(static_cast<int16_t>(sortStripHeight(screen) + metrics.verticalSpacing));
    buildLetterGrid(screen);
    return;
  }

  // The strip stays visible over an empty FILTERED view — it is the way out.
  // Only a card with no books at all drops it: there is nothing to sort.
  if (!degraded && index.bookCount() > 0) {
    buildSortTabs(screen);
    screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  }

  if (rowCount() == 0) {
    // The empty ★ view teaches the gesture that fills it.
    screen.centeredText(sFavoritesView ? tr(STR_LIBRARY_FAVORITES_EMPTY) : tr(STR_LIBRARY_EMPTY),
                        screen.theme().bodyText);
    return;
  }
  buildRows(screen);
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

  renderUi();

  if (!detailsView) drawPositionReadout();
  // The bottom pair delivers Left/Right on this hardware, so labelling it
  // "Up/Down" describes the wrong axis — it pages the list, switches tabs and
  // steps letters, none of which is vertical. mapLabels takes previous/next
  // precisely so the caller can say what they do here. On the details page the
  // pair does nothing, and an empty label is how the hints bar says so.
  const char* prevLabel =
      detailsView ? "" : (letterGrid || tabsFocused() ? tr(STR_DIR_LEFT) : tr(STR_LIBRARY_PAGE_PREV));
  const char* nextLabel =
      detailsView ? "" : (letterGrid || tabsFocused() ? tr(STR_DIR_RIGHT) : tr(STR_LIBRARY_PAGE_NEXT));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), prevLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

// "12/69 books" at the bottom right: which book is selected, out of how many.
//
// NOT a page count. How many rows fit can vary with the view (author headings
// consume band height), so a page total grows and shrinks as you scroll —
// exactly the "6/6 then 8/8" the first version produced. The book position is
// stable by construction, and it answers the question the reader actually
// has: how far in am I, and how much is left.
void LibraryListActivity::drawPositionReadout() {
  const int count = rowCount();
  if (count <= 0) return;

  char buf[32];
  snprintf(buf, sizeof(buf), tr(STR_LIBRARY_POSITION), selectedEntry() + 1, count);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
  const int x = renderer.getScreenWidth() - width - LIBRARY_SIDE_PADDING;
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(SMALL_FONT_ID, x, y, buf, true);
}

// Page boundaries are content-dependent in author order (headings consume band
// height), so they cannot be computed from an index. They are therefore
// remembered as the reader moves forward, which makes going back exact rather
// than an estimate that would drift on every turn.
void LibraryListActivity::nextPage() {
  const int count = rowCount();
  auto& n = activeNav();
  const int next = n.top + n.visibleRows;
  if (next >= count) return;
  if (pageStarts.empty()) pageStarts.push_back(0);
  pageStarts.push_back(static_cast<uint16_t>(next));
  n.top = next;
  n.selected = next + 1;
  requestUpdate();
}

void LibraryListActivity::previousPage(const bool selectLast) {
  auto& n = activeNav();
  if (n.top <= 0) return;
  if (pageStarts.size() > 1) {
    pageStarts.pop_back();
    n.top = pageStarts.back();
  } else {
    // No recorded history — the reader jumped here by some other route. Fall back
    // to a screenful back; it may not land on a boundary this pass, but the next
    // render re-measures and nothing is lost.
    n.top = std::max(0, n.top - n.visibleRows);
    pageStarts.assign(1, static_cast<uint16_t>(n.top));
  }
  // selectLast is only known to be right after the build that measures this
  // page, so aim past the end and let buildRows clamp it.
  n.selected = (selectLast ? n.top + n.visibleRows - 1 : n.top) + 1;
  requestUpdate();
}
