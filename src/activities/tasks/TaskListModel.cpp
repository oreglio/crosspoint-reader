#include "TaskListModel.h"

#include <algorithm>
#include <cstring>

namespace {

int findGroup(const std::vector<TaskGroup>& groups, const char* id) {
  if (id[0] == '\0') return -1;
  for (size_t i = 0; i < groups.size(); ++i) {
    if (std::strcmp(groups[i].id, id) == 0) return static_cast<int>(i);
  }
  return -1;
}

// Rang d'affichage de chaque groupe (0 = premier) : position, puis id.
// Compte les groupes qui le precedent ; MAX_GROUPS est petit, O(n^2) sans tas.
size_t groupRank(const std::vector<TaskGroup>& groups, size_t g) {
  size_t rank = 0;
  for (size_t i = 0; i < groups.size(); ++i) {
    if (groups[i].position != groups[g].position ? groups[i].position < groups[g].position
                                                 : std::strcmp(groups[i].id, groups[g].id) < 0) {
      ++rank;
    }
  }
  return rank;
}

}  // namespace

void buildTaskOrder(const std::vector<TaskRecord>& records, const std::vector<TaskGroup>& groups,
                    std::vector<int>& order, int& openCount) {
  order.clear();
  order.reserve(records.size());
  for (int i = 0; i < static_cast<int>(records.size()); ++i) order.push_back(i);

  // Rang de groupe calcule une fois par enregistrement (pile, <= 120 o) plutot
  // que cherche dans le comparateur. 0 = sans groupe / inconnu / tache faite.
  uint8_t rank[MAX_TASKS] = {};
  const size_t n = std::min(records.size(), MAX_TASKS);
  for (size_t i = 0; i < n; ++i) {
    if (records[i].done) continue;
    const int g = findGroup(groups, records[i].group);
    if (g >= 0) rank[i] = static_cast<uint8_t>(1 + std::min<size_t>(groupRank(groups, static_cast<size_t>(g)), 254));
  }
  auto rankOf = [&](int i) { return static_cast<size_t>(i) < MAX_TASKS ? rank[i] : uint8_t{0}; };

  std::sort(order.begin(), order.end(), [&](int a, int b) {
    if (records[a].done == records[b].done && rankOf(a) != rankOf(b)) return rankOf(a) < rankOf(b);
    return taskOrderBefore(records[a], records[b]);
  });

  openCount = 0;
  while (openCount < static_cast<int>(order.size()) && !records[order[openCount]].done) {
    ++openCount;
  }
}

void buildTaskListRows(const std::vector<int>& order, int openCount, TaskListView view,
                       const std::vector<TaskRecord>& records, const std::vector<TaskGroup>& groups,
                       std::vector<TaskListRow>& rows) {
  rows.clear();
  const int total = static_cast<int>(order.size());
  if (total == 0) {
    rows.push_back({TaskRowKind::AddTask, -1, -1});
    return;
  }
  if (openCount < 0) openCount = 0;
  if (openCount > total) openCount = total;

  if (view == TaskListView::Open) {
    if (openCount == 0) {
      // Tout est fait : la vue ouverte propose l'ajout plutot qu'une liste vide.
      rows.push_back({TaskRowKind::AddTask, -1, -1});
      return;
    }
    // Au plus un titre par groupe : reserve la borne haute.
    rows.reserve(static_cast<size_t>(openCount) + groups.size());
    int lastGroup = -1;
    for (int i = 0; i < openCount; ++i) {
      const int g = findGroup(groups, records[static_cast<size_t>(order[i])].group);
      // Les taches d'un meme groupe sont contigues dans `order`.
      if (g >= 0 && g != lastGroup) rows.push_back({TaskRowKind::GroupHeader, -1, g});
      lastGroup = g;
      rows.push_back({TaskRowKind::Task, order[i], -1});
    }
    return;
  }

  rows.reserve(static_cast<size_t>(total - openCount));
  for (int i = openCount; i < total; ++i) rows.push_back({TaskRowKind::Task, order[i], -1});
}

int taskListGroupJump(const std::vector<TaskListRow>& rows, int selected, int direction) {
  const int total = static_cast<int>(rows.size());
  if (direction == 0) return -1;
  const int step = direction > 0 ? 1 : -1;
  for (int i = selected + step; i >= 0 && i < total; i += step) {
    if (rows[static_cast<size_t>(i)].kind == TaskRowKind::GroupHeader) return i;
  }
  return -1;
}

int taskListStepSkippingHeaders(const std::vector<TaskListRow>& rows, int selected, int direction) {
  const int total = static_cast<int>(rows.size());
  if (total == 0) return -1;
  const int step = direction < 0 ? -1 : 1;
  int i = selected;
  for (int tries = 0; tries < total; ++tries) {
    i = ((i + step) % total + total) % total;
    if (rows[static_cast<size_t>(i)].kind != TaskRowKind::GroupHeader) return i;
  }
  return -1;
}

int taskGroupOpenCount(const std::vector<TaskListRow>& rows, int headerRow) {
  int count = 0;
  for (size_t i = static_cast<size_t>(headerRow) + 1; i < rows.size(); ++i) {
    if (rows[i].kind == TaskRowKind::GroupHeader) break;
    ++count;
  }
  return count;
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
