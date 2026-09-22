#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "TaskListModel.h"

namespace {

TaskRecord make(const char* id, const char* title, uint8_t priority, bool done = false) {
  TaskRecord r{};
  std::snprintf(r.id, sizeof(r.id), "%s", id);
  std::snprintf(r.title, sizeof(r.title), "%s", title);
  r.priority = priority;
  r.done = done;
  return r;
}

}  // namespace

// --- buildTaskOrder ---------------------------------------------------------
// Reuses taskOrderBefore (Task 1); these tests pin the ADDITIONAL rule this
// file adds on top of it (tickedHere), plus a couple of sanity checks that
// the canonical comparator is genuinely being called and not re-derived.

TEST(BuildTaskOrder, OpenFirstByPriorityThenId) {
  std::vector<TaskRecord> records = {
      make("w3", "basse", 2),
      make("w1", "haute", 0),
      make("w2", "normale", 1),
  };
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, {}, order, openCount);

  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(openCount, 3);
  EXPECT_EQ(order[0], 1);  // w1, haute
  EXPECT_EQ(order[1], 2);  // w2, normale
  EXPECT_EQ(order[2], 0);  // w3, basse
}

TEST(BuildTaskOrder, DoneTasksSinkAfterOpenOnesAndOpenCountExcludesThem) {
  std::vector<TaskRecord> records = {
      make("w1", "faite haute", 0, true),
      make("w2", "ouverte basse", 2),
  };
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, {}, order, openCount);

  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(openCount, 1);
  EXPECT_EQ(order[0], 1);  // w2 ouverte, malgre sa priorite plus basse
  EXPECT_EQ(order[1], 0);  // w1 faite, en fin de liste
}

TEST(BuildTaskOrder, TickedHereStaysInPlaceInsteadOfSinking) {
  // w1 est coche PENDANT cette visite : il doit rester tri comme une tache
  // ouverte de priorite haute, pas tomber apres w2.
  std::vector<TaskRecord> records = {
      make("w1", "cochee a l'instant", 0, true),
      make("w2", "ouverte normale", 1),
  };
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, {"w1"}, order, openCount);

  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(openCount, 2);  // les deux comptent comme ouvertes pour l'affichage
  EXPECT_EQ(order[0], 0);   // w1 garde sa place de priorite haute
  EXPECT_EQ(order[1], 1);
}

TEST(BuildTaskOrder, UntickingSomethingTickedThisVisitLetsItSinkAgain) {
  // Cochee puis decochee dans la meme visite : plus dans tickedHere, elle
  // redevient une tache faite ordinaire (le cas que le rapport appelle
  // "cocher-decocher").
  std::vector<TaskRecord> records = {
      make("w1", "faite avant l'ouverture de l'ecran", 0, true),
      make("w2", "ouverte normale", 1),
  };
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, /*tickedHere=*/{}, order, openCount);
  EXPECT_EQ(openCount, 1);
  EXPECT_EQ(order[0], 1);  // w2 ouverte d'abord
  EXPECT_EQ(order[1], 0);  // w1 faite, en fin de liste : jamais dans tickedHere
}

TEST(BuildTaskOrder, EqualPriorityFallsBackToId) {
  std::vector<TaskRecord> records = {make("w2", "b", 1), make("w1", "a", 1)};
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, {}, order, openCount);
  EXPECT_EQ(order[0], 1);  // w1 < w2
  EXPECT_EQ(order[1], 0);
}

// --- buildTaskListRows -------------------------------------------------------

TEST(BuildTaskListRows, HidesDoneSectionByDefault) {
  std::vector<int> order = {0, 1, 2};
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, /*openCount=*/2, /*showDone=*/false, rows);

  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[0].recordIndex, 0);
  EXPECT_EQ(rows[1].recordIndex, 1);
}

TEST(BuildTaskListRows, ShowDoneInsertsOneHeaderBeforeTheDoneTail) {
  std::vector<int> order = {0, 1, 2, 3};
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, /*openCount=*/2, /*showDone=*/true, rows);

  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[1].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[2].kind, TaskRowKind::DoneHeader);
  EXPECT_EQ(rows[3].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[3].recordIndex, 2);
  EXPECT_EQ(rows[4].recordIndex, 3);
}

TEST(BuildTaskListRows, NoHeaderWhenNothingIsDone) {
  std::vector<int> order = {0, 1};
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, /*openCount=*/2, /*showDone=*/true, rows);
  ASSERT_EQ(rows.size(), 2u);
  for (const auto& row : rows) EXPECT_EQ(row.kind, TaskRowKind::Task);
}

TEST(BuildTaskListRows, TogglingDoneOffAfterUntickingTheLastCompletedTaskDropsTheHeader) {
  // Le cas que le brief de correction nomme explicitement : la bascule est
  // ouverte, la derniere tache "terminees" est decochee -> plus rien apres
  // openCount -> l'en-tete doit disparaitre au prochain rebuild, pas rester
  // orpheline au-dessus d'une section vide.
  std::vector<int> order = {0, 1};
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, /*openCount=*/2, /*showDone=*/true, rows);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[1].kind, TaskRowKind::Task);
}

// --- taskListStepSelection / taskListNormalizeSelection ----------------------

TEST(TaskListStepSelection, SkipsOverTheHeaderRow) {
  std::vector<TaskListRow> rows = {
      {TaskRowKind::Task, 0},
      {TaskRowKind::DoneHeader, -1},
      {TaskRowKind::Task, 1},
  };
  EXPECT_EQ(taskListStepSelection(rows, 0, +1), 2);  // saute l'en-tete
  EXPECT_EQ(taskListStepSelection(rows, 2, -1), 0);  // et dans l'autre sens
}

TEST(TaskListStepSelection, WrapsAround) {
  std::vector<TaskListRow> rows = {{TaskRowKind::Task, 0}, {TaskRowKind::Task, 1}};
  EXPECT_EQ(taskListStepSelection(rows, 1, +1), 0);
  EXPECT_EQ(taskListStepSelection(rows, 0, -1), 1);
}

TEST(TaskListStepSelection, EmptyRowsReturnsNegativeOne) {
  std::vector<TaskListRow> rows;
  EXPECT_EQ(taskListStepSelection(rows, 0, +1), -1);
}

TEST(TaskListStepSelection, SteppingOnTheLastOpenTaskTogglesForwardIntoTheDoneHeader) {
  // Cocher la derniere tache ouverte pendant que "terminees" est deplie ne
  // doit pas planter la selection : Bas depuis elle doit sauter l'en-tete et
  // atterrir sur la premiere tache terminee, pas rester bloque dessus.
  std::vector<TaskListRow> rows = {
      {TaskRowKind::Task, 0},
      {TaskRowKind::DoneHeader, -1},
      {TaskRowKind::Task, 1},
      {TaskRowKind::Task, 2},
  };
  EXPECT_EQ(taskListStepSelection(rows, 0, +1), 2);
}

TEST(TaskListNormalizeSelection, ClampsOutOfRangeIndex) {
  std::vector<TaskListRow> rows = {{TaskRowKind::Task, 0}, {TaskRowKind::Task, 1}};
  EXPECT_EQ(taskListNormalizeSelection(rows, 99), 1);
  EXPECT_EQ(taskListNormalizeSelection(rows, -5), 0);
}

TEST(TaskListNormalizeSelection, StepsPastAHeaderLandedOnAfterRebuild) {
  std::vector<TaskListRow> rows = {
      {TaskRowKind::Task, 0},
      {TaskRowKind::DoneHeader, -1},
      {TaskRowKind::Task, 1},
  };
  EXPECT_EQ(taskListNormalizeSelection(rows, 1), 2);
}

TEST(TaskListNormalizeSelection, EmptyRowsReturnsNegativeOne) {
  std::vector<TaskListRow> rows;
  EXPECT_EQ(taskListNormalizeSelection(rows, 0), -1);
}

// --- taskListPageJump ---------------------------------------------------
// TaskListActivity itself is not host-compiled (it needs GfxRenderer,
// MappedInputManager, TaskStore), so the composition that matters --
// "a page jump landing on the header gets corrected" -- has to live here to
// be reachable by a host test at all. Verified by mutation on a scratch copy
// outside the git tree: removing the taskListNormalizeSelection() call from
// taskListPageJump() (returning `jumped` directly) makes both tests below
// fail; restoring it makes them pass again.

TEST(TaskListPageJump, ForwardJumpLandingExactlyOnTheDoneHeaderIsCorrectedToATaskRow) {
  // ButtonNavigator::nextPageIndex(current=0, count=7, pageRows=3) rends
  // exactement 3 -- l'index de l'en-tete -- puisque nextPageIndex ne sait
  // rien des lignes non selectionnables.
  std::vector<TaskListRow> rows = {
      {TaskRowKind::Task, 0}, {TaskRowKind::Task, 1}, {TaskRowKind::Task, 2}, {TaskRowKind::DoneHeader, -1},
      {TaskRowKind::Task, 3}, {TaskRowKind::Task, 4}, {TaskRowKind::Task, 5},
  };
  EXPECT_EQ(taskListPageJump(rows, /*selected=*/0, /*pageRows=*/3, /*direction=*/+1), 4);
}

TEST(TaskListPageJump, BackwardJumpLandingExactlyOnTheDoneHeaderIsCorrectedToATaskRow) {
  // ButtonNavigator::previousPageIndex(current=6, count=7, pageRows=3) rend
  // aussi 3 : le saut arriere ignore la ligne d'en-tete tout autant que le
  // saut avant.
  std::vector<TaskListRow> rows = {
      {TaskRowKind::Task, 0}, {TaskRowKind::Task, 1}, {TaskRowKind::Task, 2}, {TaskRowKind::DoneHeader, -1},
      {TaskRowKind::Task, 3}, {TaskRowKind::Task, 4}, {TaskRowKind::Task, 5},
  };
  EXPECT_EQ(taskListPageJump(rows, /*selected=*/6, /*pageRows=*/3, /*direction=*/-1), 4);
}

TEST(TaskListPageJump, EmptyRowsReturnsNegativeOne) {
  std::vector<TaskListRow> rows;
  EXPECT_EQ(taskListPageJump(rows, 0, 3, +1), -1);
}
