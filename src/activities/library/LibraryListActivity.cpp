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
// The ★ view survives the shelf the same way: with the star leading the strip,
// leaving in the favorites view and returning to Recent read as a bug on the
// device. A boot still starts on Recent — the full shelf is the honest default.
bool sFavoritesView = false;
// And the sort itself, for the same reason: choosing A-Z, leaving and coming
// back to Recent read as "the filters do not save". One session-long shelf
// state, statics only, zero settings.
library::SortOrder sSortOrder = library::SortOrder::DateDesc;
// The ★ view carries its own remembered order: sorting favorites by author and
// then browsing Recent must not cost the favorites their order.
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

// Search lives in the header, so every tab is a complete view/order state.
constexpr int kFavTab = 0;
constexpr int kRecentTab = 1;
constexpr int kTitleAscTab = 2;
constexpr int kTitleDescTab = 3;
constexpr int kAuthorTab = 4;
constexpr int kTabSlots = kAuthorTab + 1;

int sortTabIndex(const library::SortOrder order) {
  switch (order) {
    case library::SortOrder::TitleAsc:
      return kTitleAscTab;
    case library::SortOrder::TitleDesc:
      return kTitleDescTab;
    case library::SortOrder::AuthorAsc:
      return kAuthorTab;
    case library::SortOrder::DateDesc:
      return kRecentTab;
  }
  return kRecentTab;
}

library::SortOrder orderForTab(const int tab) {
  if (tab == kTitleAscTab) return library::SortOrder::TitleAsc;
  if (tab == kTitleDescTab) return library::SortOrder::TitleDesc;
  if (tab == kAuthorTab) return library::SortOrder::AuthorAsc;
  return library::SortOrder::DateDesc;
}

// The strip needs the mode alone. The header strings carry a "Library ·" prefix
// that reads as four copies of the word once they sit side by side.
const char* tabLabelFor(const int tab) {
  if (tab == kRecentTab) return tr(STR_LIBRARY_TAB_RECENT);
  if (tab == kTitleAscTab) return "A-Z";
  if (tab == kTitleDescTab) return "Z-A";
  if (tab == kAuthorTab) return tr(STR_LIBRARY_TAB_AUTHOR);
  return nullptr;
}

int LibraryListActivity::tabCount() const { return kTabSlots; }

int LibraryListActivity::activeTab() const { return sFavoritesView ? kFavTab : sortTabIndex(sSortOrder); }

const char* LibraryListActivity::tabLabel(const int index) const { return tabLabelFor(index); }

fui::BitmapRef LibraryListActivity::tabIcon(const int index) const {
  return index == kFavTab ? fui::bitmapFromIcon(icon_star_16_fui) : fui::BitmapRef{};
}

uint16_t LibraryListActivity::tabInputMask() const { return fui::InputTouch | fui::InputLongPress; }

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
  selectLastOnNextBuild = false;
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
    app.on(ACTION_SEARCH, &LibraryListActivity::searchActionTrampoline, this);
    app.on(ACTION_LETTER, &LibraryListActivity::letterActionTrampoline, this);
    app.on(ACTION_LETTER_MODE, &LibraryListActivity::letterModeActionTrampoline, this);
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
  state.titleDescending = sSortOrder == library::SortOrder::TitleDesc;
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
                           selectLastOnNextBuild = false;
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
      case 1:
        sFavSortOrder = library::SortOrder::TitleAsc;
        break;
      case 2:
        sFavSortOrder = library::SortOrder::TitleDesc;
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

void LibraryListActivity::onTabAction(const int index) {
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
  selectLastOnNextBuild = false;
  app.clearTapFlash();
  requestUpdate();
}

void LibraryListActivity::onTabLongPress(const int index) {
  if (index == kFavTab) openFavoritesSortMenu();
}

void LibraryListActivity::stepTab(const int direction) {
  const int next = (activeTab() + direction + kTabSlots) % kTabSlots;
  onTabAction(next);
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
  selectLastOnNextBuild = false;
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
      selectLastOnNextBuild = false;
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

void LibraryListActivity::toggleLetterGridMode() {
  if (currentOrder() != library::SortOrder::AuthorAsc) return;
  jumpByGivenName = !jumpByGivenName;
  computeLettersPresent();
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
  if (currentOrder() != library::SortOrder::AuthorAsc) return;
  const int active = self->jumpByGivenName ? 0 : 1;
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
                           auto& n = activeNav();
                           n.selected = !query.empty() && rowCount() == 0 && !degraded ? 0 : 1;
                           n.top = 0;
                           requestUpdate();
                         });
}

void LibraryListActivity::searchActionTrampoline(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<LibraryListActivity*>(user);
  self->app.clearTapFlash();
  self->openSearch();
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
        if (currentOrder() == library::SortOrder::AuthorAsc) {
          letterCursor = -1;
          requestUpdate();
        }
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
  // list.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    auto& n = activeNav();
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? n.pageRows() : -n.pageRows();
    if (n.scrollBy(delta, rowCount())) {
      selectLastOnNextBuild = false;
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
    if (mappedInput.getHeldTime() >= kHoldMs) {
      if (tabsFocused()) {
        if (activeTab() == kFavTab) openFavoritesSortMenu();
      } else {
        openBookMenu();
      }
      return true;
    }
    if (tabsFocused()) {
      if (currentOrder() != library::SortOrder::DateDesc) openLetterGrid();
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
      stepTab(1);
    } else {
      nextPage();
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) &&
      (searchShortcutActive() || tabsFocused() || count > 0)) {
    if (searchShortcutActive()) {
      openSearch();
    } else if (tabsFocused()) {
      stepTab(-1);
    } else if (mappedInput.getHeldTime() >= kHoldMs) {
      n.selected = 1;
      n.top = 0;
      selectLastOnNextBuild = false;
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
    } else if (n.selected - 1 < n.top + n.pageRows() - 1 && n.selected - 1 < count - 1) {
      n.selected++;
      requestUpdate();
    } else if (n.top + n.pageRows() < count) {
      nextPage();
    }
    return true;
  }

  return false;
}

bool LibraryListActivity::searchShortcutActive() const {
  if (detailsView || letterGrid || degraded) return false;
  if (ringPos() == 1) return true;
  return tabsFocused() && !query.empty() && rowCount() == 0;
}

// Touch routing for the modal grid: handleCustomInput consumes the whole pass
// while a mode is up, so the base's routeListTouch never runs — the app's hit
// rects (letters, mode line) are routed here instead.
void LibraryListActivity::routeModalTouch() {
  const auto route = UiAppHost::routeTouch(mappedInput, /*withLongPress=*/true);
  if (route.routed && app.invalidated()) requestUpdate();
}

void LibraryListActivity::buildSearchAction(UiScreen& screen) {
  if (!mappedInput.hasTouchHardware() || detailsView || letterGrid || degraded) return;

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  constexpr int16_t actionBandHeight = 60;
  fui::HeaderProps props;
  props.borderEdges = fui::EdgesNone;
  props.styles.explicitlySet = true;
  props.trailingIcon = fui::bitmapFromIcon(icon_search_32);
  props.trailingAction = ACTION_SEARCH;
  props.trailingStyles = fui::plainStyles();
  fui::header(
      screen.frame(),
      fui::Rect{static_cast<int16_t>(header.x), static_cast<int16_t>(header.y + header.height - actionBandHeight),
                static_cast<int16_t>(header.width), actionBandHeight},
      props);
}

void LibraryListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = rowCount();
  const bool grouped = currentOrder() == library::SortOrder::AuthorAsc;

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 3;
  props.headerUnderline = false;
  props.scrollIndicator = false;
  syncTabListViewport(screen, props, /*hasSubtitle=*/!grouped);

  // The fixed-height estimate is an upper bound: wrapping and inline headings
  // can only reduce the number of logical book rows that fit.
  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1);
  if (winTitles.size() < cap) winTitles.resize(cap);
  if (winAuthors.size() < cap) winAuthors.resize(cap);
  if (winHeaders.size() < cap) winHeaders.resize(cap);
  winItems.clear();
  if (winItems.capacity() < cap) winItems.reserve(cap);

  const fui::BitmapRef bookIcon = listIconFor(UIIcon::Book);
  const fui::BitmapRef starIcon = fui::bitmapFromIcon(icon_star_24_fui);

  int books = 0;
  int headers = 0;
  // Capture this after syncTabListViewport(), because its clamp must also move
  // the beginning of the materialized window.
  const int windowStart = static_cast<int>(props.topIndex);
  for (int entry = windowStart; entry < count && books < static_cast<int>(cap); entry++) {
    bool isFavorite = false;
    std::string& title = winTitles[static_cast<size_t>(books)];
    std::string& author = winAuthors[static_cast<size_t>(books)];
    rowTextFor(entry, title, author, &isFavorite);

    const bool startsGroup =
        grouped && !author.empty() && (books == 0 || author != winAuthors[static_cast<size_t>(books - 1)]);
    fui::ListItem item;
    if (startsGroup) {
      std::string& heading = winHeaders[static_cast<size_t>(headers++)];
      heading = author;
      const size_t lastSpace = heading.find_last_of(' ');
      if (lastSpace != std::string::npos && lastSpace + 1 < heading.size()) {
        heading = heading.substr(lastSpace + 1) + ", " + heading.substr(0, lastSpace);
      }
      item.sectionHeading = heading.c_str();
    }

    item.label = title.c_str();
    if (!grouped && !author.empty()) item.subtitle = author.c_str();
    item.icon = (isFavorite && !sFavoritesView) ? starIcon : bookIcon;
    item.actionValue = static_cast<int16_t>(entry);
    winItems.push_back(item);
    books++;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
  screen.list(props);
  if (selectLastOnNextBuild) {
    selectLastOnNextBuild = false;
    if (nav.drawnRows > 0) {
      nav.selected = nav.top + nav.drawnRows;
      nav.rebuildNeeded = true;
    }
  }
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

void LibraryListActivity::buildLetterGrid(UiScreen& screen) {
  const fui::Rect body = screen.body();
  auto& target = screen.target();
  const int cell = (body.width - 2 * LIBRARY_SIDE_PADDING) / kLetterCols;
  const int rows = (kLetterCount + kLetterCols - 1) / kLetterCols;
  const bool hasNameMode = currentOrder() == library::SortOrder::AuthorAsc;
  const int cellH = body.height / (rows + (hasNameMode ? 1 : 0));

  if (hasNameMode) {
    const char* labels[2] = {tr(STR_LIBRARY_JUMP_GIVEN), tr(STR_LIBRARY_JUMP_SURNAME)};
    const int active = jumpByGivenName ? 0 : 1;
    fui::TextStyle modeText = screen.theme().smallText;
    modeText.align = fui::TextAlign::Center;
    const int16_t modeH = target.lineHeight(modeText.font);
    constexpr int16_t kModeGap = 20;
    int16_t labelW[2];
    for (int i = 0; i < 2; i++) labelW[i] = target.measureText(modeText.font, labels[i], modeText).width;
    int16_t mx = static_cast<int16_t>(body.x + (body.width - (labelW[0] + labelW[1] + kModeGap)) / 2);
    const int16_t modeY = static_cast<int16_t>(body.y + 2);

    for (int i = 0; i < 2; i++) {
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
      screen.button(mode, fui::Rect{static_cast<int16_t>(mx - 5), static_cast<int16_t>(modeY - 2),
                                    static_cast<int16_t>(labelW[i] + 10), static_cast<int16_t>(modeH + 4)});
      if (on && letterCursor >= 0) {
        target.fill(fui::Rect{mx, static_cast<int16_t>(modeY + modeH + 1), labelW[i], 1},
                    fui::Paint::solid(fui::Color::Black));
      }
      mx = static_cast<int16_t>(mx + labelW[i] + kModeGap);
    }
  }

  const int16_t top = static_cast<int16_t>(body.y + (hasNameMode ? cellH / 2 : 3));
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
  buildSearchAction(screen);

  if (detailsView) {
    buildDetails(screen);
    return;
  }
  if (letterGrid) {
    buildLetterGrid(screen);
    return;
  }

  // The strip stays visible over an empty FILTERED view — it is the way out.
  // Only a card with no books at all drops it: there is nothing to sort.
  if (!degraded && index.bookCount() > 0) {
    buildTabBar(screen);
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
  for (int pass = 0; activeNav().consumeRebuildNeeded() && pass < 8; ++pass) {
    renderer.clearScreen();
    if (mappedInput.hasTouchHardware()) {
      TouchHeaderBackButton::draw(renderer, uiTarget, header, title, true);
    } else {
      GUI.drawHeader(renderer, header, title);
    }
    renderUi();
  }

  if (!detailsView) drawPositionReadout();
  // The bottom pair delivers Left/Right on this hardware, so labelling it
  // "Up/Down" describes the wrong axis — it pages the list, switches tabs and
  // steps letters, none of which is vertical. mapLabels takes previous/next
  // precisely so the caller can say what they do here. On the details page the
  // pair does nothing, and an empty label is how the hints bar says so.
  const char* prevLabel =
      detailsView ? "" : (letterGrid || tabsFocused() ? tr(STR_DIR_LEFT) : tr(STR_LIBRARY_PAGE_PREV));
  if (searchShortcutActive()) prevLabel = tr(STR_SEARCH);
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

void LibraryListActivity::nextPage() {
  const int count = rowCount();
  auto& nav = activeNav();
  const int next = nav.top + std::max(1, nav.pageRows());
  if (next >= count) return;
  selectLastOnNextBuild = false;
  nav.top = next;
  nav.selected = next + 1;
  requestUpdate();
}

void LibraryListActivity::previousPage(const bool selectLast) {
  auto& nav = activeNav();
  selectLastOnNextBuild = false;
  if (nav.top <= 0) return;
  nav.top = std::max(0, nav.top - std::max(1, nav.pageRows()));
  nav.selected = nav.top + 1;
  selectLastOnNextBuild = selectLast;
  requestUpdate();
}
