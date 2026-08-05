#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <LibraryIndexFile.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

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
inline constexpr int LIBRARY_TITLE_LINES = 2;
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
  // Text for one visible row, owned for the duration of a render.
  struct RowText {
    std::string title;
    std::string author;
  };


  bool openIndex();
  // Walk the card and write a fresh index. Blocking, with a popup: at ~70 books
  // it is well under a second, and it only runs when the index is missing or the
  // user asks.
  bool rebuildIndex();
  void measureRows();
  int rowHeightFor(int titleLines, bool hasAuthor) const;
  bool rowTextFor(int entry, std::string& title, std::string& author);
  void drawRows();
  void drawPositionReadout();
  void openSelectedBook();
  void cycleSortOrder();
  const char* sortOrderLabel() const;

  library::LibraryIndexFile index;
  library::SortOrder sortOrder = library::SortOrder::DateDesc;

  std::vector<RowText> rowText;
  std::vector<freeink::ui::ListItem> uiItems;

  int selectedIndex = 0;
  int topIndex = 0;
  int visibleRows = 1;
  // Row geometry captured while building the screen, so the separators drawn
  // afterwards land exactly on the widget's own row boundaries.
  int listTop = 0;
  int listHeight = 0;
  int titleLineH = 0;
  int authorLineH = 0;
  bool indexReady = false;
  // Set when the walk finished but the sort did not, so the screen can say the
  // order is discovery order rather than silently showing a wrong one.
  bool degraded = false;

  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
};
