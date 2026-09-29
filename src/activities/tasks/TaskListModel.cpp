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

void buildTaskListRows(const std::vector<int>& order, int openCount, bool showDone, std::vector<TaskListRow>& rows) {
  rows.clear();
  const int total = static_cast<int>(order.size());
  if (total == 0) {
    // Seul cas ou la ligne d'ajout existe : dans l'etat "tout est fait",
    // l'index n'est pas vide, la ligne "N faites" est la, et l'ajout passe par
    // le bouton Gauche comme sur une liste peuplee.
    rows.push_back({TaskRowKind::AddTask, -1});
    return;
  }
  if (openCount < 0) openCount = 0;
  if (openCount > total) openCount = total;
  const bool hasDone = openCount < total;
  rows.reserve(static_cast<size_t>(showDone ? total + 1 : openCount + 1));

  for (int i = 0; i < openCount; ++i) rows.push_back({TaskRowKind::Task, order[i]});

  // Presente repliee aussi : un accordeon qu'on ne voit pas ne s'ouvre pas.
  if (hasDone) {
    rows.push_back({TaskRowKind::DoneSection, -1});
    if (showDone) {
      for (int i = openCount; i < total; ++i) rows.push_back({TaskRowKind::Task, order[i]});
    }
  }
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
