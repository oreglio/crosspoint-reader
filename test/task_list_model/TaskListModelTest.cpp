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
// Reuses taskOrderBefore (Task 1); these are sanity checks that the canonical
// comparator is genuinely being called and not re-derived.

TEST(BuildTaskOrder, OpenFirstByPriorityThenId) {
  std::vector<TaskRecord> records = {
      make("w3", "basse", 2),
      make("w1", "haute", 0),
      make("w2", "normale", 1),
  };
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, order, openCount);

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
  buildTaskOrder(records, order, openCount);

  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(openCount, 1);
  EXPECT_EQ(order[0], 1);  // w2 ouverte, malgre sa priorite plus basse
  EXPECT_EQ(order[1], 0);  // w1 faite, en fin de liste
}

TEST(BuildTaskOrder, EqualPriorityFallsBackToId) {
  std::vector<TaskRecord> records = {make("w2", "b", 1), make("w1", "a", 1)};
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, order, openCount);
  EXPECT_EQ(order[0], 1);  // w1 < w2
  EXPECT_EQ(order[1], 0);
}

// --- buildTaskListRows -------------------------------------------------------
// Vue exclusive Open / Done : la ligne d'ajout n'existe que quand la vue
// ouverte n'a rien a montrer (index vide ou tout fait) ou que l'index est vide.

TEST(BuildTaskListRows, OpenViewListsOnlyOpenTasks) {
  std::vector<int> order = {2, 0, 1};
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, /*openCount=*/2, TaskListView::Open, rows);

  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[0].recordIndex, 2);
  EXPECT_EQ(rows[1].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[1].recordIndex, 0);
}

TEST(BuildTaskListRows, DoneViewListsOnlyDoneTasksInOrder) {
  std::vector<int> order = {2, 0, 1};
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, /*openCount=*/1, TaskListView::Done, rows);

  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[0].recordIndex, 0);
  EXPECT_EQ(rows[1].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[1].recordIndex, 1);
}

TEST(BuildTaskListRows, EmptyIndexIsSingleAddRowInBothViews) {
  std::vector<int> order;
  std::vector<TaskListRow> rows;
  for (const TaskListView view : {TaskListView::Open, TaskListView::Done}) {
    buildTaskListRows(order, /*openCount=*/0, view, rows);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].kind, TaskRowKind::AddTask);
  }
}

TEST(BuildTaskListRows, AllDoneOpenViewShowsAddRow) {
  std::vector<int> order = {0, 1};
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, /*openCount=*/0, TaskListView::Open, rows);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::AddTask);
}

TEST(BuildTaskListRows, DoneViewWithNoDoneTasksIsEmpty) {
  std::vector<int> order = {0};
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, /*openCount=*/1, TaskListView::Done, rows);
  EXPECT_TRUE(rows.empty());
}

TEST(BuildTaskListRows, RowZeroIsTheDefaultSelectionInOpenView) {
  // UiListActivity::onEnter() remet la selection a 0 : premiere tache ouverte,
  // ou la ligne d'ajout quand il n'y en a aucune.
  std::vector<TaskListRow> rows;

  buildTaskListRows(/*order=*/{4, 2, 7}, /*openCount=*/1, TaskListView::Open, rows);
  ASSERT_FALSE(rows.empty());
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[0].recordIndex, 4);

  buildTaskListRows(/*order=*/{}, /*openCount=*/0, TaskListView::Open, rows);
  ASSERT_FALSE(rows.empty());
  EXPECT_EQ(rows[0].kind, TaskRowKind::AddTask);
}

// --- taskListClampSelection --------------------------------------------------
// Every row is selectable now, so the only job left after a rebuild is to keep
// the index in bounds. The old normalizer also stepped off the header; these
// tests pin that it no longer steps off anything.

TEST(TaskListClampSelection, ClampsOutOfRangeIndex) {
  std::vector<TaskListRow> rows = {{TaskRowKind::Task, 0}, {TaskRowKind::Task, 1}};
  EXPECT_EQ(taskListClampSelection(rows, 99), 1);
  EXPECT_EQ(taskListClampSelection(rows, -5), 0);
  // La frontiere qui compte vraiment : une liste qui raccourcit d'une ligne
  // sous une selection posee sur sa derniere ligne laisse selected == size().
  EXPECT_EQ(taskListClampSelection(rows, 2), 1);

  std::vector<TaskListRow> addOnly = {{TaskRowKind::AddTask, -1}};
  EXPECT_EQ(taskListClampSelection(addOnly, 1), 0);
}

TEST(TaskListClampSelection, AStaleIndexPastTheEndLandsOnTheLastRow) {
  // Garde de bornes pure : un indice perime au-dela de la fin atterrit sur la
  // derniere ligne telle qu'elle est, sans chercher une tache.
  std::vector<TaskListRow> rows = {{TaskRowKind::Task, 0}, {TaskRowKind::Task, 1}};
  EXPECT_EQ(taskListClampSelection(rows, 4), 1);

  std::vector<TaskListRow> addOnly = {{TaskRowKind::AddTask, -1}};
  EXPECT_EQ(taskListClampSelection(addOnly, 3), 0);
}

TEST(TaskListClampSelection, EmptyRowsReturnsNegativeOne) {
  std::vector<TaskListRow> rows;
  EXPECT_EQ(taskListClampSelection(rows, 0), -1);
}

// --- cibles d'une action en lot ----------------------------------------------
// Toujours du plus grand indice au plus petit : effacer dans cet ordre ne
// decale aucun indice qui reste a traiter.

TEST(TaskSelectionTargets, SelectedIndicesComeOutDescending) {
  TaskSelection selection;
  selection.set(0);
  selection.set(4);
  selection.set(2);
  uint8_t out[MAX_TASKS];
  ASSERT_EQ(taskSelectionTargets(selection, 5, out), 3u);
  EXPECT_EQ(out[0], 4);
  EXPECT_EQ(out[1], 2);
  EXPECT_EQ(out[2], 0);
}

TEST(TaskSelectionTargets, BitsPastTheRecordCountAreIgnored) {
  // Une sélection plus longue que l'index (tâche disparue entre-temps) ne
  // doit jamais désigner un enregistrement qui n'existe pas.
  TaskSelection selection;
  selection.set(1);
  selection.set(7);
  uint8_t out[MAX_TASKS];
  ASSERT_EQ(taskSelectionTargets(selection, 3, out), 1u);
  EXPECT_EQ(out[0], 1);
}

TEST(TaskSelectionTargets, EmptySelectionGivesNothing) {
  uint8_t out[MAX_TASKS];
  EXPECT_EQ(taskSelectionTargets(TaskSelection{}, 10, out), 0u);
}

TEST(TaskDoneTargets, OnlyFinishedTasksDescending) {
  std::vector<TaskRecord> records = {
      make("w1", "a", 1, true),
      make("w2", "b", 1, false),
      make("w3", "c", 0, true),
      make("w4", "d", 2, false),
  };
  uint8_t out[MAX_TASKS];
  ASSERT_EQ(taskDoneTargets(records, out), 2u);
  EXPECT_EQ(out[0], 2);
  EXPECT_EQ(out[1], 0);
}

TEST(TaskDoneTargets, NothingDoneGivesNothing) {
  std::vector<TaskRecord> records = {make("w1", "a", 1), make("w2", "b", 1)};
  uint8_t out[MAX_TASKS];
  EXPECT_EQ(taskDoneTargets(records, out), 0u);
}
