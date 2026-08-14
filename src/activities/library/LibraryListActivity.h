#pragma once

#include <LibraryFavoritesFile.h>
#include <LibraryIndexFile.h>

#include <string>
#include <vector>

#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"
#include "util/HoldOpenReleaseLock.h"

// One flat list of every book on the card, newest first, with the title on
// line 1 and the author under it. The author column is the whole point rather
// than a styling choice: the problem being solved is "I cannot find my books
// because I do not know the authors", and that is answered by a column the
// eye can sweep, not by a tidier filename.
//
// Rows render through fui::list on a UiTabListActivity ring (0 = the sort
// strip, 1..N = the books), which is what brings touch: rows, tabs, the A-Z
// grid and the search strip all register FreeInkUI hit rects. Titles are
// truncated to one line by the widget — accepted upstream ("more books in the
// screen, even if half the name is hidden"); per-item heights come back with
// the SDK-side follow-up.
//
// Only the visible window of rows is materialized per render (title/author
// strings and ListItems), so nothing proportional to the library is held: the
// index streams from SD and the screen keeps at most a page of strings.
inline constexpr int LIBRARY_SIDE_PADDING = 12;

class LibraryListActivity final : public UiTabListActivity {
 public:
  explicit LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 protected:
  // --- UiListActivity / UiTabListActivity contract ---------------------------
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  int tabCount() const override;
  int activeTab() const override;
  const char* tabLabel(int index) const override;
  void onTabAction(int index) override;
  void onTabLongPress(int index) override;
  void stepTab(int direction) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  // Every button is dispatched in handleButtons with the shelf's own paging
  // semantics; the base ring walk must not run behind it.
  void navigateButtons() override {}

 private:
  // The shelf's own actions, after the base's ACTION_ROW / ACTION_TAB.
  static constexpr freeink::ui::ActionId ACTION_LETTER = ACTION_TAB_USER;
  static constexpr freeink::ui::ActionId ACTION_LETTER_MODE = ACTION_TAB_USER + 1;

  // The readers open the Library from a HOLD of either button pair, and all
  // four of those buttons move the cursor or the sort strip here on release.
  // The Confirm and Back routes into this screen arm the MappedInputManager
  // suppressions instead; the pairs have none, so the lock lives here.
  HoldOpenReleaseLock openingGestureLock_;
  bool openIndex();
  // Walk the card and write a fresh index. Blocking, with a popup: at ~70 books
  // it is well under a second, and it only runs when the index is missing or the
  // user asks.
  bool rebuildIndex();
  bool rowTextFor(int entry, std::string& title, std::string& author, bool* isFavorite = nullptr);
  void openSelectedBook();
  // Swap TitleAsc/TitleDesc in place: the Titles tab keeps its slot and the
  // direction state lives for the whole power-on session (file-static).
  void flipTitleDirection();
  // Long-press on a row: the row's secondary actions, headed by the book's own
  // title so there is no doubt which row they land on.
  void openBookMenu();
  // Long-press on the focused ★ tab: pick the order the favorites read in
  // without leaving the view — the strip's sort tabs would exit it.
  void openFavoritesSortMenu();
  // The menu's destructive entry, confirmed first. Reuses the Recent Books
  // deletion flow, then cleans the two things only this screen knows about:
  // the favorites entry and the index, reconciled on the spot.
  void promptDeleteSelectedBook();
  void toggleFavoriteAt(int entry);
  // The favorites identity of one visible row: {fnv1a32(basename), fileSize},
  // the same pair the index rebuild reconciles by.
  bool rowKeyFor(int entry, library::FavoriteKey& key);
  // Put the cursor back on a remembered book, wherever the current view and
  // order have moved it. An identity search, so it survives everything.
  void restoreSelection(const library::FavoriteKey& sel);
  // The selection captured by openSelectedBook before it closes the index, so
  // onExit can still record which book the reader just left for.
  library::FavoriteKey exitSelection{};

  // Ring 0 is the strip; the selected BOOK is ring - 1.
  int selectedEntry() const;
  bool tabsFocused() const { return ringPos() == 0; }
  // Row + viewport reset after a data change; ring 0 (strip focus) survives,
  // any row selection collapses to the first row. Page history dies with the
  // old boundaries.
  void resetListPosition();
  // Cursor within the strip. Separate from the active view because the strip
  // carries one entry that is not a sort mode: Search.
  int tabCursor = 0;

  // Rows surviving the current query, as positions in the active sort order.
  // Empty query means no filtering and this stays untouched, so the ordinary
  // shelf pays nothing for the feature.
  std::string query;
  std::vector<uint16_t> filtered;
  void openSearch();
  // Details is a mode of this activity too, like the grid: a full-screen page
  // for the selected row, render + Back, no lifecycle of its own.
  bool detailsView = false;
  void buildDetails(UiScreen& screen);
  // The A-Z grid is a mode of this activity, not a separate one: it borrows the
  // same render and input pass, so it needs no lifecycle of its own.
  bool letterGrid = false;
  int letterCursor = 0;
  void buildLetterGrid(UiScreen& screen);
  void openLetterGrid();
  void jumpToLetter(char letter);
  void toggleLetterGridMode();
  // Touch routing while a modal mode (the grid) consumes the loop pass, so
  // its component hit rects still dispatch.
  void routeModalTouch();
  static void letterActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  static void letterModeActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  // One bit per letter, computed when the grid opens. Testing each letter against
  // the index while drawing would re-read every record 26 times per frame.
  uint32_t lettersPresent = 0;
  void computeLettersPresent();
  // Which word of a name the grid's letters refer to. No rule can tell "Lu
  // Xun" (surname first) from "Jane Austen" (surname last), so the reader
  // says which they mean instead of the code guessing.
  bool jumpByGivenName = false;
  char letterOf(const library::ClixRecord& record);
  void applyFilter();
  int rowCount() const;
  int rowFor(int entry) const;
  void cycleSortOrder(bool forward = true);
  const char* sortOrderLabel() const;

  // The sort strip: fui::tabBar with the shelf's own two-state treatment,
  // plus the state decorations no component carries (the Titles direction
  // triangle and the Search "filtered" dot), drawn on the band through the
  // same FreeInkUI target.
  void buildSortTabs(UiScreen& screen);
  int16_t sortStripHeight(UiScreen& screen) const;
  // The list itself. Materializes ListItems and their strings for the visible
  // window only, mirroring the widget's own layout math (heights, section
  // headers in author order) so the page the reader sees is exactly the page
  // navigation counts.
  void buildRows(UiScreen& screen);
  void drawPositionReadout();
  void nextPage();
  void previousPage(bool selectLast = false);

  library::LibraryIndexFile index;
  library::LibraryFavoritesFile favorites;
  // The ★ view flag lives in a file-static (sFavoritesView), like the title
  // direction: with the star leading the strip, leaving the shelf in the ★
  // view and coming back to Recent read as a bug on the device.
  // One popup, two menus that can never coexist: the row's book menu and the
  // ★ tab's sort menu.
  OptionPopup popup;

  // First entry of each page visited on the way here, so going back lands on
  // the same boundaries the reader came through. Needed because pages are not
  // uniform in author order: section headers consume band height, so a page's
  // size is only known once built.
  std::vector<uint16_t> pageStarts;
  bool indexReady = false;
  // Set when the walk finished but the sort did not, so the screen can say the
  // order is discovery order rather than silently showing a wrong one.
  bool degraded = false;

  // Visible-window row storage, reused across renders (buildRows). Bounded by
  // the densest page (~23 rows), never by the library. Headings get their own
  // storage: the surname-first inversion must NOT overwrite the author slot,
  // whose raw value the next row's group comparison reads.
  std::vector<freeink::ui::ListItem> winItems;
  std::vector<std::string> winTitles;
  std::vector<std::string> winAuthors;
  std::vector<std::string> winHeaders;
};
