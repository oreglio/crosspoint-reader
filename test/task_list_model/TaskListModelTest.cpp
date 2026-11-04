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

std::vector<TaskRecord> blank(size_t n) { return std::vector<TaskRecord>(n, TaskRecord{}); }

TaskGroup grp(const char* id, const char* name, uint16_t position) {
  TaskGroup g{};
  std::snprintf(g.id, sizeof(g.id), "%s", id);
  std::snprintf(g.name, sizeof(g.name), "%s", name);
  g.position = position;
  return g;
}

TaskRecord inGroup(const char* id, uint8_t priority, const char* group, bool done = false) {
  TaskRecord r = make(id, id, priority, done);
  std::snprintf(r.group, sizeof(r.group), "%s", group);
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

TEST(BuildTaskOrder, EqualPriorityFallsBackToId) {
  std::vector<TaskRecord> records = {make("w2", "b", 1), make("w1", "a", 1)};
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, {}, order, openCount);
  EXPECT_EQ(order[0], 1);  // w1 < w2
  EXPECT_EQ(order[1], 0);
}

// --- buildTaskListRows -------------------------------------------------------
// Vue exclusive Open / Done : la ligne d'ajout n'existe que quand la vue
// ouverte n'a rien a montrer (index vide ou tout fait) ou que l'index est vide.

TEST(BuildTaskListRows, OpenViewListsOnlyOpenTasks) {
  std::vector<int> order = {2, 0, 1};
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, /*openCount=*/2, TaskListView::Open, blank(8), {}, rows);

  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[0].recordIndex, 2);
  EXPECT_EQ(rows[1].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[1].recordIndex, 0);
}

TEST(BuildTaskListRows, DoneViewListsOnlyDoneTasksInOrder) {
  std::vector<int> order = {2, 0, 1};
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, /*openCount=*/1, TaskListView::Done, blank(8), {}, rows);

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
    buildTaskListRows(order, /*openCount=*/0, view, blank(8), {}, rows);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].kind, TaskRowKind::AddTask);
  }
}

TEST(BuildTaskListRows, AllDoneOpenViewShowsAddRow) {
  std::vector<int> order = {0, 1};
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, /*openCount=*/0, TaskListView::Open, blank(8), {}, rows);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::AddTask);
}

TEST(BuildTaskListRows, DoneViewWithNoDoneTasksIsEmpty) {
  std::vector<int> order = {0};
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, /*openCount=*/1, TaskListView::Done, blank(8), {}, rows);
  EXPECT_TRUE(rows.empty());
}

TEST(BuildTaskListRows, RowZeroIsTheDefaultSelectionInOpenView) {
  // UiListActivity::onEnter() remet la selection a 0 : premiere tache ouverte,
  // ou la ligne d'ajout quand il n'y en a aucune.
  std::vector<TaskListRow> rows;

  buildTaskListRows(/*order=*/{4, 2, 7}, /*openCount=*/1, TaskListView::Open, blank(8), {}, rows);
  ASSERT_FALSE(rows.empty());
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[0].recordIndex, 4);

  buildTaskListRows(/*order=*/{}, /*openCount=*/0, TaskListView::Open, blank(8), {}, rows);
  ASSERT_FALSE(rows.empty());
  EXPECT_EQ(rows[0].kind, TaskRowKind::AddTask);
}

// --- taskListClampSelection --------------------------------------------------
// Every row is selectable now, so the only job left after a rebuild is to keep
// the index in bounds. The old normalizer also stepped off the header; these
// tests pin that it no longer steps off anything.

TEST(TaskListClampSelection, ClampsOutOfRangeIndex) {
  std::vector<TaskListRow> rows = {{TaskRowKind::Task, -1, 0}, {TaskRowKind::Task, -1, 1}};
  EXPECT_EQ(taskListClampSelection(rows, 99), 1);
  EXPECT_EQ(taskListClampSelection(rows, -5), 0);
  // La frontiere qui compte vraiment : une liste qui raccourcit d'une ligne
  // sous une selection posee sur sa derniere ligne laisse selected == size().
  EXPECT_EQ(taskListClampSelection(rows, 2), 1);

  std::vector<TaskListRow> addOnly = {{TaskRowKind::AddTask, -1, -1}};
  EXPECT_EQ(taskListClampSelection(addOnly, 1), 0);
}

TEST(TaskListClampSelection, AStaleIndexPastTheEndLandsOnTheLastRow) {
  // Garde de bornes pure : un indice perime au-dela de la fin atterrit sur la
  // derniere ligne telle qu'elle est, sans chercher une tache.
  std::vector<TaskListRow> rows = {{TaskRowKind::Task, -1, 0}, {TaskRowKind::Task, -1, 1}};
  EXPECT_EQ(taskListClampSelection(rows, 4), 1);

  std::vector<TaskListRow> addOnly = {{TaskRowKind::AddTask, -1, -1}};
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

// --- groupes -----------------------------------------------------------------

TEST(BuildTaskOrderGroups, UngroupedThenGroupsByPositionThenPriority) {
  std::vector<TaskGroup> groups = {grp("ga", "A", 5), grp("gb", "B", 2)};
  std::vector<TaskRecord> records = {
      inGroup("t1", 0, "ga"),  // groupe A (position 5)
      inGroup("t2", 2, "gb"),  // groupe B basse
      inGroup("t3", 0, "gb"),  // groupe B haute
      inGroup("t4", 2, ""),    // sans groupe
  };
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, groups, order, openCount);
  ASSERT_EQ(openCount, 4);
  EXPECT_EQ(order, (std::vector<int>{3, 2, 1, 0}));
}

TEST(BuildTaskOrderGroups, EqualPositionFallsBackToGroupId) {
  std::vector<TaskGroup> groups = {grp("gz", "Z", 1), grp("ga", "A", 1)};
  std::vector<TaskRecord> records = {inGroup("t1", 0, "gz"), inGroup("t2", 0, "ga")};
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, groups, order, openCount);
  EXPECT_EQ(order, (std::vector<int>{1, 0}));
}

TEST(BuildTaskOrderGroups, UnknownGroupIsTreatedAsUngrouped) {
  std::vector<TaskGroup> groups = {grp("ga", "A", 1)};
  std::vector<TaskRecord> records = {inGroup("t1", 0, "ga"), inGroup("t2", 1, "gone")};
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, groups, order, openCount);
  EXPECT_EQ(order, (std::vector<int>{1, 0}));

  std::vector<TaskListRow> rows;
  buildTaskListRows(order, openCount, TaskListView::Open, records, groups, rows);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[0].recordIndex, 1);
  EXPECT_EQ(rows[1].kind, TaskRowKind::GroupHeader);
  EXPECT_EQ(rows[1].groupIndex, 0);
  EXPECT_EQ(rows[2].recordIndex, 0);
}

TEST(BuildTaskOrderGroups, DoneTasksIgnoreGroup) {
  std::vector<TaskGroup> groups = {grp("ga", "A", 1)};
  std::vector<TaskRecord> records = {inGroup("t2", 1, "ga", true), inGroup("t1", 0, "", true), inGroup("t3", 0, "ga")};
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, groups, order, openCount);
  EXPECT_EQ(openCount, 1);
  EXPECT_EQ(order, (std::vector<int>{2, 1, 0}));  // faites : priorite puis id, groupe ignore

  std::vector<TaskListRow> rows;
  buildTaskListRows(order, openCount, TaskListView::Done, records, groups, rows);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[1].kind, TaskRowKind::Task);
}

TEST(BuildTaskListRowsGroups, HeaderOnlyForGroupsWithOpenTasks) {
  std::vector<TaskGroup> groups = {grp("ga", "A", 1), grp("gb", "B", 2), grp("gc", "C", 3)};
  std::vector<TaskRecord> records = {inGroup("t1", 0, "gb"), inGroup("t2", 0, "ga", true), inGroup("t3", 0, ""),
                                     inGroup("t4", 1, "gb")};
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, groups, order, openCount);
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, openCount, TaskListView::Open, records, groups, rows);
  ASSERT_EQ(rows.size(), 4u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[0].groupIndex, -1);
  EXPECT_EQ(rows[1].kind, TaskRowKind::GroupHeader);
  EXPECT_EQ(rows[1].groupIndex, 1);
  EXPECT_EQ(rows[2].recordIndex, 0);
  EXPECT_EQ(rows[3].recordIndex, 3);
  EXPECT_EQ(taskGroupOpenCount(rows, 1), 2);
}

TEST(BuildTaskListRowsGroups, DoneViewHasNoHeaders) {
  std::vector<TaskGroup> groups = {grp("ga", "A", 1)};
  std::vector<TaskRecord> records = {inGroup("t1", 0, "ga", true)};
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, groups, order, openCount);
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, openCount, TaskListView::Done, records, groups, rows);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
}

namespace {
std::vector<TaskListRow> sample() {
  // T H T T H T
  std::vector<TaskListRow> r;
  r.push_back({TaskRowKind::Task, -1, 0});
  r.push_back({TaskRowKind::GroupHeader, 0, -1});
  r.push_back({TaskRowKind::Task, -1, 1});
  r.push_back({TaskRowKind::Task, -1, 2});
  r.push_back({TaskRowKind::GroupHeader, 1, -1});
  r.push_back({TaskRowKind::Task, -1, 3});
  return r;
}
}  // namespace

TEST(TaskListGroupJump, ForwardAndBackwardWithoutWrap) {
  const auto rows = sample();
  EXPECT_EQ(taskListGroupJump(rows, 0, +1), 1);
  EXPECT_EQ(taskListGroupJump(rows, 1, +1), 4);
  EXPECT_EQ(taskListGroupJump(rows, 4, +1), -1);
  EXPECT_EQ(taskListGroupJump(rows, 5, -1), 4);
  EXPECT_EQ(taskListGroupJump(rows, 4, -1), 1);
  EXPECT_EQ(taskListGroupJump(rows, 1, -1), -1);
  EXPECT_EQ(taskListGroupJump(rows, 0, -1), -1);
}

TEST(TaskListStepSkippingHeaders, NeverLandsOnHeaderAndWraps) {
  const auto rows = sample();
  for (int dir : {-1, +1}) {
    for (int s = 0; s < static_cast<int>(rows.size()); ++s) {
      const int n = taskListStepSkippingHeaders(rows, s, dir);
      ASSERT_GE(n, 0);
      EXPECT_EQ(rows[n].kind, TaskRowKind::Task);
    }
  }
  EXPECT_EQ(taskListStepSkippingHeaders(rows, 0, +1), 2);
  EXPECT_EQ(taskListStepSkippingHeaders(rows, 3, +1), 5);
  EXPECT_EQ(taskListStepSkippingHeaders(rows, 5, +1), 0);
  EXPECT_EQ(taskListStepSkippingHeaders(rows, 0, -1), 5);
  EXPECT_EQ(taskListStepSkippingHeaders(rows, 2, -1), 0);
}

TEST(TaskListStepSkippingHeaders, OnlyHeadersReturnsMinusOne) {
  std::vector<TaskListRow> r = {{TaskRowKind::GroupHeader, 0, -1}};
  EXPECT_EQ(taskListStepSkippingHeaders(r, 0, +1), -1);
}

TEST(TaskGroupOpenCount, CountsUntilNextHeader) {
  const auto rows = sample();
  EXPECT_EQ(taskGroupOpenCount(rows, 1), 2);
  EXPECT_EQ(taskGroupOpenCount(rows, 4), 1);
}

TEST(TaskListStepSkippingHeaders, NothingSelectedStartsFromTheMatchingEnd) {
  const auto rows = sample();
  // Sans selection, reculer arrive sur la derniere ligne (pas l'avant-derniere).
  EXPECT_EQ(taskListStepSkippingHeaders(rows, -1, -1), 5);
  EXPECT_EQ(taskListStepSkippingHeaders(rows, -1, +1), 0);
  // Une derniere ligne qui serait un titre est sautee elle aussi.
  std::vector<TaskListRow> r = {{TaskRowKind::GroupHeader, 0, -1}, {TaskRowKind::Task, -1, 0}};
  EXPECT_EQ(taskListStepSkippingHeaders(r, -1, +1), 1);
  EXPECT_EQ(taskListStepSkippingHeaders(r, -1, -1), 1);
  // Un indice perime au-dela de la fin se comporte comme -1.
  EXPECT_EQ(taskListStepSkippingHeaders(rows, 99, -1), 5);
}

TEST(TaskGroupDisplayOrder, ByPositionThenId) {
  const std::vector<TaskGroup> groups = {grp("gc", "C", 2), grp("gb", "B", 1), grp("ga", "A", 2)};
  uint8_t out[MAX_GROUPS];
  ASSERT_EQ(taskGroupDisplayOrder(groups, out), 3u);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 2);
  EXPECT_EQ(out[2], 0);
}

TEST(TaskGroupDisplayOrder, NoGroupsGivesNothing) {
  uint8_t out[MAX_GROUPS];
  EXPECT_EQ(taskGroupDisplayOrder({}, out), 0u);
}
