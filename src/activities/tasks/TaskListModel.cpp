#include "TaskListModel.h"

#include <algorithm>
#include <cstring>

namespace {

bool isTickedHere(const std::vector<std::string>& tickedHere, const char* id) {
  for (const auto& t : tickedHere) {
    if (t == id) return true;
  }
  return false;
}

}  // namespace

void buildTaskOrder(const std::vector<TaskRecord>& records, const std::vector<std::string>& tickedHere,
                    std::vector<int>& order, int& openCount) {
  order.clear();
  order.reserve(records.size());
  for (int i = 0; i < static_cast<int>(records.size()); ++i) order.push_back(i);

  // Une tache cochee pendant cette visite est triee comme si elle etait
  // encore ouverte : c'est ce qui la garde a sa place au lieu de la faire
  // tomber dans la section repliee qu'on vient de cocher.
  auto effectiveDone = [&](int idx) { return records[idx].done && !isTickedHere(tickedHere, records[idx].id); };

  std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
    const bool doneA = effectiveDone(a);
    const bool doneB = effectiveDone(b);
    if (doneA != doneB) return !doneA;
    if (records[a].priority != records[b].priority) return records[a].priority < records[b].priority;
    return std::strcmp(records[a].id, records[b].id) < 0;
  });

  openCount = 0;
  while (openCount < static_cast<int>(order.size()) && !effectiveDone(order[openCount])) ++openCount;
}

void buildTaskListRows(const std::vector<int>& order, int openCount, bool showDone, std::vector<TaskListRow>& rows) {
  rows.clear();
  const int total = static_cast<int>(order.size());
  if (openCount < 0) openCount = 0;
  if (openCount > total) openCount = total;
  rows.reserve(showDone ? total + 1 : openCount);

  for (int i = 0; i < openCount; ++i) rows.push_back({TaskRowKind::Task, order[i]});

  if (showDone && openCount < total) {
    rows.push_back({TaskRowKind::DoneHeader, -1});
    for (int i = openCount; i < total; ++i) rows.push_back({TaskRowKind::Task, order[i]});
  }
}

int taskListStepSelection(const std::vector<TaskListRow>& rows, int index, int direction) {
  const int total = static_cast<int>(rows.size());
  if (total == 0) return -1;
  int next = index;
  for (int step = 0; step < total; ++step) {
    next = (next + direction % total + total) % total;
    if (rows[next].kind == TaskRowKind::Task) return next;
  }
  // Rien de selectionnable (que des en-tetes) : ne devrait jamais arriver
  // puisqu'une en-tete n'existe que quand il y a au moins une tache derriere.
  return index;
}

int taskListNormalizeSelection(const std::vector<TaskListRow>& rows, int selected) {
  const int total = static_cast<int>(rows.size());
  if (total == 0) return -1;
  if (selected < 0) selected = 0;
  if (selected >= total) selected = total - 1;
  if (rows[selected].kind == TaskRowKind::Task) return selected;
  return taskListStepSelection(rows, selected, +1);
}

int taskListVisibleRows(const std::vector<int>& heights, int scrollTop, int contentHeight) {
  const int total = static_cast<int>(heights.size());
  if (scrollTop < 0 || scrollTop >= total) return 0;
  int used = 0;
  int count = 0;
  for (int i = scrollTop; i < total; ++i) {
    if (count > 0 && used + heights[i] > contentHeight) break;
    used += heights[i];
    ++count;
  }
  return count;
}

int taskListClampScrollTop(const std::vector<int>& heights, int selected, int scrollTop, int contentHeight) {
  const int total = static_cast<int>(heights.size());
  if (total == 0) return 0;
  if (selected < 0) selected = 0;
  if (selected >= total) selected = total - 1;
  if (scrollTop < 0) scrollTop = 0;
  if (scrollTop >= total) scrollTop = total - 1;
  if (scrollTop > selected) scrollTop = selected;

  // `selected` deborde-t-elle par le bas de la fenetre courante ? Avance
  // scrollTop d'une ligne a la fois jusqu'a ce qu'elle rentre : un
  // defilement en escalier, pas un recentrage qui bougerait tout l'ecran
  // pour une seule ligne de plus.
  while (scrollTop < selected) {
    int used = 0;
    bool fits = false;
    for (int i = scrollTop; i <= selected; ++i) {
      used += heights[i];
      if (used > contentHeight) break;
      if (i == selected) fits = true;
    }
    if (fits) break;
    ++scrollTop;
  }
  return scrollTop;
}
