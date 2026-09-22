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

// --- taskListVisibleRows -----------------------------------------------------

TEST(TaskListVisibleRows, CountsWholeRowsThatFit) {
  std::vector<int> heights = {30, 30, 30, 30};
  EXPECT_EQ(taskListVisibleRows(heights, 0, 100), 3);  // 90 tient, 120 deborde
  EXPECT_EQ(taskListVisibleRows(heights, 0, 30), 1);
  EXPECT_EQ(taskListVisibleRows(heights, 1, 100), 3);  // reste 3 lignes a partir de l'indice 1
}

TEST(TaskListVisibleRows, AlwaysAtLeastOneRowEvenIfItOverflows) {
  std::vector<int> heights = {500};
  EXPECT_EQ(taskListVisibleRows(heights, 0, 100), 1);
}

TEST(TaskListVisibleRows, OutOfRangeScrollTopIsZeroRows) {
  std::vector<int> heights = {30, 30};
  EXPECT_EQ(taskListVisibleRows(heights, 5, 100), 0);
  EXPECT_EQ(taskListVisibleRows(heights, -1, 100), 0);
}

// --- taskListClampScrollTop --------------------------------------------------

TEST(TaskListClampScrollTop, SelectionAboveViewportPullsScrollUpToIt) {
  std::vector<int> heights = {30, 30, 30, 30, 30};
  EXPECT_EQ(taskListClampScrollTop(heights, /*selected=*/1, /*scrollTop=*/3, 90), 1);
}

TEST(TaskListClampScrollTop, SelectionBelowViewportAdvancesScrollByOneRowAtATime) {
  std::vector<int> heights = {30, 30, 30, 30, 30};
  // 3 lignes tiennent dans 90px ; selectionner la 5e (indice 4) doit avancer
  // scrollTop jusqu'a ce qu'elle rentre, pas le recentrer d'un coup.
  EXPECT_EQ(taskListClampScrollTop(heights, /*selected=*/4, /*scrollTop=*/0, 90), 2);
}

TEST(TaskListClampScrollTop, SelectionAlreadyVisibleLeavesScrollUnchanged) {
  std::vector<int> heights = {30, 30, 30, 30};
  EXPECT_EQ(taskListClampScrollTop(heights, /*selected=*/1, /*scrollTop=*/0, 90), 0);
}

TEST(TaskListClampScrollTop, VariableRowHeightsAreRespected) {
  // Une ligne sur deux lignes de texte pese deux fois plus.
  std::vector<int> heights = {30, 60, 30, 30};
  EXPECT_EQ(taskListClampScrollTop(heights, /*selected=*/2, /*scrollTop=*/0, 90), 1);
}

TEST(TaskListClampScrollTop, EmptyHeightsReturnsZero) {
  std::vector<int> heights;
  EXPECT_EQ(taskListClampScrollTop(heights, 0, 0, 100), 0);
}
