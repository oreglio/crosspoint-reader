#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <LibraryFavoritesFile.h>
#include <LibraryIndexFile.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// One flat list of every book on the card, newest first, with the title on line
// 1 and the author on line 2 at a fixed column.
//
// The two-slot row is the whole point rather than a styling choice: the problem
// being solved is "I cannot find my books because I do not know the authors",
// and that is answered by a column the eye can sweep, not by a tidier filename.
//
// Rows are built only for the visible window, so nothing proportional to the
// library is held: the index streams from SD and the screen keeps at most a
// page of strings.
// Row layout. Rows are drawn by hand rather than through fui::list because that
// widget caps the label at one line whenever a subtitle is present
// (components/lists/list.h:445, :449), which makes a wrapped title and an
// aligned author column mutually exclusive. This screen needs both.
inline constexpr int LIBRARY_TITLE_LINES = 3;
// Height of the sort strip. Sized to the small font plus the underline that marks
// the active tab.
inline constexpr int LIBRARY_TABS_HEIGHT = 24;
inline constexpr int LIBRARY_ICON_SIZE = 24;
inline constexpr int LIBRARY_ICON_GAP = 10;
inline constexpr int LIBRARY_SIDE_PADDING = 12;
inline constexpr int LIBRARY_ROW_PADDING = 10;

class LibraryListActivity final : public Activity {
 public:
  explicit LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  bool openIndex();
  // Walk the card and write a fresh index. Blocking, with a popup: at ~70 books
  // it is well under a second, and it only runs when the index is missing or the
  // user asks.
  bool rebuildIndex();
  void measureRows();
  int rowHeightFor(int titleLines, bool hasAuthor) const;
  bool rowTextFor(int entry, std::string& title, std::string& author, bool* isFavorite = nullptr);
  void drawRows();
  void drawPositionReadout();
  void nextPage();
  void previousPage(bool selectLast = false);
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
  void toggleFavoriteAt(int entry);
  // The favorites identity of one visible row: {fnv1a32(basename), fileSize},
  // the same pair the index rebuild reconciles by.
  bool rowKeyFor(int entry, library::FavoriteKey& key);
  void drawSortTabs(int top);
  int tabsTop = 0;
  // Left/Right turn pages, so the strip cannot own that axis outright. It takes
  // it only while focused, which the reader reaches by pressing Up from the first
  // book — the one press that had nothing to do before.
  bool tabsFocused = false;
  // Cursor within the strip. Separate from sortOrder because the strip carries
  // one entry that is not a sort mode: Search.
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
  void drawDetails();
  // The A-Z grid is a mode of this activity, not a separate one: it borrows the
  // same render and input pass, so it needs no lifecycle of its own.
  bool letterGrid = false;
  int letterCursor = 0;
  void drawLetterGrid();
  void jumpToLetter(char letter);
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

  library::LibraryIndexFile index;
  library::SortOrder sortOrder = library::SortOrder::DateDesc;
  library::LibraryFavoritesFile favorites;
  // The ★ view flag lives in a file-static (sFavoritesView), like the title
  // direction: with the star leading the strip, leaving the shelf in the ★
  // view and coming back to Recent read as a bug on the device.
  // One popup, two menus that can never coexist: the row's book menu and the
  // ★ tab's sort menu.
  OptionPopup popup;

  int selectedIndex = 0;
  int topIndex = 0;
  int visibleRows = 1;
  // Row geometry captured while building the screen, so the separators drawn
  // afterwards land exactly on the widget's own row boundaries.
  int listTop = 0;
  int listHeight = 0;
  // First entry of each page visited on the way here, so going back lands on the
  // same boundaries the reader came through.
  std::vector<uint16_t> pageStarts;
  int titleLineH = 0;
  int authorLineH = 0;
  bool indexReady = false;
  // Set when the walk finished but the sort did not, so the screen can say the
  // order is discovery order rather than silently showing a wrong one.
  bool degraded = false;

  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
};
