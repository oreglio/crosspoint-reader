#include "TaskListModel.h"

#include <algorithm>

void buildTaskOrder(const std::vector<TaskRecord>& records, std::vector<int>& order, int& openCount) {
  order.clear();
  order.reserve(records.size());
  for (int i = 0; i < static_cast<int>(records.size()); ++i) order.push_back(i);

  std::sort(order.begin(), order.end(), [&](int a, int b) { return taskOrderBefore(records[a], records[b]); });

  openCount = 0;
  while (openCount < static_cast<int>(order.size()) && !records[order[openCount]].done) {
    ++openCount;
  }
}

void buildTaskListRows(const std::vector<int>& order, int openCount, TaskListView view,
                       std::vector<TaskListRow>& rows) {
  rows.clear();
  const int total = static_cast<int>(order.size());
  if (total == 0) {
    rows.push_back({TaskRowKind::AddTask, -1});
    return;
  }
  if (openCount < 0) openCount = 0;
  if (openCount > total) openCount = total;

  if (view == TaskListView::Open) {
    if (openCount == 0) {
      // Tout est fait : la vue ouverte propose l'ajout plutot qu'une liste vide.
      rows.push_back({TaskRowKind::AddTask, -1});
      return;
    }
    rows.reserve(static_cast<size_t>(openCount));
    for (int i = 0; i < openCount; ++i) rows.push_back({TaskRowKind::Task, order[i]});
    return;
  }

  rows.reserve(static_cast<size_t>(total - openCount));
  for (int i = openCount; i < total; ++i) rows.push_back({TaskRowKind::Task, order[i]});
}

int taskListClampSelection(const std::vector<TaskListRow>& rows, int selected) {
  const int total = static_cast<int>(rows.size());
  if (total == 0) return -1;
  if (selected < 0) return 0;
  if (selected >= total) return total - 1;
  return selected;
}

size_t taskSelectionTargets(const TaskSelection& selection, const size_t recordCount, uint8_t* out) {
  size_t n = 0;
  for (size_t i = std::min(recordCount, MAX_TASKS); i-- > 0;) {
    if (selection.test(i)) out[n++] = static_cast<uint8_t>(i);
  }
  return n;
}

size_t taskDoneTargets(const std::vector<TaskRecord>& records, uint8_t* out) {
  size_t n = 0;
  for (size_t i = std::min(records.size(), MAX_TASKS); i-- > 0;) {
    if (records[i].done) out[n++] = static_cast<uint8_t>(i);
  }
  return n;
}
