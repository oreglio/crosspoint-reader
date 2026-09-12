#pragma once

#include <LibraryFavoritesFile.h>
#include <LibraryIndexFile.h>

#include <memory>
#include <string>
#include <string_view>
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
// strip, 1..N = the books), which is what brings touch: rows, tabs and the
// header search action all register FreeInkUI hit rects. Titles
// wrap over up to three lines with per-item row heights, measured by the widget —
// a short title costs a short row, as the pre-conversion renderer did.
//
// Only the visible window of rows is materialized per render (title/author
// strings and ListItems), so nothing proportional to the library is held: the
// index streams from SD and the screen keeps at most a page of strings.
inline constexpr int LIBRARY_SIDE_PADDING = 12;

// Upstream's index does not store a format field, so the shelf derives it from
// the file name — the only place it is used is the Details page's size line.
enum class ShelfFormat : uint8_t { Epub, Txt, Md, Xtc, Other };
ShelfFormat shelfFormatForName(std::string_view name);

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
  freeink::ui::BitmapRef tabIcon(int index) const override;
  uint16_t tabInputMask() const override;
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
  static constexpr freeink::ui::ActionId ACTION_SEARCH = ACTION_TAB_USER;

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
  // The selected BOOK, which is not the selected ROW while groups are folded.
  int selectedBookEntry() const;
  bool tabsFocused() const { return ringPos() == 0; }
  bool searchShortcutActive() const;
  // Row + viewport reset after a data change; ring 0 (strip focus) survives,
  // any row selection collapses to the first row.
  void resetListPosition();
  // Rows surviving the current query, as positions in the active sort order.
  // Empty query means no filtering and this stays untouched, so the ordinary
  // shelf pays nothing for the feature.
  std::string query;
  std::vector<uint16_t> filtered;
  void openSearch();
  void buildSearchAction(UiScreen& screen);
  // Details is a mode of this activity, not a separate one: a full-screen page
  // for the selected row, render + Back, no lifecycle of its own.
  bool detailsView = false;
  void buildDetails(UiScreen& screen);
  static void searchActionTrampoline(const freeink::ui::ActionEvent& event, void* user);

  // --- collapsed groups ------------------------------------------------------
  //
  // The jump, adopted from upstream's screen (crosspoint/feat/library-view@
  // ad949bdd). The list folds onto its own section headings: the authors in
  // author order, the initials in title order. Pick one and the list unfolds
  // there.
  //
  // It replaces the A-Z grid, which the reader had to translate — "which letter
  // does the person I want start with, and is that their forename or their
  // surname?" — into a question the shelf can answer by showing the names
  // themselves. That grid's "By first name / By last name" toggle existed only
  // because a letter cannot say which word it refers to; a name can.
  //
  // Not a separate activity: a mode of this one, like details, sharing the
  // render and input pass. listCount() reports the group count while it is up,
  // so the base's ring, viewport and our paging all operate on the folded list
  // with no further changes.
  bool groupsCollapsed = false;
  // Entry index where each group starts, in the active sort order. uint16_t
  // because the index caps at 65535 books; allocated on demand and kept for
  // reuse, never per frame.
  std::unique_ptr<uint16_t[]> groupStarts;
  uint16_t groupCapacity = 0;
  uint16_t groupCount = 0;
  // False where no grouping exists to fold: the date orders, a degraded shelf
  // and an empty one. The ★ view IS foldable — it carries its own sort, and a
  // long favorites list wants the jump as much as the full shelf does.
  bool groupable() const;
  bool buildGroupStarts();
  int groupForBook(int bookEntry) const;
  // The folded codepoint an entry files under in title order.
  uint32_t groupInitialFor(int entry);
  // The row the fold started from, so Back returns the reader to their place
  // rather than to whichever heading they stopped scrolling on.
  int preCollapseEntry = 0;
  bool collapseGroups(int bookEntry);
  void expandToGroup(int groupEntry);
  void restoreExpandedList();
  // The heading one group shows: the author's name as "Surname, Forename", or
  // the initial. Writes into `out` rather than returning, so the visible window
  // reuses its own storage.
  void formatGroupHeading(int bookEntry, std::string& out);
  void applyFilter();
  int rowCount() const;
  int rowFor(int entry) const;

  // The list itself. Materializes ListItems and their strings for the visible
  // window only. FreeInkUI owns row and section-heading geometry, and reports
  // the measured page size back to navigation.
  void buildRows(UiScreen& screen);
  void drawPositionReadout();
  void nextPage();
  void previousPage(bool selectLast = false);

  library::LibraryIndexFile index;
  library::LibraryFavoritesFile favorites;
  // The ★ view flag lives in a file-static (sFavoritesView): with the star
  // leading the strip, leaving the shelf in the ★ view and coming back to
  // Recent read as a bug on the device.
  // One popup, two menus that can never coexist: the row's book menu and the
  // ★ tab's sort menu.
  OptionPopup popup;

  // Going up from the first row of a page lands on the final row measured on
  // the previous page. Resolved after layout, before the framebuffer is shown.
  bool selectLastOnNextBuild = false;
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
