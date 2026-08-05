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
class LibraryListActivity final : public Activity {
 public:
  explicit LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;

  // Text for one visible row, owned for the duration of a render.
  struct RowText {
    std::string title;
    std::string author;
  };

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);

  bool openIndex();
  // Walk the card and write a fresh index. Blocking, with a popup: at ~70 books
  // it is well under a second, and it only runs when the index is missing or the
  // user asks.
  bool rebuildIndex();
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
  bool uiReady = false;
  bool indexReady = false;
  // Set when the walk finished but the sort did not, so the screen can say the
  // order is discovery order rather than silently showing a wrong one.
  bool degraded = false;

  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
};
